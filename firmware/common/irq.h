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

#ifndef __IRQ_H
#define __IRQ_H

/* For uint32_t: this header is included by code that has not necessarily
 * pulled in the image's own types yet. */
#include <stdint.h>

/*
 * Turn interrupts off and return the mask setting that was in force, so the
 * caller can put it back with irq_restore(). Code between the two runs with no
 * interrupt handler able to cut in.
 *
 * PRIMASK is the Cortex-M3 register that blocks every interrupt bar the two
 * faults that cannot be masked. `cpsid i` sets it; writing the saved value
 * back clears it only if it was clear to begin with. Restoring the old
 * setting, rather than simply enabling interrupts at the end, is what makes
 * the pair safe to nest and safe to use inside an interrupt handler -- a
 * handler that called irq_save_disable() must still return with interrupts
 * masked.
 *
 * No barrier is needed either side. Writing PRIMASK takes effect on the next
 * instruction, unlike a write to one of the interrupt controller's enable
 * registers, which is why the code that disables an interrupt through ICER0
 * follows it with a barrier and this does not.
 *
 * Keep the span between them short. Interrupts raised while masked are not
 * lost: the controller latches them and they run as soon as the mask comes
 * off. But a periodic interrupt whose period is shorter than the span loses a
 * tick, and the clkn timer interrupt in the bluetooth_rxtx image fires every
 * 312.5 microseconds.
 *
 * The btbr image has its own pair, with the same names and the same meaning,
 * in firmware/btbr/include/ubtbr/system.h. That image builds
 * firmware/common/ubertooth.c, so it compiles both headers -- but in different
 * translation units, which is what lets the two exist side by side. They
 * define the same two functions under different include guards, so a single
 * file that saw both would not compile: do not pull this header into
 * ubertooth.h, which btbr's own sources include alongside their system.h. The
 * price of the arrangement is that the two copies can drift apart.
 *
 * This is ARM assembly, and the host unit tests compile code that uses it. A
 * stand-in header earlier on their include path stands in for the mask there
 * (firmware/tests/stubs-lephy/irq.h), which is also where a test can make an
 * interrupt land at the boundary of a guarded section. A host build that
 * reaches this file instead has its include path wrong, and silently dropping
 * the mask would leave a guard that guards nothing.
 */
#ifdef __arm__
static inline uint32_t irq_save_disable(void)
{
	uint32_t primask;

	asm volatile ("mrs %0, primask\n\t"
	              "cpsid i"
	              : "=r" (primask) : : "memory");
	return primask;
}

static inline void irq_restore(uint32_t primask)
{
	asm volatile ("msr primask, %0" : : "r" (primask) : "memory");
}
#else
#error "irq.h is ARM assembly: a host build needs the stand-in on its include path"
#endif

#endif /* __IRQ_H */
