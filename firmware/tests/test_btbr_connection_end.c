/*
 * Unit tests for what the BR/EDR image does when link supervision ends a
 * connection: firmware/btbr/src/btphy/central_state.c and peripheral_state.c,
 * over the link layer in ll.c.
 *
 * A connection that carried traffic and then went quiet is over, and the
 * command that made it ends with the board in standby. A connection that never
 * carried a single packet is a different thing: on the pager's side the ID
 * packet it took for the paged device's acknowledgment came from something
 * else on the air, and on the paged device's side the pager did not hear the
 * acknowledgment and is still paging. Either way the page or the page scan it
 * came from has its own deadline left to run, so the board goes back to it.
 *
 * These tests drive each connection state machine to its supervision timeout
 * with and without a packet having arrived, and check which of the two it
 * does. The two resume paths themselves are fakes here, and have their own
 * tests in test_btbr_page_response.c.
 *
 * What is faked: the slot tasks, the radio, the clock, the message pool, the
 * host queues, and the two resume entry points. Each slot task fake keeps the
 * callback the state machine armed, which is how a test runs the next step.
 * The link layer is the firmware's own -- ll.c is linked in -- so the rule
 * being tested, that a connection counts as established on the first packet
 * with a header, is read from the real code rather than restated here.
 *
 * This file is part of Project Ubertooth and is released under the terms of the
 * GPL. Refer to COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"

/* The two diagnostics every btbr source can reach, declared by
 * stubs-btbr/ubtbr/debug.h. */
void cprintf(char *fmt, ...);
void __attribute__((noreturn)) die(char *fmt, ...);

/* TIMER1's counter, which both state machines read as CENTRAL_CLKN. Here it is
 * a field the tests move on by whole ticks of 312.5 usec. */
#define T1TC btphy.central_clkn

#include <ubtbr/btphy.h>
#include <ubtbr/bb.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/central_state.h>
#include <ubtbr/ll.h>
#include <ubtbr/peripheral_state.h>
#include <ubtbr/rx_task.h>
#include <ubtbr/tx_task.h>

/* ---- the firmware state the two state machines share ---- */

btphy_t btphy;
btctl_t btctl;

/* ---- the fakes ---- */

void cprintf(char *fmt, ...) { (void)fmt; }

void __attribute__((noreturn)) die(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	abort();
}

void *mem_pool_alloc(unsigned size) { return malloc(size); }
void mem_pool_free(void *p) { free(p); }

void msg_queue_init(msg_queue_t *q) { q->head = q->tail = 0; }
msg_t *msg_dequeue(msg_queue_t *q) { (void)q; return NULL; }
int msg_enqueue(msg_queue_t *q, msg_t *p) { (void)q; msg_free(p); return 0; }

/* The state the board last reported, and with which reason. */
static btctl_state_t reported_state;
static btctl_reason_t reported_reason;
static unsigned report_count;

void btctl_set_state(btctl_state_t state, btctl_reason_t reason)
{
	btctl.state = state;
	reported_state = state;
	reported_reason = reason;
	report_count++;
}

void btphy_set_mode(btphy_mode_t mode, uint32_t lap, uint8_t uap)
{
	btphy.chan_lap = lap;
	btphy.chan_uap = uap;
	btphy.mode = mode;
}

void btphy_adj_clkn_delay(int delay) { (void)delay; }

/* The two resume entry points, which have their own tests. */
static unsigned paging_resumed;
static unsigned page_scan_resumed;

void paging_state_resume(void) { paging_resumed++; }
void page_scan_state_resume(void) { page_scan_resumed++; }

/* The slot tasks: each state machine arms one at a time, and a test runs it. */
static rx_task_cb_t armed_rx_cb;
static void *armed_rx_arg;
static tx_task_cb_t armed_tx_cb;
static void *armed_tx_arg;
static unsigned tx_count;

void rx_task_schedule(unsigned delay, rx_task_cb_t cb, void *cbarg,
			unsigned flags)
{
	(void)delay; (void)flags;
	armed_rx_cb = cb;
	armed_rx_arg = cbarg;
}

void tx_task_schedule(unsigned delay, tx_task_cb_t cb, void *cbarg,
			bbhdr_t *hdr, uint8_t *data)
{
	(void)delay; (void)hdr; (void)data;
	armed_tx_cb = cb;
	armed_tx_arg = cbarg;
	tx_count++;
}

void tx_task_reset(void) { }
void rx_task_reset(void) { }

/* ---- driving a connection ---- */

/* The address the connection is built on. Any value will do: these tests are
 * about which state the board ends in, not about frequencies. */
#define PEER_BDADDR	0x112233445566ull
#define LT_ADDR		1

/* A second of the slot clock, which is the link supervision timeout in ll.c.
 * Taken as the rate rather than restated, so a change to it there does not
 * silently make these tests stop reaching the timeout. */
#define SUPERVISION_TICKS	CLKN_RATE

static void reset_state(void)
{
	memset(&btphy, 0, sizeof(btphy));
	memset(&btctl, 0, sizeof(btctl));
	btphy.my_lap = 0x5a3c17;
	btphy.my_uap = 0x42;
	reported_state = BTCTL_STATE_STANDBY;
	reported_reason = BTCTL_REASON_SUCCESS;
	report_count = 0;
	paging_resumed = 0;
	page_scan_resumed = 0;
	armed_rx_cb = NULL;
	armed_rx_arg = NULL;
	armed_tx_cb = NULL;
	armed_tx_arg = NULL;
	tx_count = 0;
}

/* Run the transmit callback the connection armed, which is what schedules its
 * next receive window. */
static void run_tx_done(void)
{
	tx_task_cb_t cb = armed_tx_cb;

	if (cb == NULL)
		die("the connection armed no transmit\n");
	armed_tx_cb = NULL;
	cb(armed_tx_arg);
}

/* Hand the connection's receive window one packet, or nothing at all.
 *
 * `flags` are the decode flags the receive task fills in: none for a window
 * that heard nothing, and BBPKT_F_HAS_PKT with BBPKT_F_HAS_HDR for a packet
 * whose baseband header decoded, which is what ll.c counts as the connection
 * being established.
 */
static void run_window(uint8_t flags, uint8_t type)
{
	rx_task_cb_t cb = armed_rx_cb;
	msg_t *msg;
	btctl_rx_pkt_t *pkt;

	if (cb == NULL)
		die("the connection armed no receive window\n");
	armed_rx_cb = NULL;

	msg = btctl_msg_alloc(BTCTL_RX_PKT);
	pkt = (btctl_rx_pkt_t *)msg_put(msg, sizeof(*pkt));
	memset(pkt, 0, sizeof(*pkt));
	pkt->clkn = btphy.central_clkn;
	pkt->flags = flags;
	pkt->bb_hdr.lt_addr = LT_ADDR;
	pkt->bb_hdr.type = type;
	cb(msg, armed_rx_arg, 0);
}

/* One packet of the peer's, which puts the connection in the established
 * state ll.c reports to the host. */
static void run_packet_window(void)
{
	run_window((1<<BBPKT_F_HAS_PKT)|(1<<BBPKT_F_HAS_HDR), BB_TYPE_POLL);
}

/* A window that heard nothing, with the clock moved past the supervision
 * timeout first, so this is the window that ends the connection. */
static void run_silent_window_past_the_timeout(void)
{
	btphy.central_clkn += SUPERVISION_TICKS + 1;
	btphy.peripheral_clkn += SUPERVISION_TICKS + 1;
	run_window(0, BB_TYPE_NULL);
}

/* ---- the cases ---- */

/*
 * The pager's side. A connection that carried a packet and then went quiet is
 * over: the command ends with the board in standby, and the page train is not
 * picked up again.
 */
MU_TEST(a_central_connection_that_carried_a_packet_ends_in_standby)
{
	reset_state();
	central_state_init();
	run_tx_done();
	run_packet_window();
	mu_assert_int_eq(BTCTL_STATE_CONNECTED, btctl.state);

	run_tx_done();
	run_silent_window_past_the_timeout();

	mu_assert_int_eq(BTCTL_STATE_STANDBY, btctl.state);
	mu_assert_int_eq(BTCTL_REASON_TIMEOUT, reported_reason);
	mu_assert_int_eq(0, paging_resumed);
}

/*
 * The same timeout on a connection that never carried a packet. Nothing was
 * ever on the other end of it, so the page attempt it came from goes on
 * instead of the board going to standby.
 */
MU_TEST(a_central_connection_that_carried_nothing_goes_back_to_paging)
{
	reset_state();
	central_state_init();
	run_tx_done();
	run_silent_window_past_the_timeout();

	mu_assert_int_eq(1, paging_resumed);
	mu_assert_int_eq(BTCTL_STATE_CONNECTED, btctl.state);
	mu_assert(reported_state != BTCTL_STATE_STANDBY,
		"the pager went to standby instead of back to the page train");
}

/*
 * The paged device's side, and the same two cases: a connection that carried
 * traffic ends the page scan command.
 */
MU_TEST(a_peripheral_connection_that_carried_a_packet_ends_in_standby)
{
	reset_state();
	peripheral_state_init(PEER_BDADDR, LT_ADDR);
	run_packet_window();
	mu_assert_int_eq(BTCTL_STATE_CONNECTED, btctl.state);

	run_tx_done();
	run_silent_window_past_the_timeout();

	mu_assert_int_eq(BTCTL_STATE_STANDBY, btctl.state);
	mu_assert_int_eq(BTCTL_REASON_TIMEOUT, reported_reason);
	mu_assert_int_eq(0, page_scan_resumed);
}

/* and one that never carried a packet goes back to scanning. */
MU_TEST(a_peripheral_connection_that_carried_nothing_goes_back_to_page_scan)
{
	reset_state();
	peripheral_state_init(PEER_BDADDR, LT_ADDR);
	run_silent_window_past_the_timeout();

	mu_assert_int_eq(1, page_scan_resumed);
	mu_assert_int_eq(BTCTL_STATE_CONNECTED, btctl.state);
	mu_assert(reported_state != BTCTL_STATE_STANDBY,
		"the board went to standby instead of back to page scan");
}

int main(void)
{
	MU_RUN(a_central_connection_that_carried_a_packet_ends_in_standby);
	MU_RUN(a_central_connection_that_carried_nothing_goes_back_to_paging);
	MU_RUN(a_peripheral_connection_that_carried_a_packet_ends_in_standby);
	MU_RUN(a_peripheral_connection_that_carried_nothing_goes_back_to_page_scan);
	return mu_report();
}
