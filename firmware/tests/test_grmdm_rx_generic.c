/*
 * The mode choice in rx_generic() (firmware/bluetooth_rxtx/bluetooth_rxtx.c).
 *
 * MODE_RX_GENERIC has two receives: the packet-mode one, which reads framed
 * packets out of the radio's FIFO, and a raw symbol stream through
 * bt_stream_rx(). Which one runs follows GRMDM's packet-mode bit, left in the
 * register by whatever set the radio up.
 *
 * That was written `if (cc2400_get(GRMDM) && 0x0400)`, a truth test rather
 * than a bit test: it asked whether GRMDM read non-zero at all, so any
 * configured radio took the packet-mode path and the symbol stream was dead
 * code. grmdm_packet_mode() in firmware/common/cc2400.h is the test it was
 * meant to be, and runs here with no radio.
 *
 * rx_generic() itself is hardware code that cannot run here, so the second
 * half scans the source for the shape, the way test_mode_dispatch.c and
 * test_frend.c do: the choice goes through the helper, and no truth test
 * against a bit mask is left in the file.
 */
#include <stdio.h>
#include <string.h>

#include "cc2400.h"
#include "minunit.h"
#include "source_scan.h"

/* The values the firmware writes to GRMDM, from the cc2400_set(GRMDM, ...)
 * calls in firmware/bluetooth_rxtx: the comment beside each says whether it
 * is packet mode, and bit 10 agrees. */
#define GRMDM_UNBUFFERED_GFSK 0x0101  /* un-buffered mode, GFSK */
#define GRMDM_BUFFERED_PACKET 0x0ce1  /* buffered, packet mode */
#define GRMDM_TX_GENERIC      0x0f61  /* buffered, packet mode */
#define GRMDM_RANGETEST       0x0df1  /* the datasheet default value */
/* Buffering (bit 11) and packet mode (bit 10) are separate: the eGo mode is
 * un-buffered and in packet mode. */
#define GRMDM_EGO_2FSK        0x04c0

MU_TEST(test_the_packet_mode_bit_is_bit_ten)
{
	mu_assert_int_eq(1 << 10, GRMDM_PACKET_MODE);
}

MU_TEST(test_packet_mode_follows_the_bit_and_not_the_register)
{
	/* The bit set, on its own and alongside the rest of the register. */
	mu_assert_int_eq(1, grmdm_packet_mode(GRMDM_PACKET_MODE));
	mu_assert_int_eq(1, grmdm_packet_mode(GRMDM_BUFFERED_PACKET));
	mu_assert_int_eq(1, grmdm_packet_mode(GRMDM_TX_GENERIC));
	mu_assert_int_eq(1, grmdm_packet_mode(GRMDM_RANGETEST));
	mu_assert_int_eq(1, grmdm_packet_mode(GRMDM_EGO_2FSK));
	mu_assert_int_eq(1, grmdm_packet_mode(0xffff));

	/* The bit clear. This is the value the truth test got wrong: a
	 * configured radio that is not in packet mode reads non-zero, so the
	 * symbol stream below the test never ran. */
	mu_assert_int_eq(0, grmdm_packet_mode(GRMDM_UNBUFFERED_GFSK));
	mu_assert_int_eq(0, grmdm_packet_mode(0xfbff));

	/* A radio nothing has set up reads zero, which is not packet mode
	 * either: the truth test agreed here and nowhere else. */
	mu_assert_int_eq(0, grmdm_packet_mode(0));
}

/* ---- the shape of rx_generic() ----------------------------------------- */

static const char *path = FW_RXTX_DIR "/bluetooth_rxtx.c";

struct scan {
	int opened;
	int rx_generic;        /* where rx_generic() is defined */
	int helper_call;       /* grmdm_packet_mode() inside it */
	int truth_test;        /* a `&&` against a hex mask, anywhere in the file */
	int truth_test_line;
};

/* Does this line test something with `&&` against a hexadecimal literal?
 * That is the bug: `x && 0x0400` compiles, warns about nothing useful, and
 * means "x is non-zero". */
static int tests_a_mask_with_and_and(const char *line)
{
	const char *p;

	for (p = strstr(line, "&&"); p != NULL; p = strstr(p + 2, "&&")) {
		const char *q = p + 2;

		while (*q == ' ' || *q == '\t' || *q == '(')
			q++;
		if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X'))
			return 1;
	}
	return 0;
}

static void scan_source(struct scan *s)
{
	char line[512];
	char name[64];
	int lineno = 0, in_block = 0, in_rx_generic = 0;
	FILE *f = fopen(path, "r");

	memset(s, 0, sizeof(*s));
	if (f == NULL)
		return;
	s->opened = 1;

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);

		if (tests_a_mask_with_and_and(line)) {
			s->truth_test++;
			if (s->truth_test_line == 0)
				s->truth_test_line = lineno;
		}

		if (in_rx_generic) {
			if (contains_identifier(line, "grmdm_packet_mode") &&
			    s->helper_call == 0)
				s->helper_call = lineno;
			if (line[0] == '}')
				in_rx_generic = 0;
			continue;
		}
		/* A definition starts in column 1 and is not a statement. */
		if (line[0] != '\0' && line[0] != '#' && line[0] != '}' &&
		    strchr(line, ';') == NULL &&
		    call_name(line, name, sizeof(name)) &&
		    strcmp(name, "rx_generic") == 0) {
			in_rx_generic = 1;
			s->rx_generic = lineno;
		}
	}
	fclose(f);
}

static struct scan s;

MU_TEST(test_rx_generic_chooses_through_the_bit_test)
{
	char msg[512];

	scan_source(&s);
	snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
	mu_assert(s.opened, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find rx_generic()'s definition, so the "
	         "check below would pass without reading anything", path);
	mu_assert(s.rx_generic > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rx_generic() does not choose its receive with "
	         "grmdm_packet_mode(); a test of its own risks being the truth "
	         "test again", path, s.rx_generic);
	mu_assert(s.helper_call > 0, msg);
}

MU_TEST(test_no_mask_is_tested_with_a_logical_and)
{
	char msg[512];

	/* A scan that read nothing would pass this by finding no `&&` at all. */
	snprintf(msg, sizeof(msg), "%s was not scanned", path);
	mu_assert(s.opened && s.rx_generic > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: `&&` is applied to a hexadecimal mask (%d place(s) in "
	         "the file): that is a truth test, not a bit test -- use `&`",
	         path, s.truth_test_line, s.truth_test);
	mu_assert(s.truth_test == 0, msg);
}

int main(void)
{
	printf("GRMDM generic receive tests\n");
	MU_RUN(test_the_packet_mode_bit_is_bit_ten);
	MU_RUN(test_packet_mode_follows_the_bit_and_not_the_register);
	MU_RUN(test_rx_generic_chooses_through_the_bit_test);
	MU_RUN(test_no_mask_is_tested_with_a_logical_and);
	return mu_report();
}
