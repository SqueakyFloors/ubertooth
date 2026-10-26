/*
 * Bounded waits on CC2400 synthesizer state.
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#ifndef __UBERTOOTH_CC2400_WAIT_H
#define __UBERTOOTH_CC2400_WAIT_H

#include <stddef.h>

#include "ubertooth.h"

/* Polls before cc2400_wait_fs_unlock() gives up. Each poll is one bit-banged
 * 8-bit SPI transaction with no delay loops (cc2400_spi()), so well under a
 * microsecond, putting the bound around a millisecond. It is kept short
 * deliberately: hop() calls this from bt_stream_rx()'s streaming loop, where a
 * long stall costs symbols. */
#define FS_UNLOCK_POLLS 1000

/* Re-strobe SRFOFF every this many polls. Short, so a strobe lost to bus
 * contention is retried quickly rather than after most of the budget. */
#define FS_UNLOCK_RESTROBE_INTERVAL 32

/* Polls before the wait for a lock gives up. Locking takes the synthesizer a
 * few hundred microseconds, so this is sized to be out of reach of a radio
 * that is working at all: at roughly a microsecond per poll it is around ten
 * milliseconds, far longer than the lock needs. The wait for an unlock is
 * deliberately shorter, so it cannot be the long one. */
#define FS_LOCK_POLLS 10000

/* Re-strobe SFSON every this many polls, for the same reason the wait for an
 * unlock re-strobes SRFOFF. */
#define FS_LOCK_RESTROBE_INTERVAL 1024

/* Wait, with a bound, for the CC2400 synthesizer to reach `locked`: 1 after a
 * SFSON strobe, 0 after SRFOFF. Calls `service`, if it is not NULL, between
 * status polls.
 *
 * The bound is what the unbounded `while (!(cc2400_status() & FS_LOCK));` in
 * the mode loops lacked: a radio that stops reporting the state the loop is
 * waiting for keeps the loop from ever looking at requested_mode again, so
 * ubertooth-util -S cannot stop the mode and the board has to be re-plugged.
 * A caller that times out has a synthesizer in the wrong state, so what it
 * reads from the radio next is not a measurement; a caller that only samples,
 * like led_specan(), can carry on and lose that one sample.
 *
 * `service` is for a loop that spends most of its time in here. USB in this
 * firmware is polled from the mode loops (handle_usb()), and endpoint 0 is
 * single-buffered, so a loop that only polls once per pass runs a whole pass
 * behind the host and control requests stall. Passing handle_usb() in keeps
 * endpoint 0 within a few microseconds of the host, and keeps it serviced
 * between this loop's bit-banged SPI transfers rather than inside one, which
 * is what servicing it from the USB interrupt instead would risk.
 *
 * Returns 1 if the synthesizer reported `locked`, 0 if the wait timed out.
 */
int cc2400_wait_fs(int locked, void (*service)(void));

/* Polls before the wait for the 16 MHz crystal oscillator gives up. The
 * oscillator settles in a few hundred microseconds from cold, so this is sized
 * the same way as the wait for a lock: out of reach of a part that is working,
 * and reached only by one that has stopped answering. */
#define XOSC_STABLE_POLLS 10000

/* Wait, with a bound, for the CC2400 to report its 16 MHz crystal oscillator
 * stable. There is nothing to re-strobe: the oscillator is started by the
 * register configuration the caller has already written, so this only polls.
 *
 * The unbounded `while (!(cc2400_status() & XOSC16M_STABLE));` it replaces sits
 * at the top of a mode, before the loop that services USB has started, so a
 * part that never reports stable leaves the board enumerated and answering
 * nothing. A caller that times out has a radio that is not
 * running; it can carry on and produce junk samples, which the host can see,
 * rather than a board that has to be power-cycled.
 *
 * Returns 1 if the oscillator reported stable, 0 if the wait timed out.
 */
int cc2400_wait_xosc_stable(void);

/* Wait for the CC2400 synthesizer to report unlocked, with a bound.
 *
 * Every poll is a bit-banged SPI transaction on the shared CC2400 bus:
 * cc2400_spi() holds CSN low across the transfer and does not mask interrupts.
 * If an interrupt handler drives the radio at the same time -- the LE PHY's DMA
 * handler reads RSSI and strobes SFSON/SRFOFF -- the status byte read here can
 * be corrupt, or the synthesizer can genuinely have been switched back on.
 *
 * An unbounded wait then hangs whichever loop called it. That is fatal in this
 * firmware because USB is polled rather than interrupt-driven (see
 * handle_usb()): with the main loop stuck the board answers no control request
 * at all, so the host sees every request time out (-7), or stall (-9) if
 * endpoint 0 was left halted, until the board is physically re-plugged.
 *
 * Callers only need the radio quiet; the teardown and register re-init that
 * follow a mode change do not depend on the synthesizer having actually
 * reported unlocked.
 *
 * This is cc2400_wait_fs(0, NULL), kept because most callers want exactly
 * that and are not loops that need to service anything while they wait.
 *
 * Returns 1 if the synthesizer reported unlocked, 0 if the wait timed out.
 */
int cc2400_wait_fs_unlock(void);

#endif /* __UBERTOOTH_CC2400_WAIT_H */
