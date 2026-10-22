/*
 * What the low-energy callback hands the decoder, libbtbb's
 * lell_allocate_and_decode_n().
 *
 * The decoder is told how many bytes were captured and reads no further than
 * that. It has to be told the truth. An earlier decoder took a bare pointer
 * and always copied 64 bytes out of it, while a received record holds only
 * 50, so a PDU whose length field claimed more than was captured showed 14
 * bytes of whatever followed the packet in memory as packet data.
 *
 * This test runs the real cb_btle(), by compiling ubertooth_callback.c into
 * itself, with the ring buffer and the decoder replaced by the fakes below.
 * The fake decoder copies exactly the number of bytes it is told, so a caller
 * that overstates the length fails here in a plain build as well as under a
 * sanitizer.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
#include "../../libubertooth/src/ubertooth_callback.c"

/* A filler byte that no crafted packet here uses. Any of it reaching the
 * decoder must have come from past the end of the packet. */
#define STACK_PATTERN 0xab

/* ---- fakes -------------------------------------------------------------- */

/* The records the faked capture delivers, in order, and what the decoder was
 * handed on the last call. */
static usb_pkt_rx next_packets[8];
static unsigned next_count;
static unsigned next_index;
static uint8_t handed[LE_MAX_USB_PACKET_SIZE];
static size_t handed_len;
static int decodes;

fifo_t* fifo_init(void) { return (fifo_t*)malloc(1); }
void fifo_inc_write_ptr(fifo_t* fifo) { (void)fifo; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet) { (void)fifo; (void)packet; }
usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return NULL; }
uint8_t fifo_empty(fifo_t* fifo) { (void)fifo; return next_index >= next_count; }

usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	(void)fifo;
	if (next_index < next_count)
		return next_packets[next_index++];
	return next_packets[0];
}

/* Stands in for the decoder. It copies exactly the bytes it is told were
 * captured, which is what makes an overstated length fail here. The packet it
 * hands back is a real but empty libbtbb packet, because the rest of
 * cb_btle() goes on to use it. What the real decoder makes of the bytes is
 * libbtbb's business; this test only checks which bytes it was given. */
void lell_allocate_and_decode_n(const uint8_t* stream, size_t stream_len,
                                uint16_t phys_channel, uint32_t clk100ns,
                                lell_packet** pkt)
{
	(void)phys_channel;
	(void)clk100ns;
	handed_len = stream_len;
	if (stream_len > sizeof(handed))
		stream_len = sizeof(handed);
	memcpy(handed, stream, stream_len);
	decodes++;
	*pkt = lell_packet_new();
}

/* ---- helpers ------------------------------------------------------------ */

/* A BLE advertising packet with every captured byte set non-zero. Any byte
 * the decoder reads is then either one of these or came from past the
 * packet. */
static void fill_packet(usb_pkt_rx* rx)
{
	unsigned i;

	memset(rx, 0, sizeof(*rx));
	rx->pkt_type = LE_PACKET;
	rx->channel = 0;
	rx->clk100ns = htole32(1234);
	for (i = 0; i < DMA_SIZE; i++)
		rx->data[i] = (uint8_t)(i + 1);
	/* The advertising access address, little endian, then a PDU header
	 * whose length field claims 58 bytes while only 50 were captured. */
	rx->data[0] = 0xd6;
	rx->data[1] = 0xbe;
	rx->data[2] = 0x89;
	rx->data[3] = 0x8e;
	rx->data[4] = 0x00;
	rx->data[5] = 58;
}

/* Written below, so the compiler cannot drop the fill as dead code. */
static volatile uint8_t sink;

/* Writes the filler byte over the stack the callback is about to use. A read
 * past the end of the packet is then unlikely to find zeros there and pass by
 * luck.
 *
 * This is best effort. The callback's own locals overwrite most of what is
 * left here, so which bytes survive depends on the stack frame the compiler
 * chose. The guarantees come from elsewhere: the fake decoder copies only the
 * length it is told, and a sanitizer build reports a longer read as a
 * buffer overflow whatever the bytes happen to be. */
static void dirty_the_stack(void)
{
	uint8_t scratch[4096];

	memset(scratch, STACK_PATTERN, sizeof(scratch));
	sink = scratch[0];
}

/* Runs the queued records through the real callback, one call each. The
 * callback prints the packet, so its output goes to /dev/null to keep it out
 * of the test's own, and the bytes the decoder was given are left in
 * handed[]. */
static void run_capture(void)
{
	ubertooth_t ut;
	int saved;
	int devnull;

	memset(&ut, 0, sizeof(ut));
	ut.fifo = fifo_init();
	ut.clk100ns_period = CLK100NS_TIMER1_PERIOD;

	memset(handed, STACK_PATTERN, sizeof(handed));
	handed_len = 0;
	decodes = 0;
	next_index = 0;

	fflush(stdout);
	saved = dup(fileno(stdout));
	devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, fileno(stdout));
		close(devnull);
	}

	dirty_the_stack();
	while (next_index < next_count)
		cb_btle(&ut, NULL);

	fflush(stdout);
	dup2(saved, fileno(stdout));
	close(saved);

	free(ut.fifo);
}

/* ---- cases -------------------------------------------------------------- */

MU_TEST(a_single_record_decodes_only_the_bytes_it_holds)
{
	fill_packet(&next_packets[0]);
	next_count = 1;
	run_capture();

	mu_assert_int_eq(1, decodes);
	/* The 50 bytes the record holds, and not one more: the PDU header
	 * claims 58, which is the case that used to read past the packet. */
	mu_assert_int_eq(DMA_SIZE, (int)handed_len);
	mu_assert(memcmp(handed, next_packets[0].data, DMA_SIZE) == 0,
	          "the decoder was given something other than the packet");
}

MU_TEST(a_long_packet_decodes_once_with_every_byte)
{
	const uint16_t total = 120;
	uint8_t packet[120];
	unsigned i;

	for (i = 0; i < total; i++)
		packet[i] = (uint8_t)(i ^ 0x3c);
	/* An access address that is not the advertising one, so the decoder
	 * sees a data-channel packet, and a header claiming the whole payload. */
	packet[0] = 0x11;
	packet[1] = 0x22;
	packet[2] = 0x33;
	packet[3] = 0x44;
	packet[4] = 0x01;
	packet[5] = (uint8_t)(total - 9);

	for (i = 0; i * DMA_SIZE < total; i++) {
		usb_pkt_rx* f = &next_packets[i];
		uint16_t offset = (uint16_t)(i * DMA_SIZE);
		uint16_t remaining = (uint16_t)(total - offset);
		uint16_t chunk = remaining < DMA_SIZE ? remaining : DMA_SIZE;

		memset(f, 0, sizeof(*f));
		f->pkt_type = offset ? LE_PACKET_CONT : LE_PACKET_START;
		f->channel = 4;
		f->clk100ns = htole32(1234);
		f->reserved[0] = total & 0xff;
		f->reserved[1] = (uint8_t)(((total >> 8) & LE_FRAG_TOTAL_HIGH_MASK) |
		                           (i << LE_FRAG_INDEX_SHIFT));
		memcpy(f->data, packet + offset, chunk);
	}
	next_count = i;
	run_capture();

	/* One packet, so one decode, however many records carried it. */
	mu_assert_int_eq(1, decodes);
	mu_assert_int_eq(total, (int)handed_len);
	mu_assert(memcmp(handed, packet, total) == 0,
	          "the decoder was given something other than the packet");
}

MU_TEST(a_chain_with_a_record_missing_decodes_nothing)
{
	const uint16_t total = 150;
	uint8_t packet[150];
	unsigned i;

	memset(packet, 0x7e, sizeof(packet));
	for (i = 0; i < 2; i++) {
		usb_pkt_rx* f = &next_packets[i];
		/* The first record, then the third: the middle one is lost. */
		uint16_t offset = (uint16_t)(i ? 100 : 0);

		memset(f, 0, sizeof(*f));
		f->pkt_type = offset ? LE_PACKET_CONT : LE_PACKET_START;
		f->channel = 4;
		f->clk100ns = htole32(1234);
		f->reserved[0] = total & 0xff;
		f->reserved[1] = (uint8_t)(((total >> 8) & LE_FRAG_TOTAL_HIGH_MASK) |
		                           ((offset / DMA_SIZE) << LE_FRAG_INDEX_SHIFT));
		memcpy(f->data, packet + offset, DMA_SIZE);
	}
	next_count = 2;
	run_capture();

	mu_assert_int_eq(0, decodes);
}

int main(void)
{
	MU_RUN(a_single_record_decodes_only_the_bytes_it_holds);
	MU_RUN(a_long_packet_decodes_once_with_every_byte);
	MU_RUN(a_chain_with_a_record_missing_decodes_nothing);
	return mu_report();
}
