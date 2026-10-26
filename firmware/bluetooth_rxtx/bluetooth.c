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

#include "bluetooth.h"

bdaddr target;
u8 afh_enabled;
u8 afh_map[10];
u8 used_channels;

/* these values for hop() can be precalculated (at leastin part) */
u8 a1, b, c1, e;
u16 d1;
/*
 * Basic mapping table (Core spec Vol 2 Part B 2.6.2): even channels in
 * ascending order followed by odd channels.  The frequency is 2402 + entry MHz.
 */
u8 bank[NUM_BREDR_CHANNELS];
/*
 * AFH mapping table (2.6.3.1): the basic table with every unused channel
 * removed, holding used_channels entries.  It is kept separate so that bank[]
 * always describes the full basic sequence, which AFH still consults first.
 */
u8 afh_bank[NUM_BREDR_CHANNELS];

/* count the number of 1 bits in a uint64_t */
static uint8_t count_bits(uint64_t n)
{
	uint8_t i = 0;
	for (i = 0; n != 0; i++)
		n &= n - 1;
	return i;
}

/* nonzero if the AFH channel map marks this channel (0-78) as used */
static u8 channel_used(u8 chan)
{
	return (afh_map[chan / 8] >> (chan % 8)) & 0x1;
}

/*
 * Do all of the one time precalculation.
 *
 * This rebuilds bank[], afh_bank[] and used_channels in place, which next_hop()
 * reads. It is safe only because both run in one context: a control request
 * reaches it through bluetooth_set_afh_map(), and the BR/EDR modes that hop
 * poll USB from their own loops, so the request runs between next_hop() calls
 * and never during one. A mode that serviced USB from the USB interrupt
 * instead could take the request mid-hop and read a half-rebuilt table; such a
 * mode would have to rebuild into a second table and swap it in, or defer the
 * change to the next hop.
 */
void precalc(void)
{
	u8 i, j, chan;
	u32 address;
	address = target.address & 0xffffffff;

	/* populate frequency register bank*/
	for (i = 0; i < NUM_BREDR_CHANNELS; i++)
		bank[i] = ((i * 2) % NUM_BREDR_CHANNELS);
		/* actual frequency is 2402 + bank[i] MHz */


	/* precalculate some of next_hop()'s variables */
	a1 = (address >> 23) & 0x1f;
	b = (address >> 19) & 0x0f;
	c1 = ((address >> 4) & 0x10) +
		((address >> 3) & 0x08) +
		((address >> 2) & 0x04) +
		((address >> 1) & 0x02) +
		(address & 0x01);
	d1 = (address >> 10) & 0x1ff;
	e = ((address >> 7) & 0x40) +
		((address >> 6) & 0x20) +
		((address >> 5) & 0x10) +
		((address >> 4) & 0x08) +
		((address >> 3) & 0x04) +
		((address >> 2) & 0x02) +
		((address >> 1) & 0x01);

	if(afh_enabled) {
		/* Count only entries actually placed in the table, so a stray
		 * reserved bit 79 in the map cannot index past the used channels. */
		j = 0;
		for (i = 0; i < NUM_BREDR_CHANNELS; i++) {
			chan = bank[i];
			if (channel_used(chan))
				afh_bank[j++] = chan;
		}
		used_channels = j;
	}
}

/* Apply an AFH channel map and rebuild the AFH mapping table.  Hosts send the
 * map after the address, so the table must be rebuilt here rather than only
 * when the address is set. */
void bluetooth_set_afh_map(const u8 *map)
{
	u8 i;
	for (i = 0; i < 10; i++)
		afh_map[i] = map[i];
	afh_enabled = 1;
	precalc();
}

/* 5 bit permutation */
static u8 perm5(u8 z, u8 p_high, u16 p_low)
{
	/* z is constrained to 5 bits, p_high to 5 bits, p_low to 9 bits */
	z &= 0x1f;
	p_high &= 0x1f;
	p_low &= 0x1ff;

	int i;
	u8 tmp, output, z_bit[5], p[14];
	static const u8 index1[] = {0, 2, 1, 3, 0, 1, 0, 3, 1, 0, 2, 1, 0, 1};
	static const u8 index2[] = {1, 3, 2, 4, 4, 3, 2, 4, 4, 3, 4, 3, 3, 2};

	/* bits of p_low and p_high are control signals */
	for (i = 0; i < 9; i++)
		p[i] = (p_low >> i) & 0x01;
	for (i = 0; i < 5; i++)
		p[i+9] = (p_high >> i) & 0x01;

	/* bit swapping will be easier with an array of bits */
	for (i = 0; i < 5; i++)
		z_bit[i] = (z >> i) & 0x01;

	/* butterfly operations */
	for (i = 13; i >= 0; i--) {
		/* swap bits according to index arrays if control signal tells us to */
		if (p[i]) {
			tmp = z_bit[index1[i]];
			z_bit[index1[i]] = z_bit[index2[i]];
			z_bit[index2[i]] = tmp;
		}
	}

	/* reconstruct output from rearranged bits */
	output = 0;
	for (i = 0; i < 5; i++)
		output += z_bit[i] << i;

	return output;
}

u16 next_hop(u32 clock)
{
	u8 a, c, x, y1, perm, next_channel;
	u16 d, y2;
	u32 base_f, f, f_dash;
	/* Read once: a map update from the USB interrupt must not change N
	 * between the check and the divisions below. */
	u8 n_used = used_channels;
	/* Fewer than the spec minimum of 20 used channels (or an unset map)
	 * falls back to basic hopping, which also avoids dividing by zero. */
	u8 afh_active = afh_enabled && n_used >= 20;

	clock &= 0xffffffff;
	/* AFH same channel mechanism (2.6.1): the peripheral-to-central slot
	 * reuses the preceding central-to-peripheral channel, i.e. CLK1 = 0. */
	if (afh_active)
		clock &= ~(u32)0x2;
	/* Variable names used in Vol 2, Part B, Section 2.6 of the spec */
	x = (clock >> 2) & 0x1f;
	y1 = (clock >> 1) & 0x01;
	y2 = y1 << 5;
	a = (a1 ^ (clock >> 21)) & 0x1f;
	/* b is already defined */
	c = (c1 ^ (clock >> 16)) & 0x1f;
	d = (d1 ^ (clock >> 7)) & 0x1ff;
	/* e is already defined */
	base_f = (clock >> 3) & 0x1fffff0;
	f = base_f % 79;

	perm = perm5(
		((x + a) % 32) ^ b,
		(y1 * 0x1f) ^ c,
		d);
	/* hop selection */
	next_channel = bank[(perm + e + f + y2) % NUM_BREDR_CHANNELS];
	/* AFH re-mapping (2.6.3.1): a used basic channel is kept unchanged; an
	 * unused one is replaced by entry k' = (PERM5 + E + F' + Y2) mod N of the
	 * AFH mapping table, where F' = 16 * CLK27-7 mod N. */
	if (afh_active && !channel_used(next_channel)) {
		f_dash = base_f % n_used;
		next_channel = afh_bank[(perm + e + f_dash + y2) % n_used];
	}
	return (2402 + next_channel);

}

/*
 * Software access-code (AC) search over one demodulated symbol buffer of
 * DMA_SIZE bytes, most significant bit of each byte first. Slides a 64-bit
 * window one symbol at a time and returns the bit offset just past the first
 * window within MAX_SYNCWORD_ERRS of target.syncword, or -1. Windows ending in
 * the last 64 symbols are not tested, leaving room for the packet header.
 *
 * Nothing currently calls this: live capture lets the CC2400 detect the sync
 * word in hardware. It is kept as a reference correlator.
 *
 * Each call searches only the buffer it is given, using a local window seeded
 * from that buffer's first 64 bits. An earlier version kept the window in a
 * file-scope variable and reseeded it only when it was zero, so later calls
 * compared bits left over from an earlier buffer. That carry was not a
 * working streaming
 * design: the window stopped 64 bits short of the previous buffer's end, the
 * buffers it was used on were not contiguous (the radio hops between them),
 * and the returned offset had no consistent origin. That version also
 * skipped the ninth byte of each buffer.
 */
int find_access_code(u8 *idle_rxbuf)
{
	u64 window = 0;
	int count;

	for (count = 0; count < 8; count++)
		window = (window << 8) | idle_rxbuf[count];

	for (count = 64; count < ((8 * DMA_SIZE) - 64); count++) {
		if (count_bits(window ^ target.syncword) < MAX_SYNCWORD_ERRS)
			return count;

		window = (window << 1) |
			((idle_rxbuf[count / 8] >> (7 - (count % 8))) & 1);
	}
	return -1;
}
