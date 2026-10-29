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
 *
 * The last part scans the packet-mode receive, rx_generic_sync(), for the size
 * of the buffer it builds each record in. enqueue() takes no length and copies
 * DMA_SIZE bytes out of whatever it is handed, and that buffer was 36 bytes:
 * every record carried 14 bytes read past the end of a stack buffer out to the
 * host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cc2400.h"
#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"

/* The values the firmware writes to GRMDM, from the cc2400_set(GRMDM, ...)
 * calls in firmware/bluetooth_rxtx. Whether each is packet mode is read from
 * bit 10 and the bit maps in cc2400_rx() and le_transmit(), not from the
 * one-line comment next to each call: those name the buffering and the
 * modulation and mostly say nothing about packet mode. */
#define GRMDM_UNBUFFERED_GFSK 0x0101  /* un-buffered, GFSK, not packet mode */
#define GRMDM_BUFFERED_PACKET 0x0ce1  /* buffered, packet mode */
#define GRMDM_TX_GENERIC      0x0f61  /* buffered, packet mode */
#define GRMDM_RANGETEST       0x0df1  /* the datasheet default value */
/* Buffering (bit 11) and packet mode (bit 10) are separate choices: the eGo
 * mode is un-buffered and in packet mode. */
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
	int rx_generic_sync;   /* where rx_generic_sync() is defined */
	int record_buffer;     /* its record buffer's declaration */
	char record_buffer_decl[SCAN_LINE_MAX];
	long syncword_len;     /* RX_GENERIC_SYNCWORD_LEN, -1 when not found */
	long data_len;         /* RX_GENERIC_DATA_LEN, -1 when not found */
};

/* The value of a plain decimal #define in the scanned file. -1 when the file
 * has no such line. */
static long read_define(const char *name)
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

/* Does this line test something with `&&` against a hexadecimal literal?
 * That is the bug: `x && 0x0400` compiles, warns about nothing useful, and
 * means "x is non-zero".
 *
 * Only a literal to the right of the operator counts. A hexadecimal number
 * in front of `&&` is nearly always the right-hand side of a comparison
 * (`llid == 0x03 &&`), and this file has two of those, so matching that side
 * as well would fail on correct code. The same goes for the forms this cannot
 * see at all: `0x0400 && x` with the mask written first, or a named mask
 * (`&& GRMDM_PACKET_MODE`). And it can be wrong the other way: `x && 0xNN ==
 * y` is a comparison and would be flagged. This is a guard against the shape
 * that was here, not a proof that no truth test can come back, and it covers
 * the whole file rather than rx_generic() alone -- a legitimate `&& 0x...`
 * added anywhere in it would have to be written another way or exempted
 * here. */
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
	int lineno = 0, in_block = 0, in_rx_generic = 0, in_rx_generic_sync = 0;
	FILE *f = fopen(path, "r");

	memset(s, 0, sizeof(*s));
	s->syncword_len = read_define("RX_GENERIC_SYNCWORD_LEN");
	s->data_len = read_define("RX_GENERIC_DATA_LEN");
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

		if (in_rx_generic_sync) {
			/* The buffer the record is built in: the first local
			 * array declared in the function. */
			if (s->record_buffer == 0 &&
			    contains_identifier(line, "u8") &&
			    strchr(line, '[') != NULL) {
				s->record_buffer = lineno;
				collapse_spaces(line, s->record_buffer_decl,
				                sizeof(s->record_buffer_decl));
			}
			if (line[0] == '}')
				in_rx_generic_sync = 0;
			continue;
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
		    call_name(line, name, sizeof(name))) {
			if (strcmp(name, "rx_generic") == 0) {
				in_rx_generic = 1;
				s->rx_generic = lineno;
			} else if (strcmp(name, "rx_generic_sync") == 0) {
				in_rx_generic_sync = 1;
				s->rx_generic_sync = lineno;
			}
		}
	}
	fclose(f);
}

static struct scan s;

MU_TEST(test_rx_generic_chooses_through_the_bit_test)
{
	char msg[512];

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

/* ---- the record rx_generic_sync() sends --------------------------------- */

/* What the record holds has to fit in what enqueue() copies, or the copy reads
 * past the data the capture wrote. The two lengths are the firmware's own
 * #defines; the firmware asserts the same sum at compile time, and this says
 * what the numbers are so a change to either is deliberate. */
MU_TEST(test_the_generic_record_fits_what_enqueue_copies)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: RX_GENERIC_SYNCWORD_LEN and RX_GENERIC_DATA_LEN not "
	         "found (read %ld and %ld)", path, s.syncword_len, s.data_len);
	mu_assert(s.syncword_len > 0 && s.data_len > 0, msg);

	mu_assert_int_eq(4, (int)s.syncword_len);
	mu_assert_int_eq(32, (int)s.data_len);
	mu_assert_int_eq(36, (int)(s.syncword_len + s.data_len));

	snprintf(msg, sizeof(msg),
	         "the record is %ld bytes but enqueue() copies %d",
	         s.syncword_len + s.data_len, DMA_SIZE);
	mu_assert(s.syncword_len + s.data_len <= DMA_SIZE, msg);
}

/*
 * The buffer itself. enqueue() is handed a pointer and copies DMA_SIZE bytes,
 * so the buffer has to be that size however little of it the record fills.
 * This was `u8 buf[len+4]`, 36 bytes, and the 14 bytes past its end went to
 * the host in every record.
 *
 * The declaration is read as text, with the size named rather than spelled as
 * a number, because the point is that the buffer tracks what enqueue() copies.
 * It must also be initialised: the bytes the capture does not write are sent,
 * so an uninitialised tail is the same leak by another route.
 */
MU_TEST(test_the_record_buffer_is_the_size_enqueue_copies)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find rx_generic_sync()'s definition, so "
	         "the checks below would pass without reading anything", path);
	mu_assert(s.opened && s.rx_generic_sync > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rx_generic_sync() declares no buffer", path,
	         s.rx_generic_sync);
	mu_assert(s.record_buffer > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the record buffer is declared `%s`; it must be sized "
	         "DMA_SIZE, because enqueue() copies that many bytes out of it",
	         path, s.record_buffer, s.record_buffer_decl);
	mu_assert(strstr(s.record_buffer_decl, "[DMA_SIZE]") != NULL, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the record buffer is declared `%s`; it must be "
	         "initialised, because the bytes the capture does not write are "
	         "sent to the host too", path, s.record_buffer,
	         s.record_buffer_decl);
	mu_assert(strchr(s.record_buffer_decl, '=') != NULL, msg);
}

int main(void)
{
	printf("GRMDM generic receive tests\n");
	/* Scanned once, so the tests that read it can run in any order. */
	scan_source(&s);
	MU_RUN(test_the_packet_mode_bit_is_bit_ten);
	MU_RUN(test_packet_mode_follows_the_bit_and_not_the_register);
	MU_RUN(test_rx_generic_chooses_through_the_bit_test);
	MU_RUN(test_no_mask_is_tested_with_a_logical_and);
	MU_RUN(test_the_generic_record_fits_what_enqueue_copies);
	MU_RUN(test_the_record_buffer_is_the_size_enqueue_copies);
	return mu_report();
}
