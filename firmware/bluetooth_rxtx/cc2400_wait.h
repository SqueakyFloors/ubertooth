/*
 * Bounded waits on CC2400 synthesizer state.
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#ifndef __UBERTOOTH_CC2400_WAIT_H
#define __UBERTOOTH_CC2400_WAIT_H

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

/* Polls before cc2400_wait_fs_lock() gives up. Locking takes the synthesizer
 * a few hundred microseconds, so this is sized to be out of reach of a radio
 * that is working at all: at roughly a microsecond per poll it is around ten
 * milliseconds, an order of magnitude more than the lock needs. */
#define FS_LOCK_POLLS 10000

/* Re-strobe SFSON every this many polls, for the same reason the unlock wait
 * re-strobes SRFOFF. */
#define FS_LOCK_RESTROBE_INTERVAL 1024

/* Wait for the CC2400 synthesizer to report locked, with a bound.
 *
 * The counterpart of cc2400_wait_fs_unlock(), after a SFSON strobe, and
 * bounded for the same reason: `while (!(cc2400_status() & FS_LOCK));` inside
 * a mode loop means a radio that never locks keeps the loop from ever looking
 * at requested_mode again, so ubertooth-util -S cannot stop the mode and the
 * board has to be re-plugged.
 *
 * A caller that times out has an unlocked synthesizer, so whatever it reads
 * from the radio next is not a measurement. Callers that only sample, like
 * led_specan(), can carry on and lose that one sample.
 *
 * Returns 1 if the synthesizer reported locked, 0 if the wait timed out.
 */
int cc2400_wait_fs_lock(void);

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
 * endpoint 0 was left halted, until the board is physically re-plugged. See
 * issue #158.
 *
 * Callers only need the radio quiet; the teardown and register re-init that
 * follow a mode change do not depend on the synthesizer having actually
 * reported unlocked.
 *
 * Returns 1 if the synthesizer reported unlocked, 0 if the wait timed out.
 */
int cc2400_wait_fs_unlock(void);

#endif /* __UBERTOOTH_CC2400_WAIT_H */
