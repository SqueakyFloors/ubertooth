/*
 * Guard for the radio setup of the range test pair in
 * firmware/bluetooth_rxtx/cc2400_rangetest.c.
 *
 * The two-board range test never completed, on this fork or on a stock
 * 2020-12-R1 stack: `ubertooth-util -n` against a `-e` repeater returned no
 * result in either direction. Three causes, each enough on its own, all
 * isolated one at a time on hardware:
 *
 *   - Neither mode held the CC2400's receive gain amplifier out of reset
 *     (MANAND). Both receivers reported a strong signal and hundreds of valid
 *     RSSI readings, and neither saw one sync word. There was no write at boot
 *     to fall back on: cc2400_init() made one and clock_start() reset the
 *     radio straight afterwards, which is why that write is gone.
 *   - The repeater never wrote GRMDM, so it decoded requests only while the
 *     radio still held its reset value -- which happens to be the packet
 *     format the sender uses. After any mode that writes GRMDM it decoded
 *     nothing.
 *   - The repeater's turnaround delay did not exist. `i = 2000000;
 *     while (--i);` reads and writes nothing the rest of the program can see,
 *     so the compiler deleted it -- confirmed in the disassembly -- and the
 *     reply was over before the sender had finished transmitting.
 *
 * Both functions are hardware code that cannot run here, so this scans the
 * source as text, the way test_mode_dispatch.c scans main()'s dispatch.
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"

#ifndef FW_DIR
#define FW_DIR ".."
#endif

/*
 * rangetest_wait_for_fifo(), every code line of it with the spacing flattened
 * and the comments left out. The wait cannot be run here, and each line of it
 * can be left standing while the wait is taken apart around it, so the whole
 * function is pinned as written and not only its pieces.
 */
#define FIFO_WAIT_BODY \
	"{ " \
	"u32 last = T0TC; " \
	"u32 waited = 0; " \
	"u32 polls = RANGETEST_LEAD_POLLS; " \
	"while (waited < RANGETEST_LEAD_100NS && polls-- > 0) { " \
	"u32 now = T0TC; " \
	"u32 step = (now + CLK100NS_PER_CLKN - last) % CLK100NS_PER_CLKN; " \
	"last = now; " \
	"if (step <= RANGETEST_LEAD_100NS) " \
	"waited += step; " \
	"} " \
	"}"

struct scan {
	int rangetest_manand;   /* cc2400_set(MANAND, ...) in the range test */
	int repeater_manand;    /* and in the repeater */
	int repeater_grmdm;     /* cc2400_set(GRMDM, ...) in the repeater */

	int rr_cleared_early;   /* `rr.valid = 0` before the serial-number read */
	int iap_read;           /* where that read is */

	int countdowns;         /* body-less countdown loops, which compile away */
	int first_countdown;
	char countdown_text[256];

	int turnaround_fn;      /* repeater_turnaround() definition */
	int turnaround_clkn;    /* its deadline is read from clkn */
	int turnaround_service; /* it services USB while it waits */
	int turnaround_mode;    /* it gives up when the mode is stopped */
	int turnaround_call;    /* and the repeater calls it */

	int turnaround_ms;      /* how long the reply is held, in ms */
	int ticks_from_clkn;    /* the delay is converted from clkn's rate */
	char ticks_text[256];   /* the conversion as written */

	int rx_off_line;        /* where the repeater stops receiving */

	int rangetest_fifo_write;  /* cc2400_fifo_write() in the range test */
	int rangetest_fifo_read;   /* cc2400_fifo_read() in it */
	int repeater_fifo_write;   /* and both in the repeater */
	int repeater_fifo_read;
	int perbyte_fifo;          /* a byte-at-a-time FIFOREG access */
	char perbyte_text[256];

	int fifo_wait_fn;        /* rangetest_wait_for_fifo() definition */
	int fifo_wait_timer;     /* it reads timer 0's 100 ns counter */
	int fifo_wait_step;      /* the step it computes, as written */
	char fifo_wait_text[256];
	int fifo_wait_guard;     /* where it drops a step longer than the lead */
	int fifo_wait_joined;    /* a line of it joined to the next by a `\` */
	int fifo_wait_polls;     /* RANGETEST_LEAD_POLLS as written */
	int wait_defines;        /* how often the wait's four numbers are defined */
	int wait_undef;          /* where one of them is undefined again */
	int rangetest_wait_depth; /* how deep in #if/#ifdef the call to the wait */
	int rangetest_read_depth; /* sits, and the FIFO read it comes before */
	int repeater_wait_depth;  /* the same two in the repeater */
	int repeater_read_depth;
	int fifo_wait_adds;      /* how many statements add to the total */
	int fifo_wait_add_ok;    /* where the one that adds the step is */
	char fifo_wait_add[256]; /* as written */
	char fifo_wait_body[1024]; /* every code line of it, flattened */
	int fifo_wait_length;    /* its length is RANGETEST_LEAD_100NS */
	int fifo_wait_cap;       /* and it gives up after RANGETEST_LEAD_POLLS */
	int byte_100ns;          /* RANGETEST_BYTE_100NS as written */
	int lead_bytes;          /* RANGETEST_LEAD_BYTES as written */
	int lead_from_byte;      /* the lead is that many byte times */
	char lead_text[256];     /* as written */

	int rangetest_sync_poll; /* the SYNC_RECEIVED poll in the range test */
	int rangetest_wait;      /* its call to the wait */
	int rangetest_clkn;      /* and its clkn_start() */
	int repeater_sync_poll;  /* the same three in the repeater */
	int repeater_wait;
	int repeater_clkn;

	/* What each side does with a packet its CRC accepted: the sender asks
	 * whether the packet is its own reply, and the repeater whether the
	 * packet is a request. */
	int filter_fn;           /* rangetest_foreign_packet() definition */
	int filter_fn_first;     /* the first payload byte it compares */
	int filter_fn_last;      /* and one past the last */
	int rangetest_crc;       /* the sender's CRC_OK check */
	int rangetest_filter;    /* its call to the filter */
	int rangetest_test;      /* where it reads the filter's answer */
	int rangetest_accept;    /* and where it takes the packet as the reply */
	int rangetest_discard;   /* where it records the discarded packet */
	int rangetest_type_flip; /* `txbuf[1] = 1`, the reply's type byte */
	int filter_fn_return;    /* the filter returns the byte index it found */
	int rangetest_first_only; /* only the first discarded packet is recorded */
	/* The exit that reports no result: where it clears the result, and where
	 * it clears the record of what was discarded. */
	int rangetest_valid_clear_late;
	int rangetest_discard_clear_late;
	int repeater_crc;        /* the repeater's CRC_OK check */
	int repeater_type;       /* the type byte on the same line */
};

/* What the scan of firmware/common/ubertooth.c records. */
struct spi_scan {
	int write_clock_sites;  /* lines that clock SCLK in cc2400_fifo_write() */
	int write_hold;         /* its spi_delay() before CSN rises */
	int read_delays;        /* spi_delay() sites in cc2400_fifo_read() */
};

/*
 * Does this line open a function body at file scope? Used only to end the
 * scope the scan is tracking, so it is deliberately blunt: no leading
 * whitespace, a parameter list, and not a prototype or a preprocessor line.
 */
static int is_function_definition(const char *line)
{
	size_t len;

	if (!is_ident_char(line[0]))
		return 0;
	if (strchr(line, '(') == NULL)
		return 0;
	len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		len--;
	return len > 0 && line[len - 1] != ';';
}

/*
 * Is this line a `while (` whose statement ends in `);` on the same line, so
 * it has no body? `} while (0)` closes a macro and does not count.
 */
static int is_body_less_while(const char *line, char *out, size_t outlen)
{
	size_t len;

	if (strstr(line, "while") == NULL || strstr(line, "while (0)") != NULL)
		return 0;
	collapse_spaces(line, out, outlen);
	len = strlen(out);
	return len >= 2 && out[len - 1] == ';' && out[len - 2] == ')';
}

/*
 * A body-less `while` that only counts a local up or down, such as
 * `i = 2000000; while (--i);`. It reads nothing and writes nothing the rest of
 * the program can see, so the compiler is free to delete it -- and does. A
 * wait on a radio state is a different shape and a different question, checked
 * by test_cc2400_wait.c.
 */
static int is_dead_countdown(const char *line)
{
	char trimmed[512];

	if (contains_identifier(line, "cc2400_status") ||
	    contains_identifier(line, "cc2400_get") ||
	    contains_identifier(line, "clkn"))
		return 0;
	if (!is_body_less_while(line, trimmed, sizeof(trimmed)))
		return 0;
	/*
	 * A wait on something the rest of the program can see is fine; what is
	 * not is a loop whose only term is a local being counted. Match the
	 * counting forms rather than requiring them on the `while` line, so
	 * `while (i--)` and a countdown set up on the line above are both caught.
	 */
	return strstr(trimmed, "--") != NULL || strstr(trimmed, "++") != NULL ||
	       strstr(trimmed, " - 1)") != NULL;
}

/*
 * The number a one-line `#define NAME <number>` gives NAME, or -1 if the line
 * is not one. Used for the two numbers that decide how long the wait before a
 * FIFO read is.
 */
static int defined_number(const char *line, const char *name)
{
	char flat[256], needle[128];
	const char *value;
	char *end;
	long number;

	collapse_spaces(line, flat, sizeof(flat));
	snprintf(needle, sizeof(needle), "#define %s ", name);
	if (strncmp(flat, needle, strlen(needle)) != 0)
		return -1;
	value = flat + strlen(needle);
	number = strtol(value, &end, 10);
	if (end == value || *end != '\0' || number < 0 || number > INT_MAX)
		return -1;
	return (int)number;
}

/*
 * The first line from `start` to the end of that function that ends in a
 * backslash, or 0 if there is none.
 *
 * The scan below reads the file a physical line at a time, and the compiler
 * does not: a backslash at the end of a line joins it to the next one. A line
 * of `// \` therefore takes the line below it into its comment, and the scan
 * still reads that line as code -- so the guard can be commented out with
 * every check on it satisfied. This reads the file again because the scan
 * strips comments as it goes, which takes the backslash with them.
 */
static int joined_line_in_function(const char *path, int start)
{
	char line[SCAN_LINE_MAX];
	int lineno = 0;
	FILE *f;

	if (start <= 0)
		return 0;
	f = fopen(path, "r");
	if (f == NULL)
		return 0;

	while (fgets(line, sizeof(line), f) != NULL) {
		size_t len = strlen(line);

		lineno++;
		if (lineno < start)
			continue;
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
		                   line[len - 1] == ' ' || line[len - 1] == '\t'))
			len--;
		if (len > 0 && line[len - 1] == '\\') {
			fclose(f);
			return lineno;
		}
		if (lineno > start && line[0] == '}')
			break;    /* the function's closing brace */
	}
	fclose(f);
	return 0;
}

static int scan_source(const char *path, struct scan *s)
{
	char line[512];
	int lineno = 0, in_block = 0, cond_depth = 0;
	int in_rangetest = 0, in_repeater = 0, in_turnaround = 0;
	int in_fifo_wait = 0, in_filter = 0;
	int fifo_wait_comment = 0, fifo_wait_ended = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);

		/*
		 * Any other function definition ends the scope being tracked, so a
		 * helper added between repeater_turnaround() and cc2400_rangetest()
		 * -- rangetest_radio_off() sits there today -- cannot satisfy the
		 * checks on the turnaround wait by accident.
		 */
		if (is_function_definition(line))
			in_rangetest = in_repeater = in_turnaround = in_fifo_wait =
			    in_filter = 0;

		if (strstr(line, "repeater_turnaround(void)") != NULL) {
			s->turnaround_fn = lineno;
			in_turnaround = 1;
			continue;
		}
		if (strstr(line, "rangetest_wait_for_fifo(void)") != NULL &&
		    s->fifo_wait_fn == 0) {
			s->fifo_wait_fn = lineno;
			in_fifo_wait = 1;
			continue;
		}
		if (strstr(line, "rangetest_foreign_packet(") != NULL &&
		    strchr(line, ';') == NULL && s->filter_fn == 0) {
			s->filter_fn = lineno;
			in_filter = 1;
			continue;
		}
		if (strstr(line, "void cc2400_rangetest(") != NULL) {
			in_rangetest = 1;
			continue;
		}
		if (strstr(line, "void cc2400_repeater(") != NULL) {
			in_repeater = 1;
			continue;
		}
		/*
		 * cc2400_txtest() is not part of the range test pair: it sets the
		 * radio transmitting and returns, and it neither receives nor waits
		 * for a peer. The scope reset above has already left it out.
		 */

		/*
		 * The delay has to be converted from clkn's rate, not written as a
		 * bare tick count: a plain number here is how a 1 s delay silently
		 * becomes 300 us.
		 */
		if (s->turnaround_ms == 0) {
			int n = defined_number(line, "REPEATER_TURNAROUND_MS");

			if (n > 0)
				s->turnaround_ms = n;
		}
		if (strstr(line, "define REPEATER_TURNAROUND_TICKS") != NULL) {
			collapse_spaces(line, s->ticks_text, sizeof(s->ticks_text));
			if (strstr(s->ticks_text, "3200") != NULL &&
			    strstr(s->ticks_text, "REPEATER_TURNAROUND_MS") != NULL &&
			    strstr(s->ticks_text, "1000") != NULL)
				s->ticks_from_clkn = 1;
		}

		/*
		 * How long the wait before a FIFO read lasts, as two numbers: a
		 * byte of air time in the counter's ticks, and how many bytes to
		 * wait for.
		 */
		if (s->byte_100ns == 0) {
			int n = defined_number(line, "RANGETEST_BYTE_100NS");

			if (n > 0)
				s->byte_100ns = n;
		}
		if (s->lead_bytes == 0) {
			int n = defined_number(line, "RANGETEST_LEAD_BYTES");

			if (n > 0)
				s->lead_bytes = n;
		}
		if (s->fifo_wait_polls == 0) {
			int n = defined_number(line, "RANGETEST_LEAD_POLLS");

			if (n > 0)
				s->fifo_wait_polls = n;
		}
		/*
		 * How many times each of the wait's four numbers is defined, and
		 * whether any of them is undefined again. Everything else here
		 * reads the text of the first definition; the compiler reads the
		 * last one, so `#undef RANGETEST_LEAD_100NS` and a smaller value
		 * would leave every check satisfied and the guard with nothing
		 * left to drop.
		 */
		if (strstr(line, "define RANGETEST_LEAD_100NS") != NULL ||
		    strstr(line, "define RANGETEST_LEAD_POLLS") != NULL ||
		    strstr(line, "define RANGETEST_LEAD_BYTES") != NULL ||
		    strstr(line, "define RANGETEST_BYTE_100NS") != NULL)
			s->wait_defines++;
		if (strstr(line, "#undef") != NULL &&
		    strstr(line, "RANGETEST_") != NULL)
			s->wait_undef = lineno;
		/*
		 * How deep in #if/#ifdef this line is. The checks on the two
		 * calls to the wait read the order of the lines, not whether
		 * they are built, so a conditional that is never true -- `#if
		 * 0`, `#ifdef NEVER` -- would take the wait away with all of
		 * them passing. The file's own `#ifdef TX_ENABLE` and
		 * `#ifdef UBERTOOTH_ONE` are why this counts depth instead of
		 * refusing conditionals outright.
		 */
		{
			const char *p = line;

			while (*p == ' ' || *p == '\t')
				p++;
			if (strncmp(p, "#if", 3) == 0)
				cond_depth++;
			else if (strncmp(p, "#endif", 6) == 0 && cond_depth > 0)
				cond_depth--;
		}
		/*
		 * The wait's length has to be a number of byte times, not a tick
		 * count of its own: a plain number here is how a wait for one byte
		 * silently becomes a wait for thirty.
		 */
		if (strstr(line, "define RANGETEST_LEAD_100NS") != NULL) {
			collapse_spaces(line, s->lead_text, sizeof(s->lead_text));
			/*
			 * Pinned as written, not merely as a line mentioning the
			 * two names: `... * 10` reads as a number of byte times
			 * and still sets a lead no step can exceed, which leaves
			 * the guard in the wait with nothing to drop.
			 */
			if (strcmp(s->lead_text,
			           "#define RANGETEST_LEAD_100NS "
			           "(RANGETEST_LEAD_BYTES * RANGETEST_BYTE_100NS)") == 0)
				s->lead_from_byte = 1;
		}

		if (in_fifo_wait && !fifo_wait_ended) {
			char flat[SCAN_LINE_MAX];

			/*
			 * strip_comments() has already run on the line, so what
			 * is left of a comment is an empty line. Comments are
			 * therefore out of everything below, which leaves them
			 * free to be rewritten and keeps a check from being
			 * satisfied by a sentence that only talks about the code.
			 */
			collapse_spaces(line, flat, sizeof(flat));
			if (flat[0] == '\0')
				continue;

			/*
			 * Every code line of the function, flattened into one
			 * string. The checks after it say what each line is for;
			 * this says that those lines are the whole of it. Each
			 * check on its own can be satisfied while the wait is
			 * taken apart around it -- `waited = waited + step;`
			 * above the guard, a second write to `step` below the
			 * pinned one -- and none of that survives this.
			 */
			if (s->fifo_wait_body[0] != '\0')
				strncat(s->fifo_wait_body, " ",
				        sizeof(s->fifo_wait_body) -
				        strlen(s->fifo_wait_body) - 1);
			strncat(s->fifo_wait_body, flat,
			        sizeof(s->fifo_wait_body) -
			        strlen(s->fifo_wait_body) - 1);

			if (contains_identifier(line, "T0TC"))
				s->fifo_wait_timer = 1;
			if (contains_identifier(line, "RANGETEST_LEAD_POLLS"))
				s->fifo_wait_cap = 1;
			/*
			 * The loop's own condition, pinned as written. The guard
			 * below names RANGETEST_LEAD_100NS too, so a check for
			 * the name alone would no longer say that the wait runs
			 * the whole lead.
			 */
			if (strncmp(flat, "while (", 7) == 0 &&
			    strcmp(flat, "while (waited < RANGETEST_LEAD_100NS && "
			                 "polls-- > 0) {") == 0)
				s->fifo_wait_length = 1;
			/*
			 * The one piece of arithmetic in the change, pinned as
			 * written: nothing here could run it, and its operands the
			 * other way round still compiles, still looks like a
			 * wrap-around step, and ends the wait early.
			 */
			if (strstr(flat, "step =") != NULL) {
				snprintf(s->fifo_wait_text,
				         sizeof(s->fifo_wait_text), "%s", flat);
				if (strcmp(s->fifo_wait_text,
				           "u32 step = (now + CLK100NS_PER_CLKN - last) % "
				           "CLK100NS_PER_CLKN;") == 0)
					s->fifo_wait_step = 1;
			}
			/*
			 * The guard, pinned as written for the same reason:
			 * `RANGETEST_LEAD_100NS <= step` compiles, reads as a
			 * guard, and keeps only the steps that should be
			 * dropped.
			 */
			if (strcmp(flat, "if (step <= RANGETEST_LEAD_100NS)") == 0)
				s->fifo_wait_guard = lineno;
			/*
			 * And the one statement the guard stands in front of,
			 * with its line, so that an addition of something other
			 * than the step, a second addition, or an addition the
			 * guard does not reach -- an empty guard body leaves the
			 * guard itself intact -- is not silently allowed.
			 */
			if (strstr(flat, "waited +=") != NULL) {
				snprintf(s->fifo_wait_add,
				         sizeof(s->fifo_wait_add), "%s", flat);
				s->fifo_wait_adds++;
				if (strcmp(s->fifo_wait_add, "waited += step;") == 0)
					s->fifo_wait_add_ok = lineno;
			}
			/* The function's closing brace ends the body. */
			if (line[0] == '}')
				fifo_wait_ended = 1;
		}

		/*
		 * The range the filter compares, as two numbers. It must start
		 * at byte 1 and not at 0: byte 0 is the length, and a reply that
		 * is this board's but whose length byte is wrong is this test's
		 * answer arriving corrupted, which the caller reports rather
		 * than discards.
		 */
		/* The index it returns, pinned as written: nothing here runs the
		 * function, and `return 1;` compiles, still looks like a byte
		 * number, and makes every warning name the same byte. */
		if (in_filter && strstr(line, "return i;") != NULL &&
		    s->filter_fn_return == 0)
			s->filter_fn_return = lineno;
		if (in_filter && strstr(line, "for (i =") != NULL &&
		    s->filter_fn_first == 0) {
			int first = -1, last = -1;

			if (sscanf(line, " for (i = %d; i < %d", &first, &last) == 2) {
				s->filter_fn_first = first;
				s->filter_fn_last = last;
			}
		}

		if (in_turnaround) {
			if (contains_identifier(line, "clkn"))
				s->turnaround_clkn = 1;
			if (contains_identifier(line, "service_usb"))
				s->turnaround_service = 1;
			if (contains_identifier(line, "requested_mode"))
				s->turnaround_mode = 1;
		}

		if (!in_rangetest && !in_repeater)
			continue;

		if (is_dead_countdown(line)) {
			s->countdowns++;
			if (s->first_countdown == 0) {
				s->first_countdown = lineno;
				collapse_spaces(line, s->countdown_text,
				                sizeof(s->countdown_text));
			}
		}

		if (contains_identifier(line, "cc2400_set") &&
		    contains_identifier(line, "MANAND")) {
			if (in_rangetest && s->rangetest_manand == 0)
				s->rangetest_manand = lineno;
			if (in_repeater && s->repeater_manand == 0)
				s->repeater_manand = lineno;
		}
		if (in_repeater && contains_identifier(line, "cc2400_set") &&
		    contains_identifier(line, "GRMDM") && s->repeater_grmdm == 0)
			s->repeater_grmdm = lineno;

		if (in_rangetest) {
			if (contains_identifier(line, "iap_entry") && s->iap_read == 0)
				s->iap_read = lineno;
			if (is_assignment_of(line, "rr.valid", "0") && s->iap_read == 0 &&
			    s->rr_cleared_early == 0)
				s->rr_cleared_early = lineno;
			/* The same two writes on the exit that reports no result,
			 * which is the one after the serial-number read. */
			if (is_assignment_of(line, "rr.valid", "0") && s->iap_read > 0 &&
			    s->rangetest_valid_clear_late == 0)
				s->rangetest_valid_clear_late = lineno;
			if (is_assignment_of(line, "rr.discarded", "0") &&
			    s->iap_read > 0 && s->rangetest_discard_clear_late == 0)
				s->rangetest_discard_clear_late = lineno;
		}

		if (in_repeater && contains_identifier(line, "repeater_turnaround") &&
		    s->turnaround_call == 0)
			s->turnaround_call = lineno;

		/*
		 * The FIFO has to be reached through the two helpers. A
		 * cc2400_get8(FIFOREG) or cc2400_set8(FIFOREG, b) in a loop makes
		 * one SPI transfer per byte: the write then lands nowhere and the
		 * read comes back wrong.
		 */
		if (contains_identifier(line, "FIFOREG") &&
		    (contains_identifier(line, "cc2400_get8") ||
		     contains_identifier(line, "cc2400_set8")) &&
		    s->perbyte_fifo == 0) {
			s->perbyte_fifo = lineno;
			collapse_spaces(line, s->perbyte_text, sizeof(s->perbyte_text));
		}
		if (contains_identifier(line, "cc2400_fifo_write")) {
			if (in_rangetest && s->rangetest_fifo_write == 0)
				s->rangetest_fifo_write = lineno;
			if (in_repeater && s->repeater_fifo_write == 0)
				s->repeater_fifo_write = lineno;
		}
		if (contains_identifier(line, "cc2400_fifo_read")) {
			if (in_rangetest && s->rangetest_fifo_read == 0) {
				s->rangetest_fifo_read = lineno;
				s->rangetest_read_depth = cond_depth;
			}
			if (in_repeater && s->repeater_fifo_read == 0) {
				s->repeater_fifo_read = lineno;
				s->repeater_read_depth = cond_depth;
			}
		}

		/* Where each mode notices a sync word, where it waits for the
		 * radio to have the packet, and where it starts the counter that
		 * wait reads. */
		if (contains_identifier(line, "SYNC_RECEIVED")) {
			if (in_rangetest && s->rangetest_sync_poll == 0)
				s->rangetest_sync_poll = lineno;
			if (in_repeater && s->repeater_sync_poll == 0)
				s->repeater_sync_poll = lineno;
		}
		if (calls_identifier(line, "rangetest_wait_for_fifo")) {
			if (in_rangetest && s->rangetest_wait == 0) {
				s->rangetest_wait = lineno;
				s->rangetest_wait_depth = cond_depth;
			}
			if (in_repeater && s->repeater_wait == 0) {
				s->repeater_wait = lineno;
				s->repeater_wait_depth = cond_depth;
			}
		}
		if (calls_identifier(line, "clkn_start")) {
			if (in_rangetest && s->rangetest_clkn == 0)
				s->rangetest_clkn = lineno;
			if (in_repeater && s->repeater_clkn == 0)
				s->repeater_clkn = lineno;
		}

		/*
		 * Where each side decides that a packet its CRC accepted is one
		 * of its own. Without this, the sender took any such packet as
		 * the repeater's reply and ended the run as a mismatch, and the
		 * repeater answered a reply as readily as a request -- so two
		 * repeaters in range answered each other.
		 */
		if (contains_identifier(line, "STATUS_CRC_OK")) {
			if (in_rangetest && s->rangetest_crc == 0)
				s->rangetest_crc = lineno;
			if (in_repeater && s->repeater_crc == 0) {
				s->repeater_crc = lineno;
				/* The type byte has to be read on the same line:
				 * it is the condition for taking the packet, not
				 * a check made after taking it. */
				if (strstr(line, "buf[1]") != NULL)
					s->repeater_type = lineno;
			}
		}
		if (in_rangetest && calls_identifier(line, "rangetest_foreign_packet") &&
		    s->rangetest_filter == 0)
			s->rangetest_filter = lineno;
		/* Its answer, and the one place the packet is taken as the
		 * reply: a receive loop that called the filter, recorded a
		 * discard and took the packet anyway would satisfy the two
		 * above on their own. */
		if (in_rangetest && contains_identifier(line, "differs") &&
		    !is_assignment_to(line, "differs") && s->rangetest_test == 0)
			s->rangetest_test = lineno;
		if (in_rangetest && is_assignment_of(line, "got_reply", "1") &&
		    s->rangetest_accept == 0)
			s->rangetest_accept = lineno;
		/* Where the expected payload becomes the reply rather than the
		 * request, which the filter compares against. */
		if (in_rangetest && is_assignment_of(line, "txbuf[1]", "1") &&
		    s->rangetest_type_flip == 0)
			s->rangetest_type_flip = lineno;
		/* Only the first discarded packet of a run is kept, which is what
		 * the host reports: without the guard the warning would name the
		 * last one instead, and the header says the first. */
		if (in_rangetest && strstr(line, "rr.discarded == 0") != NULL &&
		    s->rangetest_first_only == 0)
			s->rangetest_first_only = lineno;
		/* Where it records one, not the clear on the way in. */
		if (in_rangetest && is_assignment_to(line, "rr.discarded") &&
		    !is_assignment_of(line, "rr.discarded", "0") &&
		    s->rangetest_discard == 0)
			s->rangetest_discard = lineno;

		/* The SRFOFF that ends the repeater's receive, which has to come
		 * before the delay rather than after it. */
		if (in_repeater && strstr(line, "SRFOFF") != NULL &&
		    s->rx_off_line == 0)
			s->rx_off_line = lineno;
	}

	fclose(f);
	s->fifo_wait_joined = joined_line_in_function(path, s->fifo_wait_fn);
	return 1;
}

/*
 * Scan cc2400_fifo_write() and cc2400_fifo_read() in
 * firmware/common/ubertooth.c: how many places clock the bus, and where the
 * delays are.
 */
static int scan_spi_source(const char *spi_path, struct spi_scan *s)
{
	char line[512], name[128], current[128] = "";
	int in_block = 0, depth = 0;
	FILE *f = fopen(spi_path, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p;

		strip_comments(line, &in_block);
		if (depth == 0 && call_name(line, name, sizeof(name)))
			snprintf(current, sizeof(current), "%s", name);

		if (strcmp(current, "cc2400_fifo_write") == 0) {
			if (strstr(line, "SCLK_SET") != NULL)
				s->write_clock_sites++;
			if (contains_identifier(line, "spi_delay"))
				s->write_hold++;
		} else if (strcmp(current, "cc2400_fifo_read") == 0) {
			if (contains_identifier(line, "spi_delay"))
				s->read_delays++;
		}

		for (p = line; *p != '\0'; p++) {
			if (*p == '{')
				depth++;
			else if (*p == '}')
				depth--;
		}
	}

	fclose(f);
	return 1;
}

/*
 * What the scan of the badge's header records: how long the badge listens for
 * the repeater's reply, and the numbers that decide it.
 *
 * The badge is the only thing that ever sends this range test a request, and
 * the repeater above is the only thing that ever answers one, so the two
 * budgets have to agree. They are built from different trees by different
 * compilers and share no header, which is why the badge keeps its own copy of
 * the delay and why this reads both.
 */
struct badge_scan {
	int turnaround_ms;      /* the badge's copy of the delay above */
	int poll_us;            /* the floor it sizes its budget with */
	int window_from_turnaround; /* its window is sized from the delay */
	char window_text[256];      /* as written */
	int polls_from_window;      /* and its poll budget from the window */
	char polls_text[256];       /* as written */
	/* The two SPI clock divisors and the wait before a FIFO read. A
	 * divisor is the reciprocal of a rate, so the slower clock is the
	 * larger number. */
	int brg_fast;
	int brg_fifo_read;
	int lead_polls;
};

static int scan_badge_header(const char *badge_path, struct badge_scan *b)
{
	char line[SCAN_LINE_MAX];
	int in_block = 0;
	FILE *f = fopen(badge_path, "r");

	if (f == NULL)
		return 0;
	memset(b, 0, sizeof(*b));
	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		if (b->turnaround_ms == 0) {
			int n = defined_number(line, "REPEATER_TURNAROUND_MS");

			if (n > 0)
				b->turnaround_ms = n;
		}
		if (b->poll_us == 0) {
			int n = defined_number(line, "SYNC_POLL_US");

			if (n > 0)
				b->poll_us = n;
		}
		if (b->brg_fast == 0) {
			int n = defined_number(line, "SPI_BRG_FAST");

			if (n > 0)
				b->brg_fast = n;
		}
		if (b->brg_fifo_read == 0) {
			int n = defined_number(line, "SPI_BRG_FIFO_READ");

			if (n > 0)
				b->brg_fifo_read = n;
		}
		if (b->lead_polls == 0) {
			int n = defined_number(line, "FIFO_LEAD_POLLS");

			if (n > 0)
				b->lead_polls = n;
		}
		/*
		 * Both of these are pinned as written, as the tick conversion
		 * above is: nothing here can run them, and a plain number in
		 * either place is how a window sized from the peer's delay
		 * silently becomes a window sized from nothing.
		 */
		if (strstr(line, "define SYNC_RECEIVED_MS") != NULL) {
			collapse_spaces(line, b->window_text,
			                sizeof(b->window_text));
			if (contains_identifier(b->window_text,
			                        "REPEATER_TURNAROUND_MS"))
				b->window_from_turnaround = 1;
		}
		if (strstr(line, "define SYNC_RECEIVED_POLLS") != NULL) {
			collapse_spaces(line, b->polls_text,
			                sizeof(b->polls_text));
			if (contains_identifier(b->polls_text,
			                        "SYNC_RECEIVED_MS") &&
			    contains_identifier(b->polls_text, "SYNC_POLL_US"))
				b->polls_from_window = 1;
		}
	}
	fclose(f);
	return 1;
}

/*
 * What the scan of the badge's own source records.
 *
 * The badge runs the same range test against the same radio, so the radio's
 * two framing requirements are the badge's too: a FIFO transfer gives the
 * FIFO's address once and then the data with chip select held low, and a FIFO
 * read needs a slower clock than a register read. The badge cannot share the
 * Ubertooth's helpers, which drive LPC17xx pins, so it has its own pair
 * written against the R8C's UART, and this reads them.
 *
 * Nothing here can run that code: no toolchain in this repository builds the
 * badge image and nobody here has a badge. Reading it as text is the only
 * check there is, which is why this one looks at the shape of both helpers
 * rather than only at the range test calling them.
 */
struct badge_source_scan {
	int perbyte_fifo;          /* a per-byte FIFO transfer, if one is left */
	char perbyte_text[256];    /* as written */
	int write_fn, read_fn;     /* where each helper is defined */
	int write_address_bytes;   /* lines sending the FIFO's address in it */
	int write_hold;            /* the hold before chip select rises */
	int read_address_bytes;
	int read_hold;
	int read_slow_clock;       /* the clock dropped for the read */
	int read_clock_restored;   /* and put back after it */
	int rangetest_fifo_write;  /* the range test's calls */
	int rangetest_fifo_read;
	int rangetest_sync_wait;   /* its wait for the radio to have the packet */
	int rangetest_lead;
};

static int scan_badge_source(const char *badge_source, struct badge_source_scan *b)
{
	char line[SCAN_LINE_MAX], name[128], current[128] = "";
	int in_block = 0, depth = 0, lineno = 0;
	FILE *f = fopen(badge_source, "r");

	if (f == NULL)
		return 0;
	memset(b, 0, sizeof(*b));

	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p;

		lineno++;
		strip_comments(line, &in_block);
		if (depth == 0 && call_name(line, name, sizeof(name)))
			snprintf(current, sizeof(current), "%s", name);

		/* The fault this is here for: one SPI transaction per byte,
		 * which writes nothing into the FIFO and reads nothing out.
		 * Named on one line, as it was written; and because that is a
		 * shape rather than a rule, a single-register transfer anywhere
		 * in the range test counts too -- it has no other use for one,
		 * and this scan is the only check this image gets. */
		if (b->perbyte_fifo == 0 &&
		    ((contains_identifier(line, "FIFOREG") &&
		      (contains_identifier(line, "cc2400_get8") ||
		       contains_identifier(line, "cc2400_set8"))) ||
		     (strcmp(current, "cc2400_range_test") == 0 &&
		      (calls_identifier(line, "cc2400_get8") ||
		       calls_identifier(line, "cc2400_set8"))))) {
			b->perbyte_fifo = lineno;
			collapse_spaces(line, b->perbyte_text,
			                sizeof(b->perbyte_text));
		}

		if (strcmp(current, "cc2400_fifo_write") == 0) {
			if (b->write_fn == 0)
				b->write_fn = lineno;
			if (contains_identifier(line, "FIFOREG"))
				b->write_address_bytes++;
			if (calls_identifier(line, "spi_delay"))
				b->write_hold = lineno;
		} else if (strcmp(current, "cc2400_fifo_read") == 0) {
			if (b->read_fn == 0)
				b->read_fn = lineno;
			if (contains_identifier(line, "FIFOREG"))
				b->read_address_bytes++;
			if (calls_identifier(line, "spi_delay"))
				b->read_hold = lineno;
			if (contains_identifier(line, "SPI_BRG_FIFO_READ"))
				b->read_slow_clock = lineno;
			if (contains_identifier(line, "SPI_BRG_FAST"))
				b->read_clock_restored = lineno;
		} else if (strcmp(current, "cc2400_range_test") == 0) {
			if (calls_identifier(line, "cc2400_fifo_write") &&
			    b->rangetest_fifo_write == 0)
				b->rangetest_fifo_write = lineno;
			if (calls_identifier(line, "cc2400_fifo_read") &&
			    b->rangetest_fifo_read == 0)
				b->rangetest_fifo_read = lineno;
			if (calls_identifier(line, "cc2400_wait_sync_received") &&
			    b->rangetest_sync_wait == 0)
				b->rangetest_sync_wait = lineno;
			if (calls_identifier(line, "fifo_lead_delay") &&
			    b->rangetest_lead == 0)
				b->rangetest_lead = lineno;
		}

		for (p = line; *p != '\0'; p++) {
			if (*p == '{')
				depth++;
			else if (*p == '}')
				depth--;
		}
	}

	fclose(f);
	return 1;
}

static struct scan s;
static struct spi_scan spi;
static struct badge_scan badge;
static struct badge_source_scan badge_src;
static const char *path = FW_RXTX_DIR "/cc2400_rangetest.c";
static const char *spi_path = FW_COMMON_DIR "/ubertooth.c";
static const char *badge_path = FW_DIR "/tc13badge/tc13badge.h";
static const char *badge_source = FW_DIR "/tc13badge/tc13badge.c";

/*
 * The deafness. Without MANAND the demodulator never settles on a packet, so
 * neither board decodes the other however strong the signal is.
 */
MU_TEST(test_both_modes_hold_the_gain_amplifier_out_of_reset)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_rangetest() does not write MANAND, so it hears no "
	         "sync word: the write at boot is discarded by the chip reset in "
	         "clock_start()", path);
	mu_assert(s.rangetest_manand > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_repeater() does not write MANAND, so it hears no "
	         "sync word and never answers a request", path);
	mu_assert(s.repeater_manand > 0, msg);
}

/*
 * The repeater has to state the packet format rather than inherit whatever the
 * last mode left in GRMDM.
 */
MU_TEST(test_the_repeater_sets_the_packet_format)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_repeater() does not write GRMDM, so it decodes "
	         "requests only while the radio still holds its reset value and "
	         "nothing at all after a mode that writes GRMDM", path);
	mu_assert(s.repeater_grmdm > 0, msg);
}

/*
 * The turnaround delay has to be a real wait. A counted loop over a local is
 * deleted by the compiler, and then the reply lands while the sender is still
 * transmitting.
 */
MU_TEST(test_the_turnaround_delay_is_a_timed_wait)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s:%d: `%s` counts a local with no side effects, so the "
	         "compiler deletes it and the delay does not happen",
	         path, s.first_countdown, s.countdown_text);
	mu_assert(s.countdowns == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: no repeater_turnaround() to wait in", path);
	mu_assert(s.turnaround_fn > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: repeater_turnaround() does not read clkn, so its delay "
	         "is not timed", path, s.turnaround_fn);
	mu_assert(s.turnaround_clkn, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: repeater_turnaround() does not service USB, so control "
	         "requests stall for its whole length", path, s.turnaround_fn);
	mu_assert(s.turnaround_service, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: repeater_turnaround() does not re-read requested_mode, "
	         "so -S cannot stop the repeater during the delay",
	         path, s.turnaround_fn);
	mu_assert(s.turnaround_mode, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_repeater() never calls repeater_turnaround(), so it "
	         "answers before the sender has finished its burst", path);
	mu_assert(s.turnaround_call > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: REPEATER_TURNAROUND_TICKS is `%s`, which does not convert "
	         "milliseconds at clkn's 3200 ticks a second: a delay written as "
	         "a bare tick count is wrong by a factor of thousands and nothing "
	         "else here would notice", path, s.ticks_text);
	mu_assert(s.ticks_from_clkn, msg);
}

/*
 * The repeater has to stop receiving before it waits, not after. The rest of
 * the sender's burst would otherwise arrive through the delay and fill the
 * 32-byte FIFO the reply is written into.
 */
MU_TEST(test_the_repeater_stops_receiving_before_it_waits)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_repeater() has no SRFOFF at all", path);
	mu_assert(s.rx_off_line > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the repeater's first SRFOFF is at line %d, after the "
	         "turnaround wait at line %d: the rest of the sender's burst "
	         "arrives into the reply's FIFO during the delay",
	         path, s.rx_off_line, s.turnaround_call);
	mu_assert(s.rx_off_line < s.turnaround_call, msg);
}

/*
 * rr is static. The serial-number read can return early, so clearing rr only
 * at the end of the test leaves `ubertooth-util -m` reporting the previous
 * run's levels as this one's.
 */
MU_TEST(test_the_result_is_cleared_before_the_test_can_return_early)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_rangetest() does not clear rr.valid before the "
	         "serial-number read at line %d, which can return without "
	         "measuring anything", path, s.iap_read);
	mu_assert(s.rr_cleared_early > 0, msg);
}

/*
 * Both modes reach the radio's FIFO the way every other mode does. Writing or
 * reading it a byte at a time, each byte its own SPI transfer, is what left
 * the range test comparing 22 zeros against its own payload: the write put
 * nothing in the FIFO, and a read at that clock speed returns the wrong
 * bytes.
 */
/*
 * Each side takes only a packet of its own exchange.
 *
 * The radio's CRC_OK flag says a packet is self consistent, not that it came
 * from the other end of this test. Taking any packet it accepted cost both
 * sides something: the sender reported another pair's range test, or a stray
 * packet, as `invalid range test: mismatch on byte <n>` and ended the run,
 * and the repeater answered a reply as readily as a request, so two repeaters
 * in range of each other could answer each other.
 */
MU_TEST(test_each_side_takes_only_its_own_packets)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the sender's CRC check, so the "
	         "checks below would pass without reading anything", path);
	mu_assert_fatal(s.rangetest_crc > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the sender does not call rangetest_foreign_packet(), so "
	         "any packet its CRC accepted is taken as the repeater's reply",
	         path, s.rangetest_crc);
	mu_assert_fatal(s.rangetest_filter > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the sender does not record what it discarded in "
	         "rr.discarded, so a run that ignored somebody else's packet "
	         "says nothing about it",
	         path, s.rangetest_filter);
	mu_assert(s.rangetest_discard > s.rangetest_filter, msg);

	/* And the answer decides. A loop that called the filter, recorded the
	 * discard and then took the packet anyway would pass the two checks
	 * above and change nothing on the air. */
	snprintf(msg, sizeof(msg),
	         "%s:%d: the filter's answer is not read between the call at "
	         "line %d and `got_reply = 1` at line %d, so nothing says the "
	         "answer decides whether the packet is taken",
	         path, s.rangetest_filter, s.rangetest_filter, s.rangetest_accept);
	mu_assert(s.rangetest_test > s.rangetest_filter &&
	          s.rangetest_accept > s.rangetest_test, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the repeater's CRC check", path);
	mu_assert_fatal(s.repeater_crc > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the repeater takes a packet without reading its type "
	         "byte on the same line, so it can answer another repeater's "
	         "reply", path, s.repeater_crc);
	mu_assert(s.repeater_type == s.repeater_crc, msg);
}

/*
 * And the sender's filter asks one question: is this packet's serial number
 * mine?
 *
 * Bytes 2 to 17 are the serial number. The bytes around it -- the length and
 * type before it, the levels after it -- are not part of the question: a
 * packet carrying this board's serial number is this test's own answer, and a
 * wrong byte in it says that answer arrived corrupted, which the full
 * comparison afterwards reports as a mismatch. A filter that took those bytes
 * in as well would discard a corrupted reply instead, and leave the run
 * waiting with nothing said.
 *
 * The expected payload has to carry the reply's type byte by the time the
 * filter runs, which is why the last check is about where that byte is
 * written: after the request burst has gone out with the request's type, and
 * before the receive loop reads the answer.
 */
MU_TEST(test_the_filter_asks_only_about_the_serial_number)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find rangetest_foreign_packet()'s "
	         "definition", path);
	mu_assert_fatal(s.filter_fn > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the filter compares from byte %d, not from byte 2: the "
	         "serial number starts there, and a corrupted length or type in "
	         "this board's own reply has to be reported and not discarded",
	         path, s.filter_fn, s.filter_fn_first);
	mu_assert(s.filter_fn_first == 2, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the filter compares up to byte %d; the 16-byte serial "
	         "number ends at 18, and bytes 18 to 21 carry the levels each "
	         "side fills in",
	         path, s.filter_fn, s.filter_fn_last);
	mu_assert(s.filter_fn_last == 18, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the filter does not return the index it found, so every "
	         "warning would name the same byte whichever one differed",
	         path, s.filter_fn);
	mu_assert(s.filter_fn_return > s.filter_fn, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the discard is recorded without asking whether one is "
	         "recorded already, so a run would report the last packet it "
	         "ignored where the host reports the first",
	         path, s.rangetest_discard);
	mu_assert(s.rangetest_first_only > 0 &&
	          s.rangetest_first_only < s.rangetest_discard, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find `txbuf[1] = 1`, which turns the "
	         "request that was sent into the reply the filter looks for",
	         path);
	mu_assert_fatal(s.rangetest_type_flip > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the reply's type byte is written at line %d, which is "
	         "not between the request burst's FIFO write at line %d and the "
	         "filter at line %d: written earlier the burst goes out as a "
	         "reply, which the repeater now ignores, and written later the "
	         "filter compares against the request's type byte",
	         path, s.rangetest_type_flip, s.rangetest_type_flip,
	         s.rangetest_fifo_write, s.rangetest_filter);
	mu_assert(s.rangetest_type_flip > s.rangetest_fifo_write &&
	          s.rangetest_type_flip < s.rangetest_filter, msg);
}

/*
 * A run that ends without a reply reports neither a result nor a discard, and
 * clears them in that order.
 *
 * `ubertooth-util -n` tells this run's discard from the run before it by
 * waiting for an empty result first, so the two have to be cleared together --
 * a run stopped after it had discarded somebody else's packet would otherwise
 * leave that record for a poll of the next run to read as its own. The
 * discard goes first for the same reason: an empty result is what tells the
 * host the record describes the run it is watching, so a read landing between
 * the two stores would take this run's discard for that one's.
 */
MU_TEST(test_a_run_without_a_reply_clears_what_it_discarded_first)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the `rr.valid = 0` on the exit that "
	         "reports no result", path);
	mu_assert_fatal(s.rangetest_valid_clear_late > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: that exit does not clear `rr.discarded` before the "
	         "result at line %d, so a read in between takes this run's "
	         "discarded packet for the next run's",
	         path, s.rangetest_discard_clear_late, s.rangetest_valid_clear_late);
	mu_assert(s.rangetest_discard_clear_late > 0 &&
	          s.rangetest_discard_clear_late < s.rangetest_valid_clear_late,
	          msg);
}

MU_TEST(test_the_fifo_is_reached_through_the_helpers)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s:%d: `%s` reaches the FIFO a byte at a time, one SPI transfer "
	         "each: the radio wants the address once and then the bytes while "
	         "CSN stays low, so a write like this lands nowhere and a read "
	         "comes back wrong. Use cc2400_fifo_write() and cc2400_fifo_read()",
	         path, s.perbyte_fifo, s.perbyte_text);
	mu_assert(s.perbyte_fifo == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_rangetest() does not call cc2400_fifo_write(), so its "
	         "request packets carry no data", path);
	mu_assert(s.rangetest_fifo_write > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_rangetest() does not call cc2400_fifo_read(), so it "
	         "cannot read the reply", path);
	mu_assert(s.rangetest_fifo_read > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_repeater() does not call cc2400_fifo_write(), so its "
	         "replies carry no data", path);
	mu_assert(s.repeater_fifo_write > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_repeater() does not call cc2400_fifo_read(), so it "
	         "cannot read the request", path);
	mu_assert(s.repeater_fifo_read > 0, msg);
}

/*
 * Neither mode may start clocking the packet out of the FIFO the moment the
 * radio reports a sync word.
 *
 * The flag is set when the sync word ends, before the first payload byte
 * exists, so a read that starts there returns a byte 0 the radio has not
 * written. That was `mismatch on byte 0` every five to ten range tests, and
 * only ever byte 0: the read is slower than the air, so from the second byte
 * on the radio stays ahead of it.
 */
MU_TEST(test_the_read_waits_for_the_radio_to_have_the_packet)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: no rangetest_wait_for_fifo() to wait in", path);
	mu_assert_fatal(s.fifo_wait_fn > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rangetest_wait_for_fifo() does not read T0TC, so its "
	         "wait is not timed: a counted loop here is deleted by the "
	         "compiler, as the turnaround delay was", path, s.fifo_wait_fn);
	mu_assert(s.fifo_wait_timer, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rangetest_wait_for_fifo() takes its step as `%s`. It has "
	         "to be `u32 step = (now + CLK100NS_PER_CLKN - last) %% "
	         "CLK100NS_PER_CLKN;`, which is what makes a step across T0TC's "
	         "reset come out as the time it was: those operands the other way "
	         "round over-count the step and end the wait early, which is the "
	         "bug this wait is here to fix",
	         path, s.fifo_wait_fn, s.fifo_wait_text);
	mu_assert(s.fifo_wait_step, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rangetest_wait_for_fifo() has no "
	         "`if (step <= RANGETEST_LEAD_100NS)` line, written just so, so "
	         "every step the modulo returns is counted. The timer's period is "
	         "not CLK100NS_PER_CLKN for the one period a clock drift "
	         "correction lengthens, and a step across that period's reset "
	         "comes out of the fixed modulus as an arbitrary number: counting "
	         "it ends the wait at once, which is the wrong byte 0 the wait is "
	         "for", path, s.fifo_wait_fn);
	mu_assert(s.fifo_wait_guard, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rangetest_wait_for_fifo() adds to its total %d times, "
	         "the last as `%s`. One `waited += step;` is the whole of it: "
	         "anything else, or a second one, waits for a length no test here "
	         "knows", path, s.fifo_wait_fn, s.fifo_wait_adds, s.fifo_wait_add);
	mu_assert(s.fifo_wait_adds == 1 && s.fifo_wait_add_ok > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the guard is at line %d and the addition it stands in front "
	         "of at line %d. The addition has to be the next line: an empty "
	         "guard body leaves the guard reading correctly above an addition "
	         "it no longer reaches, which counts every step again",
	         path, s.fifo_wait_guard, s.fifo_wait_add_ok);
	mu_assert(s.fifo_wait_add_ok == s.fifo_wait_guard + 1, msg);

	/*
	 * And the whole function, so that nothing can be added between the
	 * lines the checks above name. Each of them can be satisfied while the
	 * wait is taken apart around it.
	 */
	snprintf(msg, sizeof(msg),
	         "%s:%d: rangetest_wait_for_fifo() reads\n  %s\nand it has to "
	         "read\n  %s\nComments are not compared, so only the code has "
	         "changed. Anything added between these lines -- another write to "
	         "`waited` or to `step`, a second guard -- waits for a length no "
	         "test here knows, and the wait exists to be exactly this long",
	         path, s.fifo_wait_fn, s.fifo_wait_body, FIFO_WAIT_BODY);
	mu_assert(strcmp(s.fifo_wait_body, FIFO_WAIT_BODY) == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rangetest_wait_for_fifo() does not wait "
	         "RANGETEST_LEAD_100NS", path, s.fifo_wait_fn);
	mu_assert(s.fifo_wait_length, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rangetest_wait_for_fifo() has no RANGETEST_LEAD_POLLS "
	         "cap, so a timer 0 that is not running leaves it spinning and "
	         "the board answering nothing until the watchdog resets it",
	         path, s.fifo_wait_fn);
	mu_assert(s.fifo_wait_cap, msg);

	/*
	 * And the cap has to be larger than the number of turns the wait needs,
	 * or it ends the wait instead of backstopping it -- which is the early
	 * read again, and the body above would not change by one character.
	 */
	snprintf(msg, sizeof(msg),
	         "%s: RANGETEST_LEAD_POLLS is %d. A turn of the wait is a few "
	         "ticks, so the %d ticks it waits take a few hundred turns: a cap "
	         "near that is what ends the wait, not a backstop against a timer "
	         "that is not running", path, s.fifo_wait_polls,
	         s.lead_bytes * s.byte_100ns);
	mu_assert(s.fifo_wait_polls >= 10000, msg);

	/*
	 * Everything above reads the text of the first definition of each of the
	 * wait's numbers. The compiler reads the last one.
	 */
	snprintf(msg, sizeof(msg),
	         "%s: the wait's four numbers are defined %d times between them, "
	         "and one of them is undefined again at line %d. Each is defined "
	         "once and stays defined: a second definition is the value the "
	         "wait is built with, and every check here would still read the "
	         "first", path, s.wait_defines, s.wait_undef);
	mu_assert(s.wait_defines == 4 && s.wait_undef == 0, msg);

	/*
	 * The wait has to be built wherever the read it comes before is built.
	 * Both sit inside the file's `#ifdef TX_ENABLE`, so what this compares
	 * is their depth against each other rather than against zero: a
	 * conditional that is never true -- `#if 0`, `#ifdef NEVER` -- around
	 * the wait alone would leave every check above it passing and take the
	 * wait out of the build.
	 */
	snprintf(msg, sizeof(msg),
	         "%s: in the range test the wait is %d deep in #if/#ifdef and the "
	         "read it comes before is %d deep; in the repeater, %d and %d. A "
	         "wait built under a condition the read is not is a wait that can "
	         "be left out of the firmware with every check here passing",
	         path, s.rangetest_wait_depth, s.rangetest_read_depth,
	         s.repeater_wait_depth, s.repeater_read_depth);
	mu_assert(s.rangetest_wait_depth == s.rangetest_read_depth &&
	          s.repeater_wait_depth == s.repeater_read_depth, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: a line of rangetest_wait_for_fifo() ends in a backslash, "
	         "which joins it to the line below. A comment written that way "
	         "takes the next line into itself, and the checks here read that "
	         "line as code", path, s.fifo_wait_joined);
	mu_assert(s.fifo_wait_joined == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_rangetest() notices a sync word at line %d and reads "
	         "the FIFO at line %d without waiting in between, so byte 0 of "
	         "the reply comes back wrong every five to ten runs",
	         path, s.rangetest_sync_poll, s.rangetest_fifo_read);
	mu_assert(s.rangetest_wait > s.rangetest_sync_poll &&
	          s.rangetest_wait < s.rangetest_fifo_read, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_repeater() notices a sync word at line %d and reads "
	         "the FIFO at line %d without waiting in between: the serial "
	         "number it copies from the request into its reply is read a byte "
	         "early", path, s.repeater_sync_poll, s.repeater_fifo_read);
	mu_assert(s.repeater_wait > s.repeater_sync_poll &&
	          s.repeater_wait < s.repeater_fifo_read, msg);

	/*
	 * The counter the wait reads only advances once the timer is enabled, so
	 * a mode that forgot this would wait in it forever. Checked against the
	 * sync poll rather than the wait, so this says what it means when the
	 * wait itself is the thing that has gone missing.
	 */
	snprintf(msg, sizeof(msg),
	         "%s: cc2400_rangetest() does not call clkn_start() before the "
	         "receive loop at line %d, so T0TC does not advance and the wait "
	         "before the FIFO read never ends", path, s.rangetest_sync_poll);
	mu_assert(s.rangetest_clkn > 0 &&
	          s.rangetest_clkn < s.rangetest_sync_poll, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_repeater() does not call clkn_start() before the "
	         "receive loop at line %d, so T0TC does not advance and the wait "
	         "before the FIFO read never ends", path, s.repeater_sync_poll);
	mu_assert(s.repeater_clkn > 0 && s.repeater_clkn < s.repeater_sync_poll,
	          msg);
}

/*
 * How long that wait is. One byte of air time is enough, because the wait
 * starts when the sync word has already been noticed; a few more cover the
 * jitter of the delay loop and of the radio writing the byte. It must stay
 * well short of the packet, though: the rest of the sender's burst keeps
 * arriving into the same 32-byte FIFO while the packet is read out of it.
 */
MU_TEST(test_the_wait_is_a_few_bytes_of_air_time)
{
	char msg[768];
	/* A byte at the 1 Mbit/s these packets go out at, in 100 ns ticks. */
	const int byte_100ns = 8 * 1000 / 100;

	snprintf(msg, sizeof(msg),
	         "%s: RANGETEST_BYTE_100NS is %d, not the %d ticks of 100 ns a "
	         "byte takes at 1 Mbit/s", path, s.byte_100ns, byte_100ns);
	mu_assert(s.byte_100ns == byte_100ns, msg);

	/*
	 * The upper bound is a quarter of the radio's 32-byte FIFO. Every byte
	 * waited for is a byte of the sender's burst sitting in that FIFO while
	 * the packet is read out of it, and the read is only safe while the FIFO
	 * does not fill.
	 */
	snprintf(msg, sizeof(msg),
	         "%s: RANGETEST_LEAD_BYTES is %d. It needs at least the one byte "
	         "the radio has not written yet when it reports the sync word, "
	         "and no more than 8, a quarter of the radio's 32-byte FIFO, "
	         "which is holding the burst while the packet is read out",
	         path, s.lead_bytes);
	mu_assert(s.lead_bytes >= 1 && s.lead_bytes <= 8, msg);

	snprintf(msg, sizeof(msg),
	         "%s: RANGETEST_LEAD_100NS is `%s`, which is not "
	         "RANGETEST_LEAD_BYTES byte times: a wait written as a bare tick "
	         "count is wrong by a factor of tens and nothing else here would "
	         "notice", path, s.lead_text);
	mu_assert(s.lead_from_byte, msg);

	/*
	 * And the lead has to be shorter than the counter's own period, or the
	 * guard in the wait has nothing left to drop: a step out of the fixed
	 * modulus is always under CLK100NS_PER_CLKN, so a lead at or above it
	 * admits every step, however wrong.
	 */
	snprintf(msg, sizeof(msg),
	         "%s: the lead is %d ticks and the counter resets every %d. A "
	         "step the modulo returns is always under that, so a lead this "
	         "long is a lead no step can exceed, and the wait counts a step "
	         "taken in a period the drift correction lengthened as if it were "
	         "real", path, s.lead_bytes * s.byte_100ns, CLK100NS_PER_CLKN);
	mu_assert(s.lead_bytes * s.byte_100ns < CLK100NS_PER_CLKN, msg);
}

/*
 * The shared FIFO transfers, which the two modes above now depend on.
 *
 * The write must clock the address and the data and nothing else. It used to
 * clock a further eight pulses after the data, which the radio took as one
 * more FIFO byte, so every packet carried a stray byte. With the radio's CRC
 * off that is a tail the receiver ignores; with it on, as the range test has
 * it, the CRC covers the stray byte and no receiver can match it.
 *
 * The read must keep its per-bit delays. At the speed cc2400_spi() clocks at
 * a FIFO read returns the wrong bytes, whatever the framing.
 */
MU_TEST(test_the_shared_fifo_transfers_keep_their_shape)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_fifo_write() clocks the bus in %d places, not the two "
	         "it needs (the address byte and the data). A third clocks bits "
	         "that are not data, and the radio takes them as another FIFO byte",
	         spi_path, spi.write_clock_sites);
	mu_assert(spi.write_clock_sites == 2, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_fifo_write() does not call spi_delay(), so CSN rises "
	         "before the radio has taken the last byte", spi_path);
	mu_assert(spi.write_hold > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_fifo_read() has %d spi_delay() calls. It needs one on "
	         "each side of every clock edge plus the hold before CSN rises: "
	         "read at cc2400_spi()'s speed the FIFO returns wrong bytes",
	         spi_path, spi.read_delays);
	mu_assert(spi.read_delays >= 3, msg);
}

/*
 * One copy of the range test.
 *
 * The assembly_test image carried a twin of these two functions, copied long
 * before and never kept up. Everything the tests above pin was fixed here and
 * not there -- the gain amplifier write, the FIFO going through the helpers,
 * the turnaround delay a compiler does not delete -- and no step of the
 * assembly test procedure reached it, so nothing would have noticed. It is
 * gone, and this walks firmware/ so another cannot arrive unnoticed.
 *
 * The ToorCon badge is skipped. Its cc2400_range_test() is a different
 * function for a different part, with its own register map and its own
 * compiler, and shares no source with this image, so it is not a copy of this
 * one and could not be merged into it.
 */
static const char *const copied_functions[] = {
	"cc2400_rangetest",
	"cc2400_repeater",
};

/* Directories under firmware/ that hold no image of this part: the badge, for
 * the reason above, and the host-compiled tests, which name these functions in
 * strings and in lists like the one above. */
static const char *const not_an_image[] = { "tc13badge", "tests" };

/* Is the last thing on this line a semicolon? A declaration of one of these
 * functions ends that way and a definition does not, so this is what keeps a
 * prototype from being reported as a second copy. */
static int ends_in_semicolon(const char *line)
{
	size_t length = strlen(line);

	while (length > 0 && isspace((unsigned char)line[length - 1]))
		length--;
	return length > 0 && line[length - 1] == ';';
}

/* How many of these definitions one C source has, printing each. */
static int copies_in(const char *file)
{
	FILE *source = fopen(file, "r");
	char line[512];
	int in_block = 0, lineno = 0, found = 0;

	if (source == NULL)
		return 0;
	while (fgets(line, sizeof(line), source) != NULL) {
		size_t i;

		lineno++;
		strip_comments(line, &in_block);
		/* A definition, not a call, a declaration or an include: these
		 * are all written with the return type in the first column and
		 * the parameter list on the same line, every call to one is
		 * inside a function and so indented, and a declaration ends in
		 * a semicolon. */
		if (isspace((unsigned char)line[0]) || line[0] == '#' ||
		    strchr(line, '(') == NULL || ends_in_semicolon(line))
			continue;
		for (i = 0; i < sizeof(copied_functions) /
		                sizeof(copied_functions[0]); ++i) {
			if (!contains_identifier(line, copied_functions[i]))
				continue;
			printf("  FAIL %s:%d is a second copy of %s(): %s",
			       file, lineno, copied_functions[i], line);
			found++;
		}
	}
	fclose(source);
	return found;
}

/*
 * The C sources the walk covers, as a shell pattern: every one under
 * firmware/, at any depth, other than the two directories above. find is used
 * rather than a directory walk of its own because btbr keeps its sources three
 * levels down, and telling a directory from a file portably is more code than
 * this test is worth.
 */
#define FIND_SOURCES "find " FW_DIR " -name '*.c'"

/* Sources the tree has today. The walk checks it found at least this many, so
 * one that stopped finding them fails rather than passing on nothing. */
#define SOURCES_EXPECTED 60

/*
 * Count the definitions in every one of those sources other than this image's
 * own, and say how many were read. -1 if the listing cannot be run.
 */
static int count_copies(int *sources)
{
	char command[1024];
	char file[512];
	FILE *listing;
	size_t i;
	int found = 0;

	snprintf(command, sizeof(command), "%s", FIND_SOURCES);
	for (i = 0; i < sizeof(not_an_image) / sizeof(not_an_image[0]); ++i) {
		char clause[128];

		snprintf(clause, sizeof(clause), " -not -path '%s/%s/*'",
		         FW_DIR, not_an_image[i]);
		strncat(command, clause, sizeof(command) - strlen(command) - 1);
	}

	*sources = 0;
	listing = popen(command, "r");
	if (listing == NULL)
		return -1;
	while (fgets(file, sizeof(file), listing) != NULL) {
		size_t length = strlen(file);

		while (length > 0 && file[length - 1] == '\n')
			file[--length] = '\0';
		(*sources)++;
		if (strcmp(file, path) == 0)
			continue;
		found += copies_in(file);
	}
	if (pclose(listing) != 0)
		return -1;
	return found;
}

/*
 * The badge listens long enough to hear the reply.
 *
 * The repeater holds its reply so that the sender has finished its burst and
 * armed its receiver, and the badge is a sender with no host to tell it to
 * keep waiting: a window shorter than the delay means the badge has given up
 * before the reply is sent, and the test cannot pass however good the link
 * is. The budget it had for years, 28561 polls, came to about 90 ms of a
 * 1000 ms delay.
 *
 * Checked as three numbers and two definitions rather than as a duration,
 * because none of it can run here: the delay is the same on both sides, the
 * poll cost the badge sizes with is no longer than one poll really is, and
 * each of the badge's two steps is written in terms of the step before it.
 */
MU_TEST(test_the_badge_listens_past_the_repeaters_turnaround)
{
	char why[512];

	snprintf(why, sizeof(why),
	         "%s does not define REPEATER_TURNAROUND_MS as a plain number",
	         badge_path);
	mu_assert_fatal(badge.turnaround_ms > 0, why);
	snprintf(why, sizeof(why), "%s does not define SYNC_POLL_US",
	         badge_path);
	mu_assert_fatal(badge.poll_us > 0, why);

	snprintf(why, sizeof(why),
	         "%s holds the reply for %d ms but %s sizes its listening from "
	         "%d ms, so the badge waits for a reply that is sent after it "
	         "has given up",
	         path, s.turnaround_ms, badge_path, badge.turnaround_ms);
	mu_assert(badge.turnaround_ms == s.turnaround_ms, why);

	/*
	 * A poll of the radio's status is one byte on the badge's UART at
	 * 2.5 MHz, so 3.2 us, and the chip-select writes and the loop add to
	 * it. A larger figure here would make the budget fewer polls than the
	 * window it is meant to be.
	 */
	snprintf(why, sizeof(why),
	         "%s: SYNC_POLL_US is %d, longer than one poll of the radio's "
	         "status takes, so the window is shorter than it says",
	         badge_path, badge.poll_us);
	mu_assert(badge.poll_us <= 3, why);

	snprintf(why, sizeof(why),
	         "%s: SYNC_RECEIVED_MS is `%s`, which is not written in terms "
	         "of REPEATER_TURNAROUND_MS",
	         badge_path, badge.window_text);
	mu_assert(badge.window_from_turnaround, why);
	snprintf(why, sizeof(why),
	         "%s: SYNC_RECEIVED_POLLS is `%s`, which is not written in "
	         "terms of SYNC_RECEIVED_MS and SYNC_POLL_US",
	         badge_path, badge.polls_text);
	mu_assert(badge.polls_from_window, why);
}

/*
 * The badge's half of the same fault. Its range test reached the radio's FIFO
 * a byte at a time, so it wrote an empty FIFO and read zeros out of one. The
 * radio's checksum flag is set by exactly that, so the badge got as far as
 * believing a packet had arrived, and then the comparison against what it had
 * sent failed at the first byte -- which is why its range test could not pass
 * however close the two boards were.
 */
MU_TEST(test_the_badge_reaches_the_fifo_through_its_own_helpers)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s:%d: `%s` is one SPI transaction per byte, which writes "
	         "nothing into the FIFO and reads nothing out of it",
	         badge_source, badge_src.perbyte_fifo, badge_src.perbyte_text);
	mu_assert(badge_src.perbyte_fifo == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: no cc2400_fifo_write() for the badge to transmit with",
	         badge_source);
	mu_assert(badge_src.write_fn > 0, msg);
	snprintf(msg, sizeof(msg),
	         "%s: no cc2400_fifo_read() for the badge to receive with",
	         badge_source);
	mu_assert(badge_src.read_fn > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_range_test() does not transmit through "
	         "cc2400_fifo_write()", badge_source);
	mu_assert(badge_src.rangetest_fifo_write > 0, msg);
	snprintf(msg, sizeof(msg),
	         "%s: cc2400_range_test() does not receive through "
	         "cc2400_fifo_read()", badge_source);
	mu_assert(badge_src.rangetest_fifo_read > 0, msg);
}

/*
 * The shape of the badge's two helpers: the FIFO's address once rather than
 * once per byte, the datasheet's hold before chip select rises, and the
 * slower clock a FIFO read needs, put back when the read is done.
 */
MU_TEST(test_the_badges_fifo_transfers_keep_their_shape)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_fifo_write() names FIFOREG %d times, not once: "
	         "the radio takes the address once and then the data",
	         badge_source, badge_src.write_fn, badge_src.write_address_bytes);
	mu_assert(badge_src.write_address_bytes == 1, msg);
	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_fifo_read() names FIFOREG %d times, not once",
	         badge_source, badge_src.read_fn, badge_src.read_address_bytes);
	mu_assert(badge_src.read_address_bytes == 1, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_fifo_write() does not hold chip select after "
	         "the last clock (the datasheet's t_ps)",
	         badge_source, badge_src.write_fn);
	mu_assert(badge_src.write_hold > 0, msg);
	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_fifo_read() does not hold chip select after "
	         "the last clock", badge_source, badge_src.read_fn);
	mu_assert(badge_src.read_hold > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_fifo_read() does not drop the clock to "
	         "SPI_BRG_FIFO_READ, so it reads wrong bytes however it is "
	         "framed", badge_source, badge_src.read_fn);
	mu_assert(badge_src.read_slow_clock > 0, msg);
	snprintf(msg, sizeof(msg),
	         "%s:%d: cc2400_fifo_read() does not put the clock back to "
	         "SPI_BRG_FAST, so every later transfer runs at the slow one",
	         badge_source, badge_src.read_fn);
	mu_assert(badge_src.read_clock_restored > 0, msg);
	snprintf(msg, sizeof(msg),
	         "%s: cc2400_fifo_read() restores the clock at line %d, before "
	         "it drops it at %d, so the read itself runs at the fast one",
	         badge_source, badge_src.read_clock_restored,
	         badge_src.read_slow_clock);
	mu_assert(badge_src.read_slow_clock < badge_src.read_clock_restored, msg);

	snprintf(msg, sizeof(msg),
	         "%s: SPI_BRG_FIFO_READ is %d and SPI_BRG_FAST is %d; a divisor "
	         "is the reciprocal of a rate, so the read's has to be the "
	         "larger of the two", badge_path,
	         badge.brg_fifo_read, badge.brg_fast);
	mu_assert(badge.brg_fifo_read > badge.brg_fast, msg);
}

/*
 * The badge waits before it reads, for the same reason the Ubertooth does:
 * the radio raises its sync-word flag before the first payload byte is in the
 * FIFO, so a read that starts there gets a byte that does not exist yet.
 */
MU_TEST(test_the_badge_waits_for_the_radio_to_have_the_packet)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: no FIFO_LEAD_POLLS for the badge to wait", badge_path);
	mu_assert(badge.lead_polls > 0, msg);

	/*
	 * And it has to be the right length, for the same two reasons the
	 * Ubertooth's RANGETEST_LEAD_BYTES does: at least the one byte the
	 * radio has not written when it reports the sync word, and no more
	 * than a quarter of its 32-byte FIFO, which is holding the repeater's
	 * burst while the packet is read out of it.
	 *
	 * The badge counts nops rather than ticks, so the bound is in turns of
	 * that loop. One turn is a nop and the loop's decrement and branch,
	 * between 2 and 10 of this part's 50 ns cycles, so the wait is between
	 * polls/10 and polls/2 microseconds. Both ends have to land in the
	 * 8 to 64 us the two reasons leave.
	 */
	snprintf(msg, sizeof(msg),
	         "%s: FIFO_LEAD_POLLS is %d, so the wait is %d to %d us. It has "
	         "to be at least the 8 us of one byte at its fastest, and no "
	         "more than the 64 us of a quarter FIFO at its slowest",
	         badge_path, badge.lead_polls, badge.lead_polls / 10,
	         badge.lead_polls / 2);
	mu_assert(badge.lead_polls / 10 >= 8 && badge.lead_polls / 2 <= 64, msg);

	/*
	 * The read's clock, against the rate it is derived from rather than
	 * only against the fast one: in clock synchronous serial mode the bit
	 * rate is f1 / (2 * (u0brg + 1)), f1 is 20 MHz, and the Ubertooth's
	 * working FIFO read clocks at about 570 kbit/s.
	 */
	if (badge.brg_fifo_read > 0) {
		int kbit = 20000 / (2 * (badge.brg_fifo_read + 1));

		snprintf(msg, sizeof(msg),
		         "%s: SPI_BRG_FIFO_READ is %d, which clocks the read at "
		         "%d kbit/s. The Ubertooth reads its FIFO at about 570 "
		         "and gets the bytes right; 500 to 600 is the band "
		         "around that", badge_path, badge.brg_fifo_read, kbit);
		mu_assert(kbit >= 500 && kbit <= 600, msg);
	}

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_range_test() does not wait after the sync word, "
	         "so byte 0 of a reply is whatever the FIFO holds",
	         badge_source);
	mu_assert(badge_src.rangetest_lead > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the wait is at line %d, the sync word at %d and the read "
	         "at %d: it has to come between them", badge_source,
	         badge_src.rangetest_lead, badge_src.rangetest_sync_wait,
	         badge_src.rangetest_fifo_read);
	mu_assert(badge_src.rangetest_sync_wait > 0 &&
	          badge_src.rangetest_lead > badge_src.rangetest_sync_wait &&
	          badge_src.rangetest_lead < badge_src.rangetest_fifo_read, msg);
}

MU_TEST(test_the_range_test_has_one_copy)
{
	int sources = 0;
	int copies = count_copies(&sources);
	char why[256];

	mu_assert_fatal(copies >= 0, "cannot list the sources under " FW_DIR);
	snprintf(why, sizeof(why),
	         "the walk read %d C sources under " FW_DIR ", fewer than the "
	         "%d the tree has", sources, SOURCES_EXPECTED);
	mu_assert_fatal(sources >= SOURCES_EXPECTED, why);
	mu_assert_int_eq(0, copies);
}

/* ---- the arithmetic the wait before the FIFO read is built on ---------- */

/*
 * Run here because the firmware's own copy reads a hardware register this
 * cannot.
 *
 * Timer 0 resets every CLK100NS_PER_CLKN ticks, so the wait adds up the steps
 * it sees instead of comparing against a deadline the reset would carry it
 * past. `(now + period - last) % period` is what makes a step across the reset
 * come out as the time it was. This is a copy of the loop, not the firmware's
 * own: what pins the firmware to this arithmetic and to the guard below is the
 * source scan above, and what these say is the behavior that arithmetic has to
 * come to. The generic receive has the same accumulation, with its own
 * runnable model, in test_grmdm_rx_generic.c.
 */
#define CLKN_PERIOD ((uint32_t)CLK100NS_PER_CLKN)  /* 312.5 us in 100 ns ticks */

/*
 * The two period lengths a clock drift fix produces, in the same ticks. The
 * clock interrupt in bluetooth_rxtx.c writes 3124 plus its offset to the
 * timer's match register, and the fix sets that offset to 1 for a clock
 * running fast and 6249 for one running slow; a period is one tick longer than
 * the match value. Nothing scans those two numbers, so a change to them wants
 * these changed with it.
 */
#define FAST_FIX_PERIOD 3126u
#define SLOW_FIX_PERIOD 9374u

/*
 * The lead the firmware waits, four 1 Mbit/s byte times. The counts below are
 * for this length, so the sweep checks it against the firmware's own figures
 * rather than assuming them.
 */
#define MODEL_LEAD 320u

/*
 * The firmware's loop, with the counter supplied instead of read. uint32_t
 * throughout, as T0TC and the firmware's own variables are: the wraparound of
 * that width is what the modulo is there to turn back into a time, and a
 * 64-bit model of it would give different numbers.
 */
static uint32_t accumulate(const uint32_t *counter, unsigned n,
                           uint32_t target, uint32_t cap)
{
	uint32_t last = counter[0], waited = 0, polls = cap;
	unsigned i = 1;

	while (waited < target && polls-- > 0) {
		uint32_t now = counter[i < n ? i : n - 1];
		uint32_t step = (now + CLKN_PERIOD - last) % CLKN_PERIOD;

		i++;
		last = now;
		/* The firmware's own guard: a step longer than the whole lead
		 * is not a step this loop took, so the modulo was taken against
		 * the wrong period. */
		if (step <= target)
			waited += step;
	}
	return waited;
}

MU_TEST(test_the_wait_adds_up_steps_across_the_counters_reset)
{
	/* A counter advancing 100 ticks a turn reaches 320 on the fourth. */
	static const uint32_t plain[] = { 0, 100, 200, 300, 400, 500 };
	/* And one that resets partway: 3000, 3100, then 25 and on. The step
	 * from 3100 to 25 is 50 ticks of real time, not a huge number. */
	static const uint32_t wraps[] = { 3000, 3100, 25, 125, 225, 325 };
	uint32_t got;

	got = accumulate(plain, 6, 320, 1000);
	mu_assert(got >= 320 && got < 420, "a plain count must reach the lead");

	got = accumulate(wraps, 6, 320, 1000);
	mu_assert(got >= 320 && got < 420,
	          "a count that crosses the counter's reset must still add up "
	          "to the lead, not end the wait at once");

	/* The subtraction the modulo replaces: `now - last` across the reset is
	 * a huge unsigned number, which is the bug the modulo is there for. */
	mu_assert((uint32_t)(25u - 3100u) > 320u,
	          "a bare subtraction across the reset must be the large number "
	          "this guards against");

	/*
	 * The period is not always CLK100NS_PER_CLKN: the clock interrupt's
	 * drift correction moves the timer's match value for one period,
	 * which can make it about three times as long. A step across the reset
	 * of such a period comes out of the fixed modulus as an arbitrary
	 * number -- 2,056 for the pair below -- and counting it would end the
	 * wait before the radio has written byte 0, which is the `mismatch on
	 * byte 0` the wait is for. The guard drops it, so the wait carries on
	 * and ends on honest steps.
	 */
	{
		static const uint32_t extended[] = {
			9000, 10, 110, 210, 310, 410, 510
		};

		mu_assert_int_eq(2056, (int)((10u + CLKN_PERIOD - 9000u) %
		                             CLKN_PERIOD));
		got = accumulate(extended, 7, 320, 1000);
		mu_assert(got >= 320 && got < 420,
		          "a step out of a period that was not the usual length "
		          "must be dropped, not counted");
	}

	/* A counter that is not advancing is ended by the cap, not by the
	 * target. */
	{
		static const uint32_t stuck[] = { 7, 7 };

		got = accumulate(stuck, 2, 320, 50);
		mu_assert(got < 320, "a stopped counter must give up short");
	}
}

/*
 * What the guard counts, and what it drops, over a whole period.
 *
 * Every pair of readings taken across the reset of a period `length` ticks
 * long is tried. `real` is the time that actually passed between the two
 * readings, which the firmware cannot know; `step` is what the fixed modulus
 * makes of them.
 *
 * Two numbers come out. The first is how many single steps end the wait on
 * their own with less than the lead behind them, which is what the fault looks
 * like. The second is the most any counted step over-states the time it stands
 * for, and it is the one that settles the question: while no counted step
 * over-states its time, the running total can never be ahead of the clock, so
 * no number of them can end the wait early either.
 */
struct period_sweep {
	long ends_the_wait_early;  /* with the guard in place */
	long would_without_guard;  /* if every step were counted */
	uint32_t worst_over_count; /* counted step minus the time it stood for */
};

static struct period_sweep sweep_period(uint32_t length)
{
	struct period_sweep r = { 0, 0, 0 };
	uint32_t last, now;

	for (last = 1; last < length; last++) {
		for (now = 0; now < last; now++) {
			uint32_t step = (now + CLKN_PERIOD - last) % CLKN_PERIOD;
			uint32_t real = now + length - last;

			if (step >= MODEL_LEAD && real < MODEL_LEAD)
				r.would_without_guard++;
			if (step > MODEL_LEAD)
				continue;    /* the guard drops it */
			if (step >= MODEL_LEAD && real < MODEL_LEAD)
				r.ends_the_wait_early++;
			if (step > real && step - real > r.worst_over_count)
				r.worst_over_count = step - real;
		}
	}
	return r;
}

/*
 * No reading of a period a drift fix lengthened can end the wait before the
 * lead has really passed.
 *
 * A fix for a clock running slow is the one that matters: 51,040 pairs of
 * readings across the reset of that period used to end the wait with less than
 * the lead behind them, as little as one tick of it. None do now, and no step
 * the guard still counts over-states the time it stands for, so an
 * accumulation of them cannot end the wait early either.
 *
 * These are the two lengths a drift fix produces. The host can set the timer's
 * match value to a length of its own, and for some of those a step out of the
 * wrong period is small enough to pass the guard; that is a wider fix than
 * this wait, since it would move the generic receive's copy too, and it is
 * tracked on its own.
 */
MU_TEST(test_no_reading_of_a_drift_fixed_period_ends_the_wait_early)
{
	struct period_sweep slow = sweep_period(SLOW_FIX_PERIOD);
	struct period_sweep fast = sweep_period(FAST_FIX_PERIOD);
	char msg[256];

	snprintf(msg, sizeof(msg),
	         "the firmware waits %d byte times of %d ticks, %d in all, and "
	         "this sweep is written for %u: its counts no longer describe "
	         "the firmware", s.lead_bytes, s.byte_100ns,
	         s.lead_bytes * s.byte_100ns, MODEL_LEAD);
	mu_assert((uint32_t)(s.lead_bytes * s.byte_100ns) == MODEL_LEAD, msg);

	mu_assert_int_eq(51040, (int)slow.would_without_guard);
	mu_assert_int_eq(0, (int)fast.would_without_guard);

	snprintf(msg, sizeof(msg),
	         "%ld readings of the long period still end the wait before the "
	         "lead has passed", slow.ends_the_wait_early);
	mu_assert(slow.ends_the_wait_early == 0, msg);

	snprintf(msg, sizeof(msg),
	         "a counted step over-states the time it stands for by %u ticks, "
	         "so enough of them end the wait before the lead has passed",
	         slow.worst_over_count);
	mu_assert(slow.worst_over_count == 0, msg);

	snprintf(msg, sizeof(msg),
	         "the period a fix for a fast clock leaves is one tick longer "
	         "than the usual one, and %ld readings of it end the wait early "
	         "(over-count %u)", fast.ends_the_wait_early,
	         fast.worst_over_count);
	mu_assert(fast.ends_the_wait_early == 0 && fast.worst_over_count == 0,
	          msg);
}

int main(void)
{
	printf("range test radio setup tests\n");

	if (!scan_source(path, &s)) {
		printf("cannot open %s to scan\n", path);
		return 1;
	}
	if (!scan_spi_source(spi_path, &spi)) {
		printf("cannot open %s to scan\n", spi_path);
		return 1;
	}
	if (!scan_badge_header(badge_path, &badge)) {
		printf("cannot open %s to scan\n", badge_path);
		return 1;
	}
	if (!scan_badge_source(badge_source, &badge_src)) {
		printf("cannot open %s to scan\n", badge_source);
		return 1;
	}

	MU_RUN(test_both_modes_hold_the_gain_amplifier_out_of_reset);
	MU_RUN(test_the_repeater_sets_the_packet_format);
	MU_RUN(test_the_turnaround_delay_is_a_timed_wait);
	MU_RUN(test_the_repeater_stops_receiving_before_it_waits);
	MU_RUN(test_the_result_is_cleared_before_the_test_can_return_early);
	MU_RUN(test_the_fifo_is_reached_through_the_helpers);
	MU_RUN(test_each_side_takes_only_its_own_packets);
	MU_RUN(test_a_run_without_a_reply_clears_what_it_discarded_first);
	MU_RUN(test_the_filter_asks_only_about_the_serial_number);
	MU_RUN(test_the_read_waits_for_the_radio_to_have_the_packet);
	MU_RUN(test_the_wait_is_a_few_bytes_of_air_time);
	MU_RUN(test_the_shared_fifo_transfers_keep_their_shape);
	MU_RUN(test_the_badge_listens_past_the_repeaters_turnaround);
	MU_RUN(test_the_badge_reaches_the_fifo_through_its_own_helpers);
	MU_RUN(test_the_badges_fifo_transfers_keep_their_shape);
	MU_RUN(test_the_badge_waits_for_the_radio_to_have_the_packet);
	MU_RUN(test_the_range_test_has_one_copy);
	MU_RUN(test_the_wait_adds_up_steps_across_the_counters_reset);
	MU_RUN(test_no_reading_of_a_drift_fixed_period_ends_the_wait_early);
	return mu_report();
}
