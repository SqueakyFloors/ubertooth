/*
 * Tests for the shared option-value parsers and the one argument rule in
 * host/ubertooth-tools/src/opt_parse.c.
 */
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "opt_parse.h"

static int checks;
static int failures;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
		failures++; \
	} \
} while (0)

/* Every refused form, in one list: none of these is a value. */
static const char *const not_a_number[] = {
	"", " ", "\t5", "\n5", "+5", "5x", "x5", "abc", "5 ", "1.5", "0x10",
	"--5", "5-", NULL,
};

static void test_decimal(void)
{
	int value;
	size_t i;

	value = -1;
	CHECK(opt_parse_int("0", 0, 10, &value) == 0 && value == 0);
	CHECK(opt_parse_int("10", 0, 10, &value) == 0 && value == 10);
	CHECK(opt_parse_int("007", 0, 10, &value) == 0 && value == 7);
	CHECK(opt_parse_int("-128", -128, 127, &value) == 0 && value == -128);

	/* refused, and *value is left alone */
	value = 4242;
	for (i = 0; not_a_number[i] != NULL; i++)
		CHECK(opt_parse_int(not_a_number[i], 0, 10, &value) < 0);
	CHECK(opt_parse_int("-1", 0, 10, &value) < 0);   /* a sign, out of range */
	CHECK(opt_parse_int("11", 0, 10, &value) < 0);   /* above max */
	CHECK(opt_parse_int("99999999999999999999", 0, INT_MAX, &value) < 0);
	CHECK(opt_parse_int(NULL, 0, 10, &value) < 0);
	CHECK(opt_parse_int("5", 0, 10, NULL) < 0);
	CHECK(value == 4242);
}

static void test_decimal_diagnostic(void)
{
	int value = 4242;

	/* opt_require_int() is opt_parse_int() plus the message; the message
	 * itself is checked by the per-tool CLI tests. */
	CHECK(opt_require_int("2440", 2402, 2480, "frequency", " MHz",
	                      &value) == 0 && value == 2440);
	fprintf(stderr, "(one expected diagnostic follows)\n");
	CHECK(opt_require_int("abc", 2402, 2480, "frequency", " MHz", &value) < 0);
	CHECK(value == 2440);
}

static void test_hex(void)
{
	static const char *const not_hex[] = {
		"", " ", "\tf", "\nf", "+f", "fg", "gf", "zzzz", "f ", "1.5",
		"0xff", "--f", "f-", NULL,
	};
	uint32_t value = 0xdeadbeef;
	size_t i;

	CHECK(opt_parse_hex("ff", 2, 2, &value) == 0 && value == 0xff);
	CHECK(opt_parse_hex("FF", 2, 2, &value) == 0 && value == 0xff);
	CHECK(opt_parse_hex("0", 1, 8, &value) == 0 && value == 0);
	CHECK(opt_parse_hex("ffffffff", 1, 8, &value) == 0 && value == 0xffffffffu);

	/* refused, and *value is left alone. "abc" is hex, so the decimal
	 * list above does not apply here. */
	value = 0xdeadbeef;
	for (i = 0; not_hex[i] != NULL; i++)
		CHECK(opt_parse_hex(not_hex[i], 1, 8, &value) < 0);
	CHECK(opt_parse_hex("f", 2, 2, &value) < 0);     /* too few digits */
	CHECK(opt_parse_hex("fff", 2, 2, &value) < 0);   /* too many digits */
	CHECK(opt_parse_hex("-ff", 1, 8, &value) < 0);
	CHECK(opt_parse_hex(" ff", 1, 8, &value) < 0);
	CHECK(opt_parse_hex("ffg", 1, 8, &value) < 0);
	CHECK(opt_parse_hex(NULL, 1, 8, &value) < 0);
	CHECK(opt_parse_hex("ff", 1, 8, NULL) < 0);
	CHECK(value == 0xdeadbeefu);
}

static void test_lap_and_uap(void)
{
	uint32_t lap = 0xdeadbeef;
	uint8_t uap = 0x5a;

	CHECK(opt_parse_lap("446688", &lap) == 0 && lap == 0x446688);
	CHECK(opt_parse_lap("000000", &lap) == 0 && lap == 0);
	CHECK(opt_parse_uap("22", &uap) == 0 && uap == 0x22);
	CHECK(opt_parse_uap("ff", &uap) == 0 && uap == 0xff);

	/* exactly 6 and exactly 2 digits; refused values are left alone */
	lap = 0xdeadbeef;
	uap = 0x5a;
	CHECK(opt_parse_lap("44668", &lap) < 0);
	CHECK(opt_parse_lap("4466889", &lap) < 0);
	CHECK(opt_parse_lap("44668g", &lap) < 0);
	CHECK(opt_parse_lap("", &lap) < 0);
	CHECK(opt_parse_lap(NULL, &lap) < 0);
	CHECK(opt_parse_uap("2", &uap) < 0);
	CHECK(opt_parse_uap("222", &uap) < 0);
	CHECK(opt_parse_uap("2g", &uap) < 0);
	CHECK(opt_parse_uap("", &uap) < 0);
	CHECK(opt_parse_uap(NULL, &uap) < 0);
	CHECK(opt_parse_uap("22", NULL) < 0);
	CHECK(lap == 0xdeadbeefu);
	CHECK(uap == 0x5a);
}

/* These cases came from ubertooth-btle's own -a tests (#171). */
static void test_access_address(void)
{
	uint32_t aa = 0xdeadbeef;

	CHECK(opt_parse_access_address("8e89bed6", &aa) == 0 && aa == 0x8e89bed6u);
	CHECK(opt_parse_access_address("8E89BED6", &aa) == 0 && aa == 0x8e89bed6u);
	/* fewer than 8 digits means leading zeros */
	CHECK(opt_parse_access_address("12345", &aa) == 0 && aa == 0x00012345u);
	CHECK(opt_parse_access_address("0", &aa) == 0 && aa == 0);

	/* refused, and *address is left alone */
	aa = 0xdeadbeef;
	CHECK(opt_parse_access_address("zzzz", &aa) < 0);
	CHECK(opt_parse_access_address("", &aa) < 0);
	CHECK(opt_parse_access_address("123456789", &aa) < 0);
	CHECK(opt_parse_access_address("8e89bed6x", &aa) < 0);
	CHECK(opt_parse_access_address("0x8e89bed6", &aa) < 0);
	CHECK(opt_parse_access_address("-1", &aa) < 0);
	CHECK(opt_parse_access_address(" 1", &aa) < 0);
	CHECK(opt_parse_access_address(NULL, &aa) < 0);
	CHECK(aa == 0xdeadbeefu);
}

static void test_bd_addr(void)
{
	const uint8_t expected[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	const uint8_t untouched[] = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
	uint8_t address[6];
	static const char *const refused[] = {
		"",
		"11:22",                   /* too short */
		"11:22:33:44:55:66:77",    /* too long */
		"11:22:33:44:55:6",        /* one digit short */
		"11:22:33:44:55:GG",       /* not hex */
		"11-22-33-44-55-66",       /* wrong separator */
		"11:22:33:44:5566:",       /* separators in the wrong places */
		"112233445566",
		" 1:22:33:44:55:66",
		"-1:22:33:44:55:66",
		NULL,
	};
	size_t i;

	memset(address, 0, sizeof(address));
	CHECK(opt_parse_bd_addr("11:22:33:44:55:66", address) == 0);
	CHECK(memcmp(address, expected, sizeof(address)) == 0);
	CHECK(opt_parse_bd_addr("AA:bB:cC:dD:eE:fF", address) == 0);
	CHECK(address[0] == 0xaa && address[5] == 0xff);

	/* refused, and the result is left alone */
	memcpy(address, untouched, sizeof(address));
	for (i = 0; refused[i] != NULL; i++)
		CHECK(opt_parse_bd_addr(refused[i], address) < 0);
	CHECK(opt_parse_bd_addr(NULL, address) < 0);
	CHECK(opt_parse_bd_addr("11:22:33:44:55:66", NULL) < 0);
	CHECK(memcmp(address, untouched, sizeof(address)) == 0);
}

static void test_value_checks(void)
{
	CHECK(opt_value_decimal("0"));
	CHECK(opt_value_decimal("2440"));
	CHECK(!opt_value_decimal("-90"));
	CHECK(!opt_value_decimal("24x"));
	CHECK(!opt_value_decimal(""));
	CHECK(!opt_value_decimal(NULL));

	CHECK(opt_value_signed_decimal("-90"));
	CHECK(opt_value_signed_decimal("90"));
	CHECK(!opt_value_signed_decimal("-"));
	CHECK(!opt_value_signed_decimal("-b"));
	CHECK(!opt_value_signed_decimal(NULL));

	CHECK(opt_value_not_option("8e89bed6"));
	CHECK(opt_value_not_option("zzzz"));
	CHECK(!opt_value_not_option("-"));
	CHECK(!opt_value_not_option("-f"));
	CHECK(!opt_value_not_option(""));
	CHECK(!opt_value_not_option(NULL));
}

/* opt_separate_value() and opt_check_no_operands() read getopt's optind. */
static void test_next_word_and_operands(void)
{
	char *argv[] = { (char *)"tool", (char *)"2440", (char *)"-b", NULL };
	int argc = 3;

	optind = 1;
	CHECK(opt_separate_value(argc, argv, opt_value_decimal) == argv[1]);
	CHECK(optind == 2);
	/* the following option is not a value, and optind does not move */
	CHECK(opt_separate_value(argc, argv, opt_value_decimal) == NULL);
	CHECK(optind == 2);
	/* nothing left to take */
	optind = 3;
	CHECK(opt_separate_value(argc, argv, opt_value_decimal) == NULL);
	CHECK(optind == 3);
	CHECK(opt_separate_value(argc, argv, NULL) == NULL);

	optind = 3;
	CHECK(opt_check_no_operands(argc, argv) == 0);
	optind = 1;
	fprintf(stderr, "(one expected diagnostic follows)\n");
	CHECK(opt_check_no_operands(argc, argv) < 0);
}

int main(void)
{
	test_decimal();
	test_decimal_diagnostic();
	test_hex();
	test_lap_and_uap();
	test_access_address();
	test_bd_addr();
	test_value_checks();
	test_next_word_and_operands();
	printf("opt_parse: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
