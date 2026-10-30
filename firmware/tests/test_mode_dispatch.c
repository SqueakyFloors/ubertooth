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
 *     faux peripheral cases were all like that.
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
#define DISPATCH_CASES 20

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
	int forget;        /* forget_hop_state() in the case, else 0 */
	int ego_mode_set;  /* its `ego_mode =` */
	int restart;       /* its `ego_restart =` */
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
	/* The second pair, around the mode record and the copy it applies. */
	int irq_hold_2;
	int irq_release_2;
	int idle_fn;          /* where cc2400_idle() is defined */
	int idle_fn_lines;    /* how many lines of it the scan read */
	int idle_fn_mode;     /* `mode = ...` inside cc2400_idle() */
	int idle_fn_request;  /* `requested_mode = ...` inside cc2400_idle() */
	int idle_fn_clear;    /* usb_queue_init() inside cc2400_idle() */
	int idle_fn_ego_mode; /* `ego_mode = ...` inside cc2400_idle() */
	int iir_call;         /* rssi_iir_reset() in the dispatch */
	int iir_calls;        /* how many */
	int idle_fn_iir;      /* rssi_iir_reset() inside cc2400_idle() */
	/* reset_absorbed_mode_state(): where it is defined, and whether it
	 * clears the channel averages. */
	int absorbed_reset_fn;
	int absorbed_reset_fn_iir;
	int absorbed_reset_fn_mod;  /* where it puts the modulation back */

	int ego_reset_call;   /* ego_access_code_reset() in the dispatch */
	int ego_reset_calls;  /* how many */
	/* UBERTOOTH_SET_AFHMAP: its case, the copy it keeps for the mode
	 * request it followed, and the guard that keeps one only while a mode
	 * start is still to come. UBERTOOTH_CLEAR_AFHMAP: its case and the
	 * copy it drops. */
	int afhmap_case;
	int afhmap_keep;
	int afhmap_guard;
	int afhmap_clear_case;
	int afhmap_clear_drop;
	int stated_call;      /* apply_stated_settings() in the dispatch */
	int stated_calls;     /* how many */
	/* apply_stated_settings(): where it is defined, where it clears the
	 * copy, and where it first reads what the copy holds. */
	int stated_fn;
	int stated_fn_clear;
	int stated_fn_clear_last;
	int stated_fn_clears;
	int stated_fn_first_use;
	int stated_fn_match;  /* where it compares the two modes */
	int stated_fn_pending; /* where it asks whether that mode is still asked for */

	/* The modulation request, which the host sends after its mode request:
	 * the copy it keeps for that mode, and the guard that keeps it to a
	 * mode that is still to start. */
	int setmod_case;
	int setmod_stated;
	int setmod_guard;
	/* The jam mode request. The host sends it after its mode request, as it
	 * does the modulation, so it keeps a copy for that mode (jam_stated)
	 * under the same guard (jam_guard). */
	int jam_case;
	int jam_stated;
	int jam_guard;
	/* program_stated_modulation(), which is what lets a modulation arrive
	 * after the mode request: where it is defined, and where the capture
	 * loop calls it. What it does there is the model's to check. */
	int mod_fn;
	int stream_mod_call;
	/* What the teardown resets. The modulation goes back in cc2400_idle(),
	 * but the jam mode does not: cc2400_idle() runs in the middle of the
	 * promiscuous mode, so the jam mode is reset in the dispatch instead
	 * (dispatch_jam), the one place a capture has actually ended. */
	int idle_fn_modulation;
	int idle_fn_manand;   /* cc2400_set(MANAND, ...) inside cc2400_idle() */
	int dispatch_jam;     /* jam_mode reset in the dispatch teardown */

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

	/* Inside set_channel_request(), which the channel set request calls:
	 * where it writes the channel the board tunes to, where it asks whether
	 * LE follow mode is running, and where it sets the carrier-sense
	 * threshold. */
	int channel_assign;
	int channel_follow_guard;
	int channel_cs_set;
};

/* Keep a finished vendor request case, if it is one of the ones these tests
 * are about: a case that asks for a mode, or that resets without asking for
 * one. The handler has far more cases than that. */
static void keep_request_case(struct scan *s, const struct request_case *rc)
{
	if (rc->line == 0 || (rc->mode[0] == '\0' && rc->reset == 0 &&
	                      rc->forget == 0))
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
	int in_absorbed_fn = 0;
	int in_stated_fn = 0;
	char in_case = 0;   /* 'm' or 'j' inside the two settings' cases */
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

		/* The AFH map request's case, and the copy it keeps. Scanned
		 * here rather than through the request_case machinery below,
		 * which only follows the cases that ask for a mode. */
		if (s->afhmap_case == 0 &&
		    strstr(line, "case UBERTOOTH_SET_AFHMAP:") != NULL)
			s->afhmap_case = lineno;
		if (s->afhmap_case > 0 && s->afhmap_keep == 0 &&
		    strstr(line, "stated_for_starting_mode(STATED_AFH_MAP)") != NULL)
			s->afhmap_keep = lineno;
		if (s->afhmap_case > 0 && s->afhmap_keep == 0 &&
		    s->afhmap_guard == 0 &&
		    strstr(line, "if (requested_mode != mode)") != NULL)
			s->afhmap_guard = lineno;
		if (s->afhmap_clear_case == 0 &&
		    strstr(line, "case UBERTOOTH_CLEAR_AFHMAP:") != NULL)
			s->afhmap_clear_case = lineno;
		if (s->afhmap_clear_case > 0 && s->afhmap_clear_drop == 0 &&
		    strstr(line, "stated_what &= ~STATED_AFH_MAP") != NULL)
			s->afhmap_clear_drop = lineno;

		/* The modulation and jam mode requests, and the copy each keeps.
		 * Scanned here for the reason the AFH map request is: neither
		 * asks for a mode, so the request_case machinery below does not
		 * follow them.
		 *
		 * `in_case` is which of the two the lines below belong to, so a
		 * keep call in some later case cannot stand in for a missing
		 * one here. A `case` label or a `break;` ends it. */
		/* Both requests state a setting after a mode request, so each
		 * keeps a copy for that mode under the `requested_mode != mode`
		 * guard. */
		if (strstr(line, "case UBERTOOTH_SET_MOD:") != NULL) {
			s->setmod_case = lineno;
			in_case = 'm';
		} else if (strstr(line, "case UBERTOOTH_JAM_MODE:") != NULL) {
			s->jam_case = lineno;
			in_case = 'j';
		} else if (strstr(line, "case ") != NULL ||
		           strstr(line, "break;") != NULL) {
			in_case = 0;
		}
		if (in_case == 'm' && s->setmod_stated == 0 &&
		    strstr(line, "stated_for_starting_mode(STATED_MODULATION)")
		    != NULL)
			s->setmod_stated = lineno;
		if (in_case == 'm' && s->setmod_stated == 0 &&
		    s->setmod_guard == 0 &&
		    strstr(line, "if (requested_mode != mode)") != NULL)
			s->setmod_guard = lineno;
		if (in_case == 'j' && s->jam_stated == 0 &&
		    strstr(line, "stated_for_starting_mode(STATED_JAM_MODE)")
		    != NULL)
			s->jam_stated = lineno;
		if (in_case == 'j' && s->jam_stated == 0 && s->jam_guard == 0 &&
		    strstr(line, "if (requested_mode != mode)") != NULL)
			s->jam_guard = lineno;

		/* program_stated_modulation(), and the capture loop's call to
		 * it. The definition is the line that opens the function, so
		 * the call inside the loop cannot be read as the definition. */
		if (s->mod_fn == 0 &&
		    strstr(line, "static uint8_t program_stated_modulation(")
		    != NULL)
			s->mod_fn = lineno;
		else if (strstr(line, "program_stated_modulation(") != NULL &&
		         s->stream_mod_call == 0)
			s->stream_mod_call = lineno;

		/* reset_absorbed_mode_state()'s body, scanned for the same reason
		 * cc2400_idle()'s is: what it resets, and what it leaves alone. */
		if (in_absorbed_fn) {
			if (strstr(line, "rssi_iir_reset(") != NULL &&
			    s->absorbed_reset_fn_iir == 0)
				s->absorbed_reset_fn_iir = lineno;
			if (is_assignment_to(line, "modulation") &&
			    s->absorbed_reset_fn_mod == 0)
				s->absorbed_reset_fn_mod = lineno;
			if (line[0] == '}')
				in_absorbed_fn = 0;
			continue;
		}

		/*
		 * apply_stated_settings()'s body. The copy of what the host
		 * stated has to be cleared whether it is applied or not: a copy
		 * kept for a mode that never started belongs to a run that is
		 * over, and leaving it would carry that run's channel into
		 * whatever starts next.
		 */
		if (in_stated_fn) {
			/* The guard that keeps a copy to its own mode: the body
			 * has to compare the mode request the copy followed with
			 * the mode that is starting. Without it, a copy left by a
			 * run whose mode never started would be applied to
			 * whatever starts next -- the carry-over the reset exists
			 * to prevent. */
			if (contains_identifier(line, "stated_for_mode") &&
			    contains_identifier(line, "starting_mode") &&
			    s->stated_fn_match == 0)
				s->stated_fn_match = lineno;
			/* And what keeps a copy whose mode has not started yet:
			 * the dispatch reads the request once on its way in, so
			 * a second tool's request and its setting can both be
			 * answered during the reset, and the mode starting here
			 * is the first tool's. */
			if (contains_identifier(line, "stated_for_mode") &&
			    contains_identifier(line, "requested_mode") &&
			    s->stated_fn_pending == 0)
				s->stated_fn_pending = lineno;
			if (is_assignment_to(line, "stated_what")) {
				if (s->stated_fn_clear == 0)
					s->stated_fn_clear = lineno;
				/* The last one is the apply path's: the drop
				 * branch clears it too, and recording only the
				 * first would let the apply path stop. */
				s->stated_fn_clear_last = lineno;
				s->stated_fn_clears++;
			}
			if (contains_identifier(line, "what") &&
			    !is_assignment_to(line, "stated_what") &&
			    strstr(line, "if") != NULL &&
			    s->stated_fn_first_use == 0)
				s->stated_fn_first_use = lineno;
			if (line[0] == '}')
				in_stated_fn = 0;
			continue;
		}

		if (!in_main && !in_idle_fn) {
			if (strstr(line, "apply_stated_settings(") != NULL &&
			    strchr(line, ';') == NULL) {
				in_stated_fn = 1;
				s->stated_fn = lineno;
				continue;
			}
			if (strstr(line, "reset_absorbed_mode_state(") != NULL &&
			    strchr(line, ';') == NULL) {
				in_absorbed_fn = 1;
				s->absorbed_reset_fn = lineno;
				continue;
			}
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

			/* set_channel_request()'s body: what a channel set
			 * writes, and what it leaves out while LE follow mode
			 * is running. The channel set asks for no mode, so it is
			 * not one of the request cases kept below. */
			if (strcmp(fn, "set_channel_request") == 0) {
				if (is_assignment_to(line, "channel") &&
				    s->channel_assign == 0)
					s->channel_assign = lineno;
				if (strstr(line, "mode != MODE_BT_FOLLOW_LE")
				    != NULL && s->channel_follow_guard == 0)
					s->channel_follow_guard = lineno;
				if (strstr(line, "cs_threshold_calc_and_set(")
				    != NULL && s->channel_cs_set == 0)
					s->channel_cs_set = lineno;
			}

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
				if (strstr(line, "forget_hop_state(") != NULL &&
				    current.forget == 0)
					current.forget = lineno;
				if (is_assignment_to(line, "ego_mode") &&
				    current.ego_mode_set == 0)
					current.ego_mode_set = lineno;
				if (is_assignment_to(line, "ego_restart") &&
				    current.restart == 0)
					current.restart = lineno;
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
			if (is_assignment_to(line, "ego_mode") &&
			    s->idle_fn_ego_mode == 0)
				s->idle_fn_ego_mode = lineno;
			if (is_assignment_to(line, "modulation") &&
			    s->idle_fn_modulation == 0)
				s->idle_fn_modulation = lineno;
			if (strstr(line, "rssi_iir_reset(") != NULL &&
			    s->idle_fn_iir == 0)
				s->idle_fn_iir = lineno;
			/* The value as well as the register: a teardown that
			 * wrote the override rather than the reset value would
			 * leave exactly the carry-over this is about. */
			if (contains_identifier(line, "cc2400_set") &&
			    contains_identifier(line, "MANAND") &&
			    contains_identifier(line, "MANAND_RESET_VALUE") &&
			    s->idle_fn_manand == 0)
				s->idle_fn_manand = lineno;
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
		if (strstr(line, "rssi_iir_reset(") != NULL) {
			s->iir_calls++;
			if (s->iir_call == 0)
				s->iir_call = lineno;
		}
		if (strstr(line, "ego_access_code_reset(") != NULL) {
			s->ego_reset_calls++;
			if (s->ego_reset_call == 0)
				s->ego_reset_call = lineno;
		}
		if (strstr(line, "apply_stated_settings(") != NULL) {
			s->stated_calls++;
			if (s->stated_call == 0)
				s->stated_call = lineno;
		}
		/* The jam mode is reset in the dispatch teardown, not in
		 * cc2400_idle(), because that runs in the middle of the
		 * promiscuous mode. */
		if (is_assignment_to(line, "jam_mode") && s->dispatch_jam == 0)
			s->dispatch_jam = lineno;
		if (strstr(line, "usb_irq_hold(") != NULL) {
			if (s->irq_hold == 0)
				s->irq_hold = lineno;
			else if (s->irq_hold_2 == 0)
				s->irq_hold_2 = lineno;
		}
		if (strstr(line, "usb_irq_release(") != NULL) {
			if (s->irq_release == 0)
				s->irq_release = lineno;
			else if (s->irq_release_2 == 0)
				s->irq_release_2 = lineno;
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
 * The E-GO access code is put back on the way out of a mode, in the dispatch
 * and nowhere else: it belongs to the capture that asked for it, and it used
 * to stay on the board until a reset, so the next ubertooth-ego run captured
 * with the code the last one was given.
 */
MU_TEST(test_leaving_a_mode_puts_the_access_code_back)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the dispatch calls ego_access_code_reset() %d times; it "
	         "belongs with the other resets, on the way out of whatever mode "
	         "was running",
	         path, s.ego_reset_calls);
	mu_assert(s.ego_reset_calls == 1, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: ego_access_code_reset() is called at or after the "
	         "`mode =` at line %d, so it would run after the new mode was "
	         "recorded rather than as part of leaving the old one",
	         path, s.ego_reset_call, s.mode_assign);
	mu_assert(s.ego_reset_call > 0 && s.mode_assign > 0 &&
	          s.ego_reset_call < s.mode_assign, msg);
}

/*
 * What the host stated after its mode request is applied once, after the
 * reset that would otherwise have thrown it away and before the cases, whose
 * own setup wins. After `mode` as well, because a channel request reads it.
 */
MU_TEST(test_what_was_stated_is_applied_between_the_reset_and_the_cases)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the dispatch calls apply_stated_settings() %d times; a "
	         "mode start applies the copy once",
	         path, s.stated_calls);
	mu_assert(s.stated_calls == 1, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: apply_stated_settings() is called outside the window "
	         "between the `mode =` at line %d and the first "
	         "`case MODE_...:` at line %d, so it would run before the reset "
	         "it undoes or after the mode's own setup",
	         path, s.stated_call, s.mode_assign, s.first_case);
	mu_assert(s.stated_call > s.mode_assign && s.stated_call < s.first_case,
	          msg);
}

/*
 * The mode record and the copy it applies are one step as far as the request
 * handler is concerned: both of the requests that leave a copy run from the
 * USB interrupt, so one arriving between them would have its own copy dropped
 * by the read-then-clear, or leave the channel from its request and the hop
 * mode from the copy.
 */
MU_TEST(test_the_mode_record_and_the_copy_hold_the_usb_interrupt_off)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the dispatch has no second usb_irq_hold() / "
	         "usb_irq_release() pair (found %d / %d), so the `mode =` at "
	         "line %d and the apply_stated_settings() at line %d run with "
	         "the request handler live",
	         path, s.irq_hold_2, s.irq_release_2, s.mode_assign,
	         s.stated_call);
	mu_assert(s.irq_hold_2 > 0 && s.irq_release_2 > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the `mode =` at line %d and apply_stated_settings() at "
	         "line %d are not both inside the hold at lines %d-%d",
	         path, s.mode_assign, s.stated_call, s.irq_hold_2,
	         s.irq_release_2);
	mu_assert(s.irq_hold_2 < s.mode_assign && s.mode_assign < s.stated_call &&
	          s.stated_call < s.irq_release_2, msg);
}

/*
 * A copy for a mode that is not the one starting is dropped -- unless that
 * mode is still the one the host is waiting for, which is the one case where
 * its mode start is yet to come. Dropping it always would lose a second
 * tool's setting when its request arrived during the first tool's teardown;
 * keeping it always would carry a dead run's channel into a later mode.
 */
MU_TEST(test_a_stated_setting_is_dropped_unless_its_mode_is_still_asked_for)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "%s: the scan did not find "
	         "apply_stated_settings()'s definition", path);
	mu_assert(s.stated_fn > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: apply_stated_settings() does not compare "
	         "`stated_for_mode` with `requested_mode`, so a copy whose mode "
	         "has not started yet is either dropped or kept without asking "
	         "whether that mode is still coming",
	         path, s.stated_fn);
	mu_assert(s.stated_fn_pending > 0, msg);

	/* Both branches clear it: the one that drops a dead copy and the one
	 * that applies a copy. The last `stated_what = 0` is the applying
	 * branch's, so it is the one that has to come before the copy is read:
	 * a branch that applied a copy and left it would apply it again at the
	 * next mode start. */
	snprintf(msg, sizeof(msg),
	         "%s:%d: apply_stated_settings() clears the copy %d times; the "
	         "branch that drops one and the branch that applies one each "
	         "have to",
	         path, s.stated_fn, s.stated_fn_clears);
	mu_assert(s.stated_fn_clears == 2, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: it reads what the copy holds at line %d before the "
	         "applying branch clears it at line %d, so a copy it applied "
	         "could be left behind for the next mode start",
	         path, s.stated_fn, s.stated_fn_first_use, s.stated_fn_clear_last);
	mu_assert(s.stated_fn_clear_last > 0 && s.stated_fn_first_use > 0 &&
	          s.stated_fn_clear_last < s.stated_fn_first_use, msg);
}

/*
 * And it is applied only to the mode it was stated for. The model in
 * test_mode_state_reset.c checks what that rule does; this checks that the
 * firmware still asks the question, because a body that applied every copy it
 * held would pass every other test here while carrying one run's channel into
 * the next mode.
 */
MU_TEST(test_a_stated_setting_is_applied_only_to_the_mode_it_was_stated_for)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: apply_stated_settings() does not compare "
	         "`stated_for_mode` with the mode that is starting, so a copy "
	         "stated for a mode that never started would be applied to "
	         "whatever starts next",
	         path, s.stated_fn);
	mu_assert(s.stated_fn_match > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the comparison is at or after the first use of what the "
	         "copy holds, at line %d",
	         path, s.stated_fn_match, s.stated_fn_first_use);
	mu_assert(s.stated_fn_first_use > 0 &&
	          s.stated_fn_match < s.stated_fn_first_use, msg);
}

/*
 * The modulation request keeps a copy for the mode it was stated for, and only
 * for that mode. ubertooth-dump sends it after the request that starts its
 * mode, as every tool sends its channel, so the mode start's reset would
 * otherwise throw it away and the dump would hold Basic Rate records. The
 * guard is the other half: a copy made while the mode it names is already
 * running would wait for whatever mode started next.
 */
MU_TEST(test_the_modulation_request_keeps_a_copy_for_its_own_mode)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the UBERTOOTH_SET_MOD case, so the "
	         "checks below would pass without reading anything", path);
	mu_assert_fatal(s.setmod_case > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the modulation request does not call "
	         "stated_for_starting_mode(STATED_MODULATION), so `ubertooth-dump "
	         "-l` dumps Basic Rate records on a board left in another mode",
	         path, s.setmod_case);
	mu_assert(s.setmod_stated > s.setmod_case, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the modulation request keeps its copy without asking "
	         "whether the mode it names is still to start, so one stated "
	         "under a running capture would reach an unrelated mode start",
	         path, s.setmod_case);
	mu_assert(s.setmod_guard > s.setmod_case &&
	          s.setmod_guard < s.setmod_stated, msg);
}

/*
 * The jam mode request keeps a copy for the mode it was stated for, and only
 * for that mode. ubertooth-btle sends it after the request that starts its
 * follow or promiscuous mode -- as it does the modulation -- so a jam mode
 * left by a tool killed before its mode started names that mode and is
 * dropped rather than reaching the next capture (#872). The copy is under the
 * `requested_mode != mode` guard, so a jam mode stated under a running capture
 * is applied live and keeps no copy.
 */
MU_TEST(test_the_jam_mode_request_keeps_a_copy_for_its_own_mode)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the UBERTOOTH_JAM_MODE case", path);
	mu_assert_fatal(s.jam_case > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the jam mode request does not call "
	         "stated_for_starting_mode(STATED_JAM_MODE), so a jam mode is "
	         "not carried across the mode start it was stated for and "
	         "`ubertooth-btle -f -j` follows without jamming on a board left "
	         "in another mode", path, s.jam_case);
	mu_assert(s.jam_stated > s.jam_case, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the jam mode request keeps its copy without asking "
	         "whether the mode it names is still to start, so one stated "
	         "under a running capture would reach an unrelated mode start",
	         path, s.jam_case);
	mu_assert(s.jam_guard > s.jam_case && s.jam_guard < s.jam_stated, msg);
}

/*
 * And the capture loop is what makes the modulation's order work. cc2400_rx()
 * writes MDMCTRL once, as the mode comes up, which is before the host's next
 * request can arrive: a modulation stated after the mode request reached the
 * global and not the radio, so a plain `ubertooth-dump -l` from an idle board
 * captured at Basic Rate and said nothing.
 */
MU_TEST(test_the_capture_loop_programs_a_stated_modulation)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find program_stated_modulation()'s "
	         "definition", path);
	mu_assert_fatal(s.mod_fn > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: nothing calls program_stated_modulation(), so the "
	         "modulation a capture asked for would only ever be the one the "
	         "mode started with", path, s.mod_fn);
	mu_assert(s.stream_mod_call > s.mod_fn, msg);
}

/*
 * Leaving a mode puts the modulation back, which is what a stated copy is for:
 * a value one run stated reaches the mode it was stated for and no other. The
 * modulation goes back in cc2400_idle(), which the teardown runs.
 *
 * The jam mode does not: cc2400_idle() runs in the middle of the promiscuous
 * mode, inside bt_generic_le(), before bt_le_sync() has read the jam mode, so
 * resetting it there left `ubertooth-btle -J` silent. The jam mode is reset in
 * the dispatch instead, the one place a capture has actually ended, so a jam
 * mode one run asked for is still not carried into the next.
 */
MU_TEST(test_leaving_a_mode_puts_the_modulation_and_jam_mode_back)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_idle() no longer puts `modulation` back, so a "
	         "capture would inherit the modulation of the run before it",
	         path, s.idle_fn);
	mu_assert(s.idle_fn_modulation > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the dispatch teardown no longer resets `jam_mode`, so a jam "
	         "mode one run asked for would be carried into the next", path);
	mu_assert(s.dispatch_jam > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the dispatch resets `jam_mode` at line %d, which is not "
	         "before the `mode =` at line %d, so the reset would run after "
	         "the mode it belongs to had been recorded",
	         path, s.dispatch_jam, s.dispatch_jam, s.mode_assign);
	mu_assert(s.dispatch_jam > 0 && s.mode_assign > 0 &&
	          s.dispatch_jam < s.mode_assign, msg);
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
 * Leaving a mode also forgets every channel's running average. Those averages
 * take hundreds of packets to build, so they are the one piece of RSSI state
 * the per-packet reset must not touch; this is where they are dropped, beside
 * the queued packets and for the same reason. Clearing them per packet instead
 * left the filter one packet old forever, which rounds to zero for every
 * reading the radio reports in the band: every Classic record carried the same
 * noise figure, the -55 dBm the host reads an empty filter as, whatever the
 * radio heard.
 */
MU_TEST(test_leaving_a_mode_forgets_the_channel_averages)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the dispatch loop calls rssi_iir_reset() %d times; it "
	         "calls it once, on the way out of whatever mode was running",
	         path, s.iir_calls);
	mu_assert(s.iir_calls == 1, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rssi_iir_reset() is called at or after the `mode =` at "
	         "line %d, so the old mode's averages would be dropped after the "
	         "new mode was already recorded", path, s.iir_call,
	         s.mode_assign);
	mu_assert(s.iir_call > 0 && s.mode_assign > 0 &&
	          s.iir_call < s.mode_assign, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_idle() clears the channel averages, but it also "
	         "runs at boot and from inside bt_generic_le() and bt_le_sync(), "
	         "so the filter would keep restarting under a running capture",
	         path, s.idle_fn_iir);
	mu_assert(s.idle_fn_iir == 0, msg);
}

/*
 * The other half of the rule: a mode request the running stream absorbs keeps
 * the averages. That reset is for what a mode start configures, and the
 * averages are not that -- they are what the radio has been hearing, and the
 * three absorbable modes all receive the same modulation over the same 79
 * channels without the radio ever stopping. Clearing them there would cost
 * each absorbed capture the few hundred packets the filter takes to build up.
 * test_mode_state_reset.c works the rule through on a model of both paths.
 */
MU_TEST(test_an_absorbed_mode_start_keeps_the_channel_averages)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: reset_absorbed_mode_state() was not found, so the check "
	         "below cannot mean anything", path);
	mu_assert(s.absorbed_reset_fn > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: reset_absorbed_mode_state() clears the channel "
	         "averages; the radio never stopped receiving, so they still "
	         "describe the band the absorbed capture is listening to",
	         path, s.absorbed_reset_fn_iir);
	mu_assert(s.absorbed_reset_fn_iir == 0, msg);
}

/*
 * What an absorbed mode start does put back: the modulation. The three
 * absorbable modes all read Basic Rate packets, and the request that starts
 * one of them states no modulation, so without this a killed
 * `ubertooth-dump -l` left the AFH detection or follow absorbed after it
 * demodulating low energy -- finding little and saying nothing. It can be
 * reset under a running receive because the capture loop writes MDMCTRL when
 * the modulation changes.
 */
MU_TEST(test_an_absorbed_mode_start_puts_the_modulation_back)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: reset_absorbed_mode_state() was not found, so the check "
	         "below cannot mean anything", path);
	mu_assert_fatal(s.absorbed_reset_fn > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: reset_absorbed_mode_state() does not put `modulation` "
	         "back, so a mode absorbed after a low-energy dump would read "
	         "Classic packets with the wrong demodulator",
	         path, s.absorbed_reset_fn);
	mu_assert(s.absorbed_reset_fn_mod > s.absorbed_reset_fn, msg);
}

/*
 * The E-GO mode request is a mode start the dispatch never sees: ego_main()'s
 * loop runs while MODE_EGO is the mode asked for, so a second request for it
 * is absorbed there, the way a running symbol stream absorbs one of its three.
 * The handler's part is to store the mode, forget the hopping as any mode start
 * does, and set the flag the loop starts the state machine over on.
 *
 * The order matters twice over: the loop reads the mode only after it has seen
 * the flag, and a reset after the request's own setup would discard what the
 * request just asked for.
 */
MU_TEST(test_the_ego_request_restarts_the_running_mode)
{
	char msg[1024];
	const struct request_case *ego = NULL;
	int i;

	for (i = 0; i < s.request_count; i++)
		if (strcmp(s.requests[i].mode, "MODE_EGO") == 0)
			ego = &s.requests[i];

	snprintf(msg, sizeof(msg),
	         "%s: the scan found no vendor request case asking for MODE_EGO, "
	         "so the checks below would pass without reading anything", path);
	mu_assert_fatal(ego != NULL, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: %s does not set ego_restart, so ego_main()'s loop goes "
	         "on running the state machine the last run entered: an "
	         "`ubertooth-ego -r` after a killed `-f` captures in follow mode",
	         path, ego->line, ego->request);
	mu_assert(ego->restart > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: %s sets ego_restart (line %d) before storing the mode "
	         "(line %d), so the loop can see the flag and read the mode the "
	         "run before it asked for", path, ego->line, ego->request,
	         ego->restart, ego->ego_mode_set);
	mu_assert(ego->ego_mode_set > 0 && ego->ego_mode_set < ego->restart, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: %s does not call forget_hop_state(), so this mode start "
	         "-- which never reaches the dispatch -- leaves the hop mode and "
	         "AFH map of whatever ran before it", path, ego->line,
	         ego->request);
	mu_assert(ego->forget > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: %s forgets the hopping at line %d, after its own setup "
	         "at line %d, so the reset discards what the request just asked "
	         "for", path, ego->line, ego->request, ego->forget, ego->setup);
	mu_assert(ego->setup > 0 && ego->forget < ego->setup, msg);
}

/*
 * ...and the mode it stored has to survive the teardown of the mode it
 * replaces. cc2400_idle() used to put ego_mode back to following, and it runs
 * between the request and the dispatch that reads the mode: after a killed
 * ubertooth-specan, `ubertooth-ego -r` hopped the E-GO channel list and `-i`
 * did not interfere. Which E-GO mode to run is part of the request that starts
 * the mode, not state the mode that is ending left behind.
 */
MU_TEST(test_cc2400_idle_does_not_choose_the_ego_mode)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_idle() assigns `ego_mode`, so it overwrites the "
	         "mode the UBERTOOTH_EGO request that is starting stored",
	         path, s.idle_fn_ego_mode);
	mu_assert(s.idle_fn_ego_mode == 0, msg);
}

/*
 * The other half of the restart: ego_main()'s loop reads the E-GO mode and the
 * restart flag inside the loop. The mode used to be read once, on the way in,
 * which is what made the second request a no-op -- the loop carried on, and
 * the mode that request stored was never read.
 */
MU_TEST(test_the_ego_loop_reads_the_mode_inside_its_loop)
{
	const char *ego_path = FW_RXTX_DIR "/ego.c";
	char line[SCAN_LINE_MAX], msg[640];
	int lineno = 0, in_block = 0, in_ego_main = 0;
	int loop_line = 0, mode_read = 0, restart_read = 0, queue_clear = 0;
	FILE *f = fopen(ego_path, "r");

	snprintf(msg, sizeof(msg), "cannot open %s to scan", ego_path);
	mu_assert_fatal(f != NULL, msg);

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		if (strstr(line, "usb_queue_init(") != NULL && queue_clear == 0)
			queue_clear = lineno;
		if (!in_ego_main) {
			if (strstr(line, "void ego_main(") != NULL)
				in_ego_main = 1;
			continue;
		}
		if (strstr(line, "while (1)") != NULL && loop_line == 0) {
			loop_line = lineno;
			continue;
		}
		if (loop_line == 0)
			continue;
		if (contains_identifier(line, "ego_mode") && mode_read == 0)
			mode_read = lineno;
		if (contains_identifier(line, "ego_restart") && restart_read == 0)
			restart_read = lineno;
	}
	fclose(f);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find ego_main()'s `while (1)`, so the "
	         "checks below would pass without reading anything", ego_path);
	mu_assert(loop_line > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: ego_main() does not read ego_restart inside its loop (loop "
	         "at line %d), so a second E-GO request -- which the loop "
	         "absorbs, because it asks for the mode the loop is already "
	         "running -- starts nothing", ego_path, loop_line);
	mu_assert(restart_read > loop_line, msg);

	snprintf(msg, sizeof(msg),
	         "%s: ego_main() does not read ego_mode inside its loop (loop at "
	         "line %d), so the state machine is the one the run that entered "
	         "the loop asked for, whatever a later request stored",
	         ego_path, loop_line);
	mu_assert(mode_read > loop_line, msg);

	snprintf(msg, sizeof(msg),
	         "%s: nothing in ego.c drops what the ending run had queued "
	         "(usb_queue_init()), so a restart hands the next run packets "
	         "captured in the mode it replaced -- the drop the dispatch runs "
	         "for a mode start it sees", ego_path);
	mu_assert(queue_clear > 0, msg);
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

/* ------------------------------------------------------------------ *
 * The radio state a mode starts from
 * ------------------------------------------------------------------ */

/*
 * MANAND overrides the radio's internal signals, and a mode that writes it
 * leaves that override behind: the LE PHY forces the low-noise amplifier and
 * mixers on, the receive paths hold the receive gain amplifier out of reset.
 * The two spectrum analyzers do not write the register at all, so what they
 * measure is whatever the mode before them left -- unless the teardown puts it
 * back, which is what this checks.
 *
 * Boot cannot stand in for the teardown. cc2400_init() ran a MANAND write of
 * its own, and clock_start() resets the radio a few microseconds later, so
 * that write never reached a mode; the next case pins that nothing has moved
 * back there.
 */
MU_TEST(test_leaving_a_mode_puts_the_radio_overrides_back)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_idle() does not put MANAND back to "
	         "MANAND_RESET_VALUE, so a mode that does not write the register "
	         "itself -- specan(), led_specan() -- runs with whatever override "
	         "the mode before it left", path);
	mu_assert(s.idle_fn_manand > 0, msg);
}

/*
 * And nothing writes a radio register at boot, where the reset in
 * clock_start() would wipe it. A write there looks like initialisation and is
 * dead: on a board that has just booted, MANAND read 0xffff, the chip's reset
 * value, never the 0x7fff cc2400_init() wrote.
 */
MU_TEST(test_the_boot_path_writes_no_radio_register)
{
	const char *init_path = FW_COMMON_DIR "/ubertooth.c";
	char line[SCAN_LINE_MAX], msg[512], found[SCAN_LINE_MAX] = "";
	int lineno = 0, in_block = 0, in_init = 0, read_lines = 0, found_line = 0;
	FILE *f = fopen(init_path, "r");

	snprintf(msg, sizeof(msg), "cannot open %s to scan", init_path);
	mu_assert_fatal(f != NULL, msg);

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		if (!in_init) {
			if (strstr(line, "void cc2400_init(") != NULL)
				in_init = 1;
			continue;
		}
		read_lines++;
		if ((contains_identifier(line, "cc2400_set") ||
		     contains_identifier(line, "cc2400_set8") ||
		     contains_identifier(line, "cc2400_strobe")) &&
		    found_line == 0) {
			found_line = lineno;
			collapse_spaces(line, found, sizeof(found));
		}
		if (line[0] == '}')
			break;
	}
	fclose(f);

	mu_assert(read_lines > 10, "cc2400_init()'s body scanned as fewer than "
	                           "10 lines, so the check below would pass "
	                           "without reading anything");
	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_init() writes a radio register (%s), and "
	         "clock_start() resets the radio straight afterwards, so the "
	         "write is undone before any mode sees it", init_path,
	         found_line, found);
	mu_assert(found_line == 0, msg);
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

		if (rc->mode[0] != '\0' || (rc->reset == 0 && rc->forget == 0))
			continue;
		snprintf(msg, sizeof(msg),
		         "%s:%d: %s asks for no mode but resets what a mode start "
		         "resets (reset_absorbed_mode_state() at line %d, "
		         "forget_hop_state() at line %d)",
		         path, rc->line, rc->request, rc->reset, rc->forget);
		mu_assert(0, msg);
	}
}

/*
 * The AFH map a request states is kept for the mode request it followed, the
 * way a channel is. ubertooth-follow states its map just after asking for the
 * hopping, and on a board left in a mode that captures nothing the teardown's
 * clear_afh_map() runs a moment after that request was answered: without the
 * copy the map is thrown away and the follow hops all 79 channels instead of
 * the piconet's.
 */
MU_TEST(test_the_afh_map_request_keeps_a_copy_for_the_mode_starting)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the UBERTOOTH_SET_AFHMAP case, so "
	         "the check below would pass without reading anything", path);
	mu_assert(s.afhmap_case > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the AFH map request does not call "
	         "stated_for_starting_mode(STATED_AFH_MAP), so a map stated "
	         "after a mode request is lost when the mode start's reset "
	         "clears it", path, s.afhmap_case);
	mu_assert(s.afhmap_keep > s.afhmap_case, msg);

	/* And only while a mode start is still to come, as the channel request
	 * has it: a map sent by a capture that is already running would
	 * otherwise be handed to the next capture asking for the same mode. */
	snprintf(msg, sizeof(msg),
	         "%s:%d: the AFH map request keeps a copy without testing "
	         "`requested_mode != mode`, so a map sent during a running "
	         "capture is kept for the next one", path, s.afhmap_case);
	mu_assert(s.afhmap_guard > s.afhmap_case &&
	          s.afhmap_guard < s.afhmap_keep, msg);
}

/*
 * And an explicit clear drops it. A host that clears the map means the capture
 * to hop the whole band, so a map stated earlier for the mode that is still to
 * start must not be applied at its mode start.
 */
MU_TEST(test_clearing_the_afh_map_drops_a_stated_one)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the UBERTOOTH_CLEAR_AFHMAP case, so "
	         "the check below would pass without reading anything", path);
	mu_assert(s.afhmap_clear_case > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the clear does not drop a stated map "
	         "(stated_what &= ~STATED_AFH_MAP), so the map cleared here is "
	         "applied again at the mode start", path, s.afhmap_clear_case);
	mu_assert(s.afhmap_clear_drop > s.afhmap_clear_case, msg);
}

/*
 * A channel set reaches the channel the board tunes to, whatever mode is
 * running.
 *
 * The write used to sit inside `if (mode != MODE_BT_FOLLOW_LE)`, because the
 * loop that ran LE follow mode in those days picked the channel up from
 * `requested_channel` instead. le_phy.c took that mode over and the loop went
 * with it, so in LE follow mode the write happened nowhere: the request still
 * returned success and the board kept its old channel, and the next mode to
 * bring the radio up tuned to that one.
 *
 * The carrier-sense threshold is the one thing still left out in that mode:
 * writing it reaches the radio over the serial bus the low-energy engine is
 * using, and that engine does not use carrier sense. So the guard must hold
 * nothing but that write.
 */
MU_TEST(test_a_channel_set_is_not_skipped_in_le_follow_mode)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the channel set does not write `channel`", path);
	mu_assert_fatal(s.channel_assign > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the channel set no longer asks whether LE follow mode is "
	         "running, so this test cannot say what the answer changes",
	         path);
	mu_assert_fatal(s.channel_follow_guard > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: `channel` is written at line %d, after the LE follow "
	         "check at line %d, so a board in that mode keeps its old "
	         "channel and the host is told otherwise",
	         path, s.channel_assign, s.channel_assign,
	         s.channel_follow_guard);
	mu_assert(s.channel_assign < s.channel_follow_guard, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the carrier-sense threshold is set at line %d, which is "
	         "not after the LE follow check at line %d: it is the one write "
	         "that must be left out in that mode",
	         path, s.channel_cs_set, s.channel_follow_guard);
	mu_assert(s.channel_cs_set > s.channel_follow_guard, msg);
}

int main(void)
{
	printf("mode dispatch tests\n");
	MU_RUN(test_source_was_scanned);
	MU_RUN(test_mode_is_recorded_once);
	MU_RUN(test_mode_is_recorded_before_the_cases);
	MU_RUN(test_leaving_a_mode_idles_once_first);
	MU_RUN(test_leaving_a_mode_puts_the_access_code_back);
	MU_RUN(test_what_was_stated_is_applied_between_the_reset_and_the_cases);
	MU_RUN(test_the_mode_record_and_the_copy_hold_the_usb_interrupt_off);
	MU_RUN(test_a_stated_setting_is_dropped_unless_its_mode_is_still_asked_for);
	MU_RUN(test_a_stated_setting_is_applied_only_to_the_mode_it_was_stated_for);

	MU_RUN(test_the_modulation_request_keeps_a_copy_for_its_own_mode);
	MU_RUN(test_the_jam_mode_request_keeps_a_copy_for_its_own_mode);
	MU_RUN(test_the_capture_loop_programs_a_stated_modulation);
	MU_RUN(test_leaving_a_mode_puts_the_modulation_and_jam_mode_back);
	MU_RUN(test_leaving_a_mode_drops_what_it_queued);
	MU_RUN(test_leaving_a_mode_forgets_the_channel_averages);
	MU_RUN(test_an_absorbed_mode_start_keeps_the_channel_averages);
	MU_RUN(test_an_absorbed_mode_start_puts_the_modulation_back);
	MU_RUN(test_the_queue_clear_holds_the_usb_interrupt_off);
	MU_RUN(test_cc2400_idle_does_not_choose_the_mode);
	MU_RUN(test_cc2400_idle_does_not_drop_queued_packets);
	MU_RUN(test_leaving_a_mode_puts_the_radio_overrides_back);
	MU_RUN(test_the_boot_path_writes_no_radio_register);
	MU_RUN(test_the_stream_loop_runs_every_mode_dispatched_to_it);
	MU_RUN(test_the_stream_loop_keeps_the_indirect_mode_apart);
	MU_RUN(test_leaving_a_mode_forgets_the_afh_map);
	MU_RUN(test_the_afh_map_request_keeps_a_copy_for_the_mode_starting);
	MU_RUN(test_clearing_the_afh_map_drops_a_stated_one);
	MU_RUN(test_the_absorbed_modes_are_the_streamed_modes);
	MU_RUN(test_an_absorbable_request_resets_before_its_own_setup);
	MU_RUN(test_setting_a_channel_resets_nothing);
	MU_RUN(test_the_ego_request_restarts_the_running_mode);
	MU_RUN(test_cc2400_idle_does_not_choose_the_ego_mode);
	MU_RUN(test_the_ego_loop_reads_the_mode_inside_its_loop);
	MU_RUN(test_a_channel_set_is_not_skipped_in_le_follow_mode);
	return mu_report();
}
