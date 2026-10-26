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

#ifndef __USB_IRQ_H
#define __USB_IRQ_H

/*
 * Hold off the USB interrupt across a section that must not be re-entered,
 * and put it back the way it was. Both firmware images use this, so there is
 * one definition of it.
 *
 * Everything that drives the USB controller goes through registers there is
 * only one of: USBCtrl selects which endpoint reads and writes apply to, and
 * the protocol engine is driven by a command-code handshake reported through
 * the device interrupt bits. Two contexts doing that at once would each
 * consume the other's handshake, and the loser would wait for a completion
 * that has already been taken. Code that drives the controller from a loop
 * therefore keeps the interrupt out for the duration.
 *
 * usb_irq_hold() reports whether the interrupt was enabled; pass that to
 * usb_irq_release(), which re-enables it only if it was. So nesting is safe,
 * a caller that already holds it off pays only a register read, and a caller
 * that runs before USB is up -- btbr's early_printf() -- does not switch the
 * interrupt on. The interrupt is not lost while held off: the NVIC keeps it
 * pending and takes it as soon as the mask is lifted.
 *
 * ISER0 and ICER0 come from lpc17.h, which the including image's ubertooth.h
 * pulls in.
 */
static inline u32 usb_irq_hold(void)
{
	u32 was_enabled = ISER0 & ISER0_ISE_USB;

	if (was_enabled) {
		ICER0 = ICER0_ICE_USB;
		/* A write to NVIC ICER0 is not self-synchronising on Cortex-M3, so
		 * without these an interrupt already pending can still be taken
		 * after the mask -- inside the section being protected. le_phy.c
		 * works around the same thing for the DMA line with a flag. They
		 * are ARM instructions, and the host unit tests compile this
		 * header, where there is no interrupt to order against -- those
		 * define FW_HOST_TEST. Any other build without them is a mistake,
		 * and silently dropping them would leave the hold unsynchronised
		 * with nothing to show for it. */
#ifdef __arm__
		__asm__ volatile ("dsb" ::: "memory");
		__asm__ volatile ("isb" ::: "memory");
#elif !defined(FW_HOST_TEST)
#error "usb_irq_hold() needs the memory barriers on this target"
#endif
	}

	return was_enabled;
}

static inline void usb_irq_release(u32 was_enabled)
{
	if (was_enabled)
		ISER0 = ISER0_ISE_USB;
}

#endif /* __USB_IRQ_H */
