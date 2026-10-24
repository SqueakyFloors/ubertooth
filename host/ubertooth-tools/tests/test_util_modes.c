/*
 * Tests of ubertooth-util's exclusive mode check, without a device.
 */
#include <stdio.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "util_modes.h"

#define ALL_MODES "-q -n -e -t -x -I -i -f"

/* Runs the check and captures what it writes to err. */
static int check(const int requested[UTIL_MODE_COUNT], char *text, size_t size)
{
	FILE *err = tmpfile();
	size_t length;
	int rc;

	mu_assert(err != NULL, "tmpfile failed");
	if (err == NULL)
		return -1;
	rc = util_check_modes(requested, err);
	rewind(err);
	length = fread(text, 1, size - 1, err);
	text[length] = '\0';
	fclose(err);
	return rc;
}

MU_TEST(options_are_named_in_table_order)
{
	static const char *const expected[UTIL_MODE_COUNT] = {
		"-q", "-n", "-e", "-t", "-x", "-I", "-i", "-f",
	};
	int m;

	for (m = 0; m < UTIL_MODE_COUNT; m++)
		mu_assert(strcmp(expected[m], util_mode_option((enum util_mode)m)) == 0,
		          "mode option");
	mu_assert(util_mode_option(UTIL_MODE_COUNT) == NULL, "out of range");
}

MU_TEST(no_mode_or_one_mode_is_accepted_silently)
{
	int requested[UTIL_MODE_COUNT] = { 0 };
	char text[256];
	int m;

	mu_assert_int_eq(0, check(requested, text, sizeof(text)));
	mu_assert_int_eq(0, (long)strlen(text));
	for (m = 0; m < UTIL_MODE_COUNT; m++) {
		memset(requested, 0, sizeof(requested));
		requested[m] = 1;
		mu_assert_int_eq(0, check(requested, text, sizeof(text)));
		mu_assert_int_eq(0, (long)strlen(text));
	}
}

MU_TEST(every_pair_of_modes_is_rejected_with_both_options_named)
{
	int a, b;

	for (a = 0; a < UTIL_MODE_COUNT; a++) {
		for (b = a + 1; b < UTIL_MODE_COUNT; b++) {
			int requested[UTIL_MODE_COUNT] = { 0 };
			char text[256], expected[256];

			requested[a] = 1;
			requested[b] = 1;
			snprintf(expected, sizeof(expected),
			         "error: options %s %s cannot be combined; give only one of "
			         ALL_MODES "\n",
			         util_mode_option((enum util_mode)a),
			         util_mode_option((enum util_mode)b));
			mu_assert_int_eq(1, check(requested, text, sizeof(text)));
			mu_assert(strcmp(expected, text) == 0, "pair diagnostic");
		}
	}
}

MU_TEST(more_than_two_modes_are_rejected_and_all_named)
{
	int requested[UTIL_MODE_COUNT];
	char text[256];
	int m;

	for (m = 0; m < UTIL_MODE_COUNT; m++)
		requested[m] = 1;
	mu_assert_int_eq(1, check(requested, text, sizeof(text)));
	mu_assert(strcmp("error: options " ALL_MODES " cannot be combined; "
	                 "give only one of " ALL_MODES "\n", text) == 0,
	          "all-modes diagnostic");

	memset(requested, 0, sizeof(requested));
	requested[UTIL_MODE_LED_SPECAN] = 1;
	requested[UTIL_MODE_TX_TEST] = 1;
	requested[UTIL_MODE_IDENTIFY] = 1;
	mu_assert_int_eq(1, check(requested, text, sizeof(text)));
	mu_assert(strncmp("error: options -q -t -I cannot be combined;", text,
	                  strlen("error: options -q -t -I cannot be combined;")) == 0,
	          "three-mode diagnostic");
}

MU_TEST(any_nonzero_value_counts_as_requested)
{
	int requested[UTIL_MODE_COUNT] = { 0 };
	char text[256];

	requested[UTIL_MODE_XMAS] = 7;
	mu_assert_int_eq(0, check(requested, text, sizeof(text)));
	requested[UTIL_MODE_REPEATER] = -1;
	mu_assert_int_eq(1, check(requested, text, sizeof(text)));
}

int main(void)
{
	MU_RUN(options_are_named_in_table_order);
	MU_RUN(no_mode_or_one_mode_is_accepted_silently);
	MU_RUN(every_pair_of_modes_is_rejected_with_both_options_named);
	MU_RUN(more_than_two_modes_are_rejected_and_all_named);
	MU_RUN(any_nonzero_value_counts_as_requested);
	return mu_report();
}
