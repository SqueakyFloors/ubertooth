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
	int channel_rc;      /* returned by cmd_set_channel() */
	int rx_syms_rc;      /* returned by cmd_rx_syms() */
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
	(void)ut;
	return fake.api_rc;
}

void ubertooth_stop(ubertooth_t* ut)
{
	ut->devh = NULL; /* as the library's cleanup leaves it */
	fake.stops++;
}

int cmd_set_channel(struct libusb_device_handle* devh, u16 channel)
{
	(void)devh;
	record('c');
	fake.last_channel = channel;
	return fake.channel_rc;
}

int cmd_rx_syms(struct libusb_device_handle* devh)
{
	(void)devh;
	record('s');
	return fake.rx_syms_rc;
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

/* A refused sweep is named, in one line on standard error, and ends the
 * command there: it used to be ignored, so the board stayed on whatever
 * channel it was left on and the tool printed an AFH map that meant nothing,
 * reporting success. */
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
	mu_assert_int_eq(1, fake.stops); /* the board is still released */
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, "error: could not set sweep mode\n") == 0, r.err);
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
	mu_assert(strcmp(r.err, "error: could not set sweep mode\n") == 0, r.err);
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

	reset_fake();
	fake.api_rc = -1;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
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
