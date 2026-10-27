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
	int thread_starts;
	int thread_stops;
	int stops;
	int receives;         /* turns of the capture loop */
	int blocks_to_end;    /* ...before the fake ends the capture */
	uint8_t afh_mode;     /* what the target reports for AFH */
	uint8_t afh_map[AFH_MAP_BYTES];
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

/* Any non-negative number: every call the tool makes on the adapter is faked
 * below, and the tool never closes it, so no real descriptor is needed. */
int hci_open_dev(int dev_id)
{
	(void)dev_id;
	return 3;
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

int cmd_set_bdaddr(struct libusb_device_handle* devh, uint64_t address)
{
	(void)devh;
	(void)address;
	record('a');
	return 0;
}

int cmd_set_clock(struct libusb_device_handle* devh, uint32_t clkn)
{
	(void)devh;
	(void)clkn;
	record('k');
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
	r = RUN("-a", TARGET);
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
	mu_assert(r.err[0] == '\0', r.err);
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
	mu_assert(r.rc != 0, "a follow that never streamed must not exit 0");
	mu_assert(strcmp(fake.calls, "aki") == 0, fake.calls);
	mu_assert_int_eq(0, fake.thread_starts);
	mu_assert_int_eq(0, fake.receives);
	mu_assert_int_eq(1, fake.stops);
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
	return mu_report();
}
