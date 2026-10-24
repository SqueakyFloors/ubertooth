/*
 * Unit tests for firmware/bluetooth_rxtx/identify_blink.c
 *
 * These run on the host (plain gcc): the blink pattern of the identify mode is
 * pure integer bookkeeping, split out of identify.c so it can be checked
 * without an ARM toolchain or a board. What matters is that the LEDs start on
 * and keep alternating, that the schedule advances by exactly one interval per
 * toggle (so a slightly late interrupt does not stretch the blink) without
 * ever falling behind the timer counter, and that it keeps working across the
 * 32-bit wrap of that counter.
 */
#include <stdint.h>
#include <stdio.h>

#include "minunit.h"
#include "identify_blink.h"
#include "../../host/libubertooth/src/ubertooth_interface.h"

#define INTERVAL UBERTOOTH_IDENTIFY_TOGGLE_MS

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
		/* the handler runs exactly when the toggle was due */
		identify_blink_toggle(&blink, blink.next, INTERVAL);
		/* on, off, on, ... : the LEDs are off after an odd toggle */
		mu_assert_int_eq(i % 2 == 0, blink.level);
		mu_assert_int_eq((i + 1) * INTERVAL, (int)blink.next);
	}
}

/* A toggle is scheduled from the match that just fired, not from the time the
 * handler ran, so handling it a little late does not slow the blink down. */
MU_TEST(test_a_slightly_late_toggle_does_not_shift_the_schedule)
{
	identify_blink_t blink;
	uint32_t due;

	identify_blink_start(&blink, 500, INTERVAL);
	due = blink.next;
	identify_blink_toggle(&blink, due + INTERVAL / 2, INTERVAL);
	mu_assert_int_eq((int)(due + INTERVAL), (int)blink.next);
}

/*
 * A toggle handled a whole interval late would otherwise schedule the next
 * match in the past, and the timer would not match it again until its 32-bit
 * counter came all the way round - leaving the LEDs stuck. The next match must
 * stay in the future.
 */
MU_TEST(test_a_very_late_toggle_schedules_the_next_match_in_the_future)
{
	identify_blink_t blink;
	uint32_t now;
	int late;

	for (late = 1; late <= 4; late++) {
		identify_blink_start(&blink, 2000, INTERVAL);
		now = blink.next + (uint32_t)late * INTERVAL;
		identify_blink_toggle(&blink, now, INTERVAL);
		mu_assert((int32_t)(blink.next - now) > 0, "next match is in the future");
		mu_assert_int_eq((int)(now + INTERVAL), (int)blink.next);
	}

	/* exactly due again counts as late: the match must move on */
	identify_blink_start(&blink, 2000, INTERVAL);
	now = blink.next + INTERVAL;
	identify_blink_toggle(&blink, now, INTERVAL);
	mu_assert((int32_t)(blink.next - now) > 0, "an exactly-due match moves on");
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

	/* handled on time, across the wrap */
	identify_blink_toggle(&blink, blink.next, INTERVAL);
	mu_assert_int_eq(0, blink.level);
	mu_assert_int_eq((int)(uint32_t)(now + 2 * INTERVAL), (int)blink.next);

	/* and late, across the wrap: still scheduled ahead of the counter */
	now = UINT32_MAX - 1;
	identify_blink_start(&blink, now, INTERVAL);
	identify_blink_toggle(&blink, blink.next + 3 * INTERVAL, INTERVAL);
	mu_assert((int32_t)(blink.next - (uint32_t)(now + 4 * INTERVAL)) > 0,
	          "a late toggle across the wrap still moves ahead");
}

/* The interval is in timer ticks, and identify.c prescales TIMER3 to 1 ms per
 * tick: a blink that is not a few per second is a unit mix-up. */
MU_TEST(test_the_blink_interval_is_a_few_blinks_a_second)
{
	mu_assert(INTERVAL >= 100 && INTERVAL <= 500,
	          "identify blink interval is not 1-5 blinks a second");
}

int main(void)
{
	printf("identify blink tests\n");
	MU_RUN(test_starts_on_one_interval_before_the_first_toggle);
	MU_RUN(test_each_toggle_flips_the_leds_and_advances_one_interval);
	MU_RUN(test_a_slightly_late_toggle_does_not_shift_the_schedule);
	MU_RUN(test_a_very_late_toggle_schedules_the_next_match_in_the_future);
	MU_RUN(test_the_schedule_wraps_with_the_timer_counter);
	MU_RUN(test_the_blink_interval_is_a_few_blinks_a_second);
	return mu_report();
}
