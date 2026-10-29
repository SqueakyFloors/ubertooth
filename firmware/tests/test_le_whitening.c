/*
 * The whitening sequence, as the low-energy capture paths lay it over a packet.
 *
 * Both of them dewhiten through bluetooth_le.c's le_dewhiten_from(), which
 * works four bytes at a time from a packed copy of whitening[] that it builds
 * on first use. Earlier releases had two ways of doing it side by side: a
 * table of ready-made words covering the first 48 bytes of a packet, and a
 * loop that looked up one bit of whitening[] at a time for everything past
 * that -- about 1700 times over for a Data Length Extension payload, between
 * two packets on the air.
 *
 * So there are two things to show, and both are checked below for all 40
 * channels. The words the board now computes are the words that table held,
 * which is what says the sequence itself has not moved. And a whole buffer
 * comes out byte for byte the same as the bit-at-a-time loop produced,
 * including the part of a packet the table never reached, at every offset and
 * length -- the four-byte step leaves one to three bytes at the end of an odd
 * length, and a caller may start part way into a packet.
 */
#include <stdio.h>
#include <string.h>

#include "minunit.h"
/* The firmware's own ubertooth.h over a fake register file, as
 * test_le_phy_logic.c uses it: bluetooth_le.c is compiled beside this test. */
#include "bluetooth_le.h"

/*
 * The ARM `rbit` instruction, which the firmware's rbit() is one line of
 * inline assembly around: reverse the order of the 32 bits.
 */
u32 rbit(u32 value)
{
	u32 result = 0;
	int i;

	for (i = 0; i < 32; i++)
		if (value & (1u << i))
			result |= 1u << (31 - i);
	return result;
}

/* ------------------------------------------------------------------ *
 * The two references
 * ------------------------------------------------------------------ */

/*
 * The whitening words earlier releases carried in flash, one row per channel
 * index and twelve words of four bytes in each: the first 48 bytes of a PDU
 * on that channel. Kept here as a fixture, so what the board shipped and what
 * it computes now can be compared.
 */
static const u32 shipped_whitening_word[BTLE_CHANNELS][12] = {
	{ 0xc3bcb240, 0x5f4a371f, 0x9a9cf685, 0x44c5d6c1, 0xe1de5920, 0xafa51b8f,
	  0xcd4e7b42, 0x2262eb60, 0xf0ef2c90, 0x57d28dc7, 0x66a73da1, 0x113175b0, },
	{ 0xbcb24089, 0x4a371fc3, 0x9cf6855f, 0xc5d6c19a, 0xde592044, 0xa51b8fe1,
	  0x4e7b42af, 0x62eb60cd, 0xef2c9022, 0xd28dc7f0, 0xa73da157, 0x3175b066, },
	{ 0x3da157d2, 0x75b066a7, 0x96481131, 0x46e3f877, 0x9ed0abe9, 0xbad83353,
	  0xcb240898, 0xa371fc3b, 0xcf6855f4, 0x5d6c19a9, 0xe592044c, 0x51b8fe1d, },
	{ 0x42afa51b, 0x60cd4e7b, 0x902262eb, 0xc7f0ef2c, 0xa157d28d, 0xb066a73d,
	  0x48113175, 0xe3f87796, 0xd0abe946, 0xd833539e, 0x240898ba, 0x71fc3bcb, },
	{ 0x3f877964, 0x0abe946e, 0x833539ed, 0x40898bad, 0x1fc3bcb2, 0x855f4a37,
	  0xc19a9cf6, 0x2044c5d6, 0x8fe1de59, 0x42afa51b, 0x60cd4e7b, 0x902262eb, },
	{ 0x40898bad, 0x1fc3bcb2, 0x855f4a37, 0xc19a9cf6, 0x2044c5d6, 0x8fe1de59,
	  0x42afa51b, 0x60cd4e7b, 0x902262eb, 0xc7f0ef2c, 0xa157d28d, 0xb066a73d, },
	{ 0xc19a9cf6, 0x2044c5d6, 0x8fe1de59, 0x42afa51b, 0x60cd4e7b, 0x902262eb,
	  0xc7f0ef2c, 0xa157d28d, 0xb066a73d, 0x48113175, 0xe3f87796, 0xd0abe946, },
	{ 0xbe946e3f, 0x3539ed0a, 0x898bad83, 0xc3bcb240, 0x5f4a371f, 0x9a9cf685,
	  0x44c5d6c1, 0xe1de5920, 0xafa51b8f, 0xcd4e7b42, 0x2262eb60, 0xf0ef2c90, },
	{ 0x3bcb2408, 0xf4a371fc, 0xa9cf6855, 0x4c5d6c19, 0x1de59204, 0xfa51b8fe,
	  0xd4e7b42a, 0x262eb60c, 0x0ef2c902, 0x7d28dc7f, 0x6a73da15, 0x13175b06, },
	{ 0x44c5d6c1, 0xe1de5920, 0xafa51b8f, 0xcd4e7b42, 0x2262eb60, 0xf0ef2c90,
	  0x57d28dc7, 0x66a73da1, 0x113175b0, 0xf8779648, 0xabe946e3, 0x33539ed0, },
	{ 0xc5d6c19a, 0xde592044, 0xa51b8fe1, 0x4e7b42af, 0x62eb60cd, 0xef2c9022,
	  0xd28dc7f0, 0xa73da157, 0x3175b066, 0x77964811, 0xe946e3f8, 0x539ed0ab, },
	{ 0xbad83353, 0xcb240898, 0xa371fc3b, 0xcf6855f4, 0x5d6c19a9, 0xe592044c,
	  0x51b8fe1d, 0xe7b42afa, 0x2eb60cd4, 0xf2c90226, 0x28dc7f0e, 0x73da157d, },
	{ 0xc7f0ef2c, 0xa157d28d, 0xb066a73d, 0x48113175, 0xe3f87796, 0xd0abe946,
	  0xd833539e, 0x240898ba, 0x71fc3bcb, 0x6855f4a3, 0x6c19a9cf, 0x92044c5d, },
	{ 0xb8fe1de5, 0xb42afa51, 0xb60cd4e7, 0xc902262e, 0xdc7f0ef2, 0xda157d28,
	  0x5b066a73, 0x64811317, 0x6e3f8779, 0xed0abe94, 0xad833539, 0xb240898b, },
	{ 0x39ed0abe, 0x8bad8335, 0xbcb24089, 0x4a371fc3, 0x9cf6855f, 0xc5d6c19a,
	  0xde592044, 0xa51b8fe1, 0x4e7b42af, 0x62eb60cd, 0xef2c9022, 0xd28dc7f0, },
	{ 0x46e3f877, 0x9ed0abe9, 0xbad83353, 0xcb240898, 0xa371fc3b, 0xcf6855f4,
	  0x5d6c19a9, 0xe592044c, 0x51b8fe1d, 0xe7b42afa, 0x2eb60cd4, 0xf2c90226, },
	{ 0x33539ed0, 0x0898bad8, 0xfc3bcb24, 0x55f4a371, 0x19a9cf68, 0x044c5d6c,
	  0xfe1de592, 0x2afa51b8, 0x0cd4e7b4, 0x02262eb6, 0x7f0ef2c9, 0x157d28dc, },
	{ 0x4c5d6c19, 0x1de59204, 0xfa51b8fe, 0xd4e7b42a, 0x262eb60c, 0x0ef2c902,
	  0x7d28dc7f, 0x6a73da15, 0x13175b06, 0x87796481, 0xbe946e3f, 0x3539ed0a, },
	{ 0xcd4e7b42, 0x2262eb60, 0xf0ef2c90, 0x57d28dc7, 0x66a73da1, 0x113175b0,
	  0xf8779648, 0xabe946e3, 0x33539ed0, 0x0898bad8, 0xfc3bcb24, 0x55f4a371, },
	{ 0xb240898b, 0x371fc3bc, 0xf6855f4a, 0xd6c19a9c, 0x592044c5, 0x1b8fe1de,
	  0x7b42afa5, 0xeb60cd4e, 0x2c902262, 0x8dc7f0ef, 0x3da157d2, 0x75b066a7, },
	{ 0xcf6855f4, 0x5d6c19a9, 0xe592044c, 0x51b8fe1d, 0xe7b42afa, 0x2eb60cd4,
	  0xf2c90226, 0x28dc7f0e, 0x73da157d, 0x175b066a, 0x79648113, 0x946e3f87, },
	{ 0xb066a73d, 0x48113175, 0xe3f87796, 0xd0abe946, 0xd833539e, 0x240898ba,
	  0x71fc3bcb, 0x6855f4a3, 0x6c19a9cf, 0x92044c5d, 0xb8fe1de5, 0xb42afa51, },
	{ 0x3175b066, 0x77964811, 0xe946e3f8, 0x539ed0ab, 0x98bad833, 0x3bcb2408,
	  0xf4a371fc, 0xa9cf6855, 0x4c5d6c19, 0x1de59204, 0xfa51b8fe, 0xd4e7b42a, },
	{ 0x4e7b42af, 0x62eb60cd, 0xef2c9022, 0xd28dc7f0, 0xa73da157, 0x3175b066,
	  0x77964811, 0xe946e3f8, 0x539ed0ab, 0x98bad833, 0x3bcb2408, 0xf4a371fc, },
	{ 0xcb240898, 0xa371fc3b, 0xcf6855f4, 0x5d6c19a9, 0xe592044c, 0x51b8fe1d,
	  0xe7b42afa, 0x2eb60cd4, 0xf2c90226, 0x28dc7f0e, 0x73da157d, 0x175b066a, },
	{ 0xb42afa51, 0xb60cd4e7, 0xc902262e, 0xdc7f0ef2, 0xda157d28, 0x5b066a73,
	  0x64811317, 0x6e3f8779, 0xed0abe94, 0xad833539, 0xb240898b, 0x371fc3bc, },
	{ 0x3539ed0a, 0x898bad83, 0xc3bcb240, 0x5f4a371f, 0x9a9cf685, 0x44c5d6c1,
	  0xe1de5920, 0xafa51b8f, 0xcd4e7b42, 0x2262eb60, 0xf0ef2c90, 0x57d28dc7, },
	{ 0x4a371fc3, 0x9cf6855f, 0xc5d6c19a, 0xde592044, 0xa51b8fe1, 0x4e7b42af,
	  0x62eb60cd, 0xef2c9022, 0xd28dc7f0, 0xa73da157, 0x3175b066, 0x77964811, },
	{ 0x371fc3bc, 0xf6855f4a, 0xd6c19a9c, 0x592044c5, 0x1b8fe1de, 0x7b42afa5,
	  0xeb60cd4e, 0x2c902262, 0x8dc7f0ef, 0x3da157d2, 0x75b066a7, 0x96481131, },
	{ 0x48113175, 0xe3f87796, 0xd0abe946, 0xd833539e, 0x240898ba, 0x71fc3bcb,
	  0x6855f4a3, 0x6c19a9cf, 0x92044c5d, 0xb8fe1de5, 0xb42afa51, 0xb60cd4e7, },
	{ 0xc902262e, 0xdc7f0ef2, 0xda157d28, 0x5b066a73, 0x64811317, 0x6e3f8779,
	  0xed0abe94, 0xad833539, 0xb240898b, 0x371fc3bc, 0xf6855f4a, 0xd6c19a9c, },
	{ 0xb60cd4e7, 0xc902262e, 0xdc7f0ef2, 0xda157d28, 0x5b066a73, 0x64811317,
	  0x6e3f8779, 0xed0abe94, 0xad833539, 0xb240898b, 0x371fc3bc, 0xf6855f4a, },
	{ 0x2262eb60, 0xf0ef2c90, 0x57d28dc7, 0x66a73da1, 0x113175b0, 0xf8779648,
	  0xabe946e3, 0x33539ed0, 0x0898bad8, 0xfc3bcb24, 0x55f4a371, 0x19a9cf68, },
	{ 0x5d6c19a9, 0xe592044c, 0x51b8fe1d, 0xe7b42afa, 0x2eb60cd4, 0xf2c90226,
	  0x28dc7f0e, 0x73da157d, 0x175b066a, 0x79648113, 0x946e3f87, 0x39ed0abe, },
	{ 0xdc7f0ef2, 0xda157d28, 0x5b066a73, 0x64811317, 0x6e3f8779, 0xed0abe94,
	  0xad833539, 0xb240898b, 0x371fc3bc, 0xf6855f4a, 0xd6c19a9c, 0x592044c5, },
	{ 0xa371fc3b, 0xcf6855f4, 0x5d6c19a9, 0xe592044c, 0x51b8fe1d, 0xe7b42afa,
	  0x2eb60cd4, 0xf2c90226, 0x28dc7f0e, 0x73da157d, 0x175b066a, 0x79648113, },
	{ 0xde592044, 0xa51b8fe1, 0x4e7b42af, 0x62eb60cd, 0xef2c9022, 0xd28dc7f0,
	  0xa73da157, 0x3175b066, 0x77964811, 0xe946e3f8, 0x539ed0ab, 0x98bad833, },
	{ 0xa157d28d, 0xb066a73d, 0x48113175, 0xe3f87796, 0xd0abe946, 0xd833539e,
	  0x240898ba, 0x71fc3bcb, 0x6855f4a3, 0x6c19a9cf, 0x92044c5d, 0xb8fe1de5, },
	{ 0x2044c5d6, 0x8fe1de59, 0x42afa51b, 0x60cd4e7b, 0x902262eb, 0xc7f0ef2c,
	  0xa157d28d, 0xb066a73d, 0x48113175, 0xe3f87796, 0xd0abe946, 0xd833539e, },
	{ 0x5f4a371f, 0x9a9cf685, 0x44c5d6c1, 0xe1de5920, 0xafa51b8f, 0xcd4e7b42,
	  0x2262eb60, 0xf0ef2c90, 0x57d28dc7, 0x66a73da1, 0x113175b0, 0xf8779648, },
};

/*
 * Dewhitening as the loop that came before it did: one bit of whitening[] at
 * a time, reversing the bit order of each byte as it goes.
 */
static void reference_dewhiten(uint8_t *data, unsigned size, unsigned channel,
                               unsigned offset)
{
	unsigned i, j, bit;
	unsigned idx = whitening_index[btle_channel_index(channel)];

	idx = (idx + 8 * offset) % sizeof(whitening);

	for (i = 0; i < size; ++i) {
		uint8_t out = 0;

		for (j = 0; j < 8; ++j) {
			bit = (data[i] >> (7 - j)) & 1;
			bit ^= whitening[idx];
			idx = (idx + 1) % sizeof(whitening);
			out |= bit << j;
		}
		data[i] = out;
	}
}

/* The longest packet there can be: the header, a Data Length Extension
 * payload and the CRC. */
#define LONGEST_PDU (2 + LE_MAX_PAYLOAD + 3)

/* A buffer that is the same on every run, so a failure can be repeated. */
static void fill_pattern(uint8_t *data, unsigned size, unsigned seed)
{
	unsigned i;
	unsigned state = seed * 2654435761u + 1u;

	for (i = 0; i < size; i++) {
		state = state * 1103515245u + 12345u;
		data[i] = (uint8_t)(state >> 16);
	}
}

/* ------------------------------------------------------------------ *
 * The words are the words the table held
 * ------------------------------------------------------------------ */

/*
 * Dewhitening four zero bytes leaves the whitening word itself, because the
 * reversed bits of zero are zero. So this reads back every word the old table
 * held and requires it to be unchanged.
 */
MU_TEST(the_computed_words_are_the_words_the_table_held)
{
	uint8_t idx;
	unsigned n;
	int compared = 0;

	for (idx = 0; idx < BTLE_CHANNELS; idx++) {
		unsigned channel = btle_channel_index_to_phys(idx);

		for (n = 0; n < 12; n++) {
			uint8_t word[4] = { 0, 0, 0, 0 };
			u32 got;

			le_dewhiten_from(word, sizeof(word), channel, 4 * n);
			got = (u32)word[0]
			    | (u32)word[1] <<  8
			    | (u32)word[2] << 16
			    | (u32)word[3] << 24;
			compared++;
			if (got != shipped_whitening_word[idx][n]) {
				char msg[192];

				snprintf(msg, sizeof(msg),
				         "channel index %u (%u MHz), word %u: the "
				         "sequence reads 0x%08x where the table that "
				         "shipped had 0x%08x", idx, channel, n,
				         (unsigned)got,
				         (unsigned)shipped_whitening_word[idx][n]);
				mu_assert(0, msg);
			}
		}
	}

	mu_assert_int_eq(BTLE_CHANNELS * 12, compared);
}

/* ------------------------------------------------------------------ *
 * ...and a whole packet comes out as the bit-at-a-time loop left it
 * ------------------------------------------------------------------ */

/* The longest packet there is, on every channel: the part past 48 bytes is
 * what used to cost a lookup per bit. */
MU_TEST(a_longest_packet_is_dewhitened_as_it_was_before)
{
	uint8_t idx;
	int compared = 0;

	for (idx = 0; idx < BTLE_CHANNELS; idx++) {
		unsigned channel = btle_channel_index_to_phys(idx);
		uint8_t got[LONGEST_PDU], want[LONGEST_PDU];
		unsigned i;

		fill_pattern(got, sizeof(got), idx);
		memcpy(want, got, sizeof(want));
		le_dewhiten(got, sizeof(got), channel);
		reference_dewhiten(want, sizeof(want), channel, 0);
		compared++;

		for (i = 0; i < sizeof(got); i++)
			if (got[i] != want[i]) {
				char msg[192];

				snprintf(msg, sizeof(msg),
				         "channel index %u (%u MHz), byte %u of %u: "
				         "0x%02x where the bit-at-a-time loop gave "
				         "0x%02x", idx, channel, i,
				         (unsigned)sizeof(got), got[i], want[i]);
				mu_assert(0, msg);
			}
	}

	mu_assert_int_eq(BTLE_CHANNELS, compared);
}

/*
 * Every length and every starting offset the four-byte step has to cope with:
 * a length that is not a whole number of words ends in one to three bytes
 * taken on their own, and an offset that is not a multiple of four moves where
 * each group of four sits in the packet.
 */
MU_TEST(every_length_and_offset_matches_the_bit_at_a_time_loop)
{
	uint8_t idx;
	int compared = 0;

	for (idx = 0; idx < BTLE_CHANNELS; idx += 7) {
		unsigned channel = btle_channel_index_to_phys(idx);
		unsigned size, offset;

		for (size = 1; size <= 40; size++) {
			for (offset = 0; offset <= 9; offset++) {
				uint8_t got[40], want[40];

				fill_pattern(got, size, size + offset);
				memcpy(want, got, size);
				le_dewhiten_from(got, size, channel, offset);
				reference_dewhiten(want, size, channel, offset);
				compared++;
				if (memcmp(got, want, size) != 0) {
					char msg[192];

					snprintf(msg, sizeof(msg),
					         "channel index %u, %u bytes from offset "
					         "%u: dewhitened differently from the "
					         "bit-at-a-time loop", idx, size, offset);
					mu_assert(0, msg);
				}
			}
		}
	}

	/* Six channel indices, forty lengths, ten offsets. */
	mu_assert_int_eq(6 * 40 * 10, compared);
}

/*
 * The capture loop dewhitens the header first, to read how long the packet is
 * going to be, and the rest once the bytes are in. Carrying on from an offset
 * has to give the same packet as doing it in one go, or the two halves of a
 * record would be whitened against different parts of the sequence.
 */
MU_TEST(dewhitening_in_two_parts_matches_doing_it_at_once)
{
	uint8_t idx;
	int compared = 0;

	for (idx = 0; idx < BTLE_CHANNELS; idx += 3) {
		unsigned channel = btle_channel_index_to_phys(idx);
		unsigned split;

		for (split = 1; split < 20; split++) {
			uint8_t got[LONGEST_PDU], want[LONGEST_PDU];

			fill_pattern(got, sizeof(got), split);
			memcpy(want, got, sizeof(want));
			le_dewhiten(want, sizeof(want), channel);

			le_dewhiten_from(got, split, channel, 0);
			le_dewhiten_from(got + split, sizeof(got) - split, channel,
			                 split);
			compared++;

			if (memcmp(got, want, sizeof(got)) != 0) {
				char msg[192];

				snprintf(msg, sizeof(msg),
				         "channel index %u: a packet dewhitened in two "
				         "parts, split at %u bytes, differs from the "
				         "same packet done at once", idx, split);
				mu_assert(0, msg);
			}
		}
	}

	/* Fourteen channel indices, nineteen split points. */
	mu_assert_int_eq(14 * 19, compared);
}

int main(void)
{
	printf("LE whitening tests\n");
	MU_RUN(the_computed_words_are_the_words_the_table_held);
	MU_RUN(a_longest_packet_is_dewhitened_as_it_was_before);
	MU_RUN(every_length_and_offset_matches_the_bit_at_a_time_loop);
	MU_RUN(dewhitening_in_two_parts_matches_doing_it_at_once);
	return mu_report();
}
