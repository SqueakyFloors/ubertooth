/*
 * Unit tests for BR/EDR connection-state hop selection in
 * firmware/bluetooth_rxtx/bluetooth.c (precalc() and next_hop()).
 *
 * The expected frequencies were computed independently, outside this code
 * base, by a direct transcription of the Bluetooth Core specification
 * Vol 2 Part B section 2.6: the basic kernel (Figure 2.16, Table 2.2
 * connection-state inputs), the AFH re-mapping function (2.6.3.1) and the AFH
 * same channel mechanism (2.6.1).  The basic vectors also agree with libbtbb's
 * hop implementation.  They are fixed here so the tests do not use the
 * firmware algorithm as its own oracle.
 *
 * On top of those, the whole of the specification's own published hop sample
 * data is run through next_hop(): 7680 vectors from Vol 2, Part G, Section 2,
 * in spec_hop_vectors.h.  The hand-picked vectors above stay because they name
 * the rule each one is there for, and because they cover clock values and
 * channel maps Part G does not print.
 *
 * AFH rules being checked:
 *  - a basic-sequence channel that the map marks used is kept unchanged;
 *  - an unused one is replaced from the used channels only, ordered even
 *    channels ascending then odd channels ascending;
 *  - with AFH, the peripheral slot (CLK1 = 1) reuses the central slot channel.
 */
#include <string.h>

#include "minunit.h"
#include "spec_hop_vectors.h"
#include "bluetooth.h"

/* Non-static state in bluetooth.c, reset between tests. */
extern u8 bank[NUM_BREDR_CHANNELS];
extern u8 afh_bank[NUM_BREDR_CHANNELS];

#define TEST_ADDRESS UINT64_C(0x12345678)

struct hop_vector {
	u32 clock;
	u16 basic;	/* MHz with AFH disabled */
	u16 afh;	/* MHz with AFH_MAP below */
};

/*
 * Used channels: 22-48 and 72-78 (34 channels), i.e. the 2402-2423 MHz and
 * 2451-2473 MHz ranges avoided, as a host does around two busy Wi-Fi channels.
 * Byte 0 holds channels 0-7, least significant bit first.
 */
static const u8 AFH_MAP[10] = {
	0x00, 0x00, 0xc0, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x7f
};

static const struct hop_vector HOP_VECTORS[] = {
	{0x00000000, 2462, 2449},
	{0x00000001, 2462, 2449},
	{0x00000002, 2439, 2449},	/* used, but same channel as slot 0 */
	{0x00000003, 2439, 2449},
	{0x00000004, 2466, 2477},
	{0x0000007e, 2451, 2439},
	{0x00123456, 2429, 2444},
	{0x07fffffe, 2472, 2436},
	{0xffffffff, 2444, 2479},
	{0x00002000, 2456, 2424},
	{0x00045678, 2473, 2478},
	{0x00abcdef, 2436, 2435},
	{0x01000004, 2455, 2448},
	{0x03fffffc, 2450, 2450},	/* used basic channel kept */
	{0x089abcde, 2434, 2435},
	{0x00448d30, 2449, 2449},	/* used basic channel kept */
	{0x0045b07c, 2437, 2437},	/* used basic channel kept */
	{0x004c8444, 2447, 2447},	/* used basic channel kept */
};
#define NUM_VECTORS (sizeof(HOP_VECTORS) / sizeof(HOP_VECTORS[0]))

static void reset_state(u32 address)
{
	memset(&target, 0, sizeof(target));
	afh_enabled = 0;
	memset(afh_map, 0, sizeof(afh_map));
	used_channels = 0;
	memset(bank, 0, sizeof(bank));
	memset(afh_bank, 0, sizeof(afh_bank));
	target.address = address;
}

static int map_marks_used(const u8 *map, u16 mhz)
{
	unsigned chan = mhz - 2402u;
	return chan < NUM_BREDR_CHANNELS && ((map[chan / 8] >> (chan % 8)) & 1);
}

/* Apply the map in the order a host does: address, then map. */
static void follow_setup(u32 address, const u8 *map)
{
	reset_state(address);
	precalc();
	if (map != NULL)
		bluetooth_set_afh_map(map);
}

MU_TEST(test_basic_hops_match_reference)
{
	size_t i;
	reset_state(TEST_ADDRESS);
	precalc();
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].basic, next_hop(HOP_VECTORS[i].clock));
}

MU_TEST(test_afh_mapping_table_is_used_channels_even_then_odd)
{
	static const u8 expected[] = {
		22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48,
		72, 74, 76, 78,
		23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 47,
		73, 75, 77,
	};
	size_t i;
	follow_setup(TEST_ADDRESS, AFH_MAP);
	mu_assert_int_eq(sizeof(expected), used_channels);
	for (i = 0; i < sizeof(expected); i++)
		mu_assert_int_eq(expected[i], afh_bank[i]);
	/* The basic table is not overwritten by the AFH table. */
	for (i = 0; i < NUM_BREDR_CHANNELS; i++)
		mu_assert_int_eq((i * 2) % NUM_BREDR_CHANNELS, bank[i]);
}

MU_TEST(test_afh_hops_match_reference)
{
	size_t i;
	follow_setup(TEST_ADDRESS, AFH_MAP);
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].afh, next_hop(HOP_VECTORS[i].clock));
}

MU_TEST(test_map_before_address_gives_same_hops)
{
	size_t i;
	reset_state(TEST_ADDRESS);
	bluetooth_set_afh_map(AFH_MAP);
	precalc();	/* address set after the map */
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].afh, next_hop(HOP_VECTORS[i].clock));
}

MU_TEST(test_map_update_while_hopping_takes_effect)
{
	/* All 79 channels used: the adapted sequence equals the basic one
	 * except that each peripheral slot repeats its central slot. */
	static const u8 full_map[10] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
	};
	size_t i;
	follow_setup(TEST_ADDRESS, full_map);
	mu_assert_int_eq(79, used_channels);
	mu_assert_int_eq(HOP_VECTORS[0].basic, next_hop(0x2));
	/* A new map from the host replaces it without resetting the address. */
	bluetooth_set_afh_map(AFH_MAP);
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].afh, next_hop(HOP_VECTORS[i].clock));
}

MU_TEST(test_afh_hops_stay_on_used_channels)
{
	u32 clock;
	int off_map = 0, slot_mismatch = 0, kept = 0, mismatched_kept = 0;
	follow_setup(TEST_ADDRESS, AFH_MAP);
	/* Every central slot across 2^20 slots, plus its peripheral slot. */
	for (clock = 0; clock < (UINT32_C(1) << 22); clock += 4) {
		u16 afh = next_hop(clock);
		u16 peripheral = next_hop(clock | 2);
		u16 basic;
		if (!map_marks_used(AFH_MAP, afh))
			off_map++;
		if (peripheral != afh)
			slot_mismatch++;
		/* Compare with the basic sequence for the same slot. */
		afh_enabled = 0;
		basic = next_hop(clock);
		afh_enabled = 1;
		if (map_marks_used(AFH_MAP, basic)) {
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
	follow_setup(TEST_ADDRESS, AFH_MAP);
	/* What UBERTOOTH_CLEAR_AFHMAP does, without re-sending the address. */
	memset(afh_map, 0, sizeof(afh_map));
	afh_enabled = 0;
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].basic, next_hop(HOP_VECTORS[i].clock));
}

MU_TEST(test_too_few_used_channels_falls_back_to_basic)
{
	/* Channels 0-18: one short of the spec minimum of 20. */
	static const u8 small_map[10] = {0xff, 0xff, 0x07};
	size_t i;
	follow_setup(TEST_ADDRESS, small_map);
	mu_assert_int_eq(19, used_channels);
	for (i = 0; i < NUM_VECTORS; i++)
		mu_assert_int_eq(HOP_VECTORS[i].basic, next_hop(HOP_VECTORS[i].clock));
}

MU_TEST(test_reserved_map_bit_is_not_counted)
{
	u8 map[10];
	memcpy(map, AFH_MAP, sizeof(map));
	map[9] |= 0x80;	/* bit 79 is reserved; there is no channel 79 */
	follow_setup(TEST_ADDRESS, map);
	mu_assert_int_eq(34, used_channels);
}

/*
 * The specification's published sample data: three addresses, the basic
 * sequence and four channel maps, 512 clock values each.  A mismatch here is a
 * real on-air bug -- the board would hop somewhere the piconet is not.
 */
MU_TEST(test_spec_sample_data_connection_state)
{
	int set, entry, bad = 0;

	for (set = 0; set < SPEC_HOP_CONN_SETS; set++) {
		const struct spec_hop_conn_vector *v = &SPEC_HOP_CONN[set];
		int reported = 0;	/* a report budget per set, not per test */
		u8 map[10];

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
			u32 clk = SPEC_HOP_CONN_FIRST_CLK +
					entry * SPEC_HOP_CONN_CLK_STEP;
			int want = v->channels[entry];
			int got = next_hop(clk) - BT_FREQ_MIN;

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

int main(void)
{
	printf("Bluetooth hop selection tests\n");
	MU_RUN(test_spec_sample_data_connection_state);
	MU_RUN(test_basic_hops_match_reference);
	MU_RUN(test_afh_mapping_table_is_used_channels_even_then_odd);
	MU_RUN(test_afh_hops_match_reference);
	MU_RUN(test_map_before_address_gives_same_hops);
	MU_RUN(test_map_update_while_hopping_takes_effect);
	MU_RUN(test_afh_hops_stay_on_used_channels);
	MU_RUN(test_clearing_afh_restores_basic_hopping);
	MU_RUN(test_too_few_used_channels_falls_back_to_basic);
	MU_RUN(test_reserved_map_bit_is_not_counted);
	return mu_report();
}
