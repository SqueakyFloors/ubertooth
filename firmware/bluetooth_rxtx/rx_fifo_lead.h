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
 * modes receive at 1 Mbit/s, so a byte takes 8 us.
 *
 * The range test sets the modem up itself and is always at that rate. The
 * generic capture does not, so its rate is whatever the host wrote to
 * MDMCTRL, and the radio also has a 250 kbit/s mode. At 250 kbit/s a byte is
 * 32 us, so the lead below is one byte rather than four -- still the byte the
 * wait exists for, but with nothing to spare. A generic capture's record is
 * wrong for other reasons at that rate anyway: the read takes about 14 us a
 * byte, so it would outrun a 32 us byte on the air for the whole record
 * rather than only at byte 0.
 */
#define RX_FIFO_BYTE_100NS 80

/*
 * How much of the packet the radio is given time to receive before the
 * firmware starts clocking it out of the FIFO, in bytes.
 *
 * Both modes start reading as soon as the radio reports SYNC_RECEIVED. The
 * radio sets that flag when the sync word ends, which is before the first
 * payload byte exists, so a read that starts there asks the FIFO for a byte
 * the radio has not put in it yet and gets a wrong one back. Only byte 0 is
 * affected, because the read is slower than the air: cc2400_fifo_read() waits
 * out two delay loops per bit, about 14 us a byte against 8 us on the air, so
 * from the second byte on the radio is further ahead with every byte read.
 * The range test's sender reported this as `mismatch on byte 0` in about one
 * run in five.
 *
 * One byte time would do, since the wait starts at or after the end of the
 * sync word; four of the byte times above leave room for the jitter of the
 * delay loop and of the radio writing the byte. It must stay short: the radio
 * goes on writing into the same 32-byte FIFO while the packet is read out of
 * it, so a wait of a whole packet's air time would leave that FIFO nearly
 * full.
 */
#define RX_FIFO_LEAD_BYTES 4
#define RX_FIFO_LEAD_100NS (RX_FIFO_LEAD_BYTES * RX_FIFO_BYTE_100NS)

/*
 * How many times the wait may read the counter before giving up.
 *
 * The wait needs a few hundred reads. This cap is only a backstop: a timer 0
 * that is not running would otherwise leave the wait spinning, answering no
 * control request, until the watchdog reset the board.
 *
 * Reaching it costs real time: 100,000 turns of a register read and a
 * compare, with USB unserviced, once per packet. By then the radio has
 * refilled the 32-byte FIFO several times over, so that record is wrong
 * throughout rather than only at byte 0. That is worse than the wait working
 * and far better than a loop that never ends: the board goes on answering
 * between packets, so the host can stop the mode. A wait that gets here
 * counts itself, so `ubertooth-util -w` says it happened.
 */
#define RX_FIFO_LEAD_POLLS 100000

/*
 * Let the radio get the start of the packet into its FIFO, then return.
 *
 * The caller must have started timer 0 with clkn_start() before it receives:
 * that is what this measures the lead against. A counter that is not
 * advancing is caught by the poll cap above rather than leaving this waiting
 * forever.
 *
 * When the timer is running this takes tens of microseconds, which is far too
 * short to service USB in and far too short for the host to notice.
 */
void rx_wait_for_fifo(void);

#endif /* __UBERTOOTH_RX_FIFO_LEAD_H */
