/*
 * Unit tests for what the BR/EDR image's transmit task does when the scheduler
 * cannot take the whole transmit: firmware/btbr/src/btphy/tx_task.c, and the
 * count firmware/btbr/src/btphy/tdma_sched.c returns so it can tell the two
 * cases apart.
 *
 * A transmit goes on the scheduler as two tasks a slot apart: tx_prepare()
 * tunes the radio and encodes the packet, and tx_execute() turns the
 * transmitter on. The scheduler takes four tasks per slot and refuses the
 * fifth, so either of those two slots can be full. Neither task placed is a
 * link that stops forever unless the transmit task reports the completion
 * itself; only tx_prepare() placed is a packet dropped in silence unless it is
 * said on the console. tx_task_schedule_fmt() has the reasoning.
 *
 * tx_task.c is included into this translation unit, which is how its slot
 * tasks can be reached at all: they are static. The scheduler underneath is
 * the firmware's own -- tdma_sched.c is included as well, so a full slot here
 * is a real full slot and the count the transmit task reads is the scheduler's
 * own. Everything else is faked: the radio, the slot clock, the hop sequence,
 * and the codec.
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
#include <ubtbr/codec.h>
#include <ubtbr/hop.h>
#include <ubtbr/rf.h>
#include <ubtbr/tdma_sched.h>

/* ---- the fake slot clock ---- */

/*
 * The slot clock counts 100ns steps, as TIMER0 does, and advances on every
 * read, so a wait that polls it makes progress.
 */
static uint32_t fake_clock;
uint32_t fake_clock_match = CLKN_OFFSET_RESET_VAL - 1;

uint32_t fake_clock_read(void)
{
	uint32_t now = fake_clock;

	fake_clock += 10;
	return now;
}

int fake_gio6(void)
{
	return 0;
}

/* ---- the rest of the transmit task's world ---- */

static char console[512];
static unsigned console_len;
static unsigned console_lines;

void cprintf(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	console_len += vsnprintf(console + console_len,
	                         sizeof(console) - console_len, fmt, args);
	va_end(args);
	console_lines++;
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
volatile rf_state_t rf_state;
hop_state_t hop_state;

/* The radio. The strobes are recorded so a test can see whether it was touched
 * at all, and whether the transmitter was ever turned on. */
static int fake_fs_locked;
static unsigned fake_stx_strobes;
static unsigned fake_sfson_strobes;
static unsigned fake_srfoff_strobes;

u8 cc2400_status(void)
{
	return fake_fs_locked ? FS_LOCK : 0;
}

u8 cc2400_strobe(u8 reg)
{
	switch (reg) {
	case STX:
		fake_stx_strobes++;
		break;
	case SFSON:
		fake_sfson_strobes++;
		break;
	case SRFOFF:
		fake_srfoff_strobes++;
		break;
	default:
		break;
	}
	return 0;
}

void cc2400_set(u8 reg, u16 val) { (void)reg; (void)val; }

/*
 * The radio's state machine. tx_finalize() waits for the synthesizer to be
 * back on, which is where a transmit that did go out ends; the fake is in that
 * state, so such a wait returns on its first read.
 */
static u16 fake_fsm_state = STATE_STROBE_FS_ON;

u16 cc2400_get(u8 reg)
{
	if (reg == FSMSTATE)
		return fake_fsm_state;
	return 0;
}

/* The give-up count the state machine wait writes: the one ubertooth-util -w
 * reports as a radio that never answered. */
static unsigned fake_fsm_timeouts;
/* How many waits were entered at all. */
static unsigned fake_waits_marked;

uint8_t watchdog_mark_wait(uint8_t site)
{
	(void)site;
	fake_waits_marked++;
	return WATCHDOG_SITE_NONE;
}

void watchdog_leave_wait(uint8_t site)
{
	(void)site;
}

void cc2400_wait_count_timeout(enum cc2400_wait_shape shape)
{
	if (shape == CC2400_WAIT_FSM)
		fake_fsm_timeouts++;
}

static unsigned fake_tunes;

void btphy_rf_tune_chan(uint16_t channel, int tx)
{
	(void)channel;
	(void)tx;
	fake_tunes++;
}

void btphy_rf_fifo_write(uint8_t *data, unsigned len)
{
	(void)data;
	(void)len;
}

static unsigned fake_int_enables;

void btphy_rf_enable_int(btbr_int_cb_t cb, void *cb_arg, int tx)
{
	(void)cb;
	(void)cb_arg;
	(void)tx;
	fake_int_enables++;
}

void btphy_rf_disable_int(void) { }

uint8_t btphy_whiten_seed(uint32_t clk) { (void)clk; return 0; }

uint8_t hop_channel(uint32_t clkn) { (void)clkn; return 0; }

/* The codec: a single-slot packet with a short payload, which is all the
 * transmit task reads out of it. */
static const bbcodec_types_t fake_codec_type = {
	1 /* nslots */, 1 /* payload_header_bytes */, 17 /* payload_bytes */,
	BB_FEC_NONE /* fec */, 1 /* has_crc */, 0 /* voice_bytes */
};

void bbcodec_encode_header(bbcodec_t *codec, uint8_t *air_data, bbhdr_t *in_hdr,
                           uint8_t trailer, uint8_t *in_data)
{
	(void)air_data;
	(void)in_hdr;
	(void)trailer;
	(void)in_data;
	if (codec->t == NULL)
		codec->t = &fake_codec_type;
	codec->air_bytes = 30;
}

void bbcodec_encode_chunk(bbcodec_t *codec, uint8_t *air_data, bbhdr_t *in_hdr,
                          uint8_t *in_data)
{
	(void)codec;
	(void)air_data;
	(void)in_hdr;
	(void)in_data;
}

#include "../btbr/src/btphy/tx_task.c"

/*
 * The scheduler underneath, so a full slot is a real full slot and the count
 * the transmit task reads is the scheduler's own. It marks a parameter it does
 * not read with `__unused`, which the ARM toolchain's headers supply and a
 * host's C library may not, so it is spelled out here -- after the host
 * headers above, which on some platforms use the same name for a structure
 * member. Included rather than linked for that reason alone.
 */
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#include "../btbr/src/btphy/tdma_sched.c"

/* ---- driving whole slots ---- */

/*
 * One TDMA frame: run what the scheduler holds for this frame, then move on to
 * the next, as btphy_handler() does on each timer interrupt. The slot clock
 * starts each frame at zero, where the hardware restarts it.
 */
static void a_slot(void)
{
	fake_clock = 0;
	tdma_sched_execute();
	tdma_sched_advance();
}

/* What the caller is told: how many completions came, the argument the last
 * one carried, and whether it said the packet went out. */
static unsigned completions;
static void *last_completion_arg;
static int last_completion_sent;

static void count_completion(int sent, void *arg)
{
	completions++;
	last_completion_arg = arg;
	last_completion_sent = sent;
}

/* A packet to transmit. The transmit task holds the caller's header and data
 * pointers until it reports the completion, so these stand in for whatever the
 * link layer was holding. */
static bbhdr_t a_header;
static uint8_t some_data[32];

/* Something harmless to fill a slot with, so a slot can be made full the way
 * the firmware fills one: through the scheduler. */
static unsigned fillers_run;

static int a_filler(uint8_t p1, uint8_t p2, uint16_t p3)
{
	(void)p1;
	(void)p2;
	(void)p3;
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

static void a_board(int mode)
{
	int i;

	memset(&btphy, 0, sizeof(btphy));
	/* An even clock tick: a Central transmits in the first slot of a pair,
	 * and tx_execute() stops the firmware if it finds otherwise. */
	btphy.mode = mode;
	btphy.central_clkn = 0;
	memset(&a_header, 0, sizeof(a_header));

	/* Start from an empty scheduler. tdma_sched_reset() leaves the slot it
	 * is standing in alone, so the slots are run out once to clear that
	 * one as well; the counters below are zeroed afterwards. */
	tx_task.cb = NULL;
	tx_task.cb_arg = NULL;
	tx_task_reset();
	tdma_sched_reset();
	fake_fs_locked = 1;
	fake_fsm_state = STATE_STROBE_FS_ON;
	for (i = 0; i < TDMASCHED_NUM_FRAMES; i++)
		a_slot();
	tx_task.cb = NULL;
	tx_task.cb_arg = NULL;
	tx_task_reset();

	fake_clock = 0;
	fake_stx_strobes = 0;
	fake_sfson_strobes = 0;
	fake_srfoff_strobes = 0;
	fake_tunes = 0;
	fake_int_enables = 0;
	fake_fsm_timeouts = 0;
	fake_waits_marked = 0;
	fillers_run = 0;
	console_len = 0;
	console_lines = 0;
	console[0] = '\0';
	completions = 0;
	last_completion_arg = NULL;
	last_completion_sent = -1;
}

/* A whole transmit: schedule it, then run enough frames for prepare, execute
 * and finalize to have had their turn. */
static void one_transmit(void)
{
	int i;

	tx_task_schedule(0, count_completion, &a_header, &a_header, some_data);
	for (i = 0; i < 4; i++)
		a_slot();
}

/*
 * tx_task_schedule(0, ...) wants tx_prepare() in the frame the scheduler is
 * standing in now and tx_execute() in the next one.
 */
static void a_full_slot_for_the_whole_transmit(void)
{
	fill_a_slot(0, TDMASCHED_NUM_CB);
}

static void a_full_slot_for_the_start_of_transmission(void)
{
	fill_a_slot(1, TDMASCHED_NUM_CB);
}

/* ---- the count the scheduler returns ---- */

/*
 * A set of two tasks in consecutive frames, laid out as the transmit task's
 * own set is, so the count can be read without a transmit's side effects.
 */
static const struct tdma_sched_item a_two_frame_set[] = {
	SCHED_ITEM(a_filler,	0, 0, 0),	SCHED_END_FRAME(),
	SCHED_ITEM(a_filler,	0, 0, 0),	SCHED_END_FRAME(),
	SCHED_END_SET()
};

/* Room in both frames: the whole set is placed, and both tasks run. */
MU_TEST(test_a_set_that_fits_reports_every_item)
{
	a_board(BT_MODE_CENTRAL);

	mu_assert_int_eq(2, tdma_schedule_set(0, a_two_frame_set, 0));

	a_slot();
	a_slot();
	mu_assert_int_eq(2, (int)fillers_run);
}

/*
 * The first frame is full. Nothing is placed -- not even the second item,
 * whose own frame has room: a set stops at the first frame that is full, so
 * what a caller is told is that none of its set will run.
 */
MU_TEST(test_a_set_whose_first_frame_is_full_reports_nothing_placed)
{
	a_board(BT_MODE_CENTRAL);
	fill_a_slot(0, TDMASCHED_NUM_CB);

	mu_assert_int_eq(0, tdma_schedule_set(0, a_two_frame_set, 0));

	a_slot();
	a_slot();
	/* The four that filled the frame, and nothing of the set. */
	mu_assert_int_eq(TDMASCHED_NUM_CB, (int)fillers_run);
}

/*
 * The second frame is full. The first item is placed and runs; the second is
 * not. This is the count that tells a caller its set was cut in half rather
 * than refused outright.
 */
MU_TEST(test_a_set_whose_second_frame_is_full_reports_the_first_item)
{
	a_board(BT_MODE_CENTRAL);
	fill_a_slot(1, TDMASCHED_NUM_CB);

	mu_assert_int_eq(1, tdma_schedule_set(0, a_two_frame_set, 0));

	a_slot();
	a_slot();
	/* The set's first item, and the four that filled the second frame. */
	mu_assert_int_eq(TDMASCHED_NUM_CB + 1, (int)fillers_run);
}

/* ---- a transmit the scheduler refuses outright ---- */

/*
 * The regression. Not one task of the transmit was placed, so nothing is
 * coming that could report the completion -- and the caller still has to be
 * told, because its callback is what schedules the link's next receive or
 * transmit. Without that the link stops forever.
 */
MU_TEST(test_a_transmit_that_was_refused_outright_reports_once)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_whole_transmit();

	one_transmit();

	/* Nothing went on the air. */
	mu_assert_int_eq(0, (int)fake_stx_strobes);
	/* And the caller was told, once. */
	mu_assert_int_eq(1, (int)completions);
	mu_assert(last_completion_arg == &a_header,
	          "the completion did not carry the caller's argument");
	/* And the completion says the packet was not sent, so a caller whose
	 * next step is only right for a packet the other end could have heard
	 * does not take it. */
	mu_assert_int_eq(0, last_completion_sent);
	/* The slot that was full still ran what was in it. */
	mu_assert_int_eq(TDMASCHED_NUM_CB, (int)fillers_run);
}

/*
 * The user sees why. The scheduler says the slot was full, as it always has,
 * and the transmit task says what that cost: the packet was not sent. Two
 * lines, and no more.
 */
MU_TEST(test_a_refused_transmit_says_so_on_the_console)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_whole_transmit();

	one_transmit();

	mu_assert(strstr(console, "overflow") != NULL, console);
	mu_assert(strstr(console, "cannot schedule transmit") != NULL, console);
	mu_assert(strstr(console, "not sent") != NULL, console);
	mu_assert_int_eq(2, (int)console_lines);
}

/*
 * The radio is left alone. tx_prepare() is what tunes it and strobes its
 * synthesizer on, and it was never scheduled, so there is nothing to stop and
 * nothing to put back.
 */
MU_TEST(test_a_refused_transmit_does_not_touch_the_radio)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_whole_transmit();

	one_transmit();

	mu_assert_int_eq(0, (int)fake_tunes);
	mu_assert_int_eq(0, (int)fake_sfson_strobes);
	mu_assert_int_eq(0, (int)fake_int_enables);
	/* And no wait for an end of transmission that cannot come. */
	mu_assert_int_eq(0, (int)fake_fsm_timeouts);
	mu_assert_int_eq(0, (int)fake_waits_marked);
}

/*
 * Nothing the caller handed over stays held, so the transmit task takes the
 * next packet rather than stopping the firmware with "already got pkt".
 */
MU_TEST(test_a_refused_transmit_gives_the_packet_back)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_whole_transmit();

	one_transmit();

	mu_assert(tx_task.user_hdr == NULL, "the header is still held");
	mu_assert(tx_task.user_data == NULL, "the data is still held");
	mu_assert(tx_task.user_fmt == NULL, "the format is still held");
	/* Only the radio's own interrupt counts this down, and this packet
	 * never armed it. */
	mu_assert_int_eq(0, (int)tx_task.data_size);

	/* So the next packet is accepted, and goes out: the slot that was full
	 * has been run and emptied by now. */
	one_transmit();
	mu_assert_int_eq(2, (int)completions);
	mu_assert_int_eq(1, (int)fake_stx_strobes);
}

/*
 * What the connection does with it: a caller whose completion callback
 * schedules its next transmit keeps going, instead of the link stopping here.
 * The next one is scheduled four frames on, where a Central's next transmit
 * slot is.
 */
static unsigned transmits_wanted;

static void schedule_another_transmit(int sent, void *arg)
{
	(void)sent; (void)arg;
	completions++;
	if (--transmits_wanted == 0)
		return;
	tx_task_schedule(4, schedule_another_transmit, NULL,
	                 &a_header, some_data);
}

MU_TEST(test_a_link_goes_on_after_a_refused_transmit)
{
	int i;

	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_whole_transmit();
	transmits_wanted = 2;

	tx_task_schedule(0, schedule_another_transmit, NULL,
	                 &a_header, some_data);
	for (i = 0; i < 10; i++)
		a_slot();

	mu_assert_int_eq(2, (int)completions);
	/* The first was refused and did not go out; the second did. */
	mu_assert_int_eq(1, (int)fake_stx_strobes);
}

/*
 * The refusal is reported from the caller's own call rather than from a slot,
 * so a caller that schedules its next transmit from the callback is called
 * back before the first call has returned, and a caller that asks for the same
 * slot again is refused again straight away: the slot clock has not moved, so
 * that slot is still full. Each attempt reports its own refusal exactly once
 * and the run ends when the caller stops asking.
 */
static unsigned attempts_wanted;

static void try_the_same_slot_again(int sent, void *arg)
{
	(void)sent; (void)arg;
	completions++;
	if (--attempts_wanted == 0)
		return;
	tx_task_schedule(0, try_the_same_slot_again, NULL,
	                 &a_header, some_data);
}

MU_TEST(test_a_caller_that_asks_for_the_same_full_slot_again)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_whole_transmit();
	attempts_wanted = 3;

	tx_task_schedule(0, try_the_same_slot_again, NULL,
	                 &a_header, some_data);

	/* Three attempts, each refused and each reported, and no slot has run
	 * yet. */
	mu_assert_int_eq(3, (int)completions);
	mu_assert_int_eq(2 * 3, (int)console_lines);
	mu_assert_int_eq(0, (int)fake_stx_strobes);
	/* And nothing is left held, so the link is free to try again later. */
	mu_assert(tx_task.user_hdr == NULL, "the header is still held");
}

/* ---- a transmit whose start of transmission cannot be scheduled ---- */

/*
 * Only tx_prepare() was placed. It runs and schedules tx_finalize(), which
 * reports the completion and releases the packet, so the link carries on; the
 * packet is lost because the transmitter is never turned on.
 */
MU_TEST(test_a_transmit_with_no_start_reports_once)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_start_of_transmission();

	one_transmit();

	mu_assert_int_eq(0, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)completions);
	mu_assert(last_completion_arg == &a_header,
	          "the completion did not carry the caller's argument");
	/* Reported as not sent: tx_prepare() ran, but the transmitter was
	 * never turned on. */
	mu_assert_int_eq(0, last_completion_sent);
	mu_assert_int_eq(TDMASCHED_NUM_CB, (int)fillers_run);
}

/* And the drop is said on the console, where every other dropped transmit in
 * this file says so. */
MU_TEST(test_a_transmit_with_no_start_says_so_on_the_console)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_start_of_transmission();

	one_transmit();

	mu_assert(strstr(console, "overflow") != NULL, console);
	mu_assert(strstr(console, "cannot schedule start of transmission")
	          != NULL, console);
	mu_assert(strstr(console, "not sent") != NULL, console);
	mu_assert_int_eq(2, (int)console_lines);
}

/*
 * The radio is stopped and the packet released, both by the tx_finalize()
 * tx_prepare() scheduled, so the next packet is accepted.
 */
MU_TEST(test_a_transmit_with_no_start_gives_the_packet_back)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_start_of_transmission();

	one_transmit();

	mu_assert(tx_task.user_hdr == NULL, "the header is still held");
	mu_assert(tx_task.user_data == NULL, "the data is still held");
	mu_assert(tx_task.user_fmt == NULL, "the format is still held");
	mu_assert_int_eq(0, (int)tx_task.data_size);
	mu_assert(fake_srfoff_strobes > 0, "the radio was left running");
	/* The packet was prepared and then not sent, so the flag that says a
	 * packet is ready for tx_execute() must not be left standing. */
	mu_assert_int_eq(0, (int)tx_task.prepared);

	one_transmit();
	mu_assert_int_eq(2, (int)completions);
	mu_assert_int_eq(1, (int)fake_stx_strobes);
}

/*
 * No end of transmission is waited for: the transmitter was never turned on,
 * so the state tx_finalize() waits for cannot come, and waiting would spend
 * the slot polling the radio from the timer interrupt.
 */
MU_TEST(test_a_transmit_with_no_start_does_not_wait_for_the_radio)
{
	a_board(BT_MODE_CENTRAL);
	/* A radio in no state the transmit task waits for, so any wait would
	 * run to its deadline. */
	fake_fsm_state = STATE_IDLE;
	a_full_slot_for_the_start_of_transmission();

	one_transmit();

	mu_assert_int_eq(0, (int)fake_fsm_timeouts);
	mu_assert_int_eq(0, (int)fake_waits_marked);
}

/* A link whose callback schedules the next transmit carries on from here too. */
MU_TEST(test_a_link_goes_on_after_a_transmit_with_no_start)
{
	int i;

	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_start_of_transmission();
	transmits_wanted = 2;

	tx_task_schedule(0, schedule_another_transmit, NULL,
	                 &a_header, some_data);
	for (i = 0; i < 10; i++)
		a_slot();

	mu_assert_int_eq(2, (int)completions);
	mu_assert_int_eq(1, (int)fake_stx_strobes);
}

int main(void)
{
	printf("BR/EDR transmit tests: no slot for the transmit itself\n");
	MU_RUN(test_a_set_that_fits_reports_every_item);
	MU_RUN(test_a_set_whose_first_frame_is_full_reports_nothing_placed);
	MU_RUN(test_a_set_whose_second_frame_is_full_reports_the_first_item);
	MU_RUN(test_a_transmit_that_was_refused_outright_reports_once);
	MU_RUN(test_a_refused_transmit_says_so_on_the_console);
	MU_RUN(test_a_refused_transmit_does_not_touch_the_radio);
	MU_RUN(test_a_refused_transmit_gives_the_packet_back);
	MU_RUN(test_a_link_goes_on_after_a_refused_transmit);
	MU_RUN(test_a_caller_that_asks_for_the_same_full_slot_again);
	MU_RUN(test_a_transmit_with_no_start_reports_once);
	MU_RUN(test_a_transmit_with_no_start_says_so_on_the_console);
	MU_RUN(test_a_transmit_with_no_start_gives_the_packet_back);
	MU_RUN(test_a_transmit_with_no_start_does_not_wait_for_the_radio);
	MU_RUN(test_a_link_goes_on_after_a_transmit_with_no_start);
	return mu_report();
}
