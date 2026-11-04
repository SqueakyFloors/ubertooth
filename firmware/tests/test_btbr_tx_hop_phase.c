/*
 * Unit tests for the hop phase a BR/EDR transmit moves:
 * firmware/btbr/src/btphy/tx_task.c.
 *
 * Two substates sweep a phase of their own. An inquirer moves it once per
 * transmit, and a pager moves its page-response phase before each FHS it
 * sends. In the page response the paged device moves its own phase once for
 * every window it opens, so the pair only meet while both move at the same
 * rate: a phase left standing for one transmit stays one behind for the rest
 * of the attempt, and every transmit after it goes out on the frequency the
 * device's previous window used. An inquirer's phase is one of 32 guesses it
 * sweeps rather than a counter anyone shares, so nothing there has to be kept
 * level with it; what it does share with the page response is the rate, one
 * step per transmit.
 *
 * So the phase belongs to the attempt rather than to the packet, and it has to
 * move once per transmit whichever way that transmit ends -- on the air, or
 * dropped because the scheduler was full or the radio was not ready. These
 * tests pin that for each of the four, and pin that the substates which answer
 * a scan, and a connection, are left alone: those transmit on a phase they
 * were given rather than on one they sweep.
 *
 * tx_task.c is included into this translation unit, which is how its slot
 * tasks can be reached at all: they are static. The scheduler underneath is
 * the firmware's own -- tdma_sched.c is included as well, so a full slot here
 * is a real full slot. Everything else is faked: the radio, the slot clock,
 * and the codec. The hop sequence is the real hop_increment(), because the
 * phase counter it moves is what these tests read.
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
volatile rf_state_t rf_state;
hop_state_t hop_state;

/* The radio. Whether the synthesizer has locked is a knob, because a transmit
 * the radio was not ready for is one of the ways a transmit ends. */
static int fake_fs_locked;
static unsigned fake_stx_strobes;

u8 cc2400_status(void)
{
	return fake_fs_locked ? FS_LOCK : 0;
}

u8 cc2400_strobe(u8 reg)
{
	if (reg == STX)
		fake_stx_strobes++;
	return 0;
}

void cc2400_set(u8 reg, u16 val) { (void)reg; (void)val; }

/*
 * The radio's state machine, in the state tx_finalize() waits for, so such a
 * wait returns on its first read.
 */
u16 cc2400_get(u8 reg)
{
	if (reg == FSMSTATE)
		return STATE_STROBE_FS_ON;
	return 0;
}

uint8_t watchdog_mark_wait(uint8_t site)
{
	(void)site;
	return WATCHDOG_SITE_NONE;
}

void watchdog_leave_wait(uint8_t site) { (void)site; }

void cc2400_wait_count_timeout(enum cc2400_wait_shape shape) { (void)shape; }

/* Every channel the radio was tuned to, in order, so a test can read the
 * frequencies an attempt actually used rather than only the counter behind
 * them. */
#define TUNES_MAX	16
static uint8_t tuned_chan[TUNES_MAX];
static unsigned tunes;

void btphy_rf_tune_chan(uint16_t channel, int tx)
{
	(void)tx;
	if (tunes < TUNES_MAX)
		tuned_chan[tunes] = (uint8_t)(channel - BT_FREQ_MIN);
	tunes++;
}

void btphy_rf_fifo_write(uint8_t *data, unsigned len)
{
	(void)data;
	(void)len;
}

void btphy_rf_enable_int(btbr_int_cb_t cb, void *cb_arg, int tx)
{
	(void)cb;
	(void)cb_arg;
	(void)tx;
}

void btphy_rf_disable_int(void) { }

uint8_t btphy_whiten_seed(uint32_t clk) { (void)clk; return 0; }

/*
 * The channel a phase selects. The real sequence is a permutation of 79
 * channels; what matters here is only that a different phase picks a different
 * channel, so the phase itself stands in for one and a test can compare the
 * channel a transmit used with the phase the other end was on.
 */
uint8_t hop_channel(uint32_t clkn)
{
	(void)clkn;
	return hop_state.x % NUM_BREDR_CHANNELS;
}

/* The codec: a single-slot packet with a short payload, which is all the
 * transmit task reads out of it. */
static const bbcodec_types_t fake_codec_type = {
	1 /* nslots */, 1 /* payload_header_bytes */, 17 /* payload_bytes */,
	BB_FEC_NONE /* fec */, 1 /* has_crc */, 0 /* voice_bytes */,
	BB_LEN_HEADER /* length_kind */
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
 * starts each frame at zero, where the hardware restarts it.
 */
static void a_slot(void)
{
	fake_clock = 0;
	tdma_sched_execute();
	tdma_sched_advance();
}

static unsigned completions;

static void count_completion(int sent, void *arg)
{
	(void)sent;
	(void)arg;
	completions++;
}

/* A packet to transmit. The transmit task holds the caller's header and data
 * pointers until it reports the completion, so these stand in for whatever the
 * link layer was holding. */
static bbhdr_t a_header;
static uint8_t some_data[32];

/* Something harmless to fill a slot with, so a slot can be made full the way
 * the firmware fills one: through the scheduler. */
static int a_filler(uint8_t p1, uint8_t p2, uint16_t p3)
{
	(void)p1;
	(void)p2;
	(void)p3;
	return 0;
}

/* Put a slot's worth of tasks in the slot `frame_offset` frames ahead of the
 * one the scheduler is standing in, so the transmit task finds it full. */
static void fill_a_slot(uint8_t frame_offset)
{
	int i;

	for (i = 0; i < TDMASCHED_NUM_CB; i++) {
		if (tdma_schedule(frame_offset, a_filler, 0, 0, 0, 10) != 0)
			die("fill_a_slot: the slot was already full\n");
	}
}

/*
 * A transmit goes on the scheduler as tx_prepare() in the frame the scheduler
 * is standing in and tx_execute() in the next one, and tx_prepare() puts
 * tx_finalize() two frames on. Filling one of those three is how each way a
 * transmit can be dropped is reached.
 */
static void a_full_slot_for_the_whole_transmit(void) { fill_a_slot(0); }
static void a_full_slot_for_the_start_of_transmission(void) { fill_a_slot(1); }
static void a_full_slot_for_the_end_of_transmission(void) { fill_a_slot(2); }

/* ---- the phase at the other end ---- */

/*
 * The paged device moves its own phase once for every window it opens, which
 * is what the receive task does in the peripheral's page response. A page
 * response test asks whether a transmit kept the two ends level by opening one
 * window per transmit here and then comparing the counters.
 *
 * There is no counterpart for an inquiry: an inquiry-scanning device holds one
 * frequency for 1.28 seconds while the inquirer sweeps its 32 guesses, so the
 * inquiry tests below read the phase itself and not an agreement with anyone.
 */
static uint8_t peer_phase;

static void a_peer_window(void)
{
	peer_phase++;
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
	for (i = 0; i < TDMASCHED_NUM_FRAMES; i++)
		a_slot();
	tx_task.cb = NULL;
	tx_task.cb_arg = NULL;
	tx_task_reset();

	fake_clock = 0;
	fake_stx_strobes = 0;
	tunes = 0;
	memset(tuned_chan, 0, sizeof(tuned_chan));
	completions = 0;

	/* Both ends start on the same phase, which is where hop_start_response()
	 * and the start of an inquiry sweep leave them. */
	hop_state.x = 0;
	peer_phase = 0;
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

/* One transmit of an attempt: the other end opens the window that pairs with
 * it either way, so its phase moves whether this packet reaches the air or
 * not. */
static void one_transmit_of_an_attempt(void)
{
	a_peer_window();
	one_transmit();
}

/* ---- a transmit that goes out ---- */

MU_TEST(test_an_inquiry_transmit_that_goes_out_moves_the_phase_once)
{
	a_board(BT_MODE_INQUIRY);

	one_transmit();

	mu_assert_int_eq(1, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)hop_state.x);
}

MU_TEST(test_a_page_response_that_goes_out_moves_the_phase_once)
{
	a_board(BT_MODE_PAGING_RESPONSE);

	one_transmit_of_an_attempt();

	mu_assert_int_eq(1, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)hop_state.x);
	mu_assert_int_eq((int)peer_phase, (int)hop_state.x);
}

/* ---- a transmit the scheduler would not take at all ---- */

/*
 * The regression. Not one task of the transmit was placed, so the slot task
 * that normally moves the phase never ran -- and the phase still has to move,
 * because the transmit it belongs to has been and gone.
 */
MU_TEST(test_a_refused_inquiry_transmit_still_moves_the_phase)
{
	a_board(BT_MODE_INQUIRY);
	a_full_slot_for_the_whole_transmit();

	one_transmit();

	/* Nothing went out, and the radio was never even tuned. */
	mu_assert_int_eq(0, (int)fake_stx_strobes);
	mu_assert_int_eq(0, (int)tunes);
	/* The caller was still told, and the phase still moved. */
	mu_assert_int_eq(1, (int)completions);
	mu_assert_int_eq(1, (int)hop_state.x);
}

/* And in the page response, where the paged device's window moved its phase
 * whether this packet arrived or not, so the two are level only if this one
 * moved as well. */
MU_TEST(test_a_refused_page_response_still_moves_the_phase)
{
	a_board(BT_MODE_PAGING_RESPONSE);
	a_full_slot_for_the_whole_transmit();

	one_transmit_of_an_attempt();

	mu_assert_int_eq(0, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)completions);
	mu_assert_int_eq((int)peer_phase, (int)hop_state.x);
}

/* ---- the phase moves before the caller is told, not after ---- */

/*
 * Which substate the transmit belonged to is read before the caller is told,
 * not after. A refusal is reported from the caller's own call rather than from
 * a slot, and the callback is where a caller gives a page up and moves the
 * board to another substate: a page the board has run out of patience with
 * goes back to paging, and one that succeeds becomes a connection. Read the
 * substate after that and the phase of the attempt just abandoned is judged by
 * the substate that replaced it.
 */
static void give_the_page_up(int sent, void *arg)
{
	(void)sent;
	(void)arg;
	completions++;
	btphy.mode = BT_MODE_CENTRAL;
}

MU_TEST(test_the_substate_is_read_before_the_caller_is_told)
{
	a_board(BT_MODE_PAGING_RESPONSE);
	a_full_slot_for_the_whole_transmit();
	a_peer_window();

	tx_task_schedule(0, give_the_page_up, &a_header, &a_header, some_data);

	mu_assert_int_eq(1, (int)completions);
	mu_assert_int_eq(0, (int)fake_stx_strobes);
	/* The transmit was the page response's, so its phase moved, whatever
	 * the board went on to do. */
	mu_assert_int_eq((int)peer_phase, (int)hop_state.x);
}

/*
 * What it costs when the phase is left behind, and what the fix buys: the rest
 * of the attempt. The first transmit is refused, the slot it wanted is run out
 * by then, and the three after it go out -- each on the frequency the window
 * that pairs with it is listening on.
 */
MU_TEST(test_an_attempt_stays_level_with_the_other_end)
{
	int i;

	a_board(BT_MODE_PAGING_RESPONSE);
	a_full_slot_for_the_whole_transmit();

	for (i = 0; i < 4; i++)
		one_transmit_of_an_attempt();

	/* The first was refused; the three after it went out. */
	mu_assert_int_eq(3, (int)fake_stx_strobes);
	mu_assert_int_eq(4, (int)completions);
	mu_assert_int_eq(4, (int)peer_phase);
	mu_assert_int_eq((int)peer_phase, (int)hop_state.x);

	/* And each of those three used the channel of the window it pairs
	 * with: the second transmit of the attempt is the second window, and
	 * so on. A phase left behind would have put them on 1, 2 and 3. */
	mu_assert_int_eq(3, (int)tunes);
	mu_assert_int_eq(2, (int)tuned_chan[0]);
	mu_assert_int_eq(3, (int)tuned_chan[1]);
	mu_assert_int_eq(4, (int)tuned_chan[2]);
}

/* ---- the two drops that already left the phase where the other end is ---- */

/*
 * The slot that would have started the transmission is full, so only
 * tx_prepare() was placed. It ran, so it moved the phase, and the packet is
 * lost on a phase the other end is also on: the protocol's own retransmission
 * covers one packet. The phase must not move a second time for the same
 * transmit.
 */
MU_TEST(test_a_transmit_with_no_start_moves_the_phase_once)
{
	a_board(BT_MODE_INQUIRY);
	a_full_slot_for_the_start_of_transmission();

	one_transmit();

	mu_assert_int_eq(0, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)completions);
	mu_assert_int_eq(1, (int)hop_state.x);
}

/*
 * The slot that would have ended the transmission is full. tx_prepare() ran
 * and gives the transmit up from there, having already moved the phase, so
 * this one is level too.
 */
MU_TEST(test_a_transmit_with_no_end_moves_the_phase_once)
{
	a_board(BT_MODE_INQUIRY);
	a_full_slot_for_the_end_of_transmission();

	one_transmit();

	mu_assert_int_eq(0, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)completions);
	mu_assert_int_eq(1, (int)hop_state.x);
}

/*
 * The radio's synthesizer has not locked by the time the transmitter would
 * have been turned on. tx_prepare() ran, so the phase has moved for a packet
 * that does not go out, which is the same case again.
 */
MU_TEST(test_an_untuned_transmit_moves_the_phase_once)
{
	a_board(BT_MODE_INQUIRY);
	fake_fs_locked = 0;

	one_transmit();

	mu_assert_int_eq(0, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)completions);
	mu_assert_int_eq(1, (int)hop_state.x);
}

/* ---- the substates that sweep no phase of their own ---- */

/*
 * A connection hops on the clock, and the substates that answer a scan
 * transmit in the slot that pairs with the one they listened in, on the phase
 * they heard. None of them sweeps a phase per transmit, so none of them moves
 * one when a transmit is dropped either.
 */
static void a_dropped_transmit_leaves_the_phase_alone(int mode)
{
	a_board(mode);
	a_full_slot_for_the_whole_transmit();

	one_transmit();

	mu_assert_int_eq(0, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)completions);
}

MU_TEST(test_a_refused_transmit_on_a_connection_leaves_the_phase_alone)
{
	a_dropped_transmit_leaves_the_phase_alone(BT_MODE_CENTRAL);
	mu_assert_int_eq(0, (int)hop_state.x);
}

MU_TEST(test_a_refused_inquiry_answer_leaves_the_phase_alone)
{
	a_dropped_transmit_leaves_the_phase_alone(BT_MODE_INQUIRY_RESPONSE);
	mu_assert_int_eq(0, (int)hop_state.x);
}

MU_TEST(test_a_refused_page_leaves_the_phase_alone)
{
	a_dropped_transmit_leaves_the_phase_alone(BT_MODE_PAGING);
	mu_assert_int_eq(0, (int)hop_state.x);
}

int main(void)
{
	printf("BR/EDR transmit tests: the hop phase a transmit moves\n");
	MU_RUN(test_an_inquiry_transmit_that_goes_out_moves_the_phase_once);
	MU_RUN(test_a_page_response_that_goes_out_moves_the_phase_once);
	MU_RUN(test_a_refused_inquiry_transmit_still_moves_the_phase);
	MU_RUN(test_a_refused_page_response_still_moves_the_phase);
	MU_RUN(test_the_substate_is_read_before_the_caller_is_told);
	MU_RUN(test_an_attempt_stays_level_with_the_other_end);
	MU_RUN(test_a_transmit_with_no_start_moves_the_phase_once);
	MU_RUN(test_a_transmit_with_no_end_moves_the_phase_once);
	MU_RUN(test_an_untuned_transmit_moves_the_phase_once);
	MU_RUN(test_a_refused_transmit_on_a_connection_leaves_the_phase_alone);
	MU_RUN(test_a_refused_inquiry_answer_leaves_the_phase_alone);
	MU_RUN(test_a_refused_page_leaves_the_phase_alone);
	return mu_report();
}
