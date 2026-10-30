/*
 * Copyright 2026 Project Ubertooth contributors
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

#ifndef __UBERTOOTH_STACK_CHECK_H
#define __UBERTOOTH_STACK_CHECK_H

/*
 * How deep the stack has ever been since the board booted, reported in the
 * UBERTOOTH_GET_WATCHDOG reply. An overflow on this part is silent, so the
 * only way to know there is room is to measure what has been used.
 *
 * Both live in LPC17xx_Startup.c, the file that owns the stack at boot: it
 * fills the room below its own stack pointer with a known word, and
 * stack_used() looks for the lowest one still there. Every image links that
 * file, so every image can answer.
 */

/*
 * The word untouched stack holds. Any value would do that code is unlikely to
 * leave behind: this one is not zero, not a small count and not an address in
 * this part's memory map.
 */
#define STACK_PAINT 0xa5e71f3bul

/*
 * How deep into [bottom, top) the stack has ever reached: the lowest word
 * that no longer holds the pattern, measured from the top.
 *
 * The whole region is read, from the bottom, and it has to be. Searching down
 * from the top for the first word still holding the pattern would be a tenth
 * of the work and give the wrong answer, because a function that reserves
 * room for its locals does not write all of it: on a board that version
 * reported 48 bytes and never moved, where this one reported 296 at boot
 * rising to 480 under a capture. A read of 11 KB costs about 0.2 ms, and it
 * happens only when the host asks for the record.
 *
 * Here rather than beside its caller so that a host test can run it over an
 * array it painted itself; the arithmetic is the part worth pinning.
 */
static inline unsigned stack_used_between(const unsigned long *bottom,
                                          const unsigned long *top)
{
	const unsigned long *word = bottom;

	while (word < top && *word == STACK_PAINT)
		word++;

	/* A stack that ran past the bottom reads as the whole region, which is
	 * as much as the pattern can say. */
	return (unsigned)((const unsigned char *)top -
	                  (const unsigned char *)word);
}

/* Bytes of the stack region that have ever been in use. */
unsigned stack_used(void);

/* How big the region is: from the lowest address the stack may reach to the
 * top of RAM, less the 32 bytes the on-chip programmer needs. */
unsigned stack_size(void);

#endif /* __UBERTOOTH_STACK_CHECK_H */
