/*
 * Hardware-independent tests of ubertooth-dump's main(): which exits open a
 * device, and whether the handle is given back on the ones that can give it
 * back. The firmware check is the interesting part, because its two outcomes
 * leave the device in different states.
 *
 * ubertooth-dump.c is compiled into this test with main() renamed, and the
 * libubertooth functions and globals it uses are replaced by the fakes below.
 * Nothing here reaches the capture loop.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_dump_main
#include "ubertooth-dump.c"
#undef main

/* The -V answer, which libubertooth gives in every tool: faked here, so this
 * test stays about the tool's own command-line handling. The answer's own
 * cases are in test_version_report.c. */
int ubertooth_print_versions(FILE *out, const char *selector)
{
	(void)selector;
	fprintf(out, "version (test)\n");
	return 0;
}

/* ---- scripted device -------------------------------------------------- */

static struct {
	int fail_start;  /* ubertooth_start_select() returns NULL */
	/* The stop a signal recorded while the board was being opened, which
	 * the tool reads before it asks anything of the board. */
	int stop_requested;
	int api_rc;      /* what ubertooth_check_api() returns */
	/* a refused version is closed inside ubertooth_check_api(); a version
	 * that could not be read is not */
	int api_version_was_read;
	int starts, closes, frees, api_checks, registrations;
	int library_closes; /* closes done inside the library, not by the tool */
	int dumps, stops;
	/* The modulation the tool left on the handle for the library to state
	 * once the capture had started, as the fake rx_dump() below found it.
	 * -1, as ubertooth_init() leaves it, means the tool asked for none. */
	int capture_modulation;
	int dump_rc;        /* what rx_dump() returns */
	int stream_failed;  /* what ubertooth_stream_failed() reports */
	/* 1 for the cases that are meant to run a whole dump; the fakes below
	 * refuse to be reached without it. */
	int reach_capture;
	char selector[64];
	/* 1 to have the teardown's STOP refused, so the board is left running
	 * the mode the capture started. */
	int stop_refused;
} fake;

static ubertooth_t fake_ut;

/* libubertooth owns this; the tool only writes to it. */
FILE* dumpfile = NULL;

ubertooth_t* ubertooth_start_select(const char* u_arg)
{
	fake.starts++;
	snprintf(fake.selector, sizeof(fake.selector), "%s", u_arg ? u_arg : "");
	if (fake.fail_start)
		return NULL;
	memset(&fake_ut, 0, sizeof(fake_ut));
	/* As the real one leaves it. Not a real handle: nothing here
	 * dereferences devh, it is passed through and tested for NULL. */
	fake_ut.devh = (struct libusb_device_handle*)&fake_ut;
	/* And as ubertooth_init() leaves this: no capture has stated a
	 * modulation yet, which zero would read as Basic Rate. */
	fake_ut.capture_modulation = -1;
	return &fake_ut;
}

/* The real one closes the device itself when it refuses the version it read,
 * and does not when it could not read a version at all. That difference is
 * what the tool's cleanup turns on, so the fake keeps it. */
int ubertooth_check_api(ubertooth_t* ut)
{
	mu_assert(ut == &fake_ut, "checks the handle it was given");
	fake.api_checks++;
	if (fake.api_rc < 0 && fake.api_version_was_read) {
		ut->devh = NULL; /* as ubertooth_stop() leaves it */
		fake.library_closes++;
	}
	return fake.api_rc;
}

void ubertooth_close(ubertooth_t* ut)
{
	mu_assert(ut == &fake_ut, "closes the handle it was given");
	fake.closes++;
	ut->devh = NULL; /* as the real one leaves it */
}

void ubertooth_free(ubertooth_t* ut)
{
	mu_assert(ut == &fake_ut, "frees the handle it was given");
	fake.frees++;
}

/* Installed before the board is opened, so every case here reaches it. */
void ubertooth_catch_stop_signals(void)
{
	fake.registrations++;
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

int rx_dump(ubertooth_t* ut, int bitstream)
{
	(void)bitstream;
	fake.dumps++;
	/* The real one states this after it has asked for the capture mode; all
	 * this fake does is record what the tool asked for. */
	fake.capture_modulation = ut->capture_modulation;
	mu_assert(fake.reach_capture, "no test should reach the capture");
	return fake.dump_rc;
}

/* The end-of-capture gap report, which the tool calls and no case here reads:
 * a dump whose records never carried FIFO_OVERFLOW has nothing to say. */
void ubertooth_report_capture_losses(ubertooth_t* ut)
{
	(void)ut;
}

int ubertooth_stream_failed(ubertooth_t* ut)
{
	(void)ut;
	mu_assert(fake.reach_capture, "no test should reach the end of a capture");
	return fake.stream_failed;
}

void ubertooth_stop(ubertooth_t* ut)
{
	(void)ut;
	fake.stops++;
	mu_assert(fake.reach_capture, "no test should reach the end of a capture");
}

/* Whether the teardown's STOP was refused, which leaves the board running the
 * mode the capture started. Scripted, because the fake board above always
 * takes it. */
int ubertooth_stop_failed(ubertooth_t* ut)
{
	(void)ut;
	return fake.stop_refused;
}

/* ---- running main() --------------------------------------------------- */

struct run {
	int rc;
	char out[4096];
	char err[4096];
};

static void run_main(struct run* result, int argc, char* argv[])
{
	FILE* out = tmpfile();
	FILE* err = tmpfile();
	int saved_out = out != NULL ? dup(fileno(stdout)) : -1;
	int saved_err = err != NULL ? dup(fileno(stderr)) : -1;
	size_t n;

	memset(result, 0, sizeof(*result));
	mu_assert_fatal(out != NULL && err != NULL && saved_out >= 0
	                && saved_err >= 0, "capture the streams");

	fflush(stdout);
	fflush(stderr);
	dup2(fileno(out), fileno(stdout));
	dup2(fileno(err), fileno(stderr));

	/* restart option parsing for each run */
	optind = 0;
	result->rc = ubertooth_dump_main(argc, argv);

	fflush(stdout);
	fflush(stderr);
	dup2(saved_out, fileno(stdout));
	dup2(saved_err, fileno(stderr));
	close(saved_out);
	close(saved_err);

	rewind(out);
	n = fread(result->out, 1, sizeof(result->out) - 1, out);
	result->out[n] = '\0';
	rewind(err);
	n = fread(result->err, 1, sizeof(result->err) - 1, err);
	result->err[n] = '\0';
	fclose(out);
	fclose(err);
}

static void reset(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.api_rc = -1; /* so no test runs into the capture loop */
	/* Not zero: zero is Basic Rate, so a run that asked for nothing would
	 * read as one that asked for Classic. */
	fake.capture_modulation = -1;
	dumpfile = NULL;
}

/* ---- tests ------------------------------------------------------------ */

/* A refused firmware version: the library has already closed the device. The
 * tool closes and frees the handle on both of the check's failure paths,
 * because closing a device that is already closed does nothing. */
/* A stop that arrived while the board was being opened ends the run before
 * anything is asked of the board and before the dump file is opened. */
MU_TEST(a_stop_while_the_board_was_opened_dumps_nothing)
{
	char* argv[] = { "ubertooth-dump", "-d", "/dev/null", NULL };
	struct run r;

	reset();
	fake.api_rc = 0;
	fake.stop_requested = 1;
	run_main(&r, 3, argv);

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.dumps);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
	mu_assert(dumpfile == NULL, "a stopped run opened no dump file");
}

MU_TEST(a_refused_firmware_version_is_closed_and_freed)
{
	char* argv[] = { "ubertooth-dump", NULL };
	struct run r;

	reset();
	fake.api_version_was_read = 1;
	run_main(&r, 1, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.api_checks);
	mu_assert_int_eq(1, fake.library_closes);
	mu_assert_int_eq(1, fake.closes); /* harmless: already released */
	mu_assert_int_eq(1, fake.frees);
	/* The stop handlers go in before the board is opened, so a run that
	 * ends on the firmware check has them. */
	mu_assert_int_eq(1, fake.registrations);
}

/* A version that could not be read at all: the library left the device open,
 * so this is the path where the tool closes it. */
MU_TEST(a_version_that_cannot_be_read_closes_and_frees)
{
	char* argv[] = { "ubertooth-dump", NULL };
	struct run r;

	reset();
	fake.api_version_was_read = 0;
	run_main(&r, 1, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.library_closes);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
}

/* Nothing was opened, so there is nothing to release or free. */
MU_TEST(a_device_that_cannot_be_opened_frees_nothing)
{
	char* argv[] = { "ubertooth-dump", NULL };
	struct run r;

	reset();
	fake.fail_start = 1;
	run_main(&r, 1, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.starts);
	mu_assert_int_eq(0, fake.api_checks);
	mu_assert_int_eq(0, fake.closes);
	mu_assert_int_eq(0, fake.frees);
}

/* Every refusal is settled on the command line, before a device is opened,
 * and -h is the one that is not a refusal. */
MU_TEST(the_refusals_never_open_a_device)
{
	struct run r;
	char* bad_option[] = { "ubertooth-dump", "-Z", NULL };
	char* device_without_value[] = { "ubertooth-dump", "-U", NULL };
	char* file_without_value[] = { "ubertooth-dump", "-d", NULL };
	char* stray_word[] = { "ubertooth-dump", "spare", NULL };
	char* help[] = { "ubertooth-dump", "-h", NULL };

	reset();
	run_main(&r, 2, bad_option);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: ") != NULL, r.err);
	run_main(&r, 2, device_without_value);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 2, file_without_value);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 2, stray_word);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: ") != NULL, r.err);

	run_main(&r, 2, help);
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Usage:") != NULL, r.out);
	mu_assert(r.err[0] == '\0', r.err);

	mu_assert_int_eq(0, fake.starts);
	mu_assert_int_eq(0, fake.frees);
}

/* The -d file is named on the command line but only opened once every check
 * has passed, so a refused command line creates nothing. */
/* A dump the board would not stop leaves that board running the capture mode,
 * and no other tool can open it until it is reset or re-plugged, so the run
 * did not do what was asked. What was dumped stays written out, and
 * libubertooth has already named the refused request. */
MU_TEST(a_dump_whose_teardown_was_refused_exits_one)
{
	char* argv[] = { "ubertooth-dump", NULL };
	struct run r;

	reset();
	fake.api_rc = 0;
	fake.reach_capture = 1;
	fake.stop_refused = 1;
	run_main(&r, 1, argv);
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.dumps);
	mu_assert_int_eq(1, fake.stops);
	mu_assert_int_eq(1, fake.frees);

	/* The same dump on a board that takes the stop. */
	reset();
	fake.api_rc = 0;
	fake.reach_capture = 1;
	run_main(&r, 1, argv);
	mu_assert_int_eq(0, r.rc);
}

/* A run refused for its firmware creates no -d file: an empty one left behind
 * is a file the user then has to tell apart from a dump that caught nothing. */
MU_TEST(a_run_refused_for_its_firmware_opens_no_dump_file)
{
	char path[64];
	char* argv[] = { "ubertooth-dump", "-d", path, NULL };
	struct run r;
	struct stat info;
	int fd;

	reset();
	fake.api_rc = -1;
	fake.api_version_was_read = 1;
	snprintf(path, sizeof(path), "/tmp/dump-main-XXXXXX");
	fd = mkstemp(path);
	mu_assert_fatal(fd >= 0, "temp name taken");
	close(fd);
	unlink(path);

	run_main(&r, 3, argv);
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.api_checks);
	mu_assert(stat(path, &info) != 0, path);
	mu_assert(dumpfile == NULL, "no dump file was opened");
	unlink(path);
}

MU_TEST(a_refused_command_line_opens_no_dump_file)
{
	char* argv[] = { "ubertooth-dump", "-d", "/dev/null", "spare", NULL };
	struct run r;

	reset();
	run_main(&r, 4, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert(dumpfile == NULL, "no dump file was opened");
	mu_assert_int_eq(0, fake.starts);
}

/* A dump the board would not start exits 1 and writes nothing to standard
 * output. The modulation is one of the requests that can fail there -- the
 * library states it right after the capture request -- and libubertooth has
 * already said what failed, so the tool adds no line of its own.
 *
 * The -d file was opened first, so it is there and empty. The two assertions
 * about it are what pin the order, so the path is removed before the run: a
 * file left there by the test would satisfy them whichever way round the two
 * steps were. */
MU_TEST(a_dump_the_board_refused_exits_one)
{
	char path[64];
	char* argv[] = { "ubertooth-dump", "-l", "-d", path, NULL };
	struct run r;
	struct stat info;
	int fd;

	reset();
	fake.api_rc = 0;
	fake.reach_capture = 1;
	fake.dump_rc = -1;
	snprintf(path, sizeof(path), "/tmp/dump-main-XXXXXX");
	fd = mkstemp(path);
	mu_assert_fatal(fd >= 0, "temp name taken");
	close(fd);
	unlink(path);

	run_main(&r, 4, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.dumps);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(1, fake.frees);
	mu_assert(dumpfile != NULL, "the -d file was opened before the dump");
	mu_assert(stat(path, &info) == 0 && info.st_size == 0,
	          "the -d file is there and empty");
	fclose(dumpfile);
	dumpfile = NULL;
	unlink(path);
}

/* The other half of that order: a -d the tool cannot open ends the run before
 * anything is asked of the board, so the board is left as it was found. The
 * firmware holds a modulation until a mode ends, and ubertooth-follow and -afh
 * never state their own, so one set here for a dump that never ran would have
 * reached the next of those. */
MU_TEST(a_dump_file_that_cannot_be_opened_starts_no_dump)
{
	char* argv[] = { "ubertooth-dump", "-l", "-d",
	                 "/no/such/dir/dump.bin", NULL };
	struct run r;

	reset();
	fake.api_rc = 0;
	run_main(&r, 4, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.dumps);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
	mu_assert(dumpfile == NULL, "no dump file was opened");
}

/* Which modulation each option asks for. The tool leaves it on the handle and
 * the library states it after the capture request, because a mode start puts
 * the board's modulation back to Basic Rate. -c is the default, so a command
 * line with neither asks for the same one. */
MU_TEST(the_modulation_options_reach_the_board)
{
	char* classic[] = { "ubertooth-dump", "-c", NULL };
	char* low_energy[] = { "ubertooth-dump", "-l", NULL };
	char* neither[] = { "ubertooth-dump", NULL };
	struct run r;

	reset();
	fake.api_rc = 0;
	fake.reach_capture = 1;
	run_main(&r, 2, classic);
	mu_assert_int_eq(0, r.rc);
	/* MOD_BT_BASIC_RATE is 0 and the handle starts at -1, so this says the
	 * tool asked for Classic rather than for nothing. */
	mu_assert_int_eq(MOD_BT_BASIC_RATE, fake.capture_modulation);

	reset();
	fake.api_rc = 0;
	fake.reach_capture = 1;
	run_main(&r, 2, low_energy);
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(MOD_BT_LOW_ENERGY, fake.capture_modulation);

	reset();
	fake.api_rc = 0;
	fake.reach_capture = 1;
	run_main(&r, 1, neither);
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(MOD_BT_BASIC_RATE, fake.capture_modulation);
}

/* The -U value reaches the library as the user wrote it. */
MU_TEST(the_device_selector_is_passed_through)
{
	char* with_u[] = { "ubertooth-dump", "-U", "0b70", NULL };
	char* without_u[] = { "ubertooth-dump", NULL };
	struct run r;

	reset();
	run_main(&r, 3, with_u);
	mu_assert(strcmp(fake.selector, "0b70") == 0, fake.selector);

	reset();
	run_main(&r, 1, without_u);
	mu_assert(strcmp(fake.selector, "") == 0, fake.selector);
}

int main(void)
{
	/* This binary does not link libubertooth: the fakes above are the
	 * whole device, so any library call the tool grows becomes a link
	 * error rather than a silently real one. */
	MU_RUN(a_stop_while_the_board_was_opened_dumps_nothing);
	MU_RUN(a_refused_firmware_version_is_closed_and_freed);
	MU_RUN(a_version_that_cannot_be_read_closes_and_frees);
	MU_RUN(a_device_that_cannot_be_opened_frees_nothing);
	MU_RUN(the_refusals_never_open_a_device);
	MU_RUN(a_dump_whose_teardown_was_refused_exits_one);
	MU_RUN(a_run_refused_for_its_firmware_opens_no_dump_file);
	MU_RUN(a_refused_command_line_opens_no_dump_file);
	MU_RUN(a_dump_the_board_refused_exits_one);
	MU_RUN(a_dump_file_that_cannot_be_opened_starts_no_dump);
	MU_RUN(the_modulation_options_reach_the_board);
	MU_RUN(the_device_selector_is_passed_through);
	return mu_report();
}
