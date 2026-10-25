/*
 * Tests for the test harness itself: what a failed assertion does.
 *
 * mu_assert records a failure and carries on, which is right for independent
 * checks but wrong for anything the test then uses: a test that asserts a
 * pointer is not NULL and then dereferences it crashes, and a crash reports
 * no assertion. mu_assert_fatal is the form that stops the test function, so
 * the failure that mattered is the one that gets reported.
 *
 * The cases below fail assertions on purpose. Each one captures the FAIL
 * lines and restores the counters, so the deliberate failures are this test's
 * subject rather than its result.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "minunit.h"

/* Set after the assertion in each helper, so whether the helper ran on is
 * visible to the caller. */
static int ran_on_after_failed_fatal;
static int ran_on_after_failed_plain;
static int ran_on_after_passed_fatal;

static void failed_fatal_assertion(void)
{
	mu_assert_fatal(0, "deliberate failure: fatal");
	ran_on_after_failed_fatal = 1;
}

static void failed_plain_assertion(void)
{
	mu_assert(0, "deliberate failure: plain");
	ran_on_after_failed_plain = 1;
}

static void passed_fatal_assertion(void)
{
	mu_assert_fatal(1, "this one holds");
	ran_on_after_passed_fatal = 1;
}

static int evaluations;

static int evaluate(int result)
{
	evaluations++;
	return result;
}

static void failed_fatal_assertion_on_a_call(void)
{
	mu_assert_fatal(evaluate(0) == 1, "deliberate failure: one evaluation");
}

/* ---- capturing the FAIL lines ------------------------------------------- */

static FILE* capture;
static int saved_stdout;

static void capture_stdout(void)
{
	capture = tmpfile();
	saved_stdout = dup(fileno(stdout));
	mu_assert(capture != NULL && saved_stdout >= 0, "capture stdout");
	fflush(stdout);
	dup2(fileno(capture), fileno(stdout));
}

static void release_stdout(char* text, size_t len)
{
	size_t n;

	fflush(stdout);
	dup2(saved_stdout, fileno(stdout));
	close(saved_stdout);
	rewind(capture);
	n = fread(text, 1, len - 1, capture);
	text[n] = '\0';
	fclose(capture);
}

/* ---- tests -------------------------------------------------------------- */

MU_TEST(a_failed_fatal_assertion_stops_the_function_and_a_plain_one_does_not)
{
	int checks_before, fails_before, checks_after, fails_after;
	char text[1024] = "";

	/* after capture_stdout(), which makes a check of its own */
	capture_stdout();
	checks_before = mu_checks;
	fails_before = mu_fails;
	failed_fatal_assertion();
	failed_plain_assertion();
	checks_after = mu_checks;
	fails_after = mu_fails;
	release_stdout(text, sizeof(text));

	/* The two deliberate failures above are not this test's own result. */
	mu_checks = checks_before;
	mu_fails = fails_before;

	mu_assert_int_eq(0, ran_on_after_failed_fatal);
	mu_assert_int_eq(1, ran_on_after_failed_plain);

	/* Both were counted and reported: a fatal assertion reports its failure
	 * the same way, it just does not come back. */
	mu_assert_int_eq(2, checks_after - checks_before);
	mu_assert_int_eq(2, fails_after - fails_before);
	mu_assert(strstr(text, "deliberate failure: fatal") != NULL, text);
	mu_assert(strstr(text, "deliberate failure: plain") != NULL, text);
	mu_assert(strstr(text, "test_minunit.c") != NULL, text);
}

MU_TEST(a_fatal_assertion_that_holds_changes_nothing)
{
	int fails_before = mu_fails;

	passed_fatal_assertion();
	mu_assert_int_eq(1, ran_on_after_passed_fatal);
	mu_assert_int_eq(fails_before, mu_fails);
}

/* A condition with a side effect -- a call, or an assignment the test needs
 * -- must happen once, whether it holds or not. */
MU_TEST(a_fatal_assertion_evaluates_its_condition_once)
{
	int checks_before, fails_before;
	char text[256] = "";

	evaluations = 0;
	mu_assert_fatal(evaluate(1) == 1, "a condition that holds");
	mu_assert_int_eq(1, evaluations);

	evaluations = 0;
	capture_stdout();
	checks_before = mu_checks;
	fails_before = mu_fails;
	failed_fatal_assertion_on_a_call();
	release_stdout(text, sizeof(text));
	mu_checks = checks_before; /* the deliberate failure is not the result */
	mu_fails = fails_before;

	mu_assert_int_eq(1, evaluations);
	mu_assert(strstr(text, "deliberate failure: one evaluation") != NULL, text);
}

int main(void)
{
	MU_RUN(a_failed_fatal_assertion_stops_the_function_and_a_plain_one_does_not);
	MU_RUN(a_fatal_assertion_that_holds_changes_nothing);
	MU_RUN(a_fatal_assertion_evaluates_its_condition_once);
	return mu_report();
}
