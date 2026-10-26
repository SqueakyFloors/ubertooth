/*
 * What ubertooth-rx's exit status says about a live capture, run through its
 * real main() against a scripted library.
 *
 * A capture whose USB stream fails stops where it failed, so it holds less
 * than was asked for. It used to exit 0 all the same: the library printed the
 * transfer error and the receive loop kept spinning with nothing arriving
 * until the -t timer or a Ctrl-C ended the run, which a script read as a
 * clean capture. It now exits 1, while the two ordinary ends -- the timeout
 * and the interrupt -- keep exiting 0 with whatever was captured.
 *
 * main() is in ubertooth-rx.c, so that source is compiled in here with main()
 * renamed, as test_util_main.c does. Every libubertooth call is a fake below,
 * so no board is opened and no 64 MB ring buffer is allocated; libbtbb is the
 * real one, since the tool hands it the piconet and the survey.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_rx_main
#include "ubertooth-rx.c"
#undef main

/* The libubertooth globals ubertooth-rx uses. They live in ubertooth.c, which
 * is not linked here. */
FILE* infile;
FILE* dumpfile;
uint32_t systime;
int max_ac_errors = 2;

/* ---- a scripted library ------------------------------------------------- */

/* How a capture ends, once the receive loop is running. */
enum capture_end {
	END_BY_TIMEOUT,  /* the -t timer, or a board that simply stops: the fake
	                  * stops the loop the way libubertooth's handler does */
	END_BY_STREAM_FAILURE,
	END_BY_INTERRUPT
};

static struct {
	int connect_calls, check_api_calls, cleanup_handlers;
	int bulk_inits, thread_starts, thread_stops, rx_syms_calls, stops;
	int channels_set;
	int timeout_set;
	int blocks;          /* blocks handed to the callback */
	int blocks_to_end;   /* ...before the fake ends the capture */
	int connect_fails;
	int stream_failed;
	enum capture_end end;
	char selector[64];
	ubertooth_t ut;
} device;

static void reset_device(void)
{
	memset(&device, 0, sizeof(device));
	device.blocks_to_end = 3;
	device.ut.devh = (struct libusb_device_handle*)&device;
	/* Not a real ring buffer: the fake receive below never fills one, and
	 * the packet callback is faked too. */
	device.ut.fifo = (fifo_t*)malloc(1);
}

ubertooth_t* ubertooth_init(void)
{
	return &device.ut;
}

void ubertooth_free(ubertooth_t* ut)
{
	(void)ut;
}

int ubertooth_connect_select(ubertooth_t* ut, const char* u_arg)
{
	(void)ut;
	device.connect_calls++;
	snprintf(device.selector, sizeof(device.selector), "%s",
	         u_arg ? u_arg : "");
	return device.connect_fails ? -1 : 0;
}

int ubertooth_check_api(ubertooth_t* ut)
{
	(void)ut;
	device.check_api_calls++;
	return 0;
}

/* The one the library installs stops the capture on Ctrl-C; the fake does the
 * same, so a case can raise a real signal and check what the tool makes of
 * it. */
static void fake_cleanup(int sig)
{
	(void)sig;
	device.ut.stop_ubertooth = 1;
}

void register_cleanup_handler(ubertooth_t* ut, int do_exit)
{
	(void)ut;
	(void)do_exit;
	device.cleanup_handlers++;
	signal(SIGINT, fake_cleanup);
}

/* Recorded rather than armed: a real alarm would have to expire for the case
 * that must not wait for it. */
void ubertooth_set_timeout(ubertooth_t* ut, int seconds)
{
	(void)ut;
	device.timeout_set = seconds;
}

int cmd_set_channel(struct libusb_device_handle* devh, uint16_t channel)
{
	(void)devh;
	(void)channel;
	device.channels_set++;
	return 0;
}

int cmd_rx_syms(struct libusb_device_handle* devh)
{
	(void)devh;
	device.rx_syms_calls++;
	return 0;
}

int cmd_set_bdaddr(struct libusb_device_handle* devh, uint64_t address)
{
	(void)devh;
	(void)address;
	return 0;
}

int ubertooth_bulk_init(ubertooth_t* ut)
{
	(void)ut;
	device.bulk_inits++;
	return 0;
}

int ubertooth_bulk_thread_start(void)
{
	device.thread_starts++;
	return 0;
}

void ubertooth_bulk_thread_stop(void) { device.thread_stops++; }

/* One block per call, as the board would send them, and the capture ends
 * after blocks_to_end of them in the way the case asked for. A live capture
 * never ends by itself. */
int ubertooth_bulk_receive(ubertooth_t* ut, rx_callback cb, void* cb_args)
{
	cb(ut, cb_args);
	device.blocks++;
	if (device.blocks < device.blocks_to_end)
		return 0;
	switch (device.end) {
	case END_BY_STREAM_FAILURE:
		/* What libubertooth does: report the transfer error, record it on
		 * the handle, and stop the capture there. */
		fprintf(stderr, "error: receive transfer failed: Transfer error."
		        " (1)\n");
		device.stream_failed = 1;
		ut->stop_ubertooth = 1;
		return 1;
	case END_BY_INTERRUPT:
		raise(SIGINT);
		return 0;
	case END_BY_TIMEOUT:
		ut->stop_ubertooth = 1;
		return 1;
	}
	return 0;
}

int ubertooth_stream_failed(ubertooth_t* ut)
{
	(void)ut;
	return device.stream_failed;
}

void ubertooth_stop(ubertooth_t* ut)
{
	(void)ut;
	device.stops++;
}

/* The packet decode is libubertooth's and needs none of this: what a block
 * contains does not change any exit status here. */
void cb_rx(ubertooth_t* ut, void* args)
{
	(void)ut;
	(void)args;
}

void print_version(void) { printf("test\n"); }

int stream_rx_file(ubertooth_t* ut, FILE* fp, rx_callback cb, void* cb_args)
{
	(void)ut;
	(void)fp;
	(void)cb;
	(void)cb_args;
	return 0;
}

int rx_file_report(int result, const char* filename)
{
	(void)result;
	(void)filename;
	return 0;
}

/* ---- running the tool --------------------------------------------------- */

struct run_result {
	int rc;
	char out[4096];
	char err[4096];
};

static void read_back(FILE* file, char* buffer, size_t size)
{
	size_t length;

	fflush(file);
	rewind(file);
	length = fread(buffer, 1, size - 1, file);
	buffer[length] = '\0';
	mu_assert(fgetc(file) == EOF, "captured output was truncated");
}

static struct run_result run_rx(const char* const* args)
{
	struct run_result result;
	char* argv[16];
	int argc = 0;
	FILE* out = tmpfile();
	FILE* err = tmpfile();
	int saved_out = dup(fileno(stdout));
	int saved_err = dup(fileno(stderr));

	memset(&result, 0, sizeof(result));
	result.rc = -1; /* a failed capture must not look like a successful run */
	mu_assert(out != NULL && err != NULL && saved_out >= 0 && saved_err >= 0,
	          "capture output");
	if (out == NULL || err == NULL || saved_out < 0 || saved_err < 0)
		return result;
	while (args[argc] != NULL && argc < 15) {
		argv[argc] = (char*)args[argc];
		argc++;
	}
	argv[argc] = NULL;

	infile = NULL;
	dumpfile = NULL;
	optind = 0; /* restart option parsing for each run */

	fflush(stdout);
	fflush(stderr);
	dup2(fileno(out), fileno(stdout));
	dup2(fileno(err), fileno(stderr));
	result.rc = ubertooth_rx_main(argc, argv);
	fflush(stdout);
	fflush(stderr);
	dup2(saved_out, fileno(stdout));
	dup2(saved_err, fileno(stderr));
	close(saved_out);
	close(saved_err);

	read_back(out, result.out, sizeof(result.out));
	read_back(err, result.err, sizeof(result.err));
	fclose(out);
	fclose(err);
	signal(SIGINT, SIG_DFL);
	return result;
}

#define RUN_RX(...) \
	run_rx((const char* const[]){ "ubertooth-rx", __VA_ARGS__, NULL })

/* ---- tests -------------------------------------------------------------- */

MU_TEST(a_capture_whose_stream_died_exits_one)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_STREAM_FAILURE;
	device.blocks_to_end = 2;

	r = RUN_RX("-t", "30");

	mu_assert_int_eq(1, r.rc);
	/* On standard error, where the library put it. */
	mu_assert(strstr(r.err, "error: receive transfer failed") != NULL, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	/* The capture stopped at the failure rather than running on to the -t
	 * the command asked for, which is what the tool used to wait out. */
	mu_assert_int_eq(2, device.blocks);
	mu_assert_int_eq(30, device.timeout_set);
	mu_assert_int_eq(1, device.thread_stops);
	mu_assert_int_eq(1, device.stops);
}

/* A capture that ran to its timeout did what was asked: it keeps exiting 0.
 * This is the regression that matters, since it is how a bounded capture
 * normally ends. */
MU_TEST(a_bounded_capture_still_exits_zero)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;

	r = RUN_RX("-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	/* Not cut short: every block the board sent was taken. */
	mu_assert_int_eq(3, device.blocks);
	mu_assert_int_eq(10, device.timeout_set);
	mu_assert_int_eq(1, device.bulk_inits);
	mu_assert_int_eq(1, device.rx_syms_calls);
	mu_assert_int_eq(1, device.thread_starts);
	mu_assert_int_eq(1, device.thread_stops);
	mu_assert_int_eq(1, device.stops);
}

/* A Ctrl-C is a user stopping a capture that was working. */
MU_TEST(an_interrupted_capture_still_exits_zero)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_INTERRUPT;
	device.blocks_to_end = 2;

	r = RUN_RX("-U", "0");

	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(2, device.blocks);
	mu_assert(strcmp(device.selector, "0") == 0, device.selector);
}

/* Survey mode prints what it found whatever ended the capture: the results
 * are what arrived, and the exit status is what says they are incomplete. */
MU_TEST(a_survey_whose_stream_died_still_prints_its_results)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_STREAM_FAILURE;
	device.blocks_to_end = 1;

	r = RUN_RX("-z", "-t", "30");

	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, "Survey Results") != NULL, r.out);
	mu_assert(strstr(r.err, "error: receive transfer failed") != NULL, r.err);
}

/* A command line the tool will not take is refused before anything is
 * opened, so the exit status above cannot come from a run that never
 * started. */
MU_TEST(a_refused_command_line_opens_no_device)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: -u <UAP> requires -l <LAP>") != NULL,
	          r.err);
	mu_assert_int_eq(0, device.connect_calls);
	mu_assert_int_eq(0, device.blocks);
}

int main(void)
{
	/* Nothing here should look for a board; make sure of it. */
	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	MU_RUN(a_capture_whose_stream_died_exits_one);
	MU_RUN(a_bounded_capture_still_exits_zero);
	MU_RUN(an_interrupted_capture_still_exits_zero);
	MU_RUN(a_survey_whose_stream_died_still_prints_its_results);
	MU_RUN(a_refused_command_line_opens_no_device);
	return mu_report();
}
