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
 * BlueZ itself is only faked as far as finding and opening a local adapter,
 * which every run does before it reaches the Ubertooth: no case lets the tool
 * reach an inquiry, so no case needs a Bluetooth adapter on the machine
 * running the tests.
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
	return 0;
}

/* A real descriptor, so the tool's close() is a real close and giving the
 * handle back can be checked, but one that is not a Bluetooth socket. */
int hci_open_dev(int dev_id)
{
	(void)dev_id;
	fake.dev_fd = open("/dev/null", O_RDONLY);
	return fake.dev_fd;
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

/* No case here refuses this request: what the tool does with a refused one is
 * its exit status, which has its own fix. */
int cmd_rx_syms(struct libusb_device_handle* devh)
{
	(void)devh;
	record('s');
	return 0;
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
	return 0;
}

int ubertooth_bulk_thread_start()
{
	return 0;
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

/* The scan the board took: one setup request, carrying a channel above the
 * band so the board sweeps, then the capture, then the results. Nothing goes
 * to standard error. */
MU_TEST(a_scan_the_board_took_sweeps_and_exits_0)
{
	struct run_result r;

	reset_fake();
	r = RUN("-t", "5");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "cs") == 0, fake.calls);
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
	mu_assert(strcmp(fake.calls, "c") == 0, fake.calls);
	mu_assert_int_eq(0, fake.receives);
	mu_assert_int_eq(0, fake.registrations);
	/* Given back, and without a STOP: no mode had started, and a STOP to a
	 * board that is not answering would add a second failure. */
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(0, fake.stops);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
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

/* The sweep is set before either scan starts, so a refused sweep also leaves
 * the BlueZ inquiry of -s unrun: the tool reports nothing it did not gather. */
MU_TEST(a_refused_sweep_leaves_the_bluez_scan_unrun)
{
	struct run_result r;

	reset_fake();
	fake.channel_rc = -9;
	r = RUN("-s", "-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.out, "HCI scan") == NULL, r.out);
	mu_assert(strcmp(r.err, STALLED) == 0, r.err);
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
	MU_RUN(a_refused_sweep_leaves_the_bluez_scan_unrun);
	MU_RUN(a_board_or_firmware_the_tool_cannot_use_exits_1);
	MU_RUN(help_exits_0_on_standard_output);
	return mu_report();
}
