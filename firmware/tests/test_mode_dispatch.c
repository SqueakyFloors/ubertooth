/*
 * Guard for the mode dispatch in firmware/bluetooth_rxtx/bluetooth_rxtx.c
 * main() (issue #191).
 *
 * The main loop only reacts when `requested_mode != mode`, and cc2400_idle()
 * -- which clears the LEDs and the Ubertooth One amplifier, and puts the
 * channel back to 2441 MHz and the squelch back to -120 -- runs only from the
 * MODE_IDLE case. So a case that starts a mode loop without leaving `mode`
 * equal to that mode never gets an idle reset when the mode stops: `-S` sets
 * requested_mode = MODE_IDLE, the loop returns, requested_mode == mode, and
 * nothing happens. MODE_LED_SPECAN, MODE_SPECAN, MODE_BT_PROMISC_LE and
 * MODE_BT_SLAVE_LE were all like that.
 *
 * The dispatch is hardware code that cannot be run here, so this test scans
 * the source, the way test_frend.c scans the transmit paths for hard-coded PA
 * levels. It reads main()'s switch and requires every `case MODE_...:` to
 * assign `mode` in the case body, unless it is one of the exceptions below,
 * which reach the same end by another route.
 */
#include <stdio.h>
#include <string.h>

#include "minunit.h"

#ifndef FW_RXTX_DIR
#define FW_RXTX_DIR ".."
#endif

/*
 * Cases that legitimately do not assign `mode` in the case body. Each one is
 * either not a mode loop at all or sets `mode` inside the function it calls;
 * the reason is the message printed if the scan ever has to explain itself.
 */
static const struct {
	const char *mode;
	const char *why;
} exceptions[] = {
	{ "MODE_RESET",      "reboots the board, so there is no mode to leave" },
	{ "MODE_IDLE",       "cc2400_idle() sets mode itself" },
	{ "MODE_TX_GENERIC", "tx_generic() sets mode itself and restores "
	                     "requested_mode to the previous mode, so the "
	                     "dispatch runs that mode's case next" },
};

static const char *exception_reason(const char *mode)
{
	size_t i;

	for (i = 0; i < sizeof(exceptions) / sizeof(exceptions[0]); i++)
		if (strcmp(exceptions[i].mode, mode) == 0)
			return exceptions[i].why;
	return NULL;
}

/*
 * Does the text assign to `mode` (not `requested_mode`, which ends in the same
 * four letters)? Looks for the needle with no identifier character in front.
 */
static int contains_assignment(const char *text, const char *needle)
{
	const char *p = text;

	while ((p = strstr(p, needle)) != NULL) {
		char prev = (p == text) ? ' ' : p[-1];
		if (prev == ' ' || prev == '\t' || prev == '\n')
			return 1;
		p += strlen(needle);
	}
	return 0;
}

MU_TEST(test_every_mode_case_sets_mode)
{
	const char *path = FW_RXTX_DIR "/bluetooth_rxtx.c";
	char line[512], msg[1024];
	char open_mode[64] = "";
	char body[4096] = "";
	int in_main = 0, cases = 0, lineno = 0, open_line = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL) {
		mu_assert(0, "cannot open the firmware source to scan");
		printf("      (path %s)\n", path);
		return;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		char *c;

		lineno++;
		if (!in_main) {
			if (strstr(line, "int main(") != NULL)
				in_main = 1;
			continue;
		}

		c = strstr(line, "case MODE_");
		if (c == NULL && strstr(line, "default:") == NULL) {
			/* still inside the case opened last, if any */
			if (open_mode[0] != '\0' &&
			    strlen(body) + strlen(line) + 1 < sizeof(body))
				strcat(body, line);
			continue;
		}

		/* A new case (or default:) closes the previous one. */
		if (open_mode[0] != '\0') {
			const char *why = exception_reason(open_mode);
			char needle[80];

			snprintf(needle, sizeof(needle), "mode = %s;", open_mode);
			if (why == NULL) {
				snprintf(msg, sizeof(msg),
				         "%s:%d case %s: runs a mode loop without "
				         "\"mode = %s;\", so stopping it skips the "
				         "cc2400_idle() reset (issue #191)",
				         path, open_line, open_mode, open_mode);
				mu_assert(contains_assignment(body, needle), msg);
			} else {
				snprintf(msg, sizeof(msg),
				         "%s:%d case %s now assigns mode; drop it from the "
				         "exception list (was: %s)",
				         path, open_line, open_mode, why);
				mu_assert(!contains_assignment(body, needle), msg);
			}
			cases++;
			open_mode[0] = '\0';
			body[0] = '\0';
		}

		if (c != NULL) {
			if (sscanf(c, "case %63[A-Z_0-9]:", open_mode) != 1)
				open_mode[0] = '\0';
			open_line = lineno;
		}
	}

	fclose(f);

	/*
	 * How many cases the dispatch has today, so a scan that silently matched
	 * nothing cannot pass. Update it when a mode is added or removed.
	 */
	mu_assert_int_eq(19, cases);
}

int main(void)
{
	printf("mode dispatch tests\n");
	MU_RUN(test_every_mode_case_sets_mode);
	return mu_report();
}
