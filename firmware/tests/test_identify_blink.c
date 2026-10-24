/*
 * Unit tests for firmware/bluetooth_rxtx/identify_blink.c
 *
 * These run on the host (plain gcc): the blink pattern of the identify mode is
 * pure integer bookkeeping, split out of identify.c so it can be checked
 * without an ARM toolchain or a board. What matters is that the LEDs start on
 * and keep alternating, that the schedule advances by exactly one interval per
 * toggle (so a late interrupt does not stretch the blink), and that it keeps
 * working across the 32-bit wrap of the timer counter.
 */
#include <stdint.h>

#include "minunit.h"
#include "identify_blink.h"
#include "../../host/libubertooth/src/ubertooth_interface.h"

#define INTERVAL UBERTOOTH_IDENTIFY_BLINK_MS

MU_TEST(test_starts_on_one_interval_before_the_first_toggle)
{
	identify_blink_t blink;

	identify_blink_start(&blink, 1000, INTERVAL);
	mu_assert_int_eq(1, blink.level);
	mu_assert_int_eq(1000 + INTERVAL, (int)blink.next);
}

MU_TEST(test_each_toggle_flips_the_leds_and_advances_one_interval)
{
	identify_blink_t blink;
	int i;

	identify_blink_start(&blink, 0, INTERVAL);
	for (i = 1; i <= 20; i++) {
		identify_blink_toggle(&blink, INTERVAL);
		/* on, off, on, ... : the LEDs are off after an odd toggle */
		mu_assert_int_eq(i % 2 == 0, blink.level);
		mu_assert_int_eq((i + 1) * INTERVAL, (int)blink.next);
	}
}

/* A toggle is scheduled from the match that just fired, not from the time the
 * handler ran, so handling it late does not slow the blink down. */
MU_TEST(test_the_schedule_does_not_drift_with_a_late_toggle)
{
	identify_blink_t blink;
	uint32_t due;

	identify_blink_start(&blink, 500, INTERVAL);
	due = blink.next;
	identify_blink_toggle(&blink, INTERVAL);
	mu_assert_int_eq((int)(due + INTERVAL), (int)blink.next);
}

/* The timer counter is 32 bits and wraps; the schedule must wrap with it
 * instead of running away past the end of the range. */
MU_TEST(test_the_schedule_wraps_with_the_timer_counter)
{
	identify_blink_t blink;
	uint32_t now = UINT32_MAX - (INTERVAL / 2);

	identify_blink_start(&blink, now, INTERVAL);
	mu_assert_int_eq(1, blink.level);
	mu_assert(blink.next < now, "the first match wrapped past the end");
	mu_assert_int_eq((int)(uint32_t)(now + INTERVAL), (int)blink.next);

	identify_blink_toggle(&blink, INTERVAL);
	mu_assert_int_eq(0, blink.level);
	mu_assert_int_eq((int)(uint32_t)(now + 2 * INTERVAL), (int)blink.next);
}

/* The interval is in timer ticks, and identify.c prescales TIMER3 to 1 ms per
 * tick: a blink that is not a few per second is a unit mix-up. */
MU_TEST(test_the_blink_interval_is_about_three_toggles_a_second)
{
	mu_assert(INTERVAL >= 100 && INTERVAL <= 500,
	          "identify blink interval is not 2-10 toggles a second");
}

int main(void)
{
	MU_RUN(test_starts_on_one_interval_before_the_first_toggle);
	MU_RUN(test_each_toggle_flips_the_leds_and_advances_one_interval);
	MU_RUN(test_the_schedule_does_not_drift_with_a_late_toggle);
	MU_RUN(test_the_schedule_wraps_with_the_timer_counter);
	MU_RUN(test_the_blink_interval_is_about_three_toggles_a_second);
	return mu_report();
}
