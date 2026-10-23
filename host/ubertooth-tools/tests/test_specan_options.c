#include <stdio.h>
#include <string.h>

#include "specan_options.h"

static int checks;
static int failures;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
		failures++; \
	} \
} while (0)

static void expect_valid(const char *text, int expected)
{
	int value = -1;
	CHECK(specan_parse_frequency(text, &value) == 0);
	CHECK(value == expected);
}

static void expect_invalid(const char *text)
{
	int value = 99;
	CHECK(specan_parse_frequency(text, &value) < 0);
	CHECK(value == 99);
}

int main(void)
{
	expect_valid("2402", 2402);
	expect_valid("2440", 2440);
	expect_valid("2480", 2480);
	expect_invalid(NULL);
	expect_invalid("");
	expect_invalid("2401");
	expect_invalid("2481");
	expect_invalid("24x0");
	expect_invalid("2440MHz");
	expect_invalid("-2440");
	CHECK(specan_validate_frequency_range(2402, 2402) == 0);
	CHECK(specan_validate_frequency_range(2402, 2480) == 0);
	CHECK(specan_validate_frequency_range(2441, 2440) < 0);
	printf("specan options: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
