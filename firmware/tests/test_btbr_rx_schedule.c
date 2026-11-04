/*
 * Unit tests for what the BR/EDR image's receive task does when the scheduler
 * cannot take a slot it needs: firmware/btbr/src/btphy/rx_task.c.
 *
 * Every receive in this image goes through one task. It puts two slot tasks on
 * the scheduler, one to tune and one to listen, and the listening one is the
 * only thing that ever reaches the caller's packet callback. A multi-slot
 * packet adds one more task per slot of decoding, and the last of those is
 * what gives the packet buffer back.
 *
 * The scheduler takes four tasks per slot and refuses the fifth, so any of
 * those asks can be refused. A caller that was not told would sit with no task
 * left to run -- every state machine asks for its next slot from the callback
 * -- and a decode that was never continued is worse again: the packet buffer
 * stays held, and the next receive the state machine asks for stops the
 * firmware with "rx_schedule: rx_msg!=0".
 *
 * rx_task.c is included into this translation unit, which is how its slot
 * tasks can be reached at all: they are static. The scheduler underneath is
 * the firmware's own -- tdma_sched.c is included as well -- so a full slot
 * here is a real full slot and a refusal is the scheduler's own. The radio,
 * the slot clock, the DMA controller, the codec, the message pool and the hop
 * sequence are faked.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"

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
#include <ubtbr/rx_task.h>
#include <ubtbr/tdma_sched.h>
#include <ubtbr/ubertooth_dma.h>

/* ---- the fake slot clock and DMA controller ---- */

/*
 * The clock counts 100ns steps, as TIMER0 does, and advances on every read, so
 * a wait that polls it makes progress. The DMA controller hands out the bytes
 * the air would have delivered by the current reading: nothing until the
 * packet strobe went low, then one byte every 8usec. These tests always make
 * the whole packet available, because what is under test is the scheduling and
 * not the deadline -- test_btbr_rx_deadline.c covers a radio that stops
 * part-way.
 */
static uint32_t fake_clock;
uint32_t fake_clock_match = CLKN_OFFSET_RESET_VAL - 1;
static uint32_t fake_sync_clock;
static unsigned fake_bytes_available;

uint32_t fake_clock_read(void)
{
	uint32_t now = fake_clock;

	fake_clock += 10;
	return now;
}

/* The CC2400's packet strobe, active low: 0 is a sync word detected. Whether a
 * slot hears a packet decides which of the task's paths runs, so each test
 * says. */
static int fake_sync_word;

int fake_gio6(void)
{
	return fake_sync_word ? 0 : 1;
}

unsigned long dma_get_rx_offset(void)
{
	unsigned bytes;

	if (fake_clock < fake_sync_clock)
		return 0;
	bytes = (fake_clock - fake_sync_clock) / 80;
	if (bytes > fake_bytes_available)
		bytes = fake_bytes_available;
	return bytes;
}

/* ---- the rest of the receive task's world ---- */

static char console[1024];
static unsigned console_len;

void cprintf(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	console_len += vsnprintf(console + console_len,
	                         sizeof(console) - console_len, fmt, args);
	va_end(args);
	/* vsnprintf() returns the length it would have written, so a line that
	 * filled the buffer would otherwise leave console_len past the end and
	 * the next call's remaining size negative. */
	if (console_len > sizeof(console) - 1)
		console_len = sizeof(console) - 1;
}

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
hop_state_t hop_state;

/* The radio. The strobes and tunes are counted so a test can see whether it
 * was touched at all. */
static unsigned fake_tunes;
static unsigned fake_srfoff_strobes;

u8 cc2400_status(void) { return 0; }

u8 cc2400_strobe(u8 reg)
{
	if (reg == SRFOFF)
		fake_srfoff_strobes++;
	return 0;
}

void cc2400_set(u8 reg, u16 val) { (void)reg; (void)val; }

/* The receive task checks that the radio is receiving, which it is. */
static u16 fake_fsm_state = STATE_STROBE_RX;

u16 cc2400_get(u8 reg)
{
	if (reg == FSMSTATE)
		return fake_fsm_state;
	return 0;
}

uint8_t watchdog_mark_wait(uint8_t site) { (void)site; return WATCHDOG_SITE_NONE; }
void watchdog_leave_wait(uint8_t site) { (void)site; }
void cc2400_wait_count_timeout(enum cc2400_wait_shape shape) { (void)shape; }

void btphy_rf_tune_chan(uint16_t channel, int tx)
{
	(void)channel;
	(void)tx;
	fake_tunes++;
}

void btphy_rf_fifo_write(uint8_t *data, unsigned len) { (void)data; (void)len; }
void btphy_rf_enable_int(btbr_int_cb_t cb, void *cb_arg, int tx)
{ (void)cb; (void)cb_arg; (void)tx; }
void btphy_rf_disable_int(void) { }

void dio_ssp_start_rx(void) { }
void dio_ssp_stop(void) { }
void dma_init_rx_single(volatile void *buf, unsigned buf_size)
{ (void)buf; (void)buf_size; }

/* The message pool, one message at a time, with the handing out and the giving
 * back counted: a receive that is lost without rx_finalize() is one that never
 * gives its message back. */
static uint8_t fake_msg_pool[4096];
static unsigned msgs_taken;
static unsigned msgs_freed;

void *btctl_mem_alloc(unsigned size)
{
	if (size > sizeof(fake_msg_pool))
		die("test bug: message larger than the fake pool\n");
	msgs_taken++;
	return fake_msg_pool;
}

void btctl_mem_free(void *p)
{
	(void)p;
	msgs_freed++;
}

uint8_t btphy_whiten_seed(uint32_t clk) { (void)clk; return 0; }
uint8_t hop_channel(uint32_t clkn) { (void)clkn; return 0; }
void btphy_adj_clkn_delay(int delay) { (void)delay; }

/* The codec: a two-slot packet with a payload, so a decode needs a slot of its
 * own after the one that listened. A chunk decode always says there is more to
 * come, which is what puts the next decode on the scheduler. */
static bbcodec_types_t fake_codec_type = {
	2 /* nslots */, 1 /* payload_header_bytes */, 100 /* payload_bytes */,
	BB_FEC_NONE /* fec */, 1 /* has_crc */, 0 /* voice_bytes */,
	BB_LEN_HEADER /* length_kind */
};

static unsigned codec_chunk_calls;
static unsigned codec_finalize_calls;
/* How many chunk decodes a packet takes. The first is the one the listening
 * slot does; each of the rest needs a slot of its own, which is what makes
 * this a multi-slot decode. */
static unsigned codec_chunks_to_done = 4;

int bbcodec_decode_header(bbcodec_t *codec, bbhdr_t *out_hdr, uint8_t *in_data)
{
	(void)out_hdr;
	(void)in_data;
	codec->t = &fake_codec_type;
	codec->air_bytes = 400;
	return 0;
}

int bbcodec_decode_chunk(bbcodec_t *codec, uint8_t *out, bbhdr_t *in_hdr,
                         uint8_t *in_data, unsigned in_length)
{
	(void)codec; (void)out; (void)in_hdr; (void)in_data; (void)in_length;
	codec_chunk_calls++;
	/* 0 is "decoded that, there is more in the buffer", which is what the
	 * first chunk of a payload gets. */
	if (codec_chunk_calls == 1)
		return 0;
	/* BBCODEC_SHORT is "the rest of the bytes are not here yet", which is
	 * what puts the next decode on the scheduler. */
	if (codec_chunk_calls < codec_chunks_to_done)
		return BBCODEC_SHORT;
	return BBCODEC_DONE;
}

uint8_t bbcodec_decode_finalize(bbcodec_t *codec, uint8_t *out,
                                uint16_t *out_size)
{
	(void)codec;
	(void)out;
	codec_finalize_calls++;
	*out_size = 0;
	return 0;
}

#include "../btbr/src/btphy/rx_task.c"

/*
 * The scheduler underneath, so a full slot is a real full slot. It marks a
 * parameter it does not read with `__unused`, which the ARM toolchain's
 * headers supply and a host's C library may not, so it is spelled out here --
 * after the host headers above, which on some platforms use the same name for
 * a structure member. Included rather than linked for that reason alone.
 */
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#include "../btbr/src/btphy/tdma_sched.c"

/* ---- driving whole slots ---- */

/*
 * One TDMA frame: run what the scheduler holds for this frame, then move on to
 * the next, as btphy_handler() does on each timer interrupt. The slot clock
 * starts each frame where the packet strobe is read from, so a slot that hears
 * a packet has its bytes arriving from there.
 */
static void a_slot(void)
{
	fake_clock = 0;
	fake_sync_clock = 0;
	tdma_sched_execute();
	tdma_sched_advance();
}

static void some_slots(int slots)
{
	int i;

	for (i = 0; i < slots; i++)
		a_slot();
}

/* What the caller is told. */
static unsigned reports;
static unsigned report_data_size;
static int report_had_header;
static void *last_report_arg;

static int count_report(msg_t *msg, void *arg, int time_offset)
{
	btctl_hdr_t *h = (btctl_hdr_t *)msg->data;
	btctl_rx_pkt_t *pkt = (btctl_rx_pkt_t *)h->data;

	(void)time_offset;
	reports++;
	last_report_arg = arg;
	report_had_header = BBPKT_HAS_HDR(pkt) ? 1 : 0;
	report_data_size = pkt->data_size;
	msg_free(msg);
	return 0;
}

/* Something harmless to fill a slot with, so a slot can be made full the way
 * the firmware fills one: through the scheduler. */
static unsigned fillers_run;

static int a_filler(uint8_t p1, uint8_t p2, uint16_t p3)
{
	(void)p1; (void)p2; (void)p3;
	fillers_run++;
	return 0;
}

/* Put `items` tasks in the slot `frame_offset` frames ahead of the one the
 * scheduler is standing in. Every one is accepted: a slot holds
 * TDMASCHED_NUM_CB of them. */
static void fill_a_slot(uint8_t frame_offset, int items)
{
	int i;

	for (i = 0; i < items; i++) {
		if (tdma_schedule(frame_offset, a_filler, 0, 0, 0, 10) != 0)
			die("fill_a_slot: the slot was already full\n");
	}
}

static void a_board(void)
{
	int i;

	memset(&btphy, 0, sizeof(btphy));
	memset(&rx_task, 0, sizeof(rx_task));

	/* Start from an empty scheduler. tdma_sched_reset() leaves the slot it
	 * is standing in alone, so the slots are run out once to clear that
	 * one as well; the counters below are zeroed afterwards. */
	tdma_sched_reset();
	for (i = 0; i < TDMASCHED_NUM_FRAMES; i++)
		a_slot();

	fake_clock = 0;
	fake_sync_clock = 0;
	fake_sync_word = 0;
	fake_bytes_available = sizeof(rx_task.rx_dma_buf);
	fake_fsm_state = STATE_STROBE_RX;
	fake_tunes = 0;
	fake_srfoff_strobes = 0;
	fillers_run = 0;
	msgs_taken = 0;
	msgs_freed = 0;
	codec_chunk_calls = 0;
	codec_finalize_calls = 0;
	codec_chunks_to_done = 4;
	console[0] = '\0';
	console_len = 0;
	reports = 0;
	report_data_size = 0;
	report_had_header = -1;
	last_report_arg = NULL;
}

/* ---- an ordinary receive ---- */

/* A slot that is open and hears nothing: the caller is told once, and the
 * packet buffer comes back. */
MU_TEST(an_accepted_receive_listens_and_reports_once)
{
	int arg = 0;

	a_board();

	mu_assert_int_eq(0, rx_task_schedule(1, count_report, &arg, 0));
	some_slots(3);

	mu_assert_int_eq(1, reports);
	mu_assert(last_report_arg == &arg, "the callback got another caller's argument");
	mu_assert_int_eq(1, (int)fake_tunes);
	mu_assert_int_eq(1, (int)msgs_taken);
	mu_assert_int_eq(1, (int)msgs_freed);
	mu_assert(console[0] == '\0', "the console said something about a receive that was fine");
}

/* A multi-slot packet still decodes across slots while the scheduler has room
 * for each decode: that is the path the fix must leave alone. */
MU_TEST(a_multislot_decode_still_runs_across_slots)
{
	a_board();
	fake_sync_word = 1;

	mu_assert_int_eq(0, rx_task_schedule(1, count_report,
	                                     NULL, 1<<RX_F_PAYLOAD));
	some_slots(2 + 2 * TDMASCHED_NUM_FRAMES);

	mu_assert_int_eq(1, reports);
	mu_assert_int_eq(1, report_had_header);
	/* Several slots of decoding, not one. */
	mu_assert(codec_chunk_calls > 1, "the decode did not continue past its first slot");
	mu_assert_int_eq(1, (int)codec_finalize_calls);
	mu_assert_int_eq(1, (int)msgs_freed);
	mu_assert(console[0] == '\0', "the console said something about a decode that was fine");
}

/* ---- a receive the scheduler refuses ---- */

/* Both frames a receive needs are full, so no part of it is placed. The caller
 * is told so it can act. */
MU_TEST(a_refused_receive_is_reported_to_the_caller)
{
	a_board();
	fill_a_slot(1, TDMASCHED_NUM_CB);
	fill_a_slot(2, TDMASCHED_NUM_CB);

	mu_assert_int_eq(-1, rx_task_schedule(1, count_report, NULL, 0));
	mu_assert(strstr(console, "cannot schedule receive") != NULL,
		"the refused receive was not said on the console");
}

/* ... and nothing is left believing a slot is open: no task of the receive ever
 * runs, so nothing reaches the callback and the radio is not touched. */
MU_TEST(a_refused_receive_leaves_nothing_armed)
{
	a_board();
	fill_a_slot(1, TDMASCHED_NUM_CB);
	fill_a_slot(2, TDMASCHED_NUM_CB);

	mu_assert_int_eq(-1, rx_task_schedule(1, count_report, NULL, 0));
	some_slots(2 * TDMASCHED_NUM_FRAMES);

	mu_assert_int_eq(0, reports);
	mu_assert_int_eq(0, (int)fake_tunes);
	/* The eight tasks that filled the two slots did run, so they were full
	 * of real work rather than merely marked full. */
	mu_assert_int_eq(2 * TDMASCHED_NUM_CB, (int)fillers_run);
}

/*
 * The second frame alone is full, so the scheduler places the task that tunes
 * and refuses the one that listens. That is the case the count is needed for:
 * a task cannot be taken back off, so the tuning task is told to do nothing.
 * Tuning the radio and taking a packet buffer with nothing left to give them
 * back is what the whole report is there to avoid.
 */
MU_TEST(a_half_placed_receive_is_refused_and_prepares_nothing)
{
	a_board();
	fill_a_slot(2, TDMASCHED_NUM_CB);

	mu_assert_int_eq(-1, rx_task_schedule(1, count_report, NULL, 0));
	some_slots(2 * TDMASCHED_NUM_FRAMES);

	mu_assert_int_eq(0, reports);
	mu_assert_int_eq(0, (int)fake_tunes);
	mu_assert_int_eq(0, (int)msgs_taken);
	mu_assert(strstr(console, "cannot schedule receive") != NULL,
		"the half-placed receive was not said on the console");
}

/*
 * A refused receive holds no packet buffer, so the next one the caller asks
 * for is taken. Without that, the sanity check at the top of
 * rx_task_schedule() would stop the firmware with "rx_schedule: rx_msg!=0".
 */
MU_TEST(a_refused_receive_holds_nothing_so_the_next_one_is_taken)
{
	a_board();
	fill_a_slot(1, TDMASCHED_NUM_CB);
	fill_a_slot(2, TDMASCHED_NUM_CB);

	mu_assert_int_eq(-1, rx_task_schedule(1, count_report, NULL, 0));
	/* Run the full slots out, which empties them. */
	some_slots(3);
	mu_assert_int_eq(2 * TDMASCHED_NUM_CB, (int)fillers_run);

	mu_assert_int_eq(0, rx_task_schedule(1, count_report, NULL, 0));
	some_slots(3);
	mu_assert_int_eq(1, reports);
}

/* ---- a decode that cannot be continued ---- */

/*
 * The costly one. The slot the rest of the decode would go in is full, so the
 * receive ends where it stands instead of being lost in silence: the caller is
 * told, the packet buffer comes back, and the header flag is off so the report
 * reads as a receive that produced nothing.
 */
MU_TEST(a_decode_that_cannot_continue_ends_the_receive)
{
	a_board();
	fake_sync_word = 1;
	/* Frame 1 tunes, frame 2 listens, and the decode asks for frame 3. */
	fill_a_slot(3, TDMASCHED_NUM_CB);

	mu_assert_int_eq(0, rx_task_schedule(1, count_report,
	                                     NULL, 1<<RX_F_PAYLOAD));
	some_slots(2 * TDMASCHED_NUM_FRAMES);

	mu_assert_int_eq(1, reports);
	mu_assert_int_eq(0, report_had_header);
	mu_assert_int_eq(0, (int)report_data_size);
	mu_assert_int_eq(1, (int)msgs_freed);
	mu_assert(strstr(console, "cannot schedule rest of decode") != NULL,
		"the abandoned decode was not said on the console");
	/* The radio was stopped, as it is at the end of any receive. */
	mu_assert_int_eq(1, (int)fake_srfoff_strobes);
}

/*
 * And it does not hand a part-decoded payload to the codec's finalize, which
 * stops the firmware. Taking the header flag back off is what keeps
 * rx_finalize() out of there, so this is the reason that line exists.
 */
MU_TEST(an_abandoned_decode_does_not_finalize_a_part_decoded_payload)
{
	a_board();
	fake_sync_word = 1;
	fill_a_slot(3, TDMASCHED_NUM_CB);

	mu_assert_int_eq(0, rx_task_schedule(1, count_report,
	                                     NULL, 1<<RX_F_PAYLOAD));
	some_slots(2 * TDMASCHED_NUM_FRAMES);

	mu_assert_int_eq(0, (int)codec_finalize_calls);
}

/*
 * A decode that was refused once leaves the task ready for the next receive:
 * the packet buffer was given back, so the caller's next slot is taken rather
 * than stopping the firmware.
 */
MU_TEST(a_receive_after_an_abandoned_decode_is_taken)
{
	a_board();
	fake_sync_word = 1;
	fill_a_slot(3, TDMASCHED_NUM_CB);

	mu_assert_int_eq(0, rx_task_schedule(1, count_report,
	                                     NULL, 1<<RX_F_PAYLOAD));
	some_slots(2 * TDMASCHED_NUM_FRAMES);
	mu_assert_int_eq(1, reports);

	fake_sync_word = 0;
	mu_assert_int_eq(0, rx_task_schedule(1, count_report, NULL, 0));
	some_slots(3);
	mu_assert_int_eq(2, reports);
}

int main(void)
{
	printf("BR/EDR receive tests: no slot for the next receive or decode\n");
	MU_RUN(an_accepted_receive_listens_and_reports_once);
	MU_RUN(a_multislot_decode_still_runs_across_slots);
	MU_RUN(a_refused_receive_is_reported_to_the_caller);
	MU_RUN(a_refused_receive_leaves_nothing_armed);
	MU_RUN(a_half_placed_receive_is_refused_and_prepares_nothing);
	MU_RUN(a_refused_receive_holds_nothing_so_the_next_one_is_taken);
	MU_RUN(a_decode_that_cannot_continue_ends_the_receive);
	MU_RUN(an_abandoned_decode_does_not_finalize_a_part_decoded_payload);
	MU_RUN(a_receive_after_an_abandoned_decode_is_taken);
	return mu_report();
}
