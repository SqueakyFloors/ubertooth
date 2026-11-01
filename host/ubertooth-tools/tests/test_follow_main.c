/*
 * The order in which ubertooth-follow sets a board up, run through its real
 * main() against a scripted library and a scripted BlueZ adapter.
 *
 * The board's clock is read rather than set, and the read belongs beside the
 * clocks the adapter is asked for, just before the board is told to hop: the
 * offset the board is given is the difference between the piconet's clock and
 * its own, so time that passes between measuring the two is error in the
 * followed clock. The tool used to zero CLKN instead, well above the reads,
 * which on a board an earlier run had left streaming Classic symbols -- the
 * one board whose clock is running when a follow starts -- made the followed
 * clock depend on how long the setup took. There is no clock write left,
 * which is why cmd_set_clock_quiet() is not among the fakes below: a tool
 * that sent one again would not link.
 *
 * Which requests may move is decided by how they are sent. The address and
 * the clock read go out synchronously. The AFH map set (-a) and the
 * start-hopping request are asynchronous: libusb hands their completions to
 * whoever polls for events, which here is the thread
 * ubertooth_bulk_thread_start() creates, so those two must stay after it.
 *
 * The cases below pin the whole order of requests, not only the halves that
 * are required, so a request that moves anywhere is caught and looked at.
 * They also pin what a board that refuses the address or the clock read gets:
 * a follow set up on neither would follow something the user did not ask for,
 * so the run stops and says so. Clearing the AFH map, which is what a follow
 * without -a does, is synchronous and under no ordering requirement of its
 * own.
 *
 * ubertooth-follow.c is compiled in with main() renamed, as test_scan_main.c
 * does for the other BlueZ tool, and every libubertooth call is a fake here,
 * so no board is opened. libbtbb is the real one: the tool hands it the
 * piconet. BlueZ is faked as far as the adapter, the link to the target and
 * the clock reads, so no case needs a Bluetooth adapter on the machine running
 * the tests.
 */

/* RTLD_NEXT, used below to reach libbtbb's real btbb_piconet_new(), is a GNU
 * extension, and nothing in the build asks for the GNU set. This has to come
 * before any include, because the first one pulls in <features.h>, where the
 * choice is read. Same reason as in test_rx_main.c. */
#define _GNU_SOURCE 1

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_follow_main
#include "ubertooth-follow.c"
#undef main

#include "run_main.h"

/* The libubertooth globals the tool uses; they live in ubertooth.c, which is
 * not linked here. */
FILE* dumpfile;
int max_ac_errors = 2;

/* The -V answer, which libubertooth gives in every tool: faked here, so this
 * test stays about the tool's own command-line handling. The answer's own
 * cases are in test_version_report.c. */
int ubertooth_print_versions(FILE *out, const char *selector)
{
	(void)selector;
	fprintf(out, "version (test)\n");
	return 0;
}

/* ---- scripted device and adapter ---------------------------------------- */

static struct {
	int api_rc;           /* returned by ubertooth_check_api() */
	int api_released;     /* whether that call released the board itself */
	int bulk_init_rc;     /* returned by ubertooth_bulk_init() */
	int bdaddr_rc;        /* returned by cmd_set_bdaddr_quiet() */
	int clock_rc;         /* returned by cmd_read_clock_quiet() */
	uint32_t board_clkn;  /* the clock that read reports */
	int thread_start_rc;  /* returned by ubertooth_bulk_thread_start() */
	int hopping_rc;       /* returned by cmd_start_hopping() */
	int modulation_rc;    /* returned by cmd_set_modulation() */
	int modulation;       /* the modulation it was last given */
	int clkn_offset;      /* the clock offset the board was told to hop on */
	int closes;           /* ubertooth_close() */
	int frees;            /* ubertooth_free() */
	int dev_fd;           /* the adapter handle the tool was given */
	int dev_fd_closed;    /* whether the run gave it back */
	/* Whether it was already back by the first turn of the capture loop,
	 * which is what says the handle goes back before the capture rather
	 * than at the end of the run. */
	int dev_fd_closed_by_capture;
	/* The two BlueZ failures: no adapter of that name, and one that is
	 * there but will not open. Each errno is what the call leaves behind,
	 * 0 for a failure that left none. */
	int devid_fails;
	int devid_errno;
	int open_fails;
	int open_errno;
	/* The page to the target: whether it fails, and the errno it leaves.
	 * connect_keeps_errno is the case BlueZ does not promise against: the
	 * call fails and leaves errno as the last one to touch it left it,
	 * which dirty_errno below is what puts there. */
	int connect_fails;
	int connect_errno;
	int connect_keeps_errno;
	/* An errno left behind by an earlier call, set by the fake hci_devinfo()
	 * so a later failure that sets none has something stale to report. */
	int dirty_errno;
	int connects;         /* ubertooth_connect_select(): the board opened */
	/* The address the page was sent to, as the tool spelled it. */
	char paged[19];
	int thread_starts;
	int thread_stops;
	int stops;
	int receives;         /* turns of the capture loop */
	int blocks_to_end;    /* ...before the fake ends the capture */
	uint8_t afh_mode;     /* what the target reports for AFH */
	uint8_t afh_map[AFH_MAP_BYTES];
	uint64_t bdaddr;      /* the address the board was told to follow */
	/* Every device call in order, one letter each: a set the target's
	 * address, k read the board's clock, i submitted the receive transfer,
	 * p started the poll thread, d set the modulation, m set the AFH map,
	 * c cleared it, h told the board to start hopping. */
	char calls[16];
	/* 1 to have the teardown's STOP refused, so the board is left running
	 * the mode the capture started. */
	int stop_refused;
	/* 1 once the library's stop handlers are in place, and the stop a
	 * signal recorded while the board was being opened. */
	int stop_handlers;
	int stop_requested;
	/* 1 to have btbb_piconet_new() fail, as it does when the machine is out
	 * of memory. */
	int no_memory;
} fake;

static ubertooth_t fake_ut;
static int fake_devh;

static void record(char call)
{
	size_t length = strlen(fake.calls);

	if (length + 1 < sizeof(fake.calls)) {
		fake.calls[length] = call;
		fake.calls[length + 1] = '\0';
	}
}

/* Where a call landed in the recorded order, or -1 if it never happened. */
static int call_at(char call)
{
	const char* found = strchr(fake.calls, call);

	return found == NULL ? -1 : (int)(found - fake.calls);
}

static void reset_fake(void)
{
	memset(&fake, 0, sizeof(fake));
	memset(&fake_ut, 0, sizeof(fake_ut));
	/* libbtbb is the real one here, so this is the value it is handed: in
	 * range, and set again for every case because one of them hands it an
	 * out-of-range value to make it refuse. */
	max_ac_errors = 2;
	fake.dev_fd = -1;
	/* The dump file -d opens is a global of the library's, so a case that
	 * used one would otherwise leave it set for the next. */
	dumpfile = NULL;
	fake_ut.devh = (struct libusb_device_handle*)&fake_devh;
	fake.blocks_to_end = 2;
	/* A board that has been running a while, so an offset worked out from
	 * the wrong end of the subtraction does not read as the right one. The
	 * faked adapter answers 1000 for the piconet's clock, below. */
	fake.board_clkn = 600;
	/* A target using AFH on every channel, so -a reaches the map set. */
	fake.afh_mode = FOLLOW_AFH_MODE_ON;
	memset(fake.afh_map, 0xff, sizeof(fake.afh_map));
}

/* ---- faked BlueZ -------------------------------------------------------- */

int hci_devid(const char* str)
{
	(void)str;
	if (fake.devid_fails) {
		/* The tool clears errno before the call, so 0 here is a failure
		 * that set none -- which BlueZ does not promise to do. */
		errno = fake.devid_errno;
		return -1;
	}
	return 0;
}

/* A real descriptor, so the tool's close() is a real close and giving the
 * adapter handle back can be checked, but one that is not a Bluetooth socket:
 * every call the tool makes on the adapter is faked below. */
int hci_open_dev(int dev_id)
{
	(void)dev_id;
	if (fake.open_fails) {
		errno = fake.open_errno;
		return -1;
	}
	fake.dev_fd = open("/dev/null", O_RDONLY);
	if (fake.dev_fd < 0) {
		/* Say the fixture broke, rather than letting the tool report a
		 * missing adapter and a later case fail on something unrelated. */
		fprintf(stderr, "fixture: cannot open /dev/null: %s\n",
		        strerror(errno));
		exit(1);
	}
	return fake.dev_fd;
}

int hci_devinfo(int dev_id, struct hci_dev_info* di)
{
	(void)dev_id;
	memset(di, 0, sizeof(*di));
	di->pkt_type = 0xffff;
	/* This is the last call before the page, so it is where a stale errno
	 * comes from. A real BlueZ call can leave one behind even when it
	 * succeeds. */
	if (fake.dirty_errno != 0)
		errno = fake.dirty_errno;
	return 0;
}

int hci_create_connection(int dd, const bdaddr_t* bdaddr, uint16_t ptype,
                          uint16_t clkoffset, uint8_t rswitch,
                          uint16_t* handle, int to)
{
	(void)dd;
	(void)ptype;
	(void)clkoffset;
	(void)rswitch;
	(void)to;
	/* ba2str() is BlueZ's own and is not faked, so the address is spelled
	 * out here: the byte order is the one str2ba() filled in, least
	 * significant first. */
	snprintf(fake.paged, sizeof(fake.paged),
	         "%02X:%02X:%02X:%02X:%02X:%02X",
	         bdaddr->b[5], bdaddr->b[4], bdaddr->b[3],
	         bdaddr->b[2], bdaddr->b[1], bdaddr->b[0]);
	if (fake.connect_fails) {
		if (!fake.connect_keeps_errno)
			errno = fake.connect_errno;
		return -1;
	}
	*handle = 42;
	return 0;
}

int hci_disconnect(int dd, uint16_t handle, uint8_t reason, int to)
{
	(void)dd;
	(void)handle;
	(void)reason;
	(void)to;
	return 0;
}

int hci_read_afh_map(int dd, uint16_t handle, uint8_t* mode, uint8_t* map,
                     int to)
{
	(void)dd;
	(void)handle;
	(void)to;
	*mode = fake.afh_mode;
	memcpy(map, fake.afh_map, sizeof(fake.afh_map));
	return 0;
}

int hci_read_clock(int dd, uint16_t handle, uint8_t which, uint32_t* clock,
                   uint16_t* accuracy, int to)
{
	(void)dd;
	(void)handle;
	(void)which;
	(void)to;
	*clock = 1000;
	*accuracy = 0;
	return 0;
}

int hci_read_clock_offset(int dd, uint16_t handle, uint16_t* clkoffset, int to)
{
	(void)dd;
	(void)handle;
	(void)to;
	*clkoffset = 0;
	return 0;
}

/* ---- faked Ubertooth ---------------------------------------------------- */

ubertooth_t* ubertooth_init()
{
	return &fake_ut;
}

/* No case here refuses the board itself: what the tool does when no board
 * matches -U is checked against the real binary by
 * board-not-found-ubertooth-follow and by the output-stream cases. */
int ubertooth_connect_select(ubertooth_t* ut, const char* u_arg)
{
	(void)ut;
	(void)u_arg;
	fake.connects++;
	return 0;
}

int ubertooth_check_api(ubertooth_t* ut)
{
	/* The library releases the board itself when the firmware reports a
	 * version this host will not drive, through the same ubertooth_stop()
	 * faked below; a version it could not read at all leaves the board open
	 * for the caller to close. */
	if (fake.api_rc < 0 && fake.api_released)
		ubertooth_stop(ut);
	return fake.api_rc;
}

/* The real one folds the signal handler's own flag into the handle; the fake
 * ubertooth_bulk_receive() below sets the handle directly, so reading it is
 * enough here. */
int ubertooth_stopping(ubertooth_t* ut)
{
	return ut->stop_ubertooth;
}

void ubertooth_catch_stop_signals(void)
{
	fake.stop_handlers++;
}

int ubertooth_stop_requested(void)
{
	return fake.stop_requested;
}

/* The fake open always finds the board, so it never gives up on a stop. */
int ubertooth_open_stopped(void)
{
	return 0;
}

int cmd_set_bdaddr_quiet(struct libusb_device_handle* devh, uint64_t address)
{
	(void)devh;
	fake.bdaddr = address;
	record('a');
	return fake.bdaddr_rc;
}

int cmd_read_clock_quiet(struct libusb_device_handle* devh, uint32_t* clock)
{
	(void)devh;
	record('k');
	if (fake.clock_rc < 0)
		return fake.clock_rc;
	*clock = fake.board_clkn;
	return 0;
}

int cmd_set_afh_map(struct libusb_device_handle* devh, uint8_t* map)
{
	(void)devh;
	(void)map;
	record('m');
	return 0;
}

int cmd_clear_afh_map(struct libusb_device_handle* devh)
{
	(void)devh;
	record('c');
	return 0;
}

int cmd_set_modulation(struct libusb_device_handle* devh, uint16_t mod)
{
	(void)devh;
	fake.modulation = mod;
	record('d');
	return fake.modulation_rc;
}

int cmd_start_hopping(struct libusb_device_handle* devh, int clkn_offset,
                      int clk100ns_offset)
{
	(void)devh;
	(void)clk100ns_offset;
	fake.clkn_offset = clkn_offset;
	record('h');
	return fake.hopping_rc;
}

int ubertooth_bulk_init(ubertooth_t* ut)
{
	(void)ut;
	record('i');
	return fake.bulk_init_rc;
}

/* pthread_create() returns an errno, which is positive, and the tool has to
 * read it that way: the fake hands back whatever the case asked for. */
int ubertooth_bulk_thread_start()
{
	record('p');
	fake.thread_starts++;
	return fake.thread_start_rc;
}

void ubertooth_bulk_thread_stop()
{
	fake.thread_stops++;
}

/* Ends the capture after a few turns, the way an interrupted run ends it:
 * what a block contains is the callback's own business and no case here
 * should wait on a board. */
int ubertooth_bulk_receive(ubertooth_t* ut, rx_callback cb, void* cb_args)
{
	(void)cb;
	(void)cb_args;
	if (fake.receives == 0)
		fake.dev_fd_closed_by_capture = fake.dev_fd >= 0
		        && fcntl(fake.dev_fd, F_GETFD) == -1
		        && errno == EBADF;
	fake.receives++;
	if (fake.receives >= fake.blocks_to_end)
		ut->stop_ubertooth = 1;
	return 0;
}

void ubertooth_stop(ubertooth_t* ut)
{
	ut->devh = NULL; /* as the library's cleanup leaves it */
	fake.stops++;
}

/* The report about a refused request asks whether the board has left the bus
 * before it blames the board's firmware. Nothing here unplugs a board, so it
 * is always still there. */
int ubertooth_device_gone(const void* board)
{
	(void)board;
	return 0;
}

/* Whether the teardown's STOP was refused, which leaves the board running the
 * mode the capture started. Scripted, because the fake board above always
 * takes it. */
int ubertooth_stop_failed(ubertooth_t* ut)
{
	(void)ut;
	return fake.stop_refused;
}

void ubertooth_close(ubertooth_t* ut)
{
	ut->devh = NULL;
	fake.closes++;
}

/* Counted rather than freeing: the handle above is this test's own static. */
void ubertooth_free(ubertooth_t* ut)
{
	(void)ut;
	fake.frees++;
}

/* The end-of-capture gap report, which the tool calls and no case here reads:
 * a capture whose records never carried FIFO_OVERFLOW has nothing to say. */
void ubertooth_report_capture_losses(ubertooth_t* ut)
{
	(void)ut;
}

int ubertooth_stream_failed(ubertooth_t* ut)
{
	(void)ut;
	return 0;
}

void cb_rx(ubertooth_t* ut, void* args)
{
	(void)ut;
	(void)args;
}

/* ---- running the tool --------------------------------------------------- */

static struct run_result run_follow(const char* const* args)
{
	struct run_result result = run_tool_main(ubertooth_follow_main, args);

	/* Read as soon as the run is over, before this file opens anything
	 * that could be given the descriptor's number. */
	fake.dev_fd_closed = fake.dev_fd >= 0
	                     && fcntl(fake.dev_fd, F_GETFD) == -1
	                     && errno == EBADF;
	return result;
}

/* libbtbb is the real library here, so this stands in front of its
 * btbb_piconet_new() rather than replacing it: a case that has not asked for
 * the allocation to fail gets a real piconet, looked up past this definition.
 * A process only fails to allocate when it is out of memory, which a test
 * cannot reach without putting the whole run at risk, so the failure is
 * scripted. Same shape as test_rx_main.c's.
 *
 * This needs the shared libbtbb: against a static one the definition here is
 * either a duplicate symbol at link time or the only one there is, and then
 * dlsym() finds nothing and every case gets a NULL piconet. */
btbb_piconet* btbb_piconet_new(void)
{
	static btbb_piconet* (*real_btbb_piconet_new)(void);

	if (fake.no_memory)
		return NULL;
	if (real_btbb_piconet_new == NULL)
		real_btbb_piconet_new = (btbb_piconet* (*)(void))
		                        dlsym(RTLD_NEXT, "btbb_piconet_new");
	if (real_btbb_piconet_new == NULL) {
		/* The static-libbtbb case above: say so, or every case fails
		 * as "out of memory" and names no cause. */
		fputs("test: libbtbb's btbb_piconet_new() was not found;"
		      " this test needs the shared library\n", stderr);
		return NULL;
	}
	return real_btbb_piconet_new();
}

#define RUN(...) run_follow((const char* const[]){ "ubertooth-follow", __VA_ARGS__, NULL })

/* The whole setup a follow of a given target does, in order. */
#define TARGET "-l", "112233", "-u", "44"
/* The same piconet named as a whole address, which is what -a needs: the map
 * is read over a connection to the target, and -l with -u is four of the six
 * bytes. 00:00 is the address -l and -u imply. */
#define WHOLE_TARGET "-t", "00:00:44:11:22:33"

/* ---- tests -------------------------------------------------------------- */

/* The board's clock is read just before the board is told to hop, with
 * nothing between the two but the requests that have to be there. Time
 * between the measurement and the hop is error in the followed clock, which
 * is why the read does not move up to where the clock reset used to be. */
MU_TEST(the_clock_is_read_just_before_the_hopping_starts)
{
	struct run_result r;

	reset_fake();
	r = RUN(TARGET);
	mu_assert_int_eq(0, r.rc);
	mu_assert(call_at('k') >= 0, "the board's clock was never read");
	mu_assert(call_at('h') >= 0, "the board was never told to hop");
	mu_assert(call_at('k') < call_at('h'),
	          "the board's clock was read after it was told to hop");
	/* The whole order, so a request that moves anywhere is caught: address,
	 * receive transfer, poll thread, clock read, modulation, hopping, AFH
	 * map cleared. The map goes out after hopping has started because
	 * starting a mode forgets it, so one sent first would be thrown away;
	 * the modulation goes before it, because that is what the mode start
	 * programs the demodulator from and every reset leaves Basic Rate. */
	mu_assert(strcmp(fake.calls, "aipkdhc") == 0, fake.calls);
	mu_assert_int_eq(MOD_BT_BASIC_RATE, fake.modulation);
}

/* The asynchronous requests are completed by the poll thread, so they stay
 * after the thread is running: a map set moved up with the clock set would be
 * left with a transfer nothing is polling for. */
MU_TEST(the_asynchronous_requests_stay_after_the_poll_thread)
{
	struct run_result r;

	reset_fake();
	r = RUN("-a", WHOLE_TARGET);
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "aipkdhm") == 0, fake.calls);
	mu_assert(call_at('p') < call_at('m'),
	          "the AFH map was set before the poll thread was running");
	mu_assert(call_at('p') < call_at('h'),
	          "hopping was started before the poll thread was running");
}

/* A follow the board took ends by giving the thread and the board back. */
MU_TEST(a_follow_the_board_took_exits_0)
{
	struct run_result r;

	reset_fake();
	r = RUN(TARGET);
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.thread_starts);
	mu_assert_int_eq(1, fake.thread_stops);
	mu_assert_int_eq(1, fake.stops);
	mu_assert_int_eq(fake.blocks_to_end, fake.receives);
	/* Standard error carries the tool's progress, so it is not empty; what a
	 * follow the board took must not report is a failure. */
	mu_assert(strstr(r.err, "error:") == NULL, r.err);
	mu_assert(strstr(r.err, "warning:") == NULL, r.err);
	/* The capture needs nothing more from the adapter once the clocks are
	 * read, so the handle is back before the first block is read rather than
	 * being held for the whole run. */
	mu_assert(fake.dev_fd_closed_by_capture,
	          "the adapter handle was still open when the capture started");
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
	/* The stop handlers are in, installed before the board was opened.
	 * Not before the BlueZ setup above, whose calls BlueZ takes again
	 * itself when a signal cuts them short. */
	mu_assert_int_eq(1, fake.stop_handlers);
}

/* A firmware this host will not drive, and a libbtbb that refuses the decoder
 * settings, are the two failures with the board already open and no mode
 * started. Each gives the board and the handle back, rather than leaving them
 * to process exit. One close covers both firmware failures, because closing a
 * board the library has already released does nothing. */
/* A follow the board would not stop leaves that board running the capture
 * mode, and no other tool can open it until it is reset or re-plugged, so the
 * run did not do what was asked. What was captured stays written out, and
 * libubertooth has already named the refused request. */
/* A stop that arrived while the board was being opened ends the run before any
 * capture starts, with the board and the adapter given back. */
MU_TEST(a_stop_while_the_board_was_opened_captures_nothing)
{
	struct run_result r;

	reset_fake();
	fake.stop_requested = 1;
	r = RUN("-t", "11:22:33:44:55:66");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.receives);
	mu_assert(strstr(r.err, "error:") == NULL, r.err);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
}

MU_TEST(a_follow_whose_teardown_was_refused_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.stop_refused = 1;
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.stops);
	/* The follow itself ran: every block the board sent was taken. */
	mu_assert_int_eq(fake.blocks_to_end, fake.receives);
	/* Nothing of this tool's own about it. */
	mu_assert(strstr(r.err, "error:") == NULL, r.err);
}

MU_TEST(a_firmware_the_tool_cannot_use_gives_the_board_back)
{
	struct run_result r;

	/* A version mismatch: the library released the board itself, and the
	 * tool's own close then does nothing. */
	reset_fake();
	fake.api_rc = -1;
	fake.api_released = 1;
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
	mu_assert_int_eq(1, fake.stops);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	/* A version that could not be read at all leaves the board open, so the
	 * tool is what closes it. */
	reset_fake();
	fake.api_rc = -1;
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
	mu_assert_int_eq(0, fake.stops);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	/* libbtbb refusing the decoder settings is the other way out with the
	 * board open. It is the real libbtbb here, so an out-of-range value is
	 * what makes it refuse; it reports that itself, and the tool adds no
	 * line of its own. */
	reset_fake();
	max_ac_errors = -1;
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
	mu_assert_int_eq(0, fake.stops);
	mu_assert(strstr(r.err, "error:") == NULL, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
}

/* The board is configured before the stream is started, so a stream that
 * cannot start has a configured board to give back: it is stopped, the poll
 * thread is never created, and no further request goes out.
 *
 * Each of these exits 1, not the failing call's own return value, which a
 * shell wraps into a byte a script cannot read. */
MU_TEST(a_stream_that_cannot_start_gives_the_board_back)
{
	char expected[128];
	struct run_result r;

	reset_fake();
	fake.bulk_init_rc = -1;
	r = RUN(TARGET);
	/* 1, not the failing call's own return value: that used to go out as
	 * the exit status, and a shell wraps it into a byte, so a script read
	 * -1 as 255. */
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "ai") == 0, fake.calls);
	mu_assert_int_eq(0, fake.thread_starts);
	mu_assert_int_eq(0, fake.receives);
	mu_assert_int_eq(1, fake.stops);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	/* The thread that collects the capture. pthread_create() reports a
	 * failure as a positive errno, which a "< 0" test let through: the
	 * follow then had nobody to complete its transfers. */
	reset_fake();
	fake.thread_start_rc = EAGAIN;
	r = RUN(TARGET);
	snprintf(expected, sizeof(expected),
	         "error: could not start the thread that collects the capture:"
	         " %s\n", strerror(EAGAIN));
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, expected) != NULL, r.err);
	mu_assert(strcmp(fake.calls, "aip") == 0, fake.calls);
	mu_assert_int_eq(0, fake.receives);
	mu_assert_int_eq(1, fake.stops);

	/* The request that starts the hopping, which libubertooth reports
	 * itself: the status is the tool's own 1. */
	reset_fake();
	fake.hopping_rc = -9;
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.receives);
	mu_assert_int_eq(1, fake.stops);
}

/* The adapter -b names, reported the way ubertooth-scan reports the same two
 * failures: one "error: " line on standard error, with a reason only when the
 * call left one behind, and exit 1 before any board is opened. */
MU_TEST(an_adapter_that_cannot_be_used_exits_1_on_standard_error)
{
	char expected[128];
	struct run_result r;

	reset_fake();
	fake.devid_fails = 1;
	fake.devid_errno = ENODEV;
	r = RUN("-b", "hci7", TARGET);
	snprintf(expected, sizeof(expected),
	         "error: Bluetooth device hci7 not found: %s\n",
	         strerror(ENODEV));
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, expected) == 0, r.err);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(0, fake.stops);

	/* The same lookup failing without leaving an errno behind says what it
	 * knows rather than printing "Undefined error: 0" as the reason. */
	reset_fake();
	fake.devid_fails = 1;
	r = RUN("-b", "hci7", TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err,
	          "error: Bluetooth device hci7 not found\n") == 0, r.err);

	/* An adapter that is there but will not open, with and without an
	 * errno. */
	reset_fake();
	fake.open_fails = 1;
	fake.open_errno = EPERM;
	r = RUN("-b", "hci7", TARGET);
	snprintf(expected, sizeof(expected),
	         "error: cannot open Bluetooth device hci7: %s\n",
	         strerror(EPERM));
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, expected) == 0, r.err);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(0, fake.stops);

	reset_fake();
	fake.open_fails = 1;
	r = RUN("-b", "hci7", TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err,
	          "error: cannot open Bluetooth device hci7\n") == 0, r.err);
}

/* -t names the whole address, and the board is told to follow the piconet
 * inside it: the LAP and UAP. Naming the same piconet the other way, as -l and
 * -u, must reach the board as the same address. */
MU_TEST(a_whole_address_and_its_lap_and_uap_follow_the_same_piconet)
{
	struct run_result r;
	uint64_t from_whole_address;

	reset_fake();
	r = RUN(WHOLE_TARGET);
	mu_assert_int_eq(0, r.rc);
	from_whole_address = fake.bdaddr;
	/* LAP 112233 under UAP 44, which is what the address carries. */
	mu_assert(from_whole_address == 0x44112233,
	          "the board was told to follow a different piconet");

	reset_fake();
	r = RUN(TARGET);
	mu_assert_int_eq(0, r.rc);
	mu_assert(fake.bdaddr == from_whole_address,
	          "-t and -l with -u reached the board as different addresses");
}

/* The map -a read is printed either way, and by the tool: it used to come
 * from inside libbtbb, which must not write to standard output. The wording
 * is the one libbtbb used, so a reader and a script see the same line. A
 * capture that has taken standard output would swallow it, so for such a run
 * it goes to standard error instead: the map is the answer to -a, and losing
 * it is losing what the command was asked for. */
MU_TEST(the_afh_map_is_printed_beside_a_capture_on_standard_output)
{
	struct run_result r;
	const char* line = "AFH map: 0xffffffffffffffffffff\n";

	reset_fake();
	r = RUN(WHOLE_TARGET, "-a");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, line) != NULL, r.out);
	/* The reading of it is reported on standard error ("AFH map read: ..."),
	 * so only the map line itself is in question here. */
	mu_assert(strstr(r.err, line) == NULL, r.err);

	/* With the capture on standard output, the map line moves to standard
	 * error, and nothing of it is left on standard output. (What the
	 * capture itself wrote is not: libbtbb flushes it when the process
	 * exits, which a main() called in a test never does.) */
	reset_fake();
	r = RUN(WHOLE_TARGET, "-a", "-q", "-");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.err, line) != NULL, r.err);
	mu_assert(strstr(r.out, "AFH map") == NULL, r.out);
}

#ifndef TEST_REAL_API_REPORTS
/* Built against an installed libubertooth, so there is no library source to
 * compile in: the report for a request the board's firmware has not got is a
 * stub here, and STALLED below then expects no line from it. */
void ubertooth_report_unsupported_request(const char* what, int code)
{
	(void)what;
	(void)code;
}
#endif

/* The whole report for a refused request: one error line naming the request,
 * why it failed and the libusb code, and a note only where there is something
 * to try. */
#ifdef TEST_REAL_API_REPORTS
#define STALLED(what) \
	"error: could not " what \
	": the board's firmware does not support it (stalled, libusb -9)\n"
#else
#define STALLED(what) ""
#endif

/* How many lines that report puts on standard error: one when the library's
 * own report is compiled in, none when it is the stub above. The cases that
 * count the error lines of a stalled request add it up rather than naming a
 * total, so they hold in both configurations. */
#ifdef TEST_REAL_API_REPORTS
#define STALLED_REQUEST_LINES 1
#else
#define STALLED_REQUEST_LINES 0
#endif
#define NO_ANSWER(what) \
	"error: could not " what \
	": the board did not answer in time (libusb -7)\n" \
	"note: if a mode is running, stop it with ubertooth-util -S; " \
	"a board that does not answer at all usually resets itself within " \
	"a few seconds, so run the command again, and unplug it or " \
	"power-cycle its hub port if it stays silent\n"

/* How many times a string occurs, for pinning that a refusal is reported once
 * rather than alongside a second line. */
static int occurrences(const char* haystack, const char* needle)
{
	int found = 0;
	const char* at = haystack;

	while ((at = strstr(at, needle)) != NULL) {
		found++;
		at += strlen(needle);
	}
	return found;
}

/* A board that refused the target's address has nothing to match packets
 * against, so the follow stops rather than capturing from a board set to
 * whatever it was left on. Standard error names the request; standard output
 * stays empty, because nothing was captured. */
MU_TEST(a_refused_address_set_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.bdaddr_rc = -9; /* LIBUSB_ERROR_PIPE */
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	/* The run stopped at the request: the receive transfer was never
	 * submitted and the board's clock was never read. */
	mu_assert(strcmp(fake.calls, "a") == 0, fake.calls);
	mu_assert_int_eq(0, fake.thread_starts);
	mu_assert_int_eq(0, fake.receives);
	mu_assert(strstr(r.err, STALLED("set the Bluetooth address")) != NULL,
	          r.err);
	mu_assert_int_eq(STALLED_REQUEST_LINES, occurrences(r.err, "error:"));
	mu_assert(r.out[0] == '\0', r.out);
	/* The board goes back without a STOP, since no mode has started, and
	 * the adapter handle goes back with it. */
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
	mu_assert_int_eq(0, fake.stops);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	/* A board that took the request and said nothing is the case the note is
	 * about. */
	reset_fake();
	fake.bdaddr_rc = -7; /* LIBUSB_ERROR_TIMEOUT */
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, NO_ANSWER("set the Bluetooth address")) != NULL,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
}

/* Without the board's own clock there is no offset to hop on: the piconet's
 * clock alone says nothing about how far ahead of the board's it is. So a
 * board that would not report it stops the run, rather than hopping on a
 * difference taken from a clock nobody read.
 *
 * The receive thread is running by then, so it is joined; the board goes back
 * without a STOP, as it does for the address, because no mode of this run has
 * started and a board that has just refused a request would answer the STOP
 * no better. That is what keeps the report to the one line that named the
 * request. */
MU_TEST(a_refused_clock_read_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.clock_rc = -9; /* LIBUSB_ERROR_PIPE */
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	/* The capture was set up first and the run stopped at the clock read,
	 * before the board was told to hop. */
	mu_assert(strcmp(fake.calls, "aipk") == 0, fake.calls);
	mu_assert_int_eq(1, fake.thread_starts);
	mu_assert_int_eq(1, fake.thread_stops);
	mu_assert_int_eq(0, fake.receives);
	mu_assert(strstr(r.err, STALLED("read the board's clock")) != NULL, r.err);
	mu_assert_int_eq(STALLED_REQUEST_LINES, occurrences(r.err, "error:"));
	mu_assert(r.out[0] == '\0', r.out);
	/* Closed, not stopped: a STOP here would be a second request to a board
	 * that has just refused one, and a second error line under the first. */
	mu_assert_int_eq(0, fake.stops);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	reset_fake();
	fake.clock_rc = -7; /* LIBUSB_ERROR_TIMEOUT */
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, NO_ANSWER("read the board's clock")) != NULL,
	          r.err);
	mu_assert_int_eq(0, fake.stops);
	mu_assert_int_eq(1, fake.closes);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
}

/* A refused request leaves no capture file behind. The files are created once
 * the board is open and its firmware accepted, which for this tool is above
 * every request the follow is built on. Each failure after they exist removes
 * them again, so a refused run never leaves a header-only capture the user
 * has to tell apart from a capture that ran and caught nothing. Each of the
 * three file options is checked, because each is opened by a writer of its
 * own. */
MU_TEST(a_refused_request_creates_no_file)
{
	static const char* const options[] = { "-q", "-r", "-d" };
	/* The whole range the removal covers, from the first failure after the
	 * files exist to the last before the capture loop, named by the
	 * requests that went out before each. The five are the five shapes of
	 * exit the tool has here: the address, the two steps that start the
	 * capture and give the board back themselves, the clock read, and
	 * everything after it. */
	static const char* const reached[] = {
		"a", "ai", "aip", "aipk", "aipkdh"
	};
	struct run_result r;
	struct stat info;
	char path[64];
	size_t i, which;

	for (i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
		for (which = 0; which < sizeof(reached) / sizeof(reached[0]);
		     which++) {
			int fd;

			reset_fake();
			switch (which) {
			case 0:
				fake.bdaddr_rc = -9; /* LIBUSB_ERROR_PIPE */
				break;
			case 1:
				fake.bulk_init_rc = -1;
				break;
			case 2:
				fake.thread_start_rc = EAGAIN;
				break;
			case 3:
				fake.clock_rc = -9;
				break;
			default:
				fake.hopping_rc = -9;
				break;
			}
			snprintf(path, sizeof(path), "/tmp/follow-main-XXXXXX");
			fd = mkstemp(path);
			mu_assert_fatal(fd >= 0, "temp name taken");
			close(fd);
			unlink(path);

			r = RUN(options[i], path, TARGET);
			mu_assert_int_eq(1, r.rc);
			/* The run did reach that failure, so the file had been
			 * created by then: the check below would otherwise
			 * pass without the fix. */
			mu_assert(strcmp(fake.calls, reached[which]) == 0,
			          fake.calls);
			mu_assert(stat(path, &info) != 0, path);
			mu_assert_int_eq(0, fake.receives);
			unlink(path);
		}
	}
}

/* A destination the run did not create is left where it is: only a regular
 * file is removed. A symbolic link stands for the whole class here, because a
 * test can make one and take it away again, where a device node needs root
 * and a pipe would block the writer that opens it. Nothing is said about it
 * either: there was nothing to remove. */
MU_TEST(a_destination_the_run_did_not_create_stays)
{
	struct run_result r;
	struct stat info;
	char target[64];
	char link[80];
	int fd;

	reset_fake();
	fake.clock_rc = -9; /* LIBUSB_ERROR_PIPE */
	snprintf(target, sizeof(target), "/tmp/follow-main-XXXXXX");
	fd = mkstemp(target);
	mu_assert_fatal(fd >= 0, "temp name taken");
	close(fd);
	snprintf(link, sizeof(link), "%s-link", target);
	unlink(link);
	mu_assert_fatal(symlink(target, link) == 0, "cannot make a link");

	r = RUN("-q", link, TARGET);

	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "aipk") == 0, fake.calls);
	/* The link is still there, and so is the file it names. */
	mu_assert(lstat(link, &info) == 0 && S_ISLNK(info.st_mode), link);
	mu_assert(stat(target, &info) == 0, target);
	mu_assert(strstr(r.err, "cannot remove") == NULL, r.err);

	unlink(link);
	unlink(target);
}

/* A piconet that cannot be allocated ends the run with its reason, rather
 * than being dereferenced where the board is told what to follow: the tool
 * used to crash, and a crash says nothing about why. */
MU_TEST(a_piconet_that_cannot_be_allocated_exits_one)
{
	struct run_result r;

	reset_fake();
	fake.no_memory = 1;

	r = RUN(TARGET);

	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: out of memory\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	/* The piconet is allocated before the adapter and the board, so there
	 * is nothing to give back. */
	mu_assert_int_eq(-1, fake.dev_fd);
	mu_assert_int_eq(0, fake.connects);
	mu_assert_int_eq(0, fake.frees);
	mu_assert(fake.calls[0] == '\0', fake.calls);

	/* Which is also why -h and -V still answer: the piconet is allocated
	 * once the command line has been accepted, not before it is read. */
	reset_fake();
	fake.no_memory = 1;

	r = RUN("-h");

	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "ubertooth-follow") != NULL, r.out);
	mu_assert(r.err[0] == '\0', r.err);

	reset_fake();
	fake.no_memory = 1;

	r = RUN("-V");

	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "version (test)") != NULL, r.out);
	mu_assert(r.err[0] == '\0', r.err);
}

/* Half of the -l/-u pair names no piconet, so the command cannot be carried
 * out as written and is refused before the adapter is opened. It used to fall
 * into the no-target path, which says "No address given" and follows the local
 * adapter's own address instead of what was asked for. */
MU_TEST(half_a_lap_uap_pair_is_refused)
{
	struct run_result r;

	reset_fake();
	r = RUN("-l", "112233");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: -l <LAP> requires -u <UAP>\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);
	/* Nothing was opened and nothing was followed: no adapter handle, no
	 * board, and not the local address the old path read. */
	mu_assert_int_eq(-1, fake.dev_fd);
	mu_assert_int_eq(0, fake.connects);
	mu_assert(fake.calls[0] == '\0', fake.calls);

	reset_fake();
	r = RUN("-u", "44");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: -u <UAP> requires -l <LAP>\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(-1, fake.dev_fd);
	mu_assert_int_eq(0, fake.connects);
	mu_assert(fake.calls[0] == '\0', fake.calls);

	/* -a beside half a pair still reports -a needing -t: completing the
	 * pair would not make -a work, so that is the refusal to answer with. */
	reset_fake();
	r = RUN("-a", "-l", "112233");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "-a reads the AFH map over a Bluetooth "
	          "connection") != NULL, r.err);
	reset_fake();
	r = RUN("-u", "44", "-a");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "-a reads the AFH map over a Bluetooth "
	          "connection") != NULL, r.err);
}

/* A follow keyed on -l and -u pages 00:00:<UAP>:<LAP>, because ordinary piconet
 * traffic does not carry the two most significant address bytes. When that page
 * fails, whatever the reason, the failure names the guess and asks for the
 * whole address with -t rather than leaving the user with a page to an address
 * they never named. */
MU_TEST(a_failed_page_to_the_guessed_address_asks_for_t)
{
	char expected[256];
	struct run_result r;

	reset_fake();
	fake.connect_fails = 1;
	fake.connect_errno = EHOSTDOWN;
	r = RUN(TARGET);
	snprintf(expected, sizeof(expected),
	         "error: connecting to 00:00:44:11:22:33 failed: %s; -l and -u "
	         "give four of the six bytes of the target address, so 00:00 is "
	         "a guess: give the whole target address with "
	         "-t xx:xx:xx:xx:xx:xx\n", strerror(EHOSTDOWN));
	mu_assert_int_eq(1, r.rc);
	/* The guessed address really is what was paged, so the advice is about
	 * the address the user would have to change. */
	mu_assert(strcmp(fake.paged, "00:00:44:11:22:33") == 0, fake.paged);
	/* strstr, not strcmp: standard error carries the tool's progress before
	 * this, which the error-line count below is what pins. */
	mu_assert(strstr(r.err, expected) != NULL, r.err);
	mu_assert_int_eq(1, occurrences(r.err, "error:"));
	mu_assert(r.out[0] == '\0', r.out);
	/* The board was never opened, and the adapter handle went back. */
	mu_assert_int_eq(0, fake.connects);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	/* The user who gave the whole address has nothing to change, so that
	 * failure is the reason alone. */
	reset_fake();
	fake.connect_fails = 1;
	fake.connect_errno = EHOSTDOWN;
	r = RUN(WHOLE_TARGET);
	snprintf(expected, sizeof(expected),
	         "error: connecting to 00:00:44:11:22:33 failed: %s\n",
	         strerror(EHOSTDOWN));
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, expected) != NULL, r.err);
	mu_assert_int_eq(1, occurrences(r.err, "error:"));
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(0, fake.connects);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	/* A page that fails without leaving an errno behind says what it knows,
	 * rather than reporting an earlier call's errno as its cause, which is
	 * how the two adapter failures above already report themselves. The
	 * fake leaves ENOTTY behind from the call before the page and then fails
	 * the page without touching errno, so a tool that did not clear errno
	 * first would print "Inappropriate ioctl for device" as the reason. */
	reset_fake();
	fake.dirty_errno = ENOTTY;
	fake.connect_fails = 1;
	fake.connect_keeps_errno = 1;
	r = RUN(WHOLE_TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err,
	          "error: connecting to 00:00:44:11:22:33 failed\n") != NULL,
	          r.err);
	mu_assert(strstr(r.err, strerror(ENOTTY)) == NULL, r.err);
	mu_assert_int_eq(1, occurrences(r.err, "error:"));
	mu_assert(r.out[0] == '\0', r.out);
}

/* A board that took only part of a write, or answered a read with fewer bytes
 * than it was asked for, is the third way one of these requests fails.
 * libubertooth has already printed the whole line, naming the request and the
 * counts, so the tool must stop and add nothing: one error line between the
 * two of them, not two. */
MU_TEST(a_truncated_write_stops_the_follow_and_adds_no_second_line)
{
	struct run_result r;

	reset_fake();
	fake.clock_rc = UBERTOOTH_ERROR_SHORT_REPLY;
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "aipk") == 0, fake.calls);
	mu_assert_int_eq(0, fake.receives);
	/* The fake stands in for libubertooth, so it prints no line of its own:
	 * what this pins is that the tool adds none either. */
	mu_assert(strstr(r.err, "error:") == NULL, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(0, fake.stops);
	mu_assert_int_eq(1, fake.closes);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	reset_fake();
	fake.bdaddr_rc = UBERTOOTH_ERROR_SHORT_REPLY;
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "a") == 0, fake.calls);
	mu_assert(strstr(r.err, "error:") == NULL, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
}

/* -w counts 625 us timeslots, and the clock the board hops on counts 312.5 us
 * ticks, so the offset it is given is twice the delay. It used to be added
 * tick for tick, which compensated half of what the option said and left the
 * default a whole slot early. */
MU_TEST(the_usb_delay_is_given_to_the_board_in_clock_ticks)
{
	struct run_result r;

	/* The faked adapter reads a clock of 1000 ticks with no offset to the
	 * target, and the faked board reports 600 of its own, so the offset the
	 * board gets is 400 plus the delay. */
	reset_fake();
	r = RUN(TARGET, "-w", "0");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(400, fake.clkn_offset);

	reset_fake();
	r = RUN(TARGET, "-w", "5");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(410, fake.clkn_offset);

	/* The default is the same five slots. */
	reset_fake();
	r = RUN(TARGET);
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(410, fake.clkn_offset);
}

/* The board is told to hop by the difference between the piconet's clock and
 * its own, because that is what it adds the offset to. The whole clock used to
 * go out instead, with CLKN zeroed a moment earlier so that the two came to
 * the same thing; a board whose clock was not zero then hopped that far ahead
 * of the piconet.
 *
 * Both clocks wrap, so a piconet behind the board is an offset that wraps with
 * them: the board adds it and comes out on the piconet's clock either way. */
MU_TEST(the_board_is_told_the_difference_between_the_two_clocks)
{
	struct run_result r;

	/* A board that has been plugged in longer than the piconet's clock has
	 * been counting: the difference is negative, and the offset wraps. */
	reset_fake();
	fake.board_clkn = 3000;
	r = RUN(TARGET, "-w", "0");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(-2000, fake.clkn_offset);

	/* A board near the end of the range CLKN counts, which it reaches after
	 * about 15 days plugged in: the difference wraps the other way, and the
	 * board still ends up on the piconet's 1000 ticks. */
	reset_fake();
	fake.board_clkn = 0xfffffc00;
	r = RUN(TARGET, "-w", "0");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1000 + 0x400, fake.clkn_offset);
	mu_assert((uint32_t)(fake.board_clkn + (uint32_t)fake.clkn_offset)
	          == 1000, "the board would not have landed on the piconet's"
	          " clock");

	/* A board whose clock already reads the piconet's needs no offset at
	 * all. */
	reset_fake();
	fake.board_clkn = 1000;
	r = RUN(TARGET, "-w", "0");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.clkn_offset);
}

int main(void)
{
	/* This binary does not link libubertooth: the fakes above are the whole
	 * device, so any library call the tool grows becomes a link error rather
	 * than a silently real one. */
	MU_RUN(the_clock_is_read_just_before_the_hopping_starts);
	MU_RUN(the_asynchronous_requests_stay_after_the_poll_thread);
	MU_RUN(a_follow_the_board_took_exits_0);
	MU_RUN(a_stop_while_the_board_was_opened_captures_nothing);
	MU_RUN(a_follow_whose_teardown_was_refused_exits_1);
	MU_RUN(a_firmware_the_tool_cannot_use_gives_the_board_back);
	MU_RUN(a_stream_that_cannot_start_gives_the_board_back);
	MU_RUN(an_adapter_that_cannot_be_used_exits_1_on_standard_error);
	MU_RUN(a_whole_address_and_its_lap_and_uap_follow_the_same_piconet);
	MU_RUN(the_afh_map_is_printed_beside_a_capture_on_standard_output);
	MU_RUN(half_a_lap_uap_pair_is_refused);
	MU_RUN(a_failed_page_to_the_guessed_address_asks_for_t);
	MU_RUN(a_refused_address_set_exits_1);
	MU_RUN(a_refused_clock_read_exits_1);
	MU_RUN(a_refused_request_creates_no_file);
	MU_RUN(a_destination_the_run_did_not_create_stays);
	MU_RUN(a_piconet_that_cannot_be_allocated_exits_one);
	MU_RUN(a_truncated_write_stops_the_follow_and_adds_no_second_line);
	MU_RUN(the_usb_delay_is_given_to_the_board_in_clock_ticks);
	MU_RUN(the_board_is_told_the_difference_between_the_two_clocks);
	return mu_report();
}
