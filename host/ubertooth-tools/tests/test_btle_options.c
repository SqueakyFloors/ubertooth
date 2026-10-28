#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "btle_options.h"
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

static void test_clear_target(void)
{
	char text[] = "none";
	uint8_t address[OPT_BD_ADDR_BYTES];
	uint8_t mask = 17;

	memset(address, 0xaa, sizeof(address));
	CHECK(convert_mac_address(text, "target", address, &mask) == 1);
	CHECK(mask == 0);
	CHECK(memcmp(address, "\0\0\0\0\0\0", sizeof(address)) == 0);
}

static void test_full_and_masked_targets(void)
{
	char full[] = "11:22:33:44:55:66";
	char masked[] = "00:1A:7D:00:00:00/24";
	uint8_t address[OPT_BD_ADDR_BYTES] = {0};
	uint8_t mask = 0;
	const uint8_t expected_full[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	const uint8_t expected_masked[] = {0x00, 0x1a, 0x7d, 0, 0, 0};

	CHECK(convert_mac_address(full, "target", address, &mask) == 1);
	CHECK(mask == 48);
	CHECK(memcmp(address, expected_full, sizeof(address)) == 0);
	CHECK(convert_mac_address(masked, "target", address, &mask) == 1);
	CHECK(mask == 24);
	CHECK(memcmp(address, expected_masked, sizeof(address)) == 0);
}

static void test_invalid_targets(void)
{
	char short_address[] = "11:22";
	char invalid_char[] = "11:22:33:44:55:GG";
	char invalid_mask[] = "11:22:33:44:55:66/49";
	uint8_t address[OPT_BD_ADDR_BYTES] = {0};
	uint8_t mask = 0;

	CHECK(convert_mac_address(NULL, "target", address, &mask) == 0);
	CHECK(convert_mac_address(short_address, "target", address, &mask) == 0);
	CHECK(convert_mac_address(invalid_char, "target", address, &mask) == 0);
	CHECK(convert_mac_address(invalid_mask, "target", address, &mask) == 0);
}

/* A faux slave's address is one whole address: the two spellings a target can
 * use are refused here, because there is nothing to narrow or clear. */
static void test_plain_addresses(void)
{
	uint8_t address[OPT_BD_ADDR_BYTES] = {0};
	const uint8_t expected[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

	CHECK(convert_plain_mac_address("11:22:33:44:55:66", "faux slave",
	                                address) == 1);
	CHECK(memcmp(address, expected, sizeof(address)) == 0);

	CHECK(convert_plain_mac_address("none", "faux slave", address) == 0);
	CHECK(convert_plain_mac_address("11:22:33:44:55:66/24", "faux slave",
	                                address) == 0);
	CHECK(convert_plain_mac_address("11:22", "faux slave", address) == 0);
	CHECK(convert_plain_mac_address(NULL, "faux slave", address) == 0);
	/* a refused value leaves the address it was given alone */
	CHECK(memcmp(address, expected, sizeof(address)) == 0);
}

int main(void)
{
	/* The access address parser moved to opt_parse.c; its cases live in
	 * test_opt_parse.c. */
	test_clear_target();
	test_full_and_masked_targets();
	test_invalid_targets();
	test_plain_addresses();
	printf("btle options: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
