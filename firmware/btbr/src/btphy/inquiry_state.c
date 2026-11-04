/* Inquiry state
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
#include <ubtbr/cfg.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/btphy.h>
#include <ubtbr/tdma_sched.h>
#include <ubtbr/bb.h>
#include <ubtbr/rx_task.h>
#include <ubtbr/tx_task.h>

/* Max duration : 60 seconds */
#define INQ_MAX_TICKS (CLKN_RATE*60)

static struct {
	uint32_t clkn_start;
} inquiry_state;

static int inquiry_canceled(void)
{
	return btctl_get_state() != BTCTL_STATE_INQUIRY;
}

#define TX_PREPARE_IDX	3  // We will transmit at clkn1_0 = 0

static void inquiry_schedule(unsigned delay);

static int inquiry_rx_cb(msg_t *msg, void *arg, int time_offset)
{
	btctl_hdr_t *h = (btctl_hdr_t*)msg->data;
	btctl_rx_pkt_t *pkt;

	if (inquiry_canceled())
	{
		msg_free(msg);
		return 0;
	}

	if (h->type != BTCTL_RX_PKT)
		DIE("paging : expect acl rx");
	pkt = (btctl_rx_pkt_t *)h->data;

	if (!BBPKT_GOOD_CRC(pkt))
	{
		msg_free(msg);
		inquiry_schedule(3&(TX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX()));	// schedule tx in next slot
	}
	else
	{
		/* The device this found goes on the queue before anything below
		 * can end the inquiry: the host reads this queue in order and
		 * stops at the report that the board is idle, so a device
		 * queued behind one is never read. */
		if (btctl_tx_enqueue(msg))
		{
			DIE("txq full in inquiry\n");
		}
		if (pkt->bb_hdr.type == BB_TYPE_FHS)
		{
			/* Schedule rx EFHS for next RX slot. No window for the
			 * extended response leaves nothing to wait for, so go
			 * on with the inquiry, which is what this path does
			 * for any other packet (rx_task.h). */
			if (rx_task_schedule(2,
				inquiry_rx_cb, NULL,	// efhs rx callback
				1<<RX_F_PAYLOAD		// wait for payload
			) < 0)
				inquiry_schedule(3&(TX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX()));
		}
		else
		{
			/* Schedule normal inquiry */
			inquiry_schedule(3&(TX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX()));
		}
	}
	return 0;
}

static void inquiry_schedule(unsigned delay)
{
	if (clkn_elapsed(btphy.central_clkn, inquiry_state.clkn_start, INQ_MAX_TICKS))
	{
		cprintf("inquiry timeout\n");
		btctl_set_state(BTCTL_STATE_STANDBY, BTCTL_REASON_TIMEOUT);
		return;
	}

	/* Schedule TX */
	tx_task_schedule(delay,
		NULL, NULL,
		NULL, NULL); // No packet / no payload

	/* Schedule rx fhs. This window is what carries the inquiry on:
	 * inquiry_rx_cb() is where the next sweep is scheduled from. With no
	 * window there is nothing left to run, so the inquiry is over
	 * (rx_task.h). */
	if (rx_task_schedule(delay+2,
		inquiry_rx_cb, NULL,	// fhs rx callback
		1<<RX_F_PAYLOAD		// wait for payload
		) < 0)
		btctl_set_state(BTCTL_STATE_STANDBY, BTCTL_REASON_TIMEOUT);
}

/* dummy tick handler to start TX'ing with a proper clock */
static int inquiry_start_sync(uint8_t p1, uint8_t p2, uint16_t p3)
{
	unsigned delay;

	/* Use the Central clkn */
	btphy_cancel_clkn_delay();
	delay = 4+(3&(TX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX()));
	inquiry_state.clkn_start = btphy.central_clkn+delay;
	inquiry_schedule(delay);
	return 0;
}

/*
 * Inquiry Central
 */
void inquiry_state_setup(void)
{
	/* Init only with the lap, DCI(0) is used as UAP */
	btphy_set_mode(BT_MODE_INQUIRY, GIAC, 0);

	btctl_set_state(BTCTL_STATE_INQUIRY, BTCTL_REASON_SUCCESS);

	/* inquiry_start_sync() is what reads the clock and starts the sweep,
	 * so an inquiry the scheduler had no room for does not happen at all.
	 * Say so and go back to standby, on the reason the inquiry's own
	 * deadline uses. The inquiry is reported first because the host is
	 * only told about a state it was told it had left -- a standby that
	 * follows a standby sends nothing, and the host's command would wait
	 * forever. */
	if (tdma_schedule(2, inquiry_start_sync, 0,0,0,-3) < 0)
	{
		cprintf("inquiry: cannot schedule start\n");
		btctl_set_state(BTCTL_STATE_STANDBY, BTCTL_REASON_TIMEOUT);
	}
}
