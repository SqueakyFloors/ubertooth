/*
 * Copyright 2012 Dominic Spill
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "bluetooth_le.h"

#include <string.h>

extern u8 le_channel_idx;
extern u8 le_hop_amount;

u16 btle_next_hop(le_state_t *le)
{
	u16 phys;

	if (le->chan_sel_2) {
		/* Algorithm 2 asks for the channel of a given event rather than
		 * stepping on from the last one, so a missed event costs the
		 * follower nothing.
		 *
		 * The channel map is the unmapped sequence here: promiscuous
		 * recovery never sees the connection's map, which is given out
		 * at setup. On a connection using fewer than the 37 channels
		 * that lands on the right channel for the events whose unmapped
		 * channel is one the connection kept, and elsewhere for the
		 * rest. */
		le->channel_idx = le_csa2_unmapped_channel(le->event_counter,
				le->access_address);
		++le->event_counter;
		return btle_channel_index_to_phys(le->channel_idx);
	}

	phys = btle_channel_index_to_phys(le->channel_idx);
	le->channel_idx = (le->channel_idx + le->channel_increment) % 37;
	return phys;
}

// calculate channel index from physical channel
// channel is in range [BT_FREQ_MIN, BT_FREQ_MAX]
uint8_t btle_channel_index(uint16_t channel) {
	uint8_t idx;
	channel = (channel - BT_FREQ_MIN) / 2;
	if (channel == 0)
		idx = 37;
	else if (channel < 12)
		idx = channel - 1;
	else if (channel == 12)
		idx = 38;
	else if (channel < 39)
		idx = channel - 2;
	else
		idx = 39;
	return idx;
}

u16 btle_channel_index_to_phys(u8 idx) {
	u16 phys;
	if (idx < 11)
		phys = 2404 + 2 * idx;
	else if (idx < 37)
		phys = 2428 + 2 * (idx - 11);
	else if (idx == 37)
		phys = BTLE_ADV_CHANNEL_37_MHZ;
	else if (idx == 38)
		phys = BTLE_ADV_CHANNEL_38_MHZ;
	else
		phys = BTLE_ADV_CHANNEL_39_MHZ;
	return phys;
}

// calculate CRC
//	note 1: crc_init's bits should be in reverse order
//	note 2: output bytes are in reverse order compared to wire
//
//		example output:
//			0x6ff46e
//
//		bytes in packet will be:
//		  { 0x6e, 0xf4, 0x6f }
//
u32 btle_calc_crc(u32 crc_init, u8 *data, int len) {
	u32 state = crc_init & 0xffffff;
	u32 lfsr_mask = 0x5a6000; // 010110100110000000000000
	int i, j;

	for (i = 0; i < len; ++i) {
		u8 cur = data[i];
		for (j = 0; j < 8; ++j) {
			int next_bit = (state ^ cur) & 1;
			cur >>= 1;
			state >>= 1;
			if (next_bit) {
				state |= 1 << 23;
				state ^= lfsr_mask;
			}
		}
	}

	return state;
}

// runs the CRC in reverse to generate a CRCInit
//
//	crc should be big endian
//	the return will be big endian
//
u32 btle_reverse_crc(u32 crc, u8 *data, int len) {
	u32 state = crc;
	u32 lfsr_mask = 0xb4c000; // 101101001100000000000000
	u32 ret;
	int i, j;

	for (i = len - 1; i >= 0; --i) {
		u8 cur = data[i];
		for (j = 0; j < 8; ++j) {
			int top_bit = state >> 23;
			state = (state << 1) & 0xffffff;
			state |= top_bit ^ ((cur >> (7 - j)) & 1);
			if (top_bit)
				state ^= lfsr_mask;
		}
	}

	ret = 0;
	for (i = 0; i < 24; ++i)
		ret |= ((state >> i) & 1) << (23 - i);

	return ret;
}

u32 btle_crc_lut[256] = {
	0x000000, 0x01b4c0, 0x036980, 0x02dd40, 0x06d300, 0x0767c0, 0x05ba80, 0x040e40,
	0x0da600, 0x0c12c0, 0x0ecf80, 0x0f7b40, 0x0b7500, 0x0ac1c0, 0x081c80, 0x09a840,
	0x1b4c00, 0x1af8c0, 0x182580, 0x199140, 0x1d9f00, 0x1c2bc0, 0x1ef680, 0x1f4240,
	0x16ea00, 0x175ec0, 0x158380, 0x143740, 0x103900, 0x118dc0, 0x135080, 0x12e440,
	0x369800, 0x372cc0, 0x35f180, 0x344540, 0x304b00, 0x31ffc0, 0x332280, 0x329640,
	0x3b3e00, 0x3a8ac0, 0x385780, 0x39e340, 0x3ded00, 0x3c59c0, 0x3e8480, 0x3f3040,
	0x2dd400, 0x2c60c0, 0x2ebd80, 0x2f0940, 0x2b0700, 0x2ab3c0, 0x286e80, 0x29da40,
	0x207200, 0x21c6c0, 0x231b80, 0x22af40, 0x26a100, 0x2715c0, 0x25c880, 0x247c40,
	0x6d3000, 0x6c84c0, 0x6e5980, 0x6fed40, 0x6be300, 0x6a57c0, 0x688a80, 0x693e40,
	0x609600, 0x6122c0, 0x63ff80, 0x624b40, 0x664500, 0x67f1c0, 0x652c80, 0x649840,
	0x767c00, 0x77c8c0, 0x751580, 0x74a140, 0x70af00, 0x711bc0, 0x73c680, 0x727240,
	0x7bda00, 0x7a6ec0, 0x78b380, 0x790740, 0x7d0900, 0x7cbdc0, 0x7e6080, 0x7fd440,
	0x5ba800, 0x5a1cc0, 0x58c180, 0x597540, 0x5d7b00, 0x5ccfc0, 0x5e1280, 0x5fa640,
	0x560e00, 0x57bac0, 0x556780, 0x54d340, 0x50dd00, 0x5169c0, 0x53b480, 0x520040,
	0x40e400, 0x4150c0, 0x438d80, 0x423940, 0x463700, 0x4783c0, 0x455e80, 0x44ea40,
	0x4d4200, 0x4cf6c0, 0x4e2b80, 0x4f9f40, 0x4b9100, 0x4a25c0, 0x48f880, 0x494c40,
	0xda6000, 0xdbd4c0, 0xd90980, 0xd8bd40, 0xdcb300, 0xdd07c0, 0xdfda80, 0xde6e40,
	0xd7c600, 0xd672c0, 0xd4af80, 0xd51b40, 0xd11500, 0xd0a1c0, 0xd27c80, 0xd3c840,
	0xc12c00, 0xc098c0, 0xc24580, 0xc3f140, 0xc7ff00, 0xc64bc0, 0xc49680, 0xc52240,
	0xcc8a00, 0xcd3ec0, 0xcfe380, 0xce5740, 0xca5900, 0xcbedc0, 0xc93080, 0xc88440,
	0xecf800, 0xed4cc0, 0xef9180, 0xee2540, 0xea2b00, 0xeb9fc0, 0xe94280, 0xe8f640,
	0xe15e00, 0xe0eac0, 0xe23780, 0xe38340, 0xe78d00, 0xe639c0, 0xe4e480, 0xe55040,
	0xf7b400, 0xf600c0, 0xf4dd80, 0xf56940, 0xf16700, 0xf0d3c0, 0xf20e80, 0xf3ba40,
	0xfa1200, 0xfba6c0, 0xf97b80, 0xf8cf40, 0xfcc100, 0xfd75c0, 0xffa880, 0xfe1c40,
	0xb75000, 0xb6e4c0, 0xb43980, 0xb58d40, 0xb18300, 0xb037c0, 0xb2ea80, 0xb35e40,
	0xbaf600, 0xbb42c0, 0xb99f80, 0xb82b40, 0xbc2500, 0xbd91c0, 0xbf4c80, 0xbef840,
	0xac1c00, 0xada8c0, 0xaf7580, 0xaec140, 0xaacf00, 0xab7bc0, 0xa9a680, 0xa81240,
	0xa1ba00, 0xa00ec0, 0xa2d380, 0xa36740, 0xa76900, 0xa6ddc0, 0xa40080, 0xa5b440,
	0x81c800, 0x807cc0, 0x82a180, 0x831540, 0x871b00, 0x86afc0, 0x847280, 0x85c640,
	0x8c6e00, 0x8ddac0, 0x8f0780, 0x8eb340, 0x8abd00, 0x8b09c0, 0x89d480, 0x886040,
	0x9a8400, 0x9b30c0, 0x99ed80, 0x985940, 0x9c5700, 0x9de3c0, 0x9f3e80, 0x9e8a40,
	0x972200, 0x9696c0, 0x944b80, 0x95ff40, 0x91f100, 0x9045c0, 0x929880, 0x932c40
};

/*
 * Calculate a BTLE CRC one byte at a time. Thanks to Dominic Spill and
 * Michael Ossmann for writing and optimizing this.
 *
 * Arguments: CRCInit, pointer to start of packet, length of packet in
 * bytes
 * */
u32 btle_crcgen_lut(u32 crc_init, u8 *data, int len) {
	u32 state;
	int i;
	u8 key;

	state = crc_init & 0xffffff;
	for (i = 0; i < len; ++i) {
		key = data[i] ^ (state & 0xff);
		state = (state >> 8) ^ btle_crc_lut[key];
	}
	return state;
}

/*
 * The whitening sequence packed one bit to a bit, least significant bit
 * first, so that 32 bits of it can be read at once instead of looked up one
 * at a time. It holds one period and 31 bits more, which means any 32-bit
 * window of the period is contiguous and no read has to wrap around.
 *
 * It is built from whitening[] on first use rather than written out as a
 * second copy of the sequence: a copy in the source could disagree with the
 * one above it, and the 20 bytes this takes do the work of a 1920-byte table
 * of ready-made words for 40 channels, which only reached 48 bytes into a
 * packet anyway.
 *
 * On first use rather than at a mode start because the callers are in two
 * different capture engines, so there is no one place a mode start passes
 * through. It costs one pass of 158 iterations per boot, on the first packet
 * of the first low-energy capture, while the receive DMA goes on filling the
 * buffer behind it. Both callers run from a mode loop, never from an
 * interrupt, so neither can find it half built.
 */
#define WHITENING_PACKED_BITS (sizeof(whitening) + 31)
#define WHITENING_WORD_COUNT ((WHITENING_PACKED_BITS + 31) / 32)
static u32 whitening_packed[WHITENING_WORD_COUNT];
static int whitening_packed_ready;

static void pack_whitening(void) {
	unsigned i;

	for (i = 0; i < WHITENING_PACKED_BITS; ++i)
		whitening_packed[i / 32] |=
			(u32)whitening[i % sizeof(whitening)] << (i % 32);
	whitening_packed_ready = 1;
}

/* The 32 bits of the sequence that start at `idx`, packed the same way.
 *
 * The window can start anywhere in a word, so it is taken out of the pair of
 * words holding it. Shifting the 64-bit pair, rather than the two words
 * separately, is what makes a window that starts exactly on a word boundary
 * no special case: shifting a 32-bit word by 32 places is undefined. */
static u32 whitening_word(unsigned idx) {
	unsigned word = idx / 32;
	u64 pair = (u64)whitening_packed[word]
	         | (u64)whitening_packed[word + 1] << 32;

	return (u32)(pair >> (idx % 32));
}

/*
 * Dewhiten and reverse the bit order of part of a PDU in place, starting
 * `offset` bytes into it. A caller that has already dewhitened the front of a
 * packet some other way carries on from where it stopped.
 *
 * Four bytes at a time: rbit() reverses all 32 bits of a word, which reverses
 * each byte and puts the four in the opposite order, so the bytes go in most
 * significant first and come out least significant first. The whitening word
 * is then one XOR for all four, where it used to be eight lookups and a
 * modulo per byte -- which is what a 251-byte payload pays 250 times over.
 *
 * Channel is a physical channel in the range [BT_FREQ_MIN, BT_FREQ_MAX].
 */
void le_dewhiten_from(uint8_t *data, unsigned size, unsigned channel,
                      unsigned offset) {
	unsigned i = 0;
	unsigned idx = whitening_index[btle_channel_index(channel)];

	if (!whitening_packed_ready)
		pack_whitening();

	/* The whitening is one long bit sequence laid over the whole PDU, so a
	 * part of a packet can only be dewhitened once the sequence has been
	 * wound forward to where that part starts: 8 bits per byte skipped. */
	idx = (idx + 8 * offset) % sizeof(whitening);

	for (; i + 4 <= size; i += 4) {
		u32 v = (u32)data[i + 0] << 24
		      | (u32)data[i + 1] << 16
		      | (u32)data[i + 2] <<  8
		      | (u32)data[i + 3] <<  0;
		u32 out = rbit(v) ^ whitening_word(idx);

		data[i + 0] = (uint8_t)(out >>  0);
		data[i + 1] = (uint8_t)(out >>  8);
		data[i + 2] = (uint8_t)(out >> 16);
		data[i + 3] = (uint8_t)(out >> 24);
		idx = (idx + 32) % sizeof(whitening);
	}

	/* The last one to three bytes, each from the low byte of a word of the
	 * sequence: a buffer's length is whatever the packet in it is. */
	for (; i < size; ++i) {
		data[i] = (uint8_t)(rbit((u32)data[i] << 24)
		                    ^ whitening_word(idx));
		idx = (idx + 8) % sizeof(whitening);
	}
}

void le_dewhiten(uint8_t *data, unsigned size, unsigned channel) {
	le_dewhiten_from(data, size, channel, 0);
}

/*
 * Parse a channel map and populate the le_channel_remapping_t struct.
 */
void le_parse_channel_map(uint8_t *channel_map, le_channel_remapping_t *remapping) {
	unsigned i, j, byte;
	unsigned idx = 0;

	memset(remapping, 0, sizeof(*remapping));

	for (i = 0; i < 5; ++i) {
		byte = channel_map[i];
		for (j = 0; j < 8; ++j) {
			if (byte & 1) {
				remapping->channel_in_use[idx] = 1;
				remapping->remapping_index[remapping->total_channels] = idx;
				++remapping->total_channels;
			} else {
				remapping->channel_in_use[idx] = 0;
			}

			byte >>= 1;

			++idx;
			if (idx == 37)
				break;
		}
	}
}

/*
 * Map a channel index to a used index given a remapping struct.
 *
 * Both handlers that fill a remapping -- the one for a connection request and
 * the one for a channel map update -- refuse a map with no channels in it, so
 * the count is at least one by the time it arrives here. The guard below is a
 * backstop, and
 * it runs once per connection event. Without it a count of zero would be
 * silent: the processor's divide-by-zero trap is off at its reset value, so
 * the remainder would come out 0 and park a followed connection on data
 * channel 0.
 */
uint8_t le_map_channel(uint8_t channel_idx, le_channel_remapping_t *remapping) {
	if (remapping->total_channels == 0)
		return channel_idx;

	if (remapping->channel_in_use[channel_idx])
		return channel_idx;
	else
		return remapping->remapping_index[channel_idx % remapping->total_channels];
}

/*
 * The permutation channel selection algorithm 2 is built on: the bits inside
 * each octet are reversed and the two octets stay where they are. Reversing
 * all sixteen is the easy mistake and it matches none of the specification's
 * sample data.
 */
static uint8_t le_reverse_octet(uint8_t b) {
	b = (uint8_t)(((b & 0xf0) >> 4) | ((b & 0x0f) << 4));
	b = (uint8_t)(((b & 0xcc) >> 2) | ((b & 0x33) << 2));
	return (uint8_t)(((b & 0xaa) >> 1) | ((b & 0x55) << 1));
}

/*
 * The pseudo-random number an event is given, Core 5.4, Vol 6, Part B,
 * 4.5.8.3.3: three rounds of the permutation above and "multiply, add,
 * modulo", which is 17 * v + channelIdentifier kept to sixteen bits, with the
 * identifier mixed in at both ends.
 */
static uint16_t le_csa2_prn_e(uint16_t event_counter, uint16_t channel_id) {
	uint16_t v = event_counter ^ channel_id;
	unsigned i;

	for (i = 0; i < 3; ++i) {
		v = (uint16_t)((le_reverse_octet(v >> 8) << 8) |
				le_reverse_octet(v & 0xff));
		v = (uint16_t)(17u * v + channel_id);
	}

	return v ^ channel_id;
}

/*
 * The channel of one connection event under channel selection algorithm 2
 * (4.5.8.3), which a connection uses when both ends said they support it.
 *
 * Algorithm 1's channel is a running sum of the hop increment, so a follower
 * has to count every event and stay in step. This one is a function of the
 * event counter and the access address alone: an event the follower never saw
 * costs it nothing, because there is no state to carry.
 */
/* The channel identifier the algorithm gives an access address: its two
 * halves exclusive-ored together. */
static uint16_t le_csa2_channel_id(uint32_t access_address) {
	return (uint16_t)(((access_address >> 16) & 0xffff) ^
			(access_address & 0xffff));
}

/*
 * The channel algorithm 2 picks for an event before the channel map has its
 * say, which is also the channel of a connection whose map keeps all 37.
 *
 * It depends on the access address and the event counter alone. That is what
 * lets promiscuous recovery search the event counter of a connection whose
 * map it has never seen.
 */
uint8_t le_csa2_unmapped_channel(uint16_t event_counter,
                                 uint32_t access_address) {
	return (uint8_t)(le_csa2_prn_e(event_counter,
			le_csa2_channel_id(access_address)) % 37);
}

uint8_t le_csa2_channel(uint16_t event_counter, uint32_t access_address,
                        le_channel_remapping_t *remapping) {
	uint16_t channel_id = le_csa2_channel_id(access_address);
	uint16_t prn = le_csa2_prn_e(event_counter, channel_id);
	uint8_t unmapped = (uint8_t)(prn % 37);

	/* The backstop le_map_channel() carries, for the same reason: with the
	 * divide-by-zero trap off at its reset value, a count of zero would
	 * park a followed connection on data channel 0. */
	if (remapping->total_channels == 0)
		return unmapped;

	if (remapping->channel_in_use[unmapped])
		return unmapped;

	/* Where algorithm 1 reduces the channel it landed on, this one takes
	 * its place in the table from the whole pseudo-random number. */
	return remapping->remapping_index[
			(remapping->total_channels * (uint32_t)prn) >> 16];
}

/*
 * How far two readings of one span may be apart and still count as the same
 * span. The connection's own clock may be 500 ppm out and the board's clock
 * adds to that, so allow about 0.2% of the span, plus half of the 1.25 ms a
 * connection interval is counted in so that rounding alone never splits two
 * readings of the same span.
 *
 * Both estimators in the promiscuous mode measure spans against a connection
 * interval and need the same allowance.
 */
u32 le_gap_tolerance(u32 span) {
	return (span >> 9) + LE_CONN_INTERVAL_TICKS / 2;
}

/* Are these two spans the same one, read twice? */
int le_same_span(u32 a, u32 b) {
	u32 shorter = a < b ? a : b;
	u32 apart = a < b ? b - a : a - b;

	return apart <= le_gap_tolerance(shorter);
}

/* Are these two spans the same one, read twice, allowing for which packet of
 * an event each sighting was? Only the algorithm-2 search needs the second
 * allowance: the estimator it shares le_same_span() with looks for a pattern
 * the gaps repeat rather than measuring each one against an interval. */
static int le_csa2_same_span(u32 a, u32 b) {
	u32 shorter = a < b ? a : b;
	u32 apart = a < b ? b - a : a - b;

	return apart <= le_gap_tolerance(shorter) + LE_CSA2_SIGHTING_SPREAD;
}

/* How many of the gaps a candidate connection interval accounts for. Every
 * gap between two sightings on one channel is a whole number of connection
 * events, so it has to be a whole number of the interval. */
static unsigned le_csa2_interval_score(const u32 *gap, unsigned gap_count,
                                       u32 interval) {
	u32 unit = interval * LE_CONN_INTERVAL_TICKS;
	unsigned i, score = 0;

	for (i = 0; i < gap_count; ++i) {
		u32 events;

		/* Longer than the connection can be away and still be coming
		 * back, and the one bound the multiplication below needs: the
		 * product is about the gap itself, so a gap near the top of
		 * the clock's range could carry it past what fits. */
		if (gap[i] > LE_PROMISC_MAX_GAP)
			continue;

		events = LE_DIVIDE_ROUND(gap[i], unit);
		if (events < 1 || events > LE_CSA2_MAX_EVENT_GAP)
			continue;
		if (le_csa2_same_span(gap[i], events * unit))
			++score;
	}

	return score;
}

/*
 * The connection interval the gaps say, in units of 1.25 ms, or 0 when they
 * do not say one that can be believed.
 *
 * Any gap divided by the number of events it spans is a candidate, so trying
 * every gap against every event count a gap may span offers the right answer
 * among a few thousand. Each is then scored against all the gaps.
 *
 * Every whole fraction of the right interval divides the same gaps and scores
 * the same, so a tie goes to the longest. A multiple of it does not divide
 * them unless every gap happens to span a multiple of that many events, which
 * two dozen gaps make very unlikely.
 *
 * The answer is the interval as the board's clock sees it, which is what the
 * follow has to hop on, so at a long interval it can be a unit away from the
 * figure the connection was given: the two clocks differ by a few hundred
 * parts per million and a unit is a smaller difference than that. The follow
 * wants the board's figure, not the connection's.
 */
uint16_t le_csa2_interval(const u32 *gap, unsigned gap_count) {
	unsigned i, k;
	unsigned best_score = 0;
	u32 best = 0;

	/* Too few gaps for agreement between them to mean anything. The bar
	 * for a counter is ten sightings, and an interval is read from the
	 * gaps between sightings, so it takes the same number. */
	if (gap_count + 1 < LE_CSA2_MIN_SCORE)
		return 0;

	for (i = 0; i < gap_count; ++i) {
		if (gap[i] > LE_PROMISC_MAX_GAP)
			continue;

		for (k = 1; k <= LE_CSA2_MAX_EVENT_GAP; ++k) {
			u32 interval = LE_DIVIDE_ROUND(gap[i],
					k * LE_CONN_INTERVAL_TICKS);
			unsigned score;

			/* The connection's clock runs a little fast or slow
			 * against the board's, so a gap divided by its event
			 * count lands near the whole number of units the
			 * connection was given rather than on it. One unit
			 * above the longest interval is the one place where
			 * that costs the answer: the real interval would be
			 * left out and a half or a third of it would win the
			 * tie below. */
			if (interval == LE_MAX_CONN_INTERVAL + 1)
				interval = LE_MAX_CONN_INTERVAL;

			if (interval < LE_MIN_CONN_INTERVAL ||
					interval > LE_MAX_CONN_INTERVAL)
				continue;

			score = le_csa2_interval_score(gap, gap_count,
					interval);
			if (score > best_score ||
					(score == best_score && interval > best)) {
				best_score = score;
				best = interval;
			}
		}
	}

	/* All but a few of the gaps have to fit, and most of them: the fixed
	 * allowance alone would pass an interval backed by four gaps out of
	 * nine, which is the fewest this is asked to work from. */
	if (best_score + LE_CSA2_INTERVAL_MISSES < gap_count ||
			best_score * 4 < gap_count * 3)
		return 0;

	return (uint16_t)best;
}

/*
 * The event counters whose unmapped channel is the one being listened to, in
 * ascending order, and where each 256-wide block of counters begins in that
 * list.
 *
 * How many there are is known exactly, not estimated. le_csa2_prn_e() is one
 * to one in the event counter: reversing the bits inside each octet is, and so
 * is 17 * v + channelIdentifier, because 17 is odd. Over the whole range of
 * the counter the pseudo-random number therefore takes every value once, and
 * 65536 = 37 * 1771 + 9, so every channel has 1771 counters and nine of them
 * have 1772. That holds whatever channel map the connection uses, because the
 * unmapped channel does not depend on the map.
 *
 * Together they are 4 KB, which is more than the main RAM bank has to spare
 * next to the stack, so they live in the other bank. The block index costs
 * half a kilobyte of that and narrows a lookup to the seven or so entries a
 * block holds, against the eleven steps a binary search over all 1772 would
 * take; a search makes up to a million lookups, which is the time the
 * receiver spends deaf.
 */
#define LE_CSA2_HITS 1772
#define LE_CSA2_BUCKETS 256
static AHB_SRAM uint16_t le_csa2_hit[LE_CSA2_HITS];
static AHB_SRAM uint16_t le_csa2_bucket[LE_CSA2_BUCKETS + 1];
static unsigned le_csa2_hit_count;

/* Fill the list above for one channel and access address. */
static void le_csa2_build_hits(uint8_t channel_idx, u32 access_address) {
	u32 counter;
	unsigned bucket = 0;

	le_csa2_hit_count = 0;
	le_csa2_bucket[0] = 0;

	for (counter = 0; counter < 65536; ++counter) {
		while (bucket < (counter >> 8))
			le_csa2_bucket[++bucket] = (uint16_t)le_csa2_hit_count;

		if (le_csa2_unmapped_channel((uint16_t)counter,
				access_address) != channel_idx)
			continue;

		/* Cannot fire, by the counting above; the list would be
		 * truncated and the search would miss the right counter rather
		 * than answer wrongly. */
		if (le_csa2_hit_count == LE_CSA2_HITS)
			break;
		le_csa2_hit[le_csa2_hit_count++] = (uint16_t)counter;
	}

	while (bucket < LE_CSA2_BUCKETS)
		le_csa2_bucket[++bucket] = (uint16_t)le_csa2_hit_count;
}

/* Is this event counter one of them? The block index narrows it to the
 * handful of entries that can match, which is what makes scoring tens of
 * thousands of candidates affordable. */
static int le_csa2_is_hit(uint16_t counter) {
	unsigned i = le_csa2_bucket[counter >> 8];
	unsigned end = le_csa2_bucket[(counter >> 8) + 1];

	for (; i < end; ++i) {
		if (le_csa2_hit[i] == counter)
			return 1;
		if (le_csa2_hit[i] > counter)
			break;
	}

	return 0;
}

/* How many of the sightings a candidate counter accounts for. `back[j]` is
 * how many events before the newest sighting the jth one was, so the
 * candidate is the counter of the newest. Counters wrap at 16 bits and the
 * subtraction wraps with them. */
static unsigned le_csa2_counter_score(uint16_t counter, const uint16_t *back,
                                      unsigned count) {
	unsigned j, score = 0;

	for (j = 0; j < count; ++j)
		if (le_csa2_is_hit((uint16_t)(counter - back[j])))
			++score;

	return score;
}

int le_csa2_recover(const u32 *gap, unsigned gap_count, uint8_t channel_idx,
                    u32 access_address, le_csa2_recovery_t *out) {
	/* How many events before the newest sighting each sighting was, newest
	 * first. */
	uint16_t back[LE_CSA2_SIGHTINGS];
	unsigned count = 1;
	unsigned newest;
	uint16_t interval;
	u32 unit;
	unsigned i, h;
	unsigned best = 0, runner_up = 0;
	uint16_t best_counter = 0;

	memset(out, 0, sizeof(*out));

	if (channel_idx >= LE_HOP_CHANNELS || gap_count + 1 < LE_CSA2_MIN_SCORE ||
			gap_count + 1 > LE_CSA2_SIGHTINGS)
		return 0;

	/*
	 * The interval the connection is on now, read from the newest gaps
	 * rather than all of them.
	 *
	 * A connection may renegotiate its interval part-way through a run of
	 * sightings, and doubling or halving it is ordinary. An interval the
	 * connection has left still divides every gap after such a change --
	 * every multiple of twice a number is a multiple of it -- so against
	 * the whole run the interval it has left fits everything and the one
	 * it moved to fits only the newer half, and the old one wins. The
	 * answer belongs to the newest sighting and the follow has to hop on
	 * what the connection is using now, so the newest gaps are the ones
	 * that decide. The walk back below then stops where they stop fitting,
	 * and the scoring, anchored on the newest sighting, declines when too
	 * little of the run is left.
	 */
	newest = gap_count / 2;
	if (newest < LE_CSA2_MIN_SCORE - 1)
		newest = LE_CSA2_MIN_SCORE - 1;
	if (newest > gap_count)
		newest = gap_count;

	interval = le_csa2_interval(gap + (gap_count - newest), newest);
	if (interval == 0)
		return 0;

	unit = (u32)interval * LE_CONN_INTERVAL_TICKS;

	/* Turn the gaps into event offsets, working back from the newest
	 * sighting. A gap the interval does not account for ends the run:
	 * every offset beyond it is measured through it, so one such gap would
	 * move them all. Working back from the newest keeps the sightings
	 * nearest the hand-over, which is where the follow picks the
	 * connection up. */
	back[0] = 0;
	for (i = gap_count; i-- > 0; ) {
		u32 events;

		if (gap[i] > LE_PROMISC_MAX_GAP)
			break;

		events = LE_DIVIDE_ROUND(gap[i], unit);
		if (events < 1 || events > LE_CSA2_MAX_EVENT_GAP ||
				!le_csa2_same_span(gap[i], events * unit))
			break;
		back[count] = (uint16_t)(back[count - 1] + events);
		++count;
	}

	if (count < LE_CSA2_MIN_SCORE)
		return 0;

	le_csa2_build_hits(channel_idx, access_address);

	/*
	 * Every counter that puts the newest sighting on this channel, scored
	 * against all of them.
	 *
	 * The answer is anchored on the newest sighting because that is the one
	 * the follow starts from, and because it is the only sighting whose
	 * place in the run is not read through a gap. A counter that explains
	 * the older sightings but not the newest one is what a run spanning a
	 * change of connection parameters offers: the interval read above is
	 * the one in use now, so the gaps before the change are measured
	 * wrongly, every sighting behind it moves by one constant, and a
	 * counter fits those and nothing else. Scored from the newest sighting
	 * such a counter reaches only the sightings after the change, which is
	 * fewer than the bar, and the search declines -- which is the right
	 * answer, because the run holds too little of the connection as it now
	 * is to recover it.
	 *
	 * The price is on a connection using fewer than the 37 channels, where
	 * the newest sighting may be a visit the map moved here and no counter
	 * puts it on this channel. Then nothing is adopted and the mode
	 * searches again on the next run of sightings, where the newest
	 * sighting is a fresh draw.
	 *
	 * What this costs, which is time the receiver is deaf: the list above
	 * takes 65536 of the algorithm's hashes to build, and the scoring takes
	 * 1772 * LE_CSA2_SIGHTINGS lookups, about 43 thousand.
	 */
	for (h = 0; h < le_csa2_hit_count; ++h) {
		uint16_t counter = le_csa2_hit[h];
		unsigned score = le_csa2_counter_score(counter, back, count);

		if (score > best) {
			runner_up = best;
			best = score;
			best_counter = counter;
		} else if (score > runner_up) {
			runner_up = score;
		}
	}

	out->sightings = (u8)count;
	out->score = (u8)best;
	out->runner_up = (u8)runner_up;

	if (best < LE_CSA2_MIN_SCORE || best < runner_up + LE_CSA2_MIN_MARGIN)
		return 0;

	out->conn_interval = interval;
	out->event_counter = best_counter;
	return 1;
}

/*
 * Where the follow picks a recovered connection up. Returns whether the
 * recovery can be followed at all.
 *
 * The search is not instant -- it takes a couple of hundred milliseconds, and
 * the receiver is deaf for all of it -- so by the time an answer exists the
 * connection has had further events. `elapsed` is how long ago the sighting
 * the answer belongs to was, in 100 ns ticks. This carries the event counter
 * forward over the events that went by in the meantime and says how long to
 * wait before hopping to the next one.
 *
 * The hop lands at or just before that event's start, never after it: the
 * wait is rounded down to the 1.25 ms the interval timer counts in, so the
 * receiver is already on the right channel when the event begins and sits
 * there for the whole of it.
 */
int le_csa2_handoff(const le_csa2_recovery_t *rec, u32 elapsed,
                    le_csa2_handoff_t *out) {
	u32 unit, to_next_event;
	u16 events_ahead;

	memset(out, 0, sizeof(*out));

	/* A successful search answers with an interval a connection may use,
	 * but the division below is unforgiving enough to check anyway: the
	 * LPC17xx leaves DIV_0_TRP clear, so a zero divisor makes UDIV answer
	 * 0 rather than fault, and the follow would be set up to hop on a
	 * timer that never expires. */
	if (rec->conn_interval < LE_MIN_CONN_INTERVAL ||
			rec->conn_interval > LE_MAX_CONN_INTERVAL)
		return 0;

	unit = (u32)rec->conn_interval * LE_CONN_INTERVAL_TICKS;
	to_next_event = unit - elapsed % unit;

	/* An event already closer than one tick of the interval timer is
	 * skipped for the one after it: a hop cannot be scheduled finer than
	 * that, so aiming at this one would land inside it. */
	events_ahead = 1;
	if (to_next_event < LE_CONN_INTERVAL_TICKS) {
		to_next_event += unit;
		events_ahead = 2;
	}

	out->event_counter = (u16)(rec->event_counter + elapsed / unit +
			events_ahead);
	/* The follow's convention is that its counter holds the event now
	 * running, where event_counter above is the counter of the next one,
	 * so it starts a step behind; the clock interrupt advances it at the
	 * start of each event and keeps the two in step from there. */
	out->conn_count = (u16)(out->event_counter - 1);
	out->interval_timer = (u16)(to_next_event / LE_CONN_INTERVAL_TICKS);
	return 1;
}
