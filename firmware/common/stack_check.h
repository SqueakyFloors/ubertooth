/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __UBERTOOTH_STACK_CHECK_H
#define __UBERTOOTH_STACK_CHECK_H

/*
 * How deep the stack has ever been since the board booted, reported in the
 * UBERTOOTH_GET_WATCHDOG reply. The guard below catches an overflow that walks
 * into it, but not one that steps over it, so the measurement is still what
 * says whether an image has room to spare.
 *
 * Both live in LPC17xx_Startup.c, the file that owns the stack at boot: it
 * fills the room below its own stack pointer with a known word, and
 * stack_used() looks for the lowest one still there. Every image links that
 * file, so every image can answer.
 */

/*
 * The guard: 32 bytes at the bottom of the stack that the memory protection
 * unit is told to refuse, so a stack running off the bottom raises a memory
 * management fault there instead of writing over the end of .bss. The fault
 * handler records it, and ubertooth-util -w reads it back.
 *
 * The unit encodes a region size as a power of two, 2^(field + 1) bytes, and a
 * region must be aligned to its own size. sections.ld places _stack_guard on a
 * 32-byte boundary above everything allocated and puts the usable stack
 * (_stack_bottom) above it, so neither the paint nor the measurement touches
 * the guard.
 *
 * One frame larger than the guard can push past it in a single instruction and
 * land on .bss unnoticed. This catches the ordinary case, where the stack walks
 * down a frame at a time; the link-time check in sections.ld is what keeps the
 * room there in the first place.
 */
#define STACK_GUARD_SIZE 32u
#define STACK_GUARD_SIZE_FIELD 4u
_Static_assert((2u << STACK_GUARD_SIZE_FIELD) == STACK_GUARD_SIZE,
               "the guard's size field must encode STACK_GUARD_SIZE bytes");

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

/* How big the region is: from the lowest address the stack may use, above the
 * guard, to the top of RAM, less the 32 bytes the on-chip programmer needs. */
unsigned stack_size(void);

#endif /* __UBERTOOTH_STACK_CHECK_H */
