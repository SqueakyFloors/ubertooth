/*
 * The board's 100 ns capture clock across its wrap, fed straight into the
 * real time reconstruction: no device and no capture file.
 *
 * now_ns_from_clk100ns() extends clk100ns, which wraps every
 * CLK100NS_PERIOD ticks (327.68 s), into a 64-bit tick count. Extending it as
 * if it wrapped at 2^32 added 101.8 s to every packet after a wrap, so every
 * capture longer than the clock's period was wrong from the first wrap on
 * (issue #218). The reconstruction is static in ubertooth_callback.c, so that
 * source is compiled into this test.
 *
 * Only differences between reconstructed times are checked. Where a capture
 * starts is a separate matter (the anchor and the start offset), so these
 * cases hold whatever the first packet's clock is.
 */
#include <stdint.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "../../libubertooth/src/ubertooth_callback.c"

/* A clock step of 100 s, so four of them cross the wrap. */
#define STEP_TICKS 1000000000ull
/* Where the first packet lands, well away from the wrap. */
#define FIRST_TICKS 100000000ull

/* The reconstructed time of a packet captured at this clk100ns value. */
static uint64_t at_clock(ubertooth_t* ut, uint64_t ticks)
{
	usb_pkt_rx rx;

	memset(&rx, 0, sizeof(rx));
	rx.clk100ns = (uint32_t)(ticks % CLK100NS_PERIOD);
	return now_ns_from_clk100ns(ut, &rx);
}

/* A capture that has seen no packet yet. */
static void start_capture(ubertooth_t* ut)
{
	memset(ut, 0, sizeof(*ut));
}

MU_TEST(the_period_is_the_clock_the_firmware_counts)
{
	/* CLK100NS is CLK100NS_PER_CLKN per CLKN tick over CLKN's low 20 bits
	 * plus the timer, so its last value before the wrap is one below the
	 * period. */
	mu_assert_int_eq(CLK100NS_PERIOD - 1,
	                 CLK100NS_PER_CLKN * ((1u << 20) - 1)
	                 + (CLK100NS_PER_CLKN - 1));
	mu_assert_int_eq(3276800000, CLK100NS_PERIOD);
}

MU_TEST(a_tick_is_100_ns_before_any_wrap)
{
	ubertooth_t ut;
	uint64_t first, later;

	start_capture(&ut);
	first = at_clock(&ut, FIRST_TICKS);
	later = at_clock(&ut, FIRST_TICKS + STEP_TICKS);

	mu_assert_int_eq(100 * STEP_TICKS, later - first);
	mu_assert_int_eq(0, ut.clk100ns_upper);
}

MU_TEST(the_first_tick_after_a_wrap_follows_the_last_one)
{
	ubertooth_t ut;
	uint64_t last, wrapped;

	start_capture(&ut);
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
	start_capture(&ut);
	before = at_clock(&ut, 3 * STEP_TICKS);
	after = at_clock(&ut, 4 * STEP_TICKS);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100 * STEP_TICKS, after - before);
	/* What the 2^32 extension added on top of the step. */
	mu_assert(after - before < 100 * STEP_TICKS + 1000000000ull,
	          "the wrap added a step of its own");
}

MU_TEST(several_wraps_stay_continuous)
{
	ubertooth_t ut;
	uint64_t ticks, first = 0, previous = 0, now;
	int packet;

	start_capture(&ut);
	for (packet = 0; packet < 24; packet++) {
		ticks = FIRST_TICKS + (uint64_t)packet * STEP_TICKS;
		now = at_clock(&ut, ticks);
		if (packet == 0)
			first = now;
		else
			mu_assert_int_eq(100 * STEP_TICKS, now - previous);
		mu_assert_int_eq(100 * (ticks - FIRST_TICKS), now - first);
		previous = now;
	}
	/* 24 packets 100 s apart span seven of the clock's periods. */
	mu_assert_int_eq(7, ut.clk100ns_upper);
}

int main(void)
{
	MU_RUN(the_period_is_the_clock_the_firmware_counts);
	MU_RUN(a_tick_is_100_ns_before_any_wrap);
	MU_RUN(the_first_tick_after_a_wrap_follows_the_last_one);
	MU_RUN(a_wrap_adds_the_clocks_period_not_2_32);
	MU_RUN(several_wraps_stay_continuous);
	return mu_report();
}
