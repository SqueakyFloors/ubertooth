/*
 * Unit tests for the receive deadline in firmware/btbr/src/btphy/rx_task.c
 *
 * The BR/EDR image's receive task used to wait for a packet's bytes with two
 * bare spins on the DMA buffer:
 *
 *	while((size=rx_buf_update()) < NUM_PKT_HDR_BYTES);
 *
 * Bytes only appear there while the radio keeps clocking symbols into the SSP,
 * so a radio that stops part-way through a packet, or a DMA transfer that never
 * finishes, leaves nothing to end the wait. Both spins run from a slot task in
 * TIMER0_IRQHandler, and USB in this image is served from an interrupt of the
 * same priority, so the board answered the host nothing until it was
 * re-plugged. Each wait now has a deadline on the slot clock.
 *
 * rx_task.c is included into this translation unit, which is how the deadline
 * and the two callers around it can be called at all: they are static. The
 * hardware under them is faked -- stubs-btbr-rx/ubertooth.h supplies the slot
 * clock and the packet strobe, and this file implements the radio, the DMA
 * controller, the codec, the message pool, the hop sequence and the slot
 * scheduler. None of them do any work; the fake DMA controller is the only one
 * with behaviour, and it is what makes a wait end or not.
 *
 * This file is part of Project Ubertooth and is released under the terms of the
 * GPL. Refer to COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

/* The two diagnostics every btbr source can reach. They are declared by
 * stubs-btbr/ubtbr/debug.h, which the headers below pull in, and defined among
 * the fakes. */
void cprintf(char *fmt, ...);
void __attribute__((noreturn)) die(char *fmt, ...);

#include <ubtbr/btphy.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/codec.h>
#include <ubtbr/hop.h>
#include <ubtbr/rf.h>
#include <ubtbr/tdma_sched.h>
#include <ubtbr/ubertooth_dma.h>

/* ---- the fake slot clock and DMA controller ---- */

/*
 * The fake clock and the fake DMA controller share one model of time. The clock
 * counts 100ns steps, as TIMER0 does, and advances on every read, so a wait
 * that polls it makes progress. The DMA controller hands out the bytes the air
 * would have delivered by the current clock: nothing at sync_clock, when the
 * packet strobe goes low, then one byte every 8usec, up to bytes_available. The
 * first four of those are the high half of the access code, which the radio
 * does not match on and rx_buf_update() skips. Capping bytes_available is how a
 * radio that stops part-way through a packet is modelled.
 *
 * What it does not model: the microsecond or so between a byte's last air
 * symbol and the DMA controller's pointer moving, which is part of what the
 * deadline's slack is for. A byte here is visible the instant the air finishes
 * it, so these tests see slightly more margin than a board does.
 */
static uint32_t fake_clock;
static uint32_t fake_clock_step;
static uint32_t fake_sync_clock;
static unsigned fake_bytes_available;
static unsigned fake_clock_reads;

/* The clock reading a wait gave up on, so a test can see where it stopped
 * without the read advancing it. */
static uint32_t fake_clock_peek(void)
{
	return fake_clock;
}

uint32_t fake_clock_read(void)
{
	uint32_t now = fake_clock;

	fake_clock_reads++;
	fake_clock += fake_clock_step;
	return now;
}

int fake_gio6(void)
{
	return 0;
}

/*
 * How many bytes the DMA controller has written -- what DMACC0DestAddr minus
 * the buffer gives on the board. 80 steps of 100ns is the 8usec an air byte
 * takes.
 */
static unsigned fake_dma_bytes(void)
{
	unsigned bytes;

	if (fake_clock < fake_sync_clock)
		return 0;
	bytes = (fake_clock - fake_sync_clock) / 80;
	if (bytes > fake_bytes_available)
		bytes = fake_bytes_available;
	return bytes;
}

/*
 * Set up a receive whose sync word was detected at `sync_clock`, with
 * `bytes_available` bytes ever arriving in total, sync word included. One clock
 * step per poll unless a test asks for more.
 */
static void fake_receive(uint32_t sync_clock, unsigned bytes_available)
{
	fake_clock = sync_clock;
	fake_clock_step = 1;
	fake_sync_clock = sync_clock;
	fake_bytes_available = bytes_available;
	fake_clock_reads = 0;
}

/* ---- the rest of the receive task's world ---- */

void cprintf(char *fmt, ...) { (void)fmt; }

void __attribute__((noreturn)) die(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	abort();
}

btphy_t btphy;
btctl_t btctl;
volatile rf_state_t rf_state;

u8 cc2400_status(void) { return 0; }
u8 cc2400_strobe(u8 reg) { (void)reg; return 0; }
void cc2400_set(u8 reg, u16 val) { (void)reg; (void)val; }

/*
 * The radio's state machine. The receive task only checks that the radio is
 * receiving, which it is by default, so the deadline tests below see the state
 * they expect without saying so; the tests for the state machine wait set it
 * themselves, and count the reads.
 */
static u16 fake_fsm_state = STATE_STROBE_RX;
static unsigned fake_fsm_reads;

u16 cc2400_get(u8 reg)
{
	if (reg == FSMSTATE) {
		fake_fsm_reads++;
		return fake_fsm_state;
	}
	return 0;
}

/* The breadcrumb and the give-up counts, which the state machine wait writes.
 * Both are kept here so a test can read back what it recorded. */
static uint8_t fake_live_site;
static unsigned fake_fsm_timeouts;

uint8_t watchdog_mark(uint8_t site)
{
	uint8_t previous = fake_live_site;

	fake_live_site = site;
	return previous;
}

void cc2400_wait_count_timeout(enum cc2400_wait_shape shape)
{
	if (shape == CC2400_WAIT_FSM)
		fake_fsm_timeouts++;
}

void btphy_rf_tune_chan(uint16_t channel, int tx) { (void)channel; (void)tx; }
void btphy_rf_fifo_write(uint8_t *data, unsigned len) { (void)data; (void)len; }
void btphy_rf_enable_int(btbr_int_cb_t cb, void *cb_arg, int tx)
{ (void)cb; (void)cb_arg; (void)tx; }
void btphy_rf_disable_int(void) { }

void dio_ssp_start_rx(void) { }
void dio_ssp_stop(void) { }
void dma_init_rx_single(volatile void *buf, unsigned buf_size)
{ (void)buf; (void)buf_size; }

unsigned long dma_get_rx_offset(void)
{
	return fake_dma_bytes();
}

static uint8_t fake_msg_pool[4096];

void *btctl_mem_alloc(unsigned size)
{
	if (size > sizeof(fake_msg_pool))
		die("test bug: message larger than the fake pool\n");
	return fake_msg_pool;
}

void btctl_mem_free(void *p) { (void)p; }

uint8_t btphy_whiten_seed(uint32_t clk) { (void)clk; return 0; }

hop_state_t hop_state;
uint8_t hop_channel(uint32_t clkn) { (void)clkn; return 0; }

int tdma_end_set(uint8_t p1, uint8_t p2, uint16_t p3)
{ (void)p1; (void)p2; (void)p3; return 0; }

int tdma_schedule(uint8_t frame_offset, tdma_sched_cb *cb, uint8_t p1,
                  uint8_t p2, uint16_t p3, int16_t prio)
{ (void)frame_offset; (void)cb; (void)p1; (void)p2; (void)p3; (void)prio; return 0; }

int tdma_schedule_set(uint8_t frame_offset, const struct tdma_sched_item *item_set,
                      uint16_t p3)
{ (void)frame_offset; (void)item_set; (void)p3; return 0; }

/* The codec: a decode reports a single-slot packet with a payload and nothing
 * else. The deadline paths all return before reaching it. */
static bbcodec_types_t fake_codec_type = {
	1 /* nslots */, 1 /* payload_header_bytes */, 17 /* payload_bytes */,
	0 /* has_fec23 */, 1 /* has_crc */
};

static unsigned codec_chunk_calls;

int bbcodec_decode_header(bbcodec_t *codec, bbhdr_t *out_hdr, uint8_t *in_data)
{
	(void)out_hdr;
	(void)in_data;
	codec->t = &fake_codec_type;
	codec->air_bytes = 30;
	return 0;
}

int bbcodec_decode_chunk(bbcodec_t *codec, uint8_t *out, bbhdr_t *in_hdr,
                         uint8_t *in_data, unsigned in_length)
{
	(void)codec; (void)out; (void)in_hdr; (void)in_data; (void)in_length;
	codec_chunk_calls++;
	/* 0 is "decoded that, there is more to come", which is what the first
	 * chunk of a payload gets. */
	return 0;
}

uint8_t bbcodec_decode_finalize(bbcodec_t *codec, uint8_t *out,
                                uint16_t *out_size)
{
	(void)codec;
	(void)out;
	*out_size = 0;
	return 0;
}

#include "../btbr/src/btphy/rx_task.c"

/* ---- the deadline ---- */

/*
 * A receive whose bytes turn up as the air delivers them is unaffected: the
 * wait returns the bytes it was asked for.
 */
MU_TEST(test_a_normal_header_wait_gets_its_bytes)
{
	fake_receive(RF_EXPECTED_RX_CLKN_OFFSET, sizeof(rx_task.rx_dma_buf));
	rx_task.rx_offset = 0;
	rx_task.pkt_time = RF_EXPECTED_RX_CLKN_OFFSET;

	mu_assert_int_eq(NUM_PKT_HDR_BYTES,
	                 (int)rx_wait_bytes(NUM_PKT_HDR_BYTES));
}

/* The longer of the two waits, for the first chunk of payload, likewise. */
MU_TEST(test_a_normal_data_header_wait_gets_its_bytes)
{
	fake_receive(RF_EXPECTED_RX_CLKN_OFFSET, sizeof(rx_task.rx_dma_buf));
	rx_task.rx_offset = 0;
	rx_task.pkt_time = RF_EXPECTED_RX_CLKN_OFFSET;

	mu_assert_int_eq(NUM_DATA_HDR_BYTES,
	                 (int)rx_wait_bytes(NUM_DATA_HDR_BYTES));
}

/*
 * The regression: the buffer stops advancing part-way through the header. The
 * wait has to end anyway, and report the short count, instead of spinning in
 * the timer interrupt until the board is re-plugged.
 */
MU_TEST(test_a_stalled_buffer_gives_up_at_the_deadline)
{
	fake_receive(RF_EXPECTED_RX_CLKN_OFFSET, RX_SYNC_WORD_BYTES + 2);
	rx_task.rx_offset = 0;
	rx_task.pkt_time = RF_EXPECTED_RX_CLKN_OFFSET;

	mu_assert_int_eq(2, (int)rx_wait_bytes(NUM_PKT_HDR_BYTES));
}

/* The same for the longer wait. */
MU_TEST(test_a_stalled_buffer_gives_up_on_the_data_header_too)
{
	fake_receive(RF_EXPECTED_RX_CLKN_OFFSET,
	             RX_SYNC_WORD_BYTES + NUM_PKT_HDR_BYTES);
	rx_task.rx_offset = 0;
	rx_task.pkt_time = RF_EXPECTED_RX_CLKN_OFFSET;

	mu_assert_int_eq(NUM_PKT_HDR_BYTES,
	                 (int)rx_wait_bytes(NUM_DATA_HDR_BYTES));
}

/* A buffer that never advances at all -- not even the sync word -- ends too. */
MU_TEST(test_a_buffer_that_never_advances_gives_up)
{
	fake_receive(RF_EXPECTED_RX_CLKN_OFFSET, 0);
	rx_task.rx_offset = 0;
	rx_task.pkt_time = RF_EXPECTED_RX_CLKN_OFFSET;

	mu_assert_int_eq(0, (int)rx_wait_bytes(NUM_PKT_HDR_BYTES));
}

/*
 * Where it gives up: when the bytes are due, plus the slack, and not before.
 * The deadline has to be late enough that a packet arriving at the air's own
 * rate is never cut off, which the first two tests check from the other side.
 */
MU_TEST(test_it_gives_up_when_the_bytes_are_overdue)
{
	uint32_t due = RF_EXPECTED_RX_CLKN_OFFSET
		+ (NUM_PKT_HDR_BYTES+RX_SYNC_WORD_BYTES+RX_WAIT_SLACK_BYTES)
		  *RX_CLKN_PER_AIR_BYTE;

	fake_receive(RF_EXPECTED_RX_CLKN_OFFSET, 0);
	rx_task.rx_offset = 0;
	rx_task.pkt_time = RF_EXPECTED_RX_CLKN_OFFSET;

	mu_assert_int_eq(0, (int)rx_wait_bytes(NUM_PKT_HDR_BYTES));
	/* One step past the deadline: the poll that read it was the last. */
	mu_assert_int_eq((int)due + 1, (int)fake_clock_peek());
}

/*
 * The point of using the slot clock: the wait lasts as long as the bytes are
 * late by, not as long as a number of polls takes. Run the same stalled receive
 * with the clock advancing ten times as fast per poll -- a faster CPU would
 * look the other way round, polling ten times per step -- and it still gives up
 * at the same clock reading, after a tenth of the polls.
 */
MU_TEST(test_the_deadline_is_the_slot_clock_not_a_poll_count)
{
	unsigned slow_polls, fast_polls;
	uint32_t slow_clock, fast_clock;

	fake_receive(RF_EXPECTED_RX_CLKN_OFFSET, 0);
	rx_task.rx_offset = 0;
	rx_task.pkt_time = RF_EXPECTED_RX_CLKN_OFFSET;
	rx_wait_bytes(NUM_PKT_HDR_BYTES);
	slow_polls = fake_clock_reads;
	slow_clock = fake_clock_peek();

	fake_receive(RF_EXPECTED_RX_CLKN_OFFSET, 0);
	fake_clock_step = 10;
	rx_task.rx_offset = 0;
	rx_task.pkt_time = RF_EXPECTED_RX_CLKN_OFFSET;
	rx_wait_bytes(NUM_PKT_HDR_BYTES);
	fast_polls = fake_clock_reads;
	fast_clock = fake_clock_peek();

	/* A tenth of the polls, give or take the one the rounding costs. */
	mu_assert(fast_polls * 10 >= slow_polls - 10 &&
	          fast_polls * 10 <= slow_polls + 10,
	          "the faster clock did not take about a tenth of the polls");
	/* The same deadline, reached within one step of the faster clock. */
	mu_assert(fast_clock >= slow_clock && fast_clock < slow_clock + 10,
	          "the two waits gave up at different clock readings");
}

/*
 * A sync word detected late pushes the deadline out by as much, because the
 * bytes after it are late by as much. The deadline is relative to the sync
 * word, not to the start of the slot.
 */
MU_TEST(test_a_late_sync_word_pushes_the_deadline_out)
{
	uint32_t early = RF_EXPECTED_RX_CLKN_OFFSET;
	uint32_t late = RF_EXPECTED_RX_CLKN_OFFSET + 50;
	uint32_t gave_up_early;

	fake_receive(early, 0);
	rx_task.rx_offset = 0;
	rx_task.pkt_time = early;
	rx_wait_bytes(NUM_PKT_HDR_BYTES);
	gave_up_early = fake_clock_peek();

	fake_receive(late, 0);
	rx_task.rx_offset = 0;
	rx_task.pkt_time = late;
	rx_wait_bytes(NUM_PKT_HDR_BYTES);

	mu_assert_int_eq((int)gave_up_early + 50, (int)fake_clock_peek());
}

/*
 * The ceiling has to leave room for a whole packet header to arrive at the air's
 * own rate, even when the sync word was detected as late as the task will accept
 * one. If it did not, the longest wait would be cut short of bytes that were
 * going to turn up, and a board would lose packets it used to decode. This is
 * what fails if one of the constants behind it grows -- the size of a codec
 * chunk, say, which sets how many bytes the data header wait asks for.
 *
 * The deadline is allowed to be clamped by the ceiling, and for the longest wait
 * it is: what matters is that the bytes are due before the clamp, not that the
 * slack survives it.
 */
MU_TEST(test_the_ceiling_leaves_room_for_the_longest_wait)
{
	unsigned latest_sync = WAIT_SYNCWORD;
	unsigned last_byte_due = latest_sync
		+ (NUM_DATA_HDR_BYTES+RX_SYNC_WORD_BYTES)*RX_CLKN_PER_AIR_BYTE;

	mu_assert(last_byte_due < RX_WAIT_LATEST,
	          "the longest wait can be cut off before its bytes are due");
	mu_assert(RX_WAIT_LATEST < CLKN_OFFSET_RESET_VAL,
	          "the deadline ceiling is not inside the tick");
}

/* The longest wait, with the sync word as late as it is ever accepted: the
 * bytes still arrive in time, clamp and all. */
MU_TEST(test_a_late_sync_word_still_gets_a_whole_data_header)
{
	fake_receive(WAIT_SYNCWORD, sizeof(rx_task.rx_dma_buf));
	rx_task.rx_offset = 0;
	rx_task.pkt_time = WAIT_SYNCWORD;

	mu_assert_int_eq(NUM_DATA_HDR_BYTES,
	                 (int)rx_wait_bytes(NUM_DATA_HDR_BYTES));
}

/* And the ceiling itself is honoured, for the case the two tests above rule
 * out: a sync word so late that the bytes would be due after the tick ends. */
MU_TEST(test_the_deadline_never_runs_past_the_ceiling)
{
	fake_receive(CLKN_OFFSET_RESET_VAL - 10, 0);
	rx_task.rx_offset = 0;
	rx_task.pkt_time = CLKN_OFFSET_RESET_VAL - 10;

	mu_assert_int_eq(0, (int)rx_wait_bytes(NUM_DATA_HDR_BYTES));
	/* It had already passed the ceiling, so it gave up on the first poll. */
	mu_assert_int_eq(1, (int)fake_clock_reads);
}

/*
 * The ceiling has to be far enough from the restart that a poll lands at or past
 * it. The loop ends on `CLKN_OFFSET < due` going false, so a clock that stepped
 * from below the ceiling straight past the restart would miss it and the wait
 * would run a whole tick longer. Polling in steps far coarser than any iteration
 * of the real loop takes still ends the wait inside the tick.
 */
MU_TEST(test_a_coarse_clock_still_sees_the_ceiling)
{
	fake_receive(CLKN_OFFSET_RESET_VAL - 200, 0);
	fake_clock_step = 60;
	rx_task.rx_offset = 0;
	rx_task.pkt_time = CLKN_OFFSET_RESET_VAL - 200;

	mu_assert_int_eq(0, (int)rx_wait_bytes(NUM_DATA_HDR_BYTES));
	mu_assert(fake_clock_peek() < CLKN_OFFSET_RESET_VAL,
	          "the wait ran past the end of the tick");
}

/* ---- what the callers report ---- */

/* Hand rx_pkt_hdr_callback() and rx_data_hdr_callback() a packet to fill in. */
static btctl_rx_pkt_t test_pkt;

static void start_packet(uint32_t sync_clock, unsigned bytes_available)
{
	fake_receive(sync_clock, bytes_available);
	memset(&test_pkt, 0, sizeof(test_pkt));
	memset(&rx_task.codec, 0, sizeof(rx_task.codec));
	rx_task.rx_pkt = &test_pkt;
	rx_task.rx_offset = 0;
	rx_task.rx_done = 0;
	rx_task.pkt_time = sync_clock;
	test_pkt.flags = 1<<BBPKT_F_HAS_PKT;
	codec_chunk_calls = 0;
}

/*
 * A header that never arrives: the caller says so, and the packet carries no
 * header flag -- the same report a header that fails to decode makes, so the
 * state machines above it need no new case.
 */
MU_TEST(test_an_overdue_header_reports_no_header)
{
	start_packet(RF_EXPECTED_RX_CLKN_OFFSET, RX_SYNC_WORD_BYTES + 1);

	mu_assert_int_eq(-1, rx_pkt_hdr_callback());
	mu_assert_int_eq(0, BBPKT_HAS_HDR(&test_pkt));
	/* The sync word did arrive, and that much is still true. */
	mu_assert_int_eq(1, BBPKT_HAS_PKT(&test_pkt));
}

/* A header that does arrive still decodes and still sets the flag. */
MU_TEST(test_a_header_that_arrives_is_decoded)
{
	start_packet(RF_EXPECTED_RX_CLKN_OFFSET, sizeof(rx_task.rx_dma_buf));

	mu_assert_int_eq(0, rx_pkt_hdr_callback());
	mu_assert_int_eq(1, BBPKT_HAS_HDR(&test_pkt));
}

/*
 * A payload whose first chunk never arrives. The header had decoded, so the
 * flag was set; it has to come back off, both to report the receive as having
 * produced nothing and to keep rx_finalize() away from
 * bbcodec_decode_finalize(), which stops the firmware when it is handed a
 * part-decoded payload.
 */
MU_TEST(test_an_overdue_payload_takes_the_header_flag_back)
{
	start_packet(RF_EXPECTED_RX_CLKN_OFFSET, sizeof(rx_task.rx_dma_buf));
	mu_assert_int_eq(0, rx_pkt_hdr_callback());
	mu_assert_int_eq(1, BBPKT_HAS_HDR(&test_pkt));

	/* The radio stops after the packet header. */
	fake_bytes_available = RX_SYNC_WORD_BYTES + NUM_PKT_HDR_BYTES;

	mu_assert_int_eq(-1, rx_data_hdr_callback());
	mu_assert_int_eq(0, BBPKT_HAS_HDR(&test_pkt));
	mu_assert_int_eq(0, BBPKT_HAS_CRC(&test_pkt));
	/* Nothing was decoded from a buffer too short to decode from. */
	mu_assert_int_eq(0, (int)codec_chunk_calls);
}

/* A payload whose first chunk does arrive is decoded as before. */
MU_TEST(test_a_payload_that_arrives_is_decoded)
{
	start_packet(RF_EXPECTED_RX_CLKN_OFFSET, sizeof(rx_task.rx_dma_buf));
	mu_assert_int_eq(0, rx_pkt_hdr_callback());

	mu_assert_int_eq(0, rx_data_hdr_callback());
	mu_assert_int_eq(1, BBPKT_HAS_HDR(&test_pkt));
	mu_assert_int_eq(1, (int)codec_chunk_calls);
}

/* ---- the receive task's source ---- */

/*
 * The shape the bug had, checked against the source so it cannot come back by
 * copy and paste: a wait on the DMA buffer with an empty body, which is a
 * `while (<reads rx_buf_update>);` on one line. A condition that tests the slot
 * clock is the fixed shape and is left alone -- that exact spelling, so a spin
 * cannot get past by merely naming the clock.
 *
 * Only that shape, written on one line. A reintroduced spin whose `;` or whose
 * read of the buffer is on the next line gets past this, as rx_decode()'s own
 * bounded wait is written. The point is to catch the pattern coming back by copy
 * and paste, not to prove it cannot.
 *
 * test_cc2400_wait.c scans the same file for spins on the radio's registers.
 * The DMA buffer is the other thing the receive task waits on, and that scan
 * does not see it.
 */
static int is_bare_buffer_spin(const char *line)
{
	const char *p = strstr(line, "while");
	size_t length;

	if (p == NULL || is_ident_char(p[5]))
		return 0;
	if (!contains_identifier(line, "rx_buf_update") &&
	    !contains_identifier(line, "dma_get_rx_offset"))
		return 0;
	if (strstr(line, "CLKN_OFFSET <") != NULL)
		return 0;
	/* An empty body is a ';' as the last thing on the line. */
	length = strlen(line);
	while (length > 0 && isspace((unsigned char)line[length - 1]))
		length--;
	return length > 0 && line[length - 1] == ';';
}

MU_TEST(test_the_receive_task_does_not_spin_on_the_dma_buffer)
{
	const char *path = FW_BTBR_DIR "/src/btphy/rx_task.c";
	FILE *source = fopen(path, "r");
	char line[512];
	int in_block = 0, lineno = 0, found = 0;

	mu_assert(source != NULL, path);
	while (fgets(line, sizeof(line), source) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		if (is_bare_buffer_spin(line)) {
			printf("  FAIL %s:%d spins on the DMA buffer: %s",
			       path, lineno, line);
			found++;
		}
	}
	fclose(source);

	mu_assert_int_eq(0, found);
}

/* ---- the wait on the radio's state machine ---- */

/*
 * The other thing a slot task waits on is the radio's state machine, and it is
 * bounded the same way: by a position in the slot, not by a number of tries.
 * A poll count cannot serve a slot task. These waits run inside
 * TIMER0_IRQHandler, which fires every 312.5usec and counts one slot per
 * interrupt, so the shared FSM_POLLS budget -- about 9ms -- held the handler
 * for around 28 slots and the piconet clock lost every one of those counts.
 *
 * btphy_rf_wait_fsm() is an inline in rf.h, which this file already includes,
 * so these call it against the same fake clock.
 */
static void a_wait_on(u16 state, uint32_t clock_now, uint32_t step)
{
	fake_clock = clock_now;
	fake_clock_step = step;
	fake_clock_reads = 0;
	fake_fsm_state = state;
	fake_fsm_reads = 0;
	fake_fsm_timeouts = 0;
	fake_live_site = WATCHDOG_SITE_NONE;
}

/* The state is there, so the wait ends on its first read and counts nothing. */
MU_TEST(test_the_state_machine_wait_returns_when_the_state_is_reached)
{
	a_wait_on(STATE_IDLE, 0, 1);

	mu_assert_int_eq(1, btphy_rf_wait_fsm(STATE_IDLE, 1500));
	mu_assert_int_eq(1, (int)fake_fsm_reads);
	mu_assert_int_eq(0, (int)fake_fsm_timeouts);
	/* and it left the breadcrumb as it found it */
	mu_assert_int_eq(WATCHDOG_SITE_NONE, fake_live_site);
}

/* The state never arrives: the slot clock is what ends the wait, and the
 * give-up is counted so ubertooth-util -b can show it. */
MU_TEST(test_the_state_machine_wait_gives_up_at_its_deadline)
{
	a_wait_on(STATE_STROBE_RX, 0, 10);

	mu_assert_int_eq(0, btphy_rf_wait_fsm(STATE_IDLE, 1500));
	mu_assert(fake_clock_peek() >= 1500,
	          "the wait ended before its deadline");
	/* within one step of the deadline, so it is the deadline that ended it
	 * and not some other limit */
	mu_assert(fake_clock_peek() <= 1500 + 10,
	          "something other than the deadline ended the wait");
	mu_assert_int_eq(1, (int)fake_fsm_timeouts);
	mu_assert_int_eq(WATCHDOG_SITE_NONE, fake_live_site);
}

/*
 * A task that gets here with the deadline already behind it still reads the
 * radio once. Otherwise a radio that is in the state would be reported as one
 * that never answered, and the give-up count -- which is about the radio, not
 * about the scheduling -- would climb on a board whose radio is fine.
 */
MU_TEST(test_the_state_machine_wait_reads_once_past_its_deadline)
{
	a_wait_on(STATE_IDLE, 3000, 10);
	mu_assert_int_eq(1, btphy_rf_wait_fsm(STATE_IDLE, 1500));
	mu_assert_int_eq(1, (int)fake_fsm_reads);
	mu_assert_int_eq(0, (int)fake_fsm_timeouts);

	a_wait_on(STATE_STROBE_RX, 3000, 10);
	mu_assert_int_eq(0, btphy_rf_wait_fsm(STATE_IDLE, 1500));
	mu_assert_int_eq(1, (int)fake_fsm_reads);
	mu_assert_int_eq(1, (int)fake_fsm_timeouts);
}

/*
 * No deadline runs past the end of the slot, however late a caller asks for:
 * the slot clock restarts there, so a deadline beyond it would never be
 * reached and the wait would be the unbounded one again.
 */
MU_TEST(test_the_state_machine_wait_cannot_run_past_the_slot)
{
	a_wait_on(STATE_STROBE_RX, 0, 10);

	mu_assert_int_eq(0, btphy_rf_wait_fsm(STATE_IDLE,
	                                      CLKN_OFFSET_RESET_VAL * 2));
	mu_assert(fake_clock_peek() >= RF_WAIT_LATEST,
	          "the wait ended before the end of the slot");
	mu_assert(fake_clock_peek() < CLKN_OFFSET_RESET_VAL,
	          "the wait ran past the slot restart, where its deadline\n"
	          "could never be reached");
}

/*
 * And the slot tasks are the callers that use it. The two waits outside the
 * slot machinery -- the radio being set up at boot, and the mode being torn
 * down -- keep the poll count, which is why this names the call rather than
 * scanning for the absence of the other one.
 */
MU_TEST(test_every_wait_in_a_slot_task_takes_its_deadline_from_the_slot)
{
	static const struct {
		const char *path;
		const char *call;
	} sites[] = {
		/* the retune, which every slot does */
		{ FW_BTBR_DIR "/src/btphy/rf.c",
		  "btphy_rf_wait_fsm(STATE_IDLE, RF_WAIT_LATEST)" },
		/* the scan, until this slot stops listening */
		{ FW_BTBR_DIR "/src/btphy/scan_task.c",
		  "btphy_rf_wait_fsm(STATE_STROBE_RX, SCAN_WAIT_SYNCWORD)" },
		/* the end of a transmit */
		{ FW_BTBR_DIR "/src/btphy/tx_task.c",
		  "btphy_rf_wait_fsm(STATE_STROBE_FS_ON, TX_MAX_WAIT)" },
	};
	size_t i;

	for (i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
		FILE *source = fopen(sites[i].path, "r");
		char line[512], why[512];
		int in_block = 0, found = 0;

		snprintf(why, sizeof(why), "cannot read %s", sites[i].path);
		mu_assert_fatal(source != NULL, why);
		while (fgets(line, sizeof(line), source) != NULL) {
			strip_comments(line, &in_block);
			if (strstr(line, sites[i].call) != NULL)
				found = 1;
		}
		fclose(source);

		snprintf(why, sizeof(why),
		         "%s: no `%s`, so this per-slot wait does not take its "
		         "deadline from the slot clock",
		         sites[i].path, sites[i].call);
		mu_assert(found, why);
	}

	/* No slot task may be left on the poll count. Those two files have no
	 * wait outside a slot task, so naming it at all is the mistake. */
	{
		static const char *const slot_only[] = {
			FW_BTBR_DIR "/src/btphy/scan_task.c",
			FW_BTBR_DIR "/src/btphy/tx_task.c",
		};
		size_t f;

		for (f = 0; f < sizeof(slot_only) / sizeof(slot_only[0]); f++) {
			FILE *source = fopen(slot_only[f], "r");
			char line[512], why[512];
			int in_block = 0, found = 0;

			snprintf(why, sizeof(why), "cannot read %s", slot_only[f]);
			mu_assert_fatal(source != NULL, why);
			while (fgets(line, sizeof(line), source) != NULL) {
				strip_comments(line, &in_block);
				if (calls_identifier(line, "cc2400_wait_fsm"))
					found = 1;
			}
			fclose(source);

			snprintf(why, sizeof(why),
			         "%s: calls cc2400_wait_fsm(), whose bound is a "
			         "poll count, from a slot task", slot_only[f]);
			mu_assert(!found, why);
		}
	}
}

int main(void)
{
	printf("BR/EDR receive deadline tests\n");
	MU_RUN(test_a_normal_header_wait_gets_its_bytes);
	MU_RUN(test_a_normal_data_header_wait_gets_its_bytes);
	MU_RUN(test_a_stalled_buffer_gives_up_at_the_deadline);
	MU_RUN(test_a_stalled_buffer_gives_up_on_the_data_header_too);
	MU_RUN(test_a_buffer_that_never_advances_gives_up);
	MU_RUN(test_it_gives_up_when_the_bytes_are_overdue);
	MU_RUN(test_the_deadline_is_the_slot_clock_not_a_poll_count);
	MU_RUN(test_a_late_sync_word_pushes_the_deadline_out);
	MU_RUN(test_the_ceiling_leaves_room_for_the_longest_wait);
	MU_RUN(test_a_late_sync_word_still_gets_a_whole_data_header);
	MU_RUN(test_the_deadline_never_runs_past_the_ceiling);
	MU_RUN(test_a_coarse_clock_still_sees_the_ceiling);
	MU_RUN(test_an_overdue_header_reports_no_header);
	MU_RUN(test_a_header_that_arrives_is_decoded);
	MU_RUN(test_an_overdue_payload_takes_the_header_flag_back);
	MU_RUN(test_a_payload_that_arrives_is_decoded);
	MU_RUN(test_the_receive_task_does_not_spin_on_the_dma_buffer);
	MU_RUN(test_the_state_machine_wait_returns_when_the_state_is_reached);
	MU_RUN(test_the_state_machine_wait_gives_up_at_its_deadline);
	MU_RUN(test_the_state_machine_wait_reads_once_past_its_deadline);
	MU_RUN(test_the_state_machine_wait_cannot_run_past_the_slot);
	MU_RUN(test_every_wait_in_a_slot_task_takes_its_deadline_from_the_slot);
	return mu_report();
}
