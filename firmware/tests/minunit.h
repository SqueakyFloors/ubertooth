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
