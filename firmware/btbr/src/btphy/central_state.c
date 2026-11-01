/* Central connection state
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
#include <ubtbr/paging_state.h>

#define TX_PREPARE_IDX	3  // We will transmit at clkn1_0 = 0
#define RX_PREPARE_IDX	1  //  We will receive at clkn1_0 = 2

static struct {
	link_layer_t ll;
} central_state;

static void central_state_schedule_tx(unsigned skip_slots);
static void central_state_schedule_rx(unsigned skip_slots);

static int central_canceled(void)
{
	return btctl_get_state() != BTCTL_STATE_CONNECTED;
}

static int central_rx_cb(msg_t *msg, void *arg, int time_offset)
{
	int rc;

	if (central_canceled())
	{
		msg_free(msg);
		ll_reset(&central_state.ll);
		return 0;
	}
	rc = ll_process_rx(&central_state.ll, msg);

	if (rc < 0)
	{
		/* A connection that never carried a packet usually had no peer
		 * on it: what this board took for the paged device's
		 * acknowledgment was another ID packet on the air. It can also
		 * be a real peer whose every reply was lost for the whole
		 * supervision second, and paging it again is the right answer
		 * to that too. Either way the page attempt this came from has
		 * its own deadline left to run, so go back to the page train
		 * rather than end the attempt here. */
		int had_peer = central_state.ll.is_connected;

		ll_reset(&central_state.ll);
		if (had_peer)
			btctl_set_state(BTCTL_STATE_STANDBY, -rc);
		else
			paging_state_resume();
	}
	else if (rc == 1)
	{
		central_state_schedule_tx(0);
	}
	else
	{
		central_state_schedule_rx(0);
	}
	return 0;
}

static void central_state_schedule_rx(unsigned skip_slots)
{
	/* Wait for next rx slot */
	unsigned delay = 4*skip_slots+(3&(RX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX()));

	/* Schedule rx: */
	rx_task_schedule(delay,
		central_rx_cb, NULL,	// ID rx callback
		1<<RX_F_PAYLOAD		// wait for header
	);
}

static void tx_done_cb(void* arg)
{
	central_state_schedule_rx(0);
}

static void central_state_schedule_tx(unsigned skip_slots)
{
	/* Wait for next tx slot */
	unsigned delay = skip_slots * 4 + (3&(TX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX()));
	bbhdr_t *tx_hdr;
	uint8_t *tx_data;

	ll_prepare_tx(&central_state.ll, &tx_hdr, &tx_data);

	tx_task_schedule(delay,
		tx_done_cb, NULL,
		tx_hdr, tx_data);
}

/* Called by paging task once the Peripheral acknowledged our FHS */
void central_state_init(void)
{
	/* Initialize basic hopping from Central's clock */
	btphy_set_mode(BT_MODE_CENTRAL, btphy.my_lap, btphy.my_uap);

	ll_init(&central_state.ll, 1, 1); // FIXME: fixed Peripheral lt_addr = 1

	/* We are already in sync, just schedule the first tx (in a while)*/
	cprintf("Central started\n");
	central_state_schedule_tx(1);
	btctl_set_state(BTCTL_STATE_CONNECTED, BTCTL_REASON_PAGED);
}
