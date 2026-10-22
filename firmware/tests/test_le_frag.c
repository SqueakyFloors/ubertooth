/* Host tests for the fixed-size USB fragmentation used by long LE packets. */
#include "minunit.h"
#include "ubertooth_le_frag.h"

#include <string.h>

static void make_fragment(usb_pkt_rx *f, uint8_t type, uint16_t total,
                          const uint8_t *source, uint16_t offset)
{
	uint16_t remaining = total - offset;
	uint16_t chunk = remaining < DMA_SIZE ? remaining : DMA_SIZE;
	memset(f, 0, sizeof(*f));
	f->pkt_type = type;
	f->reserved[0] = total & 0xff;
	f->reserved[1] = ((total >> 8) & LE_FRAG_TOTAL_HIGH_MASK) |
	                 ((offset / DMA_SIZE) << LE_FRAG_INDEX_SHIFT);
	memcpy(f->data, source + offset, chunk);
}

static void check_round_trip(uint16_t total)
{
	uint8_t source[LE_MAX_USB_PACKET_SIZE];
	le_usb_reassembly state = {0};
	usb_pkt_rx frame;
	const uint8_t *data = NULL;
	const usb_pkt_rx *metadata = NULL;
	uint16_t length = 0;
	uint16_t offset;
	int result = LE_REASSEMBLY_WAIT;

	for (offset = 0; offset < total; ++offset)
		source[offset] = (uint8_t)(offset ^ 0xa5);

	for (offset = 0; offset < total; offset += DMA_SIZE) {
		make_fragment(&frame, offset ? LE_PACKET_CONT : LE_PACKET_START,
		              total, source, offset);
		result = le_usb_reassemble(&state, &frame, &data, &length, &metadata);
		if (offset + DMA_SIZE < total)
			mu_assert_int_eq(LE_REASSEMBLY_WAIT, result);
	}

	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE, result);
	mu_assert_int_eq(total, length);
	mu_assert(memcmp(source, data, total) == 0, "reassembled bytes differ");
	mu_assert_int_eq(LE_PACKET, metadata->pkt_type);
}

MU_TEST(test_long_packet_round_trips)
{
	check_round_trip(51);
	check_round_trip(100);
	check_round_trip(260); /* valid DLE maximum */
	check_round_trip(264); /* transport maximum / full 8-bit length field */
}

MU_TEST(test_legacy_packet_completes_immediately)
{
	le_usb_reassembly state = {0};
	usb_pkt_rx frame = {0};
	const uint8_t *data;
	const usb_pkt_rx *metadata;
	uint16_t length;
	frame.pkt_type = LE_PACKET;
	frame.data[49] = 0x5a;
	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE,
	                 le_usb_reassemble(&state, &frame, &data, &length, &metadata));
	mu_assert_int_eq(DMA_SIZE, length);
	mu_assert_int_eq(0x5a, data[49]);
	mu_assert(metadata == &frame, "legacy metadata should be original frame");
}

MU_TEST(test_bad_or_partial_chains_are_discarded)
{
	le_usb_reassembly state = {0};
	usb_pkt_rx frame = {0};
	uint8_t source[100] = {0};
	const uint8_t *data;
	const usb_pkt_rx *metadata;
	uint16_t length;

	make_fragment(&frame, LE_PACKET_CONT, 100, source, 50);
	mu_assert_int_eq(LE_REASSEMBLY_ERROR,
	                 le_usb_reassemble(&state, &frame, &data, &length, &metadata));

	make_fragment(&frame, LE_PACKET_START, 100, source, 0);
	mu_assert_int_eq(LE_REASSEMBLY_WAIT,
	                 le_usb_reassemble(&state, &frame, &data, &length, &metadata));
	make_fragment(&frame, LE_PACKET_CONT, 99, source, 50); /* total mismatch */
	mu_assert_int_eq(LE_REASSEMBLY_ERROR,
	                 le_usb_reassemble(&state, &frame, &data, &length, &metadata));

	/* Skip fragment index 1: index 2 must be rejected, not spliced. */
	{
		uint8_t longer[150] = {0};
		make_fragment(&frame, LE_PACKET_START, 150, longer, 0);
		mu_assert_int_eq(LE_REASSEMBLY_WAIT,
		                 le_usb_reassemble(&state, &frame, &data, &length, &metadata));
		make_fragment(&frame, LE_PACKET_CONT, 150, longer, 100);
		mu_assert_int_eq(LE_REASSEMBLY_ERROR,
		                 le_usb_reassemble(&state, &frame, &data, &length, &metadata));
	}
}

MU_TEST(test_two_devices_reassemble_independently)
{
	le_usb_reassembly a = {0}, b = {0};
	usb_pkt_rx frame;
	uint8_t source_a[51] = {1}, source_b[51] = {2};
	const uint8_t *data;
	const usb_pkt_rx *metadata;
	uint16_t length;

	make_fragment(&frame, LE_PACKET_START, 51, source_a, 0);
	mu_assert_int_eq(LE_REASSEMBLY_WAIT,
	                 le_usb_reassemble(&a, &frame, &data, &length, &metadata));
	make_fragment(&frame, LE_PACKET_START, 51, source_b, 0);
	mu_assert_int_eq(LE_REASSEMBLY_WAIT,
	                 le_usb_reassemble(&b, &frame, &data, &length, &metadata));
	make_fragment(&frame, LE_PACKET_CONT, 51, source_a, 50);
	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE,
	                 le_usb_reassemble(&a, &frame, &data, &length, &metadata));
	mu_assert_int_eq(1, data[0]);
	make_fragment(&frame, LE_PACKET_CONT, 51, source_b, 50);
	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE,
	                 le_usb_reassemble(&b, &frame, &data, &length, &metadata));
	mu_assert_int_eq(2, data[0]);
}

int main(void)
{
	printf("LE USB fragmentation tests\n");
	MU_RUN(test_long_packet_round_trips);
	MU_RUN(test_legacy_packet_completes_immediately);
	MU_RUN(test_bad_or_partial_chains_are_discarded);
	MU_RUN(test_two_devices_reassemble_independently);
	return mu_report();
}
