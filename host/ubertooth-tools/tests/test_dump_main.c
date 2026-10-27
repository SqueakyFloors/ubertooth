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
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_dump_main
#include "ubertooth-dump.c"
#undef main

/* ---- scripted device -------------------------------------------------- */

static struct {
	int fail_start;  /* ubertooth_start_select() returns NULL */
	int api_rc;      /* what ubertooth_check_api() returns */
	/* a refused version is closed inside ubertooth_check_api(); a version
	 * that could not be read is not */
	int api_version_was_read;
	int starts, closes, frees, api_checks, registrations, modulations;
	int library_closes; /* closes done inside the library, not by the tool */
	char selector[64];
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

/* Reached only once the capture is about to start, which no test here does.
 * They exist so the test links, and fail loudly if a test gets that far. */
void register_cleanup_handler(ubertooth_t* ut, int do_exit)
{
	(void)ut;
	(void)do_exit;
	fake.registrations++;
	mu_assert(0, "no test should reach the capture");
}

int cmd_set_modulation(struct libusb_device_handle* devh, uint16_t mod)
{
	(void)devh;
	(void)mod;
	fake.modulations++;
	mu_assert(0, "no test should reach the capture");
	return -1;
}

int rx_dump(ubertooth_t* ut, int bitstream)
{
	(void)ut;
	(void)bitstream;
	mu_assert(0, "no test should reach the capture");
	return -1;
}

int ubertooth_stream_failed(ubertooth_t* ut)
{
	(void)ut;
	mu_assert(0, "no test should reach the end of a capture");
	return 0;
}

void ubertooth_stop(ubertooth_t* ut)
{
	(void)ut;
	mu_assert(0, "no test should reach the end of a capture");
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
	dumpfile = NULL;
}

/* ---- tests ------------------------------------------------------------ */

/* A refused firmware version: the library has already closed the device. The
 * tool closes and frees the handle on both of the check's failure paths,
 * because closing a device that is already closed does nothing. */
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
	mu_assert_int_eq(0, fake.registrations);
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
	MU_RUN(a_refused_firmware_version_is_closed_and_freed);
	MU_RUN(a_version_that_cannot_be_read_closes_and_frees);
	MU_RUN(a_device_that_cannot_be_opened_frees_nothing);
	MU_RUN(the_refusals_never_open_a_device);
	MU_RUN(a_refused_command_line_opens_no_dump_file);
	MU_RUN(the_device_selector_is_passed_through);
	return mu_report();
}
