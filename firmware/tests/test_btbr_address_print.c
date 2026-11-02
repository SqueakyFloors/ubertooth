/*
 * Every Bluetooth address the BR/EDR image prints is twelve hex digits
 * (firmware/btbr/src/btphy/btphy.c, monitor1_state.c and page_scan_state.c).
 *
 * An address is six bytes, and ubertooth-btbr's prompt takes nothing else, so
 * a printed address that lost its leading zeros is one the user cannot copy
 * back: "bdaddr 00:11:22:33:44:55" once echoed "Set bdaddr 1122334455", and
 * the "fhs: ba=" line of a page_scan or monitor session dropped them the same
 * way from the address it read off the air.
 *
 * All three lines live in hardware files that cannot run here, so they are
 * read as text, the way test_btbr_arithmetic.c reads the scan and receive
 * tasks. What a reader of the console sees was measured on a board instead.
 */
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

/* The conversion an address print has to use: zero-padded to the twelve
 * digits of a six-byte address, for a value passed as a long long. */
#define PADDED "%012llx"

/* 1 when `path` has a cprintf() line whose format writes an address right
 * after `marker`, and that conversion is the padded one. 0 when the line is
 * there and asks for something else, -1 when no line carries the marker,
 * which the caller reports rather than passing on nothing. */
static int address_after_marker_is_padded(const char *path, const char *marker)
{
	char line[SCAN_LINE_MAX];
	FILE *f = fopen(path, "r");
	int in_block = 0;
	int found = -1;

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *at;

		strip_comments(line, &in_block);
		if (!calls_identifier(line, "cprintf"))
			continue;
		at = strstr(line, marker);
		if (at == NULL)
			continue;
		/* The marker ends in the '%' the conversion starts with, so
		 * the comparison begins on that '%'. */
		found = strncmp(at + strlen(marker) - 1, PADDED,
		                strlen(PADDED)) == 0;
		break;
	}
	fclose(f);
	return found;
}

MU_TEST(every_address_the_btbr_firmware_prints_asks_for_the_padding)
{
	static const struct {
		const char *path;
		const char *marker;
	} prints[] = {
		{ FW_BTBR_DIR "/src/btphy/btphy.c",           "bdaddr %" },
		{ FW_BTBR_DIR "/src/btphy/monitor1_state.c",  "ba=%" },
		{ FW_BTBR_DIR "/src/btphy/page_scan_state.c", "ba=%" },
	};
	size_t i;

	for (i = 0; i < sizeof(prints) / sizeof(prints[0]); i++) {
		int padded = address_after_marker_is_padded(prints[i].path,
		                                            prints[i].marker);

		mu_assert_fatal(padded != -1, "a cprintf() printing an address");
		mu_assert(padded == 1, "the address conversion is " PADDED);
	}
}

/* The syncword btphy.c prints beside the address is sixteen digits wide, is
 * derived from the address, and is not an input to anything, so a plain
 * conversion is right for it. This says the two are told apart rather than
 * padded alike, and it is the line that would fail if a later sweep widened
 * every conversion in the file. */
MU_TEST(the_syncword_beside_the_address_is_not_padded_like_one)
{
	mu_assert_int_eq(0, address_after_marker_is_padded(
		FW_BTBR_DIR "/src/btphy/btphy.c", "sw %"));
}

int main(void)
{
	MU_RUN(every_address_the_btbr_firmware_prints_asks_for_the_padding);
	MU_RUN(the_syncword_beside_the_address_is_not_padded_like_one);
	return mu_report();
}
