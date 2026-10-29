/*
 * Splitting a long low-energy packet across USB records, and the host putting
 * it back together: firmware/bluetooth_rxtx/ubertooth_usb.c against
 * host/libubertooth/src/ubertooth_le_frag.c.
 *
 * A record carries 50 bytes of packet, so a packet longer than that goes out
 * as one LE_PACKET_START record and ordered LE_PACKET_CONT records. Both
 * capture paths in the firmware queue packets through the same helper -- the
 * connection follower that ubertooth-btle -f and -n use, and the promiscuous
 * mode's -p -- so getting the split wrong would cut every long packet on
 * every path at once.
 *
 * Both halves run here for real. The ring is pure logic, so ubertooth_usb.c
 * is compiled into this translation unit (as test_usb_queue.c does), and the
 * host's reassembly is plain C, so it is linked in. Each case queues a packet
 * of a chosen length, reads the records back out of the ring, and hands them
 * to the host: what comes out has to be the bytes that went in.
 *
 * The lengths are the ones where the behaviour changes: a packet that exactly
 * fills one record, the first that does not, and the longest the length field
 * in a PDU header can ask for.
 */
#include <stdio.h>
#include <string.h>

#include "minunit.h"

/* ------------------------------------------------------------------ *
 * What ubertooth_usb.c calls. This test reads records straight out of the
 * ring, so nothing below has to do anything.
 * ------------------------------------------------------------------ */

#include "usbapi.h"
#include "usbhw_lpc.h"

int USBHwEPWrite(U8 bEP, U8 *pbBuf, U32 iLen)
{
	(void)bEP; (void)pbBuf;
	return (int)iLen;
}

U8 USBHwEPGetStatus(U8 bEP) { (void)bEP; return 0; }
void USBHwEPStall(U8 bEP, BOOL fStall) { (void)bEP; (void)fStall; }
void USBInit(void) {}
void USBRegisterDescriptors(const U8 *d) { (void)d; }
void USBRegisterRequestHandler(int t, TFnHandleRequest *h, U8 *s, int n)
{
	(void)t; (void)h; (void)s; (void)n;
}
void USBRegisterWinusbInterface(U8 r, const char *g) { (void)r; (void)g; }
void USBHwConnect(BOOL c) { (void)c; }
void USBHwISR(void) {}

void get_device_serial(uint8_t *buffer, int *len)
{
	memset(buffer, 0, 17);
	*len = 17;
}

/* The NVIC, as stubs-usbq/ubertooth.h names it. */
uint32_t fake_iser0;
uint32_t fake_icer0;

/* The sender under test, compiled in so the ring's statics are reachable. */
#include "ubertooth_usb.c"

/* Every USB service pass tells the watchdog it is running and feeds it when the
 * pass is done. Neither call matters here, and what the firmware does with them
 * is tested in test_watchdog.c and test_usb_queue.c. */
uint8_t watchdog_mark(uint8_t site) { (void)site; return WATCHDOG_SITE_NONE; }
uint8_t watchdog_mark_inner(uint8_t site) { (void)site; return WATCHDOG_SITE_NONE; }
void watchdog_unmark(uint8_t site) { (void)site; }
void watchdog_usb_serviced(void) {}

/* The receiver under test. */
#include "ubertooth_le_frag.h"

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/* The longest packet there is: the access address, the PDU header, a payload
 * as long as the header's 8-bit length field can claim, and the CRC. */
#define LONGEST_PACKET LE_MAX_USB_PACKET_SIZE

/* A packet whose every byte says where it sits, so a record carrying the
 * wrong part of it cannot look right. The first four bytes are the access
 * address, which the capture loops hold separately from what they received.
 */
static uint8_t whole[LONGEST_PACKET + 8];

static void fill_whole(void)
{
	unsigned i;

	for (i = 0; i < sizeof(whole); ++i)
		whole[i] = (uint8_t)(i + 1);
}

/* The readings every record of one packet carries. */
static le_pkt_meta sample_meta(void)
{
	le_pkt_meta meta;

	meta.status = 0x12;
	meta.channel = 37;
	meta.clkn_high = 0x34;
	meta.clk100ns = 0x05060708;
	meta.rssi_max = -40;
	meta.rssi_min = -80;
	meta.rssi_avg = -60;
	meta.rssi_count = 9;
	return meta;
}

/* Queues a packet of `total` bytes and returns what usb_enqueue_le_packet()
 * said. The ring starts empty, so the records read back afterwards are this
 * packet's and nothing else. */
static int queue_packet(unsigned total)
{
	le_pkt_meta meta = sample_meta();

	fill_whole();
	return usb_enqueue_le_packet(&meta, whole, whole + 4, total - 4);
}

/* How many records are waiting. */
static unsigned queued_records(void)
{
	return (unsigned)((tail - head) & 0x7F);
}

/* ------------------------------------------------------------------ *
 * The split itself
 * ------------------------------------------------------------------ */

/* 50 bytes is what one record holds, so a packet that length still travels
 * the way it always did: one plain record, and nothing about fragments. */
MU_TEST(a_packet_that_fills_one_record_is_sent_as_one)
{
	usb_pkt_rx *r;

	usb_queue_init();
	mu_assert_int_eq(1, queue_packet(DMA_SIZE));
	mu_assert_int_eq(1, (int)queued_records());

	r = dequeue();
	mu_assert(r != NULL, "the record was not queued");
	mu_assert_int_eq(LE_PACKET, r->pkt_type);
	mu_assert_int_eq(0, r->reserved[0]);
	mu_assert_int_eq(0, r->reserved[1]);
	mu_assert(memcmp(r->data, whole, DMA_SIZE) == 0,
	          "the record does not carry the packet");
}

/* One byte more needs a second record, which is where the chain starts. */
MU_TEST(a_packet_one_byte_too_long_is_sent_as_two_records)
{
	usb_pkt_rx *r;

	usb_queue_init();
	mu_assert_int_eq(1, queue_packet(DMA_SIZE + 1));
	mu_assert_int_eq(2, (int)queued_records());

	r = dequeue();
	mu_assert_int_eq(LE_PACKET_START, r->pkt_type);
	mu_assert_int_eq(DMA_SIZE + 1, (int)r->reserved[0]);
	mu_assert_int_eq(0, r->reserved[1] >> LE_FRAG_INDEX_SHIFT);
	mu_assert(memcmp(r->data, whole, DMA_SIZE) == 0,
	          "the first record does not carry the front of the packet");

	r = dequeue();
	mu_assert_int_eq(LE_PACKET_CONT, r->pkt_type);
	mu_assert_int_eq(DMA_SIZE + 1, (int)r->reserved[0]);
	mu_assert_int_eq(1, r->reserved[1] >> LE_FRAG_INDEX_SHIFT);
	mu_assert_int_eq(whole[DMA_SIZE], r->data[0]);
}

/* Records are reused, so the bytes a short last record does not fill are
 * cleared; otherwise it hands the host the previous packet's bytes. */
MU_TEST(the_last_record_is_cleared_past_the_packet)
{
	usb_pkt_rx *r;
	unsigned i;

	usb_queue_init();
	/* Dirty every record in the ring, so wherever the packet lands, a
	 * record left unfilled hands over the bytes that were there before. */
	while ((r = usb_enqueue()) != NULL)
		memset(r->data, 0xaa, DMA_SIZE);
	while (dequeue() != NULL)
		;

	mu_assert_int_eq(1, queue_packet(DMA_SIZE + 1));
	(void)dequeue();
	r = dequeue();
	for (i = 1; i < DMA_SIZE; ++i)
		mu_assert_int_eq(0, r->data[i]);
}

/* The longest packet there can be, and the record count it needs. */
MU_TEST(the_longest_packet_is_split_across_every_record_it_needs)
{
	unsigned expected = (LONGEST_PACKET + DMA_SIZE - 1) / DMA_SIZE;
	unsigned i;

	usb_queue_init();
	mu_assert_int_eq(1, queue_packet(LONGEST_PACKET));
	mu_assert_int_eq((int)expected, (int)queued_records());

	for (i = 0; i < expected; ++i) {
		usb_pkt_rx *r = dequeue();
		unsigned offset = i * DMA_SIZE;
		unsigned chunk = LONGEST_PACKET - offset;

		if (chunk > DMA_SIZE)
			chunk = DMA_SIZE;
		mu_assert_int_eq(i == 0 ? LE_PACKET_START : LE_PACKET_CONT,
		                 r->pkt_type);
		mu_assert_int_eq((int)i, r->reserved[1] >> LE_FRAG_INDEX_SHIFT);
		mu_assert(memcmp(r->data, whole + offset, chunk) == 0,
		          "a record carries the wrong part of the packet");
	}
}

/* Nothing longer than the host's buffer is sent at all, rather than sent and
 * thrown away at the other end. */
MU_TEST(a_packet_longer_than_the_host_can_hold_is_refused)
{
	usb_queue_init();
	mu_assert_int_eq(0, queue_packet(LONGEST_PACKET + 1));
	mu_assert_int_eq(0, (int)queued_records());
}

/* Half a chain is worse than no packet: the host throws an incomplete one
 * away, and the records it did take are gone from the ring. So a ring without
 * room for the whole packet takes none of it. */
MU_TEST(a_ring_without_room_for_every_record_takes_none)
{
	unsigned needed = (LONGEST_PACKET + DMA_SIZE - 1) / DMA_SIZE;
	unsigned filler;

	usb_queue_init();
	/* Fill the ring until it has room for one record fewer than the packet
	 * needs. */
	while (usb_queue_space() > needed - 1)
		mu_assert(usb_enqueue() != NULL, "the ring refused a record early");
	filler = queued_records();

	mu_assert_int_eq(0, queue_packet(LONGEST_PACKET));
	/* Only the records queued above are there; none of the packet's. */
	mu_assert_int_eq((int)filler, (int)queued_records());
}

/* The host reads the first record's readings and uses them for the whole
 * packet, but every record carries them, so a dropped first record cannot
 * leave a chain describing itself with another packet's channel. */
MU_TEST(every_record_carries_the_packets_readings)
{
	le_pkt_meta meta = sample_meta();
	unsigned i;

	usb_queue_init();
	mu_assert_int_eq(1, queue_packet(LONGEST_PACKET));

	for (i = 0; i < (LONGEST_PACKET + DMA_SIZE - 1) / DMA_SIZE; ++i) {
		usb_pkt_rx *r = dequeue();

		mu_assert_int_eq(meta.status, r->status);
		mu_assert_int_eq(meta.channel, r->channel);
		mu_assert_int_eq(meta.clkn_high, r->clkn_high);
		mu_assert(r->clk100ns == meta.clk100ns,
		          "a record carries the wrong timestamp");
		mu_assert_int_eq(meta.rssi_max, r->rssi_max);
		mu_assert_int_eq(meta.rssi_min, r->rssi_min);
		mu_assert_int_eq(meta.rssi_avg, r->rssi_avg);
		mu_assert_int_eq(meta.rssi_count, r->rssi_count);
	}
}

/* ------------------------------------------------------------------ *
 * The round trip: what the host makes of the records
 * ------------------------------------------------------------------ */

/* These cases are about the bytes of a packet, not its timing, so they pass
 * an arrival reading of zero and keep the one reported back out of the way. */
static int reassemble(le_usb_reassembly *state, const usb_pkt_rx *frame,
                      const uint8_t **data, uint16_t *length,
                      const usb_pkt_rx **metadata)
{
	uint64_t ignored_arrival_ns;

	return le_usb_reassemble(state, frame, 0, data, length, metadata,
	                         &ignored_arrival_ns);
}

/* Hands every queued record to the host's reassembly and checks that the one
 * packet that comes out is the one that went in. */
static void round_trip(unsigned total)
{
	le_usb_reassembly state;
	const uint8_t *data = NULL;
	uint16_t length = 0;
	const usb_pkt_rx *metadata = NULL;
	le_pkt_meta meta = sample_meta();
	int completions = 0;
	usb_pkt_rx *r;

	memset(&state, 0, sizeof(state));
	usb_queue_init();
	mu_assert_int_eq(1, queue_packet(total));

	while ((r = dequeue()) != NULL) {
		int result = reassemble(&state, r, &data, &length, &metadata);

		mu_assert(result != LE_REASSEMBLY_ERROR,
		          "the host refused a record the firmware sent");
		if (result != LE_REASSEMBLY_COMPLETE)
			continue;
		++completions;
		mu_assert_int_eq((int)total, (int)length);
		mu_assert(memcmp(data, whole, total) == 0,
		          "the host put the packet back together wrongly");
		/* Whichever path it took, the host sees a plain packet with the
		 * readings the capture loop measured. */
		mu_assert_int_eq(LE_PACKET, metadata->pkt_type);
		mu_assert_int_eq(meta.channel, metadata->channel);
		mu_assert(metadata->clk100ns == meta.clk100ns,
		          "the packet carries the wrong timestamp");
	}
	mu_assert_int_eq(1, completions);
}

MU_TEST(the_host_puts_a_long_packet_back_together)
{
	/* One record exactly, the first length that needs two, a length whose
	 * last record is short, one whose last record is full, and the longest
	 * packet there is. */
	round_trip(DMA_SIZE);
	round_trip(DMA_SIZE + 1);
	round_trip(DMA_SIZE + 7);
	round_trip(2 * DMA_SIZE);
	round_trip(LONGEST_PACKET);
}

/* A packet that fits one record is handed over with the 50 bytes of the
 * record, because that record says nothing about how much of it is packet.
 * The decoder reads the length out of the packet itself. */
MU_TEST(a_single_record_packet_is_handed_over_whole)
{
	le_usb_reassembly state;
	const uint8_t *data = NULL;
	uint16_t length = 0;
	const usb_pkt_rx *metadata = NULL;

	memset(&state, 0, sizeof(state));
	usb_queue_init();
	mu_assert_int_eq(1, queue_packet(20));
	mu_assert_int_eq(LE_REASSEMBLY_COMPLETE,
	                 reassemble(&state, dequeue(), &data, &length,
	                            &metadata));
	mu_assert_int_eq(DMA_SIZE, (int)length);
	mu_assert(memcmp(data, whole, 20) == 0,
	          "the record does not carry the packet");
}

int main(void)
{
	printf("LE fragmenting enqueue tests\n");
	MU_RUN(a_packet_that_fills_one_record_is_sent_as_one);
	MU_RUN(a_packet_one_byte_too_long_is_sent_as_two_records);
	MU_RUN(the_last_record_is_cleared_past_the_packet);
	MU_RUN(the_longest_packet_is_split_across_every_record_it_needs);
	MU_RUN(a_packet_longer_than_the_host_can_hold_is_refused);
	MU_RUN(a_ring_without_room_for_every_record_takes_none);
	MU_RUN(every_record_carries_the_packets_readings);
	MU_RUN(the_host_puts_a_long_packet_back_together);
	MU_RUN(a_single_record_packet_is_handed_over_whole);
	return mu_report();
}
