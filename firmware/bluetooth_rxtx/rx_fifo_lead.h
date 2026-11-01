/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The wait between the radio reporting a sync word and the firmware clocking
 * the packet out of the radio's FIFO.
 *
 * Two modes read the FIFO that way and need the same wait: the generic
 * capture (rx_generic_sync() in bluetooth_rxtx.c) and both sides of the range
 * test (cc2400_rangetest.c). They had a copy each, with the same body and the
 * same three numbers, and a change to one of them had to be remembered for
 * the other.
 */

#ifndef __UBERTOOTH_RX_FIFO_LEAD_H
#define __UBERTOOTH_RX_FIFO_LEAD_H

/*
 * One byte of a received packet on the air, in timer 0's 100 ns ticks. Both
 * modes receive at 1 Mbit/s, so a byte takes 8 us. That is the shortest byte
 * time the radio has, so the lead below is at least one byte at any rate --
 * see rx_generic_sync(), whose rate the host chooses.
 */
#define RX_FIFO_BYTE_100NS 80

/*
 * How much of the packet the radio is given time to receive before the
 * firmware starts clocking it out of the FIFO, in bytes.
 *
 * Both modes start reading as soon as the radio reports SYNC_RECEIVED, and
 * the radio sets that flag when the sync word ends -- before the first
 * payload byte exists. A read that starts there asks the FIFO for a byte the
 * radio has not put in it yet and gets a wrong one back. Only byte 0 is
 * affected, because the read is slower than the air: cc2400_fifo_read() waits
 * out two delay loops per bit, about 14 us a byte against 8 us on the air, so
 * from the second byte on the radio is further ahead with every byte read.
 * The range test's sender reported this as `mismatch on byte 0` in about one
 * run in five.
 *
 * One byte time would do, since the wait starts at or after the end of the
 * sync word; four leave room for the jitter of the delay loop and of the
 * radio writing the byte. It must stay short: the radio goes on writing into
 * the same 32-byte FIFO while the packet is read out of it, so a wait of a
 * whole packet's air time would leave that FIFO nearly full.
 */
#define RX_FIFO_LEAD_BYTES 4
#define RX_FIFO_LEAD_100NS (RX_FIFO_LEAD_BYTES * RX_FIFO_BYTE_100NS)

/*
 * How many times the wait may read the counter before giving up.
 *
 * The wait needs a few hundred reads, so this is a backstop and not the
 * bound: a timer 0 that is not running would otherwise leave the wait
 * spinning, answering no control request, until the watchdog reset the board.
 *
 * Reaching it costs real time -- 100,000 turns with USB unserviced, once per
 * packet -- and by then the radio has refilled the 32-byte FIFO several times
 * over, so that record is wrong throughout rather than only at byte 0. A wait
 * that gets here counts itself, so `ubertooth-util -w` says it happened.
 */
#define RX_FIFO_LEAD_POLLS 100000

/*
 * Let the radio get the start of the packet into its FIFO, then return.
 *
 * The caller must have started timer 0 with clkn_start() before it receives:
 * that is what the lead is measured against. A counter that is not advancing
 * is caught by the poll cap above rather than leaving this waiting forever.
 *
 * When the timer is running this takes tens of microseconds, far too short to
 * service USB in and far too short for the host to notice.
 */
void rx_wait_for_fifo(void);

#endif /* __UBERTOOTH_RX_FIFO_LEAD_H */
