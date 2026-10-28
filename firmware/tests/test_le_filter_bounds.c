/*
 * How much of an advertising capture the target filter in
 * firmware/bluetooth_rxtx/le_phy.c needs before it compares the address in it.
 *
 * A capture's size counts three things: the two-byte PDU header, the payload,
 * and the three CRC bytes the radio hands over behind the payload. A length
 * check that leaves the CRC out of that sum accepts a payload three bytes
 * shorter than it meant to, and the filter then reads three payload bytes and
 * three CRC bytes as if they were a six-byte address. With a target set, a
 * runt advertising PDU was kept or dropped on bytes that are not an address.
 *
 * filter_match() is static inside le_phy.c, which is hardware code and cannot
 * run here, so this test does what test_mode_state_reset.c does: it models the
 * decision, runs the cases against the model, and then scans le_phy.c to check
 * the firmware's bounds are the modelled ones. A firmware bound that drifts
 * from the model fails the scan rather than passing unseen.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

#ifndef FW_RXTX_DIR
#error "FW_RXTX_DIR must name firmware/bluetooth_rxtx"
#endif

/* The parts of a capture, and the shortest capture that carries the addresses
 * the filter compares. The scan below checks le_phy.c defines the same. */
#define LE_HEADER_LEN 2
#define LE_ADDR_LEN   6
#define LE_CRC_LEN    3
#define LE_ONE_ADDR_SIZE (LE_HEADER_LEN + LE_ADDR_LEN + LE_CRC_LEN)
#define LE_TWO_ADDR_SIZE (LE_HEADER_LEN + 2 * LE_ADDR_LEN + LE_CRC_LEN)

/* PDU types, from the low four bits of the first header byte. */
#define ADV_IND         0x00
#define ADV_DIRECT_IND  0x01
#define ADV_NONCONN_IND 0x02
#define SCAN_REQ        0x03
#define SCAN_RSP        0x04
#define CONNECT_REQ     0x05
#define ADV_SCAN_IND    0x06

/* Channel indices: 0-36 carry a connection, 37-39 carry advertising. */
#define DATA_CHANNEL_INDEX 0
#define ADV_CHANNEL_INDEX  37

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The target the filter compares against, with a mask that makes every byte
 * matter, and one address that is not the target. */
static const uint8_t target[LE_ADDR_LEN]      = { 1, 2, 3, 4, 5, 6 };
static const uint8_t target_mask[LE_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff,
                                                  0xff, 0xff };
static const uint8_t other[LE_ADDR_LEN]       = { 9, 9, 9, 9, 9, 9 };

/* The model: filter_match() from le_phy.c with the corrected bounds. It takes
 * a channel index rather than a physical channel, so none of the radio is
 * needed here. */
static int bd_addr_cmp(const uint8_t *bd_addr)
{
	unsigned i;

	for (i = 0; i < LE_ADDR_LEN; ++i)
		if ((bd_addr[i] & target_mask[i]) != target[i])
			return 0;
	return 1;
}

static int filter_match(int target_set, unsigned channel_index,
                        const uint8_t *data, unsigned size)
{
	if (!target_set)
		return 1;

	/* every data channel packet is allowed through */
	if (channel_index < 37)
		return 1;

	switch (data[0] & 0xf) {
	case ADV_IND:
	case ADV_NONCONN_IND:
	case ADV_SCAN_IND:
	case SCAN_RSP:
		if (size < LE_ONE_ADDR_SIZE)
			return 0;
		return bd_addr_cmp(&data[LE_HEADER_LEN]);

	case ADV_DIRECT_IND:
	case SCAN_REQ:
	case CONNECT_REQ:
		if (size < LE_TWO_ADDR_SIZE)
			return 0;
		return bd_addr_cmp(&data[LE_HEADER_LEN]) ||
		       bd_addr_cmp(&data[LE_HEADER_LEN + LE_ADDR_LEN]);

	default:
		return 0;
	}
}

/*
 * Build a capture the way the receive path does: two header bytes, pdu_len
 * payload bytes, then three CRC bytes. Returns the size the firmware would
 * record for it. The buffer is longer than any capture built here, so a case
 * can write an address past the capture's end and see whether the filter read
 * bytes it should not have.
 */
#define CAPTURE_BUF 64

static unsigned build(uint8_t *data, uint8_t type, unsigned pdu_len)
{
	memset(data, 0, CAPTURE_BUF);
	data[0] = type;
	data[1] = (uint8_t) pdu_len;
	return LE_HEADER_LEN + pdu_len + LE_CRC_LEN;
}

static void put_addr(uint8_t *data, unsigned offset, const uint8_t *addr)
{
	memcpy(data + offset, addr, LE_ADDR_LEN);
}

/*
 * The bug. Six payload bytes are needed to hold the address, so a payload of
 * three plus the three CRC bytes used to pass a bound of eight, and the filter
 * compared payload and CRC bytes as if they were an address. Every one of
 * these captures carries the target's bytes where the address would be, so a
 * comparison that ran would report a match: the old bound kept them.
 */
MU_TEST(a_payload_too_short_for_an_address_is_not_compared)
{
	uint8_t data[CAPTURE_BUF];
	unsigned pdu_len;

	for (pdu_len = 0; pdu_len < LE_ADDR_LEN; ++pdu_len) {
		unsigned size = build(data, ADV_IND, pdu_len);
		char msg[128];

		put_addr(data, LE_HEADER_LEN, target);
		snprintf(msg, sizeof(msg),
		         "an ADV_IND of %u payload bytes (size %u) was compared "
		         "against the target", pdu_len, size);
		mu_assert(!filter_match(1, ADV_CHANNEL_INDEX, data, size), msg);
	}
}

/*
 * The other half of the bound: a payload exactly long enough is still
 * compared, and matches or not on the address it carries. Six payload bytes
 * with the header and the CRC is a capture of eleven.
 */
MU_TEST(a_payload_exactly_long_enough_is_still_compared)
{
	static const uint8_t type[] = { ADV_IND, ADV_NONCONN_IND, ADV_SCAN_IND,
	                                SCAN_RSP };
	uint8_t data[CAPTURE_BUF];
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(type); ++i) {
		unsigned size = build(data, type[i], LE_ADDR_LEN);
		char msg[128];

		mu_assert_int_eq(LE_ONE_ADDR_SIZE, (int) size);

		put_addr(data, LE_HEADER_LEN, target);
		snprintf(msg, sizeof(msg),
		         "a PDU of type 0x%02x carrying the target was not matched",
		         type[i]);
		mu_assert(filter_match(1, ADV_CHANNEL_INDEX, data, size), msg);

		put_addr(data, LE_HEADER_LEN, other);
		snprintf(msg, sizeof(msg),
		         "a PDU of type 0x%02x carrying another address was matched",
		         type[i]);
		mu_assert(!filter_match(1, ADV_CHANNEL_INDEX, data, size), msg);
	}
}

/*
 * A type that carries two addresses needs room for both. Eleven payload bytes
 * hold the first address and five bytes of the second, a capture of sixteen:
 * one short of the seventeen two addresses need.
 */
MU_TEST(a_two_address_type_needs_room_for_both)
{
	static const uint8_t type[] = { ADV_DIRECT_IND, SCAN_REQ, CONNECT_REQ };
	uint8_t data[CAPTURE_BUF];
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(type); ++i) {
		unsigned size = build(data, type[i], 2 * LE_ADDR_LEN - 1);
		char msg[128];

		/* One byte short, with the target in both address positions: a
		 * bound that counted only the header and the addresses would
		 * compare this and match it. */
		mu_assert_int_eq(LE_TWO_ADDR_SIZE - 1, (int) size);
		put_addr(data, LE_HEADER_LEN, target);
		put_addr(data, LE_HEADER_LEN + LE_ADDR_LEN, target);
		snprintf(msg, sizeof(msg),
		         "a PDU of type 0x%02x one byte short of two addresses was "
		         "compared against the target", type[i]);
		mu_assert(!filter_match(1, ADV_CHANNEL_INDEX, data, size), msg);

		/* Long enough: either address being the target is a match. */
		size = build(data, type[i], 2 * LE_ADDR_LEN);
		mu_assert_int_eq(LE_TWO_ADDR_SIZE, (int) size);

		put_addr(data, LE_HEADER_LEN, target);
		put_addr(data, LE_HEADER_LEN + LE_ADDR_LEN, other);
		snprintf(msg, sizeof(msg),
		         "a PDU of type 0x%02x with the target first was not matched",
		         type[i]);
		mu_assert(filter_match(1, ADV_CHANNEL_INDEX, data, size), msg);

		put_addr(data, LE_HEADER_LEN, other);
		put_addr(data, LE_HEADER_LEN + LE_ADDR_LEN, target);
		snprintf(msg, sizeof(msg),
		         "a PDU of type 0x%02x with the target second was not matched",
		         type[i]);
		mu_assert(filter_match(1, ADV_CHANNEL_INDEX, data, size), msg);

		put_addr(data, LE_HEADER_LEN, other);
		put_addr(data, LE_HEADER_LEN + LE_ADDR_LEN, other);
		snprintf(msg, sizeof(msg),
		         "a PDU of type 0x%02x with neither address the target was "
		         "matched", type[i]);
		mu_assert(!filter_match(1, ADV_CHANNEL_INDEX, data, size), msg);
	}
}

/*
 * What the filter did otherwise, unchanged: with no target every capture
 * passes, including a runt one; every data channel packet passes; and an
 * advertising type the filter does not know carries no address it can compare,
 * so it does not match.
 */
MU_TEST(the_rest_of_the_filter_is_unchanged)
{
	uint8_t data[CAPTURE_BUF];
	unsigned size = build(data, ADV_IND, 0);

	mu_assert(filter_match(0, ADV_CHANNEL_INDEX, data, size),
	          "a capture was filtered out with no target set");
	mu_assert(filter_match(1, DATA_CHANNEL_INDEX, data, size),
	          "a data channel packet was filtered out");

	size = build(data, 0x07, 2 * LE_ADDR_LEN);
	put_addr(data, LE_HEADER_LEN, target);
	mu_assert(!filter_match(1, ADV_CHANNEL_INDEX, data, size),
	          "an advertising type the filter does not know was matched");
}

/* The whole file, comments and string literals removed, spaces collapsed. */
static char source[400000];

static void load_le_phy(void)
{
	char path[512];
	char line[4096];
	char clean[4096];
	FILE *f;
	int in_block = 0;
	size_t used = 0;

	snprintf(path, sizeof(path), "%s/le_phy.c", FW_RXTX_DIR);
	f = fopen(path, "r");
	if (f == NULL) {
		/* The scan is what ties the model to the firmware, so a file that
		 * cannot be read has to stop the test, not shorten it. */
		fprintf(stderr, "cannot open %s\n", path);
		exit(1);
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		strip_string_literals(line);
		collapse_spaces(line, clean, sizeof(clean));
		if (clean[0] == '\0')
			continue;
		used += (size_t) snprintf(source + used, sizeof(source) - used,
		                          "%s ", clean);
		if (used >= sizeof(source) - 1)
			break;
	}
	fclose(f);
}

/*
 * The firmware's bounds are the model's. The constants are checked by their
 * definitions and the guards by the names they use, so a guard rewritten as a
 * bare sum fails here even when the sum is right: what the numbers mean is
 * part of what this pins.
 */
MU_TEST(the_firmware_uses_the_modelled_bounds)
{
	/* Each definition with the space that follows it, so a value of 3 is not
	 * found inside one of 30. load_le_phy() puts a space after every line,
	 * so the last definition on a line has one too. */
	mu_assert(strstr(source, "#define LE_HEADER_LEN 2 ") != NULL,
	          "le_phy.c does not define LE_HEADER_LEN as 2");
	mu_assert(strstr(source, "#define LE_ADDR_LEN 6 ") != NULL,
	          "le_phy.c does not define LE_ADDR_LEN as 6");
	mu_assert(strstr(source, "#define LE_CRC_LEN 3 ") != NULL,
	          "le_phy.c does not define LE_CRC_LEN as 3");
	mu_assert(strstr(source,
	                 "#define LE_ONE_ADDR_SIZE (LE_HEADER_LEN + LE_ADDR_LEN +"
	                 " LE_CRC_LEN) ") != NULL,
	          "le_phy.c's one-address bound is not header + address + CRC");
	mu_assert(strstr(source,
	                 "#define LE_TWO_ADDR_SIZE (LE_HEADER_LEN + 2 *"
	                 " LE_ADDR_LEN + LE_CRC_LEN) ") != NULL,
	          "le_phy.c's two-address bound is not header + two addresses "
	          "+ CRC");

	/* Each bound with the types it guards and the comparison it admits, as
	 * one run of text: checking the guards on their own would still pass if
	 * the two were swapped between the case groups. The type bytes are the
	 * ones the model switches on, in the source's order. */
	mu_assert(strstr(source,
	                 "case 0x00: case 0x02: case 0x06: case 0x04:"
	                 " if (buf->size < LE_ONE_ADDR_SIZE) return 0;"
	                 " return bd_addr_cmp(&buf->data[LE_HEADER_LEN]);")
	          != NULL,
	          "the one-address types do not refuse a capture shorter than "
	          "LE_ONE_ADDR_SIZE and then compare the address behind the "
	          "header");
	mu_assert(strstr(source,
	                 "case 0x01: case 0x03: case 0x05:"
	                 " if (buf->size < LE_TWO_ADDR_SIZE) return 0;"
	                 " return bd_addr_cmp(&buf->data[LE_HEADER_LEN]) ||"
	                 " bd_addr_cmp(&buf->data[LE_HEADER_LEN + LE_ADDR_LEN]);")
	          != NULL,
	          "the two-address types do not refuse a capture shorter than "
	          "LE_TWO_ADDR_SIZE and then compare both addresses");
	mu_assert(strstr(source, "for (i = 0; i < LE_ADDR_LEN; ++i)") != NULL,
	          "an address is not compared over LE_ADDR_LEN bytes");

	/* And a capture's size still counts all three parts, which is the whole
	 * reason the CRC belongs in the bounds. Either spelling: writing that
	 * sum with these constants is the natural follow-up and is no change to
	 * what it counts. */
	mu_assert(is_assignment_of(source, "current_rxbuf->size",
	                           "length + 2 + 3") ||
	          is_assignment_of(source, "current_rxbuf->size",
	                           "length + LE_HEADER_LEN + LE_CRC_LEN"),
	          "a capture's size is no longer the payload plus header and CRC");
}

int main(void)
{
	printf("LE target filter length bound tests\n");
	load_le_phy();
	MU_RUN(a_payload_too_short_for_an_address_is_not_compared);
	MU_RUN(a_payload_exactly_long_enough_is_still_compared);
	MU_RUN(a_two_address_type_needs_room_for_both);
	MU_RUN(the_rest_of_the_filter_is_unchanged);
	MU_RUN(the_firmware_uses_the_modelled_bounds);
	return mu_report();
}
