/* Page state
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
#include <ubtbr/debug.h>
#include <ubtbr/btphy.h>
#include <ubtbr/tdma_sched.h>
#include <ubtbr/bb.h>
#include <ubtbr/rx_task.h>
#include <ubtbr/tx_task.h>
#include <ubtbr/central_state.h>
#include <ubtbr/paging_state.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/hop.h>

/* Max duration : 45 seconds */
#define PAGING_MAX_TICKS (CLKN_RATE*45)
#define TX_PREPARE_IDX	3  // We will transmit at clkn1_0 = 0

#define PAGING_1ST_CALLBACK ((void *)0xCAFE)
#define PAGING_2ND_CALLBACK ((void *)0xF00D)

extern btphy_t btphy;

static struct {
	uint32_t clkn_start;
	uint32_t clkn_got_id2;
	/* The paged device's address, which the page train's access code and
	 * hop sequence are built from. Kept so the train can be picked up
	 * again after a connection that retuned both to this board's own
	 * address; see paging_state_resume(). */
	uint32_t paged_lap;
	uint8_t paged_uap;
	bbhdr_t fhs_hdr;
	uint8_t fhs_data[20];
} paging_state;

static int paging_canceled(void)
{
	return btctl_get_state() != BTCTL_STATE_PAGE;
}

/* Would a page response sent in tick `clkn` fall outside pagerespTO? The tick
 * of the transmit is the one that has to be inside it, not the tick this is
 * asked in: the answer here is wanted a slot ahead, when the transmit is
 * scheduled. */
static int page_response_timeout_exceeded(uint32_t clkn)
{
	return clkn_elapsed(clkn, paging_state.clkn_got_id2,
				PAGE_RESP_TO_TICKS);
}

static void paging_schedule(unsigned delay);

/* Timeframe:
 * usec                 0                                625                      1250              1875
 * clk          -1      0                1                2            3             4         5    6
 * Central:     prep tx | tx id/prep tx  |  tx id/prep rx | rx/prep rx | rx/prep tx* | tx fhs  | .. |rx
 * Peripheral:          |                |             .. | tx id(2)   |             |         | .. |tx id(3)
 */

/* We received ID(3) from paged device */
static int paging_rx_ack_cb(msg_t *msg, void *arg, int time_offset)
{
	btctl_hdr_t *h = (btctl_hdr_t*)msg->data;
	btctl_rx_pkt_t *pkt;

	if (paging_canceled())
	{
		msg_free(msg);
		return 0;
	}
	pkt = (btctl_rx_pkt_t *)h->data;

	if (BBPKT_HAS_PKT(pkt))
	{
		cprintf("ID(3)\n");
		central_state_init();
	}
	else{
		uint8_t delay = 3&(TX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX());
		/* If the setup fails before the CONNECTION state has been reached, the following procedure shall be carried out.
		   The Peripheral shall listen as long as no FHS packet is received until pagerespTO is exceeded.*/
		if(page_response_timeout_exceeded(btphy.central_clkn+delay+1))
		{
			cprintf("no ID(3)\n");
			paging_schedule(delay);
		}else{
			/* Retry sending FHS; no need to tune to the same x-1 channel since `Peripheral Page Response` Hopping sequence
			   advance `N` only when Central's clock1 reaches 0. */
			bbpkt_fhs_finalize_payload(paging_state.fhs_data, (btphy.central_clkn+delay+1)>>2);
			tx_task_schedule(delay, NULL, NULL, &paging_state.fhs_hdr, paging_state.fhs_data);
			rx_task_schedule(delay+2, paging_rx_ack_cb, NULL, 0);
		}
	}
	msg_free(msg);
	return 0;
}

/* We received the ID(2) from paged device */
static int paging_rx_cb(msg_t *msg, void *arg, int time_offset)
{
	btctl_hdr_t *h = (btctl_hdr_t*)msg->data;
	btctl_rx_pkt_t *pkt;
	unsigned delay; 

	if (paging_canceled())
		goto end;
	pkt = (btctl_rx_pkt_t *)h->data;

	if (BBPKT_HAS_PKT(pkt))
	{
		/* We received the Peripheral's ID(2) at t'(k).
		 * Freeze the `calc_iterate_peripheral_page_scan_phase(clk)` value
		 * as the X-phase we will carry.
		 * Send FHS in next slot and start basic hopping
		 * The FHS shall be sent on chan f(k+1), following the std paging hop
		 * */
		delay = 3&(TX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX());
		paging_state.clkn_got_id2 = btphy.central_clkn;

		btphy_set_mode_no_ap(BT_MODE_PAGING_RESPONSE);
		hop_freeze_clock(btphy.central_clkn);
		
		/* Write clk27_2 in fhs payload
		 * Will tx at (clkn+1) */
		bbpkt_fhs_finalize_payload(paging_state.fhs_data, (btphy.central_clkn+delay+1)>>2);

		/* Schedule tx Central's page response (FHS): */
		tx_task_schedule(delay, 	// must schedule tx_prepare now to start tx in next clkn
			NULL, NULL,
			/* skip 4 bytes of sw */
			&paging_state.fhs_hdr, paging_state.fhs_data);

		/* Schedule rx Peripheral's ID(3) page response: */
		rx_task_schedule(delay+2,
			paging_rx_ack_cb, NULL,	// ID rx callback
			0			// no payload
			);
	}
	else
	{
		if (arg == PAGING_1ST_CALLBACK){
			/* Schedule rx ID(2): this listens for ID(2) in the second clock of the Central slot */
			rx_task_schedule(0,
				paging_rx_cb, PAGING_2ND_CALLBACK,	// ID rx callback
				0			// no payload for an ID packet
				);
		}else{
			/* schedule tx in next slot */
			paging_schedule(3&(TX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX()));
		}
	}
end:
	msg_free(msg);
	return 0;
}

static void paging_schedule(unsigned delay)
{
	if (clkn_elapsed(btphy.central_clkn, paging_state.clkn_start, PAGING_MAX_TICKS))
	{
		cprintf("paging timeout\n");
		btctl_set_state(BTCTL_STATE_STANDBY, BTCTL_REASON_TIMEOUT);
		return;
	}

	/* In case we are coming here from paging_rx_ack_cb where page_response_timeout_exceeded
	   is true, make sure we revert to mode BT_MODE_PAGING instead of BT_MODE_PAGING_RESPONSE */
	btphy_set_mode_no_ap(BT_MODE_PAGING);

	/* Schedule TX ID(1) in the 1st half of the Central's TX slot */
	tx_task_schedule(delay,
		NULL, NULL,	// no tx callback
		NULL, NULL);	// no header / no payload
	
	/* Schedule TX ID(1) in the 2nd half of the Central's TX slot */
	tx_task_schedule(delay+1,
		NULL, NULL,	// no tx callback
		NULL, NULL);	// no header / no payload 

	/* Schedule RX ID(2) in the `1st half of the Central's TX slot` + 625uS. 
	   If ID(2) fails to arrive, paging_rx_cb will schedule another consequtive
	   RX in the 2nd half of the same slot */
	rx_task_schedule(delay+2,
		paging_rx_cb, PAGING_1ST_CALLBACK,	// ID rx callback
		0			// no payload for an ID packet
	);
}

/* Pick the page train up again after a connection that carried no packet.
 *
 * The ID packet that answers a page message and the ID packet that
 * acknowledges the page response are the same thing, a bare access code, so
 * the pager cannot tell them apart: a board that is still page scanning
 * answers the page response's access code the way it answers a page message,
 * and that answer lands in the window this pager waits for the
 * acknowledgment in. The connection then has nobody on the other end, and
 * link supervision ends it a second later.
 *
 * The page the user asked for still has most of its 45 seconds to run, so the
 * train starts again instead of one stray answer ending the whole attempt.
 * The deadline is the original one, so however many stray answers arrive, the
 * command lasts no longer than a single page attempt. */
void paging_state_resume(void)
{
	/* The connection tuned the radio and the hop sequence to this board's
	 * own address; the page train needs the paged device's back. */
	btphy_set_mode(BT_MODE_PAGING, paging_state.paged_lap,
			paging_state.paged_uap);
	btctl_set_state(BTCTL_STATE_PAGE, BTCTL_REASON_TIMEOUT);
	paging_schedule(3&(TX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX()));
}

/* dummy tick handler to start TX'ing with a proper clock */
static int paging_start_sync(uint8_t p1, uint8_t p2, uint16_t p3)
{
	unsigned delay;

	/* Use the Central clkn */
	btphy_cancel_clkn_delay();
	delay = 4+(3&(TX_PREPARE_IDX-CUR_CENTRAL_SLOT_IDX()));
	paging_state.clkn_start = btphy.central_clkn+delay;
	paging_schedule(delay);
	return 0;
}

/*
 * Paging Central
 * Refs: timings at BT52|V2PB|Table 8.3
 * In a nutshell, we must:
 * k 0: Tx ID(1) packets with sw of paged device on f(k)
 * k 2: clk1(1): Receive ID(2) packet in response of the paged device 	at clk1(1) 
 * k 4: Tx FHS packet with sw of paged device
 *	if contains Central's (low_sw, lap, nap, uap, etc.), and clk27_2 sampled at start of tx.
 * k 6: Rx ID(3) from the Peripheral, and start Central task.
 */
void paging_state_setup(uint32_t lap, uint8_t uap)
{
	// Reset paging state
	btphy_set_mode(BT_MODE_PAGING, lap, uap);
	paging_state.paged_lap = lap;
	paging_state.paged_uap = uap;

	/* A simple Header for our fhs */ 
	paging_state.fhs_hdr.lt_addr = 0;		//  This seems right
	paging_state.fhs_hdr.type = BB_TYPE_FHS;
	paging_state.fhs_hdr.flags = 0; 		// Depends on devices, some sends BTHDR_FLOW|BTHDR_SEQN, others send 0, both seems right

	/* Prepare constant part of FHS payload */
	bbpkt_fhs_prepare_payload(paging_state.fhs_data,
		btphy.my_sw&0xffffffff, 	// parity: low32 bits of Central's sw
		btphy.my_lap,		// Central's bdaddr
		btphy.my_uap,		// 
		btphy.my_nap,		// 
		0x5A020C,		// class: FIXME make this configurable
		1,			// lt_addr: FIXME: fixed ltaddr = 1
		0 // No eir
		);

	tdma_schedule(2, paging_start_sync, 0,0,0,-3);
	btctl_set_state(BTCTL_STATE_PAGE, BTCTL_REASON_SUCCESS);
}
