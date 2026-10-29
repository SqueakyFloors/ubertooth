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

/* Bytes of the stack region that have ever been in use. A stack that ran past
 * the bottom of the region reads as the whole of it, which is as much as the
 * word can say. */
unsigned stack_used(void);

/* How big the region is: from the lowest address the stack may reach to the
 * top of RAM, less the 32 bytes the on-chip programmer needs. */
unsigned stack_size(void);

#endif /* __UBERTOOTH_STACK_CHECK_H */
