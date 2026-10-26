/*
 * What the low-energy callback hands the decoder, libbtbb's
 * lell_allocate_and_decode().
 *
 * The decoder takes a bare pointer with no length and always copies 64 bytes
 * out of it. A received packet holds only 50. The callback used to hand it
 * the packet's own array, so it copied 14 bytes of whatever followed the
 * packet in memory. The decoder reads those 14 bytes for the tail of the
 * payload and for the CRC, so a PDU whose length field claimed more than was
 * captured showed that leftover memory as packet data.
 *
 * This test runs the real cb_btle(), by compiling ubertooth_callback.c into
 * itself, with the ring buffer and the decoder replaced by the fakes below.
 * The fake decoder copies the same 64 bytes the real one does, so a caller
 * that hands it a shorter buffer fails here in a plain build as well as under
 * a sanitizer.
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

/* The one packet the faked capture delivers, and a copy of the bytes the
 * decoder was handed. */
static usb_pkt_rx next_packet;
static uint8_t handed[LE_SYM_LEN];
static int decodes;

fifo_t* fifo_init(void) { return (fifo_t*)malloc(1); }
void fifo_inc_write_ptr(fifo_t* fifo) { (void)fifo; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet) { (void)fifo; (void)packet; }
usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return NULL; }
uint8_t fifo_empty(fifo_t* fifo) { (void)fifo; return 1; }

usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	(void)fifo;
	return next_packet;
}

/* Stands in for the decoder. It copies the same 64 bytes the real one copies,
 * which is what makes a short buffer fail here. The packet it hands back is a
 * real but empty libbtbb packet, because the rest of cb_btle() goes on to use
 * it. What the real decoder makes of the bytes is libbtbb's business; this
 * test only checks which bytes it was given. */
void lell_allocate_and_decode(const uint8_t* stream, uint16_t phys_channel,
                              uint32_t clk100ns, lell_packet** pkt)
{
	(void)phys_channel;
	(void)clk100ns;
	memcpy(handed, stream, LE_SYM_LEN);
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
 * chose. The guarantees come from elsewhere: the first case checks the buffer
 * directly, and a sanitizer build reports the read as a stack-buffer-overflow
 * whatever the bytes happen to be. */
static void dirty_the_stack(void)
{
	uint8_t scratch[4096];

	memset(scratch, STACK_PATTERN, sizeof(scratch));
	sink = scratch[0];
}

/* Runs one capture through the real callback. The callback prints the packet,
 * so its output goes to /dev/null to keep it out of the test's own, and the
 * bytes the decoder was given are left in handed[]. */
static void one_packet(void)
{
	ubertooth_t ut;
	int saved;
	int devnull;

	memset(&ut, 0, sizeof(ut));
	ut.fifo = fifo_init();
	ut.clk100ns_period = CLK100NS_TIMER1_PERIOD;

	memset(handed, STACK_PATTERN, sizeof(handed));
	decodes = 0;

	fflush(stdout);
	saved = dup(fileno(stdout));
	devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, fileno(stdout));
		close(devnull);
	}

	dirty_the_stack();
	cb_btle(&ut, NULL);

	fflush(stdout);
	dup2(saved, fileno(stdout));
	close(saved);

	free(ut.fifo);
}

/* ---- cases -------------------------------------------------------------- */

MU_TEST(the_buffer_holds_the_packet_and_zeros)
{
	/* The packet with filler laid down immediately behind it. All 64 bytes
	 * the decoder reads are then accounted for, without depending on what
	 * a stack frame happens to hold. */
	struct {
		usb_pkt_rx rx;
		uint8_t behind[LE_SYM_LEN];
	} frame;
	uint8_t symbols[LE_SYM_LEN];
	unsigned i;

	fill_packet(&frame.rx);
	memset(frame.behind, STACK_PATTERN, sizeof(frame.behind));

	memset(symbols, STACK_PATTERN, sizeof(symbols));
	ubertooth_le_symbols(&frame.rx, &symbols);

	mu_assert(memcmp(symbols, frame.rx.data, DMA_SIZE) == 0,
	          "the captured bytes are not the packet's");
	for (i = DMA_SIZE; i < LE_SYM_LEN; i++)
		mu_assert_int_eq(0, symbols[i]);
}

MU_TEST(a_capture_decodes_no_byte_from_past_the_packet)
{
	unsigned i;

	fill_packet(&next_packet);
	one_packet();

	mu_assert_int_eq(1, decodes);
	mu_assert(memcmp(handed, next_packet.data, DMA_SIZE) == 0,
	          "the decoder was given something other than the packet");
	/* The 14 bytes the capture does not reach. The decoder reads them for
	 * the tail of the payload and for the CRC, so they have to be zero and
	 * not whatever the callback's stack held. */
	for (i = DMA_SIZE; i < LE_SYM_LEN; i++)
		mu_assert_int_eq(0, handed[i]);
}

int main(void)
{
	MU_RUN(the_buffer_holds_the_packet_and_zeros);
	MU_RUN(a_capture_decodes_no_byte_from_past_the_packet);
	return mu_report();
}
