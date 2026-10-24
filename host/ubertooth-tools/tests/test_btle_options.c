#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "btle_options.h"

static int checks;
static int failures;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
		failures++; \
	} \
} while (0)

static void test_clear_target(void)
{
	char text[] = "none";
	uint8_t address[6];
	uint8_t mask = 17;

	memset(address, 0xaa, sizeof(address));
	CHECK(convert_mac_address(text, address, &mask) == 1);
	CHECK(mask == 0);
	CHECK(memcmp(address, "\0\0\0\0\0\0", sizeof(address)) == 0);
}

static void test_full_and_masked_targets(void)
{
	char full[] = "11:22:33:44:55:66";
	char masked[] = "00:1A:7D:00:00:00/24";
	uint8_t address[6] = {0};
	uint8_t mask = 0;
	const uint8_t expected_full[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	const uint8_t expected_masked[] = {0x00, 0x1a, 0x7d, 0, 0, 0};

	CHECK(convert_mac_address(full, address, &mask) == 1);
	CHECK(mask == 48);
	CHECK(memcmp(address, expected_full, sizeof(address)) == 0);
	CHECK(convert_mac_address(masked, address, &mask) == 1);
	CHECK(mask == 24);
	CHECK(memcmp(address, expected_masked, sizeof(address)) == 0);
}

static void test_invalid_targets(void)
{
	char short_address[] = "11:22";
	char invalid_char[] = "11:22:33:44:55:GG";
	char invalid_mask[] = "11:22:33:44:55:66/49";
	uint8_t address[6] = {0};
	uint8_t mask = 0;

	CHECK(convert_mac_address(NULL, address, &mask) == 0);
	CHECK(convert_mac_address(short_address, address, &mask) == 0);
	CHECK(convert_mac_address(invalid_char, address, &mask) == 0);
	CHECK(convert_mac_address(invalid_mask, address, &mask) == 0);
}

static void test_access_address_parsing(void)
{
	uint32_t aa = 0xdeadbeef;

	CHECK(parse_access_address("8e89bed6", &aa) == 1 && aa == 0x8e89bed6u);
	CHECK(parse_access_address("8E89BED6", &aa) == 1 && aa == 0x8e89bed6u);
	/* fewer than 8 digits means leading zeros */
	CHECK(parse_access_address("12345", &aa) == 1 && aa == 0x00012345u);
	CHECK(parse_access_address("0", &aa) == 1 && aa == 0);

	/* refused, and *address is left alone */
	aa = 0xdeadbeef;
	CHECK(parse_access_address("zzzz", &aa) == 0);
	CHECK(parse_access_address("", &aa) == 0);
	CHECK(parse_access_address("123456789", &aa) == 0);
	CHECK(parse_access_address("8e89bed6x", &aa) == 0);
	CHECK(parse_access_address("0x8e89bed6", &aa) == 0);
	CHECK(parse_access_address("-1", &aa) == 0);
	CHECK(parse_access_address(" 1", &aa) == 0);
	CHECK(parse_access_address(NULL, &aa) == 0);
	CHECK(aa == 0xdeadbeefu);
}

int main(void)
{
	test_clear_target();
	test_full_and_masked_targets();
	test_invalid_targets();
	test_access_address_parsing();
	printf("btle options: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
