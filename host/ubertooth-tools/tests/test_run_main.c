/*
 * Tests of the shared test harness in run_main.h, which every test that
 * drives a tool's main() relies on. A fault here would quietly weaken all of
 * them: a capture read back from the wrong stream, a truncation that passes,
 * a second run that cannot parse its options.
 *
 * The tool under test is the fake main() below rather than a real tool, so
 * each case checks the harness and nothing else.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
/* The harness under test. It brings in the standard streams it redirects and
 * the optind it resets. */
#include "run_main.h"

/* The longest argument a case passes. */
#define MAX_ARG 32

/* What the fake tool was given, and what it should do with it. The arguments
 * are copied here because the harness frees its argv once the run is over. */
static struct {
	int argc;
	char argv[RUN_MAIN_MAX_ARGS][MAX_ARG];
	int argv_is_terminated; /* argv[argc] was NULL, as getopt needs */
	int optind_on_entry;
	int runs;
	int rc;               /* what the fake tool returns */
	size_t out_bytes;     /* this many 'o' bytes on standard output */
	int write_to_argv;    /* overwrite argv[1], as a tool parsing in place does */
	int replace_argv_pointer; /* swap argv[1] for a literal, as a tool
	                           * rewriting a long option does */
} tool;

static void reset_tool(void)
{
	memset(&tool, 0, sizeof(tool));
}

static int fake_tool_main(int argc, char *argv[])
{
	int i;

	tool.runs++;
	tool.optind_on_entry = optind;
	tool.argc = argc;

	printf("a result\n");
	fprintf(stderr, "error: a diagnostic\n");
	for (; tool.out_bytes > 0; tool.out_bytes--)
		putchar('o');
	/* Bounded by what it is overwriting, so a case passing a short argument
	 * cannot run off the end of it. */
	if (tool.write_to_argv && argc > 1)
		snprintf(argv[1], strlen(argv[1]) + 1, "written");
	/* A pointer swapped for a string literal, rather than a write through
	 * it: what a tool does to rewrite a long option to a short one. */
	if (tool.replace_argv_pointer && argc > 1)
		argv[1] = (char *)"-l";

	/* Recorded after the write above, so a case can see what the tool left
	 * in its own argv. */
	for (i = 0; i < argc && i < RUN_MAIN_MAX_ARGS; i++)
		snprintf(tool.argv[i], MAX_ARG, "%s", argv[i]);
	tool.argv_is_terminated = argv[argc] == NULL;

	/* The harness leaves option parsing to the tool, so a tool that moved
	 * optind must not decide where the next run starts. */
	optind = 7;
	return tool.rc;
}

#define RUN(...)                                                     \
	run_tool_main(fake_tool_main,                                    \
	              (const char *const[]){ "a-tool", __VA_ARGS__, NULL })

/* ---- tests -------------------------------------------------------------- */

/* The two streams are captured apart from each other, and the tool's exit
 * status is what the run reports. */
MU_TEST(each_stream_is_captured_on_its_own_with_the_exit_status)
{
	struct run_result r;

	reset_tool();
	tool.rc = 3;
	r = RUN("-x");
	mu_assert_int_eq(3, r.rc);
	mu_assert(strcmp(r.out, "a result\n") == 0, r.out);
	mu_assert(strcmp(r.err, "error: a diagnostic\n") == 0, r.err);
}

/* The arguments arrive as a real argv: argv[0] first, the count right, and a
 * NULL after the last one, which getopt relies on. */
MU_TEST(the_arguments_arrive_as_a_real_argv)
{
	reset_tool();
	RUN("-v", "1", "stray");
	mu_assert_int_eq(4, tool.argc);
	mu_assert(strcmp(tool.argv[0], "a-tool") == 0, tool.argv[0]);
	mu_assert(strcmp(tool.argv[1], "-v") == 0, tool.argv[1]);
	mu_assert(strcmp(tool.argv[2], "1") == 0, tool.argv[2]);
	mu_assert(strcmp(tool.argv[3], "stray") == 0, tool.argv[3]);
	mu_assert(tool.argv_is_terminated, "argv was not NULL-terminated");

	/* a command line with nothing but the tool's name */
	reset_tool();
	run_tool_main(fake_tool_main, (const char *const[]){ "a-tool", NULL });
	mu_assert_int_eq(1, tool.argc);
	mu_assert(tool.argv_is_terminated, "argv was not NULL-terminated");
}

/* Option parsing restarts for each run, so the second run of a case reads the
 * same command line as the first. Without this, a tool that advanced optind
 * would leave the next run starting from the middle of its arguments. */
MU_TEST(option_parsing_restarts_for_each_run)
{
	reset_tool();
	RUN("-x");
	mu_assert_int_eq(0, tool.optind_on_entry);
	RUN("-x");
	mu_assert_int_eq(2, tool.runs);
	mu_assert_int_eq(0, tool.optind_on_entry);
}

/* The tool is handed copies, because a real argv is writable and some tools
 * split a value in place. The strings a case passes are string literals, so
 * writing to them would be undefined, and the next run would read what the
 * last one wrote. */
MU_TEST(the_tool_may_write_to_its_argv)
{
	static const char *const args[] = { "a-tool", "-t22:44:66:88:aa:cc",
	                                    NULL };

	reset_tool();
	tool.write_to_argv = 1;
	run_tool_main(fake_tool_main, args);
	mu_assert(strcmp(tool.argv[1], "written") == 0, tool.argv[1]);
	/* what the case passed is untouched */
	mu_assert(strcmp(args[1], "-t22:44:66:88:aa:cc") == 0, args[1]);

	reset_tool();
	run_tool_main(fake_tool_main, args);
	mu_assert(strcmp(tool.argv[1], "-t22:44:66:88:aa:cc") == 0,
	          tool.argv[1]);
}

/*
 * A tool may also replace a pointer in its argv instead of writing through
 * it: that is how a long option is rewritten to the short one getopt knows.
 * So the copies given back when the run is over are a list the harness kept,
 * not the argv the tool was handed -- that one no longer holds only what was
 * allocated for it, and free() on a string literal aborted the whole test
 * binary when this case was written.
 *
 * The run completing is most of the check; the assertions say the replacement
 * really happened and that what the case passed is untouched.
 */
MU_TEST(the_tool_may_replace_a_pointer_in_its_argv)
{
	static const char *const args[] = { "a-tool", "--long-option", NULL };
	struct run_result r;

	reset_tool();
	tool.replace_argv_pointer = 1;
	r = run_tool_main(fake_tool_main, args);
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(tool.argv[1], "-l") == 0, tool.argv[1]);
	mu_assert(strcmp(args[1], "--long-option") == 0, args[1]);
}

/* Each run starts with empty captures, so what one run printed cannot turn up
 * in the next. */
MU_TEST(a_run_does_not_see_what_the_run_before_it_printed)
{
	struct run_result r;

	reset_tool();
	tool.out_bytes = 100;
	r = RUN("-x");
	/* "a result\n" and the 100 bytes after it */
	mu_assert_int_eq(109, (int)strlen(r.out));

	reset_tool();
	r = RUN("-x");
	mu_assert(strcmp(r.out, "a result\n") == 0, r.out);
}

/* Both of the test's own streams are put back, so this file's own output is
 * not written into a temporary file nobody reads. */
MU_TEST(the_test_gets_its_own_streams_back)
{
	struct stat out_before, err_before, out_after, err_after;

	reset_tool();
	mu_assert_fatal(fstat(STDOUT_FILENO, &out_before) == 0, "fstat failed");
	mu_assert_fatal(fstat(STDERR_FILENO, &err_before) == 0, "fstat failed");
	RUN("-x");
	mu_assert_fatal(fstat(STDOUT_FILENO, &out_after) == 0, "fstat failed");
	mu_assert_fatal(fstat(STDERR_FILENO, &err_after) == 0, "fstat failed");

	/* The same file is on each of the two descriptors as before the run. */
	mu_assert(out_before.st_dev == out_after.st_dev &&
	          out_before.st_ino == out_after.st_ino,
	          "standard output was not put back");
	mu_assert(err_before.st_dev == err_after.st_dev &&
	          err_before.st_ino == err_after.st_ino,
	          "standard error was not put back");
}

/*
 * Part of what the harness does is report a failure: a capture that did not
 * fit, a command line longer than one run can pass on. Checking that means
 * letting an assertion fail on purpose, which prints to standard output and
 * counts against the test run. These two hold both aside, so what happens in
 * between can be counted instead.
 */
static struct {
	FILE *sink;
	int saved_out;
	int checks_before;
	int fails_before;
} quiet;

/* 0 if the streams could not be moved, in which case nothing was changed. */
static int begin_quiet(void)
{
	quiet.sink = tmpfile();
	quiet.saved_out = dup(STDOUT_FILENO);
	quiet.checks_before = mu_checks;
	quiet.fails_before = mu_fails;

	fflush(stdout);
	if (quiet.sink == NULL || quiet.saved_out < 0 ||
	    dup2(fileno(quiet.sink), STDOUT_FILENO) < 0) {
		if (quiet.sink != NULL)
			fclose(quiet.sink);
		if (quiet.saved_out >= 0)
			close(quiet.saved_out);
		mu_assert(0, "the test's own output could not be held aside");
		return 0;
	}
	return 1;
}

/* How many assertions failed since begin_quiet(). */
static int end_quiet(void)
{
	int restored;
	int failures;

	fflush(stdout);
	restored = dup2(quiet.saved_out, STDOUT_FILENO);
	close(quiet.saved_out);
	fclose(quiet.sink);

	failures = mu_fails - quiet.fails_before;
	mu_checks = quiet.checks_before;
	mu_fails = quiet.fails_before;
	mu_assert(restored >= 0, "the test's own output could not be put back");
	return failures;
}

static int failures_from_read_back(FILE *file, char *buffer, size_t size)
{
	if (!begin_quiet())
		return -1;
	read_back(file, buffer, size);
	return end_quiet();
}

static int failures_from_run(const char *const *args)
{
	if (!begin_quiet())
		return -1;
	run_tool_main(fake_tool_main, args);
	return end_quiet();
}

/* A command line longer than the harness can pass on is reported rather than
 * cut short, which would leave the case testing a command nobody wrote. */
MU_TEST(a_command_line_too_long_for_one_run_is_reported)
{
	static const char *const too_many[RUN_MAIN_MAX_ARGS + 2] = {
		"a-tool", "1", "2", "3", "4", "5", "6", "7", "8", "9",
		"10", "11", "12", "13", "14", "15", NULL
	};
	const char *const *just_fits = too_many + 1; /* drops one argument */

	/* The first entry is the tool's name, so this is one too many. */
	reset_tool();
	mu_assert_int_eq(1, failures_from_run(too_many));

	reset_tool();
	mu_assert_int_eq(0, failures_from_run(just_fits));
	mu_assert_int_eq(RUN_MAIN_MAX_ARGS, tool.argc);
}

MU_TEST(a_capture_that_did_not_fit_is_reported)
{
	char buffer[8];
	FILE *file = tmpfile();

	mu_assert_fatal(file != NULL, "tmpfile failed");

	/* a capture that fits exactly: seven bytes and the terminator */
	rewind(file);
	mu_assert_fatal(fputs("1234567", file) >= 0, "writing the capture failed");
	mu_assert_int_eq(0, failures_from_read_back(file, buffer, sizeof(buffer)));
	mu_assert(strcmp(buffer, "1234567") == 0, buffer);

	/* one byte more than fits */
	rewind(file);
	mu_assert_fatal(fputs("12345678", file) >= 0, "writing the capture failed");
	mu_assert_int_eq(1, failures_from_read_back(file, buffer, sizeof(buffer)));
	/* the prefix is still a string, so a failing case can print it */
	mu_assert(strcmp(buffer, "1234567") == 0, buffer);
	fclose(file);

	/* an empty capture is not a truncated one */
	file = tmpfile();
	mu_assert_fatal(file != NULL, "tmpfile failed");
	mu_assert_int_eq(0, failures_from_read_back(file, buffer, sizeof(buffer)));
	mu_assert(buffer[0] == '\0', buffer);
	fclose(file);
}

int main(void)
{
	MU_RUN(each_stream_is_captured_on_its_own_with_the_exit_status);
	MU_RUN(the_arguments_arrive_as_a_real_argv);
	MU_RUN(option_parsing_restarts_for_each_run);
	MU_RUN(the_tool_may_write_to_its_argv);
	MU_RUN(the_tool_may_replace_a_pointer_in_its_argv);
	MU_RUN(a_run_does_not_see_what_the_run_before_it_printed);
	MU_RUN(the_test_gets_its_own_streams_back);
	MU_RUN(a_capture_that_did_not_fit_is_reported);
	MU_RUN(a_command_line_too_long_for_one_run_is_reported);
	return mu_report();
}
