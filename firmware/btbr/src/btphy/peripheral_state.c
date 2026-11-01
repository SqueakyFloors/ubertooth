/* Peripheral connection state
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
#include <ubtbr/ll.h>
#include <ubtbr/page_scan_state.h>

#define TX_PREPARE_IDX	1  // We will transmit at clkn1_0 = 2
#define RX_PREPARE_IDX	3  //  We will receive at clkn1_0 = 0

static struct {
	/* Parameters */
	uint64_t central_bdaddr;
	uint8_t lt_addr;
	link_layer_t ll;
} peripheral_state;

static void peripheral_state_schedule_rx(unsigned skip_slots);
static void peripheral_state_schedule_tx(unsigned skip_slots);

static int peripheral_state_canceled(void)
{
	return btctl_get_state() != BTCTL_STATE_CONNECTED;
}

static int peripheral_rx_cb(msg_t *msg, void *arg, int time_offset)
{
	btctl_hdr_t *h = (btctl_hdr_t*)msg->data;
	btctl_rx_pkt_t *pkt;
	int rc;

	if (peripheral_state_canceled())
	{
		ll_reset(&peripheral_state.ll);
		msg_free(msg);
		return 0;
	}
	/* Check if a packet was received, to adjust clock if needed */
	pkt = (btctl_rx_pkt_t *)h->data;

	if (BBPKT_HAS_PKT(pkt))
	{
		/* Resync to Central */
		btphy_adj_clkn_delay(time_offset);
	}

	/* Do link layer */
	rc = ll_process_rx(&peripheral_state.ll, msg);
	if (rc < 0)
	{
		/* A connection that never carried a packet means the central
		 * did not hear this board's acknowledgment of the page
		 * response, so there is nothing on the link. The page scan it
		 * came from has its own deadline left to run, so go back to
		 * scanning rather than end it here. */
		int had_peer = peripheral_state.ll.is_connected;

		ll_reset(&peripheral_state.ll);
		if (had_peer)
			btctl_set_state(BTCTL_STATE_STANDBY, -rc);
		else
			page_scan_state_resume();
	}
	else if (rc == 1)
	{
		peripheral_state_schedule_tx(0);
	}
	else
	{
		peripheral_state_schedule_rx(0);
	}
	return 0;
}

static void tx_done_cb(void* arg)
{
	peripheral_state_schedule_rx(0);
}

static void peripheral_state_schedule_tx(unsigned skip_slots)
{
	/* Wait for next tx slot */
	unsigned delay = 4*skip_slots+(3&(TX_PREPARE_IDX-CUR_PERIPHERAL_SLOT_IDX()));
	ll_tx_t tx;

	ll_prepare_tx(&peripheral_state.ll, &tx);

	tx_task_schedule_fmt(delay,
		tx_done_cb, NULL,
		tx.hdr, tx.data, tx.fmt, tx.data_bytes);
}

static void peripheral_state_schedule_rx(unsigned skip_slots)
{
	/* Wait for next rx slot */
	unsigned delay = 4*skip_slots+(3&(RX_PREPARE_IDX-CUR_PERIPHERAL_SLOT_IDX()));

	/* Schedule rx: */
	rx_task_schedule(delay,
		peripheral_rx_cb, NULL,	// ID rx callback
		1<<RX_F_PAYLOAD		// wait for header
	);
}

void peripheral_state_init(uint64_t central_bdaddr, uint8_t lt_addr)
{
	peripheral_state.central_bdaddr = central_bdaddr;
	peripheral_state.lt_addr = lt_addr;

	/* Initialize basic hopping from Central's clock */
	btphy_set_mode(BT_MODE_PERIPHERAL, central_bdaddr&0xffffff, 0xff & (central_bdaddr>>24));

	ll_init(&peripheral_state.ll, 0, lt_addr);

	cprintf("Peripheral started\n");
	btctl_set_state(BTCTL_STATE_CONNECTED, BTCTL_REASON_PAGED);
	peripheral_state_schedule_rx(1);
}
