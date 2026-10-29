/*
 * Copyright 2013 Dominic Spill, Michael Ossmann
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

#include "cc2400_rangetest.h"
#include "cc2400_wait.h"
#include "ubertooth_clock.h"
#include "ubertooth_usb.h"

/*
 * This range test transmits a Bluetooth-like (but not Bluetooth compatible)
 * packet to a repeater and then receives the repeated packet.  It is only
 * useful if another device is running cc2400_repeater() some distance away.
 *
 * The outgoing packet is transmitted at each power amplifier (PA) level from
 * lowest to highest.  It is sent several times at each level.  The repeater
 * takes the first packet to be received correctly and repeats it in a similar
 * manner.  The result should indicate the lowest successful PA level in each
 * direction.
 *
 * The range test packet consists of:
 *   preamble: 4 bytes
 *   sync word: 4 bytes
 *   payload:
 *     length: 1 byte (21)
 *     packet type: 1 byte (0 = request; 1 = reply)
 *     LPC17xx serial number: 16 bytes
 *     request pa: 1 byte
 *     request number: 1 byte
 *     reply pa: 1 byte
 *     reply number: 1 byte
 *   crc: 2 bytes
 */

rangetest_result rr;

#ifdef TX_ENABLE
/*
 * Hold the CC2400's receive gain amplifier out of reset.
 *
 * Neither of these two modes used to write this, and neither could decode the
 * other: on the bench both receivers reported a strong signal, hundreds of
 * valid RSSI readings, and not one sync word. With this write the exchange
 * completes in under a second.
 *
 * There is no write at boot to inherit. One used to sit in cc2400_init(), and
 * clock_start() reset the CC2400 a few microseconds later and took the
 * register back to the chip's 0xffff, so a board that had just booted read
 * 0xffff and never 0x7fff. Every receive path in this firmware writes it
 * itself, and these two now do the same.
 */
#define RANGETEST_MANAND 0x7fff

/*
 * How long the repeater lets the sender finish before it answers.
 *
 * The sender transmits its whole request burst and only then switches its
 * radio to receive. The repeater answers the first request it decodes, which
 * is normally the first packet of that burst, so without a wait its reply is
 * over before the sender is listening, and the sender waits for a reply that
 * has already been and gone.
 *
 * The burst is 8 amplifier levels times 16 packets of 32 bytes on air at
 * 1 Mbit/s: 256 us of air time each, so about 33 ms. The rest of its length is
 * the sender writing 128 packets into the radio, waiting on the state machine
 * between them and servicing USB as it goes, so it varies with what the host
 * is doing and is not measured here.
 *
 * Waiting too long costs nothing -- the sender listens until it is stopped --
 * and waiting too little costs the whole test: the reply lands while the
 * sender is still transmitting, the sender then listens for a reply that has
 * been and gone, and that is the "no result in either direction" this is here
 * to fix. So this takes the generous end: a second, thirty times the air time.
 * A reply in a second is still immediate to anyone watching.
 */
/*
 * Why these two modes reach the radio's FIFO through cc2400_fifo_write() and
 * cc2400_fifo_read(), like every other mode, and not a byte at a time.
 *
 * They used to write and read the FIFO with cc2400_set8(FIFOREG, b) and
 * cc2400_get8(FIFOREG) in a loop. Each of those is a separate SPI transfer:
 * CSN drops, the FIFO's address goes out, one byte of data follows, CSN rises
 * again. Neither direction works that way, and both failures were measured on
 * a pair of boards.
 *
 * The write put nothing in the FIFO at all. The radio needs the address once
 * and then the data bytes while CSN stays low, and it needs CSN held low for
 * a moment after the last clock (the datasheet's t_ps). A per-byte transfer
 * gives it neither, so the FIFO stayed empty and each packet went out as
 * preamble, sync word and CRC with no data in between.
 *
 * The read came back as 22 zeros -- not the data, and not even the FIFO's
 * real contents: a read at the speed cc2400_spi() clocks at returns wrong
 * bytes whatever the framing, and only the slower clock cc2400_fifo_read()
 * uses gets them right.
 *
 * The two faults hid each other, and hid behind the CRC. The radio's CRC_OK
 * flag does not mean "a good packet arrived": the datasheet defines it as
 * "the next two bytes in the FIFO equal the CRC16 over the bytes already read
 * from the FIFO". Reading 22 zeros out of an empty FIFO gives a CRC of zero,
 * and the next two bytes read as zero too, so the flag came up set. Both
 * boards therefore believed they had a valid packet, exchanged one, and the
 * sender compared 22 zeros against its own 22 bytes.
 */

#define REPEATER_TURNAROUND_MS 1000
#define REPEATER_TURNAROUND_TICKS (3200u * REPEATER_TURNAROUND_MS / 1000u)

/*
 * One byte of a range test packet on the air, in timer 0's 100 ns ticks.
 * These packets go out at 1 Mbit/s, so a byte takes 8 us.
 */
#define RANGETEST_BYTE_100NS 80

/*
 * How much of the packet the radio is given time to receive before the
 * firmware starts clocking it out of the FIFO, in bytes.
 *
 * Both receive loops below start reading as soon as the radio reports
 * SYNC_RECEIVED. The radio sets that flag when the sync word ends, which is
 * before the first payload byte exists, so a read that starts there asks the
 * FIFO for a byte the radio has not put in it yet and gets a wrong one back.
 * That is the `mismatch on byte 0` the sender reported every five to ten
 * runs: byte 0 and never any other byte, because the read is slower than the
 * air. cc2400_fifo_read() waits out two delay loops per bit, about 14 us a
 * byte against 8 us on the air, so from the second byte on the radio is
 * further ahead with every byte read.
 *
 * Only the first byte therefore has to be waited for. The wait starts when the
 * flag is noticed, which is at or after the sync word, so one byte time would
 * do; four leaves room for the jitter of the delay loop and of the radio
 * writing the byte. It must stay short: the rest of the sender's burst keeps
 * arriving into the same 32-byte FIFO while the packet is read out of it, and
 * a wait of the whole packet's air time would leave that FIFO nearly full.
 */
#define RANGETEST_LEAD_BYTES 4
#define RANGETEST_LEAD_100NS (RANGETEST_LEAD_BYTES * RANGETEST_BYTE_100NS)

/*
 * How many times the wait below may read the counter before giving up.
 *
 * The wait needs a few hundred reads. This cap is only a backstop: a timer 0
 * that is not running would otherwise leave the wait spinning, answering no
 * control request, until the watchdog reset the board. Reaching it reads the
 * packet as early as the firmware used to, which is a wrong byte 0 at worst.
 */
#define RANGETEST_LEAD_POLLS 100000

/*
 * Let the radio get the start of the packet into its FIFO, then return.
 *
 * Timer 0 counts 100 ns ticks and resets every CLK100NS_PER_CLKN of them --
 * 312.5 us, one clkn tick -- so this adds up the steps it sees rather than
 * comparing against a deadline the reset would carry it past. Each step is
 * taken modulo that period, which is what makes a step across the reset come
 * out as the time it was: `now - last` alone would be a huge unsigned number
 * there and end the wait at once. A step that comes out longer than the whole
 * lead is discarded rather than counted, which is what covers a period a clock
 * drift fix has lengthened; see the loop, which also says what it does not
 * cover.
 *
 * Both callers start the timer with clkn_start() before they receive. A
 * counter that is not advancing is caught by the poll cap above rather than
 * leaving this waiting forever.
 *
 * Tens of microseconds is far too short to service USB in, and far too short
 * for the host to notice.
 */
static void rangetest_wait_for_fifo(void)
{
	u32 last = T0TC;
	u32 waited = 0;
	u32 polls = RANGETEST_LEAD_POLLS;

	while (waited < RANGETEST_LEAD_100NS && polls-- > 0) {
		u32 now = T0TC;
		u32 step = (now + CLK100NS_PER_CLKN - last) % CLK100NS_PER_CLKN;

		last = now;
		/*
		 * A step longer than the whole lead is not a step this loop
		 * took: one turn of it is a register read and a division, a
		 * handful of ticks. So a larger figure means the modulo above
		 * was taken against the wrong period.
		 *
		 * The timer's match value is not fixed. The clock interrupt
		 * corrects for clock drift by moving it for one period, and a
		 * correction for a clock running slow makes that period about
		 * three times as long; a step taken one tick after the end of
		 * such a period comes out of the fixed modulus as 1,673 ticks
		 * against a lead of 320, which would end the wait before the
		 * radio had written byte 0.
		 *
		 * The host arms that correction with a drift figure while
		 * following Classic traffic, and ending a mode stops the clock
		 * and clears the figure -- but not the match value itself,
		 * which the timer keeps. So the period this mode starts in can
		 * be the long one left behind by the mode before it, until the
		 * first clock interrupt writes the usual value back.
		 *
		 * Dropping the step is the safe direction: the wait runs
		 * another turn, and the poll cap still ends the loop.
		 *
		 * This covers the two period lengths a drift fix produces. The
		 * host can set the match value to a length of its own, through
		 * UBERTOOTH_TRIM_CLOCK and UBERTOOTH_START_HOPPING, and for
		 * some of those a step out of the wrong period is small enough
		 * to pass this guard. Taking the step from the sign of the
		 * difference rather than a fixed modulus, as le_clk100ns_step()
		 * does, is what would cover every length, and the generic
		 * receive's copy of this wait would move with it.
		 */
		if (step <= RANGETEST_LEAD_100NS)
			waited += step;
	}
}

/*
 * Sit out the turnaround delay, servicing USB, and report whether the host
 * asked for another mode while it ran.
 *
 * This was `i = 2000000; while (--i);` with a comment saying it allowed time
 * for the requester to switch to receive. It did not: i is a plain local and
 * the loop has no side effects, so the compiler deleted it -- confirmed in the
 * disassembly -- and the built firmware replied immediately.
 *
 * clkn is the Bluetooth native clock, 3200 ticks per second. The deadline is
 * absolute, so servicing USB inside the wait cannot stretch it the way a
 * counted delay loop would.
 *
 * Returns 1 if the delay ran to its end, 0 if the mode was stopped during it.
 */
static int repeater_turnaround(void)
{
	u32 until = clkn + REPEATER_TURNAROUND_TICKS;

	while ((int32_t)(clkn - until) < 0) {
		if (requested_mode != MODE_REPEATER)
			return 0;
		service_usb();
	}
	return 1;
}

/* Put the radio and the amplifier back to quiet. Both exits from the range test
 * go through here: the one that finishes the test and the one the host stops. */
static void rangetest_radio_off(void)
{
	TXLED_CLR;
	cc2400_strobe(SRFOFF);
	cc2400_wait_fs_unlock();
#ifdef UBERTOOTH_ONE
	PAEN_CLR;
#endif
	RXLED_CLR;
	USRLED_CLR;
}

/*
 * Whether a packet the radio's CRC accepted came from another board, and if it
 * did, which byte says so.
 *
 * Bytes 2 to 17 of the payload are the sender's 16-byte serial number, and the
 * repeater copies them into its reply, so they are what says a packet belongs
 * to this exchange. A packet carrying someone else's serial number is a second
 * pair's range test on the same channel, or a stray packet the CRC happened to
 * accept: the radio's CRC_OK flag says the payload is self consistent, not
 * that it came from the repeater this test is talking to.
 *
 * Nothing else is part of this. The length and type bytes before the serial
 * number, and the levels after it, are checked once the packet has been taken
 * as the reply -- a packet with this board's serial number is this test's
 * answer, and a wrong byte in it means that answer arrived corrupted, which is
 * reported as a mismatch rather than discarded. That is the diagnosis a FIFO
 * read that started a byte early was found with.
 *
 * Returns -1 when the packet is this board's, otherwise the index of the first
 * serial-number byte that is not.
 */
static int rangetest_foreign_packet(const u8 *rxbuf, const u8 *expected)
{
	int i;

	for (i = 2; i < 18; i++)
		if (rxbuf[i] != expected[i])
			return i;
	return -1;
}

void cc2400_rangetest(volatile u16 *chan_ptr)
{
	u32 command[5];
	u32 result[5];
	int i;
	u8 len = 22;
	u8 pa = 0;
	u8 txbuf[len];
	u8 rxbuf[len];
	int got_reply = 0;

	/*
	 * rr is static and this run has not measured anything yet. Clearing it
	 * on entry, as well as where the test ends without a reply, covers the
	 * two remaining ways `ubertooth-util -m` could report the previous run's
	 * levels as this one's: a read while the test is still running, and the
	 * serial-number read below failing and returning early.
	 */
	rr.valid = 0;
	/* What this run discarded, for the same reason: it describes this run. */
	rr.discarded = 0;

	txbuf[0] = len - 1; // length of data (rest of payload)
	txbuf[1] = 0; // request

	// read device serial number
	command[0] = 58;
	iap_entry(command, result);
	if ((result[0] & 0xFF) != 0) //status check
		return;
	txbuf[2] = (result[1] >> 24) & 0xFF;
	txbuf[3] = (result[1] >> 16) & 0xFF;
	txbuf[4] = (result[1] >> 8) & 0xFF;
	txbuf[5] = result[1] & 0xFF;
	txbuf[6] = (result[2] >> 24) & 0xFF;
	txbuf[7] = (result[2] >> 16) & 0xFF;
	txbuf[8] = (result[2] >> 8) & 0xFF;
	txbuf[9] = result[2] & 0xFF;
	txbuf[10] = (result[3] >> 24) & 0xFF;
	txbuf[11] = (result[3] >> 16) & 0xFF;
	txbuf[12] = (result[3] >> 8) & 0xFF;
	txbuf[13] = result[3] & 0xFF;
	txbuf[14] = (result[4] >> 24) & 0xFF;
	txbuf[15] = (result[4] >> 16) & 0xFF;
	txbuf[16] = (result[4] >> 8) & 0xFF;
	txbuf[17] = result[4] & 0xFF;

	txbuf[18] = pa; // request pa
	txbuf[19] = 0; // request number
	txbuf[20] = 0xff; // reply pa
	txbuf[21] = 0xff; // reply number

	// Bluetooth-like modulation
	cc2400_set(MANAND,  RANGETEST_MANAND);
	cc2400_set(LMTST,   0x2b22);
	cc2400_set(MDMTST0, 0x134b);
	cc2400_set(GRMDM,   0x0df1);  // default value
	cc2400_set(FSDIV,   *chan_ptr);
	cc2400_set(SYNCH,   0xf9ae);
	cc2400_set(SYNCL,   0x1584);
	cc2400_set(FREND,   8 | pa);
	cc2400_set(MDMCTRL, 0x0029);
	cc2400_wait_xosc_stable(service_usb);
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, service_usb);
	TXLED_SET;
#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif
	/*
	 * 128 packets, eight amplifier levels of 16. The waits in here answer
	 * control requests, so the host can stop the mode part-way through the
	 * burst; both loops check for that rather than transmitting the rest of
	 * it after the board has agreed to stop.
	 */
	for (pa = 0; pa < 8 && requested_mode == MODE_RANGE_TEST; pa++) {
		cc2400_set(FREND, 8 | pa);
		txbuf[18] = pa;
		for (i = 0; i < 16 && requested_mode == MODE_RANGE_TEST; i++) {
			txbuf[19] = i;
			cc2400_wait_fsm(STATE_STROBE_FS_ON, service_usb);
			// transmit a packet
			cc2400_fifo_write(len, txbuf);
			cc2400_strobe(STX);
		}
	}
	if (requested_mode != MODE_RANGE_TEST) {
		rangetest_radio_off();
		return;
	}

	// sent packet, now look for repeated packet
	cc2400_wait_fsm(STATE_STROBE_FS_ON, service_usb);
	TXLED_CLR;
	cc2400_strobe(SRFOFF);
	cc2400_wait_fs_unlock();
	cc2400_set(FSDIV, *chan_ptr - 1);
	cc2400_wait_xosc_stable(service_usb);
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, service_usb);
	RXLED_SET;
	/* txbuf becomes the reply this test is listening for: the same payload
	 * with the type byte the repeater sends back. Bytes 18 to 21 carry the
	 * levels and numbers each side fills in, and are not compared. */
	txbuf[1] = 1;
	/* A time base for the wait before each FIFO read, as the repeater starts
	 * one for its turnaround delay. Started here rather than on the way into
	 * the mode, so the timer's interrupt does not run through the transmit
	 * burst above, which writes the radio's FIFO unmasked. The dispatch's
	 * cc2400_idle() stops it again on the way out of this mode. */
	clkn_start();
	/*
	 * Wait for the repeater's reply. There is no deadline: the other board
	 * may be out of range, or not running, so this can wait indefinitely and
	 * that is not a failure. What it must not do is wait without servicing
	 * USB, which is how this used to leave a board enumerated and answering
	 * no control request -- not even the one that stops the mode -- until it
	 * was re-plugged.
	 */
	while (!got_reply && requested_mode == MODE_RANGE_TEST) {
		cc2400_wait_fsm(STATE_STROBE_FS_ON, service_usb);
		cc2400_strobe(SRX);
		while (!(cc2400_status() & SYNC_RECEIVED)) {
			service_usb();
			if (requested_mode != MODE_RANGE_TEST)
				break;
		}
		if (requested_mode != MODE_RANGE_TEST)
			break;
		USRLED_SET;
		/* See RANGETEST_LEAD_BYTES: reading the moment the sync word
		 * ends returns a byte 0 the radio has not written yet. */
		rangetest_wait_for_fifo();
		cc2400_fifo_read(len, rxbuf);
		if (cc2400_status() & STATUS_CRC_OK) {
			int differs = rangetest_foreign_packet(rxbuf, txbuf);

			if (differs < 0) {
				got_reply = 1;
			} else {
				/*
				 * Somebody else's packet. Keep listening rather
				 * than end the run on it: another pair's range
				 * test on this channel is not a fault in this
				 * one. The first such packet is recorded for
				 * the host to mention once.
				 */
				if (rr.discarded == 0)
					rr.discarded = 2 + differs;
				USRLED_CLR;
			}
		} else {
			USRLED_CLR;
		}
	}

	// done
	if (got_reply)
		cc2400_wait_fsm(STATE_STROBE_FS_ON, service_usb);
	rangetest_radio_off();

	/*
	 * Only a reply that arrived gives a result, so a test the host stopped
	 * clears rr instead of leaving the previous test's numbers in it:
	 * ubertooth-util -r reads rr whenever rr.valid is set, and would
	 * otherwise report an older test's answer as this one's.
	 */
	if (!got_reply) {
		/* What it discarded goes first, because an empty result is what
		 * tells the host its record now describes the run it is
		 * watching: a read landing between the two stores would take
		 * this run's discarded packet for that one's. The host has
		 * already had this one from its polls. */
		rr.discarded = 0;
		rr.valid = 0;
		return;
	}

	// get test result
	rr.valid       = 1;
	rr.request_pa  = rxbuf[18];
	rr.request_num = rxbuf[19];
	rr.reply_pa    = rxbuf[20];
	rr.reply_num   = rxbuf[21];

	/*
	 * Make sure the reply is as expected. The serial number matched already
	 * -- that is how this packet was taken as the reply -- so what is left
	 * for this to catch is a wrong length or type byte, which says this
	 * board's own reply arrived corrupted. The whole range is still
	 * compared, so the check does not depend on which bytes the filter
	 * above happens to cover.
	 */
	for (i = 0; i < 18; i++)
		if (rxbuf[i] != txbuf[i])
			rr.valid = 2 + i;

	USRLED_CLR;
}
#endif

/* This is the repeater implementation to be used with cc2400_rangetest(). */
#ifdef TX_ENABLE
void cc2400_repeater(volatile u16 *chan_ptr)
{
	int i;
	u8 len = 22;
	u8 pa = 0;
	u8 buf[len];
	int got_request;
	u16 chan;

	/* A time base for the turnaround delay below. The repeater does not
	 * otherwise use clkn, and the dispatch's cc2400_idle() stops it again on
	 * the way out of this mode. */
	clkn_start();

	/* This used to be `while (1)` with a FIXME asking for a way to turn it
	 * off. There was none: nothing in here looked at requested_mode and
	 * nothing serviced USB, so the board answered no control request from
	 * the moment the mode started and only a re-plug ended it. */
	while (requested_mode == MODE_REPEATER) {
		/*
		 * One channel for the whole exchange. A request serviced while this
		 * pass runs writes the mode's channel straight through, and the
		 * reply is tuned after the turnaround delay, so reading the live
		 * value twice could answer on a channel the request was not heard
		 * on. A channel change takes effect on the next pass instead.
		 */
		chan = *chan_ptr;

		/* See RANGETEST_MANAND: without this the repeater decodes nothing. */
		cc2400_set(MANAND,  RANGETEST_MANAND);
		cc2400_set(LMTST,   0x2b22);
		cc2400_set(MDMTST0, 0x134b);
		/* The packet format the sender uses: 4 preamble bytes, a 32-bit sync
		 * word, CRC on. The repeater used to leave this register alone, so it
		 * worked only while the radio still held its reset value -- which
		 * happens to be this. After any mode that writes GRMDM, a capture
		 * mode or `-q`, it could not decode a request at all. */
		cc2400_set(GRMDM,   0x0df1);
		cc2400_set(FSDIV,   chan - 1);
		cc2400_set(SYNCH,   0xf9ae);
		cc2400_set(SYNCL,   0x1584);
		cc2400_set(FREND,   0x0008); // minimum tx power
		cc2400_set(MDMCTRL, 0x0029); // 160 kHz frequency deviation
		cc2400_wait_xosc_stable(service_usb);
		cc2400_strobe(SFSON);
		cc2400_wait_fs(1, service_usb);
		RXLED_SET;
		TXLED_CLR;
		USRLED_CLR;
#ifdef UBERTOOTH_ONE
		PAEN_SET;
#endif
		/* Wait for a request to repeat. No deadline, for the reason given
		 * in cc2400_rangetest(). */
		got_request = 0;
		while (!got_request && requested_mode == MODE_REPEATER) {
			cc2400_wait_fsm(STATE_STROBE_FS_ON, service_usb);
			USRLED_CLR;
			cc2400_strobe(SRX);
			while (!(cc2400_status() & SYNC_RECEIVED)) {
				service_usb();
				if (requested_mode != MODE_REPEATER)
					break;
			}
			if (requested_mode != MODE_REPEATER)
				break;
			USRLED_SET;
			/* The same wait the sender needs. A wrong byte 0 here is not
			 * what the sender reported -- the repeater writes its own
			 * byte 0 into the reply -- but bytes 2 to 17 are the sender's
			 * serial number, which this read copies into that reply. */
			rangetest_wait_for_fifo();
			cc2400_fifo_read(len, buf);
			/*
			 * Only a request is answered. Byte 1 is the packet type,
			 * 0 for a request and 1 for a reply, and a repeater that
			 * answered a reply would answer another repeater -- and
			 * that one would answer back, the two of them repeating
			 * each other on the channel the sender is listening on.
			 */
			if ((cc2400_status() & STATUS_CRC_OK) && buf[1] == 0)
				got_request = 1;
		}
		if (!got_request)
			break;

		/*
		 * Got a packet. Stop receiving before anything else: the rest of
		 * the sender's burst would otherwise keep arriving through the
		 * delay below and fill the radio's 32-byte FIFO, which is the same
		 * FIFO the reply is written into. The delay used to be a loop the
		 * compiler deleted, so this ordering did not matter before.
		 */
		USRLED_CLR;
		RXLED_CLR;
		cc2400_strobe(SRFOFF);
		cc2400_wait_fs_unlock();

		/* Now let the sender finish its burst and arm its receiver. */
		if (!repeater_turnaround())
			break;
		cc2400_wait_xosc_stable(service_usb);
		cc2400_set(FSDIV, chan);
		cc2400_wait_xosc_stable(service_usb);
		cc2400_strobe(SFSON);
		TXLED_SET;
		buf[0] = len - 1; // length of data (rest of payload)
		buf[1] = 1; // reply
		/* Stops part-way through the reply burst if the host stops the
		 * mode, for the reason the request burst does. */
		for (pa = 0; pa < 8 && requested_mode == MODE_REPEATER; pa++) {
			cc2400_set(FREND, 8 | pa);
			buf[20] = pa;
			for (i = 0; i < 16 && requested_mode == MODE_REPEATER; i++) {
				buf[21] = i;
				cc2400_wait_fsm(STATE_STROBE_FS_ON, service_usb);
				cc2400_fifo_write(len, buf);
				cc2400_strobe(STX);
			}
		}
		TXLED_CLR;
		cc2400_strobe(SRFOFF);
		cc2400_wait_fs_unlock();
	}
}
#endif

#ifdef TX_ENABLE
void cc2400_txtest(volatile u8 *mod_ptr, volatile u16 *chan_ptr)
{
	u16 mdmctrl;
	if (*mod_ptr == MOD_BT_BASIC_RATE) {
		mdmctrl = 0x0029; // 160 kHz frequency deviation
	} else if (*mod_ptr == MOD_BT_LOW_ENERGY) {
		mdmctrl = 0x0040; // 250 kHz frequency deviation
	} else {
		/* oops */
		return;
	}
	cc2400_set(LMTST,   0x2b22);
	cc2400_set(MDMTST0, 0x334b); // with PRNG
	cc2400_set(GRMDM,   0x0df1); // default value
	cc2400_set(FSDIV,   *chan_ptr);
	cc2400_set(MDMCTRL, mdmctrl);

	cc2400_wait_xosc_stable(service_usb);
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, service_usb);
	TXLED_SET;
	cc2400_strobe(STX);
#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif
}
#endif
