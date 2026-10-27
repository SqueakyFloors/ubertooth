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

/*
 * Reads of AGCCTRL before this test gives up on the radio. The radio answers a
 * register read in a few microseconds once it is out of reset. This image never
 * starts the PLL, so the processor runs on its 4 MHz internal oscillator, where
 * each of these 24-bit transfers takes about 115 microseconds and the whole
 * budget a little over a second: out of reach of a part that is answering at
 * all, which is all the bound has to be.
 */
#define AGCCTRL_READS 10000

/*
 * Half the give-up blink. wait_ms() is calibrated for the 100 MHz clock this
 * image never configures, so on the internal oscillator each half lasts about
 * 25 times as long -- around half a second, which is what a blink wants anyway.
 */
#define GIVE_UP_BLINK_MS 20

int main()
{
	unsigned reads;

	gpio_init();
	TXLED_SET;

	cc2400_init();
	RXLED_SET;

	cc2400_reset();
	/*
	 * The radio reads its reset value back, or this gives up. A radio that
	 * answers nothing at all never gets here: cc2400_reset() waits for its
	 * own readback unbounded, as boot code in every image does. What the
	 * bound covers is a part that answers the bus but never reports AGCCTRL
	 * reset, which used to spin here forever.
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
