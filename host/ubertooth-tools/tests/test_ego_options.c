#include <stdint.h>
#include <stdio.h>

#include "ego_options.h"

static int checks;
static int failures;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
		failures++; \
	} \
} while (0)

static void expect_valid(const char *text, uint8_t expected)
{
	uint8_t value = 0;
	CHECK(ego_parse_packet_length(text, &value) == 0);
	CHECK(value == expected);
}

static void expect_invalid(const char *text)
{
	uint8_t value = 23;
	CHECK(ego_parse_packet_length(text, &value) < 0);
	CHECK(value == 23);
}

int main(void)
{
	expect_valid("1", 1);
	expect_valid("18", 18);
	expect_valid("48", 48);
	expect_invalid(NULL);
	expect_invalid("");
	expect_invalid("0");
	expect_invalid("49");
	expect_invalid("-1");
	expect_invalid("abc");
	expect_invalid("12x");
	expect_invalid("256");
	expect_invalid("257");
	expect_invalid("999999999999999999999999");
	printf("ego options: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
