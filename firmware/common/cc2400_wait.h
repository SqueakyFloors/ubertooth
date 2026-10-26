/*
 * Bounded waits on CC2400 radio state.
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#ifndef __UBERTOOTH_CC2400_WAIT_H
#define __UBERTOOTH_CC2400_WAIT_H

#include <stddef.h>

/* Angle brackets, not quotes: this header sits next to firmware/common's own
 * ubertooth.h, and a quoted include would always pick that one up -- including
 * in the host tests, which put a hardware-free stand-in earlier on the include
 * path and cannot compile the real one. */
#include <ubertooth.h>

/*
 * What a caller does when one of these waits gives up: carry on. The radio is
 * in the wrong state, so the next thing the caller reads from it is not a
 * measurement and the next packet it sends may not go out -- but the bound is
 * there to end the loop, not to make the radio work, and every alternative
 * loses the same packet or sample. Carrying on is what keeps the board
 * answering the host, which can then stop the mode; the hang these replace
 * needed the board re-plugged.
 *
 * So no caller has to check the return value, and most do not. A site that
 * must not carry on says why where it checks.
 */

/* Polls before cc2400_wait_fs_unlock() gives up. Each poll is one bit-banged
 * 8-bit SPI transaction with no delay loops (cc2400_spi()), about 1.6
 * microseconds, putting the bound a little under two milliseconds. It is kept
 * short deliberately: hop() calls this from bt_stream_rx()'s streaming loop,
 * where a long stall costs symbols. */
#define FS_UNLOCK_POLLS 1000

/* Re-strobe SRFOFF every this many polls. Short, so the synthesizer is pushed
 * back off quickly if a handler switched it on, rather than after most of the
 * budget. */
#define FS_UNLOCK_RESTROBE_INTERVAL 32

/* Polls before the wait for a lock gives up. Locking takes the synthesizer a
 * few hundred microseconds, so this is sized to be out of reach of a radio
 * that is working at all: at about 1.6 microseconds per poll it is around
 * sixteen milliseconds, far longer than the lock needs. The wait for an unlock
 * is deliberately shorter, so it cannot be the long one. */
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
 * `service` is for a loop that spends most of its time in here. USB in the
 * bluetooth_rxtx image is polled from the mode loops (handle_usb()), and
 * endpoint 0 is single-buffered, so a loop that only polls once per pass runs
 * a whole pass behind the host and control requests stall. Passing
 * handle_usb() in keeps endpoint 0 within a few microseconds of the host, and
 * keeps it serviced between this loop's bit-banged SPI transfers rather than
 * inside one, which is what servicing it from the USB interrupt instead would
 * risk. The BR/EDR image drives USB from the USB interrupt, so its callers
 * pass NULL.
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
 * stable. Calls `service`, if it is not NULL, between reads, for the same
 * reason cc2400_wait_fs() does. There is nothing to re-strobe: the oscillator
 * is started by the register configuration the caller has already written, so
 * this only polls.
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
int cc2400_wait_xosc_stable(void (*service)(void));

/* Wait for the CC2400 synthesizer to report unlocked, with a bound.
 *
 * The status byte each poll reads is itself intact: cc2400_spi() masks
 * interrupts for the length of a transfer, so a handler cannot interleave with
 * it. What the poll can still miss is the synthesizer going back on under it,
 * because an interrupt handler may strobe it -- the LE PHY's DMA handler
 * strobes SFSON and SRFOFF between polls -- so the state being waited for
 * never arrives.
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

/* Polls before the wait on the radio's state machine gives up. Each poll is a
 * cc2400_get(), a 24-bit bit-banged transfer -- 8 bits of address and 16 of
 * data -- which takes about 4.6 microseconds with interrupts masked, so 2000
 * polls is roughly 9 milliseconds, and longer again for the callers that pass
 * a service callback, since each poll then also services USB.
 *
 * The states callers wait for are reached in well under a millisecond: the
 * longest is the end of a transmit, and the longest packet any caller sends is
 * the range test's 22 bytes, 176 microseconds at 1 Mbit. The BR/EDR image's
 * waits are shorter still -- the state machine reaching idle after a strobe
 * off, or receive after a strobe to receive. So the bound is out of reach of a
 * radio that is working at all, which is all it has to be.
 *
 * Running to the bound is not free, and it costs the BR/EDR image most: its
 * waits run inside TIMER0_IRQHandler, which fires every 312.5 microseconds,
 * and btphy_handler() counts one slot per interrupt. Nine milliseconds in the
 * handler means around 28 matches the timer cannot latch, so the piconet clock
 * loses that many counts and stays off by them. A bound sized to the slot,
 * the way tx_task.c's own wait is, would hold the damage to one slot -- but
 * the state after a strobe to receive needs the synthesizer locked first, so
 * how short is safe has to be measured on a board before it is tightened. The
 * alternative this replaces was overrunning every slot from then on.
 */
#define FSM_POLLS 2000

/* Wait, with a bound, for the CC2400's state machine to report `state`: one of
 * the STATE_* values, compared against the low five bits of FSMSTATE. Calls
 * `service`, if it is not NULL, between reads, for the same reason
 * cc2400_wait_fs() does.
 *
 * The bluetooth_rxtx image waits here for STATE_STROBE_FS_ON, which the state
 * machine returns to when a transmit finishes. The unbounded
 * `while ((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);` it replaces
 * sits in br_transmit()'s streaming loop, so a radio that stops advancing left
 * the board enumerated and answering no control request.
 *
 * The BR/EDR image waits here for STATE_IDLE before it retunes and for
 * STATE_STROBE_RX after it strobes the radio to receive, from a slot task
 * running in the timer interrupt. A task that does not return blocks the USB
 * interrupt behind it, so there the same stall shows up as a board that has
 * stopped answering while its LEDs are still on.
 *
 * Returns 1 if the state machine reported `state`, 0 if the wait timed out.
 */
int cc2400_wait_fsm(u8 state, void (*service)(void));

#endif /* __UBERTOOTH_CC2400_WAIT_H */
