/*
 * The order in which ubertooth-follow sets a board up, run through its real
 * main() against a scripted library and a scripted BlueZ adapter.
 *
 * Zeroing the board's clock moves the clock the board stamps its records
 * with, so it has to happen before the receive transfer is submitted. It used
 * to happen afterwards: a record sent before the reset and read after it
 * carries the period count from before, the host reads that count as a
 * difference from the previous record's, and 150 followed by 0 reads as 106
 * periods gone by -- about 34,700 s added to every later timestamp.
 *
 * Which requests may move is decided by how they are sent. The address and
 * the clock go out synchronously, so the board has them before anything is in
 * flight. The AFH map set (-a) and the start-hopping request are
 * asynchronous: libusb hands their completions to whoever polls for events,
 * which here is the thread ubertooth_bulk_thread_start() creates, so those two
 * must stay after it.
 *
 * The cases below pin the whole order of requests, not only the two halves
 * that are required, so a request that moves anywhere is caught and looked at.
 * They also pin what a board that refuses one of those two requests gets: a
 * follow set up on neither the target's address nor a zeroed clock would
 * follow something the user did not ask for, so the run stops and says so.
 * Clearing the AFH map, which is what a follow without -a does, is
 * synchronous and under no ordering requirement of its own.
 *
 * ubertooth-follow.c is compiled in with main() renamed, as test_scan_main.c
 * does for the other BlueZ tool, and every libubertooth call is a fake here,
 * so no board is opened. libbtbb is the real one: the tool hands it the
 * piconet. BlueZ is faked as far as the adapter, the link to the target and
 * the clock reads, so no case needs a Bluetooth adapter on the machine running
 * the tests.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_follow_main
#include "ubertooth-follow.c"
#undef main

/* The libubertooth globals the tool uses; they live in ubertooth.c, which is
 * not linked here. */
FILE* dumpfile;
int max_ac_errors = 2;

/* ---- scripted device and adapter ---------------------------------------- */

static struct {
	int bulk_init_rc;     /* returned by ubertooth_bulk_init() */
	int bdaddr_rc;        /* returned by cmd_set_bdaddr_quiet() */
	int clock_rc;         /* returned by cmd_set_clock_quiet() */
	int closes;           /* ubertooth_close() */
	int dev_fd;           /* the adapter handle the tool was given */
	int dev_fd_closed;    /* whether the run gave it back */
	/* Whether it was already back by the first turn of the capture loop,
	 * which is what says the handle goes back before the capture rather
	 * than at the end of the run. */
	int dev_fd_closed_by_capture;
	int thread_starts;
	int thread_stops;
	int stops;
	int receives;         /* turns of the capture loop */
	int blocks_to_end;    /* ...before the fake ends the capture */
	uint8_t afh_mode;     /* what the target reports for AFH */
	uint8_t afh_map[AFH_MAP_BYTES];
	uint64_t bdaddr;      /* the address the board was told to follow */
	/* Every device call in order, one letter each: a set the target's
	 * address, k set the board's clock, i submitted the receive transfer,
	 * p started the poll thread, m set the AFH map, c cleared it, h told
	 * the board to start hopping. */
	char calls[16];
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
	fake.dev_fd = -1;
	fake_ut.devh = (struct libusb_device_handle*)&fake_devh;
	fake.blocks_to_end = 2;
	/* A target using AFH on every channel, so -a reaches the map set. */
	fake.afh_mode = FOLLOW_AFH_MODE_ON;
	memset(fake.afh_map, 0xff, sizeof(fake.afh_map));
}

/* ---- faked BlueZ -------------------------------------------------------- */

int hci_devid(const char* str)
{
	(void)str;
	return 0;
}

/* A real descriptor, so the tool's close() is a real close and giving the
 * adapter handle back can be checked, but one that is not a Bluetooth socket:
 * every call the tool makes on the adapter is faked below. */
int hci_open_dev(int dev_id)
{
	(void)dev_id;
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
	return 0;
}

int hci_create_connection(int dd, const bdaddr_t* bdaddr, uint16_t ptype,
                          uint16_t clkoffset, uint8_t rswitch,
                          uint16_t* handle, int to)
{
	(void)dd;
	(void)bdaddr;
	(void)ptype;
	(void)clkoffset;
	(void)rswitch;
	(void)to;
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

/* No case here refuses a board or a firmware version: what the tool does with
 * those is checked against the real binary by board-not-found-ubertooth-follow
 * and by the output-stream cases. */
int ubertooth_connect_select(ubertooth_t* ut, const char* u_arg)
{
	(void)ut;
	(void)u_arg;
	return 0;
}

int ubertooth_check_api(ubertooth_t* ut)
{
	(void)ut;
	return 0;
}

/* The real one folds the signal handler's own flag into the handle; the fake
 * ubertooth_bulk_receive() below sets the handle directly, so reading it is
 * enough here. */
int ubertooth_stopping(ubertooth_t* ut)
{
	return ut->stop_ubertooth;
}

void register_cleanup_handler(ubertooth_t* ut, int do_exit)
{
	(void)ut;
	(void)do_exit;
}

int cmd_set_bdaddr_quiet(struct libusb_device_handle* devh, uint64_t address)
{
	(void)devh;
	fake.bdaddr = address;
	record('a');
	return fake.bdaddr_rc;
}

int cmd_set_clock_quiet(struct libusb_device_handle* devh, uint32_t clkn)
{
	(void)devh;
	(void)clkn;
	record('k');
	return fake.clock_rc;
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

int cmd_start_hopping(struct libusb_device_handle* devh, int clkn_offset,
                      int clk100ns_offset)
{
	(void)devh;
	(void)clkn_offset;
	(void)clk100ns_offset;
	record('h');
	return 0;
}

int ubertooth_bulk_init(ubertooth_t* ut)
{
	(void)ut;
	record('i');
	return fake.bulk_init_rc;
}

int ubertooth_bulk_thread_start()
{
	record('p');
	fake.thread_starts++;
	return 0;
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
	(void)ut;
	fake.stops++;
}

void ubertooth_close(ubertooth_t* ut)
{
	ut->devh = NULL;
	fake.closes++;
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

struct run_result {
	int rc;
	char out[8192];
	char err[4096];
};

static void read_back(FILE* file, char* buffer, size_t size)
{
	size_t length;

	fflush(file);
	rewind(file);
	length = fread(buffer, 1, size - 1, file);
	buffer[length] = '\0';
	/* A capture that did not fit must fail loudly instead of comparing a
	 * prefix. */
	mu_assert(fgetc(file) == EOF, "captured output was truncated");
}

static struct run_result run_follow(const char* const* args)
{
	struct run_result result;
	char* argv[16];
	int argc = 0;
	int saved_out, saved_err;
	FILE* out = tmpfile();
	FILE* err = tmpfile();

	memset(&result, 0, sizeof(result));
	result.rc = -1; /* a failed capture must not look like a successful run */
	mu_assert(out != NULL && err != NULL, "tmpfile failed");
	if (out == NULL || err == NULL) {
		if (out != NULL)
			fclose(out);
		if (err != NULL)
			fclose(err);
		return result;
	}
	while (args[argc] != NULL && argc < 15) {
		argv[argc] = (char*)args[argc];
		argc++;
	}
	argv[argc] = NULL;

	fflush(stdout);
	fflush(stderr);
	saved_out = dup(STDOUT_FILENO);
	saved_err = dup(STDERR_FILENO);
	dup2(fileno(out), STDOUT_FILENO);
	dup2(fileno(err), STDERR_FILENO);
	optind = 0; /* restart option parsing for each run */
	result.rc = ubertooth_follow_main(argc, argv);
	/* Read before anything else can take the descriptor's number back. */
	fake.dev_fd_closed = fake.dev_fd >= 0
	                     && fcntl(fake.dev_fd, F_GETFD) == -1
	                     && errno == EBADF;
	fflush(stdout);
	fflush(stderr);
	dup2(saved_out, STDOUT_FILENO);
	dup2(saved_err, STDERR_FILENO);
	close(saved_out);
	close(saved_err);

	read_back(out, result.out, sizeof(result.out));
	read_back(err, result.err, sizeof(result.err));
	fclose(out);
	fclose(err);
	return result;
}

#define RUN(...) run_follow((const char* const[]){ "ubertooth-follow", __VA_ARGS__, NULL })

/* The whole setup a follow of a given target does, in order. */
#define TARGET "-l", "112233", "-u", "44"
/* The same piconet named as a whole address, which is what -a needs: the map
 * is read over a connection to the target, and -l with -u is four of the six
 * bytes. 00:00 is the address -l and -u imply. */
#define WHOLE_TARGET "-t", "00:00:44:11:22:33"

/* ---- tests -------------------------------------------------------------- */

/* The board's clock is zeroed before the receive transfer is submitted, so no
 * record the host reads can carry a count from before the reset. */
MU_TEST(the_clock_is_set_before_the_receive_transfer)
{
	struct run_result r;

	reset_fake();
	r = RUN(TARGET);
	mu_assert_int_eq(0, r.rc);
	mu_assert(call_at('k') >= 0, "the board's clock was never set");
	mu_assert(call_at('i') >= 0, "the receive transfer was never submitted");
	mu_assert(call_at('k') < call_at('i'),
	          "the clock was zeroed after the receive transfer was submitted:"
	          " a record from before the reset carries the period count from"
	          " before it");
	/* The whole order, so a request that moves anywhere is caught: address,
	 * clock, receive transfer, poll thread, hopping, AFH map cleared. The
	 * map goes out after hopping has started because starting a mode forgets
	 * it, so one sent first would be thrown away. */
	mu_assert(strcmp(fake.calls, "akiphc") == 0, fake.calls);
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
	mu_assert(strcmp(fake.calls, "akiphm") == 0, fake.calls);
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
}

/* The board is configured before the stream is started, so a stream that
 * cannot start has a configured board to give back: it is stopped, the poll
 * thread is never created, and no further request goes out. */
MU_TEST(a_stream_that_cannot_start_gives_the_board_back)
{
	struct run_result r;

	reset_fake();
	fake.bulk_init_rc = -1;
	r = RUN(TARGET);
	/* 1, not the failing call's own return value: that used to go out as
	 * the exit status, and a shell wraps it into a byte, so a script read
	 * -1 as 255. */
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "aki") == 0, fake.calls);
	mu_assert_int_eq(0, fake.thread_starts);
	mu_assert_int_eq(0, fake.receives);
	mu_assert_int_eq(1, fake.stops);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
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

/* The whole report for a refused request: one error line naming the request,
 * why it failed and the libusb code, and a note only where there is something
 * to try. */
#define STALLED(what) \
	"error: could not " what \
	": the board does not support this command (stalled) (libusb -9)\n"
#define NO_ANSWER(what) \
	"error: could not " what \
	": the board did not answer in time (libusb -7)\n" \
	"note: if a mode is running, stop it with ubertooth-util -S; if the " \
	"board does not answer at all, re-plug it\n"

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
	/* The run stopped at the request: the clock was never set and the
	 * receive transfer was never submitted. */
	mu_assert(strcmp(fake.calls, "a") == 0, fake.calls);
	mu_assert_int_eq(0, fake.thread_starts);
	mu_assert_int_eq(0, fake.receives);
	mu_assert(strstr(r.err, STALLED("set the Bluetooth address")) != NULL,
	          r.err);
	mu_assert_int_eq(1, occurrences(r.err, "error:"));
	mu_assert(r.out[0] == '\0', r.out);
	/* The board goes back without a STOP, since no mode has started, and
	 * the adapter handle goes back with it. */
	mu_assert_int_eq(1, fake.closes);
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

/* A board that refused the clock reset keeps the clock it has been running
 * since it was plugged in. Start-hopping adds its offset to that rather than
 * replacing it, so the follow would hop on the wrong clock and every packet
 * time would be out by however long the board had been up. */
MU_TEST(a_refused_clock_set_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.clock_rc = -9; /* LIBUSB_ERROR_PIPE */
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	/* The address went out first and the run stopped at the clock. */
	mu_assert(strcmp(fake.calls, "ak") == 0, fake.calls);
	mu_assert_int_eq(0, fake.thread_starts);
	mu_assert_int_eq(0, fake.receives);
	mu_assert(strstr(r.err, STALLED("zero the board's clock")) != NULL, r.err);
	mu_assert_int_eq(1, occurrences(r.err, "error:"));
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(0, fake.stops);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	reset_fake();
	fake.clock_rc = -7; /* LIBUSB_ERROR_TIMEOUT */
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, NO_ANSWER("zero the board's clock")) != NULL,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
}

/* A board that took only part of a write is the third way one of these
 * requests fails. libubertooth has already printed the whole line, naming the
 * request and the counts, so the tool must stop and add nothing: one error
 * line between the two of them, not two. */
MU_TEST(a_truncated_write_stops_the_follow_and_adds_no_second_line)
{
	struct run_result r;

	reset_fake();
	fake.clock_rc = UBERTOOTH_ERROR_SHORT_REPLY;
	r = RUN(TARGET);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "ak") == 0, fake.calls);
	mu_assert_int_eq(0, fake.receives);
	/* The fake stands in for libubertooth, so it prints no line of its own:
	 * what this pins is that the tool adds none either. */
	mu_assert(strstr(r.err, "error:") == NULL, r.err);
	mu_assert(r.out[0] == '\0', r.out);
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

int main(void)
{
	/* This binary does not link libubertooth: the fakes above are the whole
	 * device, so any library call the tool grows becomes a link error rather
	 * than a silently real one. */
	MU_RUN(the_clock_is_set_before_the_receive_transfer);
	MU_RUN(the_asynchronous_requests_stay_after_the_poll_thread);
	MU_RUN(a_follow_the_board_took_exits_0);
	MU_RUN(a_stream_that_cannot_start_gives_the_board_back);
	MU_RUN(a_whole_address_and_its_lap_and_uap_follow_the_same_piconet);
	MU_RUN(a_refused_address_set_exits_1);
	MU_RUN(a_refused_clock_set_exits_1);
	MU_RUN(a_truncated_write_stops_the_follow_and_adds_no_second_line);
	return mu_report();
}
