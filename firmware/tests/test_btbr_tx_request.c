/* What the device makes of a BTCTL_TX_PKT_REQ the host sends it.
 *
 * The host tool checks a `tx` command line before it sends anything, but it
 * is not the only thing that can send this request: an older build of it, a
 * hand-written client, or a message that arrived short. So the device checks
 * the request again, and these are the checks that keep the encoder reading
 * inside the message the host framed. Every refusal is reached here through
 * btctl_work(), the way a request from the host reaches it.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "minunit.h"

#include <ubtbr/btctl_intf.h>
#include <ubtbr/btphy.h>
#include <ubtbr/bb.h>
#include <ubtbr/codec.h>

btphy_t btphy;
btctl_t btctl;

void __attribute__((noreturn)) die(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	abort();
}

void cprintf(char *fmt, ...) { (void)fmt; }

/* ---- the pool, and which blocks have been handed back ---- */

#define FREED_MAX	32
static void *freed[FREED_MAX];
static unsigned freed_count;

void *mem_pool_alloc(unsigned size) { return malloc(size); }

void mem_pool_free(void *p)
{
	if (freed_count < FREED_MAX)
		freed[freed_count++] = p;
}

static int was_freed(const void *p)
{
	unsigned i;

	for (i = 0; i < freed_count; i++)
		if (freed[i] == p)
			return 1;
	return 0;
}

/* The real queue.c is linked, so a queue behaves as it does on the board: a
 * ring of MSG_QUEUE_SIZE, and msg_queue_empty() answers from its own counters.
 */
static unsigned queued(msg_queue_t *q)
{
	return (unsigned)((q->tail - q->head) & (MSG_QUEUE_SIZE - 1));
}

/* ---- everything else btctl.c reaches for, which no refusal touches ---- */

void btphy_rf_off(void) { }
void btphy_rf_set_freq_off(uint16_t off) { (void)off; }
void btphy_rf_set_max_ac_errors(uint8_t n) { (void)n; }
void btphy_set_bdaddr(uint64_t bdaddr) { (void)bdaddr; }
void btphy_timer_add(uint32_t instant, btphy_timer_fn_t cb, void *cb_arg,
			uint8_t anyway)
{ (void)instant; (void)cb; (void)cb_arg; (void)anyway; }
void cc2400_idle(void) { }
void hop_set_afh_map(uint8_t *map) { (void)map; }
void inquiry_state_setup(uint32_t a, uint8_t b) { (void)a; (void)b; }
void inquiry_scan_state_setup(void) { }
void paging_state_setup(uint64_t a, uint8_t b) { (void)a; (void)b; }
void paging_state_resume(void) { }
void page_scan_state_setup(void) { }
void page_scan_state_resume(void) { }
void monitor_state_setup(uint64_t a) { (void)a; }
void monitor1_state_setup(uint64_t a) { (void)a; }
void hop_cfg_afh(uint8_t *map) { (void)map; }
void watchdog_intent(unsigned i) { (void)i; }
void watchdog_stretch(void) { }
void watchdog_unstretch(void) { }
int usb_work(void) { return 0; }
void reset(void) { die("the board was reset\n"); }
void set_isp(void) { die("the board went to the bootloader\n"); }

/* ---- building a request ---- */

#define LT_ADDR		1

/* A request as the host builds it: the payload header where the format has
 * one, then the data. Each part can be made wrong on purpose.
 */
struct request {
	unsigned format;
	unsigned lt_addr;
	unsigned repeat;
	unsigned data_size;	/* what the request says it carries */
	unsigned really;	/* what the message is framed to hold */
	unsigned header_says;	/* the payload header's length, or ~0u for right */
	unsigned truncate;	/* frame the message shorter than the request */
};

static msg_t *build(const struct request *r)
{
	unsigned payload = r->really;
	unsigned size = sizeof(btctl_hdr_t) + sizeof(btctl_tx_pkt_req_t)
		+ payload;
	msg_t *msg = msg_alloc(400);
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;
	btctl_tx_pkt_req_t *req = (btctl_tx_pkt_req_t*)hdr->data;
	const bbcodec_types_t *t;
	uint8_t air_type;
	unsigned i, hlen, voice;

	memset(msg->data, 0, 400);
	hdr->type = BTCTL_TX_PKT_REQ;
	req->lt_addr = r->lt_addr;
	req->format = r->format;
	req->repeat = r->repeat;
	req->data_size = r->data_size;

	t = bbcodec_tx_format(r->format, &air_type);
	hlen = t ? t->payload_header_bytes : 0;
	voice = t ? t->voice_bytes : 0;
	if (hlen != 0 && payload >= voice + hlen)
	{
		unsigned data = r->header_says == ~0u ?
			payload - voice - hlen : r->header_says;
		unsigned word = (data << 3) | (1 << 2) | 2;

		req->data[voice] = word & 0xff;
		if (hlen == 2)
			req->data[voice + 1] = (word >> 8) & 0xff;
	}
	for (i = voice + hlen; i < payload; i++)
		req->data[i] = i & 0xff;

	msg->write = msg->data + (r->truncate ? r->truncate : size);
	return msg;
}

/* Hand the request to the device the way the host's arrives, and say whether
 * it was taken for a transmit slot.
 */
static int offered(const struct request *r, msg_t **out)
{
	msg_t *msg = build(r);
	unsigned before = queued(&btctl.pkt_tx_q);

	if (out)
		*out = msg;
	btctl_rx_enqueue(msg);
	if (btctl_work() != 1)
		die("the request was not dispatched\n");
	return queued(&btctl.pkt_tx_q) > before;
}

static void reset_state(void)
{
	memset(&btphy, 0, sizeof(btphy));
	memset(&btctl, 0, sizeof(btctl));
	memset(&freed, 0, sizeof(freed));
	freed_count = 0;
	btctl.state = BTCTL_STATE_CONNECTED;
}

/* A request that is right in every way, which each test below spoils one part
 * of. An EV3 of 17 data bytes: no payload header, so data_size is the data.
 */
static const struct request good = {
	BTCTL_FMT_EV3, LT_ADDR, 1, 17, 17, ~0u, 0
};

/* ---- the tests ---- */

MU_TEST(a_well_formed_request_is_taken)
{
	msg_t *msg;

	reset_state();
	mu_assert(offered(&good, &msg), "the request was queued");
	mu_assert(!was_freed(msg),
		"the message belongs to the queue, so it is not freed");
}

MU_TEST(a_request_outside_a_connection_is_refused)
{
	msg_t *msg;

	reset_state();
	btctl.state = BTCTL_STATE_STANDBY;
	mu_assert(!offered(&good, &msg), "there is no slot to transmit in");
	mu_assert(was_freed(msg), "and the message is handed back");
}

MU_TEST(a_message_too_short_to_hold_a_request_is_refused)
{
	struct request r = good;

	reset_state();
	/* Three bytes into the request, so every field after it is whatever
	 * the buffer held before. */
	r.truncate = sizeof(btctl_hdr_t) + 3;
	mu_assert(!offered(&r, NULL), "a short request is not believed");
}

MU_TEST(a_data_size_past_the_message_is_refused)
{
	struct request r = good;

	reset_state();
	r.data_size = 30;
	r.really = 17;
	mu_assert(!offered(&r, NULL),
		"the encoder would read past what the host sent");
}

MU_TEST(a_payload_header_that_disagrees_is_refused)
{
	struct request r = { BTCTL_FMT_DM1, LT_ADDR, 1, 10, 10, ~0u, 0 };

	reset_state();
	mu_assert(offered(&r, NULL), "the DM1 with an honest header is taken");

	reset_state();
	/* Nine data bytes sent, the header claiming sixteen. */
	r.header_says = 16;
	mu_assert(!offered(&r, NULL),
		"the header's length is checked against the bytes sent");
}

MU_TEST(a_length_the_format_will_not_carry_is_refused)
{
	struct request r = good;

	reset_state();
	r.data_size = r.really = 31;
	mu_assert(!offered(&r, NULL), "an EV3 carries 30 data bytes at most");

	reset_state();
	r = good;
	r.data_size = r.really = 0;
	mu_assert(!offered(&r, NULL), "and at least one");

	reset_state();
	r = good;
	r.data_size = r.really = 30;
	mu_assert(offered(&r, NULL), "30 is taken");
}

MU_TEST(a_transport_address_outside_a_connection_is_refused)
{
	struct request r = good;

	reset_state();
	r.lt_addr = 0;
	mu_assert(!offered(&r, NULL), "zero is the broadcast address");

	reset_state();
	r = good;
	r.lt_addr = 8;
	mu_assert(!offered(&r, NULL), "and there is no eighth transport");
}

MU_TEST(the_count_is_bounded_by_the_slots_the_run_occupies)
{
	struct request r = good;

	reset_state();
	r.repeat = 0;
	mu_assert(!offered(&r, NULL), "a run of no packets is refused");

	/* EV3 fills one slot, so it may be repeated the whole budget. */
	reset_state();
	r = good;
	r.repeat = BTCTL_TX_PKT_MAX_SLOTS;
	mu_assert(offered(&r, NULL), "400 one-slot packets are allowed");

	reset_state();
	r = good;
	r.repeat = BTCTL_TX_PKT_MAX_SLOTS + 1;
	mu_assert(!offered(&r, NULL), "401 are not");

	/* EV5 fills three, so a third as many. */
	reset_state();
	r = good;
	r.format = BTCTL_FMT_EV5;
	r.data_size = r.really = 180;
	r.repeat = BTCTL_TX_PKT_MAX_SLOTS / 3;
	mu_assert(offered(&r, NULL), "133 three-slot packets are allowed");

	reset_state();
	r.repeat = BTCTL_TX_PKT_MAX_SLOTS / 3 + 1;
	mu_assert(!offered(&r, NULL), "134 are not");
}

MU_TEST(only_one_run_waits_at_a_time)
{
	msg_t *second;

	reset_state();
	mu_assert(offered(&good, NULL), "the first run is taken");
	/* The link layer has not come for it, so it is still waiting. Another
	 * request now would add a second run's worth of time in which the
	 * board sends nothing of its own, so it is refused instead. */
	mu_assert(!offered(&good, &second),
		"a request while one is already waiting is refused");
	mu_assert(was_freed(second), "and its message is handed back");
	mu_assert_int_eq(1, (int)queued(&btctl.pkt_tx_q));

	/* Once the link layer has taken it, the next one is accepted. */
	mu_assert(msg_dequeue(&btctl.pkt_tx_q) != NULL, "the run is taken");
	mu_assert(offered(&good, NULL), "and the next request is taken too");
}

MU_TEST(an_unknown_format_is_refused)
{
	struct request r = good;

	reset_state();
	r.format = BTCTL_FMT_COUNT;
	mu_assert(!offered(&r, NULL), "the device has no such format");
}

int main(void)
{
	printf("btbr transmit request tests\n");
	MU_RUN(a_well_formed_request_is_taken);
	MU_RUN(a_request_outside_a_connection_is_refused);
	MU_RUN(a_message_too_short_to_hold_a_request_is_refused);
	MU_RUN(a_data_size_past_the_message_is_refused);
	MU_RUN(a_payload_header_that_disagrees_is_refused);
	MU_RUN(a_length_the_format_will_not_carry_is_refused);
	MU_RUN(a_transport_address_outside_a_connection_is_refused);
	MU_RUN(the_count_is_bounded_by_the_slots_the_run_occupies);
	MU_RUN(only_one_run_waits_at_a_time);
	MU_RUN(an_unknown_format_is_refused);
	return mu_report();
}
