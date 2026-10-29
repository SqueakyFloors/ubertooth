/* Hop functions
 *
 * Copyright 2012 Dominic Spill
 * Copyright 2020 Etienne Helluy-Lafont, Univ. Lille, CNRS.
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
#include <ubtbr/cfg.h>
#include <ubtbr/debug.h>
#include <ubtbr/hop.h>
#include <ubtbr/btphy.h>

hop_state_t hop_state;

/* Nmin, the fewest used channels AFH is defined for
 * (BT Core v5.1 | Vol 2, Part B, Section 2.3.1). */
#define AFH_MIN_USED_CHANNELS	20

/* do all of the one time precalculation */
void hop_init(uint32_t address)
{
	uint8_t i;

	/* populate frequency register bank */
	for (i = 0; i < NUM_BREDR_CHANNELS; i++)
	{
		hop_state.basic_bank[i] = ((i * 2) % NUM_BREDR_CHANNELS);
	}
		/* actual frequency is 2402 + bank[i] MHz */

	/* precalculate some of next_hop()'s variables */
	hop_state.a27_23 = (address >> 23) & 0x1f;
	hop_state.a22_19 = (address >> 19) & 0x0f; 	// Addr22_19
	hop_state.C = ((address >> 4) & 0x10) |
			((address >> 3) & 0x08) |
			((address >> 2) & 0x04) |
			((address >> 1) & 0x02) |
			(address & 0x01);
	hop_state.a18_10 = (address >> 10) & 0x1ff;
	hop_state.E =	((address >> 7) & 0x40) +
			((address >> 6) & 0x20) +
			((address >> 5) & 0x10) +
			((address >> 4) & 0x08) +
			((address >> 3) & 0x04) +
			((address >> 2) & 0x02) +
			((address >> 1) & 0x01);
	hop_state.x = 0;
	hop_state.scan_x = 0;
	hop_state.afh_enabled = 0;
	hop_state.afh_chan_count = 0;
}

/*
 * BT Core v5.1 | Vol 2, Part B, Section 2.6.  Returns PERM5 + E + Y2, that is
 * the kernel output before input F of its final adder.  F is left to the
 * caller because AFH re-mapping (2.6.3.1) needs this same value with two
 * different F, and because adding F here would overflow a uint8_t.
 */
static uint8_t hop_selection_kernel(uint8_t x, uint8_t y1, uint8_t y2,
			uint8_t a, uint8_t b, uint8_t c,
			uint16_t d, uint8_t e)
{
	/* Extend y1 on 5 bits */
	y1 = ~(y1-1);
	return (perm5((a+x)^b, y1^c, d) + e + y2);
}

/*
 * The channel for every substate outside the connection state: the kernel with
 * the given X, Y1 and Y2, and F = 0, mapped through the basic table.
 */
static uint8_t hop_kernel_chan(uint8_t x, uint8_t y1, uint8_t y2)
{
	uint8_t sel = hop_selection_kernel(
			x,			// X
			y1,			// Y1
			y2,			// Y2
			hop_state.a27_23,	// A = A27_23
			hop_state.a22_19,	// B = A22_19
			hop_state.C,		// C = A8,6,4,2,0
			hop_state.a18_10,	// D = A18_10
			hop_state.E		// E = A13,11,9,7,5,3,1
			) % NUM_BREDR_CHANNELS;	// F = 0 in these states

	return hop_state.basic_bank[sel];
}

/*
 * The substates whose slot selects between transmit and receive: page,
 * inquiry, and the two page response substates.  Table 2.2 gives them
 * Y1 = CLK1 and Y2 = 32 * CLK1 -- CLK1 = 0 is a central transmit slot -- and
 * an X the state machine holds.  The clock is the caller's, and in each case
 * it is the one the two ends share: the pager's estimate of the paged device's
 * clock on the pager's side, and on the paged device's side the slot clock the
 * scan set to the page it heard.
 */
static uint8_t hop_inquiry_with_phase(uint32_t clk, uint8_t x)
{
	uint8_t clk1 = 1&(clk>>1);

	return hop_kernel_chan(x, clk1, clk1<<5);
}

uint8_t hop_inquiry(uint32_t clk)
{
	return hop_inquiry_with_phase(clk, hop_state.x);
}

/*
 * Page scan and inquiry scan.  Table 2.2 gives these two substates Y1 = 0 and
 * Y2 = 0, so a scanning device stays on one frequency whichever slot it is in,
 * and an X built from CLKN16-12 of its own clock, which changes once every
 * 1.28 seconds.  The clock of the slot being tuned does not come into it,
 * which is why this takes the native clock rather than a leg's clock.
 *
 * The inquiry scan row of the table is Xir, CLKN16-12 plus the response
 * counter N; the page scan row is CLKN16-12 alone.  This image leaves N at
 * zero, where the two are the same value: N exists to move a responder off a
 * frequency it has just answered on, which matters where several devices
 * answer one inquiry, and nothing here advances it.
 *
 * The phase is kept for hop_start_response(), because a response has to start
 * from the phase that heard the packet rather than from the clock as it reads
 * by the time the packet has been decoded.
 */
uint8_t hop_scan(uint32_t clkn)
{
	hop_state.scan_x = hop_scan_phase(clkn);
	return hop_kernel_chan(hop_state.scan_x, 0, 0);
}

/*
 * The inquiry response.  Table 2.2 fixes Y1 = 1 and Y2 = 32 here, whatever
 * slot the answer goes out in: it always takes the frequency that pairs with
 * the one the inquiry arrived on.  X is the response phase, which holds for
 * the extended inquiry response that may follow the FHS as well -- 2.6.4.6
 * says that packet takes the phase of the FHS before it.
 */
static uint8_t hop_inquiry_response(void)
{
	return hop_kernel_chan(hop_state.x, 1, 32);
}

static uint8_t hop_paging(uint32_t clk)
{
	return hop_inquiry_with_phase(clk, calc_iterate_slave_page_scan_phase(clk));
}

/* nonzero if the AFH channel map marks this channel (0-78) as used */
static uint8_t afh_channel_used(uint8_t chan)
{
	return (hop_state.afh_map[chan/8] >> (chan%8)) & 0x1;
}

static void hop_set_afh_map(const uint8_t *map)
{
	uint8_t chan;
	unsigned i,count;

	for (i = 0; i < sizeof(hop_state.afh_map); i++)
		hop_state.afh_map[i] = map[i];

	/* Walk the basic table so the AFH table keeps its channel order, and
	 * count only the entries actually placed in it, so a stray reserved
	 * bit 79 in the map cannot index past the used channels. */
	for (i = 0, count=0; i < NUM_BREDR_CHANNELS; i++) {
		chan = hop_state.basic_bank[i];
		if (afh_channel_used(chan))
			hop_state.afh_bank[count++] = chan;
	}
	hop_state.afh_chan_count = count;
}

void hop_cfg_afh(uint8_t* buf)
{
	uint8_t enabled = buf[0] & 1;
	uint8_t *map = buf+1;

	if (enabled)
	{
		cprintf("(+afh)");
		hop_set_afh_map(map);
		/* Say so rather than leave the board looking as if it ignored
		 * the map: below Nmin hop_basic() hops over all 79 channels. */
		if (hop_state.afh_chan_count < AFH_MIN_USED_CHANNELS)
			cprintf("(afh<%d)", AFH_MIN_USED_CHANNELS);
	}
	else
	{
		cprintf("(-afh)");
	}
	hop_state.afh_enabled = enabled;
}

uint8_t hop_basic(uint32_t clk)
{
	uint8_t clk1, sel, chan;
	uint32_t base_f;
	/* The AFH map, table and count are only ever set together, by
	 * hop_cfg_afh(), which runs from btphy_timers_execute() earlier in the
	 * same TIMER0 handler as the scheduler callback that hops, so what is
	 * read here is one consistent set. (hop_init() also clears the count,
	 * from the main loop, but that can only turn AFH off.) */
	uint8_t n_used = hop_state.afh_chan_count;
	/* Fewer used channels than the spec defines AFH for (or an unset map)
	 * falls back to basic hopping, which also avoids dividing by zero. */
	uint8_t afh_active = hop_state.afh_enabled &&
				n_used >= AFH_MIN_USED_CHANNELS;

	/* AFH same channel mechanism (2.6.1): the peripheral-to-central slot
	 * reuses the preceding central-to-peripheral channel, i.e. CLK1 = 0. */
	if (afh_active)
		clk1 = 0;
	else
		clk1 = 1&(clk>>1);

	base_f = (clk>>(7-4))&(0x1fffff<<4);		// 16 * clk27_7

	sel = hop_selection_kernel(
		0x1f & (clk>>2),			// X = clk6_2
		clk1,					// Y1 = clk1
		clk1<<5,				// Y2 = 32*clk1
		hop_state.a27_23^(0x1f&(clk>>21)),	// A = A27_23 ^ clk25_21
		hop_state.a22_19,			// B = A22_19
		hop_state.C^(0x1f&(clk>>16)),		// C = A8,6,4,2,0 ^ clk20_16
		hop_state.a18_10^(0x1ff&(clk>>7)),	// D = a18_10 ^ clk15_7
		hop_state.E				// E = A13,11,9,7,5,3,1
		);
	chan = hop_state.basic_bank[(sel + base_f % NUM_BREDR_CHANNELS)
					% NUM_BREDR_CHANNELS];
	/* AFH re-mapping (2.6.3.1): a used basic channel is kept unchanged; an
	 * unused one is replaced by entry k' = (PERM5 + E + F' + Y2) mod N of
	 * the AFH mapping table, where F' = 16 * clk27_7 mod N. */
	if (afh_active && !afh_channel_used(chan))
		chan = hop_state.afh_bank[(sel + base_f % n_used) % n_used];
	return chan;
}

uint8_t hop_channel(uint32_t clk)
{
	switch(btphy.mode)
	{
	case BT_MODE_INQUIRY:
	case BT_MODE_PAGING_RESPONSE:
	case BT_MODE_PERIPH_PAGE_RESPONSE:
		return hop_inquiry(clk);
	case BT_MODE_INQUIRY_SCAN:
	case BT_MODE_PAGE_SCAN:
		/* A scan hops on this device's own clock, so the clock of the
		 * slot being tuned is not the input here. */
		return hop_scan(btphy.master_clkn);
	case BT_MODE_INQUIRY_RESPONSE:
		return hop_inquiry_response();
	case BT_MODE_MASTER:
	case BT_MODE_SLAVE:
		return hop_basic(clk);
	case BT_MODE_PAGING:
		return hop_paging(clk);
	default:
		DIE("Invalid hop mode %d\n", btphy.mode);
	}
}
