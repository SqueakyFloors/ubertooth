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

static void expect_code(const char *text, const uint8_t *expected, int expected_len)
{
	uint8_t code[EGO_ACCESS_CODE_BYTES] = { 0 };
	int len = 0, i;
	CHECK(ego_parse_access_code(text, code, &len) == 0);
	CHECK(len == expected_len);
	for (i = 0; i < expected_len; i++)
		CHECK(code[i] == expected[i]);
}

static void expect_bad_code(const char *text)
{
	uint8_t code[EGO_ACCESS_CODE_BYTES] = { 0xaa, 0xaa, 0xaa, 0xaa };
	int len = 7, i;
	CHECK(ego_parse_access_code(text, code, &len) < 0);
	CHECK(len == 7); /* untouched on failure */
	for (i = 0; i < EGO_ACCESS_CODE_BYTES; i++)
		CHECK(code[i] == 0xaa);
}

int main(void)
{
	static const uint8_t full[] = { 0x63, 0x0f, 0x9f, 0xfe };
	static const uint8_t one[] = { 0x0a };
	static const uint8_t two[] = { 0xAB, 0xcd };

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
	expect_code("630f9ffe", full, 4);
	expect_code("0a", one, 1);
	expect_code("ABcd", two, 2);
	expect_bad_code(NULL);
	expect_bad_code("");
	expect_bad_code("a");          /* odd digit count */
	expect_bad_code("630f9");
	expect_bad_code("630f9ff");
	expect_bad_code("630f9ffe00"); /* more than 4 bytes */
	expect_bad_code("zz");         /* not hex */
	expect_bad_code("630f9fzz");
	expect_bad_code(" 0a");
	expect_bad_code("+0a");
	{
		uint8_t code[1];
		int len;
		CHECK(ego_parse_access_code("0a", NULL, &len) < 0);
		CHECK(ego_parse_access_code("0a", code, NULL) < 0);
	}
	printf("ego options: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
