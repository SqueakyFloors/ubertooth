/*
 * Hardware-independent tests of ubertooth-debug's main(): which exits open a
 * device, and whether the handle it was given is released again.
 *
 * ubertooth-debug.c is compiled into this test with main() renamed, and the
 * libubertooth functions it calls are replaced by the fakes below, so the real
 * command-line handling runs against a scripted "device".
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

/* The parsed register list is plain malloc and free, so the fakes below
 * cannot see it. Counting the tool's own free() calls can: <stdlib.h> is
 * already included above, so this macro reaches only ubertooth-debug.c's
 * calls and not the declaration. Freeing NULL is not counted, so the count
 * is "lists given back". */
static int lists_freed;
static void count_free(void* p)
{
	if (p != NULL)
		lists_freed++;
	free(p);
}

#define main ubertooth_debug_main
#define free count_free
#include "ubertooth-debug.c"
#undef free
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
	int fail_start;   /* ubertooth_start_select() returns NULL */
	int read_rc;      /* what cmd_read_register() returns */
	int starts, closes, frees, reads;
	char selector[64];
} fake;

static ubertooth_t fake_ut;

ubertooth_t* ubertooth_start_select(const char* u_arg)
{
	fake.starts++;
	snprintf(fake.selector, sizeof(fake.selector), "%s", u_arg ? u_arg : "");
	if (fake.fail_start)
		return NULL;
	memset(&fake_ut, 0, sizeof(fake_ut));
	/* Not a real handle: nothing here dereferences devh, it is only passed
	 * through to the faked register read as a context. */
	fake_ut.devh = (struct libusb_device_handle*)&fake_ut;
	return &fake_ut;
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

int cmd_read_register(struct libusb_device_handle* devh, uint8_t reg)
{
	(void)devh;
	(void)reg;
	fake.reads++;
	return fake.read_rc;
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
	result->rc = ubertooth_debug_main(argc, argv);

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
	fake.read_rc = 0x42;
	lists_freed = 0;
}

/* ---- tests ------------------------------------------------------------ */

/* The handle used to be left outstanding on this path: the registers were
 * read and main() returned without giving it back. */
MU_TEST(a_completed_read_releases_the_handle)
{
	char* argv[] = { "ubertooth-debug", "-r", "0x02", NULL };
	struct run r;

	reset();
	run_main(&r, 3, argv);

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.starts);
	mu_assert_int_eq(1, fake.reads);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
}

/* A failed read is still a released handle: the exit status changes, not the
 * cleanup. */
MU_TEST(a_failed_read_releases_the_handle_too)
{
	char* argv[] = { "ubertooth-debug", "-r", "0x02", NULL };
	struct run r;

	reset();
	fake.read_rc = -1;
	run_main(&r, 3, argv);

	/* The documented statuses are 0 and 1, so pin the 1. */
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
	mu_assert(r.err[0] != '\0', "the failure is reported on stderr");
}

/* Nothing was opened, so there is nothing to release and nothing to free. */
MU_TEST(a_device_that_cannot_be_opened_frees_nothing)
{
	char* argv[] = { "ubertooth-debug", "-r", "0x02", NULL };
	struct run r;

	reset();
	fake.fail_start = 1;
	run_main(&r, 3, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.starts);
	mu_assert_int_eq(0, fake.reads);
	mu_assert_int_eq(0, fake.closes);
	mu_assert_int_eq(0, fake.frees);
}

/* Every refusal is settled on the command line, before a device is opened. */
MU_TEST(the_refusals_never_open_a_device)
{
	struct run r;
	char* bad_option[] = { "ubertooth-debug", "-Z", NULL };
	char* bad_register[] = { "ubertooth-debug", "-r", "0x1ff", NULL };
	char* stray_word[] = { "ubertooth-debug", "-r", "0x02", "spare", NULL };
	char* no_register[] = { "ubertooth-debug", NULL };
	char* help[] = { "ubertooth-debug", "-h", NULL };
	char* bad_verbosity_after_a_list[] = { "ubertooth-debug", "-r", "0x02",
	                                       "-v", "9", NULL };
	char* unknown_option_after_a_list[] = { "ubertooth-debug", "-r", "0x02",
	                                        "-Z", NULL };
	char* help_after_a_list[] = { "ubertooth-debug", "-r", "0x02", "-h", NULL };

	reset();
	/* An unknown option is getopt's own message, which this tool has not
	 * been converted to report itself yet. No usage text follows it, on
	 * either stream. */
	run_main(&r, 2, bad_option);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, "Usage:") == NULL, r.out);
	mu_assert(strstr(r.err, "Usage:") == NULL, r.err);

	/* A refused option value is reported the way the rest of the tools do
	 * it: the reason on standard error, nothing on standard output. */
	run_main(&r, 3, bad_register);
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: ") != NULL, r.err);

	run_main(&r, 4, stray_word);
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: ") != NULL, r.err);

	run_main(&r, 1, no_register);
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: ") != NULL, r.err);

	/* -h is what the user asked for, so it goes to standard output. */
	run_main(&r, 2, help);
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Usage:") != NULL, r.out);
	mu_assert(r.err[0] == '\0', r.err);

	/* Refusals that come after a -r has already been accepted, so the tool
	 * is holding a parsed register list when it gives up. These pin the
	 * status and that no device is opened; that the list is freed is plain
	 * malloc and free, which these fakes cannot see. */
	run_main(&r, 4, bad_verbosity_after_a_list);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 4, unknown_option_after_a_list);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 4, help_after_a_list);
	mu_assert_int_eq(0, r.rc);

	mu_assert_int_eq(0, fake.starts);
	mu_assert_int_eq(0, fake.closes);
	mu_assert_int_eq(0, fake.frees);
}

/* Every exit reached after a -r was accepted gives the parsed list back, the
 * refusal of a second -r included. The tool used to free it on none of
 * them. */
MU_TEST(the_parsed_register_list_is_given_back)
{
	char* read[] = { "ubertooth-debug", "-r", "0x02", NULL };
	char* bad_verbosity[] = { "ubertooth-debug", "-r", "0x02", "-v", "9", NULL };
	char* unknown_option[] = { "ubertooth-debug", "-r", "0x02", "-Z", NULL };
	char* with_help[] = { "ubertooth-debug", "-r", "0x02", "-h", NULL };
	char* stray_word[] = { "ubertooth-debug", "-r", "0x02", "spare", NULL };
	char* no_board[] = { "ubertooth-debug", "-r", "0x02", NULL };
	char* repeated[] = { "ubertooth-debug", "-r", "0x02", "-r", "0x04", NULL };
	struct run r;

	reset();
	run_main(&r, 3, read);
	mu_assert_int_eq(1, lists_freed);

	reset();
	run_main(&r, 5, bad_verbosity);
	mu_assert_int_eq(1, lists_freed);

	reset();
	run_main(&r, 4, unknown_option);
	mu_assert_int_eq(1, lists_freed);

	reset();
	run_main(&r, 4, with_help);
	mu_assert_int_eq(1, lists_freed);

	reset();
	run_main(&r, 4, stray_word);
	mu_assert_int_eq(1, lists_freed);

	reset();
	fake.fail_start = 1;
	run_main(&r, 3, no_board);
	mu_assert_int_eq(1, lists_freed);

	/* A second -r is refused, and the list the first one parsed is still
	 * given back on the way out. */
	reset();
	run_main(&r, 5, repeated);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: option '-r' given more than once\n") == 0,
	          r.err);
	mu_assert_int_eq(1, lists_freed);
	mu_assert_int_eq(0, fake.starts);
}

int main(void)
{
	/* Neither binary links libubertooth: the fakes above are the whole
	 * device, and any new library call the tool grows becomes a link
	 * error rather than a silently real one. */
	MU_RUN(a_completed_read_releases_the_handle);
	MU_RUN(a_failed_read_releases_the_handle_too);
	MU_RUN(a_device_that_cannot_be_opened_frees_nothing);
	MU_RUN(the_refusals_never_open_a_device);
	MU_RUN(the_parsed_register_list_is_given_back);
	return mu_report();
}
