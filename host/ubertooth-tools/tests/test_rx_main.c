/*
 * What ubertooth-rx's exit status says about a live capture, and about the
 * requests that set one up, run through its real main() against a scripted
 * library.
 *
 * A board that refuses one of those requests used to be ignored, so the
 * capture ran on whatever channel and address the board was left on and the
 * run reported success. A refused request is now named and ends the command.
 *
 * A capture that cannot be started exits 1. The failing call's own return
 * value used to go out as the exit status, and a shell wraps that into a
 * byte, so a script saw 255 or 247 rather than a failure it could read.
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
 * so no board is opened and no 72 MB ring buffer is allocated; libbtbb is the
 * real one, since the tool hands it the piconet and the survey.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_rx_main
#include "ubertooth-rx.c"
#undef main

#include "run_main.h"

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
	int bdaddrs_set;
	int channel_rc;      /* returned by cmd_set_channel_quiet() */
	int bdaddr_rc;       /* returned by cmd_set_bdaddr_quiet() */
	/* Returned by the three steps that set a capture up: the receive
	 * transfer, the poll thread that completes it, and the request that
	 * starts the mode. */
	int bulk_init_rc;
	int thread_start_rc;
	int rx_syms_rc;
	int api_rc;          /* returned by ubertooth_check_api() */
	int api_released;    /* whether that call released the board itself */
	int closes;          /* ubertooth_close() */
	int frees;           /* ubertooth_free() */
	int last_channel;    /* the channel the capture was set to, in MHz */
	int timeout_set;
	int blocks;          /* blocks handed to the callback */
	int blocks_to_end;   /* ...before the fake ends the capture */
	int connect_fails;
	int stream_failed;
	enum capture_end end;
	char selector[64];
	/* Every request the tool sent, in order, one letter each: b the
	 * Bluetooth address, s start sending symbols, c the channel or the
	 * sweep. */
	char calls[16];
	ubertooth_t ut;
	/* 1 to have the teardown's STOP refused, so the board is left running
	 * the mode the capture started. */
	int stop_refused;
} device;

static void record(char call)
{
	size_t length = strlen(device.calls);

	if (length + 1 < sizeof(device.calls)) {
		device.calls[length] = call;
		device.calls[length + 1] = '\0';
	}
}

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

/* Counted rather than freeing: the handle above is this test's own. */
void ubertooth_free(ubertooth_t* ut)
{
	(void)ut;
	device.frees++;
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
	device.check_api_calls++;
	/* The library releases the board itself when the firmware reports a
	 * version this host will not drive; a version it could not read at all
	 * leaves the board open for the caller to close. */
	if (device.api_rc < 0 && device.api_released) {
		ut->devh = NULL;
		device.stops++;
	}
	return device.api_rc;
}

/* The real one also closes the capture files the handle holds, which is what
 * flushes a PCAP header out, so the fake does it too: a case that asks where
 * a capture went has to see what the writer wrote. */
void ubertooth_close(ubertooth_t* ut)
{
	ut->devh = NULL;
	device.closes++;
	if (ut->h_pcap_bredr != NULL) {
		btbb_pcap_close(ut->h_pcap_bredr);
		ut->h_pcap_bredr = NULL;
	}
	if (ut->h_pcapng_bredr != NULL) {
		btbb_pcapng_close(ut->h_pcapng_bredr);
		ut->h_pcapng_bredr = NULL;
	}
}

/* The one the library installs stops the capture on Ctrl-C; the fake does the
 * same, so a case can raise a real signal and check what the tool makes of
 * it. */
static void fake_cleanup(int sig)
{
	(void)sig;
	device.ut.stop_ubertooth = 1;
}

/* The real one folds the signal handler's own flag into the handle; the
 * fake handler below sets the handle directly, so reading it is enough here. */
int ubertooth_stopping(ubertooth_t* ut)
{
	return ut->stop_ubertooth;
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

int cmd_set_channel_quiet(struct libusb_device_handle* devh, uint16_t channel)
{
	(void)devh;
	record('c');
	device.channels_set++;
	device.last_channel = channel;
	return device.channel_rc;
}

/* The quiet form: it prints nothing, so what the cases below see on standard
 * error is the tool's own report. */
int cmd_rx_syms_quiet(struct libusb_device_handle* devh)
{
	(void)devh;
	record('s');
	device.rx_syms_calls++;
	return device.rx_syms_rc;
}

int cmd_set_bdaddr_quiet(struct libusb_device_handle* devh, uint64_t address)
{
	(void)devh;
	(void)address;
	record('b');
	device.bdaddrs_set++;
	return device.bdaddr_rc;
}

int ubertooth_bulk_init(ubertooth_t* ut)
{
	(void)ut;
	device.bulk_inits++;
	if (device.bulk_init_rc < 0)
		/* As the library reports a transfer libusb would not take: the
		 * libusb code in the line, -1 to the caller. */
		fprintf(stderr, "error: submitting the receive transfer failed "
		        "(%d)\n", LIBUSB_ERROR_NO_DEVICE);
	return device.bulk_init_rc;
}

/* pthread_create() returns an errno, which is positive, and the tool has to
 * read it that way: the fake hands back whatever the case asked for. */
int ubertooth_bulk_thread_start(void)
{
	device.thread_starts++;
	return device.thread_start_rc;
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


/* The end-of-capture gap report, which the tool calls and no case here reads:
 * a capture whose records never carried FIFO_OVERFLOW has nothing to say. */
void ubertooth_report_queue_gaps(ubertooth_t* ut)
{
	(void)ut;
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
	return device.stop_refused;
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

static struct run_result run_rx(const char* const* args)
{
	struct run_result result;

	/* The two file globals the tool leaves behind: a run that opened one
	 * would otherwise hand the next run a stale handle. */
	infile = NULL;
	dumpfile = NULL;

	result = run_tool_main(ubertooth_rx_main, args);

	/* A run that was interrupted leaves this tool's own SIGINT handler
	 * installed, and the rest of the test run must not be caught by it. */
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

/* A capture bound for standard output takes that stream over and writes its
 * PCAP header there, and the run's own lines go to standard error instead.
 * This is what the build's capture-to-stdout smoke test used to check on a
 * runner with no board: the header is now written only once the board is
 * usable, so it is pinned here, where a board can be faked. The first bytes
 * are libpcap's nanosecond magic and the file format version; the rest of the
 * header is libpcap's own. */
MU_TEST(a_capture_on_standard_output_gets_the_stream_and_its_header)
{
	static const unsigned char HEADER_START[] = {
		0x4d, 0x3c, 0xb2, 0xa1,  /* magic, little-endian, nanoseconds */
		0x02, 0x00, 0x04, 0x00   /* format version 2.4 */
	};
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 1;

	r = RUN_RX("-q", "-", "-z", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert(memcmp(r.out, HEADER_START, sizeof(HEADER_START)) == 0,
	          "standard output starts with the PCAP header");
	/* The survey results are what this run was asked for, and the capture
	 * has taken standard output, so they are on standard error. */
	mu_assert(strstr(r.err, "Survey Results") != NULL, r.err);
}

/* A teardown the board refused leaves that board running the capture mode,
 * and no other tool can open it until it is reset or re-plugged, so the run
 * did not do what was asked. What was captured is still written out, and
 * libubertooth has already named the refused request, so nothing is added to
 * standard error here. This is the fault a lab board produced by itself: the
 * capture ran its timeout, so the run used to look like one that worked. */
MU_TEST(a_capture_whose_teardown_was_refused_exits_one)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;
	device.stop_refused = 1;

	r = RUN_RX("-t", "10");

	mu_assert_int_eq(1, r.rc);
	/* Nothing of this tool's own: the library printed the refusal. */
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert(r.out[0] == '\0', r.out);
	/* The capture itself ran to the end and kept every block. */
	mu_assert_int_eq(3, device.blocks);
	mu_assert_int_eq(1, device.stops);

	/* The same capture on a board that takes the stop. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(0, r.rc);
}

/* A survey whose teardown was refused still prints what it found: those
 * packets are real. Only the status says the board was left running. */
MU_TEST(a_survey_whose_teardown_was_refused_still_prints_its_results)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 1;
	device.stop_refused = 1;

	r = RUN_RX("-z", "-t", "10");

	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, "Survey Results") != NULL, r.out);
}

/* A run refused for its firmware creates no capture file: an empty one left
 * behind is a file the user then has to tell apart from a capture that caught
 * nothing. Each of the three file options is checked, because each is opened
 * by a writer of its own. */
MU_TEST(a_run_refused_for_its_firmware_creates_no_file)
{
	static const char* const options[] = { "-q", "-r", "-d" };
	struct run_result r;
	struct stat info;
	char path[64];
	size_t i;

	for (i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
		int fd;

		reset_device();
		device.api_rc = -1;
		device.api_released = 1;
		snprintf(path, sizeof(path), "/tmp/rx-main-XXXXXX");
		fd = mkstemp(path);
		mu_assert_fatal(fd >= 0, "temp name taken");
		close(fd);
		unlink(path);

		r = RUN_RX(options[i], path, "-t", "10");
		mu_assert_int_eq(1, r.rc);
		mu_assert_int_eq(1, device.check_api_calls);
		mu_assert(stat(path, &info) != 0, path);
		mu_assert_int_eq(0, device.rx_syms_calls);
		unlink(path);
	}
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

/* What the setup requests carry on a capture the board took. A command line
 * without -c asks for the sweep, which the board is told by a channel above
 * the band; -c sends that channel as a frequency; -l with -u sends the address
 * too. This is the regression that matters, since it is every normal run. */
MU_TEST(a_capture_the_board_took_sets_up_and_exits_zero)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.channels_set);
	mu_assert_int_eq(9999, device.last_channel);
	mu_assert_int_eq(0, device.bdaddrs_set);
	mu_assert_int_eq(1, device.rx_syms_calls);
	mu_assert(r.err[0] == '\0', r.err);

	/* -s states that default, so it leaves the sweep in place. */
	reset_device();
	r = RUN_RX("-s", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(9999, device.last_channel);

	reset_device();
	r = RUN_RX("-c", "39", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2441, device.last_channel);
	mu_assert(r.err[0] == '\0', r.err);

	reset_device();
	r = RUN_RX("-l", "112233", "-u", "ab", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.bdaddrs_set);
	mu_assert_int_eq(1, device.channels_set);
	mu_assert_int_eq(1, device.rx_syms_calls);
	mu_assert(r.err[0] == '\0', r.err);
}

/* The order the requests go in, which is the whole of what the board keeps:
 * the capture is asked for first and the channel or the sweep stated after it.
 * Stating it first lost it to the reset a mode start runs, which puts the
 * board's hop mode back to "no hopping" -- so a capture started on a board a
 * killed ubertooth-afh had left capturing sat on one channel while the command
 * line asked for the whole band. The address request is not part of that and
 * still goes first. */
MU_TEST(the_capture_is_asked_for_before_the_channel)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(device.calls, "sc") == 0, device.calls);

	reset_device();
	r = RUN_RX("-c", "39", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(device.calls, "sc") == 0, device.calls);

	reset_device();
	r = RUN_RX("-l", "112233", "-u", "ab", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(device.calls, "bsc") == 0, device.calls);
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
#define NO_ANSWER(what) \
	"error: could not " what \
	": the board did not answer in time (libusb -7)\n" \
	"note: if a mode is running, stop it with ubertooth-util -S; " \
	"a board that does not answer at all usually resets itself within " \
	"a few seconds, so run the command again, and unplug it or " \
	"power-cycle its hub port if it stays silent\n"

/* Each refused setup request is named, with the reason and what to try, on
 * standard error, and ends the command there, with nothing on standard output
 * and nothing captured. The channel request is named for what the command line
 * asked of it, because a run without -c never named a channel. */
MU_TEST(a_refused_setup_request_is_named_and_exits_one)
{
	static const struct {
		const char* option;  /* the extra option for this case, if any */
		const char* value;
		int refuse_channel;  /* which request the board refuses */
		int code;            /* what the request came back with */
		int channels;        /* requests made before the refusal */
		int bdaddrs;
		const char* message;
	} cases[] = {
		/* LIBUSB_ERROR_PIPE, how a board refuses a request it does not
		 * implement: there is nothing to suggest, so no note follows. */
		{ NULL, NULL, 1, -9, 1, 0, STALLED("set sweep mode") },
		{ "-c", "39", 1, -9, 1, 0, STALLED("set the channel") },
		{ "-u", "ab", 0, -9, 0, 1, STALLED("set the Bluetooth address") },
		/* LIBUSB_ERROR_TIMEOUT: the board took the request and said
		 * nothing, which the note is about. */
		{ "-c", "39", 1, -7, 1, 0, NO_ANSWER("set the channel") },
		{ "-u", "ab", 0, -7, 0, 1,
		  NO_ANSWER("set the Bluetooth address") },
		/* LIBUSB_ERROR_NO_DEVICE: the other code with advice. */
		{ NULL, NULL, 1, -4, 1, 0,
		  "error: could not set sweep mode: the board is no longer "
		  "attached (libusb -4)\n"
		  "note: re-attach the board and run the command again\n" },
		/* LIBUSB_ERROR_OVERFLOW and LIBUSB_ERROR_IO: named, no advice. */
		{ "-c", "39", 1, -8, 1, 0,
		  "error: could not set the channel: the board sent more data "
		  "than the request allows (libusb -8)\n" },
		{ "-c", "39", 1, -1, 1, 0,
		  "error: could not set the channel: the transfer failed "
		  "(libusb -1)\n" },
		/* A code with no plain words still names the request and the
		 * number, rather than printing nothing. */
		{ NULL, NULL, 1, -5, 1, 0,
		  "error: could not set sweep mode (libusb -5)\n" },
	};
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reset_device();
		if (cases[i].refuse_channel)
			device.channel_rc = cases[i].code;
		else
			device.bdaddr_rc = cases[i].code;

		if (cases[i].option != NULL)
			r = RUN_RX("-l", "112233", cases[i].option,
			           cases[i].value, "-t", "10");
		else
			r = RUN_RX("-l", "112233", "-t", "10");

		mu_assert_int_eq(1, r.rc);
		mu_assert(strcmp(r.err, cases[i].message) == 0, r.err);
		mu_assert(r.out[0] == '\0', r.out);
		mu_assert_int_eq(cases[i].channels, device.channels_set);
		mu_assert_int_eq(cases[i].bdaddrs, device.bdaddrs_set);
		/* Not one block was taken either way: the channel request is
		 * sent as soon as the capture has been asked for, before the
		 * receive loop. */
		mu_assert_int_eq(0, device.blocks);

		if (cases[i].refuse_channel) {
			/* The channel is stated after the capture starts, so the
			 * mode is running when this one is refused: it is
			 * stopped, which gives the board back, and the capture
			 * never reaches its loop. */
			mu_assert_int_eq(1, device.rx_syms_calls);
			mu_assert_int_eq(1, device.bulk_inits);
			mu_assert_int_eq(1, device.thread_starts);
			mu_assert_int_eq(1, device.thread_stops);
			mu_assert_int_eq(1, device.stops);
			mu_assert_int_eq(0, device.closes);
		} else {
			/* The address goes before any of that, so nothing has
			 * started yet and no capture is set up on top of a
			 * refused address. */
			mu_assert_int_eq(0, device.rx_syms_calls);
			mu_assert_int_eq(0, device.bulk_inits);
			mu_assert_int_eq(0, device.cleanup_handlers);
			/* Given back, and without a STOP: no mode had started,
			 * and a STOP to a board that is not answering would add
			 * a second failure to the report. */
			mu_assert_int_eq(1, device.closes);
			mu_assert_int_eq(0, device.stops);
		}
	}

	/* A run that needs both requests sends the address, asks for the
	 * capture, and states the channel last. */
	reset_device();
	device.channel_rc = -9;
	r = RUN_RX("-l", "112233", "-u", "ab", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(device.calls, "bsc") == 0, device.calls);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, STALLED("set sweep mode")) == 0, r.err);
}

/* A board that took only part of the address is a failed request too, and one
 * libubertooth reports in full itself, counts and all. The capture used to
 * start on top of it and exit 0, because the result was positive rather than
 * negative. The tool adds no line of its own here. */
MU_TEST(a_truncated_address_request_ends_the_run_too)
{
	struct run_result r;

	reset_device();
	device.bdaddr_rc = UBERTOOTH_ERROR_SHORT_REPLY;
	r = RUN_RX("-l", "112233", "-u", "ab", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.bdaddrs_set);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(1, device.closes);
	mu_assert_int_eq(0, device.stops);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(r.err[0] == '\0', r.err);
}

/* Survey mode needs the sweep most of all, so a refused one keeps it from
 * printing a result list it never gathered. */
MU_TEST(a_refused_sweep_leaves_survey_mode_with_no_results)
{
	struct run_result r;

	reset_device();
	device.channel_rc = -9;
	r = RUN_RX("-z", "-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out); /* no "Survey Results" list either */
	mu_assert(strcmp(r.err, STALLED("set sweep mode")) == 0, r.err);
}

/* A firmware this host will not drive ends the command before any setup
 * request, with the board released whichever way the check failed. The tool
 * closes it either way: the library has already released a board whose
 * version it refused, and closing a board that is already released does
 * nothing. */
MU_TEST(a_firmware_the_tool_cannot_use_exits_one)
{
	struct run_result r;

	reset_device();
	device.api_rc = -1;
	device.api_released = 1;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(0, device.cleanup_handlers);
	mu_assert_int_eq(1, device.stops);
	mu_assert_int_eq(1, device.closes); /* harmless: it is already released */

	reset_device();
	device.api_rc = -1;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(0, device.cleanup_handlers);
	mu_assert_int_eq(0, device.stops);
	mu_assert_int_eq(1, device.closes); /* the board is not left claimed */
}

/* A capture that cannot be started at all exits 1 with one "error: " line on
 * standard error and nothing on standard output. Each of these used to hand
 * the failing call's own return value to the exit status, which a shell wraps
 * into a byte: a script read -1 as 255 and the libusb codes as 247 and 249. */
MU_TEST(a_capture_that_cannot_start_exits_one)
{
	char expected[128];
	struct run_result r;

	/* The receive transfer libusb would not take. */
	reset_device();
	device.bulk_init_rc = -1;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: submitting the receive transfer failed "
	          "(-4)\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	/* Nothing was started on top of it: no poll thread, no mode request, no
	 * channel, no block. The board is given back. */
	mu_assert_int_eq(0, device.thread_starts);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.blocks);
	mu_assert_int_eq(1, device.stops);

	/* The thread that collects the capture. pthread_create() reports a
	 * failure as a positive errno, which a "< 0" test let through: the
	 * capture then had nobody to complete its transfers and waited forever
	 * with nothing arriving. */
	reset_device();
	device.thread_start_rc = EAGAIN;
	r = RUN_RX("-t", "10");
	snprintf(expected, sizeof(expected),
	         "error: could not start the thread that collects the capture:"
	         " %s\n", strerror(EAGAIN));
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, expected) == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(1, device.thread_starts);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.blocks);
	mu_assert_int_eq(1, device.stops);

	/* The request that starts the mode, named the way every other refused
	 * setup request is. The poll thread is running by then, so it is joined
	 * on the way out. */
	reset_device();
	device.rx_syms_rc = LIBUSB_ERROR_TIMEOUT;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, NO_ANSWER("start the capture")) == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(1, device.rx_syms_calls);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.blocks);
	mu_assert_int_eq(1, device.thread_stops);
	mu_assert_int_eq(1, device.stops);

	/* A code with no advice: still the one "error: " line that names the
	 * request, never libubertooth's shared sink, which names none. */
	reset_device();
	device.rx_syms_rc = LIBUSB_ERROR_PIPE;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, STALLED("start the capture")) == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(1, device.rx_syms_calls);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.blocks);
	mu_assert_int_eq(1, device.thread_stops);
	mu_assert_int_eq(1, device.stops);

	/* A survey prints no result list for a capture that never started. */
	reset_device();
	device.bulk_init_rc = -1;
	r = RUN_RX("-z", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
}

/* A replay opens no device, so it sends no setup request at all and a board
 * that would refuse one cannot affect it. */
MU_TEST(a_replay_sends_no_setup_request)
{
	struct run_result r;

	reset_device();
	device.channel_rc = -9;
	device.bdaddr_rc = -9;
	r = RUN_RX("-i", "/dev/null", "-l", "112233", "-u", "ab");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.bdaddrs_set);
	mu_assert_int_eq(0, device.connect_calls);
	mu_assert(r.err[0] == '\0', r.err);
}

/* The handle and its 72 MB fifo go back on every exit once it exists. They
 * were left to process exit, and were allocated before the command line had
 * been read, so even a refused command took 72 MB. */
MU_TEST(every_exit_path_gives_the_handle_back)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-t", "30");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.frees);

	/* A firmware version the host will not drive, both ways round: the
	 * library releases a board whose version it refused, and closing a
	 * board already released does nothing. */
	reset_device();
	device.api_rc = -1;
	device.api_released = 1;
	r = RUN_RX("-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.frees);

	reset_device();
	device.api_rc = -1;
	r = RUN_RX("-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.frees);

	/* A capture that could not be set up. */
	reset_device();
	device.bulk_init_rc = -1;
	r = RUN_RX("-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.frees);

	/* No board matches -U: the handle exists by then and still goes back. */
	reset_device();
	device.connect_fails = 1;
	r = RUN_RX("-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.frees);

	/* A refused command line never allocates one. */
	reset_device();
	r = RUN_RX("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, device.frees);
}

int main(void)
{
	/* Nothing here should look for a board; make sure of it. */
	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	MU_RUN(a_capture_whose_stream_died_exits_one);
	MU_RUN(every_exit_path_gives_the_handle_back);
	MU_RUN(a_capture_on_standard_output_gets_the_stream_and_its_header);
	MU_RUN(a_capture_whose_teardown_was_refused_exits_one);
	MU_RUN(a_survey_whose_teardown_was_refused_still_prints_its_results);
	MU_RUN(a_run_refused_for_its_firmware_creates_no_file);
	MU_RUN(a_bounded_capture_still_exits_zero);
	MU_RUN(an_interrupted_capture_still_exits_zero);
	MU_RUN(a_survey_whose_stream_died_still_prints_its_results);
	MU_RUN(a_refused_command_line_opens_no_device);
	MU_RUN(a_capture_the_board_took_sets_up_and_exits_zero);
	MU_RUN(the_capture_is_asked_for_before_the_channel);
	MU_RUN(a_refused_setup_request_is_named_and_exits_one);
	MU_RUN(a_truncated_address_request_ends_the_run_too);
	MU_RUN(a_refused_sweep_leaves_survey_mode_with_no_results);
	MU_RUN(a_capture_that_cannot_start_exits_one);
	MU_RUN(a_firmware_the_tool_cannot_use_exits_one);
	MU_RUN(a_replay_sends_no_setup_request);
	return mu_report();
}
