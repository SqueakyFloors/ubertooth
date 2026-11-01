/*
 * The two LED blinks that run on a timer interrupt: the identify mode's blink
 * pattern, and the timer setup both it and the xmas demo do around theirs.
 *
 * These run on the host (plain gcc). The blink pattern of the identify mode is
 * pure integer bookkeeping, split out of identify.c so it can be checked
 * without an ARM toolchain or a board. What matters is that the LEDs start on
 * and keep alternating, that the schedule advances by exactly one interval per
 * toggle (so a slightly late interrupt does not stretch the blink) without
 * ever falling behind the timer counter, and that it keeps working across the
 * 32-bit wrap of that counter.
 *
 * The timer setup around it is hardware registers, so the cases at the end
 * read identify.c and xmas.c as text instead of running them.
 */
#include <stdint.h>
#include <stdio.h>

#include <string.h>

#include "minunit.h"
#include "source_scan.h"
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


/*
 * The two LED blinks that run on a timer interrupt, read as source.
 *
 * Both start their timer, schedule a first match and then enable the match
 * interrupt, and the order of those steps is what keeps the handler and the
 * mode's loop out of each other's way. The registers are hardware, so this
 * part is scanned rather than run.
 *
 * What it pins: a match an earlier run of the same mode latched is cleared
 * before the counter runs, and the mode leaves no match armed behind it. The
 * xmas mode picks its blink times with rand(), which keeps one sequence for
 * every caller, so a handler entered while its loop is still choosing them
 * would be in rand() at the same time; and a toggle would be taken before the
 * first one was scheduled.
 */

/* What one function's body was found to contain, by line number: 0 for a line
 * that is not there. */
struct timer_scan {
	int found;          /* the function body was read at all */
	int lines;          /* lines in it */
	int clear_matches;  /* the match control register set to 0 */
	int clear_flags;    /* the interrupt flags cleared */
	int counter_enable; /* the counter started */
	int counter_reset;  /* the counter held at 0 */
	int unmask;         /* the interrupt enabled in the controller */
	int mask;           /* and disabled */
	int arm_matches;    /* matches armed with |= */
	int last_match_set; /* the last match register written */
};

/*
 * Reads one function out of a source file. `start` is the line the function
 * opens on, matched as a whole line, and the body ends at the first line that
 * is a lone closing brace. `regs` names the registers for this timer, in the
 * order the struct lists them.
 */
static int scan_timer_function(const char *path, const char *start,
                               const char *mcr, const char *ir,
                               const char *tcr, const char *enable_bit,
                               const char *disable_bit, const char *last_match,
                               struct timer_scan *out)
{
	FILE *f = fopen(path, "r");
	char raw[SCAN_LINE_MAX], line[SCAN_LINE_MAX];
	int in_block = 0, in_body = 0, lineno = 0;

	memset(out, 0, sizeof(*out));
	if (f == NULL)
		return 0;
	while (fgets(raw, sizeof(raw), f) != NULL) {
		lineno++;
		collapse_spaces(raw, line, sizeof(line));
		strip_comments(line, &in_block);
		strip_string_literals(line);
		if (!in_body) {
			if (strstr(line, start) != NULL) {
				in_body = 1;
				out->found = lineno;
			}
			continue;
		}
		if (strcmp(line, "}") == 0)
			break;
		out->lines++;
		if (is_assignment_of(line, mcr, "0"))
			out->clear_matches = lineno;
		if (is_assignment_to(line, ir))
			out->clear_flags = lineno;
		if (is_assignment_of(line, tcr, "TCR_Counter_Enable"))
			out->counter_enable = lineno;
		if (is_assignment_of(line, tcr, "TCR_Counter_Reset"))
			out->counter_reset = lineno;
		if (contains_identifier(line, enable_bit))
			out->unmask = lineno;
		if (contains_identifier(line, disable_bit))
			out->mask = lineno;
		if (strstr(line, mcr) != NULL && strstr(line, "|=") != NULL)
			out->arm_matches = lineno;
		if (last_match != NULL && is_assignment_to(line, last_match))
			out->last_match_set = lineno;
	}
	fclose(f);
	return out->found != 0;
}

static const char *xmas_path = FW_RXTX_DIR "/xmas.c";
static const char *identify_path = FW_RXTX_DIR "/identify.c";
static struct timer_scan xmas_start, xmas_stop, xmas_loop, identify_start,
                         identify_stop;

MU_TEST(test_the_blink_timers_were_scanned)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "cannot read the blink timers out of %s "
	         "and %s: timer2_start %d, timer2_stop %d, xmas_main %d, "
	         "timer3_start %d, timer3_stop %d", xmas_path, identify_path,
	         xmas_start.found, xmas_stop.found, xmas_loop.found,
	         identify_start.found, identify_stop.found);
	mu_assert_fatal(xmas_start.found && xmas_stop.found &&
	                xmas_loop.found && identify_start.found &&
	                identify_stop.found, msg);

	/* Without this, a scan that lost track of a body would pass every
	 * check below for want of anything to compare. */
	snprintf(msg, sizeof(msg), "the scanned bodies are %d, %d, %d and %d "
	         "lines", xmas_start.lines, xmas_stop.lines, xmas_loop.lines,
	         identify_start.lines);
	mu_assert(xmas_start.lines > 4 && xmas_stop.lines > 2 &&
	          xmas_loop.lines > 8 && identify_start.lines > 4, msg);
}

MU_TEST(test_a_blink_timer_starts_with_no_match_pending)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: timer2_start() does not clear T2MCR and T2IR (%d, %d), so "
	         "a match this mode latched on an earlier run is taken as soon "
	         "as the interrupt is enabled", xmas_path,
	         xmas_start.clear_matches, xmas_start.clear_flags);
	mu_assert_fatal(xmas_start.clear_matches && xmas_start.clear_flags, msg);

	snprintf(msg, sizeof(msg),
	         "%s: timer2_start() clears T2MCR at line %d and T2IR at line "
	         "%d, but starts the counter at line %d and enables the "
	         "interrupt at line %d: both clears have to come first",
	         xmas_path, xmas_start.clear_matches, xmas_start.clear_flags,
	         xmas_start.counter_enable, xmas_start.unmask);
	mu_assert(xmas_start.clear_matches < xmas_start.counter_enable &&
	          xmas_start.clear_flags < xmas_start.counter_enable &&
	          xmas_start.clear_matches < xmas_start.unmask &&
	          xmas_start.clear_flags < xmas_start.unmask, msg);

	/* identify.c clears its flags the same way. It needs no clear of its
	 * match control register on the way in, because its own teardown
	 * clears it and nothing else uses timer3. */
	snprintf(msg, sizeof(msg),
	         "%s: timer3_start() clears T3IR at line %d and starts the "
	         "counter at line %d", identify_path, identify_start.clear_flags,
	         identify_start.counter_enable);
	mu_assert(identify_start.clear_flags != 0 &&
	          identify_start.clear_flags < identify_start.counter_enable,
	          msg);
}

MU_TEST(test_a_blink_timer_leaves_no_match_armed)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: timer2_stop() does not clear T2MCR, so the three matches "
	         "stay armed for whatever powers timer2 up next", xmas_path);
	mu_assert_fatal(xmas_stop.clear_matches != 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: timer2_stop() clears T2MCR at line %d, after it holds the "
	         "counter at 0 (line %d) and masks the interrupt (line %d)",
	         xmas_path, xmas_stop.clear_matches, xmas_stop.counter_reset,
	         xmas_stop.mask);
	mu_assert(xmas_stop.clear_matches < xmas_stop.counter_reset &&
	          xmas_stop.clear_matches < xmas_stop.mask, msg);

	/* identify.c already does it, and this is what says so: it is the only
	 * reason its own entry needs no clear of the match control register. */
	snprintf(msg, sizeof(msg),
	         "%s: timer3_stop() does not clear T3MCR (%d)", identify_path,
	         identify_stop.clear_matches);
	mu_assert(identify_stop.clear_matches != 0, msg);
}

/* The blink times are chosen before the matches are armed, so the handler
 * cannot be in rand() while the loop still is. */
MU_TEST(test_the_first_blink_times_are_chosen_before_the_matches_are_armed)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find xmas_main() arming the matches (%d) "
	         "or writing the last match register (%d)", xmas_path,
	         xmas_loop.arm_matches, xmas_loop.last_match_set);
	mu_assert_fatal(xmas_loop.arm_matches && xmas_loop.last_match_set, msg);

	snprintf(msg, sizeof(msg),
	         "%s: xmas_main() arms the matches at line %d, before it has "
	         "written T2MR2 (line %d)", xmas_path, xmas_loop.arm_matches,
	         xmas_loop.last_match_set);
	mu_assert(xmas_loop.last_match_set < xmas_loop.arm_matches, msg);
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

	/* Scanned once here, so no case below depends on another having run. */
	scan_timer_function(xmas_path, "void timer2_start(void) {", "T2MCR",
	                    "T2IR", "T2TCR", "ISER0_ISE_TIMER2",
	                    "ICER0_ICE_TIMER2", NULL, &xmas_start);
	scan_timer_function(xmas_path, "void timer2_stop(void) {", "T2MCR",
	                    "T2IR", "T2TCR", "ISER0_ISE_TIMER2",
	                    "ICER0_ICE_TIMER2", NULL, &xmas_stop);
	scan_timer_function(xmas_path, "void xmas_main(void) {", "T2MCR",
	                    "T2IR", "T2TCR", "ISER0_ISE_TIMER2",
	                    "ICER0_ICE_TIMER2", "T2MR2", &xmas_loop);
	scan_timer_function(identify_path, "void timer3_start(void)", "T3MCR",
	                    "T3IR", "T3TCR", "ISER0_ISE_TIMER3",
	                    "ICER0_ICE_TIMER3", NULL, &identify_start);
	scan_timer_function(identify_path, "void timer3_stop(void)", "T3MCR",
	                    "T3IR", "T3TCR", "ISER0_ISE_TIMER3",
	                    "ICER0_ICE_TIMER3", NULL, &identify_stop);
	MU_RUN(test_the_blink_timers_were_scanned);
	MU_RUN(test_a_blink_timer_starts_with_no_match_pending);
	MU_RUN(test_a_blink_timer_leaves_no_match_armed);
	MU_RUN(test_the_first_blink_times_are_chosen_before_the_matches_are_armed);
	return mu_report();
}
