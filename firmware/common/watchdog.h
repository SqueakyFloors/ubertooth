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

#ifndef __UBERTOOTH_WATCHDOG_H
#define __UBERTOOTH_WATCHDOG_H

#include "ubertooth.h"

/*
 * The watchdog resets the board if it stops servicing USB, and leaves a
 * breadcrumb saying where it was when that happened.
 *
 * Why USB and not "the firmware is running": a board that answers nothing on
 * USB is useless to the host and cannot be recovered by it. The reset request
 * arrives on USB too, so it cannot reach a board in that state. Before this
 * watchdog, such a board stayed that way until it was unplugged or its hub
 * port was power-cycled. That is the fault this guards against: the board now
 * resets itself instead.
 *
 * So the watchdog is fed from the two places USB is actually serviced, and
 * from one more that proves it could be:
 *
 *  - watchdog_usb_serviced(), called where a USB service pass has just
 *    finished: handle_usb() in the polled modes and the main loop, and the USB
 *    interrupt handler. A pass runs whether or not anything arrived, so what
 *    reaching it proves is that a pass completed -- the controller was driven
 *    and anything the host had asked would have been answered.
 *
 *  - watchdog_usb_ready(), called once per pass by the mode loops that service
 *    USB from the interrupt instead of polling it. Those modes get no service
 *    pass of their own while the host is quiet -- the loop in xmas mode waits
 *    for a timer interrupt and nothing else -- so a feed tied to service alone
 *    would reset a board that is working. The loop body runs below every
 *    interrupt handler, so reaching it means no handler is stuck and the USB
 *    interrupt can be taken: the board would answer a request if one arrived.
 *    That is weaker than having answered one, and it is as strong as those
 *    modes allow.
 *
 * Nothing feeds it from a timer, which would only show that the CPU is
 * running.
 *
 * One thing this rests on: no interrupt is allowed to preempt the USB handler.
 * Almost all of them, the USB one included, keep the reset default priority of
 * 0, because nothing sets one. A handler that stops returning therefore stops
 * the USB handler with it, which is what makes "the board still answers USB" a
 * test of the whole firmware rather than of the USB path alone. Give some
 * radio or timer interrupt a priority above USB's and this watchdog would keep
 * feeding through exactly the wedge it exists to catch.
 *
 * One interrupt does set a priority, in both images: dma_poweron() puts the
 * GPDMA handler at 31, the lowest. That is the safe direction -- USB preempts
 * it, never the other way round -- but it does mean a GPDMA handler that
 * stopped returning would keep being fed through. It is not the fault this
 * watchdog is for: the board would still be answering the host, so the host
 * would see wrong or missing capture data rather than a board that has gone
 * silent.
 * test_watchdog.c pins both halves: nothing else sets a priority, and the one
 * that does sets the lowest.
 */

/* What the board is doing, for the breadcrumb. */
void watchdog_mode(uint8_t mode);

/* Record that this wait is starting, and return the site that was recorded
 * before, for the caller to put back when the wait is over. A wait that
 * services USB while it polls nests that way, so the inner USB pass does not
 * hide the wait the board is really in. */
uint8_t watchdog_mark(uint8_t site);

/*
 * The same for an interrupt handler, which must leave the live outer site
 * alone. That site is what a reply reports, and it is set so that a reply
 * written from inside the USB service path names the wait the mode is really
 * in. An interrupt handler is not a level of that nesting: it borrows the
 * processor and gives it back. Were it to use watchdog_mark(), the handler
 * would be left named as the outer site after returning -- and the low-energy
 * engine's byte interrupt fires once per received byte, so during a capture
 * that would be the usual reading rather than a rare one.
 *
 * This does not make every live reading right. A wait that marks itself from
 * inside a handler still claims the outer site on the way out, which is the
 * same wrong reading by another route; the polled modes mask only the USB
 * interrupt, so a handler can land in the middle of a service pass.
 *
 * So the pair below records the site for the breadcrumb only. Use it in an
 * interrupt handler; use watchdog_mark() in a wait.
 */
uint8_t watchdog_mark_handler(uint8_t site);
void watchdog_unmark(uint8_t site);

/*
 * A reset this firmware asked for, so the breadcrumb does not count it as the
 * board having stopped answering.
 *
 * Set it where the reset actually happens, not where the request arrives. A
 * request can be dropped -- btbr queues its reset, and a full queue discards
 * it -- and a board left running with the intent set would have its next real
 * wedge excused. Entering the bootloader is a reset too, so it needs no
 * intent of its own; the on-chip programmer does, because it never resets.
 */
enum watchdog_intent {
	WATCHDOG_INTENT_RUNNING = 0,
	WATCHDOG_INTENT_RESET   = 1,
	WATCHDOG_INTENT_ISP     = 2,
};

void watchdog_intent(uint8_t intent);

/*
 * Read the reset source register, clear it, and turn what the last run left in
 * the breadcrumb into a record of why the board restarted. Call this first,
 * before anything that could mark a site.
 */
void watchdog_boot(void);

/* Arm the watchdog. Call it once USB is up and the first service pass is
 * about to happen, and not before: from here on, three seconds without a feed
 * resets the board. */
void watchdog_start(void);

/* Feed it. See the two sites described above. */
void watchdog_usb_serviced(void);
void watchdog_usb_ready(void);

/*
 * Stretch the timeout to the longest the counter can hold, about 71 minutes,
 * for a hand-over to code that has no feed of its own. Only the on-chip
 * programmer (set_isp()) needs this: it keeps the chip running without a
 * reset, so the watchdog stays armed and would otherwise reset it mid-session.
 * The watchdog cannot be switched off again before a reset.
 *
 * watchdog_unstretch() puts the timeout and the intent back, for the case
 * where that hand-over returns after all. Without it the board would run on
 * unguarded, and the next wedge would be neither reset in time nor counted.
 */
void watchdog_stretch(void);
void watchdog_unstretch(void);

/* Fill UBERTOOTH_GET_WATCHDOG's eight-byte reply. */
void watchdog_report(uint8_t *data);

#endif /* __UBERTOOTH_WATCHDOG_H */
