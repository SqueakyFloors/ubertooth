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

/*
 * Blocking waits in the range test / repeater must not spin forever: if the RF
 * handshake never completes (no peer, or out of sync) the device would hang
 * with the USB command handler never regaining the main loop (only a replug
 * recovers it). WAIT_OR_BAIL spins on a condition but gives up when either the
 * host requests a different mode (e.g. -S -> MODE_IDLE, set from the USB IRQ)
 * or a clkn deadline passes, then jumps to the function's bail: cleanup.
 * clkn ticks at the Bluetooth 3200 Hz rate.
 */
extern volatile uint32_t clkn;
extern volatile uint8_t  mode;
extern volatile uint8_t  requested_mode;
#define RANGETEST_TIMEOUT_TICKS (3200u * 30u)  /* ~30 s anti-wedge net; -S is the real stop */
#define WAIT_OR_BAIL(cond) do {                                   \
		uint32_t _deadline = clkn + RANGETEST_TIMEOUT_TICKS;      \
		while (cond) {                                            \
			if (requested_mode != mode) goto bail;                \
			if ((int32_t)(clkn - _deadline) > 0) goto bail;       \
		}                                                         \
	} while (0)

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
	WAIT_OR_BAIL(!(cc2400_status() & XOSC16M_STABLE));
	cc2400_strobe(SFSON);
	WAIT_OR_BAIL(!(cc2400_status() & FS_LOCK));
	TXLED_SET;
	/* Enable USB interrupts HERE, only after the CC2400 is configured and
	 * frequency-locked above. Do NOT move this to the top of the function.
	 * REGRESSION HISTORY: enabling ISER0_ISE_USB at function entry (to make -S
	 * responsive) let a USB IRQ fire during the CC2400 register/strobe sequence
	 * and corrupted radio bring-up -- the radio never came up, TX/RX never ran,
	 * and the repeater showed no LEDs. Enabling it after FS_LOCK keeps -S
	 * serviceable during the busy waits below without disturbing setup. */
	ISER0 = ISER0_ISE_USB;
#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif
	for (pa = 0; pa < 8; pa++) {
		cc2400_set(FREND, 8 | pa);
		txbuf[18] = pa;
		for (i = 0; i < 16; i++) {
			txbuf[19] = i;
			WAIT_OR_BAIL((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
			// transmit a packet
			for (j = 0; j < len; j++)
				cc2400_set8(FIFOREG, txbuf[j]);
			cc2400_strobe(STX);
		}
	}
	// sent packet, now look for repeated packet
	WAIT_OR_BAIL((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
	TXLED_CLR;
	cc2400_strobe(SRFOFF);
	WAIT_OR_BAIL((cc2400_status() & FS_LOCK));
	cc2400_set(FSDIV, *chan_ptr - 1);
	WAIT_OR_BAIL(!(cc2400_status() & XOSC16M_STABLE));
	cc2400_strobe(SFSON);
	WAIT_OR_BAIL(!(cc2400_status() & FS_LOCK));
	RXLED_SET;
	while (1) {
		WAIT_OR_BAIL((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
		cc2400_strobe(SRX);
		WAIT_OR_BAIL(!(cc2400_status() & SYNC_RECEIVED));
		USRLED_SET;
		for (j = 0; j < len; j++)
			rxbuf[j] = cc2400_get8(FIFOREG);
		if (cc2400_status() & STATUS_CRC_OK)
			break;
		USRLED_CLR;
	}

	// done
	WAIT_OR_BAIL((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
	cc2400_strobe(SRFOFF);
	WAIT_OR_BAIL((cc2400_status() & FS_LOCK));
#ifdef UBERTOOTH_ONE
	PAEN_CLR;
#endif
	RXLED_CLR;

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
	return;

bail:
	/* Timed out or a mode change was requested: shut the radio down and leave
	 * cleanly so the main loop resumes and USB stays responsive. */
	cc2400_strobe(SRFOFF);
#ifdef UBERTOOTH_ONE
	PAEN_CLR;
#endif
	TXLED_CLR; RXLED_CLR; USRLED_CLR;
	rr.valid = 0;   /* host reads this as "invalid range test result" */
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

	//FIXME allow to be turned off
	while (requested_mode == mode) {
		cc2400_set(LMTST,   0x2b22);
		cc2400_set(MDMTST0, 0x134b);
		cc2400_set(FSDIV,   *chan_ptr - 1);
		cc2400_set(SYNCH,   0xf9ae);
		cc2400_set(SYNCL,   0x1584);
		cc2400_set(FREND,   0x0008); // minimum tx power
		cc2400_set(MDMCTRL, 0x0029); // 160 kHz frequency deviation
		WAIT_OR_BAIL(!(cc2400_status() & XOSC16M_STABLE));
		cc2400_strobe(SFSON);
		WAIT_OR_BAIL(!(cc2400_status() & FS_LOCK));
		RXLED_SET;
		TXLED_CLR;
		USRLED_CLR;
#ifdef UBERTOOTH_ONE
		PAEN_SET;
#endif
		/* Enable USB interrupts HERE, only after the CC2400 is configured and
		 * frequency-locked above. Do NOT move this to the top of the function.
		 * REGRESSION HISTORY: enabling ISER0_ISE_USB at function entry (to make
		 * -S responsive) let a USB IRQ fire during the CC2400 register/strobe
		 * sequence and corrupted radio bring-up -- the radio never came up and
		 * the repeater showed no LEDs. Enabling it after FS_LOCK keeps -S
		 * serviceable during the busy waits below without disturbing setup. */
		ISER0 = ISER0_ISE_USB;
		while (1) {
			WAIT_OR_BAIL((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
			USRLED_CLR;
			cc2400_strobe(SRX);
			WAIT_OR_BAIL(!(cc2400_status() & SYNC_RECEIVED));
			USRLED_SET;
			for (i = 0; i < len; i++)
				buf[i] = cc2400_get8(FIFOREG);
			if (cc2400_status() & STATUS_CRC_OK)
				break;
		}
		// got packet, now repeat it
		i = 2000000; while (--i); // allow time for requester to switch to rx
		USRLED_CLR;
		RXLED_CLR;
		cc2400_strobe(SRFOFF);
		WAIT_OR_BAIL((cc2400_status() & FS_LOCK));
		WAIT_OR_BAIL(!(cc2400_status() & XOSC16M_STABLE));
		cc2400_set(FSDIV, *chan_ptr);
		WAIT_OR_BAIL(!(cc2400_status() & XOSC16M_STABLE));
		cc2400_strobe(SFSON);
		TXLED_SET;
		buf[0] = len - 1; // length of data (rest of payload)
		buf[1] = 1; // reply
		for (pa = 0; pa < 8; pa++) {
			if (requested_mode != mode) goto bail;   /* prompt stop on -S */
			cc2400_set(FREND, 8 | pa);
			buf[20] = pa;
			for (i = 0; i < 16; i++) {
				buf[21] = i;
				WAIT_OR_BAIL((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
				for (j = 0; j < len; j++)
					cc2400_set8(FIFOREG, buf[j]);
				cc2400_strobe(STX);
			}
		}
		TXLED_CLR;
		cc2400_strobe(SRFOFF);
		WAIT_OR_BAIL((cc2400_status() & FS_LOCK));
	}

bail:
	/* mode change requested or timeout: shut down and return cleanly. */
	cc2400_strobe(SRFOFF);
#ifdef UBERTOOTH_ONE
	PAEN_CLR;
#endif
	TXLED_CLR; RXLED_CLR; USRLED_CLR;
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

	while (!(cc2400_status() & XOSC16M_STABLE));
	cc2400_strobe(SFSON);
	while (!(cc2400_status() & FS_LOCK));
	TXLED_SET;
	cc2400_strobe(STX);
#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif
}
#endif
