/* Page scan state
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
#include <stdlib.h>
#include <ubtbr/cfg.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/btphy.h>
#include <ubtbr/bb.h>
#include <ubtbr/tdma_sched.h>
#include <ubtbr/scan_task.h>
#include <ubtbr/rx_task.h>
#include <ubtbr/tx_task.h>
#include <ubtbr/hop.h>
#include <ubtbr/slave_state.h>

#define PAGE_SCAN_MAX_TICKS (CLKN_RATE*60)
#define TX_PREPARE_IDX	1  // We will transmit at clkn1_0 = 2
#define RX_PREPARE_IDX	3  // We will receive at clkn1_0 = 0

struct {
	fhs_info_t fhs_info;
	uint32_t clkn_start;
	/* The native clock of this board's answer to the page, which pagerespTO
	 * runs from. */
	uint32_t clkn_answer;
	/* Which of the central's two page messages the next window reads the
	 * answered page as: 0 the first, 1 the second. See
	 * page_scan_fhs_missed(). */
	uint8_t second_reading;
} page_scan_state;

static void page_scan_schedule(unsigned delay);
static int page_scan_rx_fhs_cb(msg_t *msg, void *arg, int time_offset);

/* Open a window for the central's page response, the FHS packet, at the next
 * start of a central transmit slot: that slot, and the response phase, are
 * what its frequency is computed from. */
static void page_scan_fhs_schedule(void)
{
	unsigned delay = 3&(RX_PREPARE_IDX-CUR_SLAVE_SLOT_IDX());

	/* A window is tuned in the tick before it listens, so a slot that
	 * starts in the very next tick cannot be tuned for: take the central
	 * transmit slot after it, 1.25 ms later. This is the case when the
	 * clock has just moved back from the second reading of the page, which
	 * puts the next slot of the first reading one tick away. */
	if (delay == 0)
		delay = 4;
	rx_task_schedule(delay,
		page_scan_rx_fhs_cb, NULL,
		1<<RX_F_PAYLOAD			// FHS packet has payload
		);
}

/* The central's page response did not arrive in the window this board opened.
 *
 * A central sends two page messages in a transmit slot, and this board answers
 * 625 usec after the one it heard, but the page response comes an exact
 * 1250 usec after the FIRST of the two whichever one was answered (Vol 2,
 * Part B, Section 2.4.4). So it follows this board's own answer by 625 usec for
 * a page heard first and by 312.5 usec for a page heard second, and a page
 * message says nothing about which of the two it is. The scan set this clock to
 * the message it heard, so the second reading of that page is the same thing as
 * "this clock is one tick behind the central's".
 *
 * An empty window is therefore also what a page heard second looks like: the
 * response went by a tick earlier, while the radio was still turning round from
 * this board's answer. Taking that reading is the same thing as moving this
 * clock the one tick it would be behind by, which is what every frequency and
 * transmit after it is computed from.
 *
 * 8.3.3.1 asks for the receiver to be on from 312.5 usec after the answer, so
 * that one window covers both arrival times on the one frequency of that
 * 1.25 ms. This radio cannot: a receive is tuned in the tick before it listens,
 * and that tick is the one the answer goes out in. So only one of the two
 * readings can be listened on at a time, and which one is right cannot be told
 * from the page message.
 *
 * 8.3.3.1 also has the paged device keep listening until pagerespTO, and 8.3.3.2
 * has the central repeat its page response every 1.25 ms over that time, with
 * the clock updated. So an empty window is followed by a window for the next
 * repeat, taking the two readings in turn until pagerespTO passes, after which
 * the central has stopped repeating too. Taking them in turn rather than
 * settling on one matters because which reading is right holds for 1.28 seconds
 * at a time: a scanning board's frequency comes from CLKN16-12 of its own clock
 * and the pager's sweep from CLK16-12 of its own, so for that long either every
 * page this board answers is the central's first message or every one is its
 * second. */
static void page_scan_fhs_missed(void)
{
	if (clkn_elapsed(MASTER_CLKN, page_scan_state.clkn_answer,
				PAGE_RESP_TO_TICKS))
	{
		page_scan_schedule(0);
		return;
	}
	/* The other reading of the page, as the one tick the two differ by. */
	if (page_scan_state.second_reading)
		btphy.slave_clkn--;
	else
		btphy.slave_clkn++;
	page_scan_state.second_reading ^= 1;
	page_scan_fhs_schedule();
}

static int page_scan_canceled(void)
{
	return btctl_get_state() != BTCTL_STATE_PAGE_SCAN;
}

static void page_scan_tx_cb(void *arg)
{
	cprintf("fhs: ba=%llx clk27_2=%x lta=%d\n",
		page_scan_state.fhs_info.bdaddr,
		page_scan_state.fhs_info.clk27_2,
		page_scan_state.fhs_info.lt_addr);

	slave_state_init(page_scan_state.fhs_info.bdaddr,
			page_scan_state.fhs_info.lt_addr);
}

/* RX FHS cb */
static int page_scan_rx_fhs_cb(msg_t *msg, void *arg, int time_offset)
{
	btctl_hdr_t *h = (btctl_hdr_t*)msg->data;
	btctl_rx_pkt_t *pkt;

	if (page_scan_canceled())
	{
		cprintf("page scan canceled\n");
		goto end;
	}
	pkt = (btctl_rx_pkt_t *)h->data;
	if (BBPKT_GOOD_CRC(pkt))
	{
		if (pkt->bb_hdr.type != BB_TYPE_FHS)
		{
			/* Not the packet this leg waits for, so there is no
			 * page to answer. Go back to scanning rather than
			 * leave the board in page scan with nothing left to
			 * run, which would wait forever. A stray multi-slot
			 * packet also reaches here several ticks late, which
			 * is too late to re-read the page. */
			cprintf("(bad type %d)", pkt->bb_hdr.type);
			page_scan_schedule(0);
			goto end;
		}
		// Parse fhs to get bdaddr/clkn/ltaddr
		bbpkt_decode_fhs(pkt->bt_data, &page_scan_state.fhs_info);


		// Set slave clkn
		btphy.slave_clkn = (page_scan_state.fhs_info.clk27_2<<2)
				 + (btphy.slave_clkn-pkt->clkn);
		
		/* Schedule TX ID(3)*/
		tx_task_schedule(3&(TX_PREPARE_IDX-CUR_SLAVE_SLOT_IDX()),
			page_scan_tx_cb, NULL,
			NULL, NULL); 	// no header / no payload
		
	}
	else{
		page_scan_fhs_missed();
	}
end:
	msg_free(msg);
	return 0;
}

/* RX ID(1) cb */
static void page_scan_rx_id_cb(int sw_detected, void *arg)
{
	unsigned delay;
	if(page_scan_canceled())
	{
		cprintf("page scan canceled\n");
		return;
	}
	if (sw_detected)
	{
		/* we should be at slave_clk = 0, prepare in one slot*/
		console_putc('!');

		/* The two legs below answer the page, which is a substate of
		 * its own: it starts from the phase the scan window heard the
		 * page on, advances it once per central transmit slot, and
		 * takes its transmit or receive frequency from the slot.
		 *
		 * The slot comes from the clock the scan just set to the page
		 * it heard, so the two ends agree on it: this board answers in
		 * the slot that pairs with the one the page arrived in. The
		 * board's own untouched clock shares no slot boundary with the
		 * pager. */
		btphy_set_mode_no_ap(BT_MODE_PERIPH_PAGE_RESPONSE);
		hop_start_response();

		/* Schedule TX ID(2) */
		delay = 3&(TX_PREPARE_IDX-CUR_SLAVE_SLOT_IDX());
		tx_task_schedule(delay,
			NULL, NULL,	// no tx callback
			NULL, NULL); 	// no header / no payload

		/* The answer goes out in the tick after the one that tunes the
		 * transmit, and pagerespTO runs from it. */
		page_scan_state.clkn_answer = MASTER_CLKN+delay+1;

		/* Schedule rx FHS, reading the page just heard as the first of
		 * the central's two (see page_scan_fhs_missed()) */
		page_scan_state.second_reading = 0;
		page_scan_fhs_schedule();
	}
	else
	{
		page_scan_schedule(0);
	}
}

static void page_scan_schedule(unsigned delay)
{
	if (clkn_elapsed(MASTER_CLKN, page_scan_state.clkn_start, PAGE_SCAN_MAX_TICKS))
	{
		cprintf("page scan timeout\n");
		btctl_set_state(BTCTL_STATE_STANDBY, BTCTL_REASON_TIMEOUT);
		return;
	}
	/* Back to the scan substate, which every path here comes from: an
	 * answer that led nowhere leaves the response substate behind. */
	btphy_set_mode_no_ap(BT_MODE_PAGE_SCAN);
	scan_task_schedule(delay, page_scan_rx_id_cb, NULL);
}

/*
 * Page slave
 */
void page_scan_state_setup(void)
{
	btphy_set_mode(BT_MODE_PAGE_SCAN, btphy.my_lap, btphy.my_uap);
	btctl_set_state(BTCTL_STATE_PAGE_SCAN, BTCTL_REASON_SUCCESS);
	page_scan_state.clkn_start = MASTER_CLKN+1;
	page_scan_schedule(1);
}
