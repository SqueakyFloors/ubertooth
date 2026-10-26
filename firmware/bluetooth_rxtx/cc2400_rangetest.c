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

void cc2400_rangetest(volatile u16 *chan_ptr)
{
	u32 command[5];
	u32 result[5];
	int i;
	int j;
	u8 len = 22;
	u8 pa = 0;
	u8 txbuf[len];
	u8 rxbuf[len];
	int got_reply = 0;

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
			for (j = 0; j < len; j++)
				cc2400_set8(FIFOREG, txbuf[j]);
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
		for (j = 0; j < len; j++)
			rxbuf[j] = cc2400_get8(FIFOREG);
		if (cc2400_status() & STATUS_CRC_OK)
			got_reply = 1;
		else
			USRLED_CLR;
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
		rr.valid = 0;
		return;
	}

	// get test result
	rr.valid       = 1;
	rr.request_pa  = rxbuf[18];
	rr.request_num = rxbuf[19];
	rr.reply_pa    = rxbuf[20];
	rr.reply_num   = rxbuf[21];

	// make sure rx packet is as expected
	txbuf[1] = 1; // expected value in rxbuf
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
	int j;
	u8 len = 22;
	u8 pa = 0;
	u8 buf[len];
	int got_request;

	/* This used to be `while (1)` with a FIXME asking for a way to turn it
	 * off. There was none: nothing in here looked at requested_mode and
	 * nothing serviced USB, so the board answered no control request from
	 * the moment the mode started and only a re-plug ended it. */
	while (requested_mode == MODE_REPEATER) {
		cc2400_set(LMTST,   0x2b22);
		cc2400_set(MDMTST0, 0x134b);
		cc2400_set(FSDIV,   *chan_ptr - 1);
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
			for (i = 0; i < len; i++)
				buf[i] = cc2400_get8(FIFOREG);
			if (cc2400_status() & STATUS_CRC_OK)
				got_request = 1;
		}
		if (!got_request)
			break;

		/*
		 * Got a packet, now repeat it. The delay gives the requester time
		 * to switch to receive, and it deliberately does not service USB:
		 * every pass through handle_usb() would stretch it, and the other
		 * board is waiting on this timing. It is a busy-wait of a few tens
		 * of milliseconds, well inside the host's one-second request
		 * timeout, so a request that lands in it is answered late rather
		 * than lost. The mode is re-checked once the delay is over.
		 */
		i = 2000000; while (--i); // allow time for requester to switch to rx
		if (requested_mode != MODE_REPEATER)
			break;
		USRLED_CLR;
		RXLED_CLR;
		cc2400_strobe(SRFOFF);
		cc2400_wait_fs_unlock();
		cc2400_wait_xosc_stable(service_usb);
		cc2400_set(FSDIV, *chan_ptr);
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
				for (j = 0; j < len; j++)
					cc2400_set8(FIFOREG, buf[j]);
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
