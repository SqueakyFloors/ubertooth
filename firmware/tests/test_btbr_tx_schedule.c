/*
 * Unit tests for what the BR/EDR image's transmit task does when the slot that
 * would have ended a transmission cannot take it:
 * firmware/btbr/src/btphy/tx_task.c.
 *
 * A transmit is three slot tasks. tx_prepare() tunes the radio, encodes the
 * packet and puts tx_finalize() on the scheduler; tx_execute() turns the
 * transmitter on a slot later; tx_finalize() waits for the air to go quiet,
 * stops the radio, releases the packet and reports the completion the caller
 * is waiting for. The caller's callback is what makes a connection go on to
 * its next slot, so a transmit that never reported one would be a link that
 * stops forever.
 *
 * The scheduler takes four tasks per slot and refuses the fifth. A page fills
 * three of the places in one slot on its own, so the slot tx_prepare() wants
 * for tx_finalize() can be full. These cases pin what happens then: the
 * packet is given up, said so on the console, and the completion is reported
 * from tx_prepare() -- exactly once, because the usual reporter is the task
 * that could not be scheduled.
 *
 * tx_task.c is included into this translation unit, which is how its slot
 * tasks can be reached at all: they are static. The scheduler underneath is
 * the firmware's own -- tdma_sched.c is included as well, so the slot a task
 * lands in and the order tasks run in are the board's and not this file's,
 * and a slot is filled here the way the firmware fills one. Everything else
 * is faked: the radio, the slot clock, the hop sequence, and the codec.
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

/* The radio. The strobes are recorded so a test can see whether the
 * transmitter was ever turned on and whether the radio was stopped. */
static int fake_fs_locked;
static unsigned fake_stx_strobes;
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
/* How many waits were entered at all. The watchdog's record of which wait is
 * running is restored when a wait returns, so after the fact it says nothing;
 * this says whether the firmware ever told the watchdog it was about to
 * wait. */
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

void btphy_rf_tune_chan(uint16_t channel, int tx) { (void)channel; (void)tx; }

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
 * The scheduler underneath, so the slot a task lands in and the order tasks
 * run in are the firmware's own. It marks a parameter it does not read with
 * `__unused`, which the ARM toolchain's headers supply and a host's C library
 * may not, so it is spelled out here -- after the host headers above, which on
 * some platforms use the same name for a structure member. Included rather
 * than linked for that reason alone.
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

/* What the caller is told. */
static unsigned completions;
static void *last_completion_arg;

static void count_completion(void *arg)
{
	completions++;
	last_completion_arg = arg;
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
	fake_srfoff_strobes = 0;
	fake_int_enables = 0;
	fake_fsm_timeouts = 0;
	fake_waits_marked = 0;
	fillers_run = 0;
	console_len = 0;
	console_lines = 0;
	console[0] = '\0';
	completions = 0;
	last_completion_arg = NULL;
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
 * Fill the slot the end of transmission would go in. A Central's transmit puts
 * tx_finalize() two frames after the one tx_prepare() runs in, and
 * tx_task_schedule(0, ...) runs tx_prepare() in the frame the scheduler is
 * standing in now.
 */
static void a_full_slot_for_the_end_of_transmission(void)
{
	fill_a_slot(2, TDMASCHED_NUM_CB);
}

/* ---- a transmit whose end can be scheduled ---- */

/* The ordinary path, for comparison: the packet goes on the air, the caller is
 * told once that the slot is over, and nothing is said on the console. */
MU_TEST(test_a_transmit_whose_end_fits_goes_out_and_reports_once)
{
	a_board(BT_MODE_CENTRAL);

	one_transmit();

	mu_assert_int_eq(1, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)completions);
	mu_assert(last_completion_arg == &a_header,
	          "the completion did not carry the caller's argument");
	mu_assert_int_eq(0, (int)console_lines);
}

/* ---- a transmit whose end cannot be scheduled ---- */

/*
 * The regression. The end of transmission has nowhere to go, so the packet is
 * given up -- and the caller still has to be told the slot is over, because
 * its callback is what schedules the connection's next receive or transmit.
 * Without that the link stops forever.
 */
MU_TEST(test_a_transmit_whose_end_cannot_be_scheduled_reports_once)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_end_of_transmission();

	one_transmit();

	/* Nothing went on the air. */
	mu_assert_int_eq(0, (int)fake_stx_strobes);
	/* And the caller was told, once. */
	mu_assert_int_eq(1, (int)completions);
	mu_assert(last_completion_arg == &a_header,
	          "the completion did not carry the caller's argument");
	/* The slot that was full still ran what was in it. */
	mu_assert_int_eq(TDMASCHED_NUM_CB, (int)fillers_run);
}

/*
 * The user sees why. The scheduler says the slot was full, as it always has,
 * and the transmit task says what that cost: the packet was not sent. Two
 * lines, and no more -- a refusal is reported once, not once per slot.
 */
MU_TEST(test_it_says_so_on_the_console)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_end_of_transmission();

	one_transmit();

	mu_assert(strstr(console, "overflow") != NULL, console);
	mu_assert(strstr(console, "not sent") != NULL, console);
	mu_assert_int_eq(2, (int)console_lines);
}

/* The radio is stopped, so the board is not left transmitting with nothing
 * coming to turn it off. */
MU_TEST(test_it_stops_the_radio)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_end_of_transmission();

	one_transmit();

	mu_assert(fake_srfoff_strobes > 0, "the radio was left running");
	mu_assert_int_eq(0, (int)fake_int_enables);
}

/*
 * Nothing the caller handed over stays held. The transmit task keeps the
 * caller's header and data pointers for the length of a transmit and the
 * caller may not reuse them until the completion; after a transmit that was
 * given up they are released, so the task takes the next packet rather than
 * stopping the firmware with "already got pkt".
 */
MU_TEST(test_it_gives_the_packet_back)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_end_of_transmission();

	one_transmit();

	mu_assert(tx_task.user_hdr == NULL, "the header is still held");
	mu_assert(tx_task.user_data == NULL, "the data is still held");
	mu_assert(tx_task.user_fmt == NULL, "the format is still held");
	/* Only the radio's own interrupt counts this down, and this packet
	 * never armed it, so a count left standing would tell the next
	 * transmit to push bytes from a pointer that has been cleared. */
	mu_assert_int_eq(0, (int)tx_task.data_size);

	/* So the next packet is accepted, and goes out: the slot that was full
	 * has been run and emptied by now. */
	one_transmit();
	mu_assert_int_eq(2, (int)completions);
	mu_assert_int_eq(1, (int)fake_stx_strobes);
}

/*
 * The transmit that was given up leaves a tx_execute() behind. tx_prepare()
 * and tx_execute() go on the scheduler together and there is no way to take
 * one back off, so the one left over has to turn nothing on: it would put the
 * transmitter on the air for a packet nothing is coming to end.
 */
MU_TEST(test_the_leftover_execute_sends_nothing)
{
	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_end_of_transmission();

	/* tx_prepare() in this slot, and the tx_execute() it leaves over in
	 * the next. */
	tx_task_schedule(0, count_completion, &a_header, &a_header, some_data);
	a_slot();
	mu_assert_int_eq(1, (int)completions);
	a_slot();

	mu_assert_int_eq(0, (int)fake_stx_strobes);
	mu_assert_int_eq(0, (int)fake_int_enables);
	/* And it said nothing of its own: the two lines were tx_prepare()'s
	 * and the scheduler's. */
	mu_assert_int_eq(2, (int)console_lines);
	mu_assert_int_eq(1, (int)completions);
}

/*
 * The radio is not polled for an end that cannot come. Waiting for it would
 * spend the slot polling from the timer interrupt and would count a radio that
 * never answered, which ubertooth-util -w reports.
 */
MU_TEST(test_it_does_not_wait_for_the_radio)
{
	a_board(BT_MODE_CENTRAL);
	/* A radio in no state the transmit task waits for, so any wait would
	 * run to its deadline. */
	fake_fsm_state = STATE_IDLE;
	a_full_slot_for_the_end_of_transmission();

	one_transmit();

	mu_assert_int_eq(0, (int)fake_fsm_timeouts);
	mu_assert_int_eq(0, (int)fake_waits_marked);
}

/*
 * What the connection does with it: a caller whose completion callback
 * schedules its next transmit keeps going, instead of the link stopping here.
 * The next one is scheduled four frames on, where a Central's next transmit
 * slot is.
 */
static unsigned transmits_wanted;

static void schedule_another_transmit(void *arg)
{
	(void)arg;
	completions++;
	if (--transmits_wanted == 0)
		return;
	tx_task_schedule(4, schedule_another_transmit, NULL,
	                 &a_header, some_data);
}

MU_TEST(test_a_link_goes_on_to_its_next_slot)
{
	int i;

	a_board(BT_MODE_CENTRAL);
	a_full_slot_for_the_end_of_transmission();
	transmits_wanted = 2;

	tx_task_schedule(0, schedule_another_transmit, NULL,
	                 &a_header, some_data);
	for (i = 0; i < 10; i++)
		a_slot();

	mu_assert_int_eq(2, (int)completions);
	/* The first had nowhere to end and did not go out; the second did. */
	mu_assert_int_eq(1, (int)fake_stx_strobes);
}

/*
 * The slot a page runs out of room in. A page ends its transmission in the
 * next frame rather than the one after, which is the frame that already
 * carries tx_execute() and the next tx_prepare(), so three of the four places
 * are taken before anything else is scheduled there. Two more tasks in that
 * frame are enough to fill it.
 */
MU_TEST(test_a_page_whose_end_cannot_be_scheduled_reports_once)
{
	a_board(BT_MODE_PAGING);

	/* The frame tx_execute() lands in, which is where a page's
	 * tx_finalize() goes too. tx_execute() takes the fourth place. */
	fill_a_slot(1, TDMASCHED_NUM_CB - 1);

	/* An ID packet, as a page sends: no header and no payload. */
	tx_task_schedule(0, count_completion, &a_header, NULL, NULL);
	a_slot();
	a_slot();

	mu_assert_int_eq(0, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)completions);
	mu_assert(strstr(console, "not sent") != NULL, console);
	mu_assert_int_eq(2, (int)console_lines);
}

int main(void)
{
	printf("BR/EDR transmit tests: no slot for the end of transmission\n");
	MU_RUN(test_a_transmit_whose_end_fits_goes_out_and_reports_once);
	MU_RUN(test_a_transmit_whose_end_cannot_be_scheduled_reports_once);
	MU_RUN(test_it_says_so_on_the_console);
	MU_RUN(test_it_stops_the_radio);
	MU_RUN(test_it_gives_the_packet_back);
	MU_RUN(test_the_leftover_execute_sends_nothing);
	MU_RUN(test_it_does_not_wait_for_the_radio);
	MU_RUN(test_a_link_goes_on_to_its_next_slot);
	MU_RUN(test_a_page_whose_end_cannot_be_scheduled_reports_once);
	return mu_report();
}
