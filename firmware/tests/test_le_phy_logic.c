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
 * What is and is not asserted here, and why:
 *
 *   What a packet holds depends on what a radio put there, so nothing here
 *   asserts a decoded value that a real transfer would have produced: a test
 *   that fed itself those bytes would be asserting what the test wrote. The
 *   radio's own settings -- the registers le_cc2400_init_rf() writes, the
 *   strobes it and le_cc2400_strobe_rx() send, the channel change_channel()
 *   tunes -- are not asserted either, because a fake register file lets that
 *   code compile, not work. Those stay covered by test_le_phy_order.c's scan
 *   and by hardware testing. le_phy_main() and le_sys_init() are not run at
 *   all.
 *
 *   What the receive and timer interrupts do run here, including the paths
 *   that reach change_channel() and le_cc2400_init_rf(), is the engine's own
 *   bookkeeping: the timestamp a packet's first byte is dated by, which match
 *   each path arms, which interrupts and transfers it enables, and what
 *   becomes of a packet when a transfer fails. Those are decisions made from
 *   numbers the test supplies, so running them asserts the engine's
 *   arithmetic and its state machine rather than the test's own bytes. Where
 *   a path reaches the radio, the fakes count the accesses and the test says
 *   how many it expects, so radio work the engine should not be doing shows
 *   up.
 *
 * Both interrupt handlers' first line is run here too: once the PHY has been
 * torn down they must do nothing but acknowledge.
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
volatile uint32_t fake_t2ir, fake_t2tcr, fake_t2tc, fake_t2pr, fake_t2mcr;
volatile uint32_t fake_pconp;
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
/* What the radio reports when the code under test reads its status. Zero --
 * no FS_LOCK -- unless a test says otherwise, which is what lets a test follow
 * a wait for the synthesizer through to the moment receive is armed again. */
static u8 radio_status;
u8 cc2400_status(void) { radio_touched++; return radio_status; }
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

/* Stop the SSP asking the controller for bytes, which the firmware does in one
 * guarded place because the loop and both handlers change that register a bit
 * at a time (ubertooth_dma.c). That file is not compiled here, so this stands
 * in for its effect, and test_le_phy_order.c's scan is what refuses a site
 * that reaches the register directly. */
void dio_ssp_rx_dma_disable(void)
{
	fake_ssp1dmacr &= ~SSPDMACR_RXDMAE;
}
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

/* ------------------------------------------------------------------ *
 * The watchdog
 * ------------------------------------------------------------------ */

/* The engine's loop and every USB service pass tell the watchdog they are
 * alive, and mark which wait they are in so a reset can say where the board
 * died. Nothing here is about that, so these only have to exist; what the
 * firmware does with them is tested in test_watchdog.c and test_usb_queue.c. */
uint8_t watchdog_mark(uint8_t site) { (void)site; return WATCHDOG_SITE_NONE; }
uint8_t watchdog_mark_inner(uint8_t site) { (void)site; return WATCHDOG_SITE_NONE; }
void watchdog_unmark(uint8_t site) { (void)site; }
void watchdog_usb_serviced(void) {}

/* Counted rather than stubbed away: one test drives a failed capture
 * transfer and asserts the engine counts it. */
static unsigned transfer_errors_counted;
void watchdog_count_transfer_error(void) { transfer_errors_counted++; }

/* How many transfer errors the engine has acknowledged to the controller,
 * counted where the fake controller's error flag is cleared. */
static unsigned errors_acknowledged;

/* ------------------------------------------------------------------ *
 * Interrupts the engine holds off
 * ------------------------------------------------------------------ */

/*
 * The engine guards the few places where it reads a value, changes it and
 * writes it back -- the buffer pool's "taken" flag, the timer's match-enable
 * register, and the handover of a finished packet -- because the main loop and
 * both interrupt handlers reach all three. On the board the guard sets
 * PRIMASK; here stubs-lephy/irq.h stands in for it, keeping a depth count and
 * running one hook as the outermost guarded section ends.
 *
 * That hook is how a test stands in for an interrupt. An interrupt raised
 * while PRIMASK is set is latched and taken the instant the mask comes off, so
 * the end of the section is where the hardware would run the handler. A test
 * sets the hook to the work a handler would do and then asserts that both the
 * handler's effect and the guarded section's effect survived.
 */
unsigned irq_mask_depth;
void (*irq_pending_hook)(void);

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
	radio_status = 0;
	transfer_errors_counted = 0;
	dma_clear_calls = 0;
	ssp_starts = 0;
	fake_t1mcr = 0;
	fake_t1mr0 = 0;
	fake_t1mr1 = 0;
	fake_t1mr2 = 0;
	fake_t1mr3 = 0;
	fake_t1tc = 0;
	fake_t2ir = 0;
	fake_t2tcr = 0;
	fake_t2tc = 0;
	fake_t2pr = 0;
	fake_t2mcr = 0;
	fake_pconp = 0;
	fake_iser0 = 0;
	fake_icer0 = 0;
	fake_dmac_int_err_stat = 0;
	fake_dmac_int_err_clr = 0;
	transfer_errors_counted = 0;
	errors_acknowledged = 0;
	memset(&le, 0, sizeof(le));
	le_packet_dropped = 0;
	usb_queue_init();
	buffers_init();
	queue_init(&packet_queue);
	current_rxbuf = NULL;
	le_phy_active = 0;
	/* The engine starts on an advertising channel. Reset here as well as in
	 * the tests that care, so no test depends on what the one before it
	 * tuned. */
	rf_channel = ADV_CHANNEL_MHZ;
	le_sync_aa = ADV_AA;
	cancel_follow = 0;
	reset_conn();
	reset_conn_event();
	irq_mask_depth = 0;
	irq_pending_hook = NULL;
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

/* The byte that carries this length through the channel's whitening, so a
 * packet driven through the receive interrupt says how long it is. */
static uint8_t whitened_length_byte(unsigned channel, uint8_t length)
{
	unsigned b;

	for (b = 0; b < 256; b++)
		if (dewhiten_length(channel, (uint8_t)b) == length)
			return (uint8_t)b;
	mu_assert(0, "no byte dewhitens to the length asked for");
	return 0;
}

/*
 * Drive a whole packet through the receive interrupt, a byte per interrupt as
 * the controller reports them, and hand back the packet the engine queued for
 * the host. NULL if it queued none.
 *
 * `fault_at` is the byte whose interrupt also reports a transfer error, or -1
 * for none. The error and the terminal count are separate flags and the
 * hardware can set both for one interrupt, including on the byte that
 * completes the packet. `bytes` stops short of the whole packet, for a fault
 * that ends it part way through; 0 feeds all of it.
 *
 * The caller releases the packet when it is done with it, as the mode's loop
 * does, or the pool of four runs out after two packets.
 */
static le_rx_t *a_packet_arrives_with_a_fault_at(unsigned pdu_len,
                                                 unsigned channel,
                                                 int fault_at, unsigned bytes)
{
	unsigned total = bytes != 0 ? bytes : pdu_len + 2 + 3;
	unsigned i;
	void *packet = NULL;

	rf_channel = channel;
	le_phy_active = 1;
	if (current_rxbuf == NULL) {
		current_rxbuf = buffer_get();
		mu_assert(current_rxbuf != NULL, "the pool gave out no buffer");
		if (current_rxbuf == NULL)
			return NULL;
	}

	for (i = 0; i < total; i++) {
		fake_dmac_int_stat = 1 << 0;
		fake_dmac_int_tc_stat = 1 << 0;
		if (fault_at >= 0 && (unsigned)fault_at == i)
			fake_dmac_int_err_stat |= 1 << 0;
		/* The second byte is the header's length field; the rest of the
		 * packet is any bytes at all. */
		le_dma_dest[i & 1] = (i == 1)
			? whitened_length_byte(channel, (uint8_t)pdu_len)
			: 0x5a;
		le_dma_interrupt(0x00100000 + i);

		/* The controller's error flag stays set until the firmware writes
		 * the clear register, as the real one does, so a test can tell an
		 * error the engine acknowledged from one it left latched. */
		if (fake_dmac_int_err_clr & (1 << 0)) {
			fake_dmac_int_err_stat &= ~(1 << 0);
			fake_dmac_int_err_clr &= ~(1 << 0);
			errors_acknowledged++;
		}
	}

	if (!queue_remove(&packet_queue, &packet))
		return NULL;
	return (le_rx_t *)packet;
}

/* The same, with the fault on the byte that completes the packet -- the case
 * these tests are about -- or with no fault at all. */
static le_rx_t *a_packet_arrives(unsigned pdu_len, unsigned channel,
                                 int fault_on_last)
{
	int last = (int)(pdu_len + 2 + 3) - 1;

	return a_packet_arrives_with_a_fault_at(pdu_len, channel,
	                                        fault_on_last ? last : -1, 0);
}

/* The status word the host reads on the record for a packet the engine
 * queued, which is where the readings and the flags for it arrive. */
static uint8_t le_status_read_for(le_rx_t *packet)
{
	usb_pkt_rx *record;

	mu_assert(usb_enqueue_le(packet) == 1,
	          "the ring would not take the packet");
	record = dequeue();
	mu_assert(record != NULL, "the packet never reached the host");
	return record == NULL ? 0 : record->status;
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

/* The fields of an LL_CONNECTION_UPDATE_IND, as the engine reads them out of
 * the control PDU. */
struct conn_update {
	uint8_t  win_size;            /* units of 1.25 ms */
	uint16_t win_offset;          /* units of 1.25 ms */
	uint16_t conn_interval;       /* units of 1.25 ms */
	uint16_t latency;
	uint16_t supervision_timeout; /* units of 10 ms */
	uint16_t instant;
};

/* An update that satisfies every range the engine checks: a 50 ms connection
 * interval, a 3.75 ms transmit window offset, a 2.5 ms transmit window and a
 * 200 ms supervision timeout, taking effect at connection event 7. */
static struct conn_update valid_conn_update(void)
{
	struct conn_update u;

	memset(&u, 0, sizeof(u));
	u.win_size = 2;
	u.win_offset = 3;
	u.conn_interval = 40;
	u.latency = 0;
	u.supervision_timeout = 20;
	u.instant = 7;
	return u;
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

/*
 * Lay an LL_CONNECTION_UPDATE_IND out as it arrives on a data channel: the
 * PDU header, the opcode, then the new parameters. The engine reads each
 * field at a fixed offset, so this is also what pins those offsets.
 */
static void build_conn_update(le_rx_t *buf, const struct conn_update *u)
{
	fill_packet(buf, 12, DATA_CHANNEL_MHZ);
	memset(buf->data, 0, sizeof(buf->data));
	buf->data[0] = 0x03;                  /* LL control PDU */
	buf->data[1] = 12;                    /* payload length */
	buf->data[2] = 0x00;                  /* LL_CONNECTION_UPDATE_IND */
	buf->data[3] = u->win_size;
	put_le(&buf->data[4], u->win_offset, 2);
	put_le(&buf->data[6], u->conn_interval, 2);
	put_le(&buf->data[8], u->latency, 2);
	put_le(&buf->data[10], u->supervision_timeout, 2);
	put_le(&buf->data[12], u->instant, 2);
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
 * What an interrupt may not cut in half
 * ------------------------------------------------------------------ */

/*
 * Three pieces of the engine's state are changed by reading a value, altering
 * it and writing it back, and all three are reached from the main loop and
 * from both interrupt handlers. The timer interrupt is the one that preempts:
 * the byte interrupt is put at the lowest priority on purpose and nothing
 * changes the timer's, which leaves it at the highest the part starts with, so
 * the timer cuts into the byte interrupt as well as into the loop.
 *
 * Each test below sets the hook to what one of the handlers does, so the
 * stand-in mask runs it where the hardware would -- as the guarded section
 * ends -- and then asserts that both sides' work survived. Each also asserts
 * the hook ran at all: code that does its read-modify-write unguarded never
 * ends a section, so the hook would never be reached, and the assertion that
 * notices is the one that fails.
 */

/* What the byte interrupt does when a packet ends: start a wait for the
 * synthesizer to lock, which arms match 2. */
static void arm_a_wait_for_the_synthesizer(void)
{
	timer1_wait_fs_lock();
}

/* What the timer interrupt does when the buffer pool was dry: arm match 3 to
 * look again shortly. */
static void arm_a_wait_for_a_buffer(void)
{
	timer1_wait_buffer();
}

/* What the buffer wait and the channel change do: take a buffer from the
 * pool. */
static le_rx_t *the_buffer_the_handler_took;
static void take_a_buffer_from_the_pool(void)
{
	the_buffer_the_handler_took = buffer_get();
}

/* What a hop does to the buffer the engine is holding: clear it for the next
 * packet (change_channel()). Reached from the connection-event match. */
static void clear_the_buffer_the_engine_holds(void)
{
	if (current_rxbuf != NULL)
		buffer_clear(current_rxbuf);
}

/*
 * The LED blink runs in the mode's loop, once for every packet handed to the
 * host. It has a counter of its own, timer2, so the loop never writes the
 * timer the radio's own deadlines live in.
 *
 * The three tests below pin that: the blink leaves timer1 exactly as it was,
 * its counter arms no match of its own, and it still lasts the 10 ms it
 * always did.
 */
MU_TEST(the_blink_leaves_the_radios_timer_alone)
{
	start_fresh();
	blink_timer_start();
	fake_t1tc = 1000;

	/* A synthesizer wait, as the byte interrupt leaves one. */
	arm_a_wait_for_the_synthesizer();
	mu_assert_fatal(fake_t1mcr & TMCR_MR2I, "the wait was not armed");

	{
		uint32_t mcr = fake_t1mcr, mr1 = fake_t1mr1;

		blink(1, 0, 0);

		mu_assert_int_eq((int)mcr, (int)fake_t1mcr);
		mu_assert_int_eq((int)mr1, (int)fake_t1mr1);
		mu_assert(fake_t1mcr & TMCR_MR2I,
		          "the blink lost the synthesizer wait, so nothing is left "
		          "to arm receive and the capture goes silent");
	}

	/* Nothing to guard, because nothing shared is read and written back. */
	mu_assert_int_eq(0, (int)irq_mask_depth);
}

MU_TEST(the_blinks_counter_arms_no_match)
{
	start_fresh();

	blink_timer_start();

	mu_assert(fake_pconp & PCONP_PCTIM2, "timer2 was left unpowered");
	mu_assert_int_eq(TCR_Counter_Enable, (int)fake_t2tcr);
	/* clock_start() puts timer2 on the same peripheral clock divider as
	 * timer1, so timer1's prescaler makes a tick of the same length here
	 * and MSEC() means the same on either counter. */
	mu_assert_int_eq(4, (int)fake_t2pr);
	mu_assert_int_eq(0, (int)fake_t2mcr);
	/* A flag the xmas demo latched before this mode started is cleared, so
	 * nothing of timer2 is left waiting at the interrupt controller. */
	mu_assert_int_eq(0xff, (int)fake_t2ir);
	mu_assert(!(fake_iser0 & ISER0_ISE_TIMER2),
	          "timer2's interrupt was unmasked, which would put the blink "
	          "back into a handler that can cut into the radio's timing");

	blink_timer_stop();
	mu_assert(!(fake_pconp & PCONP_PCTIM2), "timer2 was left powered");
}

MU_TEST(the_blink_still_lasts_ten_milliseconds)
{
	start_fresh();
	blink_timer_start();
	fake_t2tc = 7000;

	blink(0, 1, 0);
	mu_assert_int_eq((int)PIN_RXLED, (int)fake_fio1set);

	/* The clear registers here are write-only, as the part's are: only the
	 * last mask written to one is visible, so what a test can read is
	 * whether anything was cleared, and when. */
	fake_fio1clr = 0;

	/* A tick short of 10 ms: still on. */
	fake_t2tc = 7000 + MSEC(10) - 1;
	blink_expire();
	mu_assert_int_eq(0, (int)fake_fio1clr);

	fake_t2tc = 7000 + MSEC(10);
	blink_expire();
	mu_assert(fake_fio1clr != 0, "the blink was never turned off");

	/* And once off, it stays off until the next packet. */
	fake_fio1clr = 0;
	fake_t2tc += MSEC(100);
	blink_expire();
	mu_assert_int_eq(0, (int)fake_fio1clr);
}

/* A packet arriving during a blink puts the full 10 ms back, as re-arming the
 * match did. */
MU_TEST(a_second_packet_puts_the_blink_back_to_full_length)
{
	start_fresh();
	blink_timer_start();
	fake_t2tc = 0;

	blink(0, 1, 0);
	fake_t2tc = MSEC(9);
	blink(0, 1, 0);

	fake_fio1clr = 0;
	fake_t2tc = MSEC(10);
	blink_expire();
	mu_assert_int_eq(0, (int)fake_fio1clr);

	fake_t2tc = MSEC(19);
	blink_expire();
	mu_assert(fake_fio1clr != 0, "the blink outlasted its own deadline");
}

/* The counter is 32 bits of 100 ns ticks, so it wraps every 429 seconds. A
 * blink armed just before the wrap must not be read as already over, which
 * would leave it a few microseconds long instead of 10 ms. */
MU_TEST(a_blink_survives_the_counters_wrap)
{
	start_fresh();
	blink_timer_start();
	fake_t2tc = 0xffffffffu - MSEC(5);

	blink(0, 1, 0);
	fake_fio1clr = 0;

	/* The deadline has wrapped and the counter has not, so it is now the
	 * smaller of the two although it is 10 ms in the future. Comparing the
	 * two as plain unsigned numbers reads the blink as already over here. */
	blink_expire();
	mu_assert_int_eq(0, (int)fake_fio1clr);

	/* 1 ms in, still before the wrap. */
	fake_t2tc = 0xffffffffu - MSEC(4);
	blink_expire();
	mu_assert_int_eq(0, (int)fake_fio1clr);

	/* Past the wrap, but only 5 ms in. */
	fake_t2tc = MSEC(4);
	blink_expire();
	mu_assert_int_eq(0, (int)fake_fio1clr);

	fake_t2tc = MSEC(5);
	blink_expire();
	mu_assert(fake_fio1clr != 0, "the blink was never turned off");
}

/* The same register, the other way round: disarming one match may not lose
 * another that an interrupt armed. The connection-following path disarms the
 * event match from the main loop, which is where this was reachable. */
MU_TEST(disarming_one_match_cannot_lose_another)
{
	start_fresh();
	fake_t1tc = 1000;

	timer1_set_match(2000);
	mu_assert_fatal(fake_t1mcr & TMCR_MR0I, "the event match was not armed");

	irq_pending_hook = arm_a_wait_for_a_buffer;
	timer1_clear_match();

	mu_assert(irq_pending_hook == NULL,
	          "the interrupt never got in, so the match register was "
	          "changed without holding interrupts off");
	mu_assert(!(fake_t1mcr & TMCR_MR0I),
	          "the event match was not disarmed");
	mu_assert(fake_t1mcr & TMCR_MR3I,
	          "the buffer wait the interrupt armed was lost, so nothing is "
	          "left to notice a buffer coming free");
	mu_assert_int_eq(0, (int)irq_mask_depth);
}

/* Arming and disarming touch one match each and leave the rest alone, which
 * is what makes the four waits independent. */
MU_TEST(each_match_is_armed_on_its_own)
{
	start_fresh();

	timer1_set_match(1);
	timer1_wait_fs_lock();
	timer1_wait_buffer();
	mu_assert_int_eq(TMCR_MR0I | TMCR_MR2I | TMCR_MR3I, (int)fake_t1mcr);

	timer1_cancel_fs_lock();
	mu_assert_int_eq(TMCR_MR0I | TMCR_MR3I, (int)fake_t1mcr);

	timer1_cancel_wait_buffer();
	mu_assert_int_eq(TMCR_MR0I, (int)fake_t1mcr);

	timer1_clear_match();
	mu_assert_int_eq(0, (int)fake_t1mcr);
}

/*
 * Finding a free buffer and marking it taken is one step. Two contexts that
 * each read the flag before either cleared it were both handed the same
 * buffer: the engine then received into a buffer that was already being filled
 * by the other, or handed one to the host twice.
 */
MU_TEST(two_contexts_cannot_be_handed_the_same_buffer)
{
	le_rx_t *mine;
	int i, free_left;

	start_fresh();

	the_buffer_the_handler_took = NULL;
	irq_pending_hook = take_a_buffer_from_the_pool;
	mine = buffer_get();

	mu_assert_fatal(mine != NULL, "the pool refused a buffer");
	mu_assert(irq_pending_hook == NULL,
	          "the interrupt never got in, so the pool's flag is tested and "
	          "cleared without holding interrupts off");
	mu_assert_fatal(the_buffer_the_handler_took != NULL,
	                "the pool refused the interrupt a buffer while three "
	                "were free");
	mu_assert(the_buffer_the_handler_took != mine,
	          "two contexts were handed the same buffer, so two packets "
	          "would be received into one");

	/* Two gone, and the pool's own accounting says two. */
	for (free_left = 0, i = 0; i < LE_BUFFER_POOL_SIZE; i++)
		if (buffer_get() != NULL)
			free_left++;
	mu_assert_int_eq(LE_BUFFER_POOL_SIZE - 2, free_left);
	mu_assert_int_eq(0, (int)irq_mask_depth);
}

/* What the buffer the pool is about to hand out still held when the interrupt
 * got in. */
static uint8_t the_buffer_still_held_when_the_interrupt_ran;
static void look_at_the_buffer_being_handed_out(void)
{
	the_buffer_still_held_when_the_interrupt_ran = le_buffer_pool[0].data[0];
}

/*
 * The pool's own guard covers the flag and nothing else: the 260-byte clear
 * that follows is outside it.
 *
 * Where a caller wants the clear covered too it takes its own guard around the
 * call, which is what the end-of-packet handover does, and the guards nest. So
 * this is not a statement about the handover; it is that the pool adds no mask
 * of its own around the memset, and a caller reading the pool's flag is held
 * off for a few instructions rather than for a memset.
 *
 * The interrupt is stood in where the guard ends, so what it sees of the
 * buffer says which side of the guard the memset is on.
 */
MU_TEST(the_pool_guards_its_flag_and_not_the_clear_that_follows)
{
	le_rx_t *taken;

	start_fresh();

	/* Leave the first buffer looking like a packet that has been sent. */
	memset(le_buffer_pool[0].data, 0xa5, sizeof(le_buffer_pool[0].data));

	the_buffer_still_held_when_the_interrupt_ran = 0;
	irq_pending_hook = look_at_the_buffer_being_handed_out;
	taken = buffer_get();

	mu_assert_fatal(taken == &le_buffer_pool[0],
	                "the pool did not hand out the first buffer");
	mu_assert(irq_pending_hook == NULL,
	          "the interrupt never got in, so the pool's flag is tested and "
	          "cleared without holding interrupts off");
	mu_assert_int_eq(0xa5, the_buffer_still_held_when_the_interrupt_ran);
	mu_assert_int_eq(0, (int)taken->data[0]);
}

/*
 * A finished packet is handed to the host and the buffer for the next one
 * taken in one step, so current_rxbuf never names a buffer that is on its way
 * out. It used to: the packet was queued first and the pointer moved on
 * afterwards, and a timer match taken in between reached the buffer the host
 * was about to be given -- the channel change clears it, the waits receive
 * into it. The host got a record of zeroes, or one holding bytes from a later
 * moment in the air.
 */
MU_TEST(a_queued_packet_is_not_reached_by_the_match_that_follows_it)
{
	le_rx_t *packet;
	unsigned i;

	start_fresh();
	le_phy_active = 1;
	rf_channel = ADV_CHANNEL_MHZ;
	/* Held before the hook is set, so the hook stands in for a match taken
	 * during the handover and not during this. */
	current_rxbuf = buffer_get();
	mu_assert_fatal(current_rxbuf != NULL, "the pool gave out no buffer");

	irq_pending_hook = clear_the_buffer_the_engine_holds;
	packet = a_packet_arrives(20, ADV_CHANNEL_MHZ, 0);

	mu_assert_fatal(packet != NULL, "no packet reached the host");
	mu_assert(irq_pending_hook == NULL,
	          "the interrupt never got in, so the handover does not hold "
	          "interrupts off");
	mu_assert(packet != current_rxbuf,
	          "receive is armed on the buffer that was handed to the host");

	/* The packet the host is given is the one that was received. */
	mu_assert_int_eq(20 + 2 + 3, (int)packet->pos);
	mu_assert_int_eq(20 + 2 + 3, (int)packet->size);
	mu_assert_int_eq(0x00100000 - (int)USEC(48), (int)packet->timestamp);
	for (i = 0; i < packet->pos; i++)
		if (i != 1 && packet->data[i] != 0x5a) {
			mu_assert(0, "the packet handed to the host lost its bytes to "
			             "the match taken after it was queued");
			break;
		}
	mu_assert_int_eq(0, (int)irq_mask_depth);
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

	/* An access address with its top bit set. The highest byte is shifted
	 * left by 24, which runs past the largest int there is unless the byte
	 * is widened first, so this is the value a four-byte field has to
	 * survive. 0x8e89bed6 is the advertising access address. */
	buf.data[4] = 0xd6;
	buf.data[5] = 0xbe;
	buf.data[6] = 0x89;
	buf.data[7] = 0x8e;

	mu_assert_int_eq(0x8e89bed6, extract_field(&buf, 4, 4));

	buf.data[7] = 0xff;
	mu_assert_int_eq(0xff89bed6, extract_field(&buf, 4, 4));
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
		buf.rssi_sum = 46;       /* these cases are about the bytes */

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
 * A packet the ring had no room for leaves a mark on the next one that gets
 * through, so a capture with a gap in it can be told from a quiet channel.
 *
 * It matters most for long packets. One of those takes several of the ring's
 * records and is refused all or nothing, so a nearly full ring drops long
 * packets while short ones still go through -- which is how a capture can
 * lose exactly the packets that were wanted and look complete.
 */
MU_TEST(a_dropped_packet_marks_the_next_one_that_gets_through)
{
	le_rx_t one_record, long_packet, after;
	usb_pkt_rx *record;
	int filled = 0;

	start_fresh();

	/* A packet of exactly 50 bytes, so each one takes a single record... */
	fill_packet(&one_record, 41, DATA_CHANNEL_MHZ);
	mu_assert_int_eq(DMA_SIZE, 4 + one_record.size);
	/* ...and one of 260, the longest there is, which takes six. */
	fill_packet(&long_packet, 255, DATA_CHANNEL_MHZ);
	mu_assert_int_eq(LE_MAX_USB_PACKET_SIZE, 4 + long_packet.size);

	/* Leave the ring with room for one more record and no more, so a short
	 * packet still fits and the long one cannot. */
	while (usb_queue_space() > 1) {
		mu_assert_fatal(usb_enqueue_le(&one_record) == 1,
		                "the ring would not take a 50-byte packet");
		filled++;
	}

	mu_assert_int_eq(0, usb_enqueue_le(&long_packet));

	/* Nothing already in the ring is marked: the drop happened after them,
	 * and a mark on a packet that arrived before the gap would put the gap
	 * in the wrong place. */
	while ((record = dequeue()) != NULL) {
		mu_assert_int_eq(0, record->status & FIFO_OVERFLOW);
		filled--;
	}
	mu_assert_int_eq(0, filled);

	/* The next packet to get through carries the news. */
	fill_packet(&after, 10, DATA_CHANNEL_MHZ);
	mu_assert_int_eq(1, usb_enqueue_le(&after));
	record = dequeue();
	mu_assert_fatal(record != NULL, "the packet after the gap was not sent");
	mu_assert_int_eq(FIFO_OVERFLOW, record->status & FIFO_OVERFLOW);

	/* ...and only that one: the gap is reported once, not on every packet
	 * for the rest of the capture. */
	mu_assert_int_eq(1, usb_enqueue_le(&after));
	record = dequeue();
	mu_assert_fatal(record != NULL, "the second packet after the gap was "
	                                "not sent");
	mu_assert_int_eq(0, record->status & FIFO_OVERFLOW);

	/* Every fragment of a long packet carries the same readings, so the
	 * host sees the mark on the record that starts the chain. Fill the ring
	 * again, drop a long packet, then empty it so the next one fits. */
	while (usb_queue_space() > 1)
		mu_assert_fatal(usb_enqueue_le(&one_record) == 1,
		                "the ring would not fill up again");
	mu_assert_int_eq(0, usb_enqueue_le(&long_packet));
	while (dequeue() != NULL)
		;
	mu_assert_int_eq(1, usb_enqueue_le(&long_packet));
	record = dequeue();
	mu_assert_fatal(record != NULL, "the long packet after the gap was not "
	                                "sent");
	mu_assert_int_eq(LE_PACKET_START, record->pkt_type);
	mu_assert_int_eq(FIFO_OVERFLOW, record->status & FIFO_OVERFLOW);
}

/*
 * A capture starts with no gap to report, whatever the run before it left
 * behind: the flag belongs to the capture, not to the board.
 */
MU_TEST(a_drop_does_not_carry_into_the_next_capture)
{
	le_rx_t packet;
	usb_pkt_rx *record;

	start_fresh();
	fill_packet(&packet, 255, DATA_CHANNEL_MHZ);
	while (usb_queue_space() >= 6)
		mu_assert_fatal(usb_enqueue_le(&packet) == 1,
		                "the ring would not take a long packet");
	mu_assert_int_eq(0, usb_enqueue_le(&packet));

	/* What le_phy_main() does on the way in. */
	usb_queue_init();
	le_packet_dropped = 0;

	fill_packet(&packet, 10, DATA_CHANNEL_MHZ);
	mu_assert_int_eq(1, usb_enqueue_le(&packet));
	record = dequeue();
	mu_assert_fatal(record != NULL, "the first packet of the new capture "
	                                "was not sent");
	mu_assert_int_eq(0, record->status & FIFO_OVERFLOW);
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
 * The average RSSI in the record is the mean of the samples taken while the
 * packet arrived. The CC2400 reports RSSI as a negative number, so every real
 * capture has a negative sum and the division has to be a signed one. The
 * host puts the result in the per-packet header of a PCAP written with -c,
 * between the minimum and the maximum of the same packet.
 */
MU_TEST(the_average_rssi_is_the_mean_of_the_samples)
{
	le_rx_t buf;
	usb_pkt_rx *record;

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
	mu_assert_fatal(usb_enqueue_le(&buf) == 1, "the queue refused a packet");
	record = dequeue();
	mu_assert_fatal(record != NULL, "no record came back");
	mu_assert_int_eq(-20, record->rssi_avg);

	/* A real packet: every sample the same, so the mean is that value and
	 * sits on the minimum and the maximum. */
	start_fresh();
	fill_packet(&buf, 5, DATA_CHANNEL_MHZ);
	buf.rssi_sum = -70 * (int) buf.size;
	buf.rssi_min = -70;
	buf.rssi_max = -70;
	mu_assert_fatal(usb_enqueue_le(&buf) == 1, "the queue refused a packet");
	record = dequeue();
	mu_assert_fatal(record != NULL, "no record came back");
	mu_assert_int_eq(-70, record->rssi_avg);
	mu_assert(record->rssi_avg >= record->rssi_min &&
	          record->rssi_avg <= record->rssi_max,
	          "the average RSSI is outside the minimum and the maximum");
}

/*
 * A packet is stamped with the time its preamble started, which is what the
 * host writes into a capture file and what a reader measures air time from.
 * The receive interrupt that reports the first header byte is taken later
 * than that, so the engine back-dates the reading by what has gone past: 8
 * bits of preamble, the 32-bit access address and 8 bits of header, 48 us at
 * one symbol per microsecond.
 *
 * The stamp used to be back-dated by 40 us, leaving every LE packet in a
 * capture 8 us late.
 */
MU_TEST(a_packets_timestamp_is_back_dated_to_the_start_of_the_preamble)
{
	le_rx_t *buf;
	/* The time the interrupt was taken, in units of 100 ns. */
	const uint32_t interrupt_at = 0x00200000;

	start_fresh();
	le_phy_active = 1;
	rf_channel = DATA_CHANNEL_MHZ;
	current_rxbuf = buffer_get();
	mu_assert_fatal(current_rxbuf != NULL, "the pool gave out no buffer");
	buf = current_rxbuf;

	/* The DMA controller reports one byte of the packet, which is the
	 * first: the buffer the pool just handed out holds nothing yet. */
	fake_dmac_int_stat = 1 << 0;
	fake_dmac_int_tc_stat = 1 << 0;
	le_dma_dest[0] = 0x9c;
	le_dma_interrupt(interrupt_at);

	mu_assert_int_eq(1, buf->pos);
	mu_assert_int_eq(interrupt_at - USEC(48), buf->timestamp);
}

/*
 * A packet carries the access address it was received with, not the one the
 * engine is about to listen for.
 *
 * A connection request is read in the mode's loop, which sets the connection's
 * access address there and then; the radio goes on listening for the
 * advertising one until the next hop writes the new sync word. A packet
 * received in between used to be recorded under the connection's address,
 * which put advertising packets from other devices into the capture as the
 * first packets of that connection, before its transmit window had opened.
 */
MU_TEST(a_packet_carries_the_access_address_it_was_received_with)
{
	le_rx_t *buf;

	start_fresh();
	le_phy_active = 1;
	rf_channel = ADV_CHANNEL_MHZ;
	current_rxbuf = buffer_get();
	mu_assert_fatal(current_rxbuf != NULL, "the pool gave out no buffer");
	buf = current_rxbuf;

	/* A connection request has just been read: the engine holds the
	 * connection's address, and the radio is still on the advertising one
	 * because no hop has happened yet. */
	conn.access_address = 0xdeadbeef;

	fake_dmac_int_stat = 1 << 0;
	fake_dmac_int_tc_stat = 1 << 0;
	le_dma_dest[0] = 0x9c;
	le_dma_interrupt(0x00200000);

	mu_assert_int_eq(ADV_AA, buf->access_address);
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
	/* The 24-bit reversal of 0x555555, which is the constant the advertising
	 * path spells out as 0xAAAAAA. */
	mu_assert_int_eq(0xAAAAAA, conn.crc_init_reversed);
	mu_assert_int_eq(7, conn.hop_increment);
	/* 1.25 ms in units of 100 ns is 12500. */
	mu_assert_int_eq(24 * 12500, conn.conn_interval);
	mu_assert_int_eq(0, conn.win_offset);
	/* The window is kept scaled, like the offset and the interval. */
	mu_assert_int_eq(1 * 12500, conn.win_size);
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

/*
 * `ubertooth-btle -f -k1` checks each packet's CRC and drops the ones that
 * fail. A data-channel packet is checked against the starting value the
 * CONNECT_IND carried, and that value is a 24-bit field: reversing it over a
 * whole 32-bit word leaves it a byte too high, which fails every packet of
 * the connection and empties the capture.
 */
MU_TEST(a_connections_crc_init_is_reversed_as_a_24_bit_field)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;
	/* A short data PDU: the two-byte header and a four-byte payload. */
	uint8_t pdu[6] = { 0x02, 0x04, 0xde, 0xad, 0xbe, 0xef };
	uint32_t crc, recovered;

	start_fresh();
	le.do_follow = 1;
	r.crc_init = 0x123456;
	build_connect_req(&buf, &r);

	le_connect_handler(&buf);

	mu_assert_int_eq(0x123456, conn.crc_init);
	mu_assert_int_eq(0x6a2c48, conn.crc_init_reversed);

	/*
	 * And the value is the one the CRC needs, shown without repeating the
	 * reversal: the CRC of a packet, computed from it, runs backwards to the
	 * CRCInit the connection request carried. btle_reverse_crc() is the
	 * separate implementation the promiscuous path recovers a CRCInit with,
	 * so a wrong starting value here cannot cancel out.
	 */
	crc = btle_crcgen_lut(conn.crc_init_reversed, pdu, sizeof(pdu));
	recovered = btle_reverse_crc(crc, pdu, sizeof(pdu));
	mu_assert_int_eq(0x123456, recovered);
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
	} cases[9];

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
	/* 43000 steps of 10 ms is 430 s, which is 4.3e9 units of 100 ns: past
	 * what a 32-bit count holds, so scaling before the range check wraps it
	 * to half a second and it passes. */
	r = valid_connect_req(); r.supervision_timeout = 43000;
	cases[8].what = "a supervision timeout that overflows the scaled count";
	cases[8].r = r;

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
	struct conn_update u = valid_conn_update();
	le_rx_t buf;

	start_fresh();
	u.instant = 500;
	build_conn_update(&buf, &u);

	connection_update_handler(&buf);

	mu_assert_int_eq(2 * 12500, conn.win_size);
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
 * The parameters of an update are checked against the same ranges a
 * connection request's are, and an update outside them is ignored rather than
 * taken on trust: a transmit window of 255 steps would otherwise put 319 ms
 * into the timeout that waits out the first event under the new parameters,
 * and the supervision timeout it came with would be all that ended the chase.
 *
 * The connection it is already following is kept, on the parameters it
 * already has: nothing conformant sends such a value, so the packet has bit
 * errors, and any data packet whose first bytes look like a connection update
 * reaches this handler. Ending the follow would let one bit flip stop a
 * healthy capture, and the parameters in place are in range and end the
 * connection at its real supervision timeout. channel_map_update_handler()
 * treats an empty channel map the same way.
 *
 * One case per field, each one value past the boundary, so a check removed or
 * a comparison turned round fails here.
 */
MU_TEST(a_connection_update_outside_the_specification_is_ignored)
{
	le_rx_t buf;
	size_t i;
	struct bad_case {
		const char *what;
		struct conn_update u;
	} cases[8];
	struct conn_update u;

	memset(cases, 0, sizeof(cases));

	u = valid_conn_update(); u.conn_interval = 5;
	cases[0].what = "a connection interval below the 7.5 ms minimum";
	cases[0].u = u;
	u = valid_conn_update(); u.conn_interval = 3201;
	cases[1].what = "a connection interval above the 4 s maximum";
	cases[1].u = u;
	u = valid_conn_update(); u.win_offset = 41;
	cases[2].what = "a transmit window offset past the connection interval";
	cases[2].u = u;
	u = valid_conn_update(); u.win_size = 0;
	cases[3].what = "a transmit window shorter than 1.25 ms";
	cases[3].u = u;
	u = valid_conn_update(); u.win_size = 255;
	cases[4].what = "a transmit window of 319 ms";
	cases[4].u = u;
	u = valid_conn_update(); u.supervision_timeout = 9;
	cases[5].what = "a supervision timeout below 100 ms";
	cases[5].u = u;
	u = valid_conn_update(); u.supervision_timeout = 3201;
	cases[6].what = "a supervision timeout above 32 s";
	cases[6].u = u;
	/* 43000 steps of 10 ms is 430 s, which is 4.3e9 units of 100 ns: past
	 * what a 32-bit count holds, so an unchecked scaling wraps it to half a
	 * second and it passes a range check made after the multiplication. */
	u = valid_conn_update(); u.supervision_timeout = 43000;
	cases[7].what = "a supervision timeout that overflows the scaled count";
	cases[7].u = u;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char msg[256];

		/* A connection being followed, so there is something to drop. */
		start_fresh();
		le.do_follow = 1;
		{
			struct connect_req r = valid_connect_req();

			build_connect_req(&buf, &r);
			le_connect_handler(&buf);
			mu_assert_fatal(conn.access_address == 0xdeadbeef,
			                "the connection was not followed, so there "
			                "is no update to refuse");
		}

		build_conn_update(&buf, &cases[i].u);
		connection_update_handler(&buf);

		snprintf(msg, sizeof(msg), "%s was still scheduled",
		         cases[i].what);
		mu_assert(conn.conn_update_pending == 0, msg);
		/* The live parameters are untouched, so the connection carries on
		 * with the ones the connection request set. */
		snprintf(msg, sizeof(msg),
		         "%s was written into the live connection", cases[i].what);
		mu_assert(conn.conn_interval == 24 * 12500 &&
		          conn.win_size == 1 * 12500 &&
		          conn.supervision_timeout == 10 * 100000, msg);
		snprintf(msg, sizeof(msg),
		         "%s ended the follow, so one bit flip in a data packet "
		         "would stop a healthy capture", cases[i].what);
		mu_assert(!connection_was_reset() && cancel_follow == 0, msg);
	}
}

/*
 * The transmit window size of an update survives being scaled. The wire
 * carries it in units of 1.25 ms, so the smallest window the specification
 * allows is already 12500 units of 100 ns and the largest is 100000; the field
 * has to be wide enough for all of them. The timer match that waits out the
 * update's transmit window is built from this value, so a window kept short
 * would leave the capture missing the first packets of the new parameters.
 */
MU_TEST(an_updates_window_size_survives_being_scaled)
{
	le_rx_t buf;
	unsigned on_the_wire;

	/* Every window size the specification allows: 1.25 ms to 10 ms. */
	for (on_the_wire = 1; on_the_wire <= 8; on_the_wire++) {
		struct conn_update u = valid_conn_update();
		char msg[128];

		start_fresh();
		u.win_size = (uint8_t) on_the_wire;
		build_conn_update(&buf, &u);

		connection_update_handler(&buf);

		snprintf(msg, sizeof(msg),
		         "a window of %u units of 1.25 ms came out as %u units of "
		         "100 ns", on_the_wire, (unsigned) conn.win_size);
		mu_assert(conn.win_size == on_the_wire * 12500, msg);
	}
}

/*
 * At the instant an update takes effect, the engine waits out the transmit
 * window offset and then gives the first connection event a timeout that
 * spans the whole transmit window plus the longest packet. The master's first
 * packet under the new parameters may arrive anywhere inside that window, so a
 * timeout that expires earlier can close the event before the packet arrives,
 * and the capture loses it.
 */
MU_TEST(the_first_event_after_an_update_waits_out_the_whole_window)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;
	uint32_t expected_match;

	/* A connection being followed, anchored, with one event behind it. */
	start_fresh();
	le.do_follow = 1;
	build_connect_req(&buf, &r);
	le_connect_handler(&buf);
	mu_assert_fatal(conn.remapping.total_channels == 37,
	                "the connection was not followed, so there is no update "
	                "to apply");
	conn.last_anchor = 1000000;
	conn.anchor_error = 0;

	/* An update to a 50 ms interval, a 3.75 ms window offset and a 2.5 ms
	 * transmit window, at event 7. */
	{
		struct conn_update u = valid_conn_update();

		build_conn_update(&buf, &u);
		connection_update_handler(&buf);
	}

	conn.conn_event_counter = 7;
	le_phy_active = 1;

	/* The instant arrives: the window offset is waited out first, on the old
	 * interval, with the radio left alone. */
	fake_t1ir = TIR_MR0_Interrupt;
	TIMER1_IRQHandler();
	expected_match = 1000000 + 24 * 12500 + 3 * 12500 + USEC(1250) -
	                 RX_WARMUP_TIME;
	mu_assert_int_eq(expected_match, fake_t1mr0);

	/* Then the event itself, on the new interval, with the whole window and
	 * the longest packet to run in. The counter is where that match put it:
	 * the event opens a warmup before its window, and its deadline is
	 * counted from there. */
	fake_t1tc = expected_match;
	fake_t1ir = TIR_MR0_Interrupt;
	TIMER1_IRQHandler();
	expected_match = fake_t1tc + 2 * 12500 + PERIPHERAL_REPLY_LATEST + RX_WARMUP_TIME;
	mu_assert_int_eq(expected_match, fake_t1mr0);
	mu_assert_int_eq(40 * 12500, conn.conn_interval);
	mu_assert_int_eq(0, conn.conn_update_pending);

	/* The grid the events after it run on: one interval before that window,
	 * which is what finish_conn_event() advances from. Without moving it
	 * here, the estimate would carry on from the last event under the old
	 * interval, which the new window is offset from. */
	mu_assert_int_eq(1000000 + 24 * 12500 + 3 * 12500 + USEC(1250) -
	                 40 * 12500, conn.last_anchor);

	/* Once an event measures the anchor, the window is no longer added: an
	 * anchored event gets the longest packet alone, and a window-long
	 * deadline there would still be open when the next event was due. */
	conn.last_anchor = 2000000;
	conn.anchor_error = 0;
	reset_conn_event();
	fake_t1ir = TIR_MR0_Interrupt;
	TIMER1_IRQHandler();
	expected_match = fake_t1tc + PERIPHERAL_REPLY_LATEST + RX_WARMUP_TIME;
	mu_assert_int_eq(expected_match, fake_t1mr0);
}

/*
 * ...and so does the first event of a new connection, for the same reason: a
 * CONNECT_IND names a transmit window, and the master's first packet may
 * arrive anywhere inside it. A timeout of one maximum-length packet closes
 * the event before that packet arrives, so the capture misses the first
 * packets of every connection whose window is longer than the 1.25 ms
 * minimum.
 *
 * Only the first event. Every later one is anchored to within a few
 * microseconds of where its packet is due, and a window-long timeout there
 * would hold the event open past the next hop.
 */
MU_TEST(the_first_event_of_a_connection_waits_out_the_whole_window)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;
	uint32_t window_opens, expected_match;

	/* A connection with a 5 ms transmit window, four times the minimum,
	 * 3.75 ms into a 30 ms interval. */
	start_fresh();
	le.do_follow = 1;
	r.win_size = 4;
	r.win_offset = 3;
	build_connect_req(&buf, &r);

	le_connect_handler(&buf);
	mu_assert_int_eq(4 * 12500, conn.win_size);

	/* The radio is armed in time for the window to open. */
	window_opens = buf.timestamp + PACKET_DURATION(&buf) + 3 * 12500 +
	               USEC(1250);
	mu_assert_int_eq(window_opens - RX_WARMUP_TIME, fake_t1mr0);

	/* That match arrives: the event opens, with the whole window and the
	 * longest packet to run in. */
	fake_t1tc = window_opens - RX_WARMUP_TIME;
	fake_t1ir = TIR_MR0_Interrupt;
	le_phy_active = 1;
	TIMER1_IRQHandler();

	expected_match = fake_t1tc + 4 * 12500 + PERIPHERAL_REPLY_LATEST + RX_WARMUP_TIME;
	mu_assert_int_eq(expected_match, fake_t1mr0);
	mu_assert_int_eq(1, conn_event.opened);

	/* The next event is anchored, so it gets the packet timeout alone: a
	 * window-long one would still be open when the hop after it was due. */
	conn.last_anchor = fake_t1tc;
	conn.anchor_error = 0;
	reset_conn_event();
	fake_t1ir = TIR_MR0_Interrupt;
	TIMER1_IRQHandler();

	expected_match = fake_t1tc + PERIPHERAL_REPLY_LATEST + RX_WARMUP_TIME;
	mu_assert_int_eq(expected_match, fake_t1mr0);
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

/* What the timer interrupt does on every connection event: ask whether a
 * change is waiting, and read it if its instant has come. Both hooks below
 * stand in for that, run where the hardware would run the handler -- as the
 * guard the loop was inside ends. */
static int the_update_the_timer_saw_was_whole;
static void the_timer_reads_the_pending_update(void)
{
	the_update_the_timer_saw_was_whole =
		conn.conn_update_pending == 1 &&
		conn.conn_update_instant == 7 &&
		conn.conn_update_pending_interval == 40 * USEC(1250) &&
		conn.conn_update_pending_supervision_timeout == 20 * MSEC(10) &&
		conn.win_size == 2 * USEC(1250) &&
		conn.win_offset == 3 * USEC(1250);
}

static int the_map_was_pending_while_it_was_written = -1;
static int the_map_was_pending_once_it_was_whole = -1;
static unsigned the_channels_the_timer_saw;
static uint16_t the_instant_the_timer_saw;

static void the_timer_reads_the_map_once_it_is_whole(void)
{
	the_map_was_pending_once_it_was_whole = conn.channel_map_update_pending;
	the_instant_the_timer_saw = conn.channel_map_update_instant;
	the_channels_the_timer_saw = conn.pending_remapping.total_channels;
}

static void the_timer_reads_the_map_while_it_is_written(void)
{
	the_map_was_pending_while_it_was_written =
		conn.channel_map_update_pending;
	/* The map is parsed next, and then the guard that publishes it ends:
	 * look again there. */
	irq_pending_hook = the_timer_reads_the_map_once_it_is_whole;
}

/*
 * An update that replaces one still waiting for its instant is never seen as
 * a mixture of the two.
 *
 * This handler runs in the mode's loop, and the timer interrupt reads these
 * five fields on every connection event. Written one at a time, a new update
 * landing on top of a waiting one could be applied with the old instant and
 * the new interval, or the new window and the old timeout, and every deadline
 * the follow uses is built from them.
 *
 * Interrupts held off rather than the pending flag written last, because none
 * of these fields is volatile: the compiler may order ordinary stores as it
 * likes, and a flag cleared and set again with only ordinary stores in
 * between is a store it may drop. The guard's memory clobber is what settles
 * both.
 */
MU_TEST(an_update_is_never_seen_as_a_mixture_of_two)
{
	le_rx_t buf;
	struct conn_update u = valid_conn_update();

	start_fresh();
	/* One is already waiting, for an instant this connection is nowhere
	 * near. */
	conn.conn_update_pending = 1;
	conn.conn_update_instant = 900;
	conn.conn_update_pending_interval = 24 * USEC(1250);
	conn.win_size = USEC(1250);

	build_conn_update(&buf, &u);
	the_update_the_timer_saw_was_whole = -1;
	irq_pending_hook = the_timer_reads_the_pending_update;
	connection_update_handler(&buf);

	mu_assert(irq_pending_hook == NULL,
	          "the interrupt never got in, so the update is written without "
	          "holding interrupts off");
	mu_assert_int_eq(1, the_update_the_timer_saw_was_whole);
	mu_assert_int_eq(0, (int)irq_mask_depth);
}

/*
 * ...and a channel map update is not pending while its map is being written.
 *
 * le_parse_channel_map() clears the map before filling it, so an update
 * applied while it ran gave the follow a map with some of the update's
 * channels in it and the rest missing, and the receiver tuned where the peer
 * was not for the rest of the connection.
 */
MU_TEST(a_channel_map_is_not_pending_while_it_is_being_written)
{
	le_rx_t buf;

	start_fresh();
	/* One is already waiting, with a map of its own. */
	conn.channel_map_update_pending = 1;
	conn.channel_map_update_instant = 900;
	conn.pending_remapping.total_channels = 37;

	fill_packet(&buf, 8, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	/* Two channels left in use: indices 0 and 9. */
	buf.data[3] = 0x01;
	buf.data[4] = 0x02;
	put_le(&buf.data[8], 1234, 2);

	the_map_was_pending_while_it_was_written = -1;
	the_map_was_pending_once_it_was_whole = -1;
	irq_pending_hook = the_timer_reads_the_map_while_it_is_written;
	channel_map_update_handler(&buf);

	mu_assert(irq_pending_hook == NULL,
	          "an interrupt never got in, so the flag and the instant are "
	          "written without holding interrupts off");
	mu_assert_int_eq(0, the_map_was_pending_while_it_was_written);
	mu_assert_int_eq(1, the_map_was_pending_once_it_was_whole);
	mu_assert_int_eq(1234, the_instant_the_timer_saw);
	mu_assert_int_eq(2, (int)the_channels_the_timer_saw);
	mu_assert_int_eq(0, (int)irq_mask_depth);
}

/*
 * A channel map with no channels in it cannot be followed: the remapping
 * divides by the number of channels in use, and the processor's
 * divide-by-zero trap is off, so the division returns 0 instead of faulting
 * and every connection event from the instant on tunes data channel 0. The
 * connection request handler already refuses such a map; this one does too.
 */
MU_TEST(a_channel_map_update_with_no_channels_is_refused)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;

	start_fresh();
	le.do_follow = 1;
	build_connect_req(&buf, &r);
	le_connect_handler(&buf);
	mu_assert_fatal(conn.remapping.total_channels == 37,
	                "the connection was not followed, so there is no update "
	                "to refuse");

	/* A good update first: channels 0 and 9, at event 1234. */
	fill_packet(&buf, 8, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	buf.data[3] = 0x01;
	buf.data[4] = 0x02;
	put_le(&buf.data[8], 1234, 2);
	channel_map_update_handler(&buf);
	mu_assert_int_eq(2, conn.pending_remapping.total_channels);

	/* Then an empty map, at a later instant. */
	fill_packet(&buf, 8, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	put_le(&buf.data[8], 4321, 2);
	channel_map_update_handler(&buf);

	/* It is not scheduled, and it has not overwritten the update that was
	 * already waiting for its instant. */
	mu_assert_int_eq(1, conn.channel_map_update_pending);
	mu_assert_int_eq(1234, conn.channel_map_update_instant);
	mu_assert_int_eq(2, conn.pending_remapping.total_channels);

	/* The map being followed now is untouched either way. */
	mu_assert_int_eq(37, conn.remapping.total_channels);

	/*
	 * The map's last byte carries channels 32 to 36; its top three bits are
	 * reserved. A map with nothing but one of those set is empty too -- and
	 * this is what keeps the quick emptiness test and the parse, which walk
	 * the same 37 bits separately, agreeing.
	 */
	fill_packet(&buf, 8, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	buf.data[7] = 0x20;                 /* the fifth map byte */
	put_le(&buf.data[8], 4321, 2);
	channel_map_update_handler(&buf);
	mu_assert_int_eq(1234, conn.channel_map_update_instant);
	mu_assert_int_eq(2, conn.pending_remapping.total_channels);

	{
		le_channel_remapping_t parsed;

		le_parse_channel_map(&buf.data[3], &parsed);
		mu_assert_int_eq(0, parsed.total_channels);
	}

	/* A map holding only channel 36, the last real one, is not empty, so this
	 * update is taken. */
	fill_packet(&buf, 8, DATA_CHANNEL_MHZ);
	memset(buf.data, 0, sizeof(buf.data));
	buf.data[7] = 0x10;
	put_le(&buf.data[8], 5555, 2);
	channel_map_update_handler(&buf);
	mu_assert_int_eq(5555, conn.channel_map_update_instant);
	mu_assert_int_eq(1, conn.pending_remapping.total_channels);
	mu_assert_int_eq(36, conn.pending_remapping.remapping_index[0]);
}

/*
 * The backstop under both handlers: were a count of zero ever to reach the
 * remapping, each index maps to itself rather than every index to channel 0.
 */
MU_TEST(an_empty_map_does_not_divide_by_zero_in_the_remapping)
{
	le_channel_remapping_t remapping;
	uint8_t empty_map[5] = { 0, 0, 0, 0, 0 };
	unsigned i;

	le_parse_channel_map(empty_map, &remapping);
	mu_assert_int_eq(0, remapping.total_channels);

	for (i = 0; i < 37; i++)
		mu_assert_int_eq(i, le_map_channel((uint8_t)i, &remapping));
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
	{
		struct conn_update u = valid_conn_update();

		u.instant = 77;
		build_conn_update(&buf, &u);
		packet_handler(&buf);
		mu_assert_int_eq(1, conn.conn_update_pending);
		mu_assert_int_eq(77, conn.conn_update_instant);
	}

	/* ...and one of the wrong length is not. */
	start_fresh();
	{
		struct conn_update u = valid_conn_update();

		build_conn_update(&buf, &u);
		buf.data[1] = 11;      /* one byte short */
		packet_handler(&buf);
		mu_assert_int_eq(0, conn.conn_update_pending);
	}

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
	conn.anchor_error = 0;
	conn.last_packet_ts = 1000;
	fake_t1tc = 1000;
	conn.conn_event_counter = 5;
}

/*
 * The first packet of an open connection event is counted into it, anchors it,
 * and starts the inter-frame timeout that waits for the event's second packet.
 */
MU_TEST(the_first_packet_of_an_event_anchors_it_and_waits_for_the_second)
{
	le_rx_t *packet;

	anchored_connection(1000000);
	conn_event.opened = 1;

	packet = a_packet_arrives(10, DATA_CHANNEL_MHZ, 0);

	mu_assert_fatal(packet != NULL, "the packet never reached the host");
	mu_assert_int_eq(1, conn_event.num_packets);
	mu_assert_int_eq(packet->timestamp, conn_event.anchor);
	mu_assert(fake_t1mcr & TMCR_MR0I,
	          "nothing waits for the event's second packet");
	mu_assert_int_eq(fake_t1tc + IFS_TIMEOUT, fake_t1mr0);
	buffer_release(packet);
}

/*
 * A packet that arrived after its connection event closed leaves the next
 * event's own match alone.
 *
 * A packet may begin as late as its event's deadline -- the peripheral's
 * answer is allowed a whole PERIPHERAL_REPLY_LATEST after the anchor -- so the
 * deadline can expire in the microseconds between the packet's first symbol
 * and the interrupt that reports its first byte. The timer interrupt, which
 * preempts that one, has then closed the event and armed the next one in the
 * same match register.
 *
 * The packet is still whole and still goes to the host. What must not happen
 * is its bookkeeping landing in the event that follows: counted there, it put
 * its own time into an anchor that event had not reached and armed a 300
 * microsecond inter-frame timeout over the event's own match. The event after
 * then opened behind a packet instead of on the connection's grid, and the
 * event counter every instant is matched against was one ahead of the peer's.
 */
/* What the timer interrupt does when a connection event's deadline expires:
 * close the event, which counts it and arms the next one. The match it armed
 * is kept, so a test can say the packet still arriving left it alone. */
static uint32_t the_match_the_timer_armed;
static void the_timer_closes_the_connection_event(void)
{
	finish_conn_event();
	the_match_the_timer_armed = fake_t1mr0;
}

/*
 * A deadline that expires as a packet's first byte arrives closes the event
 * and leaves the next one armed, and the rest of that packet does not disturb
 * it.
 *
 * Disarming the deadline is the first thing the byte handler does with a
 * data-channel packet, and it asks whether the event is still open before it
 * does. But a match the timer has already latched is taken the instant the
 * guard around that question ends, so the event can close immediately
 * afterwards and the packet carries on inside an event that has been counted.
 * The guard at the end of the packet is what catches that: it finds the event
 * closed and leaves the next event's match and anchor alone.
 */
MU_TEST(a_deadline_that_expires_as_a_packet_starts_leaves_the_next_event_armed)
{
	le_rx_t *packet;

	anchored_connection(1000000);
	conn_event.opened = 1;
	/* The event is running to its own deadline, which the timer is about to
	 * take. */
	timer1_set_match(fake_t1tc + MSEC(1));

	/* The buffer is taken before the hook is set, so the first guarded
	 * section the packet runs into is the one that notes its event. */
	current_rxbuf = buffer_get();
	mu_assert_fatal(current_rxbuf != NULL, "the pool gave out no buffer");

	the_match_the_timer_armed = 0;
	irq_pending_hook = the_timer_closes_the_connection_event;
	packet = a_packet_arrives(10, DATA_CHANNEL_MHZ, 0);

	mu_assert(irq_pending_hook == NULL,
	          "the interrupt never got in, so the event's deadline is "
	          "dropped without holding interrupts off");
	mu_assert_fatal(packet != NULL, "the packet never reached the host");
	mu_assert_int_eq(0, conn_event.num_packets);
	mu_assert(fake_t1mcr & TMCR_MR0I,
	          "the packet that was arriving as the event closed disarmed the "
	          "next event, so nothing opens it");
	mu_assert_int_eq(the_match_the_timer_armed, fake_t1mr0);
	buffer_release(packet);
}

MU_TEST(a_packet_from_a_closed_event_leaves_the_next_event_armed)
{
	const uint32_t next_event = 2000000;
	le_rx_t *packet;

	anchored_connection(1000000);
	/* The event this packet belongs to has been closed and the next one
	 * armed, which is what finish_conn_event() leaves behind. The event
	 * counter is left where a packet that noted no event would leave its
	 * own, so only "no event was open" tells the two apart. */
	reset_conn_event();
	conn.conn_event_counter = 0;
	timer1_set_match(next_event);

	packet = a_packet_arrives(10, DATA_CHANNEL_MHZ, 0);

	mu_assert_fatal(packet != NULL, "the packet never reached the host");
	mu_assert_int_eq(0, conn_event.num_packets);
	mu_assert(fake_t1mcr & TMCR_MR0I,
	          "a packet from the event before it disarmed the next "
	          "connection event, so nothing opens it");
	mu_assert_int_eq(next_event, fake_t1mr0);
	buffer_release(packet);
}

MU_TEST(two_packets_in_an_event_anchor_it_where_they_arrived)
{
	anchored_connection(100000);

	conn_event.anchor = 123456;
	conn_event.num_packets = 2;
	conn_event.opened = 1;

	finish_conn_event();

	mu_assert_int_eq(123456, conn.last_anchor);
	mu_assert_int_eq(0, conn.anchor_error);
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
	mu_assert_int_eq(0, conn.anchor_error);
	/* The supervision timeout runs from the last packet, which this event
	 * did not have, so that time must not move. */
	mu_assert_int_eq(1000, conn.last_packet_ts);
	mu_assert_int_eq(6, conn.conn_event_counter);
	mu_assert_int_eq(0, radio_touched);
}

/*
 * One packet in an event whose anchor was never measured could be either the
 * central's packet or the peripheral's answer to it, and nothing in a data
 * PDU says which. So the estimate goes to the earliest the anchor could be,
 * one PERIPHERAL_REPLY_LATEST before the packet.
 *
 * Early is the safe direction: an event armed early is still open when its
 * packet arrives, one armed late has missed it. It used to take the packet's
 * own time, which is one maximum-length packet and an inter-frame space late
 * whenever the packet caught was the peripheral's.
 */
MU_TEST(an_unmeasured_event_puts_its_one_packet_at_the_earliest_anchor)
{
	anchored_connection(100000);
	/* No packet has measured the anchor: the estimate is the transmit
	 * window the connection named, so it can be a window early. */
	conn.win_size = 5 * 12500;
	conn.anchor_error = conn.win_size;
	conn.last_anchor = 0;
	conn_event.anchor = 55555;
	conn_event.num_packets = 1;
	fake_t1tc = 2000;

	finish_conn_event();

	mu_assert_int_eq(55555 - PERIPHERAL_REPLY_LATEST, conn.last_anchor);
	/* Still unmeasured, and the estimate is now known to be within one
	 * reply's distance: only an event that caught both packets, or one whose
	 * packet landed where an established anchor said it would, measures it. */
	mu_assert_int_eq(PERIPHERAL_REPLY_LATEST, conn.anchor_error);
	mu_assert_int_eq(2000, conn.last_packet_ts);
	/* The next event is one interval after that anchor. */
	mu_assert_int_eq(55555 - PERIPHERAL_REPLY_LATEST + 24 * 12500 -
	                 RX_WARMUP_TIME, fake_t1mr0);
	mu_assert_int_eq(0, radio_touched);
}

/*
 * An empty connection event has to leave the next one armed ahead of the
 * counter.
 *
 * The estimate used to come from conn_event.anchor, which no packet had
 * written when the event was empty: on the first event of a connection it
 * held nothing or a time from an earlier connection, so the next event was
 * armed at a match the counter had gone past. Such a match is not reached
 * until the counter wraps, 429 seconds later, and meanwhile the receiver sat
 * on one data channel: the capture picked the connection up again only when
 * its hopping came back round to that channel, 4 to 26 events later.
 */
MU_TEST(an_empty_first_event_arms_the_next_one_ahead_of_the_counter)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;
	uint32_t window_opens;

	/* A connection with a 5 ms transmit window, 3.75 ms into a 30 ms
	 * interval, followed from its request. */
	start_fresh();
	le.do_follow = 1;
	r.win_size = 4;
	r.win_offset = 3;
	build_connect_req(&buf, &r);
	le_connect_handler(&buf);
	mu_assert_fatal(conn.remapping.total_channels == 37,
	                "the connection was not followed");

	window_opens = buf.timestamp + PACKET_DURATION(&buf) + 3 * 12500 +
	               USEC(1250);
	mu_assert_int_eq(window_opens - RX_WARMUP_TIME, fake_t1mr0);

	/* Nothing in the whole window: the time is the event's deadline, and
	 * conn_event.anchor is what it was before the connection -- which is
	 * what the engine must not use. */
	fake_t1tc = window_opens + 4 * 12500 + USEC(2120);
	conn_event.anchor = 0;
	conn_event.num_packets = 0;
	conn_event.opened = 1;

	finish_conn_event();

	/* The estimate is the window the connection named, and the next event
	 * is one interval after it -- ahead of the counter, so the match is
	 * reached. */
	mu_assert_int_eq(window_opens, conn.last_anchor);
	mu_assert_int_eq(window_opens + 24 * 12500 - RX_WARMUP_TIME, fake_t1mr0);
	mu_assert(fake_t1mr0 > fake_t1tc, "the next event was armed in the past");
	mu_assert_int_eq(1, conn.conn_event_counter);
	mu_assert(!connection_was_reset(), "the connection was dropped");
}

/*
 * An event's deadline never runs into the next event's own arming, however
 * long a window the connection named.
 *
 * At the shortest interval the specification allows, 7.5 ms, the longest legal
 * window plus a maximum-length packet runs about 8.7 ms. An event that closed
 * that late would have had two intervals go by while finish_conn_event()
 * counted one connection event, and every connection update and channel map
 * instant is matched against that count: each would be matched an event early
 * from then on.
 */
MU_TEST(an_events_deadline_stops_short_of_the_next_event)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;
	uint32_t window_opens;

	/* The shortest interval, 7.5 ms, with the longest window it allows:
	 * 6.25 ms, one step below the interval. */
	start_fresh();
	le.do_follow = 1;
	r.conn_interval = 6;
	r.win_size = 5;
	r.win_offset = 0;
	build_connect_req(&buf, &r);
	le_connect_handler(&buf);
	mu_assert_int_eq(6 * 12500, conn.conn_interval);
	mu_assert_int_eq(5 * 12500, conn.win_size);

	window_opens = buf.timestamp + PACKET_DURATION(&buf) + USEC(1250);
	fake_t1tc = window_opens - RX_WARMUP_TIME;
	fake_t1ir = TIR_MR0_Interrupt;
	le_phy_active = 1;
	TIMER1_IRQHandler();

	/* One interval less the warmup, not the 6.25 ms window plus a 2.12 ms
	 * packet plus the warmup, which would have run past the moment the next
	 * event has to be armed. */
	mu_assert_int_eq(fake_t1tc + 6 * 12500 - RX_WARMUP_TIME, fake_t1mr0);
}

/*
 * An estimate the counter has already gone past skips whole intervals, and
 * the events skipped are counted.
 *
 * Events can be missed: an event may run to a deadline as long as its whole
 * interval, and an interrupt handler can hold the processor across one.
 * Arming the match where the estimate says would then put it behind the
 * counter, where it waits for the counter to wrap.
 */
MU_TEST(an_overdue_estimate_skips_whole_intervals_and_counts_them)
{
	anchored_connection(100000);
	conn.hop_increment = 7;
	conn.channel_idx = 3;
	conn_event.num_packets = 0;
	conn_event.opened = 1;
	/* Four and a half intervals past where the next event was due. */
	fake_t1tc = 100000 + 24 * 12500 + 9 * (24 * 12500) / 2;

	finish_conn_event();

	/* The silent event took the estimate one interval on, and four more
	 * were skipped to get ahead of the counter. */
	mu_assert_int_eq(100000 + 5 * 24 * 12500, conn.last_anchor);
	mu_assert_int_eq(100000 + 6 * 24 * 12500 - RX_WARMUP_TIME, fake_t1mr0);
	mu_assert(fake_t1mr0 > fake_t1tc, "the next event was armed in the past");
	/* One for the event that closed, four for the ones that went by. */
	mu_assert_int_eq(5 + 1 + 4, conn.conn_event_counter);
	/* And the hops of those four events, because the connection took them:
	 * change_channel() takes one more when it opens the event this match is
	 * for. Left behind, the receiver would be tuned where the connection
	 * was four events ago and hear nothing from it. */
	mu_assert_int_eq((3 + 4 * 7) % 37, conn.channel_idx);
}

/*
 * ...and the same across the counter's wrap, which comes round every 429
 * seconds and so inside any connection worth following.
 *
 * The comparison is a signed difference for that reason: an estimate a little
 * past the end of the counter against a reading a little past its start has to
 * read as "ahead by a little", not as "behind by 429 seconds".
 */
MU_TEST(the_overdue_test_holds_across_the_counters_wrap)
{
	const uint32_t interval = 24 * 12500;

	/* An anchor just below the counter's end, and a reading just above its
	 * start: half an interval has gone by, over the wrap. Nothing is skipped
	 * -- the next event is still half an interval ahead -- which is the half
	 * of the comparison that a difference read as unsigned would get wrong,
	 * by 429 seconds. */
	anchored_connection(0u - interval / 2);
	conn_event.num_packets = 0;
	conn_event.opened = 1;
	fake_t1tc = interval / 2;

	finish_conn_event();

	mu_assert_int_eq((uint32_t)(0u - interval / 2) + interval,
	                 conn.last_anchor);
	mu_assert_int_eq(5 + 1, conn.conn_event_counter);
	mu_assert_int_eq(conn.last_anchor + interval - RX_WARMUP_TIME,
	                 fake_t1mr0);

	/* And the other half: the same wrap with two and a half intervals gone,
	 * so the estimate is overdue and whole intervals are skipped across the
	 * end of the counter. */
	anchored_connection(0u - interval / 2);
	conn.hop_increment = 7;
	conn.channel_idx = 1;
	conn_event.num_packets = 0;
	conn_event.opened = 1;
	fake_t1tc = interval / 2 + 2 * interval;

	finish_conn_event();

	/* The closed event took the estimate one interval on, and two more were
	 * skipped to get ahead of the counter. */
	mu_assert_int_eq((uint32_t)(0u - interval / 2) + 3 * interval,
	                 conn.last_anchor);
	mu_assert_int_eq(5 + 1 + 2, conn.conn_event_counter);
	mu_assert_int_eq((1 + 2 * 7) % 37, conn.channel_idx);
	mu_assert_int_eq(conn.last_anchor + interval - RX_WARMUP_TIME,
	                 fake_t1mr0);
	mu_assert((int32_t)(fake_t1mr0 - fake_t1tc) > 0,
	          "the next event was armed behind the counter");
}

/*
 * A connection that ends at its supervision timeout leaves no connection-event
 * match armed.
 *
 * The match that has just been taken is behind the counter, and connection
 * events are the only thing it is armed for. Left enabled it would be taken
 * again when the counter wrapped, 429 seconds later, and retune the radio
 * under whatever the mode was doing by then.
 */
MU_TEST(a_connection_that_times_out_leaves_no_match_armed)
{
	anchored_connection(100000);
	conn.supervision_timeout = MSEC(100);
	conn_event.num_packets = 0;
	conn_event.opened = 1;
	/* Nothing received for longer than the supervision timeout. */
	fake_t1tc = conn.last_packet_ts + MSEC(200);

	finish_conn_event();

	mu_assert(connection_was_reset(), "the connection was not dropped");
	mu_assert(!(fake_t1mcr & TMCR_MR0I),
	          "a connection-event match is still armed with no connection");
}

/*
 * A capture transfer that fails is counted, so that a capture losing packets
 * to the hardware tells itself apart from a quiet channel.
 */
MU_TEST(a_failed_capture_transfer_is_counted)
{
	start_fresh();
	le_phy_active = 1;
	rf_channel = ADV_CHANNEL_MHZ;
	transfer_errors_counted = 0;

	/* The controller reports an error on the channel the capture receives
	 * into, and no byte with it. */
	fake_dmac_int_stat = 1 << 0;
	fake_dmac_int_tc_stat = 0;
	fake_dmac_int_err_stat = 1 << 0;
	fake_dmac_int_err_clr = 0;

	le_DMA_IRQHandler();

	mu_assert_int_eq(1, transfer_errors_counted);
	mu_assert_int_eq(1 << 0, fake_dmac_int_err_clr);
}

/* Report one byte of a packet, and a transfer error with it if `error` says
 * so. The value is what the DMA controller left in the buffer it alternates
 * between, which is the byte the engine reads. */
static void rx_byte(uint8_t value, uint32_t at, int error)
{
	fake_dmac_int_stat = 1 << 0;
	fake_dmac_int_tc_stat = 1 << 0;
	fake_dmac_int_err_stat = error ? (1 << 0) : 0;
	le_dma_dest[0] = value;
	le_dma_dest[1] = value;
	le_dma_interrupt(at);
}

/* Report a transfer error on its own, with no byte. */
static void rx_transfer_error(uint32_t at)
{
	fake_dmac_int_stat = 1 << 0;
	fake_dmac_int_tc_stat = 0;
	fake_dmac_int_err_stat = 1 << 0;
	fake_dmac_int_err_clr = 0;
	le_dma_interrupt(at);
}

/* The byte that reads as a PDU of no bytes on `channel`: five in all, with the
 * header and the CRC. The length is whitened on the air, so which byte that is
 * depends on the channel. */
static uint8_t length_byte_for_no_pdu(unsigned channel)
{
	unsigned v;

	for (v = 0; v < 256; v++)
		if (dewhiten_length(channel, (uint8_t)v) == 0)
			break;

	/* The whitening only exclusive-ors and reorders the bits, so exactly one
	 * byte reads as zero on any channel. */
	mu_assert(v < 256, "no byte reads as a length of zero");
	return (uint8_t)v;
}

/*
 * A capture transfer that fails part way through a packet on a data channel
 * closes the connection event, instead of leaving the capture with nothing
 * armed.
 *
 * The event's own deadline was cleared when the packet's first byte arrived,
 * so that the inter-frame timeout could take over, and that timeout is only
 * armed where a packet ends -- which this packet never will. The synthesizer
 * and buffer waits were disarmed when receive was armed. So without this the
 * engine holds nothing it will act on: the event never closes, the supervision
 * timeout is only checked when one does, and the capture is over while the
 * mode's loop goes on servicing USB, so the board answers the host as if
 * nothing had happened.
 *
 * This drives the receive interrupt, which reaches the radio, so the fakes
 * count those accesses; what is asserted is which match the engine armed and
 * what it did with the packet, not anything the radio would have replied.
 */
MU_TEST(a_failed_transfer_on_a_data_channel_closes_the_connection_event)
{
	le_rx_t *buf;
	uint16_t event_before;

	anchored_connection(1000000);
	le_phy_active = 1;
	rf_channel = DATA_CHANNEL_MHZ;
	/* An event is open, which is the only state receive is armed in. */
	conn_event.opened = 1;
	current_rxbuf = buffer_get();
	mu_assert_fatal(current_rxbuf != NULL, "the pool gave out no buffer");
	buf = current_rxbuf;
	transfer_errors_counted = 0;
	fake_t1tc = 1000000;
	event_before = conn.conn_event_counter;

	/* The packet's first byte arrives and clears the event's deadline. */
	rx_byte(0x9c, fake_t1tc, 0);
	mu_assert_int_eq(1, buf->pos);
	mu_assert(!(fake_t1mcr & TMCR_MR0I),
	          "the first byte did not clear the event's deadline");

	/* The transfer then fails, with no further byte. */
	rx_transfer_error(fake_t1tc);

	mu_assert_int_eq(1, transfer_errors_counted);
	mu_assert_int_eq(1 << 0, fake_dmac_int_err_clr);
	/* The half-received packet is dropped, so the next one starts at the
	 * head of the buffer rather than part way along it. */
	mu_assert_int_eq(0, buf->pos);
	mu_assert_int_eq(0, buf->size);
	mu_assert(queue_remove(&packet_queue, (void **)&buf) == 0,
	          "half a packet was handed to the host");
	/* A match is armed again, a margin ahead of the counter: the shortest
	 * wait that cannot be passed before it is armed. */
	mu_assert(fake_t1mcr & TMCR_MR0I,
	          "no match was armed, so no interrupt is due and the capture "
	          "has stopped");
	mu_assert_int_eq(fake_t1tc + CONN_EVENT_ARM_MARGIN, fake_t1mr0);

	/* Taking it closes the event and arms the next one. */
	fake_t1tc = fake_t1mr0;
	fake_t1ir = TIR_MR0_Interrupt;
	TIMER1_IRQHandler();

	mu_assert_int_eq(event_before + 1, conn.conn_event_counter);
	mu_assert(fake_t1mcr & TMCR_MR0I, "the next connection event was not armed");
	mu_assert((int32_t)(fake_t1mr0 - fake_t1tc) > 0,
	          "the next event was armed behind the counter");

	/* ...and opening that event puts the DMA channel and the SSP back and
	 * waits for the synthesizer, which is where receive is armed again. This
	 * runs change_channel(), which touches the radio, so the fakes count
	 * those accesses; what is asserted is the SSP and the match. */
	ssp_starts = 0;
	fake_t1tc = fake_t1mr0;
	fake_t1ir = TIR_MR0_Interrupt;
	TIMER1_IRQHandler();

	mu_assert(ssp_starts > 0, "the SSP was not started for the next event");
	mu_assert(fake_t1mcr & TMCR_MR2I,
	          "the next event is not waiting for the synthesizer, so receive "
	          "is never armed again");

	/* And when the synthesizer reports locked, the DMA interrupt the recovery
	 * disabled is enabled again, so the bytes of the next packet are reported.
	 * Without this the capture would be just as silent, one step further on. */
	fake_iser0 = 0;
	ssp_starts = 0;
	radio_status = FS_LOCK;
	fake_t1ir = TIR_MR2_Interrupt;
	TIMER1_IRQHandler();

	mu_assert(fake_iser0 & ISER0_ISE_DMA,
	          "the DMA interrupt was left disabled, so no byte is reported");
	mu_assert(ssp_starts > 0, "the SSP was not started before receive");
	mu_assert(!(fake_t1mcr & TMCR_MR2I), "the wait for the lock is still armed");
}

/*
 * A failed transfer leaves a match that is already armed alone.
 *
 * The error can arrive before the packet's first byte, which is what clears
 * the connection event's deadline, and that deadline is when the event is
 * meant to close. The timer interrupt also preempts the receive interrupt, so
 * a match can be taken while the recovery runs and finish_conn_event() will
 * have armed the next event by then; arming over either would open an event an
 * interval early and lose its packets.
 */
MU_TEST(a_failed_transfer_leaves_a_match_already_armed_alone)
{
	const uint32_t deadline = 1000000 + MSEC(5);

	anchored_connection(1000000);
	le_phy_active = 1;
	rf_channel = DATA_CHANNEL_MHZ;
	conn_event.opened = 1;
	current_rxbuf = buffer_get();
	mu_assert_fatal(current_rxbuf != NULL, "the pool gave out no buffer");
	fake_t1tc = 1000000;
	/* The event is open and running to its own deadline: no byte of a packet
	 * has arrived yet to clear it. */
	timer1_set_match(deadline);

	/* One strobe stops the receiver, and the recovery reaches the radio for
	 * nothing else: the event's own match is what carries on from here. */
	radio_touched = 0;
	fake_dmac_int_tc_clear = 0;
	fake_ssp1dmacr = SSPDMACR_RXDMAE;
	rx_transfer_error(fake_t1tc);

	mu_assert(fake_t1mcr & TMCR_MR0I, "the event's deadline was disarmed");
	mu_assert_int_eq(deadline, fake_t1mr0);
	mu_assert_int_eq(1, radio_touched);
	mu_assert(fake_icer0 & ICER0_ICE_DMA,
	          "the DMA interrupt was left enabled over a torn-down channel");
	/* The transfer is taken down as the end-of-packet path takes it down. The
	 * receiver keeps clocking bytes out until the strobe takes effect, and
	 * those would be read as the head of the next packet. */
	mu_assert_int_eq(0, (int)fake_dmacc0_config);
	mu_assert_int_eq(1 << 0, (int)fake_dmac_int_tc_clear);
	mu_assert_int_eq(0, (int)(fake_ssp1dmacr & SSPDMACR_RXDMAE));
}

/*
 * With no connection left to follow, the recovery waits for the synthesizer
 * rather than arming a connection event.
 *
 * This is how the state machine is found when the connection ends at its
 * supervision timeout while the recovery runs: the timer interrupt preempts
 * the receive interrupt, and both paths that stop following disarm the match
 * deliberately, because connection events are the only thing it is armed for.
 * An event opened with no connection to open it for retunes the radio under
 * whatever the mode is doing instead -- and arming nothing would leave the same
 * silence the recovery is here to prevent.
 */
MU_TEST(a_failed_transfer_arms_no_event_with_no_connection)
{
	start_fresh();
	le_phy_active = 1;
	/* The radio is still on a data channel: the follow has just ended, and
	 * change_channel() has not run yet. */
	rf_channel = DATA_CHANNEL_MHZ;
	current_rxbuf = buffer_get();
	mu_assert_fatal(current_rxbuf != NULL, "the pool gave out no buffer");
	mu_assert_fatal(conn.conn_interval == 0, "a connection is still being followed");

	rx_transfer_error(1000);

	mu_assert(!(fake_t1mcr & TMCR_MR0I),
	          "a connection event was armed with no connection to follow");
	mu_assert(fake_t1mcr & TMCR_MR2I,
	          "nothing was armed at all, which is the silence this recovers");
}

/*
 * On an advertising channel a failed transfer arms receive again. There is no
 * connection event to close there, and the engine stays on the channel until
 * the host or a connection request moves it, so the wait for the synthesizer
 * is what puts the DMA channel and receive back.
 *
 * A connection-event match can be armed on an advertising channel, for the
 * first event of a connection whose request the mode's loop has just read, so
 * this path has to leave MR0 alone.
 */
MU_TEST(a_failed_transfer_on_an_advertising_channel_arms_receive_again)
{
	le_rx_t *buf;
	const uint32_t first_event = 0x00300000;

	start_fresh();
	le_phy_active = 1;
	rf_channel = ADV_CHANNEL_MHZ;
	current_rxbuf = buffer_get();
	mu_assert_fatal(current_rxbuf != NULL, "the pool gave out no buffer");
	buf = current_rxbuf;
	transfer_errors_counted = 0;
	fake_t1tc = 0x00200000;

	/* Two bytes of an advertising packet have arrived: its header, and the
	 * length the engine reads the packet's size from. */
	rx_byte(0x42, fake_t1tc, 0);
	rx_byte(0x08, fake_t1tc + USEC(8), 0);
	mu_assert_int_eq(2, buf->pos);
	mu_assert(buf->size > 2, "the engine read no size from the header");

	/* A connection request has just been followed: the engine holds the
	 * connection's parameters and its first event is armed, while the radio is
	 * still on the advertising channel because no hop has happened yet. So it
	 * is the channel, not the absence of a connection, that decides what the
	 * recovery does here. */
	conn.conn_interval = MSEC(10);
	timer1_set_match(first_event);

	/* One strobe stops the receiver; the wait for the lock reaches the radio
	 * for nothing else until it fires. */
	radio_touched = 0;
	rx_transfer_error(fake_t1tc + USEC(16));

	mu_assert_int_eq(1, radio_touched);
	mu_assert(fake_icer0 & ICER0_ICE_DMA,
	          "the DMA interrupt was left enabled over a torn-down channel");
	mu_assert_int_eq(1, transfer_errors_counted);
	mu_assert_int_eq(1 << 0, fake_dmac_int_err_clr);
	mu_assert_int_eq(0, buf->pos);
	mu_assert_int_eq(0, buf->size);
	mu_assert(fake_t1mcr & TMCR_MR2I,
	          "receive was not armed again, so the next packet never arrives");
	/* The connection event the mode's loop armed is untouched. */
	mu_assert(fake_t1mcr & TMCR_MR0I, "the first connection event was disarmed");
	mu_assert_int_eq(first_event, fake_t1mr0);
}

/*
 * The controller can report an error in the same interrupt as the byte that
 * completed a packet. That packet is kept: it is whole, it goes to the host,
 * and the arming the end-of-packet path did for the next one stands.
 *
 * The error is still acknowledged, because a flag left set is taken as a fresh
 * error the moment the DMA interrupt is enabled again.
 */
MU_TEST(a_packet_that_completed_survives_an_error_in_the_same_interrupt)
{
	le_rx_t *buf;
	le_rx_t *queued = NULL;
	unsigned length_byte;
	unsigned guard;

	start_fresh();
	le_phy_active = 1;
	rf_channel = ADV_CHANNEL_MHZ;
	current_rxbuf = buffer_get();
	mu_assert_fatal(current_rxbuf != NULL, "the pool gave out no buffer");
	buf = current_rxbuf;
	transfer_errors_counted = 0;
	fake_dmac_int_err_clr = 0;
	fake_t1tc = 0x00200000;

	rx_byte(0x42, fake_t1tc, 0);
	rx_byte(length_byte_for_no_pdu(ADV_CHANNEL_MHZ), fake_t1tc + USEC(8), 0);
	mu_assert_int_eq(2 + 3, buf->size);

	/* The rest of the packet, with the controller reporting an error
	 * alongside its last byte. */
	for (guard = 0; buf->pos + 1 < buf->size && guard < sizeof(buf->data); guard++)
		rx_byte(0x00, fake_t1tc + USEC(16), 0);
	rx_byte(0x00, fake_t1tc + USEC(24), 1);

	mu_assert_int_eq(1, transfer_errors_counted);
	mu_assert_int_eq(1 << 0, fake_dmac_int_err_clr);
	mu_assert(queue_remove(&packet_queue, (void **)&queued) == 1,
	          "the completed packet was thrown away with the error");
	mu_assert(queued == buf, "a different buffer reached the host");
	mu_assert_int_eq(2 + 3, queued->pos);
	/* The end-of-packet path took a fresh buffer and waited for the
	 * synthesizer; the error did not undo either. */
	mu_assert(current_rxbuf != NULL && current_rxbuf != buf,
	          "no buffer was taken for the next packet");
	mu_assert(fake_t1mcr & TMCR_MR2I, "receive was not armed for the next packet");
}

/*
 * A packet that completed with an error in the same interrupt does not get a
 * second wait armed on top of the one its own path armed.
 *
 * With the pool exhausted the end-of-packet path waits for a buffer, which is
 * the only thing that can carry on: receive must not be armed without one. The
 * recovery would start a wait for the synthesizer as well, and that wait ends
 * by finding no buffer and waiting for one all over again.
 */
MU_TEST(a_completed_packet_does_not_get_a_second_wait_armed)
{
	unsigned taken;

	start_fresh();
	le_phy_active = 1;
	rf_channel = ADV_CHANNEL_MHZ;
	current_rxbuf = buffer_get();
	mu_assert_fatal(current_rxbuf != NULL, "the pool gave out no buffer");
	/* Every other buffer is in use, so the packet below leaves none. */
	for (taken = 1; taken < LE_BUFFER_POOL_SIZE; taken++)
		mu_assert_fatal(buffer_get() != NULL, "the pool ran out early");
	fake_t1tc = 0x00200000;

	rx_byte(0x42, fake_t1tc, 0);
	rx_byte(length_byte_for_no_pdu(ADV_CHANNEL_MHZ), fake_t1tc + USEC(8), 0);
	mu_assert_int_eq(2 + 3, current_rxbuf->size);
	rx_byte(0x00, fake_t1tc + USEC(16), 0);
	rx_byte(0x00, fake_t1tc + USEC(24), 0);
	/* The last byte of the packet, and the error with it. */
	rx_byte(0x00, fake_t1tc + USEC(32), 1);

	mu_assert_int_eq(1, transfer_errors_counted);
	mu_assert(current_rxbuf == NULL, "a buffer came from an exhausted pool");
	mu_assert(fake_t1mcr & TMCR_MR3I, "nothing is waiting for a buffer");
	mu_assert(!(fake_t1mcr & TMCR_MR2I),
	          "a wait for the synthesizer was armed as well, which can only "
	          "end by waiting for a buffer again");
}

/*
 * A packet the controller completes in the same interrupt as a transfer error
 * is kept -- it is whole, and the host asked for it -- and it goes to the host
 * marked, so a reader can tell it apart from a clean one. The bytes went
 * through a controller that reported a fault, so some of them may be wrong
 * while the packet is still the right length; CRC checking is off by default,
 * so nothing else would flag it. The Classic capture marks its own records on
 * the same condition.
 */
MU_TEST(a_packet_completed_as_the_transfer_faulted_is_marked)
{
	le_rx_t *packet;
	uint8_t status;

	start_fresh();

	packet = a_packet_arrives(10, DATA_CHANNEL_MHZ, 1);
	mu_assert_fatal(packet != NULL,
	                "the completed packet was thrown away with the fault");

	/* Still counted and still acknowledged: a flag left set is read as a
	 * fresh error the moment the interrupt is enabled again. */
	mu_assert_int_eq(1, transfer_errors_counted);
	mu_assert_int_eq(1, errors_acknowledged);
	mu_assert_int_eq(0, fake_dmac_int_err_stat & (1 << 0));

	status = le_status_read_for(packet);
	mu_assert_int_eq(DMA_ERROR, status & DMA_ERROR);
	/* And nothing else: no packet was lost for want of room in the ring. */
	mu_assert_int_eq(0, status & FIFO_OVERFLOW);
	buffer_release(packet);
}

/* The same on an advertising channel, which takes the other way out of the
 * end-of-packet path: there is no connection event to close, so the engine
 * waits for the synthesizer instead of arming the next event. */
MU_TEST(an_advertising_packet_completed_as_the_transfer_faulted_is_marked)
{
	le_rx_t *packet;

	start_fresh();

	packet = a_packet_arrives(10, ADV_CHANNEL_MHZ, 1);
	mu_assert_fatal(packet != NULL,
	                "the completed packet was thrown away with the fault");
	mu_assert_int_eq(1, transfer_errors_counted);
	mu_assert_int_eq(1, errors_acknowledged);

	mu_assert_int_eq(DMA_ERROR, le_status_read_for(packet) & DMA_ERROR);
	buffer_release(packet);
}

/*
 * And on the second packet of a connection event, which is the one end of
 * packet that leaves the interrupt by a return of its own: the event is closed
 * and the next one armed in there. The packet is marked, and the error is
 * acknowledged and counted on the way out like any other -- an error left
 * latched would be read as a fresh one as soon as the DMA interrupt was
 * enabled again, which is why it is taken before the byte rather than after.
 */
MU_TEST(a_fault_on_the_packet_that_closes_an_event_is_marked_and_acknowledged)
{
	le_rx_t *first, *second;

	start_fresh();
	/* A connection whose events the engine is following, so the second
	 * packet of an event closes it. */
	anchored_connection(100000);
	conn_event.num_packets = 0;
	conn_event.opened = 1;

	first = a_packet_arrives(10, DATA_CHANNEL_MHZ, 0);
	mu_assert_fatal(first != NULL, "the first packet of the event was lost");
	mu_assert_int_eq(1, conn_event.num_packets);
	mu_assert_int_eq(0, le_status_read_for(first) & DMA_ERROR);
	buffer_release(first);

	second = a_packet_arrives(10, DATA_CHANNEL_MHZ, 1);
	mu_assert_fatal(second != NULL,
	                "the packet that closed the event was thrown away with "
	                "the fault");
	mu_assert_int_eq(1, transfer_errors_counted);
	mu_assert_int_eq(1, errors_acknowledged);
	mu_assert_int_eq(0, fake_dmac_int_err_stat & (1 << 0));
	mu_assert_int_eq(DMA_ERROR, le_status_read_for(second) & DMA_ERROR);
	buffer_release(second);
}

/*
 * The mark belongs to the packet the fault arrived with, and to no other. A
 * capture is read packet by packet, so a bit left set would put every packet
 * after the fault under the same doubt.
 */
MU_TEST(a_clean_packet_is_not_marked)
{
	le_rx_t *packet;

	start_fresh();

	/* One with no fault at all. */
	packet = a_packet_arrives(10, ADV_CHANNEL_MHZ, 0);
	mu_assert_fatal(packet != NULL, "the packet was not queued");
	mu_assert_int_eq(0, le_status_read_for(packet) & DMA_ERROR);
	mu_assert_int_eq(0, transfer_errors_counted);
	buffer_release(packet);

	/* ...and one that follows a fault: the packet the fault happened to is
	 * already on its way to the host, and this one arrived cleanly. */
	packet = a_packet_arrives(10, ADV_CHANNEL_MHZ, 1);
	mu_assert_fatal(packet != NULL, "the faulted packet was not queued");
	mu_assert_int_eq(DMA_ERROR, le_status_read_for(packet) & DMA_ERROR);
	buffer_release(packet);

	packet = a_packet_arrives(10, ADV_CHANNEL_MHZ, 0);
	mu_assert_fatal(packet != NULL,
	                "the packet after the fault was not queued");
	mu_assert_int_eq(0, le_status_read_for(packet) & DMA_ERROR);
	buffer_release(packet);
}

/*
 * A fault part way through a packet takes the packet with it: it is not whole,
 * and the recovery drops what was received. The mark goes with it too. The
 * recovery keeps the buffer rather than releasing it -- nothing was handed to
 * the host, so the pool's accounting is unchanged -- so the next packet
 * received into that same buffer must not reach the host wearing the fault of
 * the one that was lost.
 */
MU_TEST(a_fault_part_way_through_a_packet_does_not_mark_the_next_one)
{
	le_rx_t *packet;

	start_fresh();

	mu_assert(a_packet_arrives_with_a_fault_at(10, ADV_CHANNEL_MHZ, 3, 4)
	          == NULL, "part of a packet was handed to the host");
	mu_assert_int_eq(1, transfer_errors_counted);
	mu_assert_int_eq(1, errors_acknowledged);

	packet = a_packet_arrives(10, ADV_CHANNEL_MHZ, 0);
	mu_assert_fatal(packet != NULL,
	                "the packet after the fault was not queued");
	mu_assert_int_eq(0, le_status_read_for(packet) & DMA_ERROR);
	buffer_release(packet);
}

/*
 * The two flags a record can carry are kept apart. A packet that follows a
 * drop and faulted carries both: the gap in the capture is what the host
 * reads today, and a fault must not take it away.
 */
MU_TEST(a_fault_does_not_hide_a_gap_in_the_capture)
{
	le_rx_t *packet;
	uint8_t status;

	start_fresh();

	/* A packet was lost for want of room in the ring, as the engine
	 * records it. */
	le_packet_dropped = 1;

	packet = a_packet_arrives(10, ADV_CHANNEL_MHZ, 1);
	mu_assert_fatal(packet != NULL, "the faulted packet was not queued");
	status = le_status_read_for(packet);
	mu_assert_int_eq(DMA_ERROR, status & DMA_ERROR);
	mu_assert_int_eq(FIFO_OVERFLOW, status & FIFO_OVERFLOW);
	buffer_release(packet);
}

/*
 * A moment the connection's parameters name, which the counter has already
 * gone past, is taken as soon as the counter allows rather than armed behind
 * it. There are no whole intervals to skip for this one -- it is a window
 * offset inside one event -- so the best that can be done is now.
 */
MU_TEST(a_moment_already_past_is_armed_just_ahead_of_the_counter)
{
	start_fresh();
	fake_t1tc = 500000;

	/* Ahead of the counter: armed where asked. */
	timer1_set_match_ahead(600000);
	mu_assert_int_eq(600000, fake_t1mr0);

	/* Gone past: armed a margin ahead instead, not behind the counter,
	 * where it would wait for the counter to wrap. */
	timer1_set_match_ahead(400000);
	mu_assert_int_eq(500000 + CONN_EVENT_ARM_MARGIN, fake_t1mr0);

	/* Within the margin counts as past, because arming takes longer than
	 * that. */
	timer1_set_match_ahead(500000 + CONN_EVENT_ARM_MARGIN / 2);
	mu_assert_int_eq(500000 + CONN_EVENT_ARM_MARGIN, fake_t1mr0);
}

/*
 * A connection update whose instant the counter stepped over still applies.
 *
 * Events can go by unobserved -- an overdue estimate skips whole intervals and
 * counts them -- so the instant may never be the counter's exact value. An
 * equality test left the update pending for the rest of the connection, and
 * the follow ran on the interval and supervision timeout the connection had
 * left behind.
 */
MU_TEST(an_instant_the_counter_stepped_over_still_applies)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;

	start_fresh();
	le.do_follow = 1;
	build_connect_req(&buf, &r);
	le_connect_handler(&buf);
	mu_assert_fatal(conn.remapping.total_channels == 37,
	                "the connection was not followed");
	conn.last_anchor = 1000000;
	conn.anchor_error = 0;

	{
		struct conn_update u = valid_conn_update();

		build_conn_update(&buf, &u);
		connection_update_handler(&buf);
	}
	mu_assert_int_eq(1, conn.conn_update_pending);

	/* The connection is at event 9; the update's instant was 7, which went
	 * by while nothing was being received. */
	conn.conn_event_counter = 9;
	le_phy_active = 1;
	fake_t1tc = 1000000;
	fake_t1ir = TIR_MR0_Interrupt;
	TIMER1_IRQHandler();
	fake_t1ir = TIR_MR0_Interrupt;
	TIMER1_IRQHandler();

	mu_assert_int_eq(0, conn.conn_update_pending);
	mu_assert_int_eq(40 * 12500, conn.conn_interval);
}

/*
 * The access address a packet is recorded with follows the radio.
 *
 * It is written where the sync word is, so that it says what the radio is
 * listening for rather than what the engine has decided to listen for next.
 * This runs le_cc2400_init_rf(), which touches the radio, so the fakes count
 * the accesses; what is asserted is the one value it is responsible for.
 */
MU_TEST(the_recorded_access_address_follows_the_radio)
{
	start_fresh();
	rf_channel = ADV_CHANNEL_MHZ;
	mu_assert_int_eq(ADV_AA, le_sync_aa);

	conn.access_address = 0xdeadbeef;
	le_cc2400_init_rf();

	mu_assert_int_eq(0xdeadbeef, le_sync_aa);
}

/*
 * A connection is followed from nothing, not from what the last one left.
 *
 * The mode's teardown leaves the connection state alone, so a run stopped part
 * way through a connection leaves its event counter, hop position, channel map
 * and any change still waiting for an instant behind. Followed with those, a
 * new connection matches every instant thousands of events out.
 */
MU_TEST(a_new_connection_starts_from_nothing)
{
	struct connect_req r = valid_connect_req();
	le_rx_t buf;

	start_fresh();
	le.do_follow = 1;

	/* What a run stopped part way through a connection leaves. */
	conn.conn_event_counter = 40000;
	conn.channel_idx = 21;
	conn.conn_update_pending = 1;
	conn.conn_update_instant = 123;
	conn.channel_map_update_pending = 1;

	build_connect_req(&buf, &r);
	le_connect_handler(&buf);

	mu_assert_int_eq(0, conn.conn_event_counter);
	mu_assert_int_eq(0, conn.channel_idx);
	mu_assert_int_eq(0, conn.conn_update_pending);
	mu_assert_int_eq(0, conn.channel_map_update_pending);
	/* ...and the connection the request named is being followed. */
	mu_assert_int_eq(0xdeadbeef, conn.access_address);
	mu_assert_int_eq(37, conn.remapping.total_channels);
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

/*
 * Neither of the two waits below may hold an interrupt handler for long. The
 * loop in le_phy_main() is the only thing servicing USB while this mode runs,
 * and it runs beneath both handlers, so a wait that does not end leaves the
 * board answering no control request at all: a live `ubertooth-btle -n`
 * capture died with "USB poll failed" after a few minutes, and the watchdog
 * then reset the board.
 *
 * Note what a test for a wait that used to be unbounded can and cannot do: if
 * the bound is taken away the binary hangs rather than reporting a failure,
 * the way test_cc2400_wait.c's do.
 */

/* The receive buffer never empties -- an SSP that has stopped answering, or a
 * radio that kept clocking symbols in because the strobe that should have
 * stopped it was lost. The drain has to end anyway. */
MU_TEST(emptying_the_symbol_buffer_gives_up_rather_than_spinning)
{
	start_fresh();
	fake_ssp1sr = SSPSR_RNE;

	le_ssp_drain();

	mu_assert(fake_ssp1sr == (uint32_t)SSPSR_RNE,
	          "the drain is expected to leave the status register alone");
}

/*
 * The wait for the synthesizer to lock keeps its short interval while a
 * healthy lock would arrive, then backs off to one no interrupt entry can
 * outrun, so the loop underneath gets its time back. It also re-asserts the
 * strobe once it has backed off, as cc2400_wait_fs() does, in case a handler
 * switched the synthesizer off between re-checks.
 *
 * cc2400_status() returns 0 in this file, so the lock never arrives.
 */
MU_TEST(an_overdue_synthesizer_lock_backs_off_and_strobes_again)
{
	unsigned i;

	start_fresh();
	fake_t1tc = 5000;

	timer1_wait_fs_lock();
	mu_assert_int_eq(5000 + USEC(3), (int)fake_t1mr2);
	mu_assert(fake_t1mcr & TMCR_MR2I, "the re-check must be armed");

	/* Every re-check inside the window keeps the short interval and does
	 * not touch the radio. */
	radio_touched = 0;
	for (i = 1; i < FS_LOCK_FAST_RECHECKS; ++i)
		timer1_recheck_fs_lock();
	mu_assert_int_eq(5000 + USEC(3), (int)fake_t1mr2);
	mu_assert_int_eq(0, radio_touched);

	/* Past the window the interval is the long one, and it stays that way
	 * rather than counting round to the short one again. */
	for (i = 0; i < FS_LOCK_SLOW_RESTROBE * 3; ++i)
		timer1_recheck_fs_lock();
	mu_assert_int_eq(5000 + USEC(100), (int)fake_t1mr2);

	/* The strobe comes every sixteenth slow re-check, not every one: a
	 * strobe arriving while the synthesizer is on its way to a lock starts
	 * it over. */
	mu_assert_int_eq(3, radio_touched);
}

/* A fresh wait starts over, so one overdue lock does not make the next
 * channel's lock slow to be noticed. */
MU_TEST(a_new_wait_for_a_lock_starts_with_the_short_interval)
{
	unsigned i;

	start_fresh();
	fake_t1tc = 5000;
	timer1_wait_fs_lock();
	for (i = 0; i < FS_LOCK_FAST_RECHECKS + FS_LOCK_SLOW_RESTROBE; ++i)
		timer1_recheck_fs_lock();
	mu_assert_int_eq(5000 + USEC(100), (int)fake_t1mr2);

	timer1_wait_fs_lock();
	mu_assert_int_eq(5000 + USEC(3), (int)fake_t1mr2);
}

int main(void)
{
	printf("le_phy capture path tests\n");

	MU_RUN(the_pool_hands_out_each_buffer_once_and_then_nothing);
	MU_RUN(a_released_buffer_comes_back_clean);
	MU_RUN(the_blink_leaves_the_radios_timer_alone);
	MU_RUN(the_blinks_counter_arms_no_match);
	MU_RUN(the_blink_still_lasts_ten_milliseconds);
	MU_RUN(a_second_packet_puts_the_blink_back_to_full_length);
	MU_RUN(a_blink_survives_the_counters_wrap);
	MU_RUN(disarming_one_match_cannot_lose_another);
	MU_RUN(each_match_is_armed_on_its_own);
	MU_RUN(two_contexts_cannot_be_handed_the_same_buffer);
	MU_RUN(the_pool_guards_its_flag_and_not_the_clear_that_follows);
	MU_RUN(a_queued_packet_is_not_reached_by_the_match_that_follows_it);
	MU_RUN(the_length_byte_is_dewhitened_the_same_way_as_the_rest);
	MU_RUN(a_field_is_read_least_significant_byte_first);
	MU_RUN(every_packet_length_survives_the_round_trip);
	MU_RUN(a_packet_too_long_for_a_chain_is_refused);
	MU_RUN(a_dropped_packet_marks_the_next_one_that_gets_through);
	MU_RUN(a_drop_does_not_carry_into_the_next_capture);
	MU_RUN(a_chain_that_does_not_fit_is_not_started);
	MU_RUN(the_average_rssi_is_the_mean_of_the_samples);
	MU_RUN(a_packets_timestamp_is_back_dated_to_the_start_of_the_preamble);
	MU_RUN(a_packet_carries_the_access_address_it_was_received_with);
	MU_RUN(a_full_ring_refuses_the_packet);
	MU_RUN(a_connection_request_is_accepted_and_scheduled);
	MU_RUN(a_connections_crc_init_is_reversed_as_a_24_bit_field);
	MU_RUN(a_connection_request_is_ignored_until_the_host_asks_to_follow);
	MU_RUN(a_connection_request_outside_the_specification_is_refused);
	MU_RUN(a_connection_request_of_the_wrong_length_is_left_alone);
	MU_RUN(a_new_connection_clears_a_pending_cancel);
	MU_RUN(a_connection_update_is_scheduled_for_its_instant);
	MU_RUN(a_connection_update_outside_the_specification_is_ignored);
	MU_RUN(an_updates_window_size_survives_being_scaled);
	MU_RUN(the_first_event_after_an_update_waits_out_the_whole_window);
	MU_RUN(the_first_event_of_a_connection_waits_out_the_whole_window);
	MU_RUN(a_channel_map_update_is_scheduled_for_its_instant);
	MU_RUN(an_update_is_never_seen_as_a_mixture_of_two);
	MU_RUN(a_channel_map_is_not_pending_while_it_is_being_written);
	MU_RUN(a_channel_map_update_with_no_channels_is_refused);
	MU_RUN(an_empty_map_does_not_divide_by_zero_in_the_remapping);
	MU_RUN(only_the_packets_the_engine_knows_are_acted_on);
	MU_RUN(a_target_filters_advertising_packets_only);
	MU_RUN(the_first_packet_of_an_event_anchors_it_and_waits_for_the_second);
	MU_RUN(a_deadline_that_expires_as_a_packet_starts_leaves_the_next_event_armed);
	MU_RUN(a_packet_from_a_closed_event_leaves_the_next_event_armed);
	MU_RUN(two_packets_in_an_event_anchor_it_where_they_arrived);
	MU_RUN(one_packet_anchors_the_event_only_near_the_prediction);
	MU_RUN(a_silent_event_moves_the_anchor_on_by_one_interval);
	MU_RUN(an_unmeasured_event_puts_its_one_packet_at_the_earliest_anchor);
	MU_RUN(an_empty_first_event_arms_the_next_one_ahead_of_the_counter);
	MU_RUN(an_events_deadline_stops_short_of_the_next_event);
	MU_RUN(an_overdue_estimate_skips_whole_intervals_and_counts_them);
	MU_RUN(the_overdue_test_holds_across_the_counters_wrap);
	MU_RUN(a_connection_that_times_out_leaves_no_match_armed);
	MU_RUN(a_moment_already_past_is_armed_just_ahead_of_the_counter);
	MU_RUN(an_instant_the_counter_stepped_over_still_applies);
	MU_RUN(the_recorded_access_address_follows_the_radio);
	MU_RUN(a_new_connection_starts_from_nothing);
	MU_RUN(a_failed_capture_transfer_is_counted);
	MU_RUN(a_failed_transfer_on_a_data_channel_closes_the_connection_event);
	MU_RUN(a_failed_transfer_leaves_a_match_already_armed_alone);
	MU_RUN(a_failed_transfer_arms_no_event_with_no_connection);
	MU_RUN(a_failed_transfer_on_an_advertising_channel_arms_receive_again);
	MU_RUN(a_packet_that_completed_survives_an_error_in_the_same_interrupt);
	MU_RUN(a_completed_packet_does_not_get_a_second_wait_armed);
	MU_RUN(a_packet_completed_as_the_transfer_faulted_is_marked);
	MU_RUN(an_advertising_packet_completed_as_the_transfer_faulted_is_marked);
	MU_RUN(a_fault_on_the_packet_that_closes_an_event_is_marked_and_acknowledged);
	MU_RUN(a_clean_packet_is_not_marked);
	MU_RUN(a_fault_part_way_through_a_packet_does_not_mark_the_next_one);
	MU_RUN(a_fault_does_not_hide_a_gap_in_the_capture);
	MU_RUN(a_torn_down_phy_ignores_a_dma_interrupt);
	MU_RUN(a_torn_down_phy_ignores_a_timer_match);
	MU_RUN(emptying_the_symbol_buffer_gives_up_rather_than_spinning);
	MU_RUN(an_overdue_synthesizer_lock_backs_off_and_strobes_again);
	MU_RUN(a_new_wait_for_a_lock_starts_with_the_short_interval);

	return mu_report();
}
