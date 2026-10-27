/*
 * Hardware-independent tests of ubertooth-scan's main(): the sweep the scan
 * needs, and what the tool reports and exits with when the board refuses it.
 *
 * ubertooth-scan.c is compiled into this test with main() renamed, and the
 * libubertooth functions it calls are replaced by the fakes below, so the real
 * command-line handling and reporting run against a scripted device. The
 * faked receive ends the capture loop on its first turn, so no case waits on a
 * board.
 *
 * BlueZ itself is faked as far as finding and opening a local adapter, which
 * every run does before it reaches the Ubertooth, and the inquiry -s asks for,
 * which finds nothing: no case needs a Bluetooth adapter on the machine
 * running the tests. Both adapter steps can be made to fail, so what the tool
 * says and which stream it says it on are pinned here too.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_scan_main
#include "ubertooth-scan.c"
#undef main

/* ---- scripted device ---------------------------------------------------- */

static struct {
	int start_fails;     /* no board answers the -U selection */
	int api_rc;          /* returned by ubertooth_check_api() */
	int api_released;    /* whether that call released the board itself */
	int closes;          /* ubertooth_close() */
	int channel_rc;      /* returned by cmd_set_channel_quiet() */
	int last_channel;    /* the channel the capture was set to, in MHz */
	int timeout;         /* seconds passed to ubertooth_set_timeout() */
	int stops;
	int registrations;   /* register_cleanup_handler() */
	int receives;        /* turns of the capture loop */
	int inquiries;       /* BlueZ inquiries the run ran */
	/* Returned by the three calls that set the capture up, so that each
	 * failure path can be run: the transfer, the poll thread, the request
	 * that starts the capture. */
	int bulk_init_rc;
	int thread_start_rc;
	int rx_syms_rc;
	/* The two BlueZ failures: no adapter of that name, and one that is
	 * there but will not open. Each is the errno the call leaves behind. */
	int devid_fails;
	int devid_errno;     /* 0 for a failure that left no errno behind */
	int open_fails;
	char adapter[16];    /* the adapter name -b asked for */
	int dev_fd;          /* the adapter handle the tool was given */
	int dev_fd_closed;   /* whether the run gave it back */
	/* Every device call in order, one letter each: c set the channel,
	 * s start sending symbols. */
	char calls[16];
} fake;

static ubertooth_t fake_ut;
static int fake_devh;

/* A libubertooth global the tool reads; it lives in ubertooth.c, which is not
 * linked here. */
int max_ac_errors = 2;

static void record(char call)
{
	size_t length = strlen(fake.calls);

	if (length + 1 < sizeof(fake.calls)) {
		fake.calls[length] = call;
		fake.calls[length + 1] = '\0';
	}
}

static void reset_fake(void)
{
	memset(&fake, 0, sizeof(fake));
	memset(&fake_ut, 0, sizeof(fake_ut));
	fake_ut.devh = (struct libusb_device_handle*)&fake_devh;
	fake.dev_fd = -1;
}

/* ---- faked BlueZ adapter ------------------------------------------------ */

int hci_devid(const char* str)
{
	snprintf(fake.adapter, sizeof(fake.adapter), "%s", str ? str : "");
	if (fake.devid_fails) {
		/* The tool clears errno before the call, so 0 here is a failure
		 * that set none -- which BlueZ does not promise to do. */
		errno = fake.devid_errno;
		return -1;
	}
	return 0;
}

/* A real descriptor, so the tool's close() is a real close and giving the
 * handle back can be checked, but one that is not a Bluetooth socket. */
int hci_open_dev(int dev_id)
{
	(void)dev_id;
	if (fake.open_fails) {
		errno = EPERM;
		return -1;
	}
	fake.dev_fd = open("/dev/null", O_RDONLY);
	return fake.dev_fd;
}

/* The inquiry -s runs, faked so that no case needs a Bluetooth adapter: it
 * finds nothing, which is all this file asks of it. */
int hci_inquiry(int dev_id, int len, int num_rsp, const uint8_t* lap,
                inquiry_info** ii, long flags)
{
	(void)dev_id;
	(void)len;
	(void)num_rsp;
	(void)lap;
	(void)ii;
	(void)flags;
	fake.inquiries++;
	return 0;
}

/* ---- faked Ubertooth ---------------------------------------------------- */

ubertooth_t* ubertooth_start_select(const char* u_arg)
{
	(void)u_arg;
	if (fake.start_fails)
		return NULL; /* the library has reported it already */
	return &fake_ut;
}

int ubertooth_check_api(ubertooth_t* ut)
{
	/* The library releases the board itself when the firmware reports a
	 * version this host will not drive; a version it could not read at all
	 * leaves the board open for the caller to close. */
	if (fake.api_rc < 0 && fake.api_released) {
		ut->devh = NULL;
		fake.stops++;
	}
	return fake.api_rc;
}

void ubertooth_close(ubertooth_t* ut)
{
	ut->devh = NULL;
	fake.closes++;
}

void ubertooth_stop(ubertooth_t* ut)
{
	ut->devh = NULL; /* as the library's cleanup leaves it */
	fake.stops++;
}

int cmd_set_channel_quiet(struct libusb_device_handle* devh, u16 channel)
{
	(void)devh;
	record('c');
	fake.last_channel = channel;
	return fake.channel_rc;
}

/* The quiet form: it prints nothing, so what the cases below see on standard
 * error is the tool's own report. */
int cmd_rx_syms_quiet(struct libusb_device_handle* devh)
{
	(void)devh;
	record('s');
	return fake.rx_syms_rc;
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
	fake.registrations++;
}

void ubertooth_set_timeout(ubertooth_t* ut, int seconds)
{
	(void)ut;
	fake.timeout = seconds;
}

int ubertooth_bulk_init(ubertooth_t* ut)
{
	(void)ut;
	if (fake.bulk_init_rc < 0)
		/* As the library reports a transfer libusb would not take: the
		 * libusb code in the line, -1 to the caller. */
		fprintf(stderr, "error: submitting the receive transfer failed "
		        "(%d)\n", LIBUSB_ERROR_NO_DEVICE);
	return fake.bulk_init_rc;
}

/* pthread_create() returns an errno, which is positive, and the tool has to
 * read it that way: the fake hands back whatever the case asked for. */
int ubertooth_bulk_thread_start()
{
	return fake.thread_start_rc;
}

void ubertooth_bulk_thread_stop()
{
}

/* Ends the capture loop on its first turn: decoding is covered by the
 * callback's own tests, and nothing here should wait on a board. */
int ubertooth_bulk_receive(ubertooth_t* ut, rx_callback cb, void* cb_args)
{
	(void)cb;
	(void)cb_args;
	fake.receives++;
	ut->stop_ubertooth = 1;
	return 0;
}

void cb_scan(ubertooth_t* ut, void* args)
{
	(void)ut;
	(void)args;
}

/* No case here fails a stream: that path has its own tests. */
int ubertooth_stream_failed(ubertooth_t* ut)
{
	(void)ut;
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
	/* A capture that did not fit must fail loudly instead of comparing a
	 * prefix. */
	mu_assert(fgetc(file) == EOF, "captured output was truncated");
}

static struct run_result run_scan(const char* const* args)
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
	if (out == NULL || err == NULL)
		return result;
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
	result.rc = ubertooth_scan_main(argc, argv);
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

#define RUN(...) run_scan((const char* const[]){ "ubertooth-scan", __VA_ARGS__, NULL })

/* ---- tests -------------------------------------------------------------- */

/* The scan the board took: the capture is asked for, then the sweep is stated
 * with a channel above the band, then the results. Nothing goes to standard
 * error.
 *
 * The order is the point. Stating the sweep first lost it whenever a mode was
 * already running, because a mode start puts the board's hop mode back to "no
 * hopping" -- so a scan after a killed tool listened to one channel and
 * printed an AFH map that means nothing. */
MU_TEST(a_scan_the_board_took_sweeps_and_exits_0)
{
	struct run_result r;

	reset_fake();
	r = RUN("-t", "5");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "sc") == 0, fake.calls);
	mu_assert_int_eq(9999, fake.last_channel);
	mu_assert_int_eq(5, fake.timeout);
	mu_assert_int_eq(1, fake.registrations);
	mu_assert_int_eq(1, fake.receives);
	mu_assert_int_eq(1, fake.stops);
	mu_assert(strcmp(fake.adapter, "hci0") == 0, fake.adapter);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
	mu_assert(strstr(r.out, "Ubertooth scan") != NULL, r.out);
	mu_assert(strstr(r.out, "Scan results:") != NULL, r.out);
	mu_assert(r.err[0] == '\0', r.err);
}

/* The whole report for a refused sweep: one error line naming the request,
 * why it failed and the libusb code, and a note only where there is something
 * to try. */
#define STALLED \
	"error: could not set sweep mode" \
	": the board does not support this command (stalled) (libusb -9)\n"
#define NO_ANSWER \
	"error: could not set sweep mode" \
	": the board did not answer in time (libusb -7)\n" \
	"note: if a mode is running, stop it with ubertooth-util -S; " \
	"if the board does not answer at all, re-plug it\n"

/* A refused sweep is named, with the reason and what to try, on standard
 * error, and ends the command there: it used to be ignored, so the board
 * stayed on whatever channel it was left on and the tool printed an AFH map
 * that meant nothing, reporting success. */
MU_TEST(a_refused_sweep_is_named_and_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.channel_rc = -9; /* LIBUSB_ERROR_PIPE, how a board refuses one */
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "sc") == 0, fake.calls);
	/* The capture had started, so it is stopped, which gives the board
	 * back; the loop that would have gathered the results is never
	 * reached. */
	mu_assert_int_eq(0, fake.receives);
	mu_assert_int_eq(1, fake.stops);
	mu_assert_int_eq(0, fake.closes);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
	/* Standard output stays empty: the heading the Ubertooth results go
	 * under is printed once the board is sweeping, so a run that gathers no
	 * results does not leave a heading over nothing. */
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, STALLED) == 0, r.err);

	/* A board that took the request and said nothing is the case the note
	 * is about. */
	reset_fake();
	fake.channel_rc = -7; /* LIBUSB_ERROR_TIMEOUT */
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.receives);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, NO_ANSWER) == 0, r.err);

	/* A code with no plain words still names the request and the number. */
	reset_fake();
	fake.channel_rc = -5;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err,
	          "error: could not set sweep mode (libusb -5)\n") == 0, r.err);
}

/* The sweep is now stated after the capture has been asked for, which is after
 * the BlueZ inquiry of -s has run. A refused sweep therefore ends a scan that
 * has already printed what BlueZ found: that half needs no Ubertooth, and what
 * it reported is what it saw. The Ubertooth half still gathers nothing, and
 * does not print its heading. */
MU_TEST(a_refused_sweep_keeps_the_bluez_results)
{
	struct run_result r;

	reset_fake();
	fake.channel_rc = -9;
	r = RUN("-s", "-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.inquiries);
	mu_assert(strstr(r.out, "HCI scan") != NULL, r.out);
	mu_assert(strstr(r.out, "Ubertooth scan") == NULL, r.out);
	mu_assert(strstr(r.out, "Scan results:") == NULL, r.out);
	mu_assert_int_eq(0, fake.receives);
	mu_assert(strcmp(r.err, STALLED) == 0, r.err);
}

/* Each of the three steps that set the capture up can fail. Each exits 1 with
 * one "error: " line on standard error and nothing on standard output, and
 * each gives back both the board and the BlueZ adapter handle this tool holds
 * alongside it. The handle is the one easily forgotten: the scan opens it
 * before it touches the Ubertooth at all.
 *
 * The status is the point of the three cases together. Each used to hand the
 * failing call's own return value to the exit status, which a shell wraps into
 * a byte, so a script read -1 as 255 and the libusb codes as 247 and 249.
 *
 * The poll thread's case asks for a positive code, because that is what
 * pthread_create() returns: a "< 0" test let a failure through, and the
 * capture then had nobody to complete its transfers and waited forever with
 * nothing arriving. */
MU_TEST(a_capture_that_cannot_be_set_up_exits_1_and_gives_everything_back)
{
	char thread_failed[128];
	struct {
		const char* calls;   /* the requests made before the failure */
		int bulk_init_rc;
		int thread_start_rc;
		int rx_syms_rc;
		const char* message;
	} cases[] = {
		{ "",  -1, 0, 0,
		  "error: submitting the receive transfer failed (-4)\n" },
		{ "",  0, EAGAIN, 0, NULL /* filled in below */ },
		/* The capture request is named the way the sweep request is, so
		 * the line carries the "error: " prefix rather than
		 * libubertooth's own unprefixed "libUSB Error" wording. */
		{ "s", 0, 0, LIBUSB_ERROR_TIMEOUT,
		  "error: could not start the capture: the board did not answer "
		  "in time (libusb -7)\n"
		  "note: if a mode is running, stop it with ubertooth-util -S; "
		  "if the board does not answer at all, re-plug it\n" },
		{ "s", 0, 0, LIBUSB_ERROR_PIPE,
		  "error: could not start the capture: the board does not "
		  "support this command (stalled) (libusb -9)\n" },
	};
	struct run_result r;
	size_t i;

	snprintf(thread_failed, sizeof(thread_failed),
	         "error: could not start the thread that collects the capture:"
	         " %s\n", strerror(EAGAIN));
	cases[1].message = thread_failed;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reset_fake();
		fake.bulk_init_rc = cases[i].bulk_init_rc;
		fake.thread_start_rc = cases[i].thread_start_rc;
		fake.rx_syms_rc = cases[i].rx_syms_rc;
		r = RUN("-t", "5");
		mu_assert_int_eq(1, r.rc);
		mu_assert(strcmp(fake.calls, cases[i].calls) == 0, fake.calls);
		/* No sweep was stated, and no results were gathered. */
		mu_assert_int_eq(0, fake.receives);
		/* The board goes back with a STOP, since a mode may be
		 * running by the time any of these fail. */
		mu_assert_int_eq(1, fake.stops);
		mu_assert_int_eq(0, fake.closes);
		mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
		mu_assert(r.out[0] == '\0', r.out);
		mu_assert(strcmp(r.err, cases[i].message) == 0, r.err);
	}
}

/* The two BlueZ failures, each one "error: " line on standard error. The
 * first used to go to standard output, where a caller redirecting the scan
 * results to a file collected it with them and saw nothing on the terminal;
 * the second was a perror() line with no "error: " prefix. */
MU_TEST(a_bluez_failure_is_reported_on_standard_error)
{
	char expected[128];
	struct run_result r;

	/* No adapter of that name. */
	reset_fake();
	fake.devid_fails = 1;
	fake.devid_errno = ENODEV;
	r = RUN("-b", "hci7");
	snprintf(expected, sizeof(expected),
	         "error: Bluetooth device hci7 not found: %s\n",
	         strerror(ENODEV));
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, expected) == 0, r.err);
	/* Nothing was opened: the adapter is found before the board. */
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(0, fake.stops);

	/* The same lookup failing without leaving an errno behind says what it
	 * knows rather than printing "Undefined error: 0" as the reason. */
	reset_fake();
	fake.devid_fails = 1;
	r = RUN("-b", "hci7");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err,
	          "error: Bluetooth device hci7 not found\n") == 0, r.err);

	/* An adapter that is there but will not open. */
	reset_fake();
	fake.open_fails = 1;
	r = RUN("-t", "5");
	snprintf(expected, sizeof(expected),
	         "error: cannot open Bluetooth device hci0: %s\n",
	         strerror(EPERM));
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, expected) == 0, r.err);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(0, fake.stops);
}

/* A board that cannot be opened, and a firmware the host will not drive, each
 * end the command before the sweep request. */
MU_TEST(a_board_or_firmware_the_tool_cannot_use_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.start_fails = 1;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	/* A version mismatch: the library has released the board, and releasing
	 * it twice is not safe. */
	reset_fake();
	fake.api_rc = -1;
	fake.api_released = 1;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
	mu_assert_int_eq(0, fake.closes);

	/* A version that could not be read at all leaves the board open, so the
	 * tool closes it rather than leaving it claimed. */
	reset_fake();
	fake.api_rc = -1;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
	mu_assert_int_eq(1, fake.closes);
}

/* -h is what the user asked for, so it goes to standard output and exits 0,
 * without opening anything. */
MU_TEST(help_exits_0_on_standard_output)
{
	struct run_result r;

	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Usage:") != NULL, r.out);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert(fake.calls[0] == '\0', fake.calls);
}

int main(void)
{
	/* This binary does not link libubertooth: the fakes above are the whole
	 * device, so any library call the tool grows becomes a link error
	 * rather than a silently real one. */
	MU_RUN(a_scan_the_board_took_sweeps_and_exits_0);
	MU_RUN(a_refused_sweep_is_named_and_exits_1);
	MU_RUN(a_refused_sweep_keeps_the_bluez_results);
	MU_RUN(a_capture_that_cannot_be_set_up_exits_1_and_gives_everything_back);
	MU_RUN(a_bluez_failure_is_reported_on_standard_error);
	MU_RUN(a_board_or_firmware_the_tool_cannot_use_exits_1);
	MU_RUN(help_exits_0_on_standard_output);
	return mu_report();
}
