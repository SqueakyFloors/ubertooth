/*
 * The wait before a received packet is clocked out of the CC2400's FIFO:
 * firmware/bluetooth_rxtx/rx_fifo_lead.c, compiled and run here.
 *
 * Two modes read the radio's FIFO as soon as it reports a sync word -- the
 * generic capture and both sides of the range test -- and the radio sets that
 * flag when the sync word ends, before the first payload byte exists. The wait
 * gives the radio time to write that byte. It had a copy in each of those two
 * files; this is the one function they now share.
 *
 * The wait reads one hardware register, timer 0's counter, so the test feeds
 * that counter instead of the board (stubs-rxlead/ubertooth.h) and the real
 * loop runs. Everything below is therefore the firmware's own code and not a
 * model of it.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cc2400_wait.h"
#include "minunit.h"
#include "rx_fifo_lead.h"
/* CLK100NS_PER_CLKN, the usual length of a timer period. */
#include "ubertooth_clock.h"

/* ------------------------------------------------------------------ *
 * The board the wait reads
 * ------------------------------------------------------------------ */

/*
 * The counter the wait polls, and how it moves.
 *
 * `step` is added on every read and the result is taken modulo `period`, which
 * is what timer 0 does: it counts up and resets at the end of each period. A
 * `step` of 0 is a counter that is not advancing.
 */
static uint32_t timer;
static uint32_t timer_step;
static uint32_t timer_period;
static unsigned long timer_reads;

uint32_t fake_clock_read(void)
{
	uint32_t now = timer;

	timer_reads++;
	timer = timer_period ? (timer + timer_step) % timer_period
	                     : timer + timer_step;
	return now;
}

/* cc2400_wait_count_timeout(), counted rather than called: the counter itself
 * and the report it feeds are tested in test_cc2400_wait.c. */
static unsigned counted[CC2400_WAIT_SHAPE_COUNT];

void cc2400_wait_count_timeout(enum cc2400_wait_shape shape)
{
	if ((unsigned)shape < CC2400_WAIT_SHAPE_COUNT)
		counted[shape]++;
}

static void reset_board(uint32_t start, uint32_t step, uint32_t period)
{
	timer = start;
	timer_step = step;
	timer_period = period;
	timer_reads = 0;
	memset(counted, 0, sizeof(counted));
}

/* ------------------------------------------------------------------ *
 * How long the wait is
 * ------------------------------------------------------------------ */

/*
 * One byte of air time is the least that can work, since the wait starts when
 * the sync word has already been noticed. Much more costs packets: the radio
 * goes on writing into the same 32-byte FIFO while the packet is read out of
 * it, so the upper bound is a quarter of that FIFO.
 */
MU_TEST(test_the_lead_is_a_few_byte_times)
{
	mu_assert_int_eq(80, RX_FIFO_BYTE_100NS);
	mu_assert(RX_FIFO_LEAD_BYTES >= 1 && RX_FIFO_LEAD_BYTES <= 8,
	          "the lead needs at least the one byte the radio has not "
	          "written yet when it reports the sync word, and no more than "
	          "8, a quarter of the radio's 32-byte FIFO");
	/*
	 * And the length is that many byte times, not a tick count of its own:
	 * a bare number stops tracking the air rate, and is how a wait for one
	 * byte silently becomes a wait for thirty.
	 */
	mu_assert_int_eq(RX_FIFO_LEAD_BYTES * RX_FIFO_BYTE_100NS,
	                 RX_FIFO_LEAD_100NS);
}

/*
 * The cap has to be well clear of the turns the wait needs, or it ends the
 * wait instead of backstopping it -- which is the early read the wait exists
 * to prevent. A turn is a few ticks, so the lead takes a few hundred.
 */
MU_TEST(test_the_cap_is_a_backstop_and_not_the_bound)
{
	mu_assert(RX_FIFO_LEAD_POLLS >= 10000,
	          "a cap near the turns the wait needs is what ends the wait, "
	          "not a backstop against a timer that is not running");
}

/* ------------------------------------------------------------------ *
 * The wait, run
 * ------------------------------------------------------------------ */

/* A counter advancing normally: the wait lasts at least the lead, and says
 * nothing happened. */
MU_TEST(test_the_wait_lasts_the_lead)
{
	reset_board(0, 10, CLK100NS_PER_CLKN);
	rx_wait_for_fifo();
	/* Each read moves the counter 10 ticks, so the lead takes
	 * RX_FIFO_LEAD_100NS / 10 of them, plus the first read that only takes
	 * a starting point. */
	mu_assert_int_eq(RX_FIFO_LEAD_100NS / 10 + 1, (int)timer_reads);
	mu_assert_int_eq(0, (int)counted[CC2400_WAIT_RX_LEAD]);
}

/*
 * A wait that starts near the end of a timer period crosses the counter's
 * reset. It must still last the lead: a step taken as `now - last` across that
 * reset is a huge unsigned number, which would end the wait at once and read
 * byte 0 before the radio had written it.
 */
MU_TEST(test_a_wait_across_the_counters_reset_still_lasts_the_lead)
{
	reset_board(CLK100NS_PER_CLKN - 20, 10, CLK100NS_PER_CLKN);
	rx_wait_for_fifo();
	mu_assert(timer_reads > 2,
	          "a wait that crosses the counter's reset ended at once, so "
	          "the radio was given no time to write byte 0");
	mu_assert_int_eq(0, (int)counted[CC2400_WAIT_RX_LEAD]);

	/* The subtraction the step helper replaces, for the record. */
	mu_assert((uint32_t)(5u - (CLK100NS_PER_CLKN - 20u)) > RX_FIFO_LEAD_100NS,
	          "a bare subtraction across the reset must be the large number "
	          "this guards against");
}

/*
 * A period longer than the usual one. The timer's period is whatever is in its
 * match register: the clock interrupt lengthens one to correct for drift, and
 * UBERTOOTH_TRIM_CLOCK and UBERTOOTH_START_HOPPING pass a length from the host
 * straight through. A step out of such a period is undercounted, because
 * nothing in the wait knows how long that period was -- and undercounting is
 * the safe direction, since the wait then runs a little long rather than
 * ending on a packet that was still arriving.
 */
MU_TEST(test_a_longer_period_does_not_end_the_wait_early)
{
	static const uint32_t periods[] = { 3126u, 4477u, 6250u, 9374u, 68660u };
	size_t i;

	for (i = 0; i < sizeof(periods) / sizeof(periods[0]); i++) {
		char msg[256];

		reset_board(periods[i] - 20, 10, periods[i]);
		rx_wait_for_fifo();
		snprintf(msg, sizeof(msg),
		         "with the timer period %u ticks long the wait ended "
		         "after %lu reads, so it did not last the lead",
		         periods[i], timer_reads);
		mu_assert(timer_reads > RX_FIFO_LEAD_100NS / 10, msg);
		mu_assert_int_eq(0, (int)counted[CC2400_WAIT_RX_LEAD]);
	}
}

/*
 * No single reading of any period the timer can be left in ends the wait on
 * its own, and no counted step over-states the time it stands for. The second
 * is what settles the question for a wait of any length: while no step runs
 * ahead of the clock, no number of them can either.
 *
 * The lengths are the ones a board can be left with. The clock interrupt
 * writes 3124 plus a 16-bit offset to the match register, so the period runs
 * from 3,125 ticks to 68,660; the four short ones below are where a fixed
 * modulus, which this wait used to take its steps with, came out as the lead
 * or more where almost no time had passed.
 */
MU_TEST(test_no_reading_of_any_timer_period_ends_the_wait_early)
{
	static const struct {
		uint32_t length;
		const char *source;
	} periods[] = {
		{ 3126u, "a drift fix for a clock running fast" },
		{ 9374u, "a drift fix for a clock running slow" },
		{ 6250u, "a host offset of 3125" },
		{ 4477u, "a host offset of 1352" },
	};
	char msg[384];
	size_t i;

	for (i = 0; i < sizeof(periods) / sizeof(periods[0]); i++) {
		uint32_t length = periods[i].length;
		long ends_early = 0;
		uint32_t worst_over_count = 0;
		uint32_t last, now;

		for (last = 0; last < length; last++) {
			for (now = 0; now < length; now++) {
				uint32_t real = now < last
				                ? now + length - last
				                : now - last;
				uint32_t step = clk100ns_step(last, now);

				if (step >= RX_FIFO_LEAD_100NS &&
				    real < RX_FIFO_LEAD_100NS)
					ends_early++;
				if (step > real &&
				    step - real > worst_over_count)
					worst_over_count = step - real;
			}
		}

		snprintf(msg, sizeof(msg),
		         "with the timer period %u ticks long (%s), %ld "
		         "readings end the wait before the lead has passed",
		         length, periods[i].source, ends_early);
		mu_assert(ends_early == 0, msg);

		snprintf(msg, sizeof(msg),
		         "with the timer period %u ticks long (%s), a counted "
		         "step over-states the time it stands for by %u ticks, "
		         "so enough of them end the wait before the lead has "
		         "passed", length, periods[i].source, worst_over_count);
		mu_assert(worst_over_count == 0, msg);
	}
}

/*
 * A counter that is not advancing. The wait has to end anyway: the board polls
 * this with USB unserviced, so a loop that never ends leaves it answering no
 * control request -- not even the one that stops the mode -- until the
 * watchdog resets it.
 *
 * Giving up is silent to the capture, which reads the packet as early as it
 * would have without the wait, so the wait counts itself and
 * `ubertooth-util -w` reports it. That count is the whole of what a user can
 * see: the records of such a capture are wrong throughout, because by then the
 * radio has refilled its 32-byte FIFO several times over.
 */
MU_TEST(test_a_stopped_counter_gives_up_and_says_so)
{
	reset_board(1234, 0, CLK100NS_PER_CLKN);
	rx_wait_for_fifo();
	mu_assert_int_eq(RX_FIFO_LEAD_POLLS + 1, (int)timer_reads);
	mu_assert_int_eq(1, (int)counted[CC2400_WAIT_RX_LEAD]);
}

/* And it is counted as its own shape of wait, so the report says which wait
 * gave up. */
MU_TEST(test_giving_up_counts_only_its_own_shape)
{
	unsigned i;

	reset_board(0, 0, CLK100NS_PER_CLKN);
	rx_wait_for_fifo();
	for (i = 0; i < CC2400_WAIT_SHAPE_COUNT; i++) {
		char msg[128];

		snprintf(msg, sizeof(msg),
		         "wait shape %u was counted %u times", i, counted[i]);
		mu_assert(counted[i] == (i == CC2400_WAIT_RX_LEAD ? 1u : 0u),
		          msg);
	}
}

/* A wait that was satisfied counts nothing, so a count above zero means a
 * board whose timer had stopped and not a board that captured normally. */
MU_TEST(test_a_wait_that_works_counts_nothing)
{
	int i;

	for (i = 0; i < 3; i++) {
		reset_board(0, 10, CLK100NS_PER_CLKN);
		rx_wait_for_fifo();
		mu_assert_int_eq(0, (int)counted[CC2400_WAIT_RX_LEAD]);
	}
}

int main(void)
{
	printf("FIFO read lead tests\n");
	MU_RUN(test_the_lead_is_a_few_byte_times);
	MU_RUN(test_the_cap_is_a_backstop_and_not_the_bound);
	MU_RUN(test_the_wait_lasts_the_lead);
	MU_RUN(test_a_wait_across_the_counters_reset_still_lasts_the_lead);
	MU_RUN(test_a_longer_period_does_not_end_the_wait_early);
	MU_RUN(test_no_reading_of_any_timer_period_ends_the_wait_early);
	MU_RUN(test_a_stopped_counter_gives_up_and_says_so);
	MU_RUN(test_giving_up_counts_only_its_own_shape);
	MU_RUN(test_a_wait_that_works_counts_nothing);
	return mu_report();
}
