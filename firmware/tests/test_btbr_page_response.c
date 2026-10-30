/*
 * Unit tests for the page response of the BR/EDR image: the paged device's side
 * in firmware/btbr/src/btphy/page_scan_state.c and the pager's side in
 * firmware/btbr/src/btphy/paging_state.c.
 *
 * A pager sends two page messages in each of its transmit slots, half a slot
 * apart, and the paged device answers 625usec after whichever of the two it
 * heard. The central's page response -- the FHS packet carrying the clock and
 * address the connection is built on -- then follows an exact 1250usec after the
 * FIRST of those two messages, whichever one was answered (Bluetooth Core
 * specification Vol 2, Part B, Section 2.4.4). So it follows the paged device's
 * own answer by 625usec when the page it heard was the first message, and by
 * only 312.5usec when it was the second. A page message says nothing about
 * which of the two it is.
 *
 * These tests drive both state machines over a faked air and check that a
 * window the paged device opens lands on the tick and the frequency of a page
 * response the pager really sends, for a page heard in either half of the slot,
 * and that it keeps opening one for every repeat that falls inside pagerespTO.
 * The frequency is the half that matters most: both ends compute it
 * from the same page response phase and the same slot, so one end reading the
 * slot differently puts its window on another channel as well as another tick.
 *
 * Both state machines are included into this translation unit, which is how
 * their slot callbacks can be called at all: they are static. They run one at a
 * time, because they share the firmware's hop and clock state. The hop sequence
 * is the firmware's own -- hop.c and the PERM5 table are linked -- so the
 * channels here are the channels a board would tune.
 *
 * What is faked: the radio, the receive, transmit and scan tasks, the slot
 * scheduler, the FHS payload codec, the message pool and the two connection
 * states. The fakes carry the air's timing, and the one piece of firmware
 * behaviour they reproduce is which substates advance the page response phase
 * when a transmit or a window is prepared; the last case reads that rule back
 * out of rx_task.c and tx_task.c as text, so the two cannot drift apart
 * silently.
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
#include "source_scan.h"

/* The two diagnostics every btbr source can reach, declared by
 * stubs-btbr/ubtbr/debug.h, plus the console the page scan marks an answered
 * page on. All three are defined among the fakes. */
void cprintf(char *fmt, ...);
void __attribute__((noreturn)) die(char *fmt, ...);
void console_putc(char c);

/* TIMER1's counter, which both state machines read as MASTER_CLKN. Here it is
 * the clock field the harness moves on by one tick -- 312.5usec -- at a time. */
#define T1TC btphy.master_clkn

#include <ubtbr/btphy.h>
#include <ubtbr/bb.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/hop.h>
#include <ubtbr/page_scan_state.h>
#include <ubtbr/paging_state.h>
#include <ubtbr/rx_task.h>
#include <ubtbr/scan_task.h>
#include <ubtbr/tdma_sched.h>
#include <ubtbr/tx_task.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the firmware state the two state machines share ---- */

btphy_t btphy;
btctl_t btctl;

/* ---- the fakes under the state machines ---- */

void cprintf(char *fmt, ...) { (void)fmt; }
void console_putc(char c) { (void)c; }

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

void btctl_set_state(btctl_state_t state, btctl_reason_t reason)
{
	(void)reason;
	btctl.state = state;
}

/*
 * btphy.c's own version also programs the radio's access code. What matters
 * here is the rest of it: the channel address the hop sequence is built from
 * (Vol 2, Part B, Table 2.3), and the substate.
 */
void btphy_set_mode(btphy_mode_t mode, uint32_t lap, uint8_t uap)
{
	btphy.chan_lap = lap;
	btphy.chan_uap = uap;
	hop_init(lap|(uap<<24));
	btphy_set_mode_no_ap(mode);
}

void btphy_cancel_clkn_delay(void) { }

/* The clock the pager wrote into each FHS it sent, oldest first. */
#define FHS_SENT_MAX 8
static uint32_t fhs_sent_clk27_2[FHS_SENT_MAX];
static unsigned fhs_sent_count;

/* The clock the paged device is told by the FHS it receives. */
static uint32_t fhs_rx_clk27_2;

/* The address in that FHS. Any value will do; it only has to come back out of
 * the connection the page response sets up. */
#define FHS_RX_BDADDR	0x112233445566ull

void bbpkt_fhs_prepare_payload(uint8_t *data, uint64_t parity, uint32_t lap,
		uint8_t uap, uint16_t nap, uint32_t cls, uint8_t ltaddr,
		uint8_t has_eir)
{
	(void)data; (void)parity; (void)lap; (void)uap;
	(void)nap; (void)cls; (void)ltaddr; (void)has_eir;
}

void bbpkt_fhs_finalize_payload(uint8_t *fhs_data, uint32_t clk27_2)
{
	(void)fhs_data;
	if (fhs_sent_count < FHS_SENT_MAX)
		fhs_sent_clk27_2[fhs_sent_count] = clk27_2;
	fhs_sent_count++;
}

void bbpkt_decode_fhs(uint8_t *data, fhs_info_t *info)
{
	(void)data;
	info->bdaddr = FHS_RX_BDADDR;
	info->clk27_2 = fhs_rx_clk27_2;
	info->lt_addr = 1;
}

/* The connection states the page response hands over to. */
static unsigned master_started;
static unsigned slave_started;
static uint64_t slave_peer_bdaddr;

void master_state_init(void) { master_started++; }

void slave_state_init(uint64_t master_bdaddr, uint8_t lt_addr)
{
	(void)lt_addr;
	slave_peer_bdaddr = master_bdaddr;
	slave_started++;
}

/* ---- the faked air ---- */

/*
 * One entry per packet a role sends and per window it opens, in the order they
 * happen. `tick` is the harness's own count, which advances with both clocks.
 */
enum air_kind { AIR_TX, AIR_RX };

struct air_event {
	enum air_kind kind;
	uint32_t tick;
	uint8_t chan;
	uint32_t clkn;		/* the slot clock the frequency came from */
};

#define AIR_MAX 32
static struct air_event air_log[AIR_MAX];
static unsigned air_count;

/*
 * A transmit and a receive each take two ticks: a prepare tick that tunes the
 * radio, and the tick after it in which the packet goes out or the window
 * listens. Items of the slot scheduler itself take one.
 */
enum pending_what {
	P_TX_PREPARE,
	P_TX_DONE,
	P_RX_PREPARE,
	P_RX_EXECUTE,
	P_TDMA,
};

struct pending {
	enum pending_what what;
	uint32_t tick;
	int done;
	rx_task_cb_t rx_cb;
	tx_task_cb_t tx_cb;
	tdma_sched_cb *tdma_cb;
	void *arg;
	uint32_t clkn;		/* what rx_prepare() stamps on the packet */
};

#define PENDING_MAX 96
static struct pending pending[PENDING_MAX];
static unsigned pending_count;
static uint32_t air_tick;

/* Which window of the role being run the air puts a packet into, counted from
 * its first; 0 for none. A whole FHS rather than a bare access code when asked
 * for. */
static unsigned air_deliver_window;
static unsigned air_window_count;
static int air_deliver_payload;

/* The scan task's callback, which the test hands a page to. */
static scan_task_cb_t scan_cb;
static void *scan_cb_arg;

static struct pending *pending_add(enum pending_what what, uint32_t tick)
{
	struct pending *p;

	if (pending_count == PENDING_MAX)
		die("pending overflow\n");
	p = &pending[pending_count++];
	memset(p, 0, sizeof(*p));
	p->what = what;
	p->tick = tick;
	return p;
}

static void air_record(enum air_kind kind, uint32_t tick, uint8_t chan,
			uint32_t clkn)
{
	struct air_event *e;

	if (air_count == AIR_MAX)
		die("air log overflow\n");
	e = &air_log[air_count++];
	e->kind = kind;
	e->tick = tick;
	e->chan = chan;
	e->clkn = clkn;
}

void tx_task_schedule(unsigned delay, tx_task_cb_t cb, void *cbarg,
			bbhdr_t *hdr, uint8_t *data)
{
	struct pending *p = pending_add(P_TX_PREPARE, air_tick+delay);

	(void)hdr;
	(void)data;
	p->tx_cb = cb;
	p->arg = cbarg;
}

void rx_task_schedule(unsigned delay, rx_task_cb_t cb, void *cbarg,
			unsigned flags)
{
	struct pending *p = pending_add(P_RX_PREPARE, air_tick+delay);

	(void)flags;
	p->rx_cb = cb;
	p->arg = cbarg;
}

void tx_task_reset(void) { }
void rx_task_reset(void) { }

int tdma_schedule(uint8_t frame_offset, tdma_sched_cb *cb, uint8_t p1,
			uint8_t p2, uint16_t p3, int16_t prio)
{
	struct pending *p = pending_add(P_TDMA, air_tick+frame_offset);

	(void)p1;
	(void)p2;
	(void)p3;
	(void)prio;
	p->tdma_cb = cb;
	return 0;
}

void scan_task_schedule(unsigned delay, scan_task_cb_t cb, void *cbarg)
{
	(void)delay;
	scan_cb = cb;
	scan_cb_arg = cbarg;
}

/*
 * What rx_task.c's rx_prepare() and tx_task.c's tx_prepare() do that the air
 * can see: the substates that sweep a phase advance it, once per packet or
 * window, and the frequency comes from the slot clock of the tick the radio is
 * being tuned for. The last case in this file reads both rules back out of
 * those two files.
 */
static uint8_t air_prepare(enum air_kind kind)
{
	uint32_t clkn = btphy_cur_clkn()+1;

	if (kind == AIR_RX) {
		if (btphy.mode == BT_MODE_PERIPH_PAGE_RESPONSE)
			hop_increment();
	} else {
		if (btphy.mode == BT_MODE_INQUIRY ||
		    btphy.mode == BT_MODE_PAGING_RESPONSE)
			hop_increment();
	}
	return hop_channel(clkn);
}

/*
 * The frequency a page response substate uses in a central transmit slot
 * (CLK1 = 0) on a given phase: what both ends compute for the page response
 * packet. Leaves the hop and mode state as it found them.
 */
static uint8_t chan_of_phase(uint8_t x)
{
	uint8_t saved_x = hop_state.x;
	btphy_mode_t saved_mode = btphy.mode;
	uint8_t chan;

	hop_state.x = x;
	btphy_set_mode_no_ap(BT_MODE_PAGING_RESPONSE);
	chan = hop_channel(0);
	btphy_set_mode_no_ap(saved_mode);
	hop_state.x = saved_x;
	return chan;
}

/* What a window gets: nothing at all unless this is the window the test asked
 * for, and then either a bare access code, which is all an ID packet is, or a
 * whole FHS. */
static void air_window_packet(btctl_rx_pkt_t *pkt)
{
	air_window_count++;
	if (air_window_count != air_deliver_window)
		return;
	pkt->flags |= 1<<BBPKT_F_HAS_PKT;
	if (air_deliver_payload) {
		pkt->flags |= (1<<BBPKT_F_HAS_HDR)|(1<<BBPKT_F_HAS_CRC)|
				(1<<BBPKT_F_GOOD_CRC);
		pkt->bb_hdr.type = BB_TYPE_FHS;
	}
}

static void air_dispatch_one(struct pending *p)
{
	struct pending *next;
	msg_t *msg;
	btctl_rx_pkt_t *pkt;
	uint32_t clkn;
	uint8_t chan;

	switch (p->what) {
	case P_TX_PREPARE:
		clkn = btphy_cur_clkn()+1;
		chan = air_prepare(AIR_TX);
		air_record(AIR_TX, air_tick+1, chan, clkn);
		/* tx_finalize(), where a transmit callback runs, is a tick
		 * after the packet has gone out. */
		next = pending_add(P_TX_DONE, air_tick+2);
		next->tx_cb = p->tx_cb;
		next->arg = p->arg;
		break;
	case P_TX_DONE:
		if (p->tx_cb)
			p->tx_cb(p->arg);
		break;
	case P_RX_PREPARE:
		clkn = btphy_cur_clkn()+1;
		chan = air_prepare(AIR_RX);
		air_record(AIR_RX, air_tick+1, chan, clkn);
		next = pending_add(P_RX_EXECUTE, air_tick+1);
		next->rx_cb = p->rx_cb;
		next->arg = p->arg;
		next->clkn = clkn;
		break;
	case P_RX_EXECUTE:
		msg = btctl_msg_alloc(BTCTL_RX_PKT);
		pkt = (btctl_rx_pkt_t*)msg_put(msg, sizeof(*pkt));
		memset(pkt, 0, sizeof(*pkt));
		pkt->clkn = p->clkn;
		air_window_packet(pkt);
		p->rx_cb(msg, p->arg, 0);
		break;
	case P_TDMA:
		p->tdma_cb(0, 0, 0);
		break;
	}
}

/*
 * One tick of the slot clock, as btphy.c's timer handler runs it: both clocks
 * move on, then this tick's tasks run. A task may schedule another for the same
 * tick, which the firmware's scheduler also runs, so the pass repeats until
 * nothing is left for this tick.
 */
static void air_run(unsigned ticks)
{
	unsigned i;
	int again;

	while (ticks--) {
		btphy.master_clkn++;
		btphy.slave_clkn++;
		air_tick++;

		again = 1;
		while (again) {
			again = 0;
			for (i = 0; i < pending_count; i++) {
				if (pending[i].done ||
				    pending[i].tick != air_tick)
					continue;
				pending[i].done = 1;
				air_dispatch_one(&pending[i]);
				again = 1;
				break;
			}
		}
	}
}

static void air_reset(void)
{
	memset(&btphy, 0, sizeof(btphy));
	memset(&btctl, 0, sizeof(btctl));
	memset(&hop_state, 0, sizeof(hop_state));
	memset(air_log, 0, sizeof(air_log));
	air_count = 0;
	pending_count = 0;
	air_tick = 0;
	air_deliver_window = 0;
	air_window_count = 0;
	air_deliver_payload = 0;
	fhs_sent_count = 0;
	fhs_rx_clk27_2 = 0;
	master_started = 0;
	slave_started = 0;
	slave_peer_bdaddr = 0;
	scan_cb = NULL;
	scan_cb_arg = NULL;
}

/* The nth event of one kind, counted from 1. Stops the run if there is no such
 * event: every caller goes on to read it. */
static const struct air_event *air_nth(const struct air_event *log,
			unsigned count, enum air_kind kind, unsigned n)
{
	unsigned i, seen = 0;

	for (i = 0; i < count; i++) {
		if (log[i].kind != kind)
			continue;
		if (++seen == n)
			return &log[i];
	}
	die("no %s number %u in the air log\n",
		kind == AIR_TX ? "transmit" : "window", n);
}

/* How many events of one kind the log holds. */
static unsigned air_tally(const struct air_event *log, unsigned count,
			enum air_kind kind)
{
	unsigned i, seen = 0;

	for (i = 0; i < count; i++) {
		if (log[i].kind == kind)
			seen++;
	}
	return seen;
}

/* ---- the two roles ---- */

/* The paged device's address. Both ends hop on it, so it is what the page
 * frequencies are built from. */
#define PAGED_LAP	0x5a3c17
#define PAGED_UAP	0x42

/* The pager's own address, which only goes into the FHS payload. */
#define PAGER_LAP	0x0d1e2f
#define PAGER_UAP	0x7b

/* Where the pager's clock starts. The page train picks its own first transmit
 * tick from it. */
#define PAGER_CLKN_START	0x2f3a16u

/* How long a role is run for: long enough for the pager to repeat its page
 * response for the whole of pagerespTO and for the paged device to open every
 * window it opens over that time. */
#define RUN_TICKS	30

static struct air_event pager_log[AIR_MAX];
static unsigned pager_log_count;
/* The harness tick, and the pager's own clock, of its first page message. */
static uint32_t pager_t0;
static uint32_t pager_clkn_t0;

/*
 * Page a device, with the paged device answering the page message the pager
 * sent `half` of the two in its transmit slot (1 or 2) and acknowledging
 * nothing afterwards, so the repeated page responses are recorded too.
 */
static void run_pager(unsigned half)
{
	air_reset();
	btphy.master_clkn = PAGER_CLKN_START;
	btphy.my_lap = PAGER_LAP;
	btphy.my_uap = PAGER_UAP;
	air_deliver_window = half;
	paging_state_setup(PAGED_LAP, PAGED_UAP);
	air_run(RUN_TICKS);

	memcpy(pager_log, air_log, sizeof(pager_log));
	pager_log_count = air_count;
	pager_t0 = air_nth(pager_log, pager_log_count, AIR_TX, 1)->tick;
	/* The clock started at PAGER_CLKN_START and moves with the harness. */
	pager_clkn_t0 = PAGER_CLKN_START + pager_t0;
}

static struct air_event paged_log[AIR_MAX];
static unsigned paged_log_count;
/* The harness tick the page arrived in, and which of the pager's two page
 * messages it was, so a paged event can be read on the pager's time. */
static uint32_t paged_t0;
static unsigned paged_half;

/*
 * Be paged: the scan is listening on `scan_phase`, hears the `half`th of the
 * pager's two page messages, answers it, and the central's page response
 * arrives in window `fhs_window` of the ones the response substate opens
 * (0 for none), carrying `clk27_2`.
 */
static void run_paged(uint8_t scan_phase, unsigned half, unsigned fhs_window,
			uint32_t clk27_2)
{
	scan_task_cb_t cb;
	void *arg;

	air_reset();
	btphy.my_lap = PAGED_LAP;
	btphy.my_uap = PAGED_UAP;
	/* A scan listens on the frequency its own CLKN16-12 selects, so this is
	 * how the board is put on the phase the pager is about to try. */
	btphy.master_clkn = (uint32_t)scan_phase << 12;
	air_deliver_window = fhs_window;
	air_deliver_payload = 1;
	fhs_rx_clk27_2 = clk27_2;
	paged_half = half;

	page_scan_state_setup();
	air_run(2);
	if (scan_cb == NULL)
		die("the page scan opened no scan window\n");

	/* What scan_task.c does when a page's access code arrives: tune the one
	 * frequency the scan listens on, which is where the response phase
	 * comes from as well, move the slot clock to the packet, and hand it
	 * over. The callback is taken first, so that a scan re-armed afterwards
	 * can be told apart from this one. */
	hop_channel(0);
	btphy.slave_clkn = 0;
	paged_t0 = air_tick;
	cb = scan_cb;
	arg = scan_cb_arg;
	scan_cb = NULL;
	cb(1, arg);
	air_run(RUN_TICKS);

	memcpy(paged_log, air_log, sizeof(paged_log));
	paged_log_count = air_count;
}

/* A paged event's tick on the pager's time: ticks since the page it heard, plus
 * which of the pager's two page messages that was. */
static uint32_t paged_air_tick(const struct air_event *e)
{
	return e->tick - paged_t0 + (paged_half-1);
}

/* The page response phase the pager tried in its `half`th page message. That is
 * the phase a scan has to be listening on for the page to be heard at all. */
static uint8_t pager_page_phase(unsigned half)
{
	const struct air_event *e = air_nth(pager_log, pager_log_count,
						AIR_TX, half);

	return calc_iterate_slave_page_scan_phase(e->clkn);
}

/* ---- the cases ---- */

/*
 * The pager's side, which Section 2.4.4 fixes and this change leaves alone: two
 * page messages half a slot apart, a window 625usec after each, and the page
 * response an exact 1250usec after the FIRST page message whichever of the two
 * was answered -- on the frequency after the one the answered page message went
 * out on, and in a central transmit slot (CLK1 = 0) either way.
 */
static void check_pager_timing(unsigned half)
{
	const struct air_event *page1, *page2, *answer, *fhs;

	run_pager(half);
	page1 = air_nth(pager_log, pager_log_count, AIR_TX, 1);
	page2 = air_nth(pager_log, pager_log_count, AIR_TX, 2);
	answer = air_nth(pager_log, pager_log_count, AIR_RX, half);
	fhs = air_nth(pager_log, pager_log_count, AIR_TX, 3);

	/* Both page messages are in the one transmit slot, so both are at
	 * CLK1 = 0, and they sweep consecutive phases. */
	mu_assert_int_eq(1, page2->tick - page1->tick);
	mu_assert_int_eq(0, page1->clkn & 2);
	mu_assert_int_eq(0, page2->clkn & 2);
	mu_assert_int_eq((uint8_t)(pager_page_phase(1)+1), pager_page_phase(2));

	/* The window for an answer, 625usec -- two ticks -- after the page
	 * message it would answer. */
	mu_assert_int_eq(half+1, answer->tick - page1->tick);

	/* The page response, 1250usec after the first page message. */
	mu_assert_int_eq(4, fhs->tick - page1->tick);
	mu_assert_int_eq(0, fhs->clkn & 2);
	mu_assert_int_eq(chan_of_phase(pager_page_phase(half)+1), fhs->chan);
	/* It says which tick it went out in, in units of four. */
	mu_assert_int_eq(fhs->clkn >> 2, fhs_sent_clk27_2[0]);
	/* Nothing acknowledged it, so no connection came up on this side. */
	mu_assert_int_eq(0, master_started);
}

MU_TEST(pager_page_response_timing_first_half)
{
	check_pager_timing(1);
}

MU_TEST(pager_page_response_timing_second_half)
{
	check_pager_timing(2);
}

/*
 * A page heard in the first half of the pager's transmit slot, the case that
 * already worked: the answer two ticks after the page, then one window, in the
 * tick the page response is in and on its frequency, and the connection.
 */
MU_TEST(paged_first_half_window_meets_the_page_response)
{
	const struct air_event *answer, *window, *ack;
	const struct air_event *fhs, *answer_window, *ack_window;
	uint8_t chan;

	run_pager(1);
	fhs = air_nth(pager_log, pager_log_count, AIR_TX, 3);
	answer_window = air_nth(pager_log, pager_log_count, AIR_RX, 1);
	ack_window = air_nth(pager_log, pager_log_count, AIR_RX, 2);
	mu_assert_int_eq(6, ack_window->tick - pager_t0);
	chan = fhs->chan;

	run_paged(pager_page_phase(1), 1, 1, fhs->clkn >> 2);

	answer = air_nth(paged_log, paged_log_count, AIR_TX, 1);
	window = air_nth(paged_log, paged_log_count, AIR_RX, 1);
	ack = air_nth(paged_log, paged_log_count, AIR_TX, 2);

	/* The answer: two ticks after the page, in the pager's window and on
	 * its frequency. */
	mu_assert_int_eq(2, paged_air_tick(answer));
	mu_assert_int_eq(answer_window->chan, answer->chan);

	/* The one window: the page response's own tick and frequency. */
	mu_assert_int_eq(4, paged_air_tick(window));
	mu_assert_int_eq(chan, window->chan);
	mu_assert_int_eq(1, air_window_count);

	/* The connection, and the acknowledgement 625usec after the page
	 * response, where the pager is listening for it. */
	mu_assert_int_eq(1, slave_started);
	mu_assert(slave_peer_bdaddr == FHS_RX_BDADDR,
		"the connection took the wrong address from the FHS");
	mu_assert_int_eq(6, paged_air_tick(ack));
	mu_assert_int_eq(ack_window->chan, ack->chan);
}

/*
 * A page heard in the second half of the pager's transmit slot. The answer goes
 * out a tick later than in the first-half case, but the page response does not:
 * it is still 1250usec after the pager's first page message, so it is already
 * past by the time the first window opens. The second window takes the other
 * reading of the page and meets the page response the pager repeats 1.25ms
 * later, on that one's frequency.
 */
MU_TEST(paged_second_half_window_meets_the_page_response)
{
	const struct air_event *answer, *first, *second, *ack;
	const struct air_event *fhs, *answer_window, *ack_window;
	uint8_t chan;

	run_pager(2);
	fhs = air_nth(pager_log, pager_log_count, AIR_TX, 3);
	answer_window = air_nth(pager_log, pager_log_count, AIR_RX, 2);
	/* The repeat is the next central transmit slot, 1.25ms on, and the
	 * window for its acknowledgement 625usec after that. */
	mu_assert_int_eq(4, fhs->tick - pager_t0);
	fhs = air_nth(pager_log, pager_log_count, AIR_TX, 4);
	ack_window = air_nth(pager_log, pager_log_count, AIR_RX, 4);
	mu_assert_int_eq(8, fhs->tick - pager_t0);
	mu_assert_int_eq(10, ack_window->tick - pager_t0);
	chan = fhs->chan;

	run_paged(pager_page_phase(2), 2, 2, fhs->clkn >> 2);

	mu_assert_fatal(air_tally(paged_log, paged_log_count, AIR_RX) >= 2,
		"the page response opened only one window, so a page heard "
		"second is answered and then left waiting");
	answer = air_nth(paged_log, paged_log_count, AIR_TX, 1);
	first = air_nth(paged_log, paged_log_count, AIR_RX, 1);
	second = air_nth(paged_log, paged_log_count, AIR_RX, 2);
	ack = air_nth(paged_log, paged_log_count, AIR_TX, 2);

	/* The answer: two ticks after the page it heard, which is the second
	 * half of the pager's receive slot, and on that half's frequency. */
	mu_assert_int_eq(3, paged_air_tick(answer));
	mu_assert_int_eq(answer_window->chan, answer->chan);

	/* The first window is a tick after the first page response has gone
	 * by: the miss this case is about. */
	mu_assert_int_eq(5, paged_air_tick(first));

	/* The second window is the repeat's own tick and frequency... */
	mu_assert_int_eq(8, paged_air_tick(second));
	mu_assert_int_eq(chan, second->chan);
	/* ...and that is a different frequency from the first window's, so a
	 * window in the right tick on the first window's channel would not
	 * do. */
	mu_assert(second->chan != first->chan,
		"the two windows came out on one frequency");

	/* The connection, and the acknowledgement 625usec after the page
	 * response, where the pager is listening for it. */
	mu_assert_int_eq(1, slave_started);
	mu_assert_int_eq(10, paged_air_tick(ack));
	mu_assert_int_eq(ack_window->chan, ack->chan);

	/* Section 2.4.4's last rule, which is what puts that acknowledgement in
	 * the pager's window: the paged device takes its timing from the page
	 * response and not from the page message, so this board's clock now
	 * reads the pager's. */
	mu_assert_int_eq(pager_clkn_t0 + (air_tick - paged_t0 + paged_half-1),
			btphy.slave_clkn);
}

/*
 * Section 8.3.3.1: the paged device keeps listening for the central's page
 * response until pagerespTO, which is 8 slots, and 8.3.3.2 has the central
 * repeat that packet every 1.25 ms over the same time. So an empty window is
 * followed by a window for the next repeat, for as long as a window still falls
 * inside pagerespTO.
 *
 * The windows take the two readings of the answered page in turn -- 1.25 ms
 * apart on one reading, which is 3 ticks and then 5 ticks as the clock moves
 * between the two -- so neither reading is starved. Each one is a central
 * transmit slot further along the page response phase, which is what makes
 * window number k meet the central's kth repeat.
 */
MU_TEST(paged_listens_for_every_repeat_until_pageresp_to)
{
	unsigned i;
	/* The ticks of the windows, on the pager's time, for a page answered in
	 * the first half: the first page response and then its repeats, taking
	 * the first reading of the page (ticks 4 and 12) and the second (7 and
	 * 15) in turn. */
	static const uint32_t window_tick[] = { 4, 7, 12, 15 };
	/* The answer, which pagerespTO runs from. */
	static const uint32_t answer_tick = 2;
	uint8_t scan_phase, repeat_chan[4];
	uint32_t fifth_repeat_tick;

	/* The pager's own page responses: the first and the third are the two
	 * this board's windows can meet when the page it heard was the pager's
	 * first message. */
	run_pager(1);
	scan_phase = pager_page_phase(1);
	repeat_chan[0] = air_nth(pager_log, pager_log_count, AIR_TX, 3)->chan;
	repeat_chan[2] = air_nth(pager_log, pager_log_count, AIR_TX, 5)->chan;
	fifth_repeat_tick = air_nth(pager_log, pager_log_count, AIR_TX, 7)->tick;

	run_paged(scan_phase, 1, 0, 0);

	mu_assert_int_eq(ARRAY_LEN(window_tick), air_window_count);
	mu_assert_int_eq(ARRAY_LEN(window_tick),
			air_tally(paged_log, paged_log_count, AIR_RX));
	for (i = 0; i < ARRAY_LEN(window_tick); i++) {
		const struct air_event *w = air_nth(paged_log, paged_log_count,
							AIR_RX, i+1);

		mu_assert_int_eq(window_tick[i], paged_air_tick(w));
		/* One central transmit slot further along the phase per
		 * window, which is the phase the pager's own kth repeat goes
		 * out on. The odd-numbered windows are checked against the
		 * channels this pager really transmitted the first and third
		 * on; the even-numbered ones take the other reading of the
		 * page, which paged_completes_on_a_later_repeat() checks
		 * against a pager whose second message was the one heard. */
		mu_assert_int_eq(chan_of_phase(scan_phase+i+1), w->chan);
		if ((i & 1) == 0)
			mu_assert_int_eq(repeat_chan[i], w->chan);
	}

	/* The last window is the last one pagerespTO allows, which is why the
	 * board stops there: the pager's fifth page response falls outside it,
	 * so there is nothing to wait for. */
	mu_assert(window_tick[ARRAY_LEN(window_tick)-1] - answer_tick
					<= PAGE_RESP_TO_TICKS,
		"the last window falls outside pagerespTO");
	mu_assert(fifth_repeat_tick - pager_t0 - answer_tick
					> PAGE_RESP_TO_TICKS,
		"the pager's fifth page response is inside pagerespTO, so the "
		"board stops one repeat too early");

	/* Nothing arrived, so the board goes back to scanning rather than
	 * sitting in page scan with no task left to run. */
	mu_assert_int_eq(0, slave_started);
	mu_assert_int_eq(BT_MODE_PAGE_SCAN, btphy.mode);
	mu_assert_int_eq(BTCTL_STATE_PAGE_SCAN, btctl.state);
	mu_assert(scan_cb != NULL, "the board stopped with nothing left to run");
}

/*
 * A page response that only arrives on a later repeat still completes the
 * connection. The page here is heard in the second half of the pager's slot and
 * the first three repeats are missed, so this is the fourth window -- past the
 * two the board used to open. The acknowledgement still lands in the window the
 * pager is listening in, because the board takes its timing from the page
 * response it received rather than from the page message.
 */
MU_TEST(paged_completes_on_a_later_repeat)
{
	const struct air_event *fhs, *ack_window, *window, *ack;
	uint8_t second_repeat_chan;

	run_pager(2);
	/* The pager's fourth page response, after its two page messages, and
	 * the window it listens for the acknowledgement in. */
	second_repeat_chan = air_nth(pager_log, pager_log_count,
					AIR_TX, 4)->chan;
	fhs = air_nth(pager_log, pager_log_count, AIR_TX, 6);
	ack_window = air_nth(pager_log, pager_log_count, AIR_RX, 6);
	mu_assert_int_eq(16, fhs->tick - pager_t0);
	mu_assert_int_eq(18, ack_window->tick - pager_t0);

	run_paged(pager_page_phase(2), 2, 4, fhs->clkn >> 2);

	mu_assert_int_eq(4, air_window_count);
	window = air_nth(paged_log, paged_log_count, AIR_RX, 4);
	ack = air_nth(paged_log, paged_log_count, AIR_TX, 2);

	/* The even-numbered windows are the ones that meet a pager whose second
	 * page message was the one heard: this run's second window is on the
	 * channel that pager sent its second page response on, and its fourth
	 * on the fourth's. */
	mu_assert_int_eq(8, paged_air_tick(air_nth(paged_log, paged_log_count,
							AIR_RX, 2)));
	mu_assert_int_eq(second_repeat_chan,
			air_nth(paged_log, paged_log_count, AIR_RX, 2)->chan);

	/* The fourth window is the fourth repeat's own tick and frequency. */
	mu_assert_int_eq(16, paged_air_tick(window));
	mu_assert_int_eq(fhs->chan, window->chan);

	mu_assert_int_eq(1, slave_started);
	mu_assert(slave_peer_bdaddr == FHS_RX_BDADDR,
		"the connection took the wrong address from the FHS");
	mu_assert_int_eq(18, paged_air_tick(ack));
	mu_assert_int_eq(ack_window->chan, ack->chan);
}

/*
 * The fakes' air_prepare() stands in for the two task files, which cannot run
 * here. Read the rule it copies back out of them: a window of the paged
 * device's page response advances the phase, the pager's page response
 * transmits do, and both tune from the slot clock of the tick being prepared.
 */
static void read_function(const char *path, const char *name, char *out,
			size_t size)
{
	char line[SCAN_LINE_MAX];
	FILE *f = fopen(path, "r");
	int in_block = 0, in_function = 0;
	char call[64];

	if (f == NULL)
		die("cannot open %s\n", path);
	out[0] = '\0';
	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		strip_string_literals(line);
		if (!in_function) {
			if (call_name(line, call, sizeof(call)) &&
			    strcmp(call, name) == 0)
				in_function = 1;
			continue;
		}
		if (strlen(out) + strlen(line) + 1 < size)
			strcat(out, line);
		/* The function's own closing brace, the only one in column 0 */
		if (line[0] == '}')
			break;
	}
	fclose(f);
	if (out[0] == '\0')
		die("no %s() found in %s\n", name, path);
}

MU_TEST(prepare_model_matches_the_task_files)
{
	char body[8192];

	read_function(FW_BTBR_DIR "/src/btphy/rx_task.c", "rx_prepare",
			body, sizeof(body));
	mu_assert(contains_identifier(body, "BT_MODE_PERIPH_PAGE_RESPONSE"),
		"rx_prepare() no longer singles out the peripheral page response");
	mu_assert(calls_identifier(body, "hop_increment"),
		"rx_prepare() no longer advances the page response phase");
	mu_assert(calls_identifier(body, "hop_channel"),
		"rx_prepare() no longer tunes from hop_channel()");
	mu_assert(is_assignment_of(body, "uint32_t rx_clkn",
					"btphy_cur_clkn()+1"),
		"rx_prepare() no longer tunes for the tick after the prepare");

	read_function(FW_BTBR_DIR "/src/btphy/tx_task.c", "tx_prepare",
			body, sizeof(body));
	mu_assert(contains_identifier(body, "BT_MODE_PAGING_RESPONSE"),
		"tx_prepare() no longer singles out the central page response");
	mu_assert(!contains_identifier(body, "BT_MODE_PERIPH_PAGE_RESPONSE"),
		"tx_prepare() now advances the phase for the paged device too");
	mu_assert(calls_identifier(body, "hop_increment"),
		"tx_prepare() no longer advances the phase");
	mu_assert(is_assignment_of(body, "uint32_t tx_clkn",
					"btphy_cur_clkn()+1"),
		"tx_prepare() no longer tunes for the tick after the prepare");
}

int main(void)
{
	MU_RUN(pager_page_response_timing_first_half);
	MU_RUN(pager_page_response_timing_second_half);
	MU_RUN(paged_first_half_window_meets_the_page_response);
	MU_RUN(paged_second_half_window_meets_the_page_response);
	MU_RUN(paged_listens_for_every_repeat_until_pageresp_to);
	MU_RUN(paged_completes_on_a_later_repeat);
	MU_RUN(prepare_model_matches_the_task_files);
	return mu_report();
}

/* The two state machines under test, included so their slot callbacks -- all
 * static -- can be driven. They come last so that everything above is already
 * declared. */
#include "../btbr/src/btphy/paging_state.c"
/* Each of the two names the slot index it prepares a transmit in. */
#undef TX_PREPARE_IDX
#include "../btbr/src/btphy/page_scan_state.c"
