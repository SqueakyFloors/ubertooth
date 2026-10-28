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

#include "watchdog.h"
#include "usb_irq.h"

/*
 * The breadcrumb, in the 32-byte region at the bottom of RAM that nothing
 * initialises (.persistent in sections.ld, whose last word is
 * bootloader_ctrl). Startup code copies .data and zeroes .bss; this region is
 * in neither, so what the last run wrote is still there after a watchdog
 * reset, and still there after the bootloader has run -- the bootloader's own
 * data sits above the same reserved region, and its stack grows down from the
 * top of RAM.
 *
 * The live_ fields are written as the firmware runs. The others are written
 * once per boot, by watchdog_boot(), from what the live_ fields held when the
 * board died.
 */
typedef struct {
	uint32_t magic;
	uint8_t  live_mode;
	uint8_t  live_site;
	uint8_t  live_outer_site;
	uint8_t  live_intent;
	uint8_t  reset_cause;
	uint8_t  reset_mode;
	uint8_t  reset_site;
	uint8_t  resets;
	uint8_t  boots;
} watchdog_crumb_t;

/* Says the fields below mean something. Any other value, and the region holds
 * whatever RAM came up with. */
#define WATCHDOG_CRUMB_MAGIC 0x57443031 /* "WD01" */

extern volatile watchdog_crumb_t watchdog_crumb;

/* .persistent is 32 bytes and its last word is bootloader_ctrl. */
_Static_assert(sizeof(watchdog_crumb_t) <= 28,
               "the breadcrumb must fit below bootloader_ctrl in .persistent");

/* The reset causes the host is told about are the part's own reset source
 * bits, so a cause can be handed over as it was read. */
_Static_assert((int)WATCHDOG_RESET_POWER_ON == RSID_POR &&
               (int)WATCHDOG_RESET_EXTERNAL == RSID_EXTR &&
               (int)WATCHDOG_RESET_WATCHDOG == RSID_WDTR &&
               (int)WATCHDOG_RESET_BROWN_OUT == RSID_BODR,
               "watchdog_reset_cause must match the reset source register");

/*
 * The watchdog counter is clocked at WDCLK/4, and WDCLKSEL is left at its
 * reset value: the internal RC oscillator, 4 MHz. So one count is one
 * microsecond, and the count is what the timeout is in milliseconds times a
 * thousand. Keeping the RC oscillator means the watchdog does not stop if the
 * PLLs or the main crystal do.
 */
#define WATCHDOG_TICKS_PER_MS 1000
#define WATCHDOG_TICKS (UBERTOOTH_WATCHDOG_TIMEOUT_MS * WATCHDOG_TICKS_PER_MS)

/* The counter is 32 bits wide, so the longest timeout is about 71 minutes. */
#define WATCHDOG_TICKS_MAX 0xFFFFFFFFu

/*
 * Set once the watchdog is armed, and the one thing the feed checks. Before
 * that the feed does nothing, which is what makes this file safe to compile
 * into an image that never arms it. Two images are in that position: the
 * test images usb_test and assembly_test link the USB service path, so they
 * call the feed, but they have no main loop that would arm the watchdog
 * first. Without this guard they would start writing to a watchdog that is
 * not running.
 */
static volatile uint8_t watchdog_armed = 0;

void watchdog_mode(uint8_t mode)
{
	watchdog_crumb.live_mode = mode;
}

/*
 * Is this running inside an interrupt handler?
 *
 * The low nine bits of the interrupt control and state register are the
 * exception the processor is in -- zero in thread mode, the exception number
 * otherwise (the Cortex-M3 chapter of the part's user manual, UM10360).
 */
static int in_interrupt_handler(void)
{
	return (SCB_ICSR & 0x1ffu) != 0;
}

uint8_t watchdog_mark(uint8_t site)
{
	uint8_t previous = watchdog_crumb.live_site;

	/*
	 * What the board was doing one level out. This is what watchdog_report()
	 * tells the host the board is busy with, because live_site cannot serve:
	 * a reply is written from inside the USB service path, which has marked
	 * itself by then, so live_site always reads back as "servicing USB". One
	 * level out is the wait the mode was really in when the request arrived.
	 */
	watchdog_crumb.live_outer_site = previous;
	watchdog_crumb.live_site = site;
	return previous;
}

uint8_t watchdog_mark_wait(uint8_t site)
{
	/*
	 * A wait may be reached from a mode loop or from inside an interrupt
	 * handler -- the crystal-oscillator wait is reached from both low-energy
	 * handlers, on every channel hop. A handler is not a level of the nesting
	 * above: it borrows the processor and gives it back, so the code under it
	 * is still in whatever wait it was in, and a wait that claimed the outer
	 * site there would leave -w naming a wait the board has left.
	 */
	if (in_interrupt_handler())
		return watchdog_mark_handler(site);
	return watchdog_mark(site);
}

void watchdog_leave_wait(uint8_t site)
{
	if (in_interrupt_handler())
		watchdog_unmark(site);
	else
		watchdog_mark(site);
}

uint8_t watchdog_mark_handler(uint8_t site)
{
	uint8_t previous = watchdog_crumb.live_site;

	watchdog_crumb.live_site = site;
	return previous;
}

void watchdog_unmark(uint8_t site)
{
	watchdog_crumb.live_site = site;
}

void watchdog_intent(uint8_t intent)
{
	watchdog_crumb.live_intent = intent;
}

void watchdog_boot(void)
{
	/* The low four bits are the reset sources. They are sticky, so clearing
	 * them here -- writing a one clears a bit -- is what makes the next boot
	 * read its own cause rather than every cause since power-on. */
	uint8_t cause = RSID & 0xF;
	int fresh;

	RSID = cause;

	/* The watchdog time-out flag is not cleared by the reset it caused, only
	 * by software, and the on-chip programmer refuses to start while it is
	 * set. The reset source register above is what this code reads instead. */
	WDMOD &= ~WDMOD_WDTOF;

	/* Power-on and brown-out both mean RAM held no breadcrumb, or held one
	 * that cannot be trusted. Anything else -- a watchdog reset, the reset
	 * pin, the bootloader handing over -- leaves RAM alone. */
	fresh = watchdog_crumb.magic != WATCHDOG_CRUMB_MAGIC ||
	        (cause & (WATCHDOG_RESET_POWER_ON | WATCHDOG_RESET_BROWN_OUT)) != 0;

	if (fresh) {
		watchdog_crumb.magic = WATCHDOG_CRUMB_MAGIC;
		watchdog_crumb.reset_mode = MODE_IDLE;
		watchdog_crumb.reset_site = WATCHDOG_SITE_NONE;
		watchdog_crumb.resets = 0;
		watchdog_crumb.boots = 0;
	} else if ((cause & WATCHDOG_RESET_WATCHDOG) &&
	           watchdog_crumb.live_intent == WATCHDOG_INTENT_RUNNING) {
		/* The board stopped feeding the watchdog without having asked to be
		 * reset: a wedge, and the only kind of reset worth a breadcrumb. */
		watchdog_crumb.reset_mode = watchdog_crumb.live_mode;
		watchdog_crumb.reset_site = watchdog_crumb.live_site;
		if (watchdog_crumb.resets < 255)
			watchdog_crumb.resets++;
	}

	watchdog_crumb.reset_cause = cause;
	watchdog_crumb.live_mode = MODE_IDLE;
	watchdog_crumb.live_site = WATCHDOG_SITE_NONE;
	watchdog_crumb.live_outer_site = WATCHDOG_SITE_NONE;
	watchdog_crumb.live_intent = WATCHDOG_INTENT_RUNNING;
	if (watchdog_crumb.boots < 255)
		watchdog_crumb.boots++;
}

/*
 * The feed is two writes to the feed register, 0xAA then 0x55, and the part
 * treats any other write to the watchdog registers in between as a failed
 * feed -- which, with the reset enabled, is itself a reset (the watchdog
 * chapter of the part's user manual, UM10360, under the feed sequence).
 *
 * Only this file writes those registers, and the only interrupt that reaches
 * it is the USB one, which feeds from the handler. Holding that interrupt off
 * across the pair is therefore enough to keep the two feed sites from
 * breaking each other's sequence. Another interrupt landing between the two
 * writes is harmless, because none of them touches the watchdog: widening the
 * mask to all interrupts would cost the radio handlers their timing for
 * nothing.
 */
static void watchdog_feed(void)
{
	u32 usb_irq;

	if (!watchdog_armed)
		return;

	usb_irq = usb_irq_hold();
	WDFEED_SEQUENCE;
	usb_irq_release(usb_irq);
}

void watchdog_start(void)
{
	/* The clock source can only be chosen while the watchdog is stopped, so
	 * it is written first. Zero selects the internal RC oscillator, which is
	 * also the reset value; writing it says so rather than relying on it. */
	WDCLKSEL = 0;
	WDTC = WATCHDOG_TICKS;
	WDMOD |= WDMOD_WDEN | WDMOD_WDRESET;
	watchdog_armed = 1;
	/* Loads WDTC into the counter and starts it. */
	watchdog_feed();
}

void watchdog_usb_serviced(void)
{
	watchdog_feed();
}

void watchdog_usb_ready(void)
{
	watchdog_feed();
}

void watchdog_stretch(void)
{
	/* As in watchdog_feed(): an image that never armed it must not be left
	 * writing to a watchdog that is not running. */
	if (!watchdog_armed)
		return;
	WDTC = WATCHDOG_TICKS_MAX;
	watchdog_feed();
}

void watchdog_unstretch(void)
{
	if (!watchdog_armed)
		return;
	WDTC = WATCHDOG_TICKS;
	watchdog_feed();
	/*
	 * Deliberately not symmetrical with watchdog_stretch(), which sets no
	 * intent: the caller sets the intent for the hand-over it is about to
	 * make, and getting here means that hand-over came back, so the board
	 * is plainly running again and any later reset is a real wedge.
	 */
	watchdog_intent(WATCHDOG_INTENT_RUNNING);
}

void watchdog_report(uint8_t *data)
{
	data[0] = watchdog_crumb.reset_cause;
	data[1] = watchdog_armed;
	data[2] = watchdog_crumb.resets;
	data[3] = watchdog_crumb.boots;
	data[4] = watchdog_crumb.reset_mode;
	data[5] = watchdog_crumb.reset_site;
	data[6] = watchdog_crumb.live_mode;
	/* Not live_site: see watchdog_mark(). */
	data[7] = watchdog_crumb.live_outer_site;
}
