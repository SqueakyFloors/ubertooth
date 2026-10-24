/*
 * The board's 100 ns capture clock across its wrap, fed straight into the
 * real time reconstruction: no device and no capture file.
 *
 * now_ns_from_clk100ns() extends clk100ns into a 64-bit tick count, adding
 * the period of the clock the mode stamps with for each wrap. Extending it as
 * if every clock were a 32-bit counter added 101.8 s to every packet after a
 * wrap of the CLKN-derived clock, so every Classic capture longer than its
 * 327.68 s period was wrong from the first wrap on; adding that period to the
 * low-energy engine's timer1 stamps, which do start over at 2^32, would take
 * 101.8 s off instead. Both clocks are exercised here. The reconstruction is
 * static in ubertooth_callback.c, so that source is compiled into this test.
 *
 * Only differences between reconstructed times are checked. Where a capture
 * starts is a separate matter (the anchor and the start offset), so these
 * cases hold whatever the first packet's clock is.
 */
#include <stdint.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "../../libubertooth/src/ubertooth_callback.c"

/* The firmware's own CLK100NS, evaluated here against the shared period: the
 * registers it reads stand in as variables. */
static volatile uint32_t clkn;
static volatile uint32_t T0TC;
#include "../../../firmware/bluetooth_rxtx/ubertooth_clock.h"

/* A clock step of 100 s, so four of them cross the CLKN-derived wrap. */
#define STEP_TICKS 1000000000ull
/* Where the first packet lands, well away from either wrap. */
#define FIRST_TICKS 100000000ull

/* The reconstructed time of a packet captured at this clock value. */
static uint64_t at_clock(ubertooth_t* ut, uint64_t ticks)
{
	usb_pkt_rx rx;

	memset(&rx, 0, sizeof(rx));
	rx.clk100ns = htole32((uint32_t)(ticks % ut->clk100ns_period));
	return now_ns_from_clk100ns( ut, &rx );
}

/* A capture that has seen no packet yet, stamped with this clock. */
static void start_capture(ubertooth_t* ut, uint64_t period)
{
	memset(ut, 0, sizeof(*ut));
	ut->clk100ns_period = period;
}

MU_TEST(the_period_is_the_clock_the_firmware_counts)
{
	/* The firmware's CLK100NS at the top of its range: one tick below the
	 * period the host adds per wrap, whatever the numbers behind it. */
	clkn = 0xfffff;                      /* the low 20 bits CLK100NS uses */
	T0TC = CLK100NS_PER_CLKN - 1;        /* the timer, just before it matches */
	mu_assert_int_eq(CLK100NS_PERIOD - 1, CLK100NS);

	/* The low-energy engine's timer1 counts the same 100 ns ticks, free
	 * running, so it starts over one tick later than 2^32 - 1. */
	mu_assert(CLK100NS_TIMER1_PERIOD == 4294967296ull, "timer1 period");
}

MU_TEST(a_tick_is_100_ns_before_any_wrap)
{
	ubertooth_t ut;
	uint64_t first, later;

	start_capture(&ut, CLK100NS_PERIOD);
	first = at_clock(&ut, FIRST_TICKS);
	later = at_clock(&ut, FIRST_TICKS + STEP_TICKS);

	mu_assert_int_eq(100 * STEP_TICKS, later - first);
	mu_assert_int_eq(0, ut.clk100ns_upper);
}

MU_TEST(the_first_tick_after_a_wrap_follows_the_last_one)
{
	ubertooth_t ut;
	uint64_t last, wrapped;

	start_capture(&ut, CLK100NS_PERIOD);
	last = at_clock(&ut, CLK100NS_PERIOD - 1);
	wrapped = at_clock(&ut, CLK100NS_PERIOD);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100, wrapped - last);
}

MU_TEST(a_wrap_adds_the_clocks_period_not_2_32)
{
	ubertooth_t ut;
	uint64_t before, after;

	/* The packets either side of the first wrap, one step apart. */
	start_capture(&ut, CLK100NS_PERIOD);
	before = at_clock(&ut, 3 * STEP_TICKS);
	after = at_clock(&ut, 4 * STEP_TICKS);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100 * STEP_TICKS, after - before);
}

MU_TEST(a_timer1_wrap_adds_2_32)
{
	ubertooth_t ut;
	uint64_t before, after;

	/* The low-energy engine's clock starts over at 2^32, not at the
	 * CLKN-derived period, so its wrap must add 2^32. */
	start_capture(&ut, CLK100NS_TIMER1_PERIOD);
	before = at_clock(&ut, 4 * STEP_TICKS);
	after = at_clock(&ut, 5 * STEP_TICKS);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100 * STEP_TICKS, after - before);
}

/* Packets a step apart over several wraps, with no step of its own anywhere. */
static void continuous_over_wraps(uint64_t period, int packets, int wraps)
{
	ubertooth_t ut;
	uint64_t ticks, first = 0, previous = 0, now;
	int packet;

	start_capture(&ut, period);
	for (packet = 0; packet < packets; packet++) {
		ticks = FIRST_TICKS + (uint64_t)packet * STEP_TICKS;
		now = at_clock(&ut, ticks);
		if (packet == 0)
			first = now;
		else
			mu_assert_int_eq(100 * STEP_TICKS, now - previous);
		mu_assert_int_eq(100 * (ticks - FIRST_TICKS), now - first);
		previous = now;
	}
	mu_assert_int_eq(wraps, ut.clk100ns_upper);
}

MU_TEST(several_wraps_stay_continuous)
{
	/* 24 packets 100 s apart span seven CLKN-derived periods, and five of
	 * timer1's. */
	continuous_over_wraps(CLK100NS_PERIOD, 24, 7);
	continuous_over_wraps(CLK100NS_TIMER1_PERIOD, 24, 5);
}

/* Which clock a mode stamps with is known only to the tool that starts it, so
 * this one reads ubertooth-btle's source: the branch that starts the
 * low-energy engine must say that its packets come from timer1. A capture run
 * by hand is the only other way to notice it stopped doing so. */
MU_TEST(the_low_energy_engine_is_started_with_its_own_period)
{
	char text[200000];
	FILE *source = fopen(BTLE_SOURCE, "r");
	size_t length;
	char *sniffing;

	mu_assert(source != NULL, "cannot read " BTLE_SOURCE);
	if (source == NULL)
		return;
	length = fread(text, 1, sizeof(text) - 1, source);
	text[length] = '\0';
	fclose(source);

	sniffing = strstr(text, "cmd_btle_sniffing(");
	mu_assert(sniffing != NULL, "ubertooth-btle no longer starts the engine");
	if (sniffing == NULL)
		return;
	mu_assert(strstr(sniffing, "CLK100NS_TIMER1_PERIOD") != NULL,
	          "the engine's clock period is not set where its mode starts");
}

int main(void)
{
	MU_RUN(the_period_is_the_clock_the_firmware_counts);
	MU_RUN(a_tick_is_100_ns_before_any_wrap);
	MU_RUN(the_first_tick_after_a_wrap_follows_the_last_one);
	MU_RUN(a_wrap_adds_the_clocks_period_not_2_32);
	MU_RUN(a_timer1_wrap_adds_2_32);
	MU_RUN(several_wraps_stay_continuous);
	MU_RUN(the_low_energy_engine_is_started_with_its_own_period);
	return mu_report();
}
