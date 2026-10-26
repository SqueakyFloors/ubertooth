/*
 * What the low-energy callback hands libbtbb's decoder.
 *
 * The decoder takes a bare pointer and copies a fixed LE_SYM_LEN bytes out of
 * it, while a received packet carries only DMA_SIZE. Handing it the received
 * packet's own array therefore read 14 bytes of whatever followed the packet
 * in memory, and the decoder reads exactly those bytes for the tail of a
 * payload and for the CRC, so a PDU whose length field claimed more than was
 * captured decoded them as packet data.
 *
 * ubertooth_callback.c is compiled into this test so cb_btle() itself runs,
 * with the ring buffer and the decoder replaced by the fakes below. The fake
 * decoder copies the same fixed length the real one copies, so a caller that
 * hands it a shorter buffer is caught here as well as by a sanitizer.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
#include "../../libubertooth/src/ubertooth_callback.c"

/* A byte no crafted packet uses, so anything of it reaching the decoder came
 * from past the end of the packet. */
#define STACK_PATTERN 0xab

/* ---- fakes -------------------------------------------------------------- */

/* The one packet a capture delivers, and what the decoder was handed. */
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

/* Stands in for lell_allocate_and_decode(), copying the fixed length the real
 * one copies. The packet it returns is a real, empty one: what the decoder
 * makes of the bytes is libbtbb's business, and only the bytes it was given
 * are checked here. */
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

/* A low-energy advertising packet whose every captured byte is set, so a byte
 * the decoder reads is either one of these or came from past the packet. */
static void fill_packet(usb_pkt_rx* rx)
{
	unsigned i;

	memset(rx, 0, sizeof(*rx));
	rx->pkt_type = LE_PACKET;
	rx->channel = 0;
	rx->clk100ns = htole32(1234);
	for (i = 0; i < DMA_SIZE; i++)
		rx->data[i] = (uint8_t)(i + 1);
	/* The advertising access address, little endian, and a PDU header
	 * claiming a payload longer than the capture reaches. */
	rx->data[0] = 0xd6;
	rx->data[1] = 0xbe;
	rx->data[2] = 0x89;
	rx->data[3] = 0x8e;
	rx->data[4] = 0x00;
	rx->data[5] = 58;
}

/* Leaves non-zero bytes in the stack the callback is about to use, so a read
 * past the end of the packet cannot quietly find zeros there and pass. */
static volatile uint8_t sink;

static void dirty_the_stack(void)
{
	uint8_t scratch[4096];

	memset(scratch, STACK_PATTERN, sizeof(scratch));
	sink = scratch[0];
}

/* One capture through the real callback, with its output kept off the test's
 * own, and the bytes the decoder was given left in handed[]. */
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
	/* The packet with the pattern laid down right behind it, so the whole
	 * length the decoder reads is accounted for without depending on what
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
	ubertooth_le_symbols(&frame.rx, symbols);

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
	/* The bytes the capture does not reach. The decoder reads them for the
	 * tail of the payload and for the CRC, so they must be zero rather
	 * than whatever the callback's stack held. */
	for (i = DMA_SIZE; i < LE_SYM_LEN; i++)
		mu_assert_int_eq(0, handed[i]);
}

int main(void)
{
	MU_RUN(the_buffer_holds_the_packet_and_zeros);
	MU_RUN(a_capture_decodes_no_byte_from_past_the_packet);
	return mu_report();
}
