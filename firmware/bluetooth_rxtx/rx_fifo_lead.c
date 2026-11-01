/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The wait before a received packet is clocked out of the CC2400's FIFO.
 * rx_fifo_lead.h says what it is for and why each of its numbers is what it
 * is.
 */

/* Angle brackets, not quotes: a host test puts a hardware-free stand-in for
 * firmware/common's ubertooth.h earlier on the include path, and a quoted
 * include would always pick up the real one. T0TC comes from there. */
#include <ubertooth.h>

#include "cc2400_wait.h"
#include "rx_fifo_lead.h"
#include "ubertooth_clock.h"

void rx_wait_for_fifo(void)
{
	u32 last = T0TC;
	u32 waited = 0;
	u32 polls = RX_FIFO_LEAD_POLLS;

	/*
	 * Timer 0 counts 100 ns ticks and resets at the end of each period, so
	 * this adds up the steps it sees rather than comparing against a
	 * deadline the reset would carry it past. clk100ns_step() measures each
	 * step and says why it is measured the way it is; the short of it is
	 * that the period is not always the usual 312.5 us, so a step cannot be
	 * taken modulo a fixed length.
	 */
	while (waited < RX_FIFO_LEAD_100NS && polls-- > 0) {
		u32 now = T0TC;

		waited += clk100ns_step(last, now);
		last = now;
	}

	/*
	 * The loop tests the lead before it tests the cap, so a short total is
	 * exactly the case where the cap ran out. `polls` cannot be used for
	 * this: the last test of `polls-- > 0` leaves it at 0xffffffff.
	 */
	if (waited < RX_FIFO_LEAD_100NS)
		cc2400_wait_count_timeout(CC2400_WAIT_RX_LEAD);
}
