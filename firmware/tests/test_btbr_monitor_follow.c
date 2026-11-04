/*
 * Unit tests for what the BR/EDR image's page watcher does when the scheduler
 * cannot take the slot that starts following a connection:
 * firmware/btbr/src/btphy/monitor1_state.c.
 *
 * The watcher listens for a page, reads the central's page response, and then
 * asks for the next slot to start following the connection the two devices are
 * making. A slot holds four tasks and refuses the fifth, and a refusal there
 * used to be thrown away: nothing of the follow was armed, nothing was left
 * listening, and the board sat in the state the user asked for until the
 * watcher's own minute ran out. It now goes back to watching for a page.
 *
 * The state machine is included into this translation unit, which is how its
 * slot task and callbacks can be reached at all: they are static. Everything
 * under it is faked -- the scan, receive and transmit tasks, the scheduler,
 * the hop sequence and the FHS codec -- because none of it decides the
 * question here. The scheduler is a switch rather than the firmware's own:
 * what a real full slot does to a real arm is pinned in
 * test_btbr_scan_schedule.c, and what is left to say is which way this state
 * machine goes when it is told.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"

/* The diagnostics every btbr source can reach, declared by
 * stubs-btbr/ubtbr/debug.h, plus the console a heard page is marked on. All
 * three are defined among the fakes. */
void cprintf(char *fmt, ...);
void __attribute__((noreturn)) die(char *fmt, ...);
void console_putc(char c);

/* TIMER1's counter, which the state machine reads as CENTRAL_CLKN. Here it is
 * the clock field the tests set. */
#define T1TC btphy.central_clkn

#include <ubtbr/btphy.h>
#include <ubtbr/bb.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/hop.h>
#include <ubtbr/monitor_state.h>
#include <ubtbr/rx_task.h>
#include <ubtbr/scan_task.h>
#include <ubtbr/tdma_sched.h>
#include <ubtbr/tx_task.h>

/* ---- the firmware state the watcher reads ---- */

btphy_t btphy;
btctl_t btctl;
hop_state_t hop_state;

/* ---- the fakes under it ---- */

static char console[256];
static unsigned console_len;

void cprintf(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	console_len += vsnprintf(console + console_len,
				sizeof(console) - console_len, fmt, args);
	va_end(args);
	/* vsnprintf() returns the length it would have written, so a line that
	 * filled the buffer would otherwise leave console_len past the end and
	 * the next call's remaining size negative. */
	if (console_len > sizeof(console) - 1)
		console_len = sizeof(console) - 1;
}

void console_putc(char c) { (void)c; }

void __attribute__((noreturn)) die(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	abort();
}

/* The state the board last reported to the host. */
static btctl_state_t reported_state;

void btctl_set_state(btctl_state_t state, btctl_reason_t reason)
{
	(void)reason;
	btctl.state = state;
	reported_state = state;
}

/* The message queue to the host, which is where a decoded FHS goes. The one
 * the watcher sends is counted and released here, so the memory pool lasts
 * the run. */
static unsigned fhs_sent_to_host;

void msg_queue_init(msg_queue_t *q) { (void)q; }

msg_t *msg_dequeue(msg_queue_t *q) { (void)q; return NULL; }

int msg_enqueue(msg_queue_t *q, msg_t *msg)
{
	(void)q;
	fhs_sent_to_host++;
	msg_free(msg);
	return 0;
}

/* The memory the messages come out of. One message is alive at a time here. */
static uint8_t message_memory[512];

void *btctl_mem_alloc(unsigned size)
{
	if (size > sizeof(message_memory))
		die("btctl_mem_alloc: %u bytes asked for\n", size);
	return message_memory;
}

void btctl_mem_free(void *p) { (void)p; }

void btphy_set_mode(btphy_mode_t mode, uint32_t lap, uint8_t uap)
{
	btphy.chan_lap = lap;
	btphy.chan_uap = uap;
	btphy.mode = mode;
}

void btphy_adj_clkn_delay(int delay) { (void)delay; }

/* The watcher is set up with the address of the device being paged, which it
 * listens on. */
static uint64_t watched_bdaddr;

void btphy_set_bdaddr(uint64_t bdaddr) { watched_bdaddr = bdaddr; }

uint8_t hop_channel(uint32_t clkn) { (void)clkn; return 0; }

/* The address and clock the FHS carries. Nothing here is about their values,
 * only about whether the FHS was read and passed on. */
void bbpkt_decode_fhs(uint8_t *data, fhs_info_t *info)
{
	(void)data;
	memset(info, 0, sizeof(*info));
	info->bdaddr = 0x001122334455ull;
	info->lt_addr = 1;
}

/* Where the follow starts, which is what must not happen on a refusal. */
static unsigned follows_started;

void monitor2_state_init(uint64_t central_bdaddr)
{
	(void)central_bdaddr;
	follows_started++;
}

/* The scan window and the two slot tasks the watcher arms. Each is armed one
 * at a time, and a test runs it. */
static scan_task_cb_t armed_scan_cb;
static void *armed_scan_arg;
static unsigned scans_armed;

int scan_task_schedule(scan_task_cb_t cb, void *cbarg)
{
	armed_scan_cb = cb;
	armed_scan_arg = cbarg;
	scans_armed++;
	return 0;
}

static rx_task_cb_t armed_rx_cb;
static void *armed_rx_arg;

void rx_task_schedule(unsigned delay, rx_task_cb_t cb, void *cbarg,
			unsigned flags)
{
	(void)delay;
	(void)flags;
	armed_rx_cb = cb;
	armed_rx_arg = cbarg;
}

static unsigned transmits_armed;

void tx_task_schedule(unsigned delay, tx_task_cb_t cb, void *cbarg,
			bbhdr_t *hdr, uint8_t *data)
{
	(void)delay;
	(void)cb;
	(void)cbarg;
	(void)hdr;
	(void)data;
	transmits_armed++;
}

/*
 * The scheduler, as a switch: `sched_refuses` makes the next arm the refusal a
 * full slot gives. What the watcher asks for is the slot that starts the
 * follow, and nothing else here goes through it.
 */
static int sched_refuses;
static unsigned slot_tasks_armed;
static tdma_sched_cb *armed_slot_task;

int tdma_schedule(uint8_t frame_offset, tdma_sched_cb *cb, uint8_t p1,
			uint8_t p2, uint16_t p3, int16_t prio)
{
	(void)frame_offset;
	(void)p1;
	(void)p2;
	(void)p3;
	(void)prio;

	if (sched_refuses)
		return -1;
	armed_slot_task = cb;
	slot_tasks_armed++;
	return 0;
}

#include "../btbr/src/btphy/monitor1_state.c"

/* ---- driving the watcher ---- */

static void a_watcher(void)
{
	memset(&btphy, 0, sizeof(btphy));
	memset(&btctl, 0, sizeof(btctl));
	btphy.my_lap = 0x9e8b33;
	btphy.my_uap = 0x42;

	armed_scan_cb = NULL;
	armed_scan_arg = NULL;
	scans_armed = 0;
	armed_rx_cb = NULL;
	transmits_armed = 0;
	slot_tasks_armed = 0;
	armed_slot_task = NULL;
	follows_started = 0;
	fhs_sent_to_host = 0;
	sched_refuses = 0;
	reported_state = BTCTL_STATE_STANDBY;
	console_len = 0;
	console[0] = '\0';

	monitor1_state_setup(0x0011223344ull);
	if (armed_scan_cb == NULL)
		die("the watcher opened no scan window\n");
}

/* A page arrives in the window the watcher opened, which is what makes it
 * answer and then listen for the page response. The callback is taken first,
 * so a scan re-armed afterwards can be told apart from this one. */
static void a_page_arrives(void)
{
	scan_task_cb_t cb = armed_scan_cb;
	void *arg = armed_scan_arg;

	armed_scan_cb = NULL;
	scans_armed = 0;
	cb(1, arg);
	if (armed_rx_cb == NULL)
		die("the watcher opened no window for the page response\n");
}

/* The page response arrives in that window and decodes, which is where the
 * follow is armed from. */
static void the_page_response_arrives(void)
{
	rx_task_cb_t cb = armed_rx_cb;
	void *arg = armed_rx_arg;
	btctl_rx_pkt_t *pkt;
	msg_t *msg;

	msg = btctl_msg_alloc(BTCTL_RX_PKT);
	pkt = (btctl_rx_pkt_t *)msg_put(msg, sizeof(*pkt));
	memset(pkt, 0, sizeof(*pkt));
	pkt->bb_hdr.type = BB_TYPE_FHS;
	pkt->flags = (1<<BBPKT_F_HAS_PKT)|(1<<BBPKT_F_HAS_HDR)
			|(1<<BBPKT_F_HAS_CRC)|(1<<BBPKT_F_GOOD_CRC);

	armed_rx_cb = NULL;
	cb(msg, arg, 0);
}

/* ---- the cases ---- */

/* The ordinary case: the follow is armed and starts, and the FHS reaches the
 * host once. */
MU_TEST(a_page_response_starts_the_follow)
{
	a_watcher();
	a_page_arrives();

	the_page_response_arrives();

	mu_assert_int_eq(1, (int)slot_tasks_armed);
	mu_assert(armed_slot_task != NULL, "no follow task was armed");
	armed_slot_task(0, 0, 0);
	mu_assert_int_eq(1, (int)follows_started);
	mu_assert_int_eq(1, (int)fhs_sent_to_host);
	mu_assert(watched_bdaddr == 0x0011223344ull,
		"the watcher listened on another address");
	/* Still in the page scan the host asked for, now following. */
	mu_assert_int_eq(BTCTL_STATE_PAGE_SCAN, btctl.state);
}

/*
 * The slot that would start the follow is full, so no part of the follow is
 * armed. The board goes back to watching for a page rather than sitting in
 * the state the host asked for with nothing left to run.
 */
MU_TEST(a_follow_that_cannot_be_armed_goes_back_to_watching)
{
	a_watcher();
	a_page_arrives();
	sched_refuses = 1;

	the_page_response_arrives();

	mu_assert_int_eq(0, (int)slot_tasks_armed);
	mu_assert_int_eq(0, (int)follows_started);
	/* Watching again, on the scan substate rather than left on the page
	 * response's. */
	mu_assert_int_eq(1, (int)scans_armed);
	mu_assert(armed_scan_cb != NULL,
		"the board stopped with nothing left to run");
	mu_assert_int_eq(BT_MODE_PAGE_SCAN, btphy.mode);
	mu_assert_int_eq(BTCTL_STATE_PAGE_SCAN, btctl.state);
}

MU_TEST(a_follow_that_cannot_be_armed_says_so_on_the_console)
{
	a_watcher();
	a_page_arrives();
	sched_refuses = 1;

	the_page_response_arrives();

	mu_assert(strstr(console, "cannot schedule follow") != NULL, console);
}

/* The FHS was decoded, so it still goes to the host: what was lost is the
 * follow, not the packet. */
MU_TEST(a_follow_that_cannot_be_armed_still_reports_the_fhs)
{
	a_watcher();
	a_page_arrives();
	sched_refuses = 1;

	the_page_response_arrives();

	mu_assert_int_eq(1, (int)fhs_sent_to_host);
}

int main(void)
{
	printf("BR/EDR page watcher tests: no slot for the follow\n");
	MU_RUN(a_page_response_starts_the_follow);
	MU_RUN(a_follow_that_cannot_be_armed_goes_back_to_watching);
	MU_RUN(a_follow_that_cannot_be_armed_says_so_on_the_console);
	MU_RUN(a_follow_that_cannot_be_armed_still_reports_the_fhs);
	return mu_report();
}
