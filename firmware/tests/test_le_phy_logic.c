/*
 * The low-energy capture engine in firmware/bluetooth_rxtx/le_phy.c, run here
 * rather than read as text.
 *
 * This is the engine behind `ubertooth-btle -n` and `-f`: it receives
 * advertising and connection packets, decides what each one means, and hands
 * them to the host. Until now none of it could be run on the host, so all of
 * it was covered by scanning the source (test_le_phy_order.c). A scan pins the
 * shape of the code; it cannot catch arithmetic that is wrong. The arithmetic
 * is most of what this file does: whitening, a packet's length, the connection
 * parameters in a CONNECT_IND against the ranges the specification allows, and
 * the anchor point a connection event is timed from.
 *
 * What makes running it possible is stubs-lephy/, which replaces the register
 * file under the firmware's own ubertooth.h with ordinary variables. So the
 * code here is the firmware's, the LED and pin definitions are the
 * firmware's, and the queue a packet is handed to is the real one:
 * ubertooth_usb.c is compiled into this translation unit, as it is in
 * test_usb_queue.c, and so is le_phy.c, because the functions under test are
 * static. The host's reassembly (ubertooth_le_frag.c) is linked in too, so a
 * long packet makes the whole round trip with real code at both ends.
 *
 * What is NOT run here, and why:
 *
 *   le_phy_main(), le_cc2400_init_rf(), change_channel(),
 *   le_cc2400_strobe_rx(), le_dma_init(), le_sys_init() and the live paths of
 *   le_DMA_IRQHandler() and TIMER1_IRQHandler() all drive the radio, the DMA
 *   controller and the SSP. A fake register file lets them compile, not work:
 *   bytes only arrive because a radio put them there, and a test that fed
 *   itself those bytes would be asserting what the test wrote, not what the
 *   board does. Those stay covered by test_le_phy_order.c's scan and by
 *   hardware testing.
 *
 * The one part of the interrupt handlers that does run here is their first
 * line: once the PHY has been torn down they must do nothing but acknowledge.
 * That is checked by asserting the radio is not touched -- the fakes below
 * count every access -- rather than by modelling one.
 */
#include <stdio.h>
#include <string.h>

#include "minunit.h"

/* The firmware's own headers, through stubs-lephy's register file: the
 * integer types, the radio and DMA prototypes the fakes below have to match,
 * and the shared interface the host reads records with. */
#include "ubertooth.h"
#include "ubertooth_clock.h"
#include "ubertooth_dma.h"
#include "cc2400_wait.h"

/* The host's side of the same path: what puts a chain of records back
 * together into one packet. Linked in so the round trip below runs the real
 * code on both ends. */
#include "ubertooth_le_frag.h"

/* ------------------------------------------------------------------ *
 * The hardware around the code under test
 * ------------------------------------------------------------------ */

/* The register file from stubs-lephy/lpc17.h. Each is the memory the
 * firmware's register name resolves to here. */
volatile uint32_t fake_t1ir, fake_t1tcr, fake_t1tc, fake_t1pr, fake_t1mcr;
volatile uint32_t fake_t1mr0, fake_t1mr1, fake_t1mr2, fake_t1mr3;
volatile uint32_t fake_iser0, fake_icer0, fake_icpr0;
volatile uint32_t fake_dmac_int_stat, fake_dmac_int_tc_stat;
volatile uint32_t fake_dmac_int_tc_clear;
volatile uint32_t fake_dmac_int_err_stat, fake_dmac_int_err_clr;
volatile uint32_t fake_dmacc0_src, fake_dmacc0_dest, fake_dmacc0_lli;
volatile uint32_t fake_dmacc0_control, fake_dmacc0_config;
volatile uint32_t fake_ssp1dr, fake_ssp1sr, fake_ssp1dmacr;
volatile uint32_t fake_fio1pin, fake_fio1set, fake_fio1clr;
volatile uint32_t fake_fio2pin, fake_fio2set, fake_fio2clr;

/*
 * Every way the code under test can reach the radio, counted rather than
 * answered. No test here drives a path that needs the radio to reply, so a
 * non-zero count means the test reached code this file does not claim to
 * cover -- which the tests assert on, and which is what keeps the honest
 * half of this file honest.
 */
static int radio_touched;
static int dma_clear_calls;
static int ssp_starts;

u16 cc2400_get(u8 reg) { (void)reg; radio_touched++; return 0; }
void cc2400_set(u8 reg, u16 val) { (void)reg; (void)val; radio_touched++; }
u8 cc2400_status(void) { radio_touched++; return 0; }
u8 cc2400_strobe(u8 reg) { (void)reg; radio_touched++; return 0; }
int cc2400_wait_xosc_stable(void (*service)(void))
{
	(void)service;
	radio_touched++;
	return 1;
}

void dio_ssp_init(void) { ssp_starts++; }
void dio_ssp_start(void) { ssp_starts++; }
void dio_ssp_stop(void) { ssp_starts++; }
void dma_clear_interrupts(unsigned channel) { (void)channel; dma_clear_calls++; }
void clkn_init(void) {}
void clkn_disable(void) {}

/*
 * The ARM `rbit` instruction, which the firmware's rbit() is one line of
 * inline assembly around: reverse the order of the 32 bits.
 */
u32 rbit(u32 value)
{
	u32 result = 0;
	int i;

	for (i = 0; i < 32; i++)
		if (value & (1u << i))
			result |= 1u << (31 - i);
	return result;
}

/* ------------------------------------------------------------------ *
 * The USB stack below the packet queue, as test_usb_queue.c fakes it. These
 * tests read packets back out of the ring with dequeue(), which is the path
 * UBERTOOTH_POLL takes, so nothing here has to reach the endpoint.
 * ------------------------------------------------------------------ */

#include "usbapi.h"
#include "usbhw_lpc.h"

int USBHwEPWrite(U8 bEP, U8 *pbBuf, U32 iLen)
{
	(void)bEP;
	(void)pbBuf;
	mu_assert(0, "the low-energy engine handed a packet to the bulk IN "
	             "endpoint, which no poll-read host reads");
	return (int)iLen;
}

U8 USBHwEPGetStatus(U8 bEP) { (void)bEP; return 0; }
void USBHwEPStall(U8 bEP, BOOL fStall) { (void)bEP; (void)fStall; }
void USBInit(void) {}
void USBRegisterDescriptors(const U8 *d) { (void)d; }
void USBRegisterRequestHandler(int t, TFnHandleRequest *h, U8 *s)
{
	(void)t; (void)h; (void)s;
}
void USBRegisterWinusbInterface(U8 r, const char *g) { (void)r; (void)g; }
void USBHwConnect(BOOL c) { (void)c; }
void USBHwISR(void) {}

void get_device_serial(uint8_t *buffer, int *len)
{
	memset(buffer, 0, 17);
	*len = 17;
}

/* ------------------------------------------------------------------ *
 * The watchdog
 * ------------------------------------------------------------------ */

/* The engine's loop and every USB service pass tell the watchdog they are
 * alive, and mark which wait they are in so a reset can say where the board
 * died. Nothing here is about that, so these only have to exist; what the
 * firmware does with them is tested in test_watchdog.c and test_usb_queue.c. */
uint8_t watchdog_mark(uint8_t site) { (void)site; return WATCHDOG_SITE_NONE; }
void watchdog_usb_serviced(void) {}

/* ------------------------------------------------------------------ *
 * The code under test
 * ------------------------------------------------------------------ */

/* The real packet queue, so a packet handed to the host goes through the
 * firmware's own ring and comes back out of it. */
#include "ubertooth_usb.c"

/* ...and the engine itself. Included rather than linked: the buffer pool, the
 * packet handlers and the connection state are all static. */
#include "le_phy.c"

/*
 * The state le_phy.c reads but does not own. bluetooth_rxtx.c defines these
 * in the firmware: the mode the host has asked for, the shared low-energy
 * settings, the advertising channel `ubertooth-btle -A` picks, and the flag
 * `ubertooth-btle` sets to stop following a connection.
 */
volatile uint8_t mode;
volatile uint8_t requested_mode;
le_state_t le;
volatile uint16_t le_adv_channel = 2402;
volatile int cancel_follow;

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/* The advertising access address, as the engine knows it. */
#define ADV_AA 0x8e89bed6u

/* A data channel and an advertising channel, by frequency: the engine keys
 * nearly every decision on which of the two a packet arrived on. */
#define DATA_CHANNEL_MHZ 2440
#define ADV_CHANNEL_MHZ  2402

static void start_fresh(void)
{
	radio_touched = 0;
	dma_clear_calls = 0;
	ssp_starts = 0;
	fake_t1mcr = 0;
	fake_t1mr0 = 0;
	fake_t1mr1 = 0;
	fake_t1mr2 = 0;
	fake_t1mr3 = 0;
	fake_t1tc = 0;
	fake_iser0 = 0;
	fake_icer0 = 0;
	memset(&le, 0, sizeof(le));
	usb_queue_init();
	buffers_init();
	queue_init(&packet_queue);
	current_rxbuf = NULL;
	le_phy_active = 0;
	cancel_follow = 0;
	reset_conn();
	reset_conn_event();
}

/* Fill a receive buffer the way the DMA handler leaves one: a PDU of
 * `pdu_len` bytes, plus the two-byte header and the three-byte CRC. */
static void fill_packet(le_rx_t *buf, unsigned pdu_len, unsigned channel)
{
	unsigned i;

	memset(buf, 0, sizeof(*buf));
	buf->size = pdu_len + 2 + 3;
	buf->pos = buf->size;
	buf->channel = channel;
	buf->access_address = ADV_AA;
	buf->timestamp = 0x11223344;
	buf->rssi_min = -70;
	buf->rssi_max = -40;
	buf->rssi_sum = 0;
	for (i = 0; i < buf->size && i < sizeof(buf->data); i++)
		buf->data[i] = (uint8_t)(i + 1);
}

/* The fields of a CONNECT_IND, as the engine reads them out of the PDU. */
struct connect_req {
	uint32_t access_address;
	uint32_t crc_init;           /* three bytes on the wire */
	uint8_t  win_size;           /* units of 1.25 ms */
	uint16_t win_offset;         /* units of 1.25 ms */
	uint16_t conn_interval;      /* units of 1.25 ms */
	uint16_t latency;
	uint16_t supervision_timeout;/* units of 10 ms */
	uint8_t  channel_map[5];
	uint8_t  hop_increment;
};

/* A request that satisfies every range the engine checks: a 30 ms connection
 * interval, no transmit window offset, the smallest legal window, a 100 ms
 * supervision timeout and all 37 data channels in use. */
static struct connect_req valid_connect_req(void)
{
	struct connect_req r;

	memset(&r, 0, sizeof(r));
	r.access_address = 0xdeadbeef;
	r.crc_init = 0x555555;
	r.win_size = 1;
	r.win_offset = 0;
	r.conn_interval = 24;
	r.latency = 0;
	r.supervision_timeout = 10;
	memset(r.channel_map, 0xff, sizeof(r.channel_map));
	r.hop_increment = 7;
	return r;
}

static void put_le(uint8_t *at, uint32_t value, unsigned bytes)
{
	unsigned i;

	for (i = 0; i < bytes; i++)
		at[i] = (uint8_t)(value >> (i * 8));
}

/*
 * Lay a CONNECT_IND out as it arrives on an advertising channel: the PDU
 * header, the two addresses, then the connection parameters. The engine reads
 * each field at a fixed offset, so this is also what pins those offsets.
 */
static void build_connect_req(le_rx_t *buf, const struct connect_req *r)
{
	fill_packet(buf, 6 + 6 + 22, ADV_CHANNEL_MHZ);
	memset(buf->data, 0, sizeof(buf->data));
	buf->data[0] = 0x05;                      /* CONNECT_IND */
	buf->data[1] = 6 + 6 + 22;                /* payload length */
	put_le(&buf->data[14], r->access_address, 4);
	put_le(&buf->data[18], r->crc_init, 3);
	buf->data[21] = r->win_size;
	put_le(&buf->data[22], r->win_offset, 2);
	put_le(&buf->data[24], r->conn_interval, 2);
	put_le(&buf->data[26], r->latency, 2);
	put_le(&buf->data[28], r->supervision_timeout, 2);
	memcpy(&buf->data[30], r->channel_map, 5);
	buf->data[35] = r->hop_increment;
}

/* Is the connection state back to what reset_conn() leaves: nothing followed,
 * listening on the advertising address? */
static int connection_was_reset(void)
{
	return conn.access_address == ADV_AA && conn.conn_interval == 0 &&
	       conn.supervision_timeout == 0 && conn.hop_increment == 0;
}

/* ------------------------------------------------------------------ *
 * The buffer pool
 * ------------------------------------------------------------------ */

/*
 * One buffer is always held, either waiting for a packet or receiving one, and
 * a received packet keeps its buffer until the main loop has handed it over.
 * So the pool running dry is ordinary, and the engine has to notice: arming
 * receive without a buffer faulted the CPU, which on this board means the LED
 * loop in fault.c and no answer to the host until it is re-plugged.
 */
MU_TEST(the_pool_hands_out_each_buffer_once_and_then_nothing)
{
	le_rx_t *held[LE_BUFFER_POOL_SIZE];
	int i, j;

	start_fresh();

	for (i = 0; i < LE_BUFFER_POOL_SIZE; i++) {
		held[i] = buffer_get();
		mu_assert_fatal(held[i] != NULL,
		                "the pool refused a buffer while one was free");
		for (j = 0; j < i; j++)
			mu_assert(held[i] != held[j],
			          "the pool handed the same buffer out twice");
	}

	mu_assert(buffer_get() == NULL,
	          "the pool handed out a buffer it had already given away, so "
	          "two packets would be received into the same one");
}

MU_TEST(a_released_buffer_comes_back_clean)
{
	le_rx_t *first, *again;
	int i;

	start_fresh();

	first = buffer_get();
	mu_assert_fatal(first != NULL, "the pool refused the first buffer");

	/* Leave it as a received packet does. */
	first->pos = 17;
	first->size = 42;
	first->rssi_sum = -500;
	first->rssi_min = -90;
	first->rssi_max = -10;
	memset(first->data, 0xa5, sizeof(first->data));

	for (i = 1; i < LE_BUFFER_POOL_SIZE; i++)
		mu_assert(buffer_get() != NULL, "the pool refused a free buffer");

	buffer_release(first);
	again = buffer_get();
	mu_assert_fatal(again == first,
	                "the only free buffer was not the one released");

	mu_assert_int_eq(0, again->pos);
	mu_assert_int_eq(0, again->size);
	mu_assert_int_eq(0, again->rssi_sum);
	mu_assert_int_eq(INT8_MAX, again->rssi_min);
	mu_assert_int_eq(INT8_MIN, again->rssi_max);
	for (i = 0; i < (int)sizeof(again->data); i++)
		if (again->data[i] != 0) {
			mu_assert(0, "a reused buffer still carries the last "
			             "packet's bytes, which would be sent to the "
			             "host as the head of the next one");
			break;
		}
}

/* ------------------------------------------------------------------ *
 * Whitening: the length of a packet
 * ------------------------------------------------------------------ */

/*
 * The DMA handler has to know how long a packet is after two bytes, which
 * means dewhitening the length byte on its own, before the rest of the packet
 * has arrived. le_phy.c does that with its own loop; bluetooth_le.c's
 * le_dewhiten() dewhitens a whole buffer. The two have to agree about the
 * second byte on every channel, or the engine receives the wrong number of
 * bytes -- too few and the packet is cut short, too many and it waits for
 * bytes that never come.
 */
MU_TEST(the_length_byte_is_dewhitened_the_same_way_as_the_rest)
{
	static const uint8_t bytes[] = { 0x00, 0x01, 0x27, 0x80, 0xa5, 0xff };
	uint8_t idx;
	size_t b;
	int compared = 0;

	for (idx = 0; idx < BTLE_CHANNELS; idx++) {
		unsigned channel = btle_channel_index_to_phys(idx);

		for (b = 0; b < sizeof(bytes); b++) {
			uint8_t pair[2];
			uint8_t own;

			pair[0] = 0x00;
			pair[1] = bytes[b];
			own = dewhiten_length(channel, pair[1]);
			le_dewhiten(pair, sizeof(pair), channel);
			compared++;
			if (pair[1] != own) {
				char msg[192];

				snprintf(msg, sizeof(msg),
				         "channel index %u (%u MHz), whitened byte 0x%02x: "
				         "the length is dewhitened as 0x%02x here and "
				         "0x%02x by le_dewhiten()", idx, channel,
				         bytes[b], own, pair[1]);
				mu_assert(0, msg);
			}
		}
	}

	mu_assert(compared == BTLE_CHANNELS * (int)sizeof(bytes),
	          "the comparison did not run on every channel");
}

/* ------------------------------------------------------------------ *
 * Reading a field out of a PDU
 * ------------------------------------------------------------------ */

/* Every connection parameter is read with this, so a wrong byte order here
 * would be a wrong value for all of them. */
MU_TEST(a_field_is_read_least_significant_byte_first)
{
	le_rx_t buf;

	fill_packet(&buf, 32, DATA_CHANNEL_MHZ);
	buf.data[4] = 0x78;
	buf.data[5] = 0x56;
	buf.data[6] = 0x34;
	buf.data[7] = 0x12;

	mu_assert_int_eq(0x78, extract_field(&buf, 4, 1));
	mu_assert_int_eq(0x5678, extract_field(&buf, 4, 2));
	mu_assert_int_eq(0x345678, extract_field(&buf, 4, 3));
	mu_assert_int_eq(0x12345678, extract_field(&buf, 4, 4));
}

/* ------------------------------------------------------------------ *
 * Handing a packet to the host
 * ------------------------------------------------------------------ */

/*
 * A record carries 50 bytes, so a long packet is sent as a chain of them and
 * the host puts it back together. The two halves have to agree exactly: the
 * record types that mark a chain, the total length packed into the two
 * reserved bytes, and the order of the fragments.
 *
 * So this is the round trip, with the real code on both sides: a receive
 * buffer through the firmware's usb_enqueue_le() into the firmware's ring,
 * out of it with dequeue() -- the path UBERTOOTH_POLL takes -- and into the
 * host's le_usb_reassemble(). What comes out has to be the packet that went
 * in, for every PDU length a packet can have. Nothing is modelled here;
 * test_le_frag_shape.c scanned both halves for the same agreement, and this
 * runs them against each other.
 */
/*
 * Records are reused, so what a packet does not fill is whatever the last
 * packet left there. This leaves the records a packet is about to land in
 * full of 0xa5, so a test that reads a record back is reading the new
 * packet's bytes and not the old ones.
 */
/*
 * Derived from the ring itself, not restated: ubertooth_usb.c is included
 * into this test and `fifo` is its ring, so changing the firmware's ring size
 * changes this too. A number copied here would go on claiming to wrap the
 * ring after the ring had grown.
 */
#define RING_RECORDS ((unsigned)(sizeof(fifo) / sizeof(fifo[0])))

static void dirty_the_ring(void)
{
	le_rx_t filler;
	unsigned i;

	/* A packet of exactly 50 bytes with the access address, so one record
	 * is filled end to end. */
	fill_packet(&filler, 41, DATA_CHANNEL_MHZ);
	mu_assert_int_eq(DMA_SIZE, 4 + filler.size);
	memset(filler.data, 0xa5, sizeof(filler.data));
	filler.access_address = 0xa5a5a5a5;
	filler.rssi_sum = (int)filler.size;

	/* Once around the whole ring, one record at a time, which leaves every
	 * record full and the queue's read and write positions back where they
	 * started. Re-initialising the queue instead would clear it. The result
	 * is asserted once, so a caller in a loop counts one check, not 256. */
	for (i = 0; i < RING_RECORDS; i++)
		if (usb_enqueue_le(&filler) != 1 || dequeue() == NULL)
			break;
	mu_assert_fatal(i == RING_RECORDS,
	                "the ring would not take the packets meant to dirty it");
}

static int round_trip(le_rx_t *buf, const uint8_t **data, uint16_t *length,
                      const usb_pkt_rx **metadata, int *records)
{
	static le_usb_reassembly state;
	usb_pkt_rx *record;
	int result = LE_REASSEMBLY_WAIT;
	/* These cases are about the bytes of a packet, not when it arrived, so
	 * every record is handed in with an arrival reading of zero and the one
	 * reported back is kept out of the way. */
	uint64_t ignored_arrival_ns;

	le_usb_reassembly_reset(&state);
	*records = 0;
	if (!usb_enqueue_le(buf))
		return -2;

	while ((record = dequeue()) != NULL) {
		(*records)++;
		result = le_usb_reassemble(&state, record, 0, data, length,
		                           metadata, &ignored_arrival_ns);
		if (result != LE_REASSEMBLY_WAIT)
			break;
	}
	/* Nothing may be left over: the chain ends with the packet. */
	if (dequeue() != NULL)
		return -3;
	return result;
}

MU_TEST(every_packet_length_survives_the_round_trip)
{
	unsigned pdu_len;

	for (pdu_len = 0; pdu_len <= 255; pdu_len++) {
		le_rx_t buf;
		const uint8_t *data = NULL;
		const usb_pkt_rx *metadata = NULL;
		uint16_t length = 0;
		unsigned total = 4 + pdu_len + 5;
		unsigned expected;
		int records = 0, result;
		char msg[224];

		start_fresh();
		dirty_the_ring();
		fill_packet(&buf, pdu_len, DATA_CHANNEL_MHZ);
		buf.access_address = 0x12345678;
		buf.rssi_sum = 46;       /* positive: see the test below */

		result = round_trip(&buf, &data, &length, &metadata, &records);

		snprintf(msg, sizeof(msg),
		         "a %u-byte PDU did not come back whole: the firmware put "
		         "%d records in the ring and the host answered %d",
		         pdu_len, records, result);
		mu_assert_fatal(result == LE_REASSEMBLY_COMPLETE, msg);

		/* One record for a short packet, a chain of 50-byte fragments
		 * for a long one. */
		snprintf(msg, sizeof(msg),
		         "a %u-byte PDU (%u bytes with the access address and CRC) "
		         "was sent as %d records", pdu_len, total, records);
		mu_assert((unsigned)records ==
		          (total + DMA_SIZE - 1) / DMA_SIZE, msg);

		/* A chain says how long the packet is, so the host hands back
		 * exactly that. A single record carries no length -- it never
		 * did -- so the host hands back all 50 of its bytes and the
		 * decoder reads the length out of the packet itself. */
		expected = records == 1 ? DMA_SIZE : total;
		snprintf(msg, sizeof(msg),
		         "a %u-byte PDU came back as %u bytes, not %u",
		         pdu_len, length, expected);
		mu_assert(length == expected, msg);

		/* The access address the packet was received with comes first,
		 * then the packet itself. */
		mu_assert_fatal(data != NULL, "the host returned no packet");
		mu_assert(memcmp(data, &buf.access_address, 4) == 0,
		          "the packet does not start with the access address");
		snprintf(msg, sizeof(msg),
		         "the bytes of a %u-byte PDU changed on the way", pdu_len);
		mu_assert(memcmp(data + 4, buf.data, buf.size) == 0, msg);

		/* Records are reused, so whatever a short packet leaves over in
		 * the one it travelled in is cleared rather than left as the
		 * last packet's bytes. */
		if (records == 1) {
			unsigned i;
			for (i = total; i < DMA_SIZE; i++) {
				snprintf(msg, sizeof(msg),
				         "byte %u after a %u-byte PDU was not cleared",
				         i, pdu_len);
				mu_assert(data[i] == 0, msg);
			}
		}

		/* ...and the readings the host describes the packet with are the
		 * ones taken while receiving it. */
		mu_assert_fatal(metadata != NULL, "the host returned no metadata");
		mu_assert_int_eq(0, metadata->clkn_high);
		mu_assert_int_eq(buf.timestamp, metadata->clk100ns);
		mu_assert_int_eq(DATA_CHANNEL_MHZ - 2402, metadata->channel);
		mu_assert_int_eq(buf.rssi_min, metadata->rssi_min);
		mu_assert_int_eq(buf.rssi_max, metadata->rssi_max);
		mu_assert_int_eq(0, metadata->rssi_count);
		mu_assert_int_eq(0, metadata->status);
		mu_assert_int_eq(0, radio_touched);
	}
}

/*
 * The longest packet the chain can carry is 264 bytes, which is exactly the
 * longest a receive buffer holds, so the engine's own packets always fit. A
 * buffer claiming to be longer is refused rather than sent as a chain the
 * host would reject half way through.
 */
MU_TEST(a_packet_too_long_for_a_chain_is_refused)
{
	le_rx_t buf;

	start_fresh();
	fill_packet(&buf, 255, DATA_CHANNEL_MHZ);
	mu_assert_int_eq(LE_MAX_USB_PACKET_SIZE, 4 + buf.size);

	buf.size += 1;
	mu_assert_int_eq(0, usb_enqueue_le(&buf));
	mu_assert(dequeue() == NULL,
	          "a packet too long to send left records in the ring");
}

/*
 * A chain is all or nothing. Half a packet is worse than none: the host drops
 * the chain and the records that did fit are wasted, so a packet that cannot
 * be sent whole is not started.
 */
MU_TEST(a_chain_that_does_not_fit_is_not_started)
{
	le_rx_t buf;
	unsigned fragments, space;

	start_fresh();
	fill_packet(&buf, 255, DATA_CHANNEL_MHZ);
	buf.rssi_sum = 46;
	fragments = (4 + buf.size + DMA_SIZE - 1) / DMA_SIZE;

	/* Fill the ring until one fragment less than the packet needs is left. */
	while (usb_queue_space() > fragments - 1)
		mu_assert_fatal(usb_enqueue() != NULL,
		                "the queue refused a packet while it had room");
	space = usb_queue_space();

	mu_assert_int_eq(0, usb_enqueue_le(&buf));
	mu_assert_int_eq(space, usb_queue_space());

	/* One fragment more and the whole chain goes in. */
	mu_assert_fatal(dequeue() != NULL, "the ring was expected to be full");
	mu_assert_int_eq(1, usb_enqueue_le(&buf));
}

/*
 * The average RSSI in the record is computed as rssi_sum / size, and size is
 * unsigned, so the sum is converted to unsigned before the division. Every
 * real capture has a negative sum -- the CC2400 reports RSSI as a negative
 * number -- which makes that division a very large positive one and the
 * average a value unrelated to the packet. The host puts it in the per-packet
 * header of a PCAP written with -r, where it is the only field this affects.
 *
 * This pins the behaviour rather than endorsing it: the fix belongs in the
 * firmware, and whoever makes it will have to come back here, which is the
 * point. The positive case is the arithmetic the code was meant to do.
 */
MU_TEST(the_average_rssi_is_the_mean_only_while_the_sum_is_positive)
{
	le_rx_t buf;
	usb_pkt_rx *record;
	int8_t expected_if_signed;

	start_fresh();
	fill_packet(&buf, 5, DATA_CHANNEL_MHZ);   /* size 10 */
	buf.rssi_sum = 200;
	mu_assert_fatal(usb_enqueue_le(&buf) == 1, "the queue refused a packet");
	record = dequeue();
	mu_assert_fatal(record != NULL, "no record came back");
	mu_assert_int_eq(20, record->rssi_avg);

	start_fresh();
	fill_packet(&buf, 5, DATA_CHANNEL_MHZ);
	buf.rssi_sum = -200;
	expected_if_signed = -20;
	mu_assert_fatal(usb_enqueue_le(&buf) == 1, "the queue refused a packet");
	record = dequeue();
	mu_assert_fatal(record != NULL, "no record came back");
	mu_assert(record->rssi_avg != expected_if_signed,
	          "the average RSSI of a packet with a negative sum is now the "
	          "mean. If that was deliberate, this test is what has to "
	          "change: say so and assert the mean");
}

/* A queue with no room left loses the packet rather than overwriting one the
 * host has not collected, and says so, which is what the engine's caller
 * would act on if it ever did. */
MU_TEST(a_full_ring_refuses_the_packet)
{
	le_rx_t buf;
	int queued = 0;

	start_fresh();
	fill_packet(&buf, 20, DATA_CHANNEL_MHZ);
	buf.rssi_sum = 25;

	while (usb_enqueue_le(&buf) == 1) {
		queued++;
		mu_assert_fatal(queued <= 1024,
		                "the queue never filled up, so it is not bounded");
	}

	mu_assert(queued > 0, "the queue refused the first packet");
	mu_assert_int_eq(0, usb_enqueue_le(&buf));

	/* Everything accepted is still there. */
	while (queued-- > 0)
		mu_assert_fatal(dequeue() != NULL,
		                "a packet the queue accepted is no longer in it");
	mu_assert(dequeue() == NULL, "the ring holds more than it accepted");
}

/* ------------------------------------------------------------------ *
 * A connection request
 * ------------------------------------------------------------------ */

/*
 * A CONNECT_IND is where following a connection starts, and the parameters in
 * it decide every deadline afterwards. The engine checks each one against the
 * range the specification allows, because a value outside it would leave the
 * capture chasing a connection that does not exist -- a supervision timeout of
 * hours, for instance, which nothing then ends.
 */
MU_TEST(a_connection_request_is_accepted_and_scheduled)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;
	uint32_t expected_match;

	start_fresh();
	le.do_follow = 1;
	build_connect_req(&buf, &r);
	fake_t1mcr = TMCR_MR2I;     /* the advertising-channel receive is armed */

	le_connect_handler(&buf);

	mu_assert_int_eq(0xdeadbeef, conn.access_address);
	mu_assert_int_eq(0x555555, conn.crc_init);
	mu_assert_int_eq(rbit(0x555555), conn.crc_init_reversed);
	mu_assert_int_eq(7, conn.hop_increment);
	/* 1.25 ms in units of 100 ns is 12500. */
	mu_assert_int_eq(24 * 12500, conn.conn_interval);
	mu_assert_int_eq(0, conn.win_offset);
	/* 10 ms in units of 100 ns is 100000. */
	mu_assert_int_eq(10 * 100000, conn.supervision_timeout);
	mu_assert_int_eq(37, conn.remapping.total_channels);

	mu_assert_int_eq(0, conn_event.num_packets);
	mu_assert_int_eq(0, conn_event.opened);

	/* The first connection event is timed from the end of this packet plus
	 * the transmit window, less the time the radio needs to warm up. */
	expected_match = buf.timestamp + PACKET_DURATION(&buf) +
	                 conn.win_offset + USEC(1250) - RX_WARMUP_TIME;
	mu_assert_int_eq(expected_match, fake_t1mr0);
	mu_assert(fake_t1mcr & TMCR_MR0I,
	          "the first connection event's deadline was not armed");
	mu_assert(!(fake_t1mcr & TMCR_MR2I),
	          "the advertising-channel receive is still armed, so the radio "
	          "would be re-armed on the advertising channel during the "
	          "connection");
	mu_assert_int_eq(0, radio_touched);
}

/* The host asks for connection following with `ubertooth-btle -f`; without
 * it the capture stays on the advertising channels. */
MU_TEST(a_connection_request_is_ignored_until_the_host_asks_to_follow)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;

	start_fresh();
	le.do_follow = 0;
	build_connect_req(&buf, &r);

	le_connect_handler(&buf);

	mu_assert(connection_was_reset(),
	          "a connection was followed without the host asking for it");
	mu_assert_int_eq(0, fake_t1mcr);
}

/*
 * Each parameter outside its range puts the engine back on the advertising
 * channels instead of following. One case per field, each one value past the
 * boundary, so a check removed or a comparison turned round fails here.
 */
MU_TEST(a_connection_request_outside_the_specification_is_refused)
{
	struct connect_req r;
	le_rx_t buf;
	size_t i;
	struct bad_case {
		const char *what;
		struct connect_req r;
	} cases[8];

	memset(cases, 0, sizeof(cases));

	r = valid_connect_req(); r.conn_interval = 5;
	cases[0].what = "a connection interval below the 7.5 ms minimum";
	cases[0].r = r;
	r = valid_connect_req(); r.conn_interval = 3201;
	cases[1].what = "a connection interval above the 4 s maximum";
	cases[1].r = r;
	r = valid_connect_req(); r.win_offset = 25;
	cases[2].what = "a transmit window offset past the connection interval";
	cases[2].r = r;
	r = valid_connect_req(); r.win_size = 0;
	cases[3].what = "a transmit window shorter than 1.25 ms";
	cases[3].r = r;
	r = valid_connect_req(); r.win_size = 9;
	cases[4].what = "a transmit window longer than 10 ms";
	cases[4].r = r;
	r = valid_connect_req(); r.supervision_timeout = 9;
	cases[5].what = "a supervision timeout below 100 ms";
	cases[5].r = r;
	r = valid_connect_req(); r.supervision_timeout = 3201;
	cases[6].what = "a supervision timeout above 32 s";
	cases[6].r = r;
	r = valid_connect_req(); memset(r.channel_map, 0, sizeof(r.channel_map));
	cases[7].what = "a channel map with no channel in it";
	cases[7].r = r;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char msg[256];

		start_fresh();
		le.do_follow = 1;
		build_connect_req(&buf, &cases[i].r);

		le_connect_handler(&buf);

		snprintf(msg, sizeof(msg),
		         "%s was accepted, so the capture would follow a "
		         "connection whose parameters cannot be right",
		         cases[i].what);
		mu_assert(connection_was_reset(), msg);
		snprintf(msg, sizeof(msg),
		         "%s was refused but a connection event was still armed",
		         cases[i].what);
		mu_assert(!(fake_t1mcr & TMCR_MR0I), msg);
	}
}

/* A PDU that is not the length of a CONNECT_IND is not one, and is left
 * alone: the fields are read at fixed offsets, so reading them out of a
 * shorter packet would be reading whatever the last packet left behind. */
MU_TEST(a_connection_request_of_the_wrong_length_is_left_alone)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;

	start_fresh();
	le.do_follow = 1;
	build_connect_req(&buf, &r);
	buf.size -= 1;

	le_connect_handler(&buf);

	mu_assert(connection_was_reset(),
	          "a packet too short to be a connection request was read as "
	          "one");
	mu_assert_int_eq(0, fake_t1mcr);
}

/* Following a new connection clears a cancel the host asked for earlier,
 * which would otherwise end the new connection at its first event. */
MU_TEST(a_new_connection_clears_a_pending_cancel)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;

	start_fresh();
	le.do_follow = 1;
	cancel_follow = 1;
	build_connect_req(&buf, &r);

	le_connect_handler(&buf);

	mu_assert_int_eq(0, cancel_follow);
	mu_assert_int_eq(0xdeadbeef, conn.access_address);
}

/* ------------------------------------------------------------------ *
 * Updates to a connection that is already being followed
 * ------------------------------------------------------------------ */

/* An update takes effect at a connection event the control PDU names, not
 * when it arrives, so the instant and the new parameters are both kept until
 * the counter reaches it. */
MU_TEST(a_connection_update_is_scheduled_for_its_instant)
{
	le_rx_t buf;

	start_fresh();
	fill_packet(&buf, 12, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	buf.data[3] = 2;              /* window size, units of 1.25 ms */
	put_le(&buf.data[4], 3, 2);   /* window offset */
	put_le(&buf.data[6], 40, 2);  /* new connection interval */
	put_le(&buf.data[10], 20, 2); /* new supervision timeout */
	put_le(&buf.data[12], 500, 2);/* instant */

	connection_update_handler(&buf);

	mu_assert_int_eq(3 * 12500, conn.win_offset);
	mu_assert_int_eq(40 * 12500, conn.conn_update_pending_interval);
	mu_assert_int_eq(20 * 100000,
	                 conn.conn_update_pending_supervision_timeout);
	mu_assert_int_eq(500, conn.conn_update_instant);
	mu_assert_int_eq(1, conn.conn_update_pending);
	/* The live parameters are untouched until the instant arrives. */
	mu_assert_int_eq(0, conn.conn_interval);
}

/*
 * The transmit window size of an update does not survive being scaled. The
 * field it is kept in is one byte, and the handler multiplies the value on
 * the wire -- units of 1.25 ms -- into it, so anything above 2 keeps only the
 * low eight bits: a 2.5 ms window becomes 16.8 microseconds. The timer match
 * that waits out the update's transmit window is then short by nearly all of
 * it, so the capture can miss the first packets of the new parameters.
 *
 * As with the average RSSI above, this pins the behaviour rather than
 * endorsing it: the fix is a wider field in the firmware, and whoever makes
 * it has to come back here.
 */
MU_TEST(an_updates_window_size_does_not_survive_being_scaled)
{
	le_rx_t buf;

	start_fresh();
	fill_packet(&buf, 12, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	buf.data[3] = 2;              /* 2.5 ms, in units of 1.25 ms */

	connection_update_handler(&buf);

	mu_assert(sizeof(conn.win_size) < sizeof(uint32_t),
	          "the update's window size is now kept in a field wide enough "
	          "for the scaled value. If that was the fix, this test is what "
	          "has to change: assert the scaled value instead");
	mu_assert_int_eq((uint8_t)(2 * 12500), conn.win_size);
}

MU_TEST(a_channel_map_update_is_scheduled_for_its_instant)
{
	le_rx_t buf;

	start_fresh();
	fill_packet(&buf, 8, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	/* Two channels left in use: indices 0 and 9. */
	buf.data[3] = 0x01;
	buf.data[4] = 0x02;
	put_le(&buf.data[8], 1234, 2);

	channel_map_update_handler(&buf);

	mu_assert_int_eq(1, conn.channel_map_update_pending);
	mu_assert_int_eq(1234, conn.channel_map_update_instant);
	mu_assert_int_eq(2, conn.pending_remapping.total_channels);
	mu_assert_int_eq(0, conn.pending_remapping.remapping_index[0]);
	mu_assert_int_eq(9, conn.pending_remapping.remapping_index[1]);
	/* ...and the map in use is not changed until the instant. */
	mu_assert_int_eq(0, conn.remapping.total_channels);
}

/* ------------------------------------------------------------------ *
 * Which packets are acted on
 * ------------------------------------------------------------------ */

/*
 * The engine acts on three packets and ignores the rest. Getting the length
 * guards wrong is the dangerous half: each handler reads fields at fixed
 * offsets, so a control PDU of the wrong length read as an update would take
 * its parameters from whatever followed the packet in the buffer.
 */
MU_TEST(only_the_packets_the_engine_knows_are_acted_on)
{
	le_rx_t buf;

	/* A connection request on an advertising channel starts a connection. */
	start_fresh();
	le.do_follow = 1;
	{
		struct connect_req r = valid_connect_req();

		build_connect_req(&buf, &r);
		packet_handler(&buf);
		mu_assert_int_eq(0xdeadbeef, conn.access_address);
	}

	/* The same request on a data channel is not one: the dispatch keys on
	 * the channel the packet arrived on. */
	start_fresh();
	le.do_follow = 1;
	{
		struct connect_req r = valid_connect_req();

		build_connect_req(&buf, &r);
		buf.channel = DATA_CHANNEL_MHZ;
		packet_handler(&buf);
		mu_assert(connection_was_reset(),
		          "a data-channel packet was read as a connection request");
	}

	/* Another advertising PDU type is ignored. */
	start_fresh();
	le.do_follow = 1;
	{
		struct connect_req r = valid_connect_req();

		build_connect_req(&buf, &r);
		buf.data[0] = 0x00;        /* ADV_IND */
		packet_handler(&buf);
		mu_assert(connection_was_reset(),
		          "an advertising packet that is not a connection request "
		          "started a connection");
	}

	/* A control PDU on a data channel: an update of the right length is
	 * taken... */
	start_fresh();
	fill_packet(&buf, 13, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	buf.data[0] = 0x03;    /* LL control PDU */
	buf.data[1] = 12;      /* length of LL_CONNECTION_UPDATE_IND */
	buf.data[2] = 0x00;    /* its opcode */
	put_le(&buf.data[12], 77, 2);
	packet_handler(&buf);
	mu_assert_int_eq(1, conn.conn_update_pending);
	mu_assert_int_eq(77, conn.conn_update_instant);

	/* ...and one of the wrong length is not. */
	start_fresh();
	fill_packet(&buf, 13, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	buf.data[0] = 0x03;
	buf.data[1] = 11;      /* one byte short */
	buf.data[2] = 0x00;
	packet_handler(&buf);
	mu_assert_int_eq(0, conn.conn_update_pending);

	/* A channel map update, the same way. */
	start_fresh();
	fill_packet(&buf, 9, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	buf.data[0] = 0x03;
	buf.data[1] = 8;
	buf.data[2] = 0x01;    /* LL_CHANNEL_MAP_IND */
	buf.data[3] = 0x01;
	put_le(&buf.data[8], 88, 2);
	packet_handler(&buf);
	mu_assert_int_eq(1, conn.channel_map_update_pending);
	mu_assert_int_eq(88, conn.channel_map_update_instant);

	start_fresh();
	fill_packet(&buf, 9, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	buf.data[0] = 0x03;
	buf.data[1] = 7;       /* one byte short */
	buf.data[2] = 0x01;
	packet_handler(&buf);
	mu_assert_int_eq(0, conn.channel_map_update_pending);

	/*
	 * A data PDU that is not an LL control PDU is left alone, whatever its
	 * third byte looks like. Every case above sets the control LLID, so
	 * without this one the LLID test could be dropped from the engine and
	 * an ordinary data packet carrying 0x00 there would be read as a
	 * connection update.
	 */
	start_fresh();
	fill_packet(&buf, 13, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	buf.data[0] = 0x02;    /* an L2CAP data PDU, not an LL control PDU */
	buf.data[1] = 12;
	buf.data[2] = 0x00;    /* what the opcode would be, if this were one */
	put_le(&buf.data[12], 99, 2);
	packet_handler(&buf);
	mu_assert_int_eq(0, conn.conn_update_pending);

	mu_assert_int_eq(0, radio_touched);
}

/* ------------------------------------------------------------------ *
 * The target filter
 * ------------------------------------------------------------------ */

/*
 * `ubertooth-btle -t` narrows a capture to one device. The filter reads the
 * addresses out of the advertising PDU, so which PDU types carry one address
 * and which carry two is part of it -- and a PDU too short to hold the
 * address it is supposed to is dropped rather than read past.
 */
MU_TEST(a_target_filters_advertising_packets_only)
{
	le_rx_t buf;
	int i;

	/*
	 * With no target asked for, everything is kept. The target and its
	 * mask are set first and only the flag left clear, because an all-zero
	 * mask matches every address: without a real target here, the engine
	 * could stop checking the flag and this would still pass, while a
	 * plain `ubertooth-btle -n` quietly dropped every advertising PDU of a
	 * type the filter cannot read.
	 */
	start_fresh();
	for (i = 0; i < 6; i++) {
		le.target[i] = (uint8_t)(0x10 + i);
		le.target_mask[i] = 0xff;
	}
	fill_packet(&buf, 20, ADV_CHANNEL_MHZ);
	buf.data[0] = 0x00;
	mu_assert(filter_match(&buf) == 1,
	          "a packet was dropped with no target set");

	le.target_set = 1;

	/* A data-channel packet is part of the connection being followed, so it
	 * is kept whatever the target is. */
	fill_packet(&buf, 20, DATA_CHANNEL_MHZ);
	mu_assert(filter_match(&buf) == 1,
	          "a data-channel packet was dropped by the target filter");

	/* ADV_IND carries one address, at the start of the payload. */
	fill_packet(&buf, 6, ADV_CHANNEL_MHZ);
	buf.data[0] = 0x00;
	memcpy(&buf.data[2], le.target, 6);
	mu_assert(filter_match(&buf) == 1, "the target's own advertising was "
	                                   "dropped");
	buf.data[2] ^= 0xff;
	mu_assert(filter_match(&buf) == 0, "another device's advertising was "
	                                   "kept");

	/* CONNECT_IND carries two, and either one matching is a match. */
	fill_packet(&buf, 6 + 6 + 22, ADV_CHANNEL_MHZ);
	buf.data[0] = 0x05;
	memcpy(&buf.data[8], le.target, 6);
	mu_assert(filter_match(&buf) == 1,
	          "a connection request to the target was dropped");
	buf.data[8] ^= 0xff;
	mu_assert(filter_match(&buf) == 0,
	          "a connection request between two other devices was kept");

	/*
	 * Too short to hold the address the type says it has. The target's own
	 * bytes are planted where the handler would read them, so a read that
	 * ran past the end of the PDU would come back a match: that is what
	 * makes these two cases test the length guards rather than the
	 * mismatch that would follow from any other filling.
	 */
	fill_packet(&buf, 2, ADV_CHANNEL_MHZ);
	buf.data[0] = 0x00;
	memcpy(&buf.data[2], le.target, 6);
	mu_assert(filter_match(&buf) == 0,
	          "an advertising packet too short to hold an address was read "
	          "for one");
	fill_packet(&buf, 6, ADV_CHANNEL_MHZ);
	buf.data[0] = 0x05;
	memcpy(&buf.data[2], le.target, 6);
	memcpy(&buf.data[8], le.target, 6);
	mu_assert(filter_match(&buf) == 0,
	          "a connection request too short to hold both addresses was "
	          "read for them");

	/* An unknown advertising type carries no address this can read. */
	fill_packet(&buf, 20, ADV_CHANNEL_MHZ);
	buf.data[0] = 0x0b;
	mu_assert(filter_match(&buf) == 0,
	          "an advertising type the filter cannot read was kept while a "
	          "target was set");

	/* A mask of zeros is how the host asks for a prefix of the address;
	 * all-zero means every device matches. */
	memset(le.target_mask, 0, sizeof(le.target_mask));
	memset(le.target, 0, sizeof(le.target));
	fill_packet(&buf, 6, ADV_CHANNEL_MHZ);
	buf.data[0] = 0x00;
	mu_assert(filter_match(&buf) == 1,
	          "an empty target mask dropped a packet");
}

/* ------------------------------------------------------------------ *
 * The anchor point of a connection event
 * ------------------------------------------------------------------ */

/*
 * Every deadline in a followed connection is measured from the anchor, and the
 * anchor is re-derived at the end of each connection event. Which packet it
 * can be taken from is the whole of it: the master's packet starts the event,
 * the slave's does not, and the engine tells them apart by how far the
 * observed time is from the one it predicted. Get this wrong and the capture
 * drifts off the connection a few events later.
 *
 * None of these cases may reach the radio, so each asserts that it did not;
 * the two that would -- a supervision timeout and a cancelled follow -- change
 * channel, which is hardware code.
 */
static void anchored_connection(uint32_t last_anchor)
{
	start_fresh();
	conn.access_address = 0x12345678;
	conn.conn_interval = 24 * 12500;
	conn.supervision_timeout = SEC(32);
	conn.last_anchor = last_anchor;
	conn.anchor_set = 1;
	conn.last_packet_ts = 1000;
	fake_t1tc = 1000;
	conn.conn_event_counter = 5;
}

MU_TEST(two_packets_in_an_event_anchor_it_where_they_arrived)
{
	anchored_connection(100000);

	conn_event.anchor = 123456;
	conn_event.num_packets = 2;
	conn_event.opened = 1;

	finish_conn_event();

	mu_assert_int_eq(123456, conn.last_anchor);
	mu_assert_int_eq(1, conn.anchor_set);
	mu_assert_int_eq(6, conn.conn_event_counter);
	mu_assert_int_eq(0, conn_event.num_packets);
	mu_assert_int_eq(0, conn_event.opened);
	mu_assert_int_eq(123456 + 24 * 12500 - RX_WARMUP_TIME, fake_t1mr0);
	mu_assert(fake_t1mcr & TMCR_MR0I, "the next hop was not armed");
	mu_assert_int_eq(0, radio_touched);
}

MU_TEST(one_packet_anchors_the_event_only_near_the_prediction)
{
	uint32_t predicted;

	/* Close enough to be the master's packet: the anchor moves to it. */
	anchored_connection(100000);
	predicted = conn.last_anchor + conn.conn_interval;
	conn_event.anchor = predicted - (ANCHOR_EPSILON - 10);
	conn_event.num_packets = 1;

	finish_conn_event();

	mu_assert_int_eq(predicted - (ANCHOR_EPSILON - 10), conn.last_anchor);
	mu_assert_int_eq(0, radio_touched);

	/* Too far: it was the slave, so the prediction stands. */
	anchored_connection(100000);
	predicted = conn.last_anchor + conn.conn_interval;
	conn_event.anchor = predicted - (ANCHOR_EPSILON + 10);
	conn_event.num_packets = 1;

	finish_conn_event();

	mu_assert_int_eq(predicted, conn.last_anchor);
	mu_assert_int_eq(0, radio_touched);
}

MU_TEST(a_silent_event_moves_the_anchor_on_by_one_interval)
{
	anchored_connection(100000);
	conn_event.num_packets = 0;
	conn_event.opened = 1;
	/* Away from the recorded time of the last packet, so that recording the
	 * current time here would be visible. With the two equal, the engine
	 * could stamp every silent event and this would still pass. */
	fake_t1tc = 2000;

	finish_conn_event();

	mu_assert_int_eq(100000 + 24 * 12500, conn.last_anchor);
	mu_assert_int_eq(1, conn.anchor_set);
	/* The supervision timeout runs from the last packet, which this event
	 * did not have, so that time must not move. */
	mu_assert_int_eq(1000, conn.last_packet_ts);
	mu_assert_int_eq(6, conn.conn_event_counter);
	mu_assert_int_eq(0, radio_touched);
}

/* The first event of a connection has no anchor to predict from, so the one
 * observed becomes it. */
MU_TEST(the_first_event_takes_its_anchor_from_the_packet)
{
	anchored_connection(100000);
	conn.anchor_set = 0;
	conn.last_anchor = 0;
	conn_event.anchor = 55555;
	conn_event.num_packets = 1;
	fake_t1tc = 2000;

	finish_conn_event();

	mu_assert_int_eq(55555, conn.last_anchor);
	mu_assert_int_eq(2000, conn.last_packet_ts);
	mu_assert_int_eq(0, radio_touched);
}

/* ------------------------------------------------------------------ *
 * A torn-down PHY
 * ------------------------------------------------------------------ */

/*
 * Both interrupt handlers belong to this PHY, and both can be entered after it
 * has been torn down: an interrupt already pending when the line is masked is
 * still taken, because a write to the interrupt controller's clear-enable
 * register is not self-synchronising on this part. Acting on one strobed the
 * radio back on underneath the main loop's own use of the bit-banged bus, and
 * left the board enumerated and answering nothing until it was re-plugged.
 *
 * The flag is what makes such an interrupt harmless, and this is the part of
 * the handlers that can be run here: entered with the flag clear, each one
 * must acknowledge and return, touching neither the radio nor the state the
 * mode no longer owns. test_le_phy_order.c checks the order of the writes to
 * the flag; this checks what the handlers then do.
 */
MU_TEST(a_torn_down_phy_ignores_a_dma_interrupt)
{
	start_fresh();
	le_phy_active = 0;
	current_rxbuf = NULL;

	/* A byte has arrived on channel 0, as far as the controller knows. */
	fake_dmac_int_stat = 1u << 0;
	fake_dmac_int_tc_stat = 1u << 0;
	fake_dmacc0_config = 0x1234;

	le_DMA_IRQHandler();

	mu_assert_int_eq(1, dma_clear_calls);
	mu_assert_int_eq(0, radio_touched);
	mu_assert_int_eq(0, ssp_starts);
	mu_assert(current_rxbuf == NULL,
	          "a torn-down PHY took a buffer out of the pool");
	mu_assert_int_eq(0x1234, fake_dmacc0_config);
	mu_assert(queue_remove(&packet_queue, (void **)&current_rxbuf) == 0,
	          "a torn-down PHY queued a packet");
}

MU_TEST(a_torn_down_phy_ignores_a_timer_match)
{
	start_fresh();
	le_phy_active = 0;
	current_rxbuf = NULL;

	/* Every match armed and every one of them reporting. */
	fake_t1mcr = TMCR_MR0I | TMCR_MR1I | TMCR_MR2I | TMCR_MR3I;
	fake_t1ir = 0x0f;

	TIMER1_IRQHandler();

	mu_assert_int_eq(0, fake_t1mcr);
	mu_assert_int_eq(0xff, fake_t1ir);
	mu_assert_int_eq(0, radio_touched);
	mu_assert_int_eq(0, ssp_starts);
	mu_assert(current_rxbuf == NULL,
	          "a torn-down PHY took a buffer out of the pool");
	mu_assert_int_eq(0, fake_t1mr0);
}

int main(void)
{
	printf("le_phy capture path tests\n");

	MU_RUN(the_pool_hands_out_each_buffer_once_and_then_nothing);
	MU_RUN(a_released_buffer_comes_back_clean);
	MU_RUN(the_length_byte_is_dewhitened_the_same_way_as_the_rest);
	MU_RUN(a_field_is_read_least_significant_byte_first);
	MU_RUN(every_packet_length_survives_the_round_trip);
	MU_RUN(a_packet_too_long_for_a_chain_is_refused);
	MU_RUN(a_chain_that_does_not_fit_is_not_started);
	MU_RUN(the_average_rssi_is_the_mean_only_while_the_sum_is_positive);
	MU_RUN(a_full_ring_refuses_the_packet);
	MU_RUN(a_connection_request_is_accepted_and_scheduled);
	MU_RUN(a_connection_request_is_ignored_until_the_host_asks_to_follow);
	MU_RUN(a_connection_request_outside_the_specification_is_refused);
	MU_RUN(a_connection_request_of_the_wrong_length_is_left_alone);
	MU_RUN(a_new_connection_clears_a_pending_cancel);
	MU_RUN(a_connection_update_is_scheduled_for_its_instant);
	MU_RUN(an_updates_window_size_does_not_survive_being_scaled);
	MU_RUN(a_channel_map_update_is_scheduled_for_its_instant);
	MU_RUN(only_the_packets_the_engine_knows_are_acted_on);
	MU_RUN(a_target_filters_advertising_packets_only);
	MU_RUN(two_packets_in_an_event_anchor_it_where_they_arrived);
	MU_RUN(one_packet_anchors_the_event_only_near_the_prediction);
	MU_RUN(a_silent_event_moves_the_anchor_on_by_one_interval);
	MU_RUN(the_first_event_takes_its_anchor_from_the_packet);
	MU_RUN(a_torn_down_phy_ignores_a_dma_interrupt);
	MU_RUN(a_torn_down_phy_ignores_a_timer_match);

	return mu_report();
}
