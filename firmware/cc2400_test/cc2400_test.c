/*
 * Copyright 2010 Michael Ossmann
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

#include "ubertooth.h"
#include "cc2400_wait.h"

/*
 * Reads of AGCCTRL before this test gives up on the radio. The radio answers a
 * register read in a few microseconds once it is out of reset, and each read
 * here is one bit-banged 24-bit transfer, so a budget this large is only ever
 * reached by a part that is not answering at all. It is the same shape as the
 * bounds in firmware/common/cc2400_wait.h, which the shipped images wait
 * through.
 */
#define AGCCTRL_READS 10000

/* Milliseconds each half of the give-up blink lasts. */
#define GIVE_UP_BLINK_MS 200

int main()
{
	unsigned reads;

	gpio_init();
	TXLED_SET;

	cc2400_init();
	RXLED_SET;

	cc2400_reset();
	/*
	 * The radio reads its reset value back, or this gives up. An unbounded
	 * wait here left a board with a dead radio looking exactly like one
	 * still starting up: two LEDs on, forever, with nothing to say which.
	 */
	for (reads = 0; reads < AGCCTRL_READS; reads++)
		if (cc2400_get(AGCCTRL) == 0xf700)
			break;
	if (reads == AGCCTRL_READS) {
		/*
		 * Blink the LEDs this test has already lit, leaving USR off.
		 * USR off is still what says the read never succeeded; the
		 * blink is what says the test reached that conclusion rather
		 * than being stuck on the way to it.
		 */
		while (1) {
			TXLED_CLR;
			RXLED_CLR;
			wait_ms(GIVE_UP_BLINK_MS);
			TXLED_SET;
			RXLED_SET;
			wait_ms(GIVE_UP_BLINK_MS);
		}
	}
	USRLED_SET;

	while (1);
}
