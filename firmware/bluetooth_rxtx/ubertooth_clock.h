/*
 * Copyright 2015 Hannes Ellinger
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

#ifndef __UBERTOOTH_CLOCK_H
#define __UBERTOOTH_CLOCK_H value

#include "inttypes.h"

/* CLK100NS_PER_CLKN and CLK100NS_PERIOD: the host reconstructs capture times
 * from the same numbers, so they live in the shared interface header. */
#include "ubertooth_interface.h"

/*
 * CLK100NS is a free-running clock with a period of 100 ns.  It resets every
 * CLK100NS_PERIOD cycles (about 5.5 minutes) - computed from clkn and
 * timer0 (T0TC)
 *
 * clkn is the native (local) clock as defined in the Bluetooth specification.
 * It advances 3200 times per second.  Two clkn periods make a Bluetooth time
 * slot.
 */
extern volatile uint32_t clkn;
extern volatile uint32_t last_hop;

extern volatile uint32_t clkn_offset;
extern volatile uint16_t clk100ns_offset;

// linear clock drift
extern volatile int16_t clk_drift_ppm;
extern volatile uint16_t clk_drift_correction;

/*
 * The largest drift the correction can act on, in parts per million, either
 * way.
 *
 * The correction adds or removes one 100 ns tick every clk_drift_correction
 * CLKN ticks, and that interval is CLK_DRIFT_PPM_MAX divided by the drift: at
 * the bound the interval is one tick, which is as often as the tick handler
 * runs. A larger figure cannot be corrected any faster, so the handler of
 * UBERTOOTH_FIX_CLOCK_DRIFT keeps the running figure within it.
 */
#define CLK_DRIFT_PPM_MAX 320

extern volatile uint32_t clkn_last_drift_fix;
extern volatile uint32_t clkn_next_drift_fix;

#define CLK100NS (CLK100NS_PER_CLKN*(clkn & 0xfffff) + T0TC)
#define LE_BASECLK (12500)                    // 1.25 ms in units of 100ns

/*
 * One step of a wait timed by timer 0: the 100 ns ticks between two readings
 * of T0TC, which a wait adds up as it polls.
 *
 * Waits are written that way, rather than by comparing CLK100NS against a
 * deadline, because CLK100NS does not only count up. It is
 * CLK100NS_PER_CLKN * (clkn & 0xfffff) + T0TC, timer 0 resets at the end of
 * each period, and clkn is advanced by the timer's own interrupt handler:
 * between the reset and the handler running, every reading is about one whole
 * clkn tick lower than the one before it. A deadline compared against that
 * would be met in that window, every 312.5 us, and the wait would give up on
 * a packet that was arriving normally.
 *
 * A reading lower than the one before it means the timer reset in between, and
 * how much of that period was left is only known if the period was the usual
 * CLK100NS_PER_CLKN ticks. Often it is not. The clock interrupt lengthens one
 * period to correct for clock drift, and UBERTOOTH_TRIM_CLOCK and
 * UBERTOOTH_START_HOPPING let the host put a length of its own in the match
 * register, which the timer keeps until the next clock interrupt writes the
 * usual value back.
 *
 * So the step is taken from the sign of the difference and not modulo a fixed
 * period. A reading that went down counts only how far into the new period the
 * wait is, which is never more than the time that really passed, whatever the
 * period's length -- a period is never shorter than CLK100NS_PER_CLKN, because
 * the match value is that minus one plus an unsigned offset. A fixed modulus
 * has no such bound: for some lengths it returns a figure much larger than the
 * step took, and a wait that counted one of those would end early.
 *
 * Undercounting is the safe direction for a deadline: the wait runs a little
 * longer than its budget rather than ending early on a packet that was still
 * arriving. A gap longer than one period is undercounted for the same reason,
 * because nothing here can tell how many resets it spanned.
 */
static inline uint32_t clk100ns_step(uint32_t last, uint32_t now)
{
	if (now >= last)
		return now - last;
	if (last <= CLK100NS_PER_CLKN)
		return CLK100NS_PER_CLKN - last + now;
	return now;
}

void clkn_stop();
void clkn_start();
void clkn_init();
void clkn_disable();

#endif
