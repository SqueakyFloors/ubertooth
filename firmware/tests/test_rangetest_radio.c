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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

#ifndef FW_DIR
#define FW_DIR ".."
#endif

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
	int fifo_wait_step;      /* the step it adds up, as written */
	char fifo_wait_text[256];
	int fifo_wait_length;    /* its length is RANGETEST_LEAD_100NS */
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

static int scan_source(const char *path, struct scan *s)
{
	char line[512];
	int lineno = 0, in_block = 0;
	int in_rangetest = 0, in_repeater = 0, in_turnaround = 0;
	int in_fifo_wait = 0;
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
			in_rangetest = in_repeater = in_turnaround = in_fifo_wait = 0;

		if (strstr(line, "repeater_turnaround(void)") != NULL) {
			s->turnaround_fn = lineno;
			in_turnaround = 1;
			continue;
		}
		if (strstr(line, "rangetest_wait_for_fifo(void)") != NULL) {
			s->fifo_wait_fn = lineno;
			in_fifo_wait = 1;
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
		/*
		 * The wait's length has to be a number of byte times, not a tick
		 * count of its own: a plain number here is how a wait for one byte
		 * silently becomes a wait for thirty.
		 */
		if (strstr(line, "define RANGETEST_LEAD_100NS") != NULL) {
			collapse_spaces(line, s->lead_text, sizeof(s->lead_text));
			if (contains_identifier(s->lead_text, "RANGETEST_LEAD_BYTES") &&
			    contains_identifier(s->lead_text, "RANGETEST_BYTE_100NS"))
				s->lead_from_byte = 1;
		}

		if (in_fifo_wait) {
			if (contains_identifier(line, "T0TC"))
				s->fifo_wait_timer = 1;
			if (contains_identifier(line, "RANGETEST_LEAD_100NS"))
				s->fifo_wait_length = 1;
			/*
			 * The one piece of arithmetic in the change, pinned as
			 * written: nothing here could run it, and its operands the
			 * other way round still compiles, still looks like a
			 * wrap-around step, and ends the wait early.
			 */
			if (strstr(line, "waited +=") != NULL) {
				collapse_spaces(line, s->fifo_wait_text,
				                sizeof(s->fifo_wait_text));
				if (strcmp(s->fifo_wait_text,
				           "waited += (now + CLK100NS_PER_CLKN - last) % "
				           "CLK100NS_PER_CLKN;") == 0)
					s->fifo_wait_step = 1;
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
			if (in_rangetest && s->rangetest_fifo_read == 0)
				s->rangetest_fifo_read = lineno;
			if (in_repeater && s->repeater_fifo_read == 0)
				s->repeater_fifo_read = lineno;
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
			if (in_rangetest && s->rangetest_wait == 0)
				s->rangetest_wait = lineno;
			if (in_repeater && s->repeater_wait == 0)
				s->repeater_wait = lineno;
		}
		if (calls_identifier(line, "clkn_start")) {
			if (in_rangetest && s->rangetest_clkn == 0)
				s->rangetest_clkn = lineno;
			if (in_repeater && s->repeater_clkn == 0)
				s->repeater_clkn = lineno;
		}

		/* The SRFOFF that ends the repeater's receive, which has to come
		 * before the delay rather than after it. */
		if (in_repeater && strstr(line, "SRFOFF") != NULL &&
		    s->rx_off_line == 0)
			s->rx_off_line = lineno;
	}

	fclose(f);
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

static struct scan s;
static struct spi_scan spi;
static const char *path = FW_RXTX_DIR "/cc2400_rangetest.c";
static const char *spi_path = FW_COMMON_DIR "/ubertooth.c";

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
 * written. That was `mismatch on byte 0` for about one range test in five, and
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
	         "%s:%d: rangetest_wait_for_fifo() adds up `%s`. It has to be "
	         "`waited += (now + CLK100NS_PER_CLKN - last) %% "
	         "CLK100NS_PER_CLKN;`, which is what makes a step across T0TC's "
	         "reset come out as the time it was: those operands the other way "
	         "round over-count the step and end the wait early, which is the "
	         "bug this wait is here to fix",
	         path, s.fifo_wait_fn, s.fifo_wait_text);
	mu_assert(s.fifo_wait_step, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rangetest_wait_for_fifo() does not wait "
	         "RANGETEST_LEAD_100NS", path, s.fifo_wait_fn);
	mu_assert(s.fifo_wait_length, msg);

	snprintf(msg, sizeof(msg),
	         "%s: cc2400_rangetest() notices a sync word at line %d and reads "
	         "the FIFO at line %d without waiting in between, so byte 0 of "
	         "the reply comes back wrong about one run in five",
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

	MU_RUN(test_both_modes_hold_the_gain_amplifier_out_of_reset);
	MU_RUN(test_the_repeater_sets_the_packet_format);
	MU_RUN(test_the_turnaround_delay_is_a_timed_wait);
	MU_RUN(test_the_repeater_stops_receiving_before_it_waits);
	MU_RUN(test_the_result_is_cleared_before_the_test_can_return_early);
	MU_RUN(test_the_fifo_is_reached_through_the_helpers);
	MU_RUN(test_the_read_waits_for_the_radio_to_have_the_packet);
	MU_RUN(test_the_wait_is_a_few_bytes_of_air_time);
	MU_RUN(test_the_shared_fifo_transfers_keep_their_shape);
	MU_RUN(test_the_range_test_has_one_copy);
	return mu_report();
}
