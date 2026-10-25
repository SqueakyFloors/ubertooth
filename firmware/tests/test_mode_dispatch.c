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
 * across the cases instead:
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
 *
 * The last test is about the other half of the handshake: a dispatched mode
 * only does something if the loop it lands in runs for that mode. Several
 * cases share bt_stream_rx(), whose loop runs `while (requested_mode ==
 * <one of a list of modes>)`, and MODE_AFH was dispatched to it without being
 * in the list -- so the function returned at once, the dispatch sat with
 * `mode == MODE_AFH`, and ubertooth-afh's hopping mode never received
 * anything. The scan pairs the dispatch with the list.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

/* How many `case MODE_...:` labels the dispatch has today, so a scan that
 * silently matched nothing cannot pass. Update it when a mode is added or
 * removed. */
#define DISPATCH_CASES 19

/* How many modes the dispatch runs by calling bt_stream_rx() itself. Same
 * purpose: update it when one is added or removed. A mode the dispatch sends
 * to a function that calls bt_stream_rx() in turn -- MODE_RX_GENERIC, through
 * rx_generic() -- is not one of these: that function sets its mode up and the
 * loop is not asked to recognise it. */
#define STREAM_RX_MODES 3

/* Where each thing the scan looks for was found, 0 if it was not. */
struct scan {
	int idle_call;        /* cc2400_idle() in main() */
	int idle_calls;       /* how many */
	int mode_assign;      /* `mode = ...` in main() */
	int mode_assigns;     /* how many */
	int first_case;       /* first `case MODE_...:` in the dispatch */
	int cases;            /* how many */
	int idle_fn;          /* where cc2400_idle() is defined */
	int idle_fn_lines;    /* how many lines of it the scan read */
	int idle_fn_mode;     /* `mode = ...` inside cc2400_idle() */
	int idle_fn_request;  /* `requested_mode = ...` inside cc2400_idle() */

	/* The modes the dispatch runs by calling bt_stream_rx(), and the
	 * condition of the loop that is meant to run them. Room for more modes
	 * than there are today, so an added one is counted rather than
	 * dropped. */
	char stream_modes[STREAM_RX_MODES + 4][32];
	int stream_mode_count;
	char stream_cond[512];
	int stream_cond_line;  /* where its `while (` starts */
	int stream_cond_depth; /* open parentheses still to close */
};

/* Remember a mode the dispatch runs through bt_stream_rx(). */
static void add_stream_mode(struct scan *s, const char *mode)
{
	int i;

	for (i = 0; i < s->stream_mode_count; i++)
		if (strcmp(s->stream_modes[i], mode) == 0)
			return;
	if (s->stream_mode_count == (int)(sizeof(s->stream_modes) /
	                                  sizeof(s->stream_modes[0])))
		return;
	snprintf(s->stream_modes[s->stream_mode_count++],
	         sizeof(s->stream_modes[0]), "%s", mode);
}

/*
 * Copy bt_stream_rx()'s loop condition into s->stream_cond, following the
 * parentheses across lines: it is a list of modes and spans several. Only the
 * first `while` of the function is read -- the inner `while (!rx_tc)` waits
 * for DMA, not for a mode.
 */
static void capture_while_condition(const char *line, int lineno,
                                    struct scan *s)
{
	const char *p;
	size_t len = strlen(s->stream_cond);

	if (s->stream_cond_line == 0) {
		p = strstr(line, "while");
		if (p == NULL)
			return;
		p = strchr(p, '(');
		if (p == NULL)
			return;
		s->stream_cond_line = lineno;
		s->stream_cond_depth = 1;
		p++;
	} else if (s->stream_cond_depth == 0) {
		return;
	} else {
		p = line;
	}

	for (; *p != '\0' && s->stream_cond_depth > 0; p++) {
		if (*p == '(') {
			s->stream_cond_depth++;
		} else if (*p == ')') {
			s->stream_cond_depth--;
			if (s->stream_cond_depth == 0)
				break;
		}
		if (len + 1 < sizeof(s->stream_cond))
			s->stream_cond[len++] = *p;
	}
	s->stream_cond[len] = '\0';
}

/* Case labels seen since the last `break;`: a fall-through group runs one
 * loop for all of them. */
#define MAX_PENDING 8

static int scan_source(const char *path, struct scan *s)
{
	char line[512];
	int lineno = 0, in_block = 0, in_main = 0, in_loop = 0, in_idle_fn = 0;
	char fn[64] = "", name[64];
	char pending[MAX_PENDING][32];
	int pending_count = 0, i;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);

		if (!in_main && !in_idle_fn) {
			if (strstr(line, "cc2400_idle(") != NULL &&
			    strstr(line, ";") == NULL) {
				in_idle_fn = 1;
				s->idle_fn = lineno;
				continue;
			}
			if (strstr(line, "int main(") != NULL) {
				in_main = 1;
				continue;
			}

			/* A definition starts in column 1 and is not a statement:
			 * remember whose body the lines below are. */
			if (line[0] != '\0' && !isspace((unsigned char)line[0]) &&
			    line[0] != '#' && line[0] != '}' &&
			    strchr(line, ';') == NULL &&
			    call_name(line, name, sizeof(name)))
				snprintf(fn, sizeof(fn), "%s", name);

			if (strcmp(fn, "bt_stream_rx") == 0)
				capture_while_condition(line, lineno, s);
			continue;
		}

		if (in_idle_fn) {
			s->idle_fn_lines++;
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
			const char *mode = strstr(line, "MODE_");
			size_t len = 0;

			s->cases++;
			if (s->first_case == 0)
				s->first_case = lineno;

			while (is_ident_char(mode[len]))
				len++;
			if (pending_count < MAX_PENDING && len < sizeof(pending[0])) {
				memcpy(pending[pending_count], mode, len);
				pending[pending_count][len] = '\0';
				pending_count++;
			}
			continue;
		}

		/* What a case group runs. The group is every label since the last
		 * `break;`, so a fall-through counts for all of its modes. */
		if (call_name(line, name, sizeof(name)) &&
		    strcmp(name, "bt_stream_rx") == 0)
			for (i = 0; i < pending_count; i++)
				add_stream_mode(s, pending[i]);
		if (strstr(line, "break;") != NULL)
			pending_count = 0;
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

	/* These counts are what makes a scan that matched nothing fail, rather
	 * than passing because it never found what it was looking for. */
	mu_assert_int_eq(DISPATCH_CASES, s.cases);
	mu_assert(s.idle_fn > 0, "the scan did not find cc2400_idle()'s "
	                         "definition, so the checks on its body below "
	                         "would pass without reading anything");
	mu_assert(s.idle_fn_lines > 20, "cc2400_idle()'s body scanned as fewer "
	                                "than 20 lines, so the scan lost track "
	                                "of where the function ends");
}

/* `mode` is recorded in exactly one place, so no case can forget it. */
MU_TEST(test_mode_is_recorded_once)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: main() assigns `mode` %d times; the dispatch records it "
	         "once, before the switch, so that a case cannot forget it and "
	         "an unknown request cannot spin the loop",
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
	         "once, on the way out of whatever mode was running",
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
	         "%s:%d: cc2400_idle() assigns `mode`; the dispatch owns it",
	         path, s.idle_fn_mode);
	mu_assert(s.idle_fn_mode == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_idle() assigns `requested_mode`, so a request "
	         "that arrives while the teardown runs is dropped and the board "
	         "silently stays idle", path, s.idle_fn_request);
	mu_assert(s.idle_fn_request == 0, msg);
}

/*
 * A dispatched mode has to be one the loop it lands in runs for. bt_stream_rx()
 * is shared by several cases and picks them out of requested_mode by name, so
 * a case added to it without being added to the condition gets a mode that
 * returns at once and streams nothing -- MODE_AFH, for ubertooth-afh.
 */
MU_TEST(test_the_stream_loop_runs_every_mode_dispatched_to_it)
{
	char msg[1024];
	int i;

	snprintf(msg, sizeof(msg),
	         "%s: the scan found %d of the %d modes the dispatch runs "
	         "through bt_stream_rx(); update STREAM_RX_MODES when one is "
	         "added or removed", path, s.stream_mode_count, STREAM_RX_MODES);
	mu_assert(s.stream_mode_count == STREAM_RX_MODES, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find bt_stream_rx()'s loop condition, so "
	         "the check below would pass without reading anything", path);
	mu_assert(s.stream_cond_line > 0 && s.stream_cond[0] != '\0', msg);

	for (i = 0; i < s.stream_mode_count; i++) {
		snprintf(msg, sizeof(msg),
		         "%s:%d: the dispatch runs %s through bt_stream_rx(), but "
		         "its loop condition (`%s`) does not name it, so the body "
		         "never runs: the function returns at once and the board "
		         "streams nothing in that mode",
		         path, s.stream_cond_line, s.stream_modes[i], s.stream_cond);
		mu_assert(contains_identifier(s.stream_cond, s.stream_modes[i]),
		          msg);
	}
}

int main(void)
{
	printf("mode dispatch tests\n");
	MU_RUN(test_source_was_scanned);
	MU_RUN(test_mode_is_recorded_once);
	MU_RUN(test_mode_is_recorded_before_the_cases);
	MU_RUN(test_leaving_a_mode_idles_once_first);
	MU_RUN(test_cc2400_idle_does_not_choose_the_mode);
	MU_RUN(test_the_stream_loop_runs_every_mode_dispatched_to_it);
	return mu_report();
}
