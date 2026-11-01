/* Which message the link layer puts in a transmit slot, and when it lets go
 * of it.
 *
 * Three messages compete for a slot: a packet the host composed in full
 * (BTCTL_TX_PKT_REQ), an ACL message the link is carrying, and a POLL when
 * there is neither. Every packet carries the link's SEQN, and the peer's next
 * ARQN acknowledges whichever one it last read, so only one of them may be
 * outstanding at a time. These tests pin that order, and pin that a run of a
 * chosen packet holds its message for exactly the slots it was given and then
 * frees it.
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
#include <ubtbr/ll.h>

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

void *mem_pool_alloc(unsigned size) { return malloc(size); }

void btctl_set_state(btctl_state_t state, btctl_reason_t reason)
{
	(void)reason;
	btctl.state = state;
}

/* The board's console, which these tests do not read. */
void cprintf(char *fmt, ...) { (void)fmt; }

/* The message queues, kept here so the real btctl_*_tx_enqueue() and
 * _dequeue() inlines work and the link layer takes its messages the way the
 * firmware does. One queue holds a handful, which is all a test needs.
 */
#define QUEUE_MAX	8
#define QUEUE_COUNT	4
static struct {
	const msg_queue_t *q;
	msg_t *msg[QUEUE_MAX];
	unsigned head, tail;
} queues[QUEUE_COUNT];

static unsigned queue_index(const msg_queue_t *q)
{
	unsigned i;

	for (i = 0; i < QUEUE_COUNT; i++)
	{
		if (queues[i].q == q)
			return i;
		if (queues[i].q == NULL)
		{
			queues[i].q = q;
			return i;
		}
	}
	die("more queues than this test has room for\n");
}

void msg_queue_init(msg_queue_t *q) { (void)queue_index(q); }

int msg_enqueue(msg_queue_t *q, msg_t *p)
{
	unsigned i = queue_index(q);

	if (queues[i].tail - queues[i].head >= QUEUE_MAX)
		return -1;
	queues[i].msg[queues[i].tail++ % QUEUE_MAX] = p;
	return 0;
}

msg_t *msg_dequeue(msg_queue_t *q)
{
	unsigned i = queue_index(q);

	if (queues[i].head == queues[i].tail)
		return NULL;
	return queues[i].msg[queues[i].head++ % QUEUE_MAX];
}

static unsigned queue_depth(const msg_queue_t *q)
{
	unsigned i = queue_index(q);

	return queues[i].tail - queues[i].head;
}

/* Which messages have been freed, so a test can tell a message the link layer
 * is still holding from one it has let go of. */
#define FREED_MAX	16
static msg_t *freed[FREED_MAX];
static unsigned freed_count;

static int was_freed(const msg_t *msg)
{
	unsigned i;

	for (i = 0; i < freed_count; i++)
		if (freed[i] == msg)
			return 1;
	return 0;
}

/* msg_free() is an inline that calls this, so the fake goes here. The message
 * itself is left allocated: a test compares addresses after the free, which
 * reading freed memory would not allow.
 */
void btctl_mem_free(void *p)
{
	if (freed_count < FREED_MAX)
		freed[freed_count++] = (msg_t*)p;
}

/* The air layout of a chosen format. One slot unless a test asks for more:
 * the slot count is what the request's repeat is spent in.
 */
static bbcodec_types_t chosen_format = {
	1 /* nslots */, 0 /* payload_header_bytes */, 30 /* payload_bytes */,
	BB_FEC_NONE /* fec */, 1 /* has_crc */, 0 /* voice_bytes */
};
static int format_is_known = 1;

const bbcodec_types_t *bbcodec_tx_format(unsigned format, uint8_t *air_type)
{
	(void)format;
	if (!format_is_known)
		return NULL;
	*air_type = BB_TYPE_DH1;
	return &chosen_format;
}

/* ---- building the two kinds of message ---- */

#define LT_ADDR		1
#define PKT_DATA_BYTES	30

static msg_t *pkt_message(unsigned repeat)
{
	unsigned size = sizeof(btctl_hdr_t) + sizeof(btctl_tx_pkt_req_t)
		+ PKT_DATA_BYTES;
	msg_t *msg = msg_alloc(size);
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;
	btctl_tx_pkt_req_t *req = (btctl_tx_pkt_req_t*)hdr->data;

	memset(msg->data, 0, size);
	hdr->type = BTCTL_TX_PKT_REQ;
	req->lt_addr = LT_ADDR;
	req->format = BTCTL_FMT_EV3;
	req->repeat = repeat;
	req->data_size = PKT_DATA_BYTES;
	msg->write = msg->data + size;
	return msg;
}

static msg_t *acl_message(void)
{
	unsigned size = sizeof(btctl_hdr_t) + sizeof(btctl_tx_pkt_t) + 17;
	msg_t *msg = msg_alloc(size);
	btctl_hdr_t *hdr = (btctl_hdr_t*)msg->data;

	memset(msg->data, 0, size);
	hdr->type = BTCTL_TX_ACL_REQ;
	msg->write = msg->data + size;
	return msg;
}

static link_layer_t ll;

static void reset_state(unsigned nslots)
{
	memset(&btphy, 0, sizeof(btphy));
	memset(&btctl, 0, sizeof(btctl));
	memset(&freed, 0, sizeof(freed));
	memset(&queues, 0, sizeof(queues));
	freed_count = 0;
	format_is_known = 1;
	chosen_format.nslots = nslots;
	ll_init(&ll, 1 /* central */, LT_ADDR);
}

/* ---- the tests ---- */

MU_TEST(a_chosen_packet_takes_the_slot_when_the_link_is_idle)
{
	ll_tx_t tx;
	msg_t *msg;

	reset_state(1);
	msg = pkt_message(1);
	btctl_pkt_tx_enqueue(msg);

	ll_prepare_tx(&ll, &tx);

	mu_assert(tx.fmt == &chosen_format,
		"tx.fmt == &chosen_format");
	mu_assert(ll.cur_pkt_msg == msg,
		"the link layer holds the chosen packet's message");
	mu_assert(ll.cur_tx_msg == NULL, "no ACL message was taken");
}

MU_TEST(a_poll_goes_out_when_nothing_is_queued)
{
	ll_tx_t tx;

	reset_state(1);

	ll_prepare_tx(&ll, &tx);

	mu_assert(tx.fmt == NULL, "a POLL has no chosen format");
	mu_assert(tx.hdr->type == BB_TYPE_POLL,
		"tx.hdr->type == BB_TYPE_POLL");
	mu_assert(tx.data == NULL, "a POLL carries no payload");
}

MU_TEST(a_chosen_packet_waits_while_an_acl_is_unacknowledged)
{
	ll_tx_t tx;
	msg_t *pkt, *acl;

	reset_state(1);
	acl = acl_message();
	btctl_acl_tx_enqueue(acl);

	/* The ACL message takes the first slot and is held for the peer's
	 * ARQN, so it is still outstanding in the next one. */
	ll_prepare_tx(&ll, &tx);
	mu_assert(ll.cur_tx_msg == acl, "the ACL message took the first slot");

	pkt = pkt_message(1);
	btctl_pkt_tx_enqueue(pkt);
	ll_prepare_tx(&ll, &tx);

	mu_assert(tx.fmt == NULL,
		"the chosen packet waited for the ACL to be acknowledged");
	mu_assert(ll.cur_pkt_msg == NULL,
		"the chosen packet was not even dequeued");
	mu_assert(queue_depth(&btctl.pkt_tx_q) == 1,
		"it is still in its queue");
}

MU_TEST(a_chosen_packet_carries_the_links_arqn_and_seqn)
{
	ll_tx_t tx;

	reset_state(1);
	btctl_pkt_tx_enqueue(pkt_message(1));
	ll.loc_seqn = 1;
	ll.loc_arqn = 1;

	ll_prepare_tx(&ll, &tx);

	mu_assert(tx.hdr->flags & (1<<BTHDR_SEQN),
		"tx.hdr->flags & (1<<BTHDR_SEQN)");
	mu_assert(tx.hdr->flags & (1<<BTHDR_ARQN),
		"tx.hdr->flags & (1<<BTHDR_ARQN)");
	mu_assert(tx.hdr->flags & (1<<BTHDR_FLOW),
		"tx.hdr->flags & (1<<BTHDR_FLOW)");
	mu_assert(ll.loc_arqn == 0, "the acknowledgment was spent on it");
}

MU_TEST(a_run_holds_its_message_for_every_slot_it_was_given)
{
	ll_tx_t tx;
	msg_t *msg;
	unsigned slot;

	reset_state(1);
	msg = pkt_message(3);
	btctl_pkt_tx_enqueue(msg);

	for (slot = 0; slot < 3; slot++)
	{
		char where[64];

		ll_prepare_tx(&ll, &tx);
		snprintf(where, sizeof(where), "slot %u took the packet", slot);
		mu_assert(tx.fmt == &chosen_format, where);
		snprintf(where, sizeof(where), "slot %u still holds it", slot);
		mu_assert(!was_freed(msg), where);
	}

	/* The slot after the last one: the packet has been transmitted, so the
	 * message is freed and the next slot has nothing to send. */
	ll_prepare_tx(&ll, &tx);
	mu_assert(was_freed(msg), "the message is freed after the last slot");
	mu_assert(ll.cur_pkt_msg == NULL, "and no longer held");
	mu_assert(tx.hdr->type == BB_TYPE_POLL,
		"tx.hdr->type == BB_TYPE_POLL");
}

MU_TEST(an_acl_message_goes_out_once_the_run_has_finished)
{
	ll_tx_t tx;
	msg_t *acl;

	reset_state(1);
	btctl_pkt_tx_enqueue(pkt_message(1));
	acl = acl_message();
	btctl_acl_tx_enqueue(acl);

	ll_prepare_tx(&ll, &tx);
	mu_assert(tx.fmt == &chosen_format, "the chosen packet went first");
	mu_assert(queue_depth(&btctl.acl_tx_q) == 1,
		"the ACL message waited its turn");

	ll_prepare_tx(&ll, &tx);
	mu_assert(ll.cur_tx_msg == acl, "then took the next slot");
	mu_assert(tx.fmt == NULL, "as an ACL message, with no chosen format");
}

MU_TEST(a_multi_slot_format_is_told_its_own_data_length)
{
	ll_tx_t tx;

	reset_state(5);
	btctl_pkt_tx_enqueue(pkt_message(1));

	ll_prepare_tx(&ll, &tx);

	/* A format with no payload header carries no length of its own, so the
	 * link layer has to pass the one the host supplied. */
	mu_assert(tx.data_bytes == PKT_DATA_BYTES,
		"tx.data_bytes == PKT_DATA_BYTES");
}

MU_TEST(a_format_the_device_does_not_know_drops_the_packet)
{
	ll_tx_t tx;
	msg_t *msg;

	reset_state(1);
	msg = pkt_message(1);
	btctl_pkt_tx_enqueue(msg);
	format_is_known = 0;

	ll_prepare_tx(&ll, &tx);

	mu_assert(was_freed(msg), "the message is freed rather than held");
	mu_assert(ll.cur_pkt_msg == NULL, "and nothing is left holding it");
	mu_assert(tx.fmt == NULL, "nothing goes out with no format");
}

MU_TEST(a_reset_frees_both_held_messages)
{
	ll_tx_t tx;
	msg_t *pkt, *acl;

	reset_state(1);
	pkt = pkt_message(4);
	btctl_pkt_tx_enqueue(pkt);
	ll_prepare_tx(&ll, &tx);
	mu_assert(ll.cur_pkt_msg == pkt, "a run is under way");

	/* The run still has slots left, so the message is held; an ACL message
	 * can only be held alongside it once the run has finished. */
	ll_reset(&ll);
	mu_assert(was_freed(pkt), "the held chosen packet was freed");
	mu_assert(ll.pkt_slots_left == 0,
		"ll.pkt_slots_left == 0");

	reset_state(1);
	acl = acl_message();
	btctl_acl_tx_enqueue(acl);
	ll_prepare_tx(&ll, &tx);
	ll_reset(&ll);
	mu_assert(was_freed(acl), "the held ACL message was freed");
}

int main(void)
{
	printf("btbr transmit queue tests\n");
	MU_RUN(a_chosen_packet_takes_the_slot_when_the_link_is_idle);
	MU_RUN(a_poll_goes_out_when_nothing_is_queued);
	MU_RUN(a_chosen_packet_waits_while_an_acl_is_unacknowledged);
	MU_RUN(a_chosen_packet_carries_the_links_arqn_and_seqn);
	MU_RUN(a_run_holds_its_message_for_every_slot_it_was_given);
	MU_RUN(an_acl_message_goes_out_once_the_run_has_finished);
	MU_RUN(a_multi_slot_format_is_told_its_own_data_length);
	MU_RUN(a_format_the_device_does_not_know_drops_the_packet);
	MU_RUN(a_reset_frees_both_held_messages);
	return mu_report();
}
