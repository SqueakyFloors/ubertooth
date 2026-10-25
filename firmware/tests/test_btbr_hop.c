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
 * hop.c reaches PERM5 through a lookup table that the firmware image builds
 * once; the test fills that table from the specification's butterfly network
 * instead, so the expected values do not come from firmware code.  The one
 * thing the test shares with the firmware is the order the butterfly stages
 * run in, which only the specification's figure shows; see PERM5 below.
 */
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include <ubtbr/hop.h>
#include <ubtbr/btphy.h>

/* Definitions the firmware image provides and hop.c reads. */
btphy_t btphy;
uint8_t perm5_lut[2][4096];

void cprintf(char *fmt, ...)
{
	(void)fmt;
}

void __attribute__((noreturn)) die(char *fmt, ...)
{
	(void)fmt;
	abort();
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

/*
 * The table hop.h's perm5() indexes: entry [0] applies the butterflies gated
 * by P13-P7, entry [1] those gated by P6-P0, each for one 7-bit slice of the
 * control word and one value of Z.
 */
static void perm5_lut_init(void)
{
	unsigned slice, z;

	for (slice = 0; slice < 128; slice++) {
		for (z = 0; z < 32; z++) {
			perm5_lut[0][(slice << 5) | z] =
				perm5_butterflies(z, slice << 7, 13, 7);
			perm5_lut[1][(slice << 5) | z] =
				perm5_butterflies(z, slice, 6, 0);
		}
	}
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
/* Channels 0-18: one short of the spec minimum of 20. */
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

/* BIG_E_ADDRESS, AFH disabled.  The first four clk values are ones where
 * PERM5 + E + F + Y2 exceeds 255. */
static const struct hop_vector BIG_E_VECTORS[] = {
	{0x00000776, 38, 0, 0},
	{0x00000777, 38, 0, 0},
	{0x000009d6, 38, 0, 0},
	{0x000009f6, 40, 0, 0},
	{0x00000000, 37, 0, 0},
	{0x00123456, 47, 0, 0},
};
#define NUM_BIG_E_VECTORS (sizeof(BIG_E_VECTORS) / sizeof(BIG_E_VECTORS[0]))

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
		mu_assert_int_eq(BIG_E_VECTORS[i].basic,
				hop_basic(BIG_E_VECTORS[i].clk));
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

int main(void)
{
	perm5_lut_init();
	printf("btbr hop selection tests\n");
	MU_RUN(test_perm5_lookup_table_matches_spec);
	MU_RUN(test_basic_bank_is_even_then_odd_channels);
	MU_RUN(test_basic_hops_match_reference);
	MU_RUN(test_basic_hops_do_not_truncate_the_kernel_sum);
	MU_RUN(test_afh_mapping_table_is_used_channels_even_then_odd);
	MU_RUN(test_full_map_hops_match_reference);
	MU_RUN(test_reduced_map_hops_match_reference);
	MU_RUN(test_too_few_used_channels_falls_back_to_basic);
	MU_RUN(test_afh_hops_keep_used_channels_and_repeat_the_central_slot);
	MU_RUN(test_clearing_afh_restores_basic_hopping);
	MU_RUN(test_map_update_while_hopping_takes_effect);
	MU_RUN(test_reserved_map_bit_is_not_counted);
	MU_RUN(test_connection_modes_hop_through_hop_basic);
	return mu_report();
}
