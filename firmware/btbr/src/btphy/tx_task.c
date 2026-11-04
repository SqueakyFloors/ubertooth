/* TX task
 *
 * Copyright 2020 Etienne Helluy-Lafont, Univ. Lille, CNRS.
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
#include <string.h>
#include <ubtbr/defines.h>
#include <ubtbr/cfg.h>
#include <ubtbr/codec.h>
#include <ubtbr/debug.h>
#include <ubtbr/bb_msg.h>
#include <ubtbr/ubertooth_dma.h>
#include <ubtbr/rf.h>
#include <ubtbr/hop.h>
#include <ubtbr/btphy.h>
#include <ubtbr/tdma_sched.h>
#include <ubtbr/tx_task.h>

static struct {
	tx_task_cb_t cb;
	void *cb_arg;
	bbhdr_t *user_hdr;
	bbcodec_t codec;
	uint8_t *user_data;
	/* The format the caller chose, or NULL to take it from the header's
	 * TYPE code, with the data length a BB_LEN_NEGOTIATED format needs
	 * told separately. */
	const bbcodec_types_t *user_fmt;
	unsigned user_data_bytes;
	unsigned data_size;
	/* Whether the transmitter was ever turned on for this packet. A packet
	 * the radio was not ready for has no end of transmission to wait for,
	 * so tx_finalize() has to be able to tell the two apart. */
	int on_air;
	/* Whether tx_prepare() finished preparing a packet for tx_execute() to
	 * send. The two go on the scheduler together, so a transmit
	 * tx_prepare() gives up on leaves a tx_execute() in the next slot that
	 * cannot be taken back off; tx_execute() turns nothing on while this
	 * is clear. */
	int prepared;
	uint8_t air_data[BYTE_ALIGN(4+54+MAX_ACL_SIZE*12)]; // to contain trailer+header+payload
	uint8_t *p;
} tx_task;

static int tx_finalize(uint8_t p1, uint8_t p2, uint16_t p3);

/* Move the hop phase on by the one transmit that is about to happen, or that
 * was about to.
 *
 * An inquirer sweeps its phase once per transmit, and a pager advances its
 * response phase before each FHS it sends. The substates that answer a scan do
 * not advance here: their answers go out in the slot that pairs with the one
 * they listened in, on the phase they heard.
 *
 * The phase belongs to the attempt rather than to the packet, so it moves once
 * per transmit whether that packet reaches the air or not. For the page
 * response that is what keeps the two ends together: the paged device advances
 * its own phase for every window it opens, so a phase left standing here is
 * one behind for the rest of the attempt, with every later transmit going out
 * on the frequency the device's previous window used and nothing in the
 * attempt to close the gap. An inquirer's phase is one of 32 guesses it sweeps
 * rather than a counter anyone shares, so a phase left standing there costs it
 * only the guess it skips, but the rate is still one step per transmit and
 * that is where it is kept. */
static void tx_advance_hop_phase(void)
{
	if (btphy.mode == BT_MODE_INQUIRY || btphy.mode == BT_MODE_PAGING_RESPONSE)
		hop_increment();
}

static void tx_fifo_cb(void *arg)
{
	unsigned len;

	if (tx_task.p == NULL)
		DIE("No p in tx_fifo_cb (size %d)\n", tx_task.data_size);

	len = MIN(tx_task.data_size, PHY_FIFO_THRESHOLD); // (128usec of tx)
	btphy_rf_fifo_write(tx_task.p, len);

	tx_task.p += len;
	tx_task.data_size -= len;
	if (tx_task.data_size == 0)
	{
		btphy_rf_disable_int();
	}
	else
	{
		// encode more if required
		bbcodec_encode_chunk(&tx_task.codec, tx_task.air_data, tx_task.user_hdr, tx_task.user_data);
	}
}

static int tx_prepare(uint8_t p1, uint8_t p2, uint16_t p3)
{
	unsigned len;
	int rc;
	uint8_t chan, nslots = 1;
	uint32_t tx_clkn = btphy_cur_clkn()+1;

	tx_advance_hop_phase();

	/* Hop & prepare TX (SFSON) */
	chan = hop_channel(tx_clkn);
	btphy_rf_tune_chan(BT_FREQ_MIN+chan, 1);
	cc2400_strobe(SFSON);
	btphy_rf_cfg_tx();

	/* Push 32 hi bits of Access code */
	btphy_rf_fifo_write(btphy.chan_sw_hi, 4);

	/* Prepare data */
	if (tx_task.user_hdr)
	{
		bbcodec_init(&tx_task.codec,
			btphy_whiten_seed(tx_clkn),
			btphy.chan_uap, 1, 0);

		if (tx_task.user_fmt)
			bbcodec_set_tx_format(&tx_task.codec, tx_task.user_fmt,
				tx_task.user_data_bytes);

		memset(tx_task.air_data, 0, sizeof(tx_task.air_data));
		/* encode the header in tx buffer & configure codec */
		bbcodec_encode_header(&tx_task.codec, tx_task.air_data, tx_task.user_hdr, btphy.chan_trailer, tx_task.user_data);

		tx_task.p = tx_task.air_data;
		tx_task.data_size = tx_task.codec.air_bytes;
		nslots = tx_task.codec.t->nslots;

		if (tx_task.user_data)
		{
			/* encode the data in tx buffer */
			bbcodec_encode_chunk(&tx_task.codec, tx_task.air_data, tx_task.user_hdr, tx_task.user_data);
		}

		/* Push start (less that 32 bytes) of message in fifo */
		len = MIN(32-4, tx_task.data_size);
		btphy_rf_fifo_write(tx_task.p, len);
		tx_task.p += len;
		tx_task.data_size -= len;
	}
	/* Schedule end of transmission */
	if(btphy.mode == BT_MODE_PAGING ){
		/* Page messages should start and end in the same TDAM slot(clk) as we need to finalize() current one
		   And prepare() the next one for sending at clk+1 the 2nd page */
		rc = tdma_schedule(1*nslots, tx_finalize, 0, 0, 0, -2);
	}else{
		/* End of transmission in most packets CAN be in the following TDAM window post-tx. */
		rc = tdma_schedule(2*nslots, tx_finalize, 0, 0, 0, -3);
	}
	if (rc < 0)
	{
		/* This packet has no end: the slot that would have ended its
		 * transmission already holds the four tasks a slot can take,
		 * which is what a page does to the slot it ends in -- it puts
		 * tx_execute(), tx_finalize() and the next tx_prepare() there.
		 * tx_finalize() is what stops the radio, releases the packet
		 * and reports the completion the caller is waiting on, so this
		 * is the one context where the completion cannot be left to
		 * it: unreported, the caller schedules neither its next
		 * receive nor its next transmit and the link stops forever.
		 *
		 * An inquiry or a page response has already moved its hop
		 * phase on for this transmit, which is where the other end's
		 * phase is too, and there is no way to put a phase back. The
		 * packet is lost either way and the retransmission the
		 * protocol does covers it, so the phase is left where it
		 * is. */
		cprintf("TX: cannot schedule end of transmission, packet not sent\n");

		/* The radio is tuned with its synthesizer strobed on by now.
		 * tx_execute() stays on the scheduler and turns nothing on,
		 * because `prepared` is never set. */
		btphy_rf_idle();

		/* Release the packet before reporting it, the order
		 * tx_finalize() uses: the caller may hand the next packet over
		 * from its callback. Nothing went on the air. */
		tx_task_reset();
		if (tx_task.cb)
			tx_task.cb(0, tx_task.cb_arg);

		return 0;
	}

	tx_task.prepared = 1;

	return 0; 
}

static int tx_execute(uint8_t p1, uint8_t p2, uint16_t p3)
{
	if (!tx_task.prepared)
	{
		/* tx_prepare() gave this transmit up, and has already said so
		 * and reported the completion. There is nothing to send. */
		return 0;
	}
	tx_task.prepared = 0;

	if (!FS_TUNED())
	{
		/* The synthesizer has not locked in the slot since tx_prepare()
		 * strobed it on, so this packet cannot go out. Stop the radio
		 * and leave the slot to tx_finalize(), which tx_prepare() has
		 * already put on the scheduler: that is what reports the
		 * completion the caller is waiting on, so the connection goes
		 * on to its next slot. Reporting it here as well would report
		 * it twice. */
		cprintf("TX: synthesizer not locked, packet not sent\n");
		tx_task.on_air = 0;
		btphy_rf_idle();
		return 0;
	}
	/* Start TX phy */
	btphy_rf_tx();
	tx_task.on_air = 1;
	if (tx_task.data_size)
		btphy_rf_enable_int(tx_fifo_cb, NULL, 1);
	
	if ( btphy.mode!=BT_MODE_PAGING && btphy_cur_clkn() & 1 )
		DIE("txe: wrong clkn %x", btphy_cur_clkn());
	else if ( btphy.mode==BT_MODE_PAGING && (btphy_cur_clkn() & 2) )
		DIE("txe: wrong clkn %x: should be in Central slot (0 or 1 mod 4) when paging");

	return 0;
}

#define	TX_MAX_WAIT 1500
static int tx_finalize(uint8_t p1, uint8_t p2, uint16_t p3)
{
	/* Whether the packet went on the air, which tx_task_reset() below
	 * clears before the caller is told. A transmit the radio never
	 * reported the end of still went out: all that was lost is when it
	 * stopped. */
	int sent = tx_task.on_air;

	/* Wait for the end of the transmission, until 150usec into the slot.
	 * The radio is stopped either way: a transmit still going at the
	 * deadline is one the radio never reported the end of.
	 *
	 * A packet the transmitter was never turned on for has no end to wait
	 * for, and the state this waits for cannot come: the wait would spend
	 * the slot polling the radio from the timer interrupt and would count
	 * a radio that never answered, which ubertooth-util -w reports. */
	if (tx_task.on_air)
		btphy_rf_wait_fsm(STATE_STROBE_FS_ON, TX_MAX_WAIT);
	
	/* Stop phy TX */ 
	btphy_rf_idle();

	tx_task_reset();
	if(tx_task.cb)
		tx_task.cb(sent, tx_task.cb_arg);

	return 0;
}

const struct tdma_sched_item tx_sched_set[] = {
	SCHED_ITEM(tx_prepare,		 0, 0, 0), 	SCHED_END_FRAME(),	// 3
	SCHED_ITEM(tx_execute,   	-3, 0, 0), 	SCHED_END_FRAME(),	// 0
	SCHED_END_SET()
};

/* How many slot tasks a transmit puts on the scheduler: the two above. Each
 * item of the set is followed by an end-of-frame marker and the set by an
 * end-of-set one, so an item added without changing this stops the build. */
#define TX_SCHED_ITEMS	2
_Static_assert(ARRAY_SIZE(tx_sched_set) == 2 * TX_SCHED_ITEMS + 1,
	"TX_SCHED_ITEMS does not match tx_sched_set");

void tx_task_reset(void)
{
	btphy_rf_disable_int();
	tx_task.on_air = 0;
	tx_task.prepared = 0;
	tx_task.user_hdr = NULL;
	tx_task.user_data = NULL;
	tx_task.user_fmt = NULL;
	tx_task.user_data_bytes = 0;
	tx_task.p = NULL;
	/* Only tx_fifo_cb() counts this down, and a packet that never went on
	 * the air never ran it. Left standing, it tells the next transmit that
	 * there are bytes to push from a buffer pointer that has just been
	 * cleared, and the transmits that carry no header of their own -- the
	 * paging and inquiry ID packets -- do not set it again. */
	tx_task.data_size = 0;
}

void tx_task_schedule_fmt(unsigned delay, tx_task_cb_t cb, void*cbarg,
		bbhdr_t *hdr, uint8_t *data,
		const bbcodec_types_t *fmt, unsigned data_bytes)
{
	int placed;

	if (tx_task.user_hdr)
		DIE("txs: already got pkt");
	tx_task.cb = cb;
	tx_task.cb_arg = cbarg;
	tx_task.user_hdr = hdr;
	tx_task.user_data = data;
	tx_task.user_fmt = fmt;
	tx_task.user_data_bytes = data_bytes;

	/* The packet is handed over before it is scheduled, because the slot
	 * clock interrupt can run tx_prepare() the moment it is on the
	 * scheduler. */
	placed = tdma_schedule_set(delay, tx_sched_set, 0);

	/* Nothing of the transmit was placed, so no task of it will ever run.
	 * tx_finalize() is what usually reports the completion the caller
	 * waits on, and that callback is what schedules the link's next
	 * receive or transmit, so there is nothing left to report it but this:
	 * unreported, the link stops forever and the next packet the caller
	 * hands over stops the firmware with "already got pkt". The radio is
	 * never tuned or strobed, because tx_prepare() is what does that. */
	if (placed <= 0)
	{
		cprintf("TX: cannot schedule transmit, packet not sent\n");

		/* The phase this transmit was to have moved. tx_prepare() is
		 * what normally moves it, and no task of this transmit will
		 * ever run, so it is moved here instead: this is the only
		 * not-sent path that would otherwise leave it short.
		 *
		 * Before the completion is reported, not after: the substate
		 * this reads is the one the transmit belonged to, and the
		 * callback is where a caller gives a page up and moves the
		 * board to another one. */
		tx_advance_hop_phase();

		/* Release the packet before reporting it, the order
		 * tx_finalize() uses: the caller may hand the next packet over
		 * from its callback. Nothing went on the air. */
		tx_task_reset();
		if (tx_task.cb)
			tx_task.cb(0, tx_task.cb_arg);
		return;
	}

	/* tx_prepare() was placed and tx_execute() was not, so the radio is
	 * still tuned and the packet still encoded, and the transmitter is
	 * never turned on. tx_prepare() schedules tx_finalize() as always,
	 * which reports the completion and releases the packet, so the link
	 * carries on; only the packet is lost, and the protocol's own
	 * retransmission covers it. */
	if (placed < TX_SCHED_ITEMS)
		cprintf("TX: cannot schedule start of transmission, packet not sent\n");
}

void tx_task_schedule(unsigned delay, tx_task_cb_t cb, void*cbarg, bbhdr_t *hdr, uint8_t *data)
{
	tx_task_schedule_fmt(delay, cb, cbarg, hdr, data, NULL, 0);
}
