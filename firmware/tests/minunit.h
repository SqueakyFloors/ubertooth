/*
 * minunit.h - a tiny, dependency-free unit-test harness for Project Ubertooth
 * firmware logic that is pure C and can be compiled and run on the host.
 *
 * Usage:
 *   #include "minunit.h"
 *   MU_TEST(name) { mu_assert_int_eq(expected, actual); }
 *   int main(void) { MU_RUN(name); return mu_report(); }
 *
 * No external dependencies: just <stdio.h>. Each assertion prints a FAIL line
 * with file:line and the expected/actual values; mu_report() prints a summary
 * and returns non-zero if any assertion failed (so `make` / CI can gate on it).
 */
#ifndef UBERTOOTH_MINUNIT_H
#define UBERTOOTH_MINUNIT_H

#include <stdio.h>

static int mu_checks = 0;
static int mu_fails = 0;
static const char *mu_current = "";

#define MU_TEST(name) static void name(void)

#define MU_RUN(name) do {                                                \
		mu_current = #name;                                              \
		printf("- %s\n", #name);                                        \
		name();                                                         \
	} while (0)

/* Generic boolean assertion. */
#define mu_assert(cond, msg) do {                                        \
		mu_checks++;                                                     \
		if (!(cond)) {                                                   \
			mu_fails++;                                                  \
			printf("  FAIL %s:%d [%s] %s\n",                            \
			       __FILE__, __LINE__, mu_current, (msg));              \
		}                                                                \
	} while (0)

/* Like mu_assert, but gives up on the rest of the test function.
 *
 * Use it for anything the test goes on to use: a plain mu_assert records the
 * failure and carries on, so a test that asserts a pointer is not NULL and
 * then dereferences it turns its own failure into a crash, and a crash
 * reports no assertion at all.
 *
 * It returns from the function it appears in, so it belongs in a test body
 * (MU_TEST), not in a helper the test calls. The condition is evaluated once.
 */
#define mu_assert_fatal(cond, msg) do {                                  \
		int _mu_ok = (cond) ? 1 : 0;                                    \
		mu_assert(_mu_ok, msg);                                          \
		if (!_mu_ok)                                                     \
			return;                                                      \
	} while (0)

/* Integer equality with a helpful diff on failure. */
#define mu_assert_int_eq(expected, actual) do {                          \
		long _e = (long)(expected);                                     \
		long _a = (long)(actual);                                       \
		mu_checks++;                                                     \
		if (_e != _a) {                                                  \
			mu_fails++;                                                  \
			printf("  FAIL %s:%d [%s] expected %ld, got %ld\n",         \
			       __FILE__, __LINE__, mu_current, _e, _a);             \
		}                                                                \
	} while (0)

static int mu_report(void)
{
	printf("\n%d checks, %d failed\n", mu_checks, mu_fails);
	return mu_fails ? 1 : 0;
}

#endif /* UBERTOOTH_MINUNIT_H */
