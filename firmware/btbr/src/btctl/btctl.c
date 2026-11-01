/* Btctl interface
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
#include <string.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/btphy.h>
#include <ubtbr/codec.h>
#include <ubtbr/system.h>
#include <ubtbr/rf.h>
#include <ubtbr/hop.h>
#include <ubtbr/inquiry_state.h>
#include <ubtbr/paging_state.h>
#include <ubtbr/central_state.h>
#include <ubtbr/inquiry_scan_state.h>
#include <ubtbr/page_scan_state.h>
#include <ubtbr/monitor_state.h>
#include <watchdog.h>

btctl_t btctl;

static void btctl_flush_queue(msg_queue_t *q)
{
	msg_t *msg;

	while ((msg = safe_dequeue(q)) != NULL)
	{
		msg_free(msg);
	}
}

static void btctl_flush_acl_tx_queue(void)
{
	/* Both carry packets for a connection, so a new page or page scan
	 * leaves neither behind to go out on the next one. */
	btctl_flush_queue(&btctl.acl_tx_q);
	btctl_flush_queue(&btctl.pkt_tx_q);
}

static void btctl_handle_debug_req(msg_t *msg)
{
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;

	cprintf("debug req: %s\n", (char*)hdr->data);
}

/* The board stays on the bus this long before it goes away -- a reset, or the
 * hand-over to the on-chip programmer: ten times the 1 ms USB frame the host's
 * next token can be away. */
#define RESET_DELAY_CLKN (CLKN_RATE/100)

static void btctl_handle_reset_req(msg_t *msg)
{
	uint32_t start;

	/*
	 * Let the host collect its answer before going away. reset() turns the
	 * USB pins off as its first act, and this runs from the loop a few
	 * microseconds after the request handler that asked for it returned --
	 * while the control transfer's status stage is still sitting in the
	 * endpoint 0 buffer, waiting for the host's next IN token, which can be
	 * a whole 1 ms frame away. Going off the bus throws that packet away,
	 * and the host sees a stalled transfer: ubertooth-dfu reads that as a
	 * board refusing to enter DFU and gives up, even though the board does
	 * land in the bootloader, which carries no serial number.
	 *
	 * The delay is spent the way the idle request spends its own: standby
	 * first, then usb_work() until the time is up. A receiving state would
	 * otherwise keep filling the queue to the host while nothing emptied it,
	 * and a state that cannot enqueue a packet calls die(), which never
	 * returns -- so a board asked to reset mid-capture would stop here
	 * instead of resetting. The states read this field to see that they have
	 * been cancelled; usb_work() is what empties the queue.
	 *
	 * The field is set here rather than through btctl_set_state(), which
	 * prints a line to the host's console and enqueues a state response, and
	 * either can reach that same die() on a queue that is already full.
	 *
	 * CENTRAL_CLKN is a free-running hardware counter, so the subtraction
	 * stays right to within a tick of its wrap, and nothing but a stopped
	 * timer 1 -- which would have stopped the whole image long before this --
	 * can hold the board here.
	 */
	btctl.state = BTCTL_STATE_STANDBY;
	start = CENTRAL_CLKN;
	while ((uint32_t)(CENTRAL_CLKN - start) < RESET_DELAY_CLKN)
		usb_work();

	/* The watchdog time-out that reset() ends in is one this firmware asked
	 * for, not the board having stopped answering. Said here, at the reset
	 * itself, rather than where the request arrives: the request is queued,
	 * and a queue that is full drops it, which would otherwise leave the
	 * board running with a later real wedge excused. */
	watchdog_intent(WATCHDOG_INTENT_RESET);
	reset();
}

static void btctl_handle_isp_req(msg_t *msg)
{
	uint32_t start;

	/*
	 * The on-chip programmer takes the board off the bus exactly as reset()
	 * does, so the host is given the same time to collect its answer, spent
	 * the same way: standby first, then usb_work() until the time is up.
	 * btctl_handle_reset_req() above says why each part of that is needed.
	 */
	btctl.state = BTCTL_STATE_STANDBY;
	start = CENTRAL_CLKN;
	while ((uint32_t)(CENTRAL_CLKN - start) < RESET_DELAY_CLKN)
		usb_work();

	/* The programmer keeps the chip running without a reset, so the
	 * watchdog stays armed and nothing in there feeds it. It cannot be
	 * switched off before a reset, so the timeout is stretched to the
	 * longest the counter holds instead. */
	watchdog_intent(WATCHDOG_INTENT_ISP);
	watchdog_stretch();

	/* Nothing stops the radio on the way into the programmer, and the
	 * programmer does not touch it: a board asked for it while a state was
	 * transmitting would be left transmitting until its power is cut.
	 * Standby above cancels the tasks; this turns the radio and the
	 * amplifier off. The other image's dispatch does the same through
	 * cc2400_idle(). */
	btphy_rf_off();

	/* set_isp() masks interrupts across the hand-over itself; see the
	 * comment there for why that is needed and why it lives in common. */
	set_isp();
	/* set_isp() is not supposed to come back -- it hands the chip to code
	 * in ROM. If it ever does, the board must not be left running for the
	 * next hour with a watchdog that cannot reset it and a wedge that would
	 * not be counted. */
	watchdog_unstretch();
}

static void btctl_handle_idle_req(msg_t *msg)
{
	uint32_t wait_clkn;

	/* Cancel everything we can */
	btctl_set_state(BTCTL_STATE_STANDBY, BTCTL_REASON_SUCCESS);

	/* Let some time to tasks to complete (FIXME: wrap)*/
	wait_clkn = CENTRAL_CLKN+30;

	while (CENTRAL_CLKN < wait_clkn)
		usb_work();
}

static void btctl_handle_set_freq_off_req(msg_t *msg)
{
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;
	btctl_set_reg_req_t *req = (btctl_set_reg_req_t*)hdr->data;

	btphy_rf_set_freq_off(req->reg&0x3f);
}

static void btctl_handle_set_max_ac_errors_req(msg_t *msg)
{
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;
	btctl_set_reg_req_t *req = (btctl_set_reg_req_t*)hdr->data;

	btphy_rf_set_max_ac_errors(req->reg&3);
}

static void btctl_handle_set_bdaddr_req(msg_t *msg)
{
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;
	btctl_set_bdaddr_req_t *req = (btctl_set_bdaddr_req_t*)hdr->data;

	btphy_set_bdaddr(req->bdaddr);
}

static void btctl_handle_inquiry_req(msg_t *msg)
{
	inquiry_state_setup();
}

static void btctl_handle_paging_req(msg_t *msg)
{
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;
	btctl_paging_req_t *req = (btctl_paging_req_t*)hdr->data;
	uint32_t lap = req->bdaddr & 0xffffff;
	uint8_t uap = (req->bdaddr >> 24) & 0xff;

	btctl_flush_acl_tx_queue();

	/* Start paging */
	paging_state_setup(lap, uap);
}

static void btctl_handle_monitor_req(msg_t *msg)
{
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;
	btctl_paging_req_t *req = (btctl_paging_req_t*)hdr->data;

	/* Start paging monitor */
	monitor1_state_setup(req->bdaddr);
}

static void btctl_handle_tx_acl_req(msg_t *msg)
{
	btctl_acl_tx_enqueue(msg);
}

/* One packet of a chosen format, to go on the air as the host composed it.
 *
 * Returns whether the request was taken. A request this firmware cannot carry
 * out is refused with a line saying why, rather than transmitted as something
 * else: the host chose this format and this length on purpose, so a packet of
 * another shape would be worse than none.
 */
static int btctl_handle_tx_pkt_req(msg_t *msg)
{
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;
	btctl_tx_pkt_req_t *req = (btctl_tx_pkt_req_t*)hdr->data;
	const bbcodec_types_t *t;
	unsigned min_data, max_data, data_size;
	uint8_t air_type;

	if (btctl.state != BTCTL_STATE_CONNECTED)
	{
		cprintf("Cannot transmit a packet outside a connection\n");
		return 0;
	}
	t = bbcodec_tx_format(req->format, &air_type);
	if (t == NULL)
	{
		cprintf("Unknown tx format %d\n", req->format);
		return 0;
	}
	if (req->repeat == 0)
	{
		cprintf("Tx packet asked for 0 slots\n");
		return 0;
	}
	/* What the host supplied, less the parts it does not choose the size
	 * of: the voice field and the payload header. */
	if (req->data_size < t->voice_bytes + t->payload_header_bytes)
	{
		cprintf("Tx packet of %d bytes is too short for format %d\n",
			req->data_size, req->format);
		return 0;
	}
	data_size = req->data_size - t->voice_bytes - t->payload_header_bytes;
	bbcodec_tx_format_limits(t, &min_data, &max_data);
	if (data_size < min_data || data_size > max_data)
	{
		cprintf("Format %d carries %d to %d data bytes, not %d\n",
			req->format, min_data, max_data, data_size);
		return 0;
	}
	if (btctl_pkt_tx_enqueue(msg) != 0)
	{
		cprintf("Tx packet queue full\n");
		return 0;
	}

	return 1;
}

static void btctl_handle_inquiry_scan_req(msg_t *msg)
{
	inquiry_scan_state_setup();
}

static void btctl_handle_page_scan_req(msg_t *msg)
{
	btctl_flush_acl_tx_queue();
	page_scan_state_setup();
}

static void btctl_handle_set_afh_req(msg_t *msg)
{
	static uint8_t afh_buf[11];
	uint32_t flags = irq_save_disable();
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;
	btctl_set_afh_req_t *req = (btctl_set_afh_req_t*)hdr->data;

	afh_buf[0] = req->mode;
	memcpy(afh_buf+1, req->map, 10);
	//hop_set_afh_map(req->map);
	btphy_timer_add(req->instant, (btphy_timer_fn_t)hop_cfg_afh, afh_buf, 0);
	irq_restore(flags);
}

static void btctl_handle_set_eir_req(msg_t *msg)
{
	btctl_hdr_t *h = (btctl_hdr_t*)msg->data;

	if (btctl.state != BTCTL_STATE_STANDBY)
	{
		cprintf("Cannot change eir_msg now\n");
		msg_free(msg);
		return;
	}
	if (btctl.eir_msg)
	{
		msg_free(btctl.eir_msg);
	}
	cprintf("EIR updated\n");
	btctl.eir_msg = msg;
	btctl.eir_pkt = (btctl_tx_pkt_t *)h->data;
}

static void btctl_handle_msg(msg_t *msg)
{
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;

	switch(hdr->type)
	{
	case BTCTL_DEBUG:
		btctl_handle_debug_req(msg);
		break;
	case BTCTL_RESET_REQ:
		btctl_handle_reset_req(msg);
		break;
	case BTCTL_ISP_REQ:
		btctl_handle_isp_req(msg);
		break;
	case BTCTL_IDLE_REQ:
		btctl_handle_idle_req(msg);
		break;
	case BTCTL_SET_FREQ_OFF_REQ:
		btctl_handle_set_freq_off_req(msg);
		break;
	case BTCTL_SET_MAX_AC_ERRORS_REQ:
		btctl_handle_set_max_ac_errors_req(msg);
		break;
	case BTCTL_SET_BDADDR_REQ:
		btctl_handle_set_bdaddr_req(msg);
		break;
	case BTCTL_INQUIRY_REQ:
		btctl_handle_inquiry_req(msg);
		break;
	case BTCTL_PAGING_REQ:
		btctl_handle_paging_req(msg);
		break;
	case BTCTL_TX_ACL_REQ:
		btctl_handle_tx_acl_req(msg);
		// don't free tx messages
		goto end_nofree;
	case BTCTL_TX_PKT_REQ:
		/* Queued, so the message belongs to the queue; a refused
		 * request is freed below like any other. */
		if (btctl_handle_tx_pkt_req(msg))
			goto end_nofree;
		break;
	case BTCTL_INQUIRY_SCAN_REQ:
		btctl_handle_inquiry_scan_req(msg);
		break;
	case BTCTL_PAGE_SCAN_REQ:
		btctl_handle_page_scan_req(msg);
		break;
	case BTCTL_MONITOR_REQ:
		btctl_handle_monitor_req(msg);
		break;
	case BTCTL_SET_EIR_REQ:
		btctl_handle_set_eir_req(msg);
		// don't free the eir message
		goto end_nofree;
	case BTCTL_SET_AFH_REQ:
		btctl_handle_set_afh_req(msg);
		break;
	default:
		cprintf("Invalid message type %d\n", hdr->type);
		break;
	}
	btctl_mem_free(msg);
end_nofree:
	return;
}

int btctl_work(void)
{
	msg_t *msg = btctl_rx_dequeue();

	if (msg == NULL)
		return 0;

	btctl_handle_msg(msg);

	return 1;
}

void btctl_send_state_resp(btctl_state_t state, btctl_reason_t reason)
{
	msg_t *msg = btctl_msg_alloc(BTCTL_STATE_RESP);
	btctl_state_resp_t *resp;

	resp = (btctl_state_resp_t*)msg_put(msg, sizeof(*resp));
	resp->state = state;
	resp->reason = reason;
	btctl_tx_enqueue(msg);
}

static inline int btctl_state_transition_valid(btctl_state_t cur,
			btctl_state_t next, btctl_reason_t reason)
{
	/* All states can go standby */
	if (next == BTCTL_STATE_STANDBY)
		return 1;
	/* Connected state must come from page/page scan */
	if (next == BTCTL_STATE_CONNECTED)
		return cur == BTCTL_STATE_PAGE
			|| cur == BTCTL_STATE_PAGE_SCAN
			|| cur == BTCTL_STATE_CONNECTED;
	/* A connection that carried no packet goes back to the page or the page
	 * scan it came from, which still has its own deadline left to run
	 * (paging_state_resume(), page_scan_state_resume()). Only those two:
	 * they report the reason the connection ended, where a page or a scan
	 * the host has just asked for reports success. So a request to start
	 * one while a connection is up is still refused here, which is what
	 * keeps it from arming a second receive window on top of the
	 * connection's. */
	if (next == BTCTL_STATE_PAGE || next == BTCTL_STATE_PAGE_SCAN)
		return cur == BTCTL_STATE_STANDBY
			|| (cur == BTCTL_STATE_CONNECTED
				&& reason != BTCTL_REASON_SUCCESS);
	/* Others states must come from standby */
	return cur == BTCTL_STATE_STANDBY;
}

static const char* btctl_state_name(btctl_state_t state)
{
	const char *name;
	static const char* btctl_state_name_tbl[BTCTL_STATE_COUNT] = {
		[BTCTL_STATE_STANDBY]	= "STANDBY", 
		[BTCTL_STATE_INQUIRY]	= "INQUIRY",
		[BTCTL_STATE_PAGE]	= "PAGE",
		[BTCTL_STATE_CONNECTED]	= "CONNECTED",
		[BTCTL_STATE_INQUIRY_SCAN] = "INQUIRY_SCAN",
		[BTCTL_STATE_PAGE_SCAN] = "PAGE_SCAN"
	};
	if (state >= BTCTL_STATE_COUNT)
		return "Invalid";

	name = btctl_state_name_tbl[state];
	if (!name)
		name = "TODO";
	return name;
}

void btctl_set_state(btctl_state_t state, btctl_reason_t reason)
{
	uint32_t flags = irq_save_disable();
	btctl_state_t old_state = btctl.state;

#if 1
	cprintf("State %s -> %s\n",
		btctl_state_name(btctl.state),
		btctl_state_name(state));
#endif

	if (!btctl_state_transition_valid(old_state, state, reason))
	{
		irq_restore(flags);
		DIE("Invalid state transition %s -> %s\n",
			btctl_state_name(old_state),
			btctl_state_name(state));
	}
	btctl.state = state;
	irq_restore(flags);
	if (!(old_state == BTCTL_STATE_STANDBY && state == BTCTL_STATE_STANDBY))
		btctl_send_state_resp(state, reason);
}

void btctl_init(void)
{
	btctl.state = BTCTL_STATE_STANDBY;
	btctl.eir_msg = NULL;
	btctl.eir_pkt = NULL;
	msg_queue_init(&btctl.rx_q);
	msg_queue_init(&btctl.tx_q);
	msg_queue_init(&btctl.acl_tx_q);
	msg_queue_init(&btctl.pkt_tx_q);
}
