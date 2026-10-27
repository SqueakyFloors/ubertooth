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
 *   - An unknown request left `mode` alone forever and spun the loop.
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
 * purpose: update it when one is added or removed. MODE_RX_GENERIC is not one
 * of these, because the dispatch sends it to rx_generic(), which picks
 * between two receives and reaches bt_stream_rx() for one of them; the loop
 * still has to name it, which the last test checks separately. */
#define STREAM_RX_MODES 3

/* Modes that reach bt_stream_rx() through a function of their own rather than
 * from a case of the dispatch, so the scan cannot pair them up but the loop
 * condition must still name them. */
#define INDIRECT_STREAM_MODE "MODE_RX_GENERIC"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Vendor request cases the scan keeps: one per `case UBERTOOTH_...:` that asks
 * for a mode. More than the handler has today, so an added one is counted. */
#define MAX_REQUEST_CASES 32

/* What one mode-asking vendor request case does, in the order it does it. */
struct request_case {
	char request[48];  /* the UBERTOOTH_... label */
	char mode[32];     /* the mode it asks for */
	int line;          /* where the label is */
	int reset;         /* reset_absorbed_mode_state() in the case, else 0 */
	char reset_mode[32]; /* the mode it passes */
	int setup;         /* its first `hop_mode =` or `requested_mode =` */
};

/* Where each thing the scan looks for was found, 0 if it was not. */
struct scan {
	int idle_call;        /* cc2400_idle() in main() */
	int idle_calls;       /* how many */
	int mode_assign;      /* `mode = ...` in main() */
	int mode_assigns;     /* how many */
	int first_case;       /* first `case MODE_...:` in the dispatch */
	int cases;            /* how many */
	int clear_call;       /* usb_queue_init() in main() */
	int clear_calls;      /* how many */
	int irq_hold;         /* usb_irq_hold() in the dispatch */
	int irq_release;      /* usb_irq_release() in the dispatch */
	int idle_fn;          /* where cc2400_idle() is defined */
	int idle_fn_lines;    /* how many lines of it the scan read */
	int idle_fn_mode;     /* `mode = ...` inside cc2400_idle() */
	int idle_fn_request;  /* `requested_mode = ...` inside cc2400_idle() */
	int idle_fn_clear;    /* usb_queue_init() inside cc2400_idle() */

	/* The modes the dispatch runs by calling bt_stream_rx(), and the
	 * condition of the loop that is meant to run them. Room for more modes
	 * than there are today, so an added one is counted rather than
	 * dropped. */
	char stream_modes[STREAM_RX_MODES + 4][32];
	int stream_mode_count;
	char stream_cond[512];
	int stream_cond_line;  /* where its `while (` starts */
	int stream_cond_depth; /* open parentheses still to close */

	int idle_fn_afh_clear; /* clear_afh_map() inside cc2400_idle() */

	/* The modes streams_symbols() names, and where its body was read. */
	char absorbed_modes[STREAM_RX_MODES + 4][32];
	int absorbed_mode_count;
	int absorbed_fn;

	/* Every vendor request case that asks for a mode. */
	struct request_case requests[MAX_REQUEST_CASES];
	int request_count;
};

/* Keep a finished vendor request case, if it is one of the ones these tests
 * are about: a case that asks for a mode, or that resets without asking for
 * one. The handler has far more cases than that. */
static void keep_request_case(struct scan *s, const struct request_case *rc)
{
	if (rc->line == 0 || (rc->mode[0] == '\0' && rc->reset == 0))
		return;
	if (s->request_count == MAX_REQUEST_CASES)
		return;
	s->requests[s->request_count++] = *rc;
}

/* The first MODE_... identifier on a line, "" if it has none. */
static void mode_on_line(const char *line, char *out, size_t size)
{
	const char *mode = strstr(line, "MODE_");
	size_t len = 0;

	out[0] = '\0';
	if (mode == NULL)
		return;
	while (is_ident_char(mode[len]))
		len++;
	if (len < size) {
		memcpy(out, mode, len);
		out[len] = '\0';
	}
}

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
	struct request_case current;
	const char *p;
	char pending[MAX_PENDING][32];
	int pending_count = 0, i;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));
	memset(&current, 0, sizeof(current));

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

			/* The modes a request can be absorbed into, as
			 * streams_symbols() lists them. */
			if (strcmp(fn, "streams_symbols") == 0) {
				char mode[32];

				if (s->absorbed_fn == 0)
					s->absorbed_fn = lineno;
				for (p = line; (p = strstr(p, "MODE_")) != NULL; ) {
					mode_on_line(p, mode, sizeof(mode));
					p += strlen("MODE_");
					if (mode[0] == '\0' ||
					    s->absorbed_mode_count ==
					        (int)ARRAY_LEN(s->absorbed_modes))
						continue;
					snprintf(s->absorbed_modes[
					             s->absorbed_mode_count++],
					         sizeof(s->absorbed_modes[0]), "%s",
					         mode);
				}
			}

			/* What each vendor request case does, in order: the
			 * handler is the one place a mode request is accepted,
			 * and a request absorbed by a running stream is reset
			 * here rather than by the dispatch. */
			if (strcmp(fn, "vendor_request_handler") == 0) {
				if (strstr(line, "case UBERTOOTH_") != NULL) {
					const char *label = strstr(line,
					                           "UBERTOOTH_");
					size_t len = 0;

					keep_request_case(s, &current);
					memset(&current, 0, sizeof(current));
					while (is_ident_char(label[len]))
						len++;
					if (len < sizeof(current.request)) {
						memcpy(current.request, label, len);
						current.request[len] = '\0';
					}
					current.line = lineno;
					continue;
				}
				if (current.line == 0)
					continue;
				if (strstr(line, "reset_absorbed_mode_state(")
				    != NULL && current.reset == 0) {
					current.reset = lineno;
					mode_on_line(line, current.reset_mode,
					             sizeof(current.reset_mode));
				}
				if ((is_assignment_to(line, "hop_mode") ||
				     is_assignment_to(line, "requested_mode")) &&
				    current.setup == 0)
					current.setup = lineno;
				if (is_assignment_to(line, "requested_mode") &&
				    current.mode[0] == '\0')
					mode_on_line(line, current.mode,
					             sizeof(current.mode));
			} else {
				/* out of the handler: the last case is done */
				keep_request_case(s, &current);
				memset(&current, 0, sizeof(current));
			}
			continue;
		}

		if (in_idle_fn) {
			s->idle_fn_lines++;
			if (strstr(line, "clear_afh_map(") != NULL &&
			    s->idle_fn_afh_clear == 0)
				s->idle_fn_afh_clear = lineno;
			if (is_assignment_to(line, "mode") && s->idle_fn_mode == 0)
				s->idle_fn_mode = lineno;
			if (is_assignment_to(line, "requested_mode") &&
			    s->idle_fn_request == 0)
				s->idle_fn_request = lineno;
			if (strstr(line, "usb_queue_init(") != NULL &&
			    s->idle_fn_clear == 0)
				s->idle_fn_clear = lineno;
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
		if (strstr(line, "usb_queue_init(") != NULL) {
			s->clear_calls++;
			if (s->clear_call == 0)
				s->clear_call = lineno;
		}
		if (strstr(line, "usb_irq_hold(") != NULL && s->irq_hold == 0)
			s->irq_hold = lineno;
		if (strstr(line, "usb_irq_release(") != NULL && s->irq_release == 0)
			s->irq_release = lineno;
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
 * Leaving a mode also drops what that mode had queued for the host. The queue
 * was only ever cleared when a mode started, so packets from a capture that
 * had ended were handed to whoever read the endpoint next, before that
 * reader's own mode had started.
 */
MU_TEST(test_leaving_a_mode_drops_what_it_queued)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the dispatch loop calls usb_queue_init() %d times; it "
	         "calls it once, on the way out of whatever mode was running",
	         path, s.clear_calls);
	mu_assert(s.clear_calls == 1, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: usb_queue_init() is called at or after the `mode =` at "
	         "line %d, so the old mode's packets would be dropped after the "
	         "new mode was already recorded", path, s.clear_call,
	         s.mode_assign);
	mu_assert(s.clear_call > 0 && s.mode_assign > 0 &&
	          s.clear_call < s.mode_assign, msg);
}

/*
 * The queue is also read from the USB interrupt, by the poll request, so the
 * clear is held off against it. Every interrupt-driven mode disables that
 * interrupt before it returns, which is why nothing races this today; the
 * hold makes it a property of the dispatch instead.
 */
MU_TEST(test_the_queue_clear_holds_the_usb_interrupt_off)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the dispatch clears the queue at line %d, outside a "
	         "usb_irq_hold() (%d) / usb_irq_release() (%d) pair",
	         path, s.clear_call, s.irq_hold, s.irq_release);
	mu_assert(s.irq_hold > 0 && s.irq_release > 0 && s.clear_call > 0 &&
	          s.irq_hold < s.clear_call && s.clear_call < s.irq_release, msg);
}

/*
 * ...and the drop belongs to the dispatch, not to cc2400_idle(). That reset
 * runs at boot, and bt_generic_le() and bt_le_sync() call it in the middle of
 * a mode, so reaching it does not mean a capture has ended.
 */
MU_TEST(test_cc2400_idle_does_not_drop_queued_packets)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_idle() drops the queue, but it also runs at boot "
	         "and from inside a mode, so the drop would not mean a capture "
	         "had ended", path, s.idle_fn_clear);
	mu_assert(s.idle_fn_clear == 0, msg);
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

	/* And the mode that gets there through rx_generic(), whose symbol-stream
	 * receive returned at once as MODE_AFH's did. */
	snprintf(msg, sizeof(msg),
	         "%s:%d: rx_generic() runs %s through bt_stream_rx(), but its "
	         "loop condition (`%s`) does not name it, so that receive "
	         "returns at once and streams nothing",
	         path, s.stream_cond_line, INDIRECT_STREAM_MODE, s.stream_cond);
	mu_assert(contains_identifier(s.stream_cond, INDIRECT_STREAM_MODE), msg);
}

/*
 * A mode that reaches the loop through a function of its own, and sets the
 * radio up there, must be kept apart from the modes the dispatch sends
 * straight in -- in both directions. Absorbing a request for one into a loop
 * entered for the other skips the dispatch, so no idle reset runs and the
 * running mode's radio setup streams under the new mode's name:
 * MODE_RX_GENERIC enters with modulation = MOD_NONE, which none of the three
 * direct modes wants.
 *
 * So the condition has to name the mode it is running, not only the mode
 * being asked for. The scan checks the pairing rather than the logic, which
 * is the most a text scan of a hardware loop can do.
 */
MU_TEST(test_the_stream_loop_keeps_the_indirect_mode_apart)
{
	char condition[sizeof(s.stream_cond)];
	char needle[64];
	char msg[1024];

	collapse_spaces(s.stream_cond, condition, sizeof(condition));

	snprintf(needle, sizeof(needle), "mode == %s", INDIRECT_STREAM_MODE);
	snprintf(msg, sizeof(msg),
	         "%s:%d: bt_stream_rx()'s loop condition (`%s`) does not say "
	         "`%s`, so it runs %s whatever mode entered it: a request for it "
	         "arriving while another mode streams is absorbed here, the "
	         "dispatch never runs, and rx_generic() never chooses between "
	         "its two receives",
	         path, s.stream_cond_line, condition, needle,
	         INDIRECT_STREAM_MODE);
	mu_assert(contains_identifier(condition, needle), msg);

	snprintf(needle, sizeof(needle), "mode != %s", INDIRECT_STREAM_MODE);
	snprintf(msg, sizeof(msg),
	         "%s:%d: bt_stream_rx()'s loop condition (`%s`) does not say "
	         "`%s`, so the modes the dispatch sends straight in keep it "
	         "running when %s entered it: their request is absorbed here, no "
	         "idle reset runs, and the generic receive's radio setup streams "
	         "under their name",
	         path, s.stream_cond_line, condition, needle,
	         INDIRECT_STREAM_MODE);
	mu_assert(contains_identifier(condition, needle), msg);
}

/*
 * Leaving a mode forgets the AFH channel map, along with the hop mode that was
 * already reset there. Nothing used to clear the map, so a board that had run
 * AFH detection kept hopping on that map through every capture that followed.
 */
MU_TEST(test_leaving_a_mode_forgets_the_afh_map)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_idle() does not clear the AFH map, so a map one "
	         "run found is still there for the next: its mode hops "
	         "adaptively on a channel set found for someone else's piconet",
	         path, s.idle_fn);
	mu_assert(s.idle_fn_afh_clear > 0, msg);
}

/*
 * The modes a request can be absorbed into are the modes bt_stream_rx()
 * streams, and streams_symbols() is where the request handler reads that list.
 * A mode added to the loop and not to the list gets no reset on the way in.
 */
MU_TEST(test_the_absorbed_modes_are_the_streamed_modes)
{
	char msg[1024];
	int i, j;

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find streams_symbols(), so the checks on "
	         "its list would pass without reading anything", path);
	mu_assert(s.absorbed_fn > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: streams_symbols() names %d modes; the dispatch runs %d "
	         "through bt_stream_rx(), and a request for one of those is "
	         "absorbed by the running loop",
	         path, s.absorbed_fn, s.absorbed_mode_count, s.stream_mode_count);
	mu_assert(s.absorbed_mode_count == s.stream_mode_count, msg);

	for (i = 0; i < s.stream_mode_count; i++) {
		int found = 0;

		for (j = 0; j < s.absorbed_mode_count; j++)
			if (strcmp(s.stream_modes[i], s.absorbed_modes[j]) == 0)
				found = 1;
		snprintf(msg, sizeof(msg),
		         "%s:%d: streams_symbols() does not name %s, which the "
		         "dispatch runs through bt_stream_rx(): a request for it "
		         "absorbed by a running stream would keep the old mode's "
		         "hop mode and AFH map",
		         path, s.absorbed_fn, s.stream_modes[i]);
		mu_assert(found, msg);
	}
}

/*
 * Each of those modes is asked for by one vendor request, and that request
 * resets before it sets anything up of its own. The order is the point: the
 * reset clears the hop mode, so a request that sets one -- AFH hopping, or
 * hopping with the piconet -- has to set it after the reset, not before.
 */
MU_TEST(test_an_absorbable_request_resets_before_its_own_setup)
{
	char msg[1024];
	int i, j, checked = 0;

	mu_assert(s.request_count > 0, "the scan found no vendor request cases");

	for (i = 0; i < s.request_count; i++) {
		const struct request_case *rc = &s.requests[i];
		int absorbable = 0;

		for (j = 0; j < s.absorbed_mode_count; j++)
			if (strcmp(rc->mode, s.absorbed_modes[j]) == 0)
				absorbable = 1;
		if (!absorbable)
			continue;
		checked++;

		snprintf(msg, sizeof(msg),
		         "%s:%d: %s asks for %s, which a running stream absorbs, "
		         "without calling reset_absorbed_mode_state(): the mode it "
		         "starts keeps the hop mode and AFH map of the one it "
		         "replaces", path, rc->line, rc->request, rc->mode);
		mu_assert(rc->reset > 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s:%d: %s resets with %s but asks for %s",
		         path, rc->reset, rc->request, rc->reset_mode, rc->mode);
		mu_assert(strcmp(rc->reset_mode, rc->mode) == 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s:%d: %s resets at line %d, after its own setup at line "
		         "%d, so the reset discards what the request just asked for",
		         path, rc->line, rc->request, rc->reset, rc->setup);
		mu_assert(rc->setup > 0 && rc->reset < rc->setup, msg);
	}

	snprintf(msg, sizeof(msg),
	         "%s: the scan checked %d of the %d absorbable mode requests",
	         path, checked, s.absorbed_mode_count);
	mu_assert(checked == s.absorbed_mode_count, msg);
}

/*
 * ...and nothing else resets. A channel set on its own is kept for the next
 * mode, so the request that sets it must not reset anything -- least of all
 * through a helper whose job is mode starts.
 */
MU_TEST(test_setting_a_channel_resets_nothing)
{
	char msg[512];
	int i;

	for (i = 0; i < s.request_count; i++) {
		const struct request_case *rc = &s.requests[i];

		if (rc->mode[0] != '\0' || rc->reset == 0)
			continue;
		snprintf(msg, sizeof(msg),
		         "%s:%d: %s asks for no mode but calls "
		         "reset_absorbed_mode_state() at line %d",
		         path, rc->line, rc->request, rc->reset);
		mu_assert(0, msg);
	}
}

int main(void)
{
	printf("mode dispatch tests\n");
	MU_RUN(test_source_was_scanned);
	MU_RUN(test_mode_is_recorded_once);
	MU_RUN(test_mode_is_recorded_before_the_cases);
	MU_RUN(test_leaving_a_mode_idles_once_first);
	MU_RUN(test_leaving_a_mode_drops_what_it_queued);
	MU_RUN(test_the_queue_clear_holds_the_usb_interrupt_off);
	MU_RUN(test_cc2400_idle_does_not_choose_the_mode);
	MU_RUN(test_cc2400_idle_does_not_drop_queued_packets);
	MU_RUN(test_the_stream_loop_runs_every_mode_dispatched_to_it);
	MU_RUN(test_the_stream_loop_keeps_the_indirect_mode_apart);
	MU_RUN(test_leaving_a_mode_forgets_the_afh_map);
	MU_RUN(test_the_absorbed_modes_are_the_streamed_modes);
	MU_RUN(test_an_absorbable_request_resets_before_its_own_setup);
	MU_RUN(test_setting_a_channel_resets_nothing);
	return mu_report();
}
