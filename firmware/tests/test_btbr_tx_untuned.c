/*
 * Unit tests for what the BR/EDR image's transmit task does when the radio's
 * frequency synthesizer has not locked: firmware/btbr/src/btphy/tx_task.c.
 *
 * A transmit is three slot tasks. tx_prepare() tunes the radio, strobes the
 * synthesizer on, encodes the packet and schedules tx_finalize(); tx_execute()
 * turns the transmitter on a slot later; tx_finalize() waits for the air to go
 * quiet, stops the radio and reports the completion the caller is waiting for.
 * The caller's callback is what makes a connection go on to its next slot, so
 * a transmit that never reported one would be a link that stops forever.
 *
 * tx_execute() returns early when it finds the synthesizer unlocked, because
 * the packet cannot go out on an untuned radio. These cases pin what happens
 * then: the completion is still reported, exactly once, and nothing the
 * transmit task was holding for the caller stays held.
 *
 * tx_task.c is included into this translation unit, which is how its slot
 * tasks can be reached at all: they are static. The scheduler underneath is
 * the firmware's own -- tdma_sched.c is linked, so the order the three tasks
 * run in is the board's order and not this file's idea of it. Everything else
 * is faked here: the radio, the slot clock, the hop sequence, and the codec.
 * The only fake with behaviour is the radio's lock bit, which is what decides
 * which path tx_execute() takes.
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
 * read, so a wait that polls it makes progress. How far it moved over a slot
 * is how these tests tell a task that waited from one that did not.
 */
static uint32_t fake_clock;
uint32_t fake_clock_match = CLKN_OFFSET_RESET_VAL - 1;

uint32_t fake_clock_read(void)
{
	uint32_t now = fake_clock;

	fake_clock += 10;
	return now;
}

static uint32_t fake_clock_peek(void)
{
	return fake_clock;
}

int fake_gio6(void)
{
	return 0;
}

/* ---- the rest of the transmit task's world ---- */

static char console[512];
static unsigned console_len;

void cprintf(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	console_len += vsnprintf(console + console_len,
	                         sizeof(console) - console_len, fmt, args);
	va_end(args);
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

/* The radio. `fake_fs_locked` is the status bit tx_execute() reads, and the
 * strobes are recorded so a test can see whether the transmitter was ever
 * turned on. */
static int fake_fs_locked;
static unsigned fake_stx_strobes;
static unsigned fake_srfoff_strobes;
static unsigned fake_sfson_strobes;

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
	case SFSON:
		fake_sfson_strobes++;
		break;
	default:
		break;
	}
	return 0;
}

void cc2400_set(u8 reg, u16 val) { (void)reg; (void)val; }

/*
 * The radio's state machine. tx_finalize() waits for the synthesizer to be
 * back on, which is where a transmit that did go out ends. The state is set
 * per test: a transmit the fake let through reaches it at once, so the wait
 * returns on its first read.
 */
static u16 fake_fsm_state = STATE_STROBE_FS_ON;

u16 cc2400_get(u8 reg)
{
	if (reg == FSMSTATE)
		return fake_fsm_state;
	return 0;
}

/* The watchdog breadcrumb and the give-up count the state machine wait writes.
 * The count is the one ubertooth-util -w reports as a radio that never
 * answered, so a test can check that a transmit which never started does not
 * add to it. */
static uint8_t fake_live_site;
static unsigned fake_fsm_timeouts;

uint8_t watchdog_mark_wait(uint8_t site)
{
	uint8_t previous = fake_live_site;

	fake_live_site = site;
	return previous;
}

void watchdog_leave_wait(uint8_t site)
{
	fake_live_site = site;
}

void cc2400_wait_count_timeout(enum cc2400_wait_shape shape)
{
	if (shape == CC2400_WAIT_FSM)
		fake_fsm_timeouts++;
}

static unsigned fake_fifo_writes;

void btphy_rf_tune_chan(uint16_t channel, int tx) { (void)channel; (void)tx; }

void btphy_rf_fifo_write(uint8_t *data, unsigned len)
{
	(void)data;
	(void)len;
	fake_fifo_writes++;
}

static unsigned fake_int_enables;
static unsigned fake_int_disables;

void btphy_rf_enable_int(btbr_int_cb_t cb, void *cb_arg, int tx)
{
	(void)cb;
	(void)cb_arg;
	(void)tx;
	fake_int_enables++;
}

void btphy_rf_disable_int(void)
{
	fake_int_disables++;
}

void *mem_pool_alloc(unsigned size) { (void)size; return NULL; }
void mem_pool_free(void *p) { (void)p; }

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
 * The scheduler underneath, so the order the three transmit tasks run in is
 * the firmware's own. It marks a parameter it does not read with `__unused`,
 * which the ARM toolchain's headers supply and a host's C library may not, so
 * it is spelled out here -- after the host headers above, which on some
 * platforms use the same name for a structure member. Included rather than
 * linked for that reason alone.
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

static void a_board_whose_synthesizer_locks(int locks)
{
	memset(&btphy, 0, sizeof(btphy));
	/* An even clock tick: a Central transmits in the first slot of a pair,
	 * and tx_execute() stops the firmware if it finds otherwise. */
	btphy.mode = BT_MODE_CENTRAL;
	btphy.central_clkn = 0;
	memset(&a_header, 0, sizeof(a_header));

	tdma_sched_reset();
	tx_task_reset();
	tx_task.cb = NULL;
	tx_task.cb_arg = NULL;

	fake_fs_locked = locks;
	fake_fsm_state = STATE_STROBE_FS_ON;
	fake_clock = 0;
	fake_stx_strobes = 0;
	fake_srfoff_strobes = 0;
	fake_sfson_strobes = 0;
	fake_fifo_writes = 0;
	fake_int_enables = 0;
	fake_int_disables = 0;
	fake_fsm_timeouts = 0;
	fake_live_site = WATCHDOG_SITE_NONE;
	console_len = 0;
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

/* ---- a transmit that goes out ---- */

/* The ordinary path, for comparison: the packet goes on the air and the caller
 * is told once that the slot is over. */
MU_TEST(test_a_transmit_that_goes_out_reports_completion_once)
{
	a_board_whose_synthesizer_locks(1);

	one_transmit();

	mu_assert_int_eq(1, (int)fake_stx_strobes);
	mu_assert_int_eq(1, (int)completions);
	mu_assert(last_completion_arg == &a_header,
	          "the completion did not carry the caller's argument");
}

/* ---- a transmit that finds the synthesizer unlocked ---- */

/*
 * The regression. The packet cannot go out, and the caller still has to be
 * told the slot is over: its callback is what schedules the connection's next
 * receive or transmit, so a missing one is a link that stops forever.
 */
MU_TEST(test_an_untuned_transmit_reports_completion_once)
{
	a_board_whose_synthesizer_locks(0);

	one_transmit();

	/* Nothing went on the air. */
	mu_assert_int_eq(0, (int)fake_stx_strobes);
	/* And the caller was told, once. */
	mu_assert_int_eq(1, (int)completions);
	mu_assert(last_completion_arg == &a_header,
	          "the completion did not carry the caller's argument");
}

/* The user sees why: a console line saying the packet was not sent. */
MU_TEST(test_an_untuned_transmit_says_so_on_the_console)
{
	a_board_whose_synthesizer_locks(0);

	one_transmit();

	mu_assert(strstr(console, "not sent") != NULL, console);
}

/* The radio is stopped, so the board is not left with a half-configured
 * transmitter on. */
MU_TEST(test_an_untuned_transmit_stops_the_radio)
{
	a_board_whose_synthesizer_locks(0);

	one_transmit();

	mu_assert(fake_srfoff_strobes > 0, "the radio was left running");
	mu_assert_int_eq(0, (int)fake_int_enables);
}

/*
 * Nothing the caller handed over stays held. The transmit task keeps the
 * caller's header and data pointers for the length of a transmit and the
 * caller may not reuse them until the completion; after an untuned slot they
 * are released, and the task takes the next packet rather than stopping the
 * firmware with "already got pkt".
 */
MU_TEST(test_an_untuned_transmit_gives_the_packet_back)
{
	a_board_whose_synthesizer_locks(0);

	one_transmit();

	mu_assert(tx_task.user_hdr == NULL, "the header is still held");
	mu_assert(tx_task.user_data == NULL, "the data is still held");
	mu_assert(tx_task.user_fmt == NULL, "the format is still held");

	/* So the next packet is accepted. */
	one_transmit();
	mu_assert_int_eq(2, (int)completions);
}

/*
 * What the connection does with it: a caller whose completion callback
 * schedules its next transmit keeps going across a slot the synthesizer missed,
 * instead of the link stopping there. The middle slot of three is the untuned
 * one.
 */
static unsigned slots_wanted;

static void schedule_another_transmit(void *arg)
{
	(void)arg;
	completions++;
	if (--slots_wanted == 0)
		return;
	/* The second transmit is the one the radio misses. */
	fake_fs_locked = slots_wanted != 2;
	tx_task_schedule(0, schedule_another_transmit, NULL, &a_header, some_data);
}

MU_TEST(test_a_link_goes_on_to_its_next_slot_after_an_untuned_transmit)
{
	int i;

	a_board_whose_synthesizer_locks(1);
	slots_wanted = 3;

	tx_task_schedule(0, schedule_another_transmit, NULL, &a_header, some_data);
	for (i = 0; i < 12; i++)
		a_slot();

	mu_assert_int_eq(3, (int)completions);
	/* Two of the three went on the air; the middle one did not. */
	mu_assert_int_eq(2, (int)fake_stx_strobes);
}

/*
 * A transmit that never started is not waited for. The end of a transmit is
 * the radio coming back to the synthesizer-on state, which an untuned slot
 * never left for the transmitter in the first place: waiting for it would
 * spend the slot polling the radio inside the timer interrupt and would count
 * a radio that never answered, which ubertooth-util -w reports. Neither
 * belongs to a slot where nothing was transmitted.
 */
MU_TEST(test_an_untuned_transmit_does_not_wait_for_the_radio)
{
	/* A radio that is in no state the transmit task waits for, so a wait
	 * would run to its deadline. */
	a_board_whose_synthesizer_locks(0);
	fake_fsm_state = STATE_IDLE;

	one_transmit();

	mu_assert_int_eq(0, (int)fake_fsm_timeouts);
	mu_assert(fake_clock_peek() < TX_MAX_WAIT,
	          "the slot was spent waiting for a transmit that never started");
	mu_assert_int_eq(WATCHDOG_SITE_NONE, fake_live_site);
}

/* The wait is still there for a transmit that did go out: that one has an end
 * on the air to wait for. */
MU_TEST(test_a_transmit_that_goes_out_is_still_waited_for)
{
	a_board_whose_synthesizer_locks(1);
	/* The radio does not reach the state, so the wait runs to its
	 * deadline and is counted. */
	fake_fsm_state = STATE_IDLE;

	one_transmit();

	mu_assert_int_eq(1, (int)fake_fsm_timeouts);
	mu_assert_int_eq(1, (int)completions);
}

int main(void)
{
	printf("BR/EDR transmit tests: an unlocked synthesizer\n");
	MU_RUN(test_a_transmit_that_goes_out_reports_completion_once);
	MU_RUN(test_an_untuned_transmit_reports_completion_once);
	MU_RUN(test_an_untuned_transmit_says_so_on_the_console);
	MU_RUN(test_an_untuned_transmit_stops_the_radio);
	MU_RUN(test_an_untuned_transmit_gives_the_packet_back);
	MU_RUN(test_a_link_goes_on_to_its_next_slot_after_an_untuned_transmit);
	MU_RUN(test_an_untuned_transmit_does_not_wait_for_the_radio);
	MU_RUN(test_a_transmit_that_goes_out_is_still_waited_for);
	return mu_report();
}
