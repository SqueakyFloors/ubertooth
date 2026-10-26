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
 *     RSSI readings, and neither saw one sync word. The write at boot does not
 *     survive: ubertooth_init() writes MANAND and then calls clock_start(),
 *     which resets the radio.
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
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

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
};

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

	if (strstr(line, "--") == NULL && strstr(line, "++") == NULL)
		return 0;
	if (contains_identifier(line, "cc2400_status") ||
	    contains_identifier(line, "cc2400_get"))
		return 0;
	return is_body_less_while(line, trimmed, sizeof(trimmed));
}

static int scan_source(const char *path, struct scan *s)
{
	char line[512];
	int lineno = 0, in_block = 0;
	int in_rangetest = 0, in_repeater = 0, in_turnaround = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);

		if (strstr(line, "repeater_turnaround(void)") != NULL) {
			s->turnaround_fn = lineno;
			in_turnaround = 1;
			in_rangetest = in_repeater = 0;
			continue;
		}
		if (strstr(line, "void cc2400_rangetest(") != NULL) {
			in_rangetest = 1;
			in_repeater = in_turnaround = 0;
			continue;
		}
		if (strstr(line, "void cc2400_repeater(") != NULL) {
			in_repeater = 1;
			in_rangetest = in_turnaround = 0;
			continue;
		}
		if (strstr(line, "void cc2400_txtest(") != NULL) {
			/* Not part of the range test pair: cc2400_txtest() sets the
			 * radio transmitting and returns, and it neither receives nor
			 * waits for a peer. */
			in_rangetest = in_repeater = in_turnaround = 0;
			continue;
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
	}

	fclose(f);
	return 1;
}

static struct scan s;
static const char *path = FW_RXTX_DIR "/cc2400_rangetest.c";

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

int main(void)
{
	printf("range test radio setup tests\n");

	if (!scan_source(path, &s)) {
		printf("cannot open %s to scan\n", path);
		return 1;
	}

	MU_RUN(test_both_modes_hold_the_gain_amplifier_out_of_reset);
	MU_RUN(test_the_repeater_sets_the_packet_format);
	MU_RUN(test_the_turnaround_delay_is_a_timed_wait);
	MU_RUN(test_the_result_is_cleared_before_the_test_can_return_early);
	return mu_report();
}
