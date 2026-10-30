/*
 * Copyright 2010, 2011 Michael Ossmann
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
/*
  Copyright 2010-07 By Opendous Inc. (www.MicropendousX.org)
  NVIC handler info copied from NXP User Manual UM10360

  Start-up code for LPC17xx.  See TODOs for
  modification instructions.

  Permission to use, copy, modify, and distribute this software
  and its documentation for any purpose and without fee is hereby
  granted, provided that the above copyright notice appear in all
  copies and that both that the copyright notice and this
  permission notice and warranty disclaimer appear in supporting
  documentation, and that the name of the author not be used in
  advertising or publicity pertaining to distribution of the
  software without specific, written prior permission.

  The author disclaim all warranties with regard to this
  software, including all implied warranties of merchantability
  and fitness.  In no event shall the author be liable for any
  special, indirect or consequential damages or any damages
  whatsoever resulting from loss of use, data or profits, whether
  in an action of contract, negligence or other tortious action,
  arising out of or in connection with the use or performance of
  this software.
*/

#include <lpc17.h>

#include "stack_check.h"

/* Reset_Handler variables defined in linker script */
extern unsigned long _interrupt_vector_table;
extern unsigned long _data;
extern unsigned long _edata;
extern unsigned long _etext;
extern unsigned long _bss;
extern unsigned long _ebss;
/* The AHB SRAM bank, which holds the large buffers and is not loaded from
 * flash, so it is zeroed here like .bss. See sections.ld. */
extern unsigned long _ahb_sram;
extern unsigned long _ahb_sram_end;
/* The guard, the bottom of the usable stack and the top of RAM; see
 * stack_check.h. */
extern unsigned long _stack_guard;
extern unsigned long _stack_bottom;
extern unsigned long _StackTop;

extern void __libc_init_array(void);
extern int main(void);

static void stack_paint(void)
{
	unsigned long *word;
	unsigned long sp;

	__asm__ volatile ("mov %0, sp" : "=r" (sp));

	/*
	 * Below this function's own stack pointer, not up to the top of RAM:
	 * the words at and above it hold the return address this function
	 * needs. A function on this architecture is given nothing below its
	 * stack pointer, so everything below is free to write.
	 */
	for (word = &_stack_bottom; word < (unsigned long *)sp; word++)
		*word = STACK_PAINT;
}

/*
 * The bootloader asks for no guard (UBERTOOTH_NO_STACK_GUARD in its Makefile,
 * beside UBERTOOTH_NO_WATCHDOG). It runs for a moment with 12 KB of stack to
 * itself, so a guard buys it nothing, and it is the image a board is recovered
 * through: the one place where a fault of our making would need someone at the
 * bench with the entry pin bridged.
 */
#ifndef UBERTOOTH_NO_STACK_GUARD

/* The words below, against the part's own encoding (the Cortex-M3 chapter of
 * UM10360). A wrong bit position would leave a region the part ignores, which
 * nothing else would notice. */
_Static_assert((MPU_RASR_ENABLE | MPU_RASR_SIZE(STACK_GUARD_SIZE_FIELD) |
                MPU_RASR_AP_NONE | MPU_RASR_XN) == 0x10000009ul,
               "the guard region's attribute word is not the part's encoding");
_Static_assert((MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA) == 0x5ul,
               "the control word is not the part's encoding");

/*
 * Refuse every access to the guard, and leave the part's default memory map in
 * place for everything else, so this adds one rule rather than describing the
 * whole map.
 *
 * The other region attributes stay zero: nothing may read, write or execute
 * here, so how the bus would treat an access that is refused does not arise.
 */
static void stack_guard_enable(void)
{
	/* A part whose unit is absent reports no regions, and writing its
	 * registers would be writing into the reserved space where they would
	 * be. The LPC175x and LPC176x parts this firmware is built for all have
	 * the unit; this is for a clone that does not. */
	if (MPU_TYPE_DREGION(MPU_TYPE) == 0)
		return;

	MPU_RNR = 0;
	/* The base's low five bits are the register's own region number and the
	 * bit that says to use it. They are all zero because the address is
	 * 32-byte aligned, which is what leaves MPU_RNR above in charge. */
	MPU_RBAR = (uint32_t)&_stack_guard;
	MPU_RASR = MPU_RASR_ENABLE | MPU_RASR_SIZE(STACK_GUARD_SIZE_FIELD) |
	           MPU_RASR_AP_NONE | MPU_RASR_XN;
	MPU_CTRL = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;

	/* The rules have to be in force before the next memory access, which
	 * the processor does not guarantee on its own. */
	__asm__ volatile ("dsb" ::: "memory");
	__asm__ volatile ("isb");
}

#else
static void stack_guard_enable(void)
{
}
#endif

unsigned stack_used(void)
{
	return stack_used_between(&_stack_bottom, &_StackTop);
}

unsigned stack_size(void)
{
	return (unsigned)((const unsigned char *)&_StackTop -
	                  (const unsigned char *)&_stack_bottom);
}

/* Reset Handler */
void Reset_Handler(void)
{
	unsigned long *src, *dest;

	/* First, while the stack is no deeper than this function. */
	stack_paint();

	/* Armed from here, so the copying below is covered too. A fault this
	 * early is not recorded: the vector table is still the previous stage's
	 * until the end of this function, and main() is what turns the fault
	 * handlers on. The 2 KB the link leaves is what makes that acceptable --
	 * startup cannot run the stack out. */
	stack_guard_enable();

	// Copy the data segment initializers from flash to SRAM
	src = &_etext;
	for(dest = &_data; dest < &_edata; )
	{
		*dest++ = *src++;
	}

	// Initialize the .bss segment of memory to zeros
	src = &_bss;
	while (src < &_ebss)
	{
		*src++ = 0;
	}

	// The same for the buffers in the other RAM bank
	src = &_ahb_sram;
	while (src < &_ahb_sram_end)
	{
		*src++ = 0;
	}

	__libc_init_array();

	// Set the vector table location.
	SCB_VTOR = (uint32_t)&_interrupt_vector_table;

	main();

	// In case main() fails, have something to breakpoint
	while (1) {;}
}
