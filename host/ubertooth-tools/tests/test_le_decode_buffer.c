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
void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns)
{ (void)fifo; (void)arrival_ns; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns)
{ (void)fifo; (void)packet; (void)arrival_ns; }
usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return NULL; }
uint8_t fifo_empty(fifo_t* fifo) { (void)fifo; return next_index >= next_count; }

uint64_t fifo_popped_arrival_ns(const fifo_t* fifo) { (void)fifo; return 0; }
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

/* What the run left on the handle and on standard error: how many gaps and
 * how many capture transfer errors the callback counted, and the warnings it
 * printed. */
static uint32_t gaps;
static uint32_t transfer_errors;
static char errors[1024];

/* Runs the queued records through the real callback, one call each. The
 * callback prints the packet, so its output goes to /dev/null to keep it out
 * of the test's own, and the bytes the decoder was given are left in
 * handed[]. Standard error is collected instead of discarded: the gap warning
 * goes there. */
static void run_capture(void)
{
	ubertooth_t ut;
	int saved, saved_err;
	int devnull;
	FILE* err;
	long len = 0;

	memset(&ut, 0, sizeof(ut));
	ut.fifo = fifo_init();
	ut.clk100ns_period = CLK100NS_TIMER1_PERIOD;

	memset(handed, STACK_PATTERN, sizeof(handed));
	handed_len = 0;
	decodes = 0;
	next_index = 0;
	errors[0] = '\0';

	fflush(stdout);
	fflush(stderr);
	saved = dup(fileno(stdout));
	saved_err = dup(fileno(stderr));
	devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, fileno(stdout));
		close(devnull);
	}
	err = tmpfile();
	if (err != NULL)
		dup2(fileno(err), fileno(stderr));

	dirty_the_stack();
	while (next_index < next_count)
		cb_btle(&ut, NULL);

	fflush(stdout);
	fflush(stderr);
	dup2(saved, fileno(stdout));
	dup2(saved_err, fileno(stderr));
	close(saved);
	close(saved_err);

	if (err != NULL) {
		len = ftell(err);
		if (len > 0) {
			if (len > (long)sizeof(errors) - 1)
				len = (long)sizeof(errors) - 1;
			rewind(err);
			len = (long)fread(errors, 1, (size_t)len, err);
		}
		if (len < 0)
			len = 0;
		errors[len] = '\0';
		fclose(err);
	}

	/* What ubertooth_queue_gaps() and ubertooth_transfer_errors() hand a
	 * tool at the end of a capture. */
	gaps = ut.queue_gaps;
	transfer_errors = ut.transfer_errors;
	free(ut.fifo);
}

/* How many times `needle` appears in the warnings the run printed. */
static int times_said(const char* needle)
{
	const char* p = errors;
	int n = 0;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
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

/* ---- gaps the board reports -------------------------------------------- */

/* The warning the first gap of a capture prints, by a phrase of its own. */
#define GAP_WARNING "dropped at least one packet"

MU_TEST(a_capture_with_nothing_dropped_reports_no_gap)
{
	fill_packet(&next_packets[0]);
	next_count = 1;
	run_capture();

	mu_assert_int_eq(0, (int)gaps);
	mu_assert_int_eq(0, times_said(GAP_WARNING));
}

/*
 * The board sets FIFO_OVERFLOW on the first packet it sends after dropping
 * one. Every one of those is a gap in the capture, and the warning is printed
 * once however many there are: a capture that lost packets all afternoon
 * would otherwise print a line per gap among the packets themselves.
 */
MU_TEST(each_dropped_packet_is_counted_and_the_warning_said_once)
{
	fill_packet(&next_packets[0]);
	fill_packet(&next_packets[1]);
	fill_packet(&next_packets[2]);
	next_packets[0].status = FIFO_OVERFLOW;
	next_packets[2].status = FIFO_OVERFLOW;
	next_count = 3;
	run_capture();

	mu_assert_int_eq(2, (int)gaps);
	mu_assert_int_eq(1, times_said(GAP_WARNING));
}

/*
 * A long packet arrives as a chain of records that all carry the first one's
 * readings, this flag among them. One gap, counted once.
 */
MU_TEST(a_long_packet_after_a_gap_counts_one_gap)
{
	const uint16_t total = 120;
	unsigned i;

	for (i = 0; i * DMA_SIZE < total; i++) {
		usb_pkt_rx* f = &next_packets[i];
		uint16_t offset = (uint16_t)(i * DMA_SIZE);

		memset(f, 0, sizeof(*f));
		f->pkt_type = offset ? LE_PACKET_CONT : LE_PACKET_START;
		f->status = FIFO_OVERFLOW;
		f->channel = 4;
		f->clk100ns = htole32(1234);
		f->reserved[0] = total & 0xff;
		f->reserved[1] = (uint8_t)(((total >> 8) & LE_FRAG_TOTAL_HIGH_MASK) |
		                           (i << LE_FRAG_INDEX_SHIFT));
	}
	next_count = i;
	mu_assert(next_count > 1, "the test packet took one record");
	run_capture();

	mu_assert_int_eq(1, (int)gaps);
	mu_assert_int_eq(1, times_said(GAP_WARNING));
}

/*
 * A gap is counted even when the record carrying the news is thrown away: it
 * is the board's word that packets were lost before it, and dropping it with
 * the record would hide the gap. A channel outside the band is the earliest
 * of those exits.
 */
MU_TEST(a_discarded_record_still_reports_its_gap)
{
	fill_packet(&next_packets[0]);
	next_packets[0].status = FIFO_OVERFLOW;
	next_packets[0].channel = NUM_BREDR_CHANNELS;
	next_count = 1;
	run_capture();

	mu_assert_int_eq(0, decodes);
	mu_assert_int_eq(1, (int)gaps);
	mu_assert_int_eq(1, times_said(GAP_WARNING));
}

/* ---- the packets a faulted transfer leaves behind ---------------------- */

#define TRANSFER_WARNING "transfer error while capturing"

/*
 * The board marks DMA_ERROR on a packet it finished receiving as the chip's
 * transfer controller reported an error. That packet is whole, which is why it
 * was kept, but a packet the same fault cut short is gone with nothing else to
 * show for it, so each mark is a place the capture may be missing packets. The
 * host counted neither engine's mark before this, so such a capture looked
 * exactly like a clean one.
 */
MU_TEST(each_packet_marked_by_a_transfer_error_is_counted_and_warned_once)
{
	fill_packet(&next_packets[0]);
	fill_packet(&next_packets[1]);
	fill_packet(&next_packets[2]);
	next_packets[0].status = DMA_ERROR;
	next_packets[2].status = DMA_ERROR;
	next_count = 3;
	run_capture();

	mu_assert_int_eq(2, (int)transfer_errors);
	mu_assert_int_eq(1, times_said(TRANSFER_WARNING));

	/* The two counts are separate: a faulted transfer is not a full queue,
	 * and a capture should not be told it has gaps it does not have. */
	mu_assert_int_eq(0, (int)gaps);
	mu_assert_int_eq(0, times_said(GAP_WARNING));
}

MU_TEST(a_capture_with_no_transfer_error_says_nothing)
{
	fill_packet(&next_packets[0]);
	next_count = 1;
	run_capture();

	mu_assert_int_eq(0, (int)transfer_errors);
	mu_assert_int_eq(0, times_said(TRANSFER_WARNING));
}

/*
 * A long packet arrives as a chain of records that all carry the first one's
 * readings, this mark among them. One transfer error, counted once.
 */
MU_TEST(a_long_packet_marked_by_a_transfer_error_counts_one)
{
	const uint16_t total = 120;
	unsigned i;

	for (i = 0; i * DMA_SIZE < total; i++) {
		usb_pkt_rx* f = &next_packets[i];
		uint16_t offset = (uint16_t)(i * DMA_SIZE);

		memset(f, 0, sizeof(*f));
		f->pkt_type = offset ? LE_PACKET_CONT : LE_PACKET_START;
		f->status = DMA_ERROR;
		f->channel = 4;
		f->clk100ns = htole32(1234);
		f->reserved[0] = total & 0xff;
		f->reserved[1] = (uint8_t)(((total >> 8) & LE_FRAG_TOTAL_HIGH_MASK) |
		                           (i << LE_FRAG_INDEX_SHIFT));
	}
	next_count = i;
	mu_assert(next_count > 1, "the test packet took one record");
	run_capture();

	mu_assert_int_eq(1, (int)transfer_errors);
	mu_assert_int_eq(1, times_said(TRANSFER_WARNING));
}

int main(void)
{
	MU_RUN(a_single_record_decodes_only_the_bytes_it_holds);
	MU_RUN(a_long_packet_decodes_once_with_every_byte);
	MU_RUN(a_chain_with_a_record_missing_decodes_nothing);
	MU_RUN(a_capture_with_nothing_dropped_reports_no_gap);
	MU_RUN(each_dropped_packet_is_counted_and_the_warning_said_once);
	MU_RUN(a_long_packet_after_a_gap_counts_one_gap);
	MU_RUN(a_discarded_record_still_reports_its_gap);
	MU_RUN(each_packet_marked_by_a_transfer_error_is_counted_and_warned_once);
	MU_RUN(a_capture_with_no_transfer_error_says_nothing);
	MU_RUN(a_long_packet_marked_by_a_transfer_error_counts_one);
	return mu_report();
}
