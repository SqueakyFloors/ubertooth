/*
 * Putting a long low-energy packet back together from the records it arrives
 * in (ubertooth_le_frag.c).
 *
 * A record carries 50 bytes of packet, so anything longer comes as a chain:
 * one first record, then continuations in order. What matters here is that a
 * complete chain gives back exactly the bytes that were captured, and that a
 * chain with a record missing gives back nothing rather than a packet with a
 * hole in it.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth_le_frag.h"

/* The longest valid packet: access address (4), PDU header (2), the 251-byte
 * payload Data Length Extension allows, and the CRC (3). */
#define LE_MAX_VALID_PACKET (4 + 2 + 251 + 3)

/* Most cases here are about the bytes of a packet rather than its timing, so
 * they go through this: it passes an arrival reading of zero and keeps the
 * one reported back out of the way. The cases that are about the arrival
 * reading call le_usb_reassemble() themselves. */
static int reassemble(le_usb_reassembly *state, const usb_pkt_rx *frame,
                      const uint8_t **data, uint16_t *length,
                      const usb_pkt_rx **metadata)
{
	uint64_t ignored_arrival_ns;

	return le_usb_reassemble(state, frame, 0, data, length, metadata,
	                         &ignored_arrival_ns);
}

/* A packet of `total` bytes, every byte differing from its neighbours so a
 * chunk copied to the wrong place shows up. */
static void fill_packet(uint8_t *packet, uint16_t total)
{
	uint16_t i;

	for (i = 0; i < total; i++)
		packet[i] = (uint8_t) (i ^ 0xa5);
}

/* The record a board sends for the chunk of `packet` that starts at `offset`,
 * in a chain describing a packet of `total` bytes. */
static void make_fragment(usb_pkt_rx *f, uint16_t total, const uint8_t *packet,
                          uint16_t offset)
{
	uint16_t remaining = total - offset;
	uint16_t chunk = remaining < DMA_SIZE ? remaining : DMA_SIZE;

	memset(f, 0, sizeof(*f));
	f->pkt_type = offset ? LE_PACKET_CONT : LE_PACKET_START;
	f->channel = 4;
	f->clk100ns = 1234;
	f->reserved[0] = total & 0xff;
	f->reserved[1] = ((total >> 8) & LE_FRAG_TOTAL_HIGH_MASK) |
	                 ((offset / DMA_SIZE) << LE_FRAG_INDEX_SHIFT);
	memcpy(f->data, packet + offset, chunk);
}

/* Feeds one whole chain in and checks the packet that comes back. */
static void check_round_trip(uint16_t total)
{
	uint8_t packet[LE_MAX_USB_PACKET_SIZE];
	le_usb_reassembly state;
	usb_pkt_rx frame;
	const uint8_t *data = NULL;
	const usb_pkt_rx *metadata = NULL;
	uint16_t length = 0;
	uint16_t offset;
	int result = LE_REASSEMBLY_WAIT;

	memset(&state, 0, sizeof(state));
	fill_packet(packet, total);

	for (offset = 0; offset < total; offset += DMA_SIZE) {
		make_fragment(&frame, total, packet, offset);
		result = reassemble(&state, &frame, &data, &length, &metadata);
		/* Nothing comes out until the last record of the chain. */
		if (offset + (uint16_t) DMA_SIZE < total)
			mu_assert_int_eq(LE_REASSEMBLY_WAIT, result);
	}

	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE, result);
	mu_assert_int_eq(total, length);
	mu_assert(memcmp(packet, data, total) == 0,
	          "the packet handed back is not the one captured");
	/* It is handed on as an ordinary record, so nothing downstream has to
	 * know it arrived in pieces, and it keeps the first record's readings. */
	mu_assert_int_eq(LE_PACKET, metadata->pkt_type);
	mu_assert_int_eq(4, metadata->channel);
	mu_assert_int_eq(1234, (int) metadata->clk100ns);
}

/* One record holds a packet of 50 bytes or less, and such a record says
 * nothing about fragments. */
MU_TEST(a_short_packet_arrives_in_one_record)
{
	le_usb_reassembly state;
	usb_pkt_rx frame;
	const uint8_t *data = NULL;
	const usb_pkt_rx *metadata = NULL;
	uint16_t length = 0;

	memset(&state, 0, sizeof(state));
	memset(&frame, 0, sizeof(frame));
	frame.pkt_type = LE_PACKET;
	frame.data[49] = 0x5a;

	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));
	mu_assert_int_eq(DMA_SIZE, length);
	mu_assert_int_eq(0x5a, data[49]);
	mu_assert(metadata == &frame,
	          "a single record describes itself, so it is handed back as it is");
}

MU_TEST(a_long_packet_is_put_back_together)
{
	check_round_trip(51);                  /* one byte over a single record */
	check_round_trip(100);                 /* two full records */
	check_round_trip(LE_MAX_VALID_PACKET); /* the 251-byte payload maximum */
	check_round_trip(LE_MAX_USB_PACKET_SIZE); /* a length field claiming 255 */
}

MU_TEST(a_chain_with_a_record_missing_gives_back_nothing)
{
	uint8_t packet[LE_MAX_USB_PACKET_SIZE];
	le_usb_reassembly state;
	usb_pkt_rx frame;
	const uint8_t *data = NULL;
	const usb_pkt_rx *metadata = NULL;
	uint16_t length = 0;

	memset(&state, 0, sizeof(state));
	fill_packet(packet, 150);

	make_fragment(&frame, 150, packet, 0);
	mu_assert_int_eq(LE_REASSEMBLY_WAIT,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));

	/* The middle record never arrives. The third must be refused, not
	 * spliced onto the first, which would leave 50 bytes of the packet
	 * holding the wrong payload. */
	make_fragment(&frame, 150, packet, 100);
	mu_assert_int_eq(LE_REASSEMBLY_ERROR,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));
	mu_assert(data == NULL, "a refused record hands back no packet");
	mu_assert_int_eq(0, length);

	/* The broken chain is gone, so its remaining records cannot complete
	 * it either. */
	make_fragment(&frame, 150, packet, 50);
	mu_assert_int_eq(LE_REASSEMBLY_ERROR,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));

	/* The next whole packet still decodes. */
	check_round_trip(100);
}

MU_TEST(a_record_that_cannot_belong_to_a_packet_is_refused)
{
	uint8_t packet[LE_MAX_USB_PACKET_SIZE];
	le_usb_reassembly state;
	usb_pkt_rx frame;
	const uint8_t *data = NULL;
	const usb_pkt_rx *metadata = NULL;
	uint16_t length = 0;

	memset(&state, 0, sizeof(state));
	fill_packet(packet, 100);

	/* A continuation with no chain started. */
	make_fragment(&frame, 100, packet, 50);
	mu_assert_int_eq(LE_REASSEMBLY_ERROR,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));

	/* A chain whose records disagree about how long the packet is. */
	make_fragment(&frame, 100, packet, 0);
	mu_assert_int_eq(LE_REASSEMBLY_WAIT,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));
	make_fragment(&frame, 99, packet, 50);
	mu_assert_int_eq(LE_REASSEMBLY_ERROR,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));

	/* A chain claiming a length no packet has: 50 bytes or less travels in
	 * one record, and nothing is longer than the transport's maximum. */
	make_fragment(&frame, 40, packet, 0);
	mu_assert_int_eq(LE_REASSEMBLY_ERROR,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));
	make_fragment(&frame, 100, packet, 0);
	frame.reserved[0] = (LE_MAX_USB_PACKET_SIZE + 1) & 0xff;
	frame.reserved[1] = (((LE_MAX_USB_PACKET_SIZE + 1) >> 8) &
	                     LE_FRAG_TOTAL_HIGH_MASK);
	mu_assert_int_eq(LE_REASSEMBLY_ERROR,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));

	/* A record type that is not a low-energy packet at all. */
	memset(&frame, 0, sizeof(frame));
	frame.pkt_type = BR_PACKET;
	mu_assert_int_eq(LE_REASSEMBLY_ERROR,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));
}

/* Two boards captured at once each have their own state, so their chains
 * cannot be spliced into each other's packets. */
MU_TEST(two_boards_do_not_mix_their_packets)
{
	uint8_t first[LE_MAX_USB_PACKET_SIZE];
	uint8_t second[LE_MAX_USB_PACKET_SIZE];
	le_usb_reassembly a, b;
	usb_pkt_rx frame;
	const uint8_t *data = NULL;
	const usb_pkt_rx *metadata = NULL;
	uint16_t length = 0;
	uint16_t i;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	for (i = 0; i < 51; i++) {
		first[i] = 0x11;
		second[i] = 0x22;
	}

	make_fragment(&frame, 51, first, 0);
	mu_assert_int_eq(LE_REASSEMBLY_WAIT,
	                 reassemble(&a, &frame, &data, &length, &metadata));
	make_fragment(&frame, 51, second, 0);
	mu_assert_int_eq(LE_REASSEMBLY_WAIT,
	                 reassemble(&b, &frame, &data, &length, &metadata));

	make_fragment(&frame, 51, first, 50);
	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE,
	                 reassemble(&a, &frame, &data, &length, &metadata));
	mu_assert_int_eq(0x11, data[0]);

	make_fragment(&frame, 51, second, 50);
	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE,
	                 reassemble(&b, &frame, &data, &length, &metadata));
	mu_assert_int_eq(0x22, data[0]);
}

/* A capture that ends part-way through a long packet must not leave the
 * handle able to finish it with the next capture's records. */
MU_TEST(a_reset_forgets_a_chain_in_progress)
{
	uint8_t packet[LE_MAX_USB_PACKET_SIZE];
	le_usb_reassembly state;
	usb_pkt_rx frame;
	const uint8_t *data = NULL;
	const usb_pkt_rx *metadata = NULL;
	uint16_t length = 0;

	memset(&state, 0, sizeof(state));
	fill_packet(packet, 100);

	make_fragment(&frame, 100, packet, 0);
	mu_assert_int_eq(LE_REASSEMBLY_WAIT,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));

	le_usb_reassembly_reset(&state);

	make_fragment(&frame, 100, packet, 50);
	mu_assert_int_eq(LE_REASSEMBLY_ERROR,
	                 reassemble(&state, &frame, &data, &length,
	                            &metadata));
}

/* The packet's timestamp is the first record's, so the arrival reading handed
 * back has to be the first record's too: the clock tracking compares the two
 * against each other, and readings from different records would not describe
 * the same moment. */
MU_TEST(a_chain_reports_the_arrival_of_the_record_it_started_with)
{
	uint8_t packet[LE_MAX_USB_PACKET_SIZE];
	le_usb_reassembly state;
	usb_pkt_rx frame;
	const uint8_t *data = NULL;
	const usb_pkt_rx *metadata = NULL;
	uint16_t length = 0;
	uint64_t arrival_ns = 0;

	memset(&state, 0, sizeof(state));
	fill_packet(packet, 100);

	/* Two records, arriving a millisecond apart. */
	make_fragment(&frame, 100, packet, 0);
	mu_assert_int_eq(LE_REASSEMBLY_WAIT,
	                 le_usb_reassemble(&state, &frame, 7000000, &data,
	                                   &length, &metadata, &arrival_ns));

	make_fragment(&frame, 100, packet, 50);
	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE,
	                 le_usb_reassemble(&state, &frame, 8000000, &data,
	                                   &length, &metadata, &arrival_ns));
	mu_assert(arrival_ns == 7000000,
	          "a chain's arrival reading is its first record's");
}

/* A whole packet in one record describes itself, so the reading handed back
 * is the one passed in. */
MU_TEST(a_single_record_reports_its_own_arrival)
{
	le_usb_reassembly state;
	usb_pkt_rx frame;
	const uint8_t *data = NULL;
	const usb_pkt_rx *metadata = NULL;
	uint16_t length = 0;
	uint64_t arrival_ns = 0;

	memset(&state, 0, sizeof(state));
	memset(&frame, 0, sizeof(frame));
	frame.pkt_type = LE_PACKET;

	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE,
	                 le_usb_reassemble(&state, &frame, 4200000000ull, &data,
	                                   &length, &metadata, &arrival_ns));
	mu_assert(arrival_ns == 4200000000ull,
	          "a single record's arrival reading is the one it came with");
}

int main(void)
{
	MU_RUN(a_short_packet_arrives_in_one_record);
	MU_RUN(a_long_packet_is_put_back_together);
	MU_RUN(a_chain_with_a_record_missing_gives_back_nothing);
	MU_RUN(a_record_that_cannot_belong_to_a_packet_is_refused);
	MU_RUN(two_boards_do_not_mix_their_packets);
	MU_RUN(a_reset_forgets_a_chain_in_progress);
	MU_RUN(a_chain_reports_the_arrival_of_the_record_it_started_with);
	MU_RUN(a_single_record_reports_its_own_arrival);
	return mu_report();
}
