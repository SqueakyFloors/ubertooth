/*
 * run_main.h - run a tool's main() in a test with both its streams captured.
 *
 * Several tests here drive a tool's real main(): they compile the tool's .c
 * file with main() renamed, replace the library calls it makes with fakes,
 * and then all need the same thing. Build an argv, send standard output and
 * standard error to temporary files, restart option parsing, call the renamed
 * main(), put the streams back, and read both captures into strings. That
 * part lives here, so a fix to it reaches every test rather than one copy.
 *
 * Include this after minunit.h and after the tool's .c file, and wrap
 * run_tool_main() in a run_<tool>() of your own. The wrapper is where
 * whatever is particular to one test stays: the globals that tool leaves
 * behind, the allocations the test counts.
 *
 *     static struct run_result run_ego(const char *const *args)
 *     {
 *             struct run_result result = run_tool_main(ubertooth_ego_main,
 *                                                      args);
 *
 *             mu_assert(fake.frees == fake.inits,
 *                       "the run did not free its device");
 *             return result;
 *     }
 *
 *     #define RUN(...) \
 *             run_ego((const char *const[]){ "ubertooth-ego", __VA_ARGS__, \
 *                                            NULL })
 *
 * A test whose captures need more room than the 4096 bytes below defines
 * RUN_RESULT_OUT_SIZE or RUN_RESULT_ERR_SIZE before including this.
 *
 * A case that drives one of a tool's own functions rather than its main()
 * captures the two streams around the call itself, with run_capture_begin()
 * and run_capture_end().
 */
#ifndef UBERTOOTH_RUN_MAIN_H
#define UBERTOOTH_RUN_MAIN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* dup(), dup2() and the standard descriptor numbers, and optind, which POSIX
 * declares here as well as in <getopt.h>. A test that brings its own getopt
 * along has already included that one by this point. */
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#ifndef RUN_RESULT_OUT_SIZE
#define RUN_RESULT_OUT_SIZE 4096
#endif

#ifndef RUN_RESULT_ERR_SIZE
#define RUN_RESULT_ERR_SIZE 4096
#endif

/* The most arguments one run can pass on, argv[0] included. */
#define RUN_MAIN_MAX_ARGS 15

/* What one run of a tool did: its exit status and both of its streams. */
struct run_result {
	int rc;
	/* Either can hold a whole usage text: a refused command line prints it
	 * on standard error, and -h on standard output. */
	char out[RUN_RESULT_OUT_SIZE];
	char err[RUN_RESULT_ERR_SIZE];
};

/* A renamed main(), as the tests compile it. */
typedef int (*run_main_fn)(int argc, char *argv[]);

/* Read a captured stream back into buffer as a string. */
static void read_back(FILE *file, char *buffer, size_t size)
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

/*
 * Both standard streams of one run, held aside while it prints into temporary
 * files. run_tool_main() below captures a whole run this way, and a test that
 * drives one of a tool's own functions rather than its main() captures that
 * the same way.
 */
struct run_capture {
	FILE *out, *err;
	/* The descriptors the real streams are held on meanwhile. */
	int saved_out, saved_err;
};

/*
 * Give back whatever a capture that could not be set up had taken: its two
 * files and the descriptors the streams were saved on. A NULL file or a
 * negative descriptor is one it never got.
 *
 * mu_assert() counts a failure and carries on, so this matters: the rest of
 * the test run would otherwise hold handles nothing can reach.
 */
static void run_capture_give_back(struct run_capture *capture)
{
	if (capture->out != NULL)
		fclose(capture->out);
	if (capture->err != NULL)
		fclose(capture->err);
	if (capture->saved_out >= 0)
		close(capture->saved_out);
	if (capture->saved_err >= 0)
		close(capture->saved_err);
}

/*
 * Send standard output and standard error to temporary files.
 *
 * Returns 0 once the capture is in place, or -1 if it could not be set up,
 * having failed an assertion, given back what it took and left both streams
 * alone. Nothing may be run after a -1: its output would reach whoever is
 * running the tests, and run_capture_end() would have nothing to read.
 */
static int run_capture_begin(struct run_capture *capture)
{
	capture->out = tmpfile();
	capture->err = tmpfile();
	capture->saved_out = -1;
	capture->saved_err = -1;
	mu_assert(capture->out != NULL && capture->err != NULL,
	          "tmpfile failed");
	if (capture->out == NULL || capture->err == NULL) {
		run_capture_give_back(capture);
		return -1;
	}

	fflush(stdout);
	fflush(stderr);
	capture->saved_out = dup(STDOUT_FILENO);
	capture->saved_err = dup(STDERR_FILENO);
	/* Nothing has been redirected yet, so there is nothing to put back. */
	mu_assert(capture->saved_out >= 0 && capture->saved_err >= 0,
	          "a standard stream could not be saved");
	if (capture->saved_out < 0 || capture->saved_err < 0) {
		run_capture_give_back(capture);
		return -1;
	}

	if (dup2(fileno(capture->out), STDOUT_FILENO) < 0 ||
	    dup2(fileno(capture->err), STDERR_FILENO) < 0) {
		/* Whichever of the two did get redirected is put back, so the
		 * rest of the test run keeps its own streams. Running the tool
		 * with one stream uncaptured would compare the other against
		 * assertions the run never had a chance to satisfy.
		 *
		 * The results are not checked: the assertion below reports the
		 * run as failed either way, and there is nothing further to do
		 * about a process whose streams cannot be moved at all. */
		dup2(capture->saved_out, STDOUT_FILENO);
		dup2(capture->saved_err, STDERR_FILENO);
		mu_assert(0, "a standard stream could not be redirected");
		run_capture_give_back(capture);
		return -1;
	}
	return 0;
}

/* Put both streams back and read the two captures into result. */
static void run_capture_end(struct run_capture *capture,
                            struct run_result *result)
{
	int restored_out, restored_err;

	fflush(stdout);
	fflush(stderr);
	/* Both are put back before either is reported on, because a stream
	 * left redirected would send the report into a capture file instead of
	 * to whoever is running the test. */
	restored_out = dup2(capture->saved_out, STDOUT_FILENO);
	restored_err = dup2(capture->saved_err, STDERR_FILENO);
	close(capture->saved_out);
	close(capture->saved_err);
	mu_assert(restored_out >= 0 && restored_err >= 0,
	          "a standard stream could not be put back");

	read_back(capture->out, result->out, sizeof(result->out));
	read_back(capture->err, result->err, sizeof(result->err));
	fclose(capture->out);
	fclose(capture->err);
}

/* Give back the copies run_tool_main() made of a test's arguments. */
static void run_main_free_argv(char **argv, int argc)
{
	while (argc > 0)
		free(argv[--argc]);
}

/*
 * Run tool_main() with args as its argv and both of its streams captured.
 *
 * args is NULL-terminated and starts with the tool's name. Each argument is
 * copied, because a real argv is writable and some tools write to it
 * (ubertooth-btle splits "<address>/<mask>" in place), while the string
 * literals a test passes are not.
 *
 * A capture that could not be set up fails an assertion and returns exit
 * status -1, which no tool returns, so a run that was never made cannot be
 * mistaken for one that worked.
 */
static struct run_result run_tool_main(run_main_fn tool_main,
                                       const char *const *args)
{
	struct run_result result;
	char *argv[RUN_MAIN_MAX_ARGS + 1];
	/* The same copies again, to give back afterwards. A tool is allowed to
	 * replace a pointer in the argv it is handed, so by the time it returns,
	 * argv no longer holds only what was allocated here, and freeing from it
	 * could free a string literal. */
	char *copies[RUN_MAIN_MAX_ARGS];
	int argc = 0;
	struct run_capture capture;

	memset(&result, 0, sizeof(result));
	result.rc = -1; /* a failed capture must not look like a successful run */

	while (args[argc] != NULL && argc < RUN_MAIN_MAX_ARGS) {
		argv[argc] = copies[argc] = strdup(args[argc]);
		mu_assert(argv[argc] != NULL, "strdup failed");
		if (argv[argc] == NULL) {
			run_main_free_argv(copies, argc);
			return result;
		}
		argc++;
	}
	argv[argc] = NULL;
	/* A command line longer than that would be cut short here, and the case
	 * would go on to test a command nobody wrote. */
	mu_assert(args[argc] == NULL, "too many arguments for one run");

	if (run_capture_begin(&capture) < 0) {
		run_main_free_argv(copies, argc);
		return result;
	}

	optind = 0; /* restart option parsing for each run */
	result.rc = tool_main(argc, argv);

	run_capture_end(&capture, &result);
	run_main_free_argv(copies, argc);
	return result;
}

#endif /* UBERTOOTH_RUN_MAIN_H */
