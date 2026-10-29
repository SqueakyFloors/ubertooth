/*
 * Unit tests for BR/EDR connection-state hop selection in
 * firmware/btbr/src/btphy/hop.c (hop_init(), hop_cfg_afh(), hop_basic()).
 *
 * The expected channels were computed independently, outside this code base,
 * by a direct transcription of the Bluetooth Core specification Vol 2 Part B
 * section 2.6: the basic kernel (Figure 2.16, Table 2.2 connection-state
 * inputs), the AFH re-mapping function (2.6.3.1) and the AFH same channel
 * mechanism (2.6.1).  That transcription reproduces every vector already
 * pinned in test_bluetooth_hop.c for the other firmware image, which is an
 * independent check on it.  The numbers are fixed here so this test does not
 * use the algorithm under test as its own oracle.
 *
 * AFH rules being checked:
 *  - the basic sequence is selected over all 79 channels first; a channel the
 *    map marks used is kept unchanged, and only an unused one is replaced from
 *    the AFH mapping table;
 *  - a map with fewer than the spec minimum of 20 channels falls back to basic
 *    hopping;
 *  - with AFH, the peripheral slot (CLK1 = 1) reuses the central slot channel.
 *
 * hop.c reaches PERM5 through the pre-computed table in btbr's perm5.c, which
 * is linked here and checked against the specification's butterfly network
 * over every input, so the expected values do not come from firmware code.
 * The one thing the test shares with the firmware is the order the butterfly
 * stages run in, which only the specification's figure shows; see PERM5 below.
 *
 * On top of those, the specification's own published hop sample data is run
 * through this image: 7680 connection-state vectors through hop_basic(), and
 * 192 page scan and inquiry scan vectors through hop_scan().  Both come from
 * Vol 2, Part G, Section 2, in spec_hop_vectors.h.  The hand-picked vectors
 * above stay because they name the rule each one is there for, and because
 * they cover addresses, clock values and channel maps Part G does not print.
 *
 * Part G prints the scan tables only where CLK1 is 0, so vectors derived from
 * them cover the rest: the published channel has to hold in every slot of its
 * 1.28-second interval, because the scan substates take neither CLK1 nor
 * the clock of the leg being tuned into the control word.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "spec_hop_vectors.h"
#include <ubtbr/hop.h>
#include <ubtbr/btphy.h>

/* Definitions the firmware image provides and hop.c reads. */
btphy_t btphy;

void cprintf(char *fmt, ...)
{
	(void)fmt;
}

void __attribute__((noreturn)) die(char *fmt, ...)
{
	va_list args;

	/*
	 * The code under test only calls this on a bug, so failing the test is
	 * the point. Print what it said, after the test names minunit has
	 * already written, so the two read in order.
	 */
	fflush(stdout);
	fputs("die: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	/*
	 * Exit rather than abort(): an aborted process gets a crash report,
	 * and on macOS a dialog with it.
	 */
	exit(1);
}

/*
 * PERM5 (2.6.2.3): butterfly i swaps the two bits of Z named below when
 * control signal P[i] is set.  The pairs are Table 2.1; P0-P8 come from input
 * D and P9-P13 from Y1 xor C.
 *
 * The spec arranges the 14 butterflies as the 7 stages of Figure 2.18, which
 * is where their order comes from: this applies P13 down to P0 one at a time,
 * equivalent because the two butterflies of a stage act on disjoint bits of Z.
 * Only the figure shows which stage comes first, so that one convention is
 * shared with the firmware's table rather than taken from the spec text; it is
 * corroborated by the vectors already pinned for the other firmware image.
 */
static const uint8_t BUTTERFLY[14][2] = {
	{0, 1}, {2, 3}, {1, 2}, {3, 4}, {0, 4}, {1, 3}, {0, 2},
	{3, 4}, {1, 4}, {0, 3}, {2, 4}, {1, 3}, {0, 3}, {1, 2},
};

/* Apply butterflies `hi` down to `lo`, each gated by bit i of p. */
static uint8_t perm5_butterflies(uint8_t z, uint16_t p, int hi, int lo)
{
	uint8_t bit[5], t;
	int i;

	for (i = 0; i < 5; i++)
		bit[i] = (z >> i) & 0x1;
	for (i = hi; i >= lo; i--) {
		if (p & (1u << i)) {
			t = bit[BUTTERFLY[i][0]];
			bit[BUTTERFLY[i][0]] = bit[BUTTERFLY[i][1]];
			bit[BUTTERFLY[i][1]] = t;
		}
	}
	z = 0;
	for (i = 0; i < 5; i++)
		z |= bit[i] << i;
	return z;
}

static uint8_t perm5_spec(uint8_t z, uint8_t p_high, uint16_t p_low)
{
	uint16_t p = (p_low & 0x1ff) | ((uint16_t)(p_high & 0x1f) << 9);

	return perm5_butterflies(z & 0x1f, p, 13, 0);
}

#define TEST_ADDRESS	UINT32_C(0x12345678)
/*
 * Second address whose E input is 127, the largest the spec allows.  With
 * Y2 = 32 and F up to 78 the kernel sum then exceeds 255, so these vectors
 * pin that the sum is not computed in a uint8_t.
 */
#define BIG_E_ADDRESS	UINT32_C(0xdeadbeef)

/* All 79 channels used; bit 79 of the last byte is reserved. */
static const uint8_t FULL_MAP[10] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
};
/*
 * Used channels: 22-48 and 72-78 (34 channels), i.e. the 2402-2423 MHz and
 * 2451-2473 MHz ranges avoided, as a host does around two busy Wi-Fi channels.
 */
static const uint8_t REDUCED_MAP[10] = {
	0x00, 0x00, 0xc0, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x7f
};
/* Channels 0-19: exactly the spec minimum of 20, where AFH still applies. */
static const uint8_t MIN_MAP[10] = {0xff, 0xff, 0x0f};
/* Channels 0-18: one short of it. */
static const uint8_t SMALL_MAP[10] = {0xff, 0xff, 0x07};

struct hop_vector {
	uint32_t clk;
	uint8_t basic;		/* AFH disabled */
	uint8_t full;		/* FULL_MAP */
	uint8_t reduced;	/* REDUCED_MAP */
};

/* Even and odd clk pairs, so each central slot is followed by its peripheral
 * slot.  Channel numbers, not MHz: the frequency is 2402 + channel MHz. */
static const struct hop_vector HOP_VECTORS[] = {
	{0x00000000, 60, 60, 47},
	{0x00000002, 37, 60, 47},
	{0x00000004, 64, 64, 75},
	{0x00000006, 53, 64, 75},
	{0x0000007e, 49, 50, 37},
	{0x00123456, 27, 42, 42},	/* used basic channel kept */
	{0x00123458,  6,  6, 73},
	{0x0012345a, 15,  6, 73},
	{0x07fffffc, 34, 34, 34},	/* used basic channel kept */
	{0x07fffffe, 70, 34, 34},	/* used basic channel kept */
	{0xfffffffc,  6,  6, 77},
	{0xfffffffe, 42,  6, 77},
	{0x00002000, 54, 54, 22},
	{0x00002002, 31, 54, 22},
	{0x00045678, 71, 71, 76},
	{0x00abcdec, 33, 33, 33},	/* used basic channel kept */
	{0x00abcdee, 34, 33, 33},	/* used basic channel kept */
	{0x03fffffc, 48, 48, 48},	/* used basic channel kept */
	{0x03fffffe,  5, 48, 48},	/* used basic channel kept */
	{0x089abcde, 32, 33, 33},	/* used basic channel kept */
	{0x00448d30, 47, 47, 47},	/* used basic channel kept */
	{0x0045b07c, 35, 35, 35},	/* used basic channel kept */
	{0x004c8444, 45, 45, 45},	/* used basic channel kept */
};
#define NUM_VECTORS (sizeof(HOP_VECTORS) / sizeof(HOP_VECTORS[0]))

struct clk_vector {
	uint32_t clk;
	uint8_t chan;
};

/* BIG_E_ADDRESS, AFH disabled.  The first four clk values are ones where
 * PERM5 + E + F + Y2 exceeds 255; the last two are controls below it. */
static const struct clk_vector BIG_E_VECTORS[] = {
	{0x00000776, 38},
	{0x00000777, 38},
	{0x000009d6, 38},
	{0x000009f6, 40},
	{0x00000000, 37},
	{0x00123456, 47},
};
#define NUM_BIG_E_VECTORS (sizeof(BIG_E_VECTORS) / sizeof(BIG_E_VECTORS[0]))

/*
 * TEST_ADDRESS under MIN_MAP.  AFH applies at exactly 20 used channels, so
 * these are the re-mapped channels, not the basic ones: ten of the twelve
 * differ from the basic column above.
 */
static const struct clk_vector MIN_MAP_VECTORS[] = {
	{0x00000000,  1},
	{0x00000002,  1},
	{0x00000004,  5},
	{0x00000006,  5},
	{0x0000007e, 10},
	{0x00123456,  7},
	{0x00123458,  6},
	{0x07fffffc, 14},
	{0xfffffffc,  6},
	{0x00002000,  9},
	{0x00045678, 12},
	{0x03fffffc, 19},
};
#define NUM_MIN_MAP_VECTORS (sizeof(MIN_MAP_VECTORS) / sizeof(MIN_MAP_VECTORS[0]))

static void set_afh(const uint8_t *map, uint8_t enabled)
{
	uint8_t buf[11];

	buf[0] = enabled;
	memcpy(buf + 1, map, 10);
	hop_cfg_afh(buf);
}

/* Apply the map the way btctl does: address first, then the AFH request. */
static void follow_setup(uint32_t address, const uint8_t *map)
{
	memset(&hop_state, 0, sizeof(hop_state));
	memset(&btphy, 0, sizeof(btphy));
	hop_init(address);
	if (map)
		set_afh(map, 1);
}

static int map_marks_used(const uint8_t *map, uint8_t chan)
{
	return chan < NUM_BREDR_CHANNELS && ((map[chan / 8] >> (chan % 8)) & 1);
}

/* The table the firmware image ships, and hop.h's indexing of it. */
MU_TEST(test_perm5_lookup_table_matches_spec)
{
	unsigned p, z;
	int bad = 0;

	for (p = 0; p < (1u << 14); p++) {
		for (z = 0; z < 32; z++) {
			if (perm5(z, p >> 9, p & 0x1ff) !=
					perm5_spec(z, p >> 9, p & 0x1ff))
				bad++;
		}
	}
	mu_assert_int_eq(0, bad);
}

MU_TEST(test_basic_bank_is_even_then_odd_channels)
{
	unsigned i;

	follow_setup(TEST_ADDRESS, NULL);
	for (i = 0; i < NUM_BREDR_CHANNELS; i++)
		mu_assert_int_eq((i * 2) % NUM_BREDR_CHANNELS,
				hop_state.basic_bank[i]);
}

MU_TEST(test_basic_hops_match_reference)
{
	size_t i;

	follow_setup(TEST_ADDRESS, NULL);
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].basic, hop_basic(HOP_VECTORS[i].clk));
}

MU_TEST(test_basic_hops_do_not_truncate_the_kernel_sum)
{
	size_t i;

	follow_setup(BIG_E_ADDRESS, NULL);
	mu_assert_int_eq(127, hop_state.E);
	for (i = 0; i < NUM_BIG_E_VECTORS; i++)
		mu_assert_int_eq(BIG_E_VECTORS[i].chan,
				hop_basic(BIG_E_VECTORS[i].clk));
}

MU_TEST(test_exactly_the_minimum_used_channels_still_adapts)
{
	size_t i;

	follow_setup(TEST_ADDRESS, MIN_MAP);
	mu_assert_int_eq(20, hop_state.afh_chan_count);
	for (i = 0; i < NUM_MIN_MAP_VECTORS; i++)
		mu_assert_int_eq(MIN_MAP_VECTORS[i].chan,
				hop_basic(MIN_MAP_VECTORS[i].clk));
}

MU_TEST(test_afh_mapping_table_is_used_channels_even_then_odd)
{
	static const uint8_t expected[] = {
		22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48,
		72, 74, 76, 78,
		23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 47,
		73, 75, 77,
	};
	unsigned i;

	follow_setup(TEST_ADDRESS, REDUCED_MAP);
	mu_assert_int_eq(sizeof(expected), hop_state.afh_chan_count);
	for (i = 0; i < sizeof(expected); i++)
		mu_assert_int_eq(expected[i], hop_state.afh_bank[i]);
	/* The basic table is not overwritten by the AFH table. */
	for (i = 0; i < NUM_BREDR_CHANNELS; i++)
		mu_assert_int_eq((i * 2) % NUM_BREDR_CHANNELS,
				hop_state.basic_bank[i]);
}

MU_TEST(test_full_map_hops_match_reference)
{
	size_t i;

	follow_setup(TEST_ADDRESS, FULL_MAP);
	mu_assert_int_eq(NUM_BREDR_CHANNELS, hop_state.afh_chan_count);
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].full, hop_basic(HOP_VECTORS[i].clk));
}

MU_TEST(test_reduced_map_hops_match_reference)
{
	size_t i;

	follow_setup(TEST_ADDRESS, REDUCED_MAP);
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].reduced, hop_basic(HOP_VECTORS[i].clk));
}

MU_TEST(test_too_few_used_channels_falls_back_to_basic)
{
	size_t i;

	follow_setup(TEST_ADDRESS, SMALL_MAP);
	mu_assert_int_eq(19, hop_state.afh_chan_count);
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].basic, hop_basic(HOP_VECTORS[i].clk));
}

MU_TEST(test_afh_hops_keep_used_channels_and_repeat_the_central_slot)
{
	uint32_t clk;
	int off_map = 0, slot_mismatch = 0, kept = 0, mismatched_kept = 0;

	follow_setup(TEST_ADDRESS, REDUCED_MAP);
	/* Every central slot across 2^20 frames, plus its peripheral slot. */
	for (clk = 0; clk < (UINT32_C(1) << 22); clk += 4) {
		uint8_t afh = hop_basic(clk);
		uint8_t peripheral = hop_basic(clk | 2);
		uint8_t basic;

		if (!map_marks_used(REDUCED_MAP, afh))
			off_map++;
		if (peripheral != afh)
			slot_mismatch++;
		/* Compare with the basic sequence for the same slot. */
		hop_state.afh_enabled = 0;
		basic = hop_basic(clk);
		hop_state.afh_enabled = 1;
		if (map_marks_used(REDUCED_MAP, basic)) {
			kept++;
			if (basic != afh)
				mismatched_kept++;
		}
	}
	mu_assert_int_eq(0, off_map);
	mu_assert_int_eq(0, slot_mismatch);
	mu_assert(kept > 0, "no used basic channels exercised");
	mu_assert_int_eq(0, mismatched_kept);
}

MU_TEST(test_clearing_afh_restores_basic_hopping)
{
	size_t i;

	follow_setup(TEST_ADDRESS, REDUCED_MAP);
	set_afh(FULL_MAP, 0);	/* what a mode 0 AFH request does */
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].basic, hop_basic(HOP_VECTORS[i].clk));
}

MU_TEST(test_map_update_while_hopping_takes_effect)
{
	size_t i;

	follow_setup(TEST_ADDRESS, FULL_MAP);
	mu_assert_int_eq(HOP_VECTORS[0].full, hop_basic(HOP_VECTORS[0].clk));
	/* A new map replaces it without re-sending the address. */
	set_afh(REDUCED_MAP, 1);
	mu_assert_int_eq(34, hop_state.afh_chan_count);
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].reduced, hop_basic(HOP_VECTORS[i].clk));
}

MU_TEST(test_reserved_map_bit_is_not_counted)
{
	uint8_t map[10];

	memcpy(map, REDUCED_MAP, sizeof(map));
	map[9] |= 0x80;	/* bit 79 is reserved; there is no channel 79 */
	follow_setup(TEST_ADDRESS, map);
	mu_assert_int_eq(34, hop_state.afh_chan_count);
}

MU_TEST(test_connection_modes_hop_through_hop_basic)
{
	size_t i;

	follow_setup(TEST_ADDRESS, REDUCED_MAP);
	for (i = 0; i < NUM_VECTORS; i++) {
		btphy.mode = BT_MODE_MASTER;
		mu_assert_int_eq(HOP_VECTORS[i].reduced,
				hop_channel(HOP_VECTORS[i].clk));
		btphy.mode = BT_MODE_SLAVE;
		mu_assert_int_eq(HOP_VECTORS[i].reduced,
				hop_channel(HOP_VECTORS[i].clk));
	}
}

/*
 * The specification's published connection-state sample data: three
 * addresses, the basic sequence and four channel maps, 512 clock values each.
 * A mismatch here is a real on-air bug -- the board would hop somewhere the
 * piconet is not.
 */
MU_TEST(test_spec_sample_data_connection_state)
{
	int set, entry, bad = 0;

	for (set = 0; set < SPEC_HOP_CONN_SETS; set++) {
		const struct spec_hop_conn_vector *v = &SPEC_HOP_CONN[set];
		int reported = 0;	/* a report budget per set, not per test */
		uint8_t map[10];

		if (v->printed_afh_map == NULL) {
			follow_setup(v->ulap, NULL);
		} else {
			mu_assert_fatal(spec_hop_afh_map(v->printed_afh_map,
							 map),
					"printed channel map is not twenty "
					"hex digits");
			follow_setup(v->ulap, map);
		}

		for (entry = 0; entry < SPEC_HOP_CONN_ENTRIES; entry++) {
			uint32_t clk = SPEC_HOP_CONN_FIRST_CLK +
					entry * SPEC_HOP_CONN_CLK_STEP;
			int want = v->channels[entry];
			int got = hop_basic(clk);

			if (got == want)
				continue;
			bad++;
			/* Enough to name the failing vector, not a flood. */
			if (reported++ < 4)
				printf("  address %08x, %s, CLK 0x%03x: "
					"channel %d, the spec says %d\n",
					v->ulap,
					v->printed_afh_map
						? v->printed_afh_map
						: "basic sequence",
					clk, got, want);
		}
	}
	mu_assert_int_eq(0, bad);
}

/*
 * The published page scan and inquiry scan sample data, run through
 * hop_scan().  In those substates Table 2.2 makes the kernel's X input
 * CLKN16-12, its Y1 and Y2 inputs 0 and its F input 0, which is the whole
 * control word, so this pins the sequence a scanning board really listens on.
 */
MU_TEST(test_spec_sample_data_page_and_inquiry_scan)
{
	int set, entry, bad = 0;

	for (set = 0; set < SPEC_HOP_SCAN_SETS; set++) {
		const struct spec_hop_scan_vector *v = &SPEC_HOP_SCAN[set];
		int reported = 0;	/* a report budget per set, not per test */

		follow_setup(v->ulap, NULL);

		for (entry = 0; entry < SPEC_HOP_SCAN_ENTRIES; entry++) {
			uint32_t clkn = SPEC_HOP_SCAN_FIRST_CLKN +
					entry * SPEC_HOP_SCAN_CLKN_STEP;
			int want = v->channels[entry];
			int got = hop_scan(clkn);

			if (got == want)
				continue;
			bad++;
			if (reported++ < 4)
				printf("  address %08x, CLKN 0x%07x: "
					"channel %d, the spec says %d\n",
					v->ulap, clkn, got, want);
		}
	}
	mu_assert_int_eq(0, bad);
}

/*
 * Part G prints the scan tables only at clock values that are multiples of
 * 0x1000, where CLK1 is 0, so on their own they cannot tell a Y1 = 0 rule from
 * a Y1 = CLK1 one.  This derives vectors that can: with Y1 = Y2 = 0 the
 * published channel for a clock must also hold at every other clock of the
 * same 1.28 seconds, because none of those bits reaches the control word.  A
 * rule that read CLK1 would move half of them.
 */
MU_TEST(test_a_scan_holds_its_channel_across_the_slots)
{
	/* Clock offsets within one CLKN16-12 value: the second half of the
	 * same slot, the next slot and its second half, the next slot pair,
	 * and the far end of the 1.28 seconds. */
	static const uint32_t OFFSETS[] = { 1, 2, 3, 4, 0x7ff, 0xfff };
	int set, entry, bad = 0;
	size_t off;

	for (set = 0; set < SPEC_HOP_SCAN_SETS; set++) {
		const struct spec_hop_scan_vector *v = &SPEC_HOP_SCAN[set];
		int reported = 0;

		follow_setup(v->ulap, NULL);

		for (entry = 0; entry < SPEC_HOP_SCAN_ENTRIES; entry++) {
			uint32_t clkn = SPEC_HOP_SCAN_FIRST_CLKN +
					entry * SPEC_HOP_SCAN_CLKN_STEP;
			int want = v->channels[entry];

			for (off = 0; off < sizeof(OFFSETS) / sizeof(OFFSETS[0]);
			     off++) {
				int got = hop_scan(clkn + OFFSETS[off]);

				if (got == want)
					continue;
				bad++;
				if (reported++ < 4)
					printf("  address %08x, CLKN 0x%07x + "
						"0x%x: channel %d, the spec "
						"says %d for the whole "
						"interval\n",
						v->ulap, clkn, OFFSETS[off],
						got, want);
			}
		}
	}
	mu_assert_int_eq(0, bad);
}

/*
 * hop_channel() routes each mode to the substate's rule.  The two scan modes
 * must reach hop_scan() with the board's own native clock, whatever clock the
 * leg being tuned hands in: that is what keeps a scan on one frequency when a
 * caller passes a slot clock.
 */
MU_TEST(test_scan_modes_hop_on_the_native_clock)
{
	static const uint8_t MODES[] = {
		BT_MODE_PAGE_SCAN, BT_MODE_INQUIRY_SCAN,
	};
	/* Clocks a caller might hand in: every slot of a pair, and two whose
	 * own CLKN16-12 differs from the native clock's. */
	static const uint32_t CALLER_CLOCKS[] = { 0, 1, 2, 3, 0x3000, 0x12345 };
	const struct spec_hop_scan_vector *v = &SPEC_HOP_SCAN[1];
	unsigned mode;
	int entry, bad = 0;
	size_t c;

	follow_setup(v->ulap, NULL);
	for (mode = 0; mode < sizeof(MODES) / sizeof(MODES[0]); mode++) {
		int reported = 0;

		btphy.mode = MODES[mode];
		for (entry = 0; entry < SPEC_HOP_SCAN_ENTRIES; entry++) {
			uint32_t clkn = SPEC_HOP_SCAN_FIRST_CLKN +
					entry * SPEC_HOP_SCAN_CLKN_STEP;

			btphy.master_clkn = clkn;
			for (c = 0; c < sizeof(CALLER_CLOCKS) /
					sizeof(CALLER_CLOCKS[0]); c++) {
				int got = hop_channel(CALLER_CLOCKS[c]);

				if (got == v->channels[entry])
					continue;
				bad++;
				if (reported++ < 4)
					printf("  mode %d, CLKN 0x%07x, caller "
						"clock 0x%x: channel %d, the "
						"spec says %d\n",
						MODES[mode], clkn,
						CALLER_CLOCKS[c], got,
						v->channels[entry]);
			}
		}
	}
	mu_assert_int_eq(0, bad);
}

/*
 * The substates that answer a page, and the page and inquiry substates
 * themselves, take their slot into the control word: Table 2.2 gives them
 * Y1 = CLK1 and Y2 = 32 x CLK1.  So the channel in a central transmit slot
 * differs from the one in the peripheral transmit slot beside it, on the same
 * phase -- the pair of frequencies an exchange transmits and listens on.
 */
MU_TEST(test_page_and_response_modes_use_the_slot)
{
	static const uint8_t MODES[] = {
		BT_MODE_INQUIRY, BT_MODE_PAGING_RESPONSE,
		BT_MODE_PERIPH_PAGE_RESPONSE,
	};
	const struct spec_hop_scan_vector *v = &SPEC_HOP_SCAN[1];
	unsigned mode;
	int phase, differ = 0;

	follow_setup(v->ulap, NULL);
	/* A clock whose CLKN16-12 is nothing like the phase, so a mode that
	 * ignored the phase and read the clock would be caught. */
	btphy.master_clkn = 0x12000;
	for (mode = 0; mode < sizeof(MODES) / sizeof(MODES[0]); mode++) {
		btphy.mode = MODES[mode];
		differ = 0;
		for (phase = 0; phase < 32; phase++) {
			int even, odd;

			hop_state.x = phase;
			/* 0x40 and 0x42 are the two slots of one pair,
			 * CLK1 = 0 and CLK1 = 1. */
			even = hop_channel(0x40);
			odd = hop_channel(0x42);
			mu_assert_int_eq(hop_inquiry(0x40), even);
			mu_assert_int_eq(hop_inquiry(0x42), odd);
			if (even != odd)
				differ++;
		}
		/* Not every phase has to move, but the rule cannot be a
		 * constant, and it has to hold for each mode on its own. */
		mu_assert(differ > 0, "the slot changes no channel in this "
				"mode, so Y1 = CLK1 is not reaching the "
				"kernel");
	}
}

/*
 * A response starts from the phase the scan window that heard the packet was
 * listening on, not from the clock as it reads once the packet is decoded. A
 * window holds its channel for several slots, so a window that straddles a
 * 1.28-second boundary would otherwise answer on the next phase.
 */
MU_TEST(test_a_response_starts_from_the_phase_the_scan_heard)
{
	const struct spec_hop_scan_vector *v = &SPEC_HOP_SCAN[1];
	/* The last slots of one 1.28-second interval: the scan tuned here. */
	const uint32_t tuned_at = 0x7ffd;

	follow_setup(v->ulap, NULL);
	btphy.mode = BT_MODE_PAGE_SCAN;
	btphy.master_clkn = tuned_at;
	mu_assert_int_eq(hop_scan(tuned_at), hop_channel(0));

	/* The packet is decoded a few slots later, over the boundary. */
	btphy.master_clkn = tuned_at + 4;
	mu_assert(hop_scan_phase(btphy.master_clkn) !=
			hop_scan_phase(tuned_at),
		"the test clocks no longer straddle a phase boundary");

	btphy.mode = BT_MODE_PERIPH_PAGE_RESPONSE;
	hop_start_response();
	mu_assert_int_eq(hop_scan_phase(tuned_at), hop_state.x);
}

/*
 * The inquiry response is the one substate Table 2.2 fixes at Y1 = 1 and
 * Y2 = 32 rather than reading the slot: the answer to an inquiry goes out on
 * the frequency that pairs with the one the inquiry arrived on, whichever slot
 * the board gets to transmit in.
 */
MU_TEST(test_inquiry_response_ignores_the_slot)
{
	const struct spec_hop_scan_vector *v = &SPEC_HOP_SCAN[1];
	int phase;

	follow_setup(v->ulap, NULL);
	btphy.master_clkn = 0x12000;
	btphy.mode = BT_MODE_INQUIRY_RESPONSE;
	for (phase = 0; phase < 32; phase++) {
		/* The peripheral transmit slot of the page and inquiry rule is
		 * the same control word, so it is the channel to expect. */
		int want;

		hop_state.x = phase;
		want = hop_inquiry(0x42);
		mu_assert_int_eq(want, hop_channel(0x40));
		mu_assert_int_eq(want, hop_channel(0x41));
		mu_assert_int_eq(want, hop_channel(0x42));
		mu_assert_int_eq(want, hop_channel(0x43));
	}
}

int main(void)
{
	printf("btbr hop selection tests\n");
	MU_RUN(test_spec_sample_data_connection_state);
	MU_RUN(test_spec_sample_data_page_and_inquiry_scan);
	MU_RUN(test_a_scan_holds_its_channel_across_the_slots);
	MU_RUN(test_scan_modes_hop_on_the_native_clock);
	MU_RUN(test_page_and_response_modes_use_the_slot);
	MU_RUN(test_inquiry_response_ignores_the_slot);
	MU_RUN(test_a_response_starts_from_the_phase_the_scan_heard);
	MU_RUN(test_perm5_lookup_table_matches_spec);
	MU_RUN(test_basic_bank_is_even_then_odd_channels);
	MU_RUN(test_basic_hops_match_reference);
	MU_RUN(test_basic_hops_do_not_truncate_the_kernel_sum);
	/* Boundary of the 20-used-channel minimum, both sides. */
	MU_RUN(test_afh_mapping_table_is_used_channels_even_then_odd);
	MU_RUN(test_full_map_hops_match_reference);
	MU_RUN(test_reduced_map_hops_match_reference);
	MU_RUN(test_exactly_the_minimum_used_channels_still_adapts);
	MU_RUN(test_too_few_used_channels_falls_back_to_basic);
	MU_RUN(test_afh_hops_keep_used_channels_and_repeat_the_central_slot);
	MU_RUN(test_clearing_afh_restores_basic_hopping);
	MU_RUN(test_map_update_while_hopping_takes_effect);
	MU_RUN(test_reserved_map_bit_is_not_counted);
	MU_RUN(test_connection_modes_hop_through_hop_basic);
	return mu_report();
}
