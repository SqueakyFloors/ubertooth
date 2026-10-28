/*
 * The LED codes a stopped board blinks in the BR/EDR image, and the sleep they
 * are timed by (firmware/btbr/src/util/fault.c and src/util/debug.c).
 *
 * The LEDs are all a faulted board offers: it answers no USB, and the watchdog
 * resets it a few seconds later. So a code has to be readable, which needs two
 * things this file checks.
 *
 * It has to be distinct. The four processor faults asked for 2, 4, 8 and 8
 * long blinks, so a bus fault and a usage fault blinked the same code.
 *
 * And it has to be blinked before the watchdog resets the board, because a
 * count cut off part-way is a different count.
 *
 * Underneath both, active_msleep() has to sleep. It was an empty loop over an
 * ordinary counter -- 250 decrements of a register for a 500 ms request, and a
 * compiler is free to delete it -- which left every code the same flicker. It
 * now counts core clocks on the system timer.
 *
 * Both files are hardware code: they write the board's LEDs and the system
 * timer's registers, so neither can run here and both are read as text, the
 * way test_cc2400_wait.c and test_btbr_arithmetic.c read the files they check.
 *
 * This file is part of Project Ubertooth and is released under the terms of the
 * GPL. Refer to COPYING for more information.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"

static const char *fault_path = FW_BTBR_DIR "/src/util/fault.c";
static const char *debug_path = FW_BTBR_DIR "/src/util/debug.c";

/* The value of a plain decimal #define in a source file. -1 when the file has
 * no such line, which every test below reports rather than passing. */
static long read_define(const char *path, const char *name)
{
	char line[SCAN_LINE_MAX];
	FILE *f = fopen(path, "r");
	long value = -1;

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p;

		if (strstr(line, "#define") == NULL)
			continue;
		p = strstr(line, name);
		if (p == NULL)
			continue;
		if ((p != line && is_ident_char(p[-1])) ||
		    is_ident_char(p[strlen(name)]))
			continue;
		value = strtol(p + strlen(name), NULL, 10);
		break;
	}
	fclose(f);
	return value;
}

/* ---- the codes ---------------------------------------------------------- */

#define CODE_COUNT 5

static const char *code_names[CODE_COUNT] = {
	"FAULT_CODE_PANIC",
	"FAULT_CODE_HARD",
	"FAULT_CODE_MEM",
	"FAULT_CODE_BUS",
	"FAULT_CODE_USAGE",
};

static long codes[CODE_COUNT];
static long blink_ms;
static long gap_ms;

static void read_codes(void)
{
	int i;

	for (i = 0; i < CODE_COUNT; i++)
		codes[i] = read_define(fault_path, code_names[i]);
	blink_ms = read_define(fault_path, "FAULT_BLINK_MS");
	gap_ms = read_define(fault_path, "FAULT_GAP_MS");
}

MU_TEST(test_every_code_was_read)
{
	char msg[256];
	int i;

	for (i = 0; i < CODE_COUNT; i++) {
		snprintf(msg, sizeof(msg), "%s: %s not found", fault_path,
		         code_names[i]);
		mu_assert_fatal(codes[i] > 0, msg);
	}
	snprintf(msg, sizeof(msg),
	         "%s: FAULT_BLINK_MS and FAULT_GAP_MS not found (read %ld and "
	         "%ld)", fault_path, blink_ms, gap_ms);
	mu_assert_fatal(blink_ms > 0 && gap_ms > 0, msg);
}

/* No two paths blink the same number of blinks: the count is the whole of what
 * the LEDs say. */
MU_TEST(test_no_two_paths_share_a_code)
{
	char msg[256];
	int i, j;

	for (i = 0; i < CODE_COUNT; i++) {
		for (j = i + 1; j < CODE_COUNT; j++) {
			snprintf(msg, sizeof(msg),
			         "%s: %s and %s both blink %ld times",
			         fault_path, code_names[i], code_names[j],
			         codes[i]);
			mu_assert(codes[i] != codes[j], msg);
		}
	}
}

/* The four processor faults keep the other image's numbers, so a code read off
 * a board means the same thing whichever image it was running. */
MU_TEST(test_the_fault_codes_match_the_other_image)
{
	mu_assert_int_eq(1, (int)codes[0]);  /* panic, which that image has not */
	mu_assert_int_eq(2, (int)codes[1]);  /* hard fault */
	mu_assert_int_eq(3, (int)codes[2]);  /* memory management fault */
	mu_assert_int_eq(4, (int)codes[3]);  /* bus fault */
	mu_assert_int_eq(5, (int)codes[4]);  /* usage fault */
}

/*
 * Every code is blinked right through before the watchdog resets the board.
 * One blink is on for FAULT_BLINK_MS and off for the same, and a gap separates
 * one repeat from the next.
 *
 * The dump that runs before the blinking takes some of the same budget, so
 * this is the floor rather than a promise: a code that does not fit even
 * without the dump cannot be read at all.
 */
MU_TEST(test_every_code_fits_inside_the_watchdog_timeout)
{
	char msg[256];
	int i;

	/* A code that was not read is negative, which would pass below. */
	mu_assert_fatal(blink_ms > 0 && gap_ms > 0, "the timings were not read");

	for (i = 0; i < CODE_COUNT; i++) {
		long ms = codes[i] * 2 * blink_ms + gap_ms;

		snprintf(msg, sizeof(msg), "%s was not read", code_names[i]);
		mu_assert_fatal(codes[i] > 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s: %s takes %ld ms to blink, and the watchdog resets "
		         "the board after %d ms", fault_path, code_names[i], ms,
		         UBERTOOTH_WATCHDOG_TIMEOUT_MS);
		mu_assert(ms < UBERTOOTH_WATCHDOG_TIMEOUT_MS, msg);
	}
}

/* A blink has to be long enough to count by eye. Anything near the old
 * sub-microsecond timing reads as a steady LED. */
MU_TEST(test_a_blink_is_long_enough_to_see)
{
	char msg[256];

	snprintf(msg, sizeof(msg), "%s: a %ld ms blink is too short to count",
	         fault_path, blink_ms);
	mu_assert(blink_ms >= 100, msg);

	snprintf(msg, sizeof(msg),
	         "%s: a %ld ms gap does not separate one repeat from the next "
	         "(a blink is %ld ms)", fault_path, gap_ms, blink_ms);
	mu_assert(gap_ms >= 2 * blink_ms, msg);
}

/* ---- which path blinks which code --------------------------------------- */

struct handler {
	const char *name;  /* the function in fault.c */
	const char *code;  /* the code it must blink */
	int found;         /* where it is defined */
	int blinks;        /* where it calls blink_fault_code() with that code */
	int other;         /* a flash_leds() call of its own */
};

static struct handler handlers[] = {
	{ "die",                  "FAULT_CODE_PANIC", 0, 0, 0 },
	{ "HardFault_Handler",    "FAULT_CODE_HARD",  0, 0, 0 },
	{ "MemManagement_Handler", "FAULT_CODE_MEM",  0, 0, 0 },
	{ "BusFault_Handler",     "FAULT_CODE_BUS",   0, 0, 0 },
	{ "UsageFault_Handler",   "FAULT_CODE_USAGE", 0, 0, 0 },
};

#define HANDLER_COUNT (sizeof(handlers) / sizeof(handlers[0]))

static int fault_opened;

static void scan_fault(void)
{
	char line[SCAN_LINE_MAX];
	int lineno = 0, in_block = 0;
	size_t current = HANDLER_COUNT;
	FILE *f = fopen(fault_path, "r");

	if (f == NULL)
		return;
	fault_opened = 1;

	while (fgets(line, sizeof(line), f) != NULL) {
		size_t i;

		lineno++;
		strip_comments(line, &in_block);
		strip_string_literals(line);

		if (current < HANDLER_COUNT) {
			if (calls_identifier(line, "blink_fault_code") &&
			    contains_identifier(line, handlers[current].code))
				handlers[current].blinks = lineno;
			else if (calls_identifier(line, "flash_leds"))
				handlers[current].other = lineno;
			if (line[0] == '}')
				current = HANDLER_COUNT;
			continue;
		}
		/* A definition starts in column 1 and is not a statement. The
		 * name is matched rather than read off the line, because die()
		 * carries an attribute in front of it and the first '(' on its
		 * line belongs to that. */
		if (line[0] == '\0' || line[0] == '#' || line[0] == '}' ||
		    strchr(line, ';') != NULL)
			continue;
		for (i = 0; i < HANDLER_COUNT; i++) {
			if (calls_identifier(line, handlers[i].name)) {
				handlers[i].found = lineno;
				current = i;
				break;
			}
		}
	}
	fclose(f);
}

MU_TEST(test_each_path_blinks_its_own_code)
{
	char msg[512];
	size_t i;

	snprintf(msg, sizeof(msg), "cannot open %s to scan", fault_path);
	mu_assert_fatal(fault_opened, msg);

	for (i = 0; i < HANDLER_COUNT; i++) {
		snprintf(msg, sizeof(msg),
		         "%s: the scan did not find %s(), so the check below "
		         "would pass without reading anything", fault_path,
		         handlers[i].name);
		mu_assert(handlers[i].found > 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s:%d: %s() does not end in "
		         "blink_fault_code(%s)", fault_path, handlers[i].found,
		         handlers[i].name, handlers[i].code);
		mu_assert(handlers[i].blinks > 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s:%d: %s() blinks the LEDs itself as well as through "
		         "blink_fault_code(), so what it shows is not the code "
		         "alone", fault_path, handlers[i].other,
		         handlers[i].name);
		mu_assert(handlers[i].other == 0, msg);
	}
}

/* ---- the sleep the codes are timed by ----------------------------------- */

struct sleep_scan {
	int opened;
	int found;        /* where active_msleep() is defined */
	int reload;       /* the system timer's reload value is set */
	int enable;       /* the timer is started */
	int poll;         /* the count flag is polled */
	int empty_loop;   /* a loop with no body inside the function */
};

static struct sleep_scan sleep_scan;

/* A `for (...);` or `while (...);` with nothing in it: the shape the old sleep
 * had, which a compiler may remove. A loop whose body is a lone `;` on the
 * next line is the same thing, and the poll below is written that way, so only
 * the one-line form counts here. */
static int is_empty_loop(const char *line)
{
	const char *p = strstr(line, "for (");

	if (p == NULL)
		p = strstr(line, "while (");
	if (p == NULL)
		return 0;
	/* ");" at the end of the line, with nothing after it. */
	p = strstr(p, ");");
	if (p == NULL)
		return 0;
	for (p += 2; *p != '\0'; p++)
		if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
			return 0;
	return 1;
}

static void scan_sleep(void)
{
	char line[SCAN_LINE_MAX];
	int lineno = 0, in_block = 0, inside = 0;
	FILE *f = fopen(debug_path, "r");

	memset(&sleep_scan, 0, sizeof(sleep_scan));
	if (f == NULL)
		return;
	sleep_scan.opened = 1;

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		strip_string_literals(line);

		if (inside) {
			if (is_assignment_to(line, "STRELOAD"))
				sleep_scan.reload = lineno;
			if (is_assignment_to(line, "STCTRL") &&
			    contains_identifier(line, "STCTRL_ENABLE"))
				sleep_scan.enable = lineno;
			if (contains_identifier(line, "STCTRL_COUNTFLAG"))
				sleep_scan.poll = lineno;
			if (is_empty_loop(line) && sleep_scan.empty_loop == 0)
				sleep_scan.empty_loop = lineno;
			if (line[0] == '}')
				inside = 0;
			continue;
		}
		if (line[0] == '\0' || line[0] == '#' || line[0] == '}' ||
		    strchr(line, ';') != NULL)
			continue;
		if (calls_identifier(line, "active_msleep")) {
			inside = 1;
			sleep_scan.found = lineno;
		}
	}
	fclose(f);
}

MU_TEST(test_the_sleep_waits_on_the_system_timer)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "cannot open %s to scan", debug_path);
	mu_assert_fatal(sleep_scan.opened, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find active_msleep()'s definition, so "
	         "the checks below would pass without reading anything",
	         debug_path);
	mu_assert_fatal(sleep_scan.found > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: active_msleep() sets no reload value on the system "
	         "timer, so it cannot be counting a known length of time",
	         debug_path, sleep_scan.found);
	mu_assert(sleep_scan.reload > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: active_msleep() does not start the system timer",
	         debug_path, sleep_scan.found);
	mu_assert(sleep_scan.enable > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: active_msleep() does not wait on the timer's count "
	         "flag, so nothing makes it wait for the time it was asked for",
	         debug_path, sleep_scan.found);
	mu_assert(sleep_scan.poll > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: active_msleep() still has a loop with an empty body; "
	         "that was the old sleep, and a compiler may remove it",
	         debug_path, sleep_scan.empty_loop);
	mu_assert(sleep_scan.empty_loop == 0, msg);
}

int main(void)
{
	printf("btbr fault blink code tests\n");
	read_codes();
	scan_fault();
	scan_sleep();
	MU_RUN(test_every_code_was_read);
	MU_RUN(test_no_two_paths_share_a_code);
	MU_RUN(test_the_fault_codes_match_the_other_image);
	MU_RUN(test_every_code_fits_inside_the_watchdog_timeout);
	MU_RUN(test_a_blink_is_long_enough_to_see);
	MU_RUN(test_each_path_blinks_its_own_code);
	MU_RUN(test_the_sleep_waits_on_the_system_timer);
	return mu_report();
}
