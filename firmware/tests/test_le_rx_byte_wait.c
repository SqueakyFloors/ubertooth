/*
 * How long the promiscuous low-energy capture waits for the rest of a packet.
 *
 * bt_le_sync() reads a PDU's header out of the first four bytes the DMA
 * controller delivered, works out how many more the packet needs, and waits
 * for them. That wait used to end on four things: the bytes arriving, the
 * transfer running out of buffer, a transfer error, or the host changing mode.
 * None of them covers the radio stopping mid-packet while the transfer stays
 * armed -- and in that case the loop spun until the host changed mode. A host
 * waiting for capture data never does, and this mode services USB from its
 * interrupt, so the board went on answering requests while the capture
 * recorded nothing.
 *
 * The wait now has a budget, built from the air time of the bytes it is owed,
 * and adds up the time it has waited as it polls. The wait itself is hardware
 * code -- it reads the DMA controller's registers and the board's timer -- so
 * this test does two things:
 *
 *  - runs the arithmetic the budget is built from, which is pure logic in
 *    bluetooth_le.h: the air time of n bytes, and one step of a wait timed by
 *    a counter that resets every 312.5 us;
 *  - models the loop around a fake timer and a fake transfer, and drives it
 *    through each way it can end, so what a timeout does to the capture is
 *    checked rather than assumed. The fake timer resets like the real one, and
 *    one case drives the loop with readings that run backwards, which is what
 *    a deadline on CLK100NS would have tripped over.
 *
 * test_le_frag_shape.c pins the firmware's own wait against this: that it
 * takes its budget from le_rx_byte_wait_clk100ns(), adds up
 * le_clk100ns_step(), breaks when the total passes the budget, and marks the
 * next record.
 */
#include <stdio.h>
#include <string.h>

#include "minunit.h"
/* bluetooth_le.h holds the two helpers under test; stubs/ubertooth.h stands in
 * for the hardware header it includes, and brings CLK100NS_PER_CLKN with it. */
#include "bluetooth_le.h"

/* ------------------------------------------------------------------ *
 * The air time of the bytes a wait is owed
 * ------------------------------------------------------------------ */

/* The 1M PHY carries one bit per microsecond: a byte is 8 us, which is 80
 * ticks of the 100 ns clock. */
MU_TEST(a_byte_takes_its_air_time)
{
	mu_assert_int_eq(80, LE_1M_BYTE_CLK100NS);
}

/* The budget for n bytes is their air time plus the slack, and the slack is
 * the whole budget when there are no bytes left to wait for. */
MU_TEST(the_budget_is_air_time_plus_slack)
{
	mu_assert_int_eq(LE_RX_BYTE_WAIT_SLACK_CLK100NS,
	                 (int)le_rx_byte_wait_clk100ns(0));
	mu_assert_int_eq(80 + LE_RX_BYTE_WAIT_SLACK_CLK100NS,
	                 (int)le_rx_byte_wait_clk100ns(1));
	mu_assert_int_eq(27 * 80 + LE_RX_BYTE_WAIT_SLACK_CLK100NS,
	                 (int)le_rx_byte_wait_clk100ns(27));
}

/*
 * The longest packet this loop will wait for: a 2-byte header, the 251 payload
 * bytes LE_MAX_PAYLOAD allows, and a 3-byte CRC, rounded up to whole words, is
 * 256 bytes -- about 2.0 ms on the air. The budget has to be longer than that,
 * or a real long packet would be cut short, and it has to stay far below the
 * three seconds the watchdog allows, or the budget would be no use.
 */
MU_TEST(the_longest_packet_fits_the_budget_with_room_to_spare)
{
	unsigned longest = 4 * (((2 + LE_MAX_PAYLOAD + 3) + 3) / 4);
	uint32_t budget = le_rx_byte_wait_clk100ns(longest);

	mu_assert_int_eq(256, (int)longest);
	mu_assert(budget > longest * 80,
	          "the budget does not cover the packet's own air time");
	/* 2.0 ms of air time, 2.2 ms of budget: both well inside a quarter of a
	 * second, let alone the watchdog's three. */
	mu_assert(budget < 2500000u / 100u,
	          "the budget is longer than a quarter of a second");
}

/* ------------------------------------------------------------------ *
 * One step of the wait
 * ------------------------------------------------------------------ */

MU_TEST(a_step_is_the_difference_within_one_timer_period)
{
	mu_assert_int_eq(0, (int)le_clk100ns_step(0, 0));
	mu_assert_int_eq(5, (int)le_clk100ns_step(10, 15));
	mu_assert_int_eq(1000, (int)le_clk100ns_step(2000, 3000));
}

/* Timer 0 resets every CLK100NS_PER_CLKN ticks, so a step across the reset
 * reads as a smaller number than the one it started from. */
MU_TEST(a_step_across_the_timer_reset_is_the_time_it_was)
{
	uint32_t last = CLK100NS_PER_CLKN - 10;

	mu_assert_int_eq(3125, CLK100NS_PER_CLKN);
	mu_assert_int_eq(14, (int)le_clk100ns_step(last, 4));
	mu_assert_int_eq(10, (int)le_clk100ns_step(last, 0));
}

/*
 * Why the wait adds up steps instead of comparing a deadline against CLK100NS.
 *
 * CLK100NS is CLK100NS_PER_CLKN * (clkn & 0xfffff) + T0TC, and clkn is
 * advanced by timer 0's own interrupt handler, so between the timer's reset
 * and the handler running every reading is about one whole clkn tick lower
 * than the one before it. A deadline on the difference would be met in that
 * window -- it comes round every 312.5 us -- and a packet that was arriving
 * normally would be dropped. Adding up T0TC steps never sees it.
 */
MU_TEST(readings_that_run_backwards_do_not_end_the_wait)
{
	uint32_t budget = le_rx_byte_wait_clk100ns(256);
	/* Four readings of CLK100NS spanning the timer's reset, with the
	 * interrupt handler yet to advance clkn on the last two. */
	uint32_t composite[] = { 10 * CLK100NS_PER_CLKN + 3100,
	                         10 * CLK100NS_PER_CLKN + 3124,
	                         10 * CLK100NS_PER_CLKN + 2,
	                         10 * CLK100NS_PER_CLKN + 20 };
	uint32_t waited = 0;
	size_t i;

	/* What a difference on CLK100NS would have said: a jump of nearly a
	 * whole clkn tick backwards, which as an unsigned difference is far
	 * past any budget. */
	mu_assert(composite[2] - composite[1] > budget,
	          "this is not the case the step helper is for");

	/* What the step helper says, fed the timer readings those composites
	 * were built from. */
	for (i = 1; i < sizeof(composite) / sizeof(composite[0]); i++)
		waited += le_clk100ns_step(composite[i - 1] % CLK100NS_PER_CLKN,
		                           composite[i] % CLK100NS_PER_CLKN);
	mu_assert_int_eq(24 + 3 + 18, (int)waited);
	mu_assert(waited <= budget, "a 4.5 us wait reads as overdue");
}

/*
 * A reading taken during a period the drift correction had made longer does
 * not say how much of that period was left, so the step counts only how far
 * into the new period the wait is. The drift fix sets the match value to 9373,
 * and a match value left by one mode's last fix can still be in the timer when
 * the next mode starts, which is the one way this is reached.
 */
MU_TEST(a_step_out_of_a_lengthened_period_counts_what_it_knows)
{
	/* 9374 ticks in that period, so 9300 -> 5 really took 79. The step
	 * counts the 5 it is sure of. */
	mu_assert_int_eq(5, (int)le_clk100ns_step(9300, 5));
	mu_assert(le_clk100ns_step(9300, 5) <= 79,
	          "the step counted more time than really passed");
	/* Within such a period the difference is the whole truth. */
	mu_assert_int_eq(200, (int)le_clk100ns_step(9000, 9200));
}

/* A gap longer than one timer period cannot be told from a short one, so it is
 * undercounted, as is a gap of exactly one period. That is the safe direction:
 * the wait runs a little longer than its budget rather than ending early on a
 * packet that was still arriving. */
MU_TEST(a_gap_longer_than_the_timer_period_is_undercounted)
{
	/* Exactly one period: the two readings are equal. */
	mu_assert_int_eq(0, (int)le_clk100ns_step(100, 100));
	/* Three periods and 50 ticks reads as 50. */
	mu_assert_int_eq(50, (int)le_clk100ns_step(100, 150));
	mu_assert(le_clk100ns_step(100, 150) < CLK100NS_PER_CLKN,
	          "a multi-period gap was counted as more than a period");
}

/* And a step never says more time passed than really did, whatever the two
 * readings are: a deadline built on it cannot end a wait early. */
MU_TEST(a_step_never_overcounts)
{
	uint32_t last, now;

	for (last = 0; last < 2 * CLK100NS_PER_CLKN; last += 37)
		for (now = 0; now < CLK100NS_PER_CLKN; now += 41) {
			uint32_t step = le_clk100ns_step(last, now);

			mu_assert(step <= 2 * CLK100NS_PER_CLKN,
			          "a step claimed more than two timer periods");
			if (now >= last)
				mu_assert_int_eq((int)(now - last), (int)step);
			else
				mu_assert(step <= CLK100NS_PER_CLKN,
				          "a step across a reset claimed more than"
				          " a period");
		}
}

/* ------------------------------------------------------------------ *
 * The loop, modelled
 * ------------------------------------------------------------------ */

/* The board state the wait reads and writes. */
static uint32_t timer;            /* T0TC, which resets every period */
static uint32_t elapsed;          /* the real time, for the fakes' own use */
static unsigned delivered;        /* bytes the transfer has written */
static int transfer_armed;        /* DMACCxConfig_E */
static int rx_err;
static int mode_changed;
static uint8_t status;
/* cc2400_wait_count_timeout(CC2400_WAIT_RX_BYTES), counted rather than called:
 * the counter itself is tested in test_cc2400_wait.c. */
static unsigned rx_byte_timeouts;

/* How the fake timer and the fake transfer advance on each pass of the loop:
 * one tick per pass, and a byte every `byte_every` ticks while the radio is
 * still sending, which it is up to `deliver_up_to` bytes. */
static uint32_t byte_every;
static unsigned deliver_up_to;

/* The outcome the loop reaches. */
typedef struct {
	int whole;          /* the packet may be handed over */
	int overdue;        /* the wait gave up */
	int cleanup;        /* the mode was stopped */
	uint8_t status;     /* what the next record will carry */
	unsigned counted;   /* radio waits counted as having given up */
	uint32_t waited;    /* how long the wait took, as the loop counted it */
} wait_result;

static void reset_board(void)
{
	/* Part-way through a timer period, so a wait of any length crosses at
	 * least one reset. */
	timer = CLK100NS_PER_CLKN - 500;
	elapsed = 0;
	delivered = 4;              /* the header has arrived */
	transfer_armed = 1;
	rx_err = 0;
	mode_changed = 0;
	status = 0;
	rx_byte_timeouts = 0;
	byte_every = 80;            /* a byte every 8 us, as on the air */
	deliver_up_to = 0;
}

/*
 * bt_le_sync()'s wait for the rest of a packet, with the registers replaced by
 * the variables above. `want` is the bytes the packet needs after the access
 * address, as the firmware works it out.
 */
static wait_result run_wait(unsigned want)
{
	uint32_t wait_budget = le_rx_byte_wait_clk100ns(want);
	uint32_t waited = 0;
	uint32_t last_tick = timer;
	int overdue = 0;
	wait_result r;

	while (delivered < want && transfer_armed && rx_err == 0
	       && !mode_changed) {
		uint32_t now_tick = timer;

		waited += le_clk100ns_step(last_tick, now_tick);
		last_tick = now_tick;
		if (waited > wait_budget) {
			overdue = 1;
			break;
		}
		/* One pass of the loop: the timer moves and resets like the
		 * real one, and the transfer delivers a byte when one is due
		 * and the radio is still sending. */
		timer = (timer + 1) % CLK100NS_PER_CLKN;
		elapsed++;
		if (delivered < deliver_up_to && elapsed % byte_every == 0)
			delivered++;
	}

	r.whole = rx_err == 0 && !overdue && delivered >= want;
	r.overdue = overdue;
	r.cleanup = mode_changed;
	if (rx_err)
		status |= DMA_ERROR;
	if (overdue)
		rx_byte_timeouts++;
	r.status = status;
	r.counted = rx_byte_timeouts;
	r.waited = waited;
	return r;
}

/* The ordinary case: the bytes arrive at the rate the air delivers them, and
 * the packet is handed over. */
MU_TEST(a_packet_that_arrives_is_handed_over)
{
	wait_result r;

	reset_board();
	deliver_up_to = 32;
	r = run_wait(32);

	mu_assert(r.whole, "a packet that arrived in full was dropped");
	mu_assert(!r.overdue, "a packet that arrived in time read as overdue");
	mu_assert_int_eq(0, r.status);
}

/* The longest packet this loop waits for still arrives in time, timer resets
 * and all: the budget has to be loose enough for the whole of it. */
MU_TEST(the_longest_packet_still_arrives_in_time)
{
	unsigned longest = 4 * (((2 + LE_MAX_PAYLOAD + 3) + 3) / 4);
	wait_result r;

	reset_board();
	deliver_up_to = longest;
	r = run_wait(longest);

	mu_assert(r.whole, "the longest packet was cut short by the budget");
	mu_assert_int_eq(0, r.status);
	/* The wait crossed several timer resets and still counted the time it
	 * really took, to within a tick. */
	mu_assert(r.waited >= (longest - 4) * 80 - 100,
	          "the wait lost time across the timer's resets");
}

/* The fault this budget is for: the radio stops delivering partway through a
 * packet and the transfer stays armed. The wait used to run until the host
 * changed mode. It now ends, within the budget. */
MU_TEST(a_radio_that_stops_mid_packet_ends_the_wait)
{
	wait_result r;

	reset_board();
	deliver_up_to = 20;         /* 16 of the 64 bytes, then silence */
	r = run_wait(64);

	mu_assert(r.overdue, "the wait did not give up on a stalled radio");
	mu_assert(!r.whole, "a part-packet was handed over");
	mu_assert(!r.cleanup, "the mode was torn down instead of carrying on");
	mu_assert(r.waited <= le_rx_byte_wait_clk100ns(64) + 1,
	          "the wait outlasted its budget");
}

/* A timeout is counted among the radio waits that gave up, which is what the
 * host reads back: the packet it happened to is thrown away, so there is
 * nothing else to carry it. It is not reported as a transfer error. */
MU_TEST(a_timeout_is_counted_for_the_host)
{
	wait_result r;

	reset_board();
	deliver_up_to = 4;          /* nothing after the header */
	r = run_wait(40);

	mu_assert_int_eq(1, (int)r.counted);
	mu_assert_int_eq(0, r.status);
}

/* The other ways the wait ends still end it, and a timeout is not claimed for
 * any of them. */
MU_TEST(the_other_ends_of_the_wait_are_unchanged)
{
	wait_result r;

	/* The transfer ran out of buffer. */
	reset_board();
	deliver_up_to = 20;
	transfer_armed = 0;
	r = run_wait(64);
	mu_assert(!r.whole, "a part-packet was handed over");
	mu_assert(!r.overdue, "a disarmed transfer was reported as a timeout");
	mu_assert_int_eq(0, (int)r.counted);

	/* A transfer error. */
	reset_board();
	deliver_up_to = 20;
	rx_err = 1;
	r = run_wait(64);
	mu_assert(!r.whole, "a packet cut short by an error was handed over");
	mu_assert(!r.overdue, "a transfer error was reported as a timeout");
	mu_assert(r.status & DMA_ERROR, "a transfer error was not reported");
	mu_assert_int_eq(0, (int)r.counted);

	/* The host stopped the mode. */
	reset_board();
	deliver_up_to = 20;
	mode_changed = 1;
	r = run_wait(64);
	mu_assert(r.cleanup, "a stopped mode did not end the wait");
	mu_assert(!r.overdue, "a stopped mode was reported as a timeout");
	mu_assert_int_eq(0, (int)r.counted);
}

int main(void)
{
	printf("LE receive byte wait tests\n");
	MU_RUN(a_byte_takes_its_air_time);
	MU_RUN(the_budget_is_air_time_plus_slack);
	MU_RUN(the_longest_packet_fits_the_budget_with_room_to_spare);
	MU_RUN(a_step_is_the_difference_within_one_timer_period);
	MU_RUN(a_step_across_the_timer_reset_is_the_time_it_was);
	MU_RUN(readings_that_run_backwards_do_not_end_the_wait);
	MU_RUN(a_step_out_of_a_lengthened_period_counts_what_it_knows);
	MU_RUN(a_gap_longer_than_the_timer_period_is_undercounted);
	MU_RUN(a_step_never_overcounts);
	MU_RUN(a_packet_that_arrives_is_handed_over);
	MU_RUN(the_longest_packet_still_arrives_in_time);
	MU_RUN(a_radio_that_stops_mid_packet_ends_the_wait);
	MU_RUN(a_timeout_is_counted_for_the_host);
	MU_RUN(the_other_ends_of_the_wait_are_unchanged);
	return mu_report();
}
