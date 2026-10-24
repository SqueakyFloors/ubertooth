/*
 * Guard for the mode dispatch in firmware/bluetooth_rxtx/bluetooth_rxtx.c
 * main().
 *
 * The dispatch is the one place a mode change happens, and it has one shape:
 *
 *     next_mode = requested_mode;
 *     if (next_mode == mode) continue;
 *     prev_mode = mode;
 *     if (prev_mode != MODE_IDLE) cc2400_idle();   // leave the old mode
 *     mode = next_mode;                            // record the new one
 *     switch (next_mode) { case MODE_...: <run it>; }
 *
 * Three things went wrong when the reset and the bookkeeping were spread
 * across the cases instead (issue #209):
 *
 *   - cc2400_idle() -- which clears the LEDs and the Ubertooth One amplifier
 *     and puts the channel back to 2441 MHz and the squelch back to -120 --
 *     ran from a `case MODE_IDLE` of its own, so it ran only if MODE_IDLE was
 *     still the request when the dispatch was reached. A mode loop that ended
 *     for its own reason, or a second request arriving in handle_usb() first,
 *     carried the old mode's state into the new mode.
 *   - A case that ran a mode loop without recording `mode` left
 *     requested_mode == mode when the mode stopped, so no reset ran at all.
 *     The LED spectrum analyzer, spectrum analyzer, BLE promiscuous and BLE
 *     faux slave cases were all like that.
 *   - An unknown request left `mode` alone for good and spun the loop.
 *
 * Recording `mode` once, before the switch, and idling once, before that,
 * makes all three unreachable -- as long as the shape holds. main() is
 * hardware code that cannot be run here, so this test scans the source, the
 * way test_frend.c scans the transmit paths for hard-coded PA levels.
 */
#include <stdio.h>
#include <string.h>

#include "minunit.h"

/* How many `case MODE_...:` labels the dispatch has today, so a scan that
 * silently matched nothing cannot pass. Update it when a mode is added or
 * removed. */
#define DISPATCH_CASES 19

/*
 * Strip C comments from a line, in place, carrying block-comment state across
 * lines. The source explains all of the above in prose, so a scan that read
 * the comments would find whatever it was looking for.
 */
static void strip_comments(char *line, int *in_block)
{
	char *r = line, *w = line;

	while (*r != '\0') {
		if (*in_block) {
			if (r[0] == '*' && r[1] == '/') {
				*in_block = 0;
				r += 2;
			} else {
				r++;
			}
			continue;
		}
		if (r[0] == '/' && r[1] == '*') {
			*in_block = 1;
			r += 2;
			continue;
		}
		if (r[0] == '/' && r[1] == '/')
			break;
		*w++ = *r++;
	}
	*w = '\0';
}

/*
 * Does the text assign to `name`? Looks for "name = " with no identifier
 * character in front, so `mode` does not match inside `requested_mode`.
 */
static int is_assignment_to(const char *text, const char *name)
{
	char needle[64];
	const char *p = text;

	snprintf(needle, sizeof(needle), "%s =", name);
	while ((p = strstr(p, needle)) != NULL) {
		char prev = (p == text) ? ' ' : p[-1];
		/* "==" is a comparison, not an assignment */
		if (p[strlen(needle)] != '=' &&
		    (prev == ' ' || prev == '\t' || prev == '(' || prev == ';'))
			return 1;
		p += strlen(needle);
	}
	return 0;
}

/* Where each thing the scan looks for was found, 0 if it was not. */
struct scan {
	int idle_call;        /* cc2400_idle() in main() */
	int idle_calls;       /* how many */
	int mode_assign;      /* `mode = ...` in main() */
	int mode_assigns;     /* how many */
	int first_case;       /* first `case MODE_...:` in the dispatch */
	int cases;            /* how many */
	int idle_fn_mode;     /* `mode = ...` inside cc2400_idle() */
	int idle_fn_request;  /* `requested_mode = ...` inside cc2400_idle() */
};

static int scan_source(const char *path, struct scan *s)
{
	char line[512];
	int lineno = 0, in_block = 0, in_main = 0, in_loop = 0, in_idle_fn = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);

		if (!in_main && !in_idle_fn) {
			if (strstr(line, "void cc2400_idle(") != NULL)
				in_idle_fn = 1;
			else if (strstr(line, "int main(") != NULL)
				in_main = 1;
			continue;
		}

		if (in_idle_fn) {
			if (is_assignment_to(line, "mode") && s->idle_fn_mode == 0)
				s->idle_fn_mode = lineno;
			if (is_assignment_to(line, "requested_mode") &&
			    s->idle_fn_request == 0)
				s->idle_fn_request = lineno;
			/* a closing brace in column 1 ends the function */
			if (line[0] == '}')
				in_idle_fn = 0;
			continue;
		}

		/* Everything below is about the dispatch, so skip main()'s setup --
		 * which calls cc2400_idle() once at boot, before any mode has
		 * run. */
		if (!in_loop) {
			if (strstr(line, "while (1)") != NULL)
				in_loop = 1;
			continue;
		}

		if (strstr(line, "cc2400_idle(") != NULL) {
			s->idle_calls++;
			if (s->idle_call == 0)
				s->idle_call = lineno;
		}
		if (is_assignment_to(line, "mode")) {
			s->mode_assigns++;
			if (s->mode_assign == 0)
				s->mode_assign = lineno;
		}
		if (strstr(line, "case MODE_") != NULL) {
			s->cases++;
			if (s->first_case == 0)
				s->first_case = lineno;
		}
	}

	fclose(f);
	return 1;
}

static struct scan s;
static const char *path = FW_RXTX_DIR "/bluetooth_rxtx.c";

MU_TEST(test_source_was_scanned)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
	mu_assert(scan_source(path, &s), msg);

	/* The counts below are what makes a scan that matched nothing fail. */
	mu_assert_int_eq(DISPATCH_CASES, s.cases);
}

/* `mode` is recorded in exactly one place, so no case can forget it. */
MU_TEST(test_mode_is_recorded_once)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: main() assigns `mode` %d times; the dispatch records it "
	         "once, before the switch, so that a case cannot forget it and "
	         "an unknown request cannot spin the loop (issue #209)",
	         path, s.mode_assigns);
	mu_assert(s.mode_assigns == 1, msg);
}

/* ...and it is recorded before the cases, not inside one of them. */
MU_TEST(test_mode_is_recorded_before_the_cases)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: the one `mode =` in main() is at or after the first "
	         "`case MODE_...:` (line %d), so it is inside a case rather than "
	         "covering every mode",
	         path, s.mode_assign, s.first_case);
	mu_assert(s.mode_assign > 0 && s.first_case > 0 &&
	          s.mode_assign < s.first_case, msg);
}

/* Leaving a mode runs the idle reset, once, before the new mode starts. */
MU_TEST(test_leaving_a_mode_idles_once_first)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: main() calls cc2400_idle() %d times; the dispatch calls it "
	         "once, on the way out of whatever mode was running (issue #209)",
	         path, s.idle_calls);
	mu_assert(s.idle_calls == 1, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_idle() is called at or after the `mode =` at "
	         "line %d, so the old mode's reset would run after the new mode "
	         "was already recorded",
	         path, s.idle_call, s.mode_assign);
	mu_assert(s.idle_call > 0 && s.mode_assign > 0 &&
	          s.idle_call < s.mode_assign, msg);
}

/*
 * cc2400_idle() resets the board and decides nothing. It used to end with
 * `mode = MODE_IDLE; requested_mode = MODE_IDLE;`, which dropped a mode
 * request that arrived while the teardown ran -- the host's vendor request had
 * succeeded, so it believed the mode had started while the board stayed idle.
 */
MU_TEST(test_cc2400_idle_does_not_choose_the_mode)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_idle() assigns `mode`; the dispatch owns it "
	         "(issue #209)", path, s.idle_fn_mode);
	mu_assert(s.idle_fn_mode == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_idle() assigns `requested_mode`, so a request "
	         "that arrives while the teardown runs is dropped and the board "
	         "silently stays idle (issue #209)", path, s.idle_fn_request);
	mu_assert(s.idle_fn_request == 0, msg);
}

int main(void)
{
	printf("mode dispatch tests\n");
	MU_RUN(test_source_was_scanned);
	MU_RUN(test_mode_is_recorded_once);
	MU_RUN(test_mode_is_recorded_before_the_cases);
	MU_RUN(test_leaving_a_mode_idles_once_first);
	MU_RUN(test_cc2400_idle_does_not_choose_the_mode);
	return mu_report();
}
