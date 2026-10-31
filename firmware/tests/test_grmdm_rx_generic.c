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
 * The rest scans the packet-mode receive, rx_generic_sync(). First the size of
 * the buffer it builds each record in: enqueue() takes no length and copies
 * DMA_SIZE bytes out of whatever it is handed, and that buffer was 36 bytes,
 * so every record carried 14 bytes read past the end of a stack buffer out to
 * the host. Then the wait that lets the radio write byte 0, and the clearing
 * of the radio's FIFO: a record holds one FIFO's worth, and bytes a longer
 * packet left behind came out in front of the next packet, which arrived
 * shifted by as many as were left.
 *
 * The last part is a model of the loop's bookkeeping that does run here: what
 * a record takes out of the FIFO, what the mark on a record cut short is read
 * from, and what is left for the next packet with the flush and without it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "cc2400.h"
#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"
/* clk100ns_step(), the firmware's own, for the runnable model at the end. */
#include "ubertooth_clock.h"

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

/*
 * rx_generic_wait_for_fifo(), every code line of it with the spacing
 * flattened and the comments left out. Each line of it can be left standing
 * while the wait is taken apart around it -- a `waited = 0;` below the
 * addition, a `break;` before the lead has passed -- so the whole function is
 * pinned as written and not only its pieces. The range test's copy of the
 * wait is pinned the same way in test_rangetest_radio.c.
 */
#define WAIT_BODY \
	"{ " \
	"u32 last = T0TC; " \
	"u32 waited = 0; " \
	"u32 polls = RX_GENERIC_LEAD_POLLS; " \
	"while (waited < RX_GENERIC_LEAD_100NS && polls-- > 0) { " \
	"u32 now = T0TC; " \
	"waited += clk100ns_step(last, now); " \
	"last = now; " \
	"} " \
	"}"

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

/*
 * One radio register a function touches: where it was saved, how many times it
 * was written, and whether its last write puts a saved value back rather than
 * setting the register up again.
 */
#define REG_MAX 32

struct reg_use {
	char name[32];
	int saved;             /* line of the cc2400_get() that saved it */
	char saved_in[64];     /* the variable that line saved it into */
	int writes;            /* how many cc2400_set() calls wrote it */
	int first_write;       /* the line of the first one */
	int last_write;        /* the line of the last one */
	int last_write_restores; /* that line names no new value */
	char last_write_text[SCAN_LINE_MAX];
};

struct scan {
	int opened;
	int rx_generic;        /* where rx_generic() is defined */
	int helper_call;       /* grmdm_packet_mode() inside it */
	int truth_test;        /* a `&&` against a hex mask, anywhere in the file */
	int truth_test_line;
	int rx_generic_sync;   /* where rx_generic_sync() is defined */
	int packet_read;       /* the loop's read of the record's payload */
	int record_buffer;     /* its record buffer's declaration */
	char record_buffer_decl[SCAN_LINE_MAX];
	long syncword_len;     /* RX_GENERIC_SYNCWORD_LEN, -1 when not found */
	long data_len;         /* RX_GENERIC_DATA_LEN, -1 when not found */
	long flags_len;        /* RX_GENERIC_FLAGS_LEN */
	long truncated;        /* RX_GENERIC_TRUNCATED */

	/* The flush of the radio's FIFO, and the mark on a record it cut. */
	int flush_def;         /* rx_generic_flush_fifo()'s definition */
	int flush_whole_fifo;  /* it reads CC2400_FIFO_SIZE bytes */
	int loop_start;        /* the capture loop's `while` */
	int flush_on_entry;    /* a flush call above that loop */
	int flush_after_read;  /* and one below the record's read */
	int mark_line;         /* the record's flags byte written */
	char mark_text[SCAN_LINE_MAX];
	int enqueue_line;      /* the record handed to enqueue() */
	char flags_off_text[SCAN_LINE_MAX];  /* RX_GENERIC_FLAGS_OFF as written */
	int flags_off_from_lengths;          /* it is the two lengths added */
	int iocfg_save;        /* IOCFG read into a variable */
	char iocfg_saved_in[64];
	int iocfg_set_empty;   /* FIFO_EMPTY put on the pin */
	int iocfg_restores;    /* how many writes put the saved value back */
	int returns;           /* `return` statements in the function */

	/* The wait before the record's own FIFO read. */
	int wait_call;         /* its call inside the capture loop */
	int wait_def;          /* rx_generic_wait_for_fifo()'s definition */
	int wait_counter;      /* it reads the 100 ns counter */
	int wait_helper;       /* and measures each step with clk100ns_step() */
	int wait_modulo;       /* or modulo a fixed period, which it must not */
	int wait_length;       /* its length is RX_GENERIC_LEAD_100NS */
	int wait_cap;          /* and it gives up after RX_GENERIC_LEAD_POLLS */
	char wait_body[1024];  /* every code line of it, flattened */
	long lead_bytes;       /* RX_GENERIC_LEAD_BYTES */
	long byte_100ns;       /* RX_GENERIC_BYTE_100NS */
	char lead_text[SCAN_LINE_MAX];  /* RX_GENERIC_LEAD_100NS as written */
	int lead_from_byte;    /* the lead is that many byte times */

	/* The radio registers tx_generic() touches. */
	int tx_generic;
	struct reg_use regs[REG_MAX];
	int reg_count;
};

/* The value of a #define in the scanned file that names a plain number,
 * decimal or hexadecimal. -1 when the file has no such line. */
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
		/* Base 0, so a flag written 0x01 reads as 1 rather than 0. */
		value = strtol(p + strlen(name), NULL, 0);
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

/*
 * The register named as the first argument of cc2400_get(...) or
 * cc2400_set(...) on this line, or NULL when the line has neither. `rest` is
 * left pointing at the value argument of a set, which is what says whether the
 * write puts a saved value back or sets the register up again.
 */
static const char *register_touched(const char *line, const char *call,
                                    char *name, size_t len,
                                    const char **rest)
{
	const char *p = strstr(line, call);
	const char *q;
	size_t n = 0;

	if (p == NULL)
		return NULL;
	p += strlen(call);
	while (*p == ' ' || *p == '\t')
		p++;
	for (q = p; is_ident_char(*q); q++)
		;
	n = (size_t)(q - p);
	if (n == 0 || n >= len)
		return NULL;
	memcpy(name, p, n);
	name[n] = '\0';
	if (rest != NULL)
		*rest = q;
	return name;
}

/*
 * The variable a line assigns to: `grmdm = cc2400_get(GRMDM);` saves GRMDM
 * into `grmdm`, and the restore has to name that variable and not another
 * register's. Empty when the line assigns to nothing.
 */
static void assignment_target(const char *line, char *out, size_t size)
{
	const char *eq = strchr(line, '=');
	const char *end, *start;
	size_t n;

	out[0] = '\0';
	if (eq == NULL || eq[1] == '=' || (eq != line && eq[-1] == '='))
		return;
	for (end = eq; end > line && !is_ident_char(end[-1]); end--)
		;
	for (start = end; start > line && is_ident_char(start[-1]); start--)
		;
	n = (size_t)(end - start);
	if (n == 0 || n >= size)
		return;
	memcpy(out, start, n);
	out[n] = '\0';
}

/* The register's entry in the scan, created on first sight. */
static struct reg_use *reg_entry(struct scan *s, const char *name)
{
	int i;

	for (i = 0; i < s->reg_count; i++)
		if (strcmp(s->regs[i].name, name) == 0)
			return &s->regs[i];
	if (s->reg_count >= REG_MAX)
		return NULL;
	snprintf(s->regs[s->reg_count].name,
	         sizeof(s->regs[s->reg_count].name), "%s", name);
	return &s->regs[s->reg_count++];
}

/*
 * Does this cc2400_set() put a saved value back, rather than set the register
 * up again?
 *
 * A restore names a local variable, which in this file is lower case, possibly
 * through a helper: `grmdm`, `gio_save`, `frend_keep_pa_level(frend)`. A setup
 * names something the function chose: a number (`0x0f61`), a field of the
 * transmit request (`tx_pkt.channel`), or a named bit from cc2400.h
 * (`GIO_FIFO_EMPTY`, `FREND_RESERVED_ONE`), all of which are upper case or a
 * hex literal. IOCFG's setup writes a named bit and a saved value together,
 * which is why any of the three in the value makes it a setup.
 */
static int value_is_a_restore(const char *value)
{
	const char *p;

	if (strstr(value, "0x") != NULL || strstr(value, "0X") != NULL)
		return 0;
	for (p = value; *p != '\0'; p++)
		if (*p >= 'A' && *p <= 'Z')
			return 0;
	return strstr(value, "tx_pkt") == NULL;
}

static void scan_source(struct scan *s)
{
	char line[512];
	char name[64];
	int lineno = 0, in_block = 0, in_rx_generic = 0, in_rx_generic_sync = 0;
	int in_wait = 0, in_tx_generic = 0, in_flush = 0;
	const char *set_value;
	FILE *f = fopen(path, "r");

	memset(s, 0, sizeof(*s));
	s->syncword_len = read_define("RX_GENERIC_SYNCWORD_LEN");
	s->data_len = read_define("RX_GENERIC_DATA_LEN");
	s->flags_len = read_define("RX_GENERIC_FLAGS_LEN");
	s->truncated = read_define("RX_GENERIC_TRUNCATED");
	s->lead_bytes = read_define("RX_GENERIC_LEAD_BYTES");
	s->byte_100ns = read_define("RX_GENERIC_BYTE_100NS");
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

		if (strstr(line, "define RX_GENERIC_FLAGS_OFF") != NULL) {
			collapse_spaces(line, s->flags_off_text,
			                sizeof(s->flags_off_text));
			if (contains_identifier(s->flags_off_text,
			                        "RX_GENERIC_SYNCWORD_LEN") &&
			    contains_identifier(s->flags_off_text,
			                        "RX_GENERIC_DATA_LEN"))
				s->flags_off_from_lengths = 1;
		}

		if (strstr(line, "define RX_GENERIC_LEAD_100NS") != NULL) {
			collapse_spaces(line, s->lead_text, sizeof(s->lead_text));
			if (contains_identifier(s->lead_text, "RX_GENERIC_LEAD_BYTES") &&
			    contains_identifier(s->lead_text, "RX_GENERIC_BYTE_100NS"))
				s->lead_from_byte = 1;
		}

		if (in_tx_generic) {
			const char *value;
			struct reg_use *r;

			if (register_touched(line, "cc2400_get(", name,
			                     sizeof(name), NULL) != NULL) {
				r = reg_entry(s, name);
				if (r != NULL && r->saved == 0) {
					r->saved = lineno;
					assignment_target(line, r->saved_in,
					                  sizeof(r->saved_in));
				}
			}
			if (register_touched(line, "cc2400_set(", name,
			                     sizeof(name), &value) != NULL) {
				r = reg_entry(s, name);
				if (r != NULL) {
					r->writes++;
					if (r->first_write == 0)
						r->first_write = lineno;
					r->last_write = lineno;
					r->last_write_restores =
						value_is_a_restore(value);
					collapse_spaces(line, r->last_write_text,
					                sizeof(r->last_write_text));
				}
			}
			if (line[0] == '}')
				in_tx_generic = 0;
			continue;
		}

		if (in_wait) {
			char flat[SCAN_LINE_MAX];

			/* strip_comments() has already run, so what is left of
			 * a comment is an empty line: comments are out of the
			 * body below, which leaves them free to be rewritten. */
			collapse_spaces(line, flat, sizeof(flat));
			if (flat[0] != '\0') {
				if (s->wait_body[0] != '\0')
					strncat(s->wait_body, " ",
					        sizeof(s->wait_body) -
					        strlen(s->wait_body) - 1);
				strncat(s->wait_body, flat,
				        sizeof(s->wait_body) -
				        strlen(s->wait_body) - 1);
			}
			if (contains_identifier(line, "T0TC"))
				s->wait_counter = lineno;
			if (contains_identifier(line, "clk100ns_step"))
				s->wait_helper = lineno;
			if (strchr(line, '%') != NULL)
				s->wait_modulo = lineno;
			if (contains_identifier(line, "RX_GENERIC_LEAD_100NS"))
				s->wait_length = lineno;
			if (contains_identifier(line, "RX_GENERIC_LEAD_POLLS"))
				s->wait_cap = lineno;
			if (line[0] == '}')
				in_wait = 0;
			continue;
		}

		if (in_flush) {
			if (strstr(line, "cc2400_fifo_read(CC2400_FIFO_SIZE") != NULL)
				s->flush_whole_fifo = lineno;
			if (line[0] == '}')
				in_flush = 0;
			continue;
		}

		if (in_rx_generic_sync) {
			if (s->wait_call == 0 &&
			    contains_identifier(line, "rx_generic_wait_for_fifo"))
				s->wait_call = lineno;
			if (s->packet_read == 0 &&
			    strstr(line, "cc2400_fifo_read(RX_GENERIC_DATA_LEN") != NULL)
				s->packet_read = lineno;
			if (s->loop_start == 0 &&
			    strstr(line, "while") != NULL &&
			    contains_identifier(line, "requested_mode") &&
			    contains_identifier(line, "MODE_RX_GENERIC"))
				s->loop_start = lineno;
			if (calls_identifier(line, "rx_generic_flush_fifo")) {
				if (s->loop_start == 0)
					s->flush_on_entry = lineno;
				else if (s->packet_read != 0 &&
				         s->flush_after_read == 0)
					s->flush_after_read = lineno;
			}
			if (s->mark_line == 0 &&
			    contains_identifier(line, "RX_GENERIC_TRUNCATED")) {
				s->mark_line = lineno;
				collapse_spaces(line, s->mark_text,
				                sizeof(s->mark_text));
			}
			/* The call, not the _Static_assert above it that names
			 * enqueue() inside a string. */
			if (s->enqueue_line == 0 &&
			    strstr(line, "enqueue(BR_PACKET") != NULL)
				s->enqueue_line = lineno;
			if (s->iocfg_save == 0 &&
			    register_touched(line, "cc2400_get(", name,
			                     sizeof(name), NULL) != NULL &&
			    strcmp(name, "IOCFG") == 0) {
				s->iocfg_save = lineno;
				assignment_target(line, s->iocfg_saved_in,
				                  sizeof(s->iocfg_saved_in));
			}
			if (register_touched(line, "cc2400_set(", name,
			                     sizeof(name), &set_value) != NULL &&
			    strcmp(name, "IOCFG") == 0) {
				if (contains_identifier(set_value, "GIO_FIFO_EMPTY"))
					s->iocfg_set_empty = lineno;
				else if (s->iocfg_saved_in[0] != '\0' &&
				         contains_identifier(set_value,
				                             s->iocfg_saved_in))
					s->iocfg_restores++;
			}
			/* A `return` statement, not the word: a string or a
			 * variable name carrying it would throw the count that
			 * the restores are compared against off. */
			{
				char flat[SCAN_LINE_MAX];

				collapse_spaces(line, flat, sizeof(flat));
				if (strncmp(flat, "return", 6) == 0 &&
				    (flat[6] == ';' || flat[6] == ' '))
					s->returns++;
			}
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
			} else if (strcmp(name, "rx_generic_wait_for_fifo") == 0) {
				in_wait = 1;
				s->wait_def = lineno;
			} else if (strcmp(name, "rx_generic_flush_fifo") == 0) {
				in_flush = 1;
				s->flush_def = lineno;
			} else if (strcmp(name, "tx_generic") == 0) {
				in_tx_generic = 1;
				s->tx_generic = lineno;
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
	         "%s: RX_GENERIC_SYNCWORD_LEN, RX_GENERIC_DATA_LEN and "
	         "RX_GENERIC_FLAGS_LEN not found (read %ld, %ld and %ld)", path,
	         s.syncword_len, s.data_len, s.flags_len);
	mu_assert(s.syncword_len > 0 && s.data_len > 0 && s.flags_len > 0, msg);

	mu_assert_int_eq(4, (int)s.syncword_len);
	mu_assert_int_eq(32, (int)s.data_len);
	mu_assert_int_eq(1, (int)s.flags_len);
	mu_assert_int_eq(37,
	                 (int)(s.syncword_len + s.data_len + s.flags_len));

	snprintf(msg, sizeof(msg),
	         "the record is %ld bytes but enqueue() copies %d",
	         s.syncword_len + s.data_len + s.flags_len, DMA_SIZE);
	mu_assert(s.syncword_len + s.data_len + s.flags_len <= DMA_SIZE, msg);

	/* The payload is one FIFO's worth, which is what makes the flush after
	 * each record a flush: a shorter read would leave part of the FIFO. */
	mu_assert_int_eq(CC2400_FIFO_SIZE, (int)s.data_len);
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

/* ---- the wait before the record's FIFO read ---------------------------- */

/*
 * The capture starts reading the moment the radio reports SYNC_RECEIVED, which
 * the radio does when the sync word ends -- before the first payload byte
 * exists. A read that starts there asks the FIFO for a byte the radio has not
 * written and gets a wrong one back. Only byte 0: cc2400_fifo_read() is slower
 * than the air, about 14 us a byte against 8 us, so from the second byte on
 * the radio is further ahead with every byte read. The range test reads the
 * same radio the same way and showed it as a wrong byte 0 in about one run in
 * five.
 *
 * rx_generic_sync() is hardware code that cannot run here, so this is a scan,
 * as the checks above are: the wait exists, it is a wait on the 100 ns
 * counter with a bound, and it runs before the read rather than after it.
 */
MU_TEST(test_the_capture_waits_for_the_first_byte_before_it_reads)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find rx_generic_sync()'s definition, so "
	         "the checks below would pass without reading anything", path);
	mu_assert(s.opened && s.rx_generic_sync > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: rx_generic_wait_for_fifo() is not defined; the capture "
	         "has nothing to let the radio write byte 0 with", path);
	mu_assert(s.wait_def > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the capture loop never calls "
	         "rx_generic_wait_for_fifo(), so byte 0 of every record is read "
	         "before the radio has written it", path, s.rx_generic_sync);
	mu_assert(s.wait_call > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the loop's read of the record's payload was not found, so "
	         "the order below cannot be checked", path);
	mu_assert(s.packet_read > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the wait runs after the FIFO read at line %d, so it "
	         "does nothing for the byte it is there for", path, s.wait_call,
	         s.packet_read);
	mu_assert(s.wait_call < s.packet_read, msg);
}

/*
 * The wait itself. It has to read the 100 ns counter, measure each step with
 * the shared clk100ns_step() -- `now - last` across the counter's reset is a
 * huge unsigned number and would end the wait at once -- and give up after a
 * bound, so a counter that is not advancing cannot leave the board answering
 * no control request until the watchdog resets it.
 */
MU_TEST(test_the_wait_is_a_bounded_wait_on_the_counter)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: no rx_generic_wait_for_fifo() to wait in", path);
	mu_assert(s.wait_def > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rx_generic_wait_for_fifo() does not read T0TC, so it "
	         "is not waiting on the clock", path, s.wait_def);
	mu_assert(s.wait_counter > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rx_generic_wait_for_fifo() does not measure its steps "
	         "with clk100ns_step(), the shared helper in ubertooth_clock.h. "
	         "A step taken as `now - last` across the counter's reset is a "
	         "huge unsigned number, which ends the wait at once",
	         path, s.wait_def);
	mu_assert(s.wait_helper > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rx_generic_wait_for_fifo() takes a step modulo a fixed "
	         "period. The timer's period is whatever is in its "
	         "match register: a clock drift fix lengthens it, and the host "
	         "can set a length of its own through UBERTOOTH_TRIM_CLOCK or "
	         "UBERTOOTH_START_HOPPING. For some of those lengths a fixed "
	         "modulus returns a figure far larger than the step took, and "
	         "counting it ends the wait before the radio has written byte 0",
	         path, s.wait_modulo);
	mu_assert(s.wait_modulo == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rx_generic_wait_for_fifo() does not wait "
	         "RX_GENERIC_LEAD_100NS", path, s.wait_def);
	mu_assert(s.wait_length > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: rx_generic_wait_for_fifo() has no RX_GENERIC_LEAD_POLLS "
	         "cap, so a timer that is not running leaves it spinning and "
	         "the board answers nothing", path, s.wait_def);
	mu_assert(s.wait_cap > 0, msg);

	/*
	 * And the whole function, so that nothing can be added between the
	 * lines the checks above name.
	 */
	{
		/* Its own buffer: this message carries the function twice. */
		static char body_msg[4096];

		snprintf(body_msg, sizeof(body_msg),
		         "%s:%d: rx_generic_wait_for_fifo() reads\n  %s\nand it "
		         "has to read\n  %s\nComments are not compared, so only "
		         "the code has changed. Anything added between these "
		         "lines -- another write to `waited`, a guard that drops "
		         "a step, an exit before the lead has passed -- waits "
		         "for a length no test here knows, and the wait exists "
		         "to be exactly this long",
		         path, s.wait_def, s.wait_body, WAIT_BODY);
		mu_assert(strcmp(s.wait_body, WAIT_BODY) == 0, body_msg);
	}
}

/*
 * How long the wait is. One byte time is the least that can work, since the
 * wait starts at or after the end of the sync word; much more than a handful
 * costs packets, because the radio goes on writing into the same 32-byte FIFO
 * while the record is read out of it. And the length has to be written as byte
 * times, not as a bare tick count: a modulation change would then move it
 * without anyone noticing.
 */
MU_TEST(test_the_lead_is_a_few_byte_times)
{
	char msg[768];

	snprintf(msg, sizeof(msg),
	         "%s: RX_GENERIC_BYTE_100NS is %ld. A byte at 1 Mbit/s is 8 us "
	         "on the air, which is 80 of timer 0's 100 ns ticks -- the "
	         "shortest byte time the radio has, so the lead is at least one "
	         "byte at any rate the host configures", path, s.byte_100ns);
	mu_assert(s.byte_100ns == 80, msg);

	snprintf(msg, sizeof(msg),
	         "%s: RX_GENERIC_LEAD_BYTES is %ld. It needs at least the one "
	         "byte the radio has not written yet, and a long wait fills the "
	         "FIFO the record is being read out of", path, s.lead_bytes);
	mu_assert(s.lead_bytes >= 1 && s.lead_bytes <= 8, msg);

	snprintf(msg, sizeof(msg),
	         "%s: RX_GENERIC_LEAD_100NS is `%s`, which is not "
	         "RX_GENERIC_LEAD_BYTES byte times: a wait written as a bare "
	         "tick count stops tracking the air rate", path, s.lead_text);
	mu_assert(s.lead_from_byte, msg);
}

/*
 * The arithmetic the wait is built on, run here because the firmware's own
 * copy of it reads a hardware register this cannot.
 *
 * Timer 0 resets at the end of each period, so the wait adds up the steps it
 * sees instead of comparing against a deadline the reset would carry it past,
 * and clk100ns_step() -- the firmware's own, from ubertooth_clock.h -- turns
 * each pair of readings into a time. The scan above pins that the firmware
 * uses that helper, and this says what it has to come to. The range test has
 * the same accumulation in cc2400_rangetest.c.
 */
#define CLKN_PERIOD ((uint32_t)CLK100NS_PER_CLKN)  /* 312.5 us in 100 ns ticks */

/*
 * The firmware's loop, with the counter supplied instead of read. uint32_t
 * throughout, as T0TC and the firmware's own variables are: the wraparound of
 * that width is what the step helper has to turn back into a time, and a
 * 64-bit model of it would give different numbers.
 */
static uint32_t accumulate(const uint32_t *counter, unsigned n,
                           uint32_t target, uint32_t cap)
{
	uint32_t last = counter[0], waited = 0, polls = cap;
	unsigned i = 1;

	while (waited < target && polls-- > 0) {
		uint32_t now = counter[i < n ? i : n - 1];

		waited += clk100ns_step(last, now);
		i++;
		last = now;
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

	/* The subtraction the helper replaces: `now - last` across the reset is
	 * a huge unsigned number, which is the bug it is there for. */
	mu_assert((uint32_t)(25u - 3100u) > 320u,
	          "a bare subtraction across the reset must be the large number "
	          "this guards against");

	/*
	 * The period is not always CLK100NS_PER_CLKN. A step across the reset
	 * of a longer one cannot be known in full -- nothing in the wait knows
	 * how long that period was -- so the helper counts only how far into
	 * the new period the second reading is. For the pair below that is 10
	 * ticks where 384 really passed. Undercounting is the safe direction:
	 * over-counting is what ends the wait before the radio has written
	 * byte 0, which is what the wait is for. The sweep of every reading of
	 * every period length the timer can hold is in test_rangetest_radio.c,
	 * whose wait shares this helper and this lead.
	 */
	{
		static const uint32_t extended[] = {
			9000, 10, 110, 210, 310, 410, 510
		};

		mu_assert_int_eq(10, (int)clk100ns_step(9000, 10));
		got = accumulate(extended, 7, 320, 1000);
		mu_assert(got >= 320 && got < 420,
		          "a step out of a period that was not the usual length "
		          "must be undercounted, never over-counted");
	}

	/* A counter that is not advancing is ended by the cap, not by the
	 * target. */
	{
		static const uint32_t stuck[] = { 7, 7 };

		got = accumulate(stuck, 2, 320, 50);
		mu_assert(got < 320, "a stopped counter must give up short");
	}
}

/* ---- the flush of the radio's FIFO ------------------------------------- */

/* Every check below reads the scan, so a scan that found nothing would pass
 * them all without reading the firmware. */
static void scan_found_the_capture(void)
{
	char msg[256];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find rx_generic_sync()'s capture loop",
	         path);
	mu_assert(s.opened && s.rx_generic_sync > 0 && s.loop_start > 0 &&
	          s.packet_read > 0, msg);
}

/*
 * A record holds one FIFO's worth, and the radio goes on writing into the same
 * FIFO while that read runs. Bytes left behind came out in front of the next
 * packet, which arrived shifted by as many as were left.
 *
 * So the capture clears the FIFO on entry to the mode and again after each
 * record, with a read of a whole FIFO's worth discarded: the radio has no
 * flush command, and its FIFO-empty signal cannot tell one byte left from
 * none, so a drain guided by that signal would leave one behind.
 */
MU_TEST(test_the_capture_clears_the_fifo_on_entry_and_after_each_record)
{
	char msg[512];

	scan_found_the_capture();

	snprintf(msg, sizeof(msg),
	         "%s: rx_generic_flush_fifo() is not defined, so nothing clears "
	         "what the radio still holds", path);
	mu_assert(s.flush_def > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the flush does not read CC2400_FIFO_SIZE bytes, so it "
	         "leaves part of the FIFO behind", path, s.flush_def);
	mu_assert(s.flush_whole_fifo > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: nothing flushes before the capture loop, so what an "
	         "earlier mode left in the FIFO comes out in front of the first "
	         "packet", path, s.loop_start);
	mu_assert(s.flush_on_entry > 0 && s.flush_on_entry < s.loop_start, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: nothing flushes after the record is read, so a packet "
	         "longer than the record shifts the next one", path,
	         s.packet_read);
	mu_assert(s.flush_after_read > s.packet_read, msg);
}

/*
 * The mark on a record the packet did not fit in, and the register it is read
 * through.
 *
 * The capture asks the radio's FIFO_EMPTY signal whether anything was left and
 * writes the answer into the record's flags byte. It has to ask after the
 * record's own read, which is what leaves a tail behind; before the flush,
 * which empties the FIFO whatever was in it; and before enqueue(), which
 * copies the buffer.
 *
 * The signal reaches the firmware on GIO6, so the mode writes IOCFG -- the one
 * radio register it writes -- and has to put it back, both at the end of the
 * loop and on the early return that leaves when the host stops the mode.
 */
MU_TEST(test_a_record_the_packet_did_not_fit_is_marked)
{
	char msg[512];

	scan_found_the_capture();

	snprintf(msg, sizeof(msg),
	         "%s: RX_GENERIC_FLAGS_LEN and RX_GENERIC_TRUNCATED not found "
	         "(read %ld and %ld)", path, s.flags_len, s.truncated);
	mu_assert(s.flags_len > 0 && s.truncated > 0, msg);
	mu_assert_int_eq(1, (int)s.flags_len);

	/* Where the byte sits, which is what the model below reads. */
	snprintf(msg, sizeof(msg),
	         "%s: RX_GENERIC_FLAGS_OFF is `%s`; it must be the sync word's "
	         "length plus the payload's", path, s.flags_off_text);
	mu_assert(s.flags_off_from_lengths, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the capture loop never writes RX_GENERIC_TRUNCATED, so "
	         "a record cut short looks like a whole packet", path,
	         s.rx_generic_sync);
	mu_assert(s.mark_line > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the mark is `%s`; only the FIFO-empty pin says whether "
	         "the radio still held bytes", path, s.mark_line, s.mark_text);
	mu_assert(contains_identifier(s.mark_text, "GIO6"), msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the mark must be taken after the record's read (line "
	         "%d), before the flush (line %d) that empties the FIFO, and "
	         "before enqueue() (line %d) copies the buffer", path,
	         s.mark_line, s.packet_read, s.flush_after_read, s.enqueue_line);
	mu_assert(s.mark_line > s.packet_read &&
	          s.mark_line < s.flush_after_read &&
	          s.mark_line < s.enqueue_line, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the mode does not put GIO_FIFO_EMPTY on the pin, so "
	         "the mark reads whatever signal the host left there", path,
	         s.rx_generic_sync);
	mu_assert(s.iocfg_set_empty > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: IOCFG is not saved with cc2400_get() before it is "
	         "written, so what the mode puts back is its own value", path,
	         s.iocfg_set_empty);
	mu_assert(s.iocfg_save > 0 && s.iocfg_saved_in[0] != '\0' &&
	          s.iocfg_save < s.iocfg_set_empty, msg);

	/* One restore for each `return`, and one for the end of the function.
	 * A path added later needs one too. */
	snprintf(msg, sizeof(msg),
	         "%s:%d: rx_generic_sync() has %d `return` statement(s) but "
	         "puts `%s` back %d time(s); every way out has to restore IOCFG",
	         path, s.rx_generic_sync, s.returns, s.iocfg_saved_in,
	         s.iocfg_restores);
	mu_assert(s.iocfg_restores == s.returns + 1, msg);
}

/* ---- a runnable model of the record, the mark and the flush ------------- */

/*
 * The capture loop is hardware code, so what runs here is a model of the
 * bookkeeping, which is all this change is: how much of the FIFO a record
 * takes, what the mark is read from, and what is left for the next packet.
 *
 * It is not a model of the radio and proves nothing about the air. How many
 * bytes the radio puts in the FIFO while a record is read out of it is given
 * to the model, not worked out by it; the FIFO holds CC2400_FIFO_SIZE bytes
 * here as on the board, so no case it runs is one the hardware could not be
 * in.
 */
static uint8_t model_fifo[CC2400_FIFO_SIZE];
static unsigned model_fifo_len;

/* The byte a read gets when it asks the FIFO for more than it holds. The
 * hardware returns whatever its data pointer was left on; the value does not
 * matter here, only that it is not payload. */
#define MODEL_STRAY 0xff

/* The radio writing `n` received bytes, counting up from `first` so a shift
 * shows in the value. It stops at the FIFO's size, as the radio does. */
static void model_fifo_write(unsigned first, unsigned n)
{
	unsigned i;

	for (i = 0; i < n && model_fifo_len < CC2400_FIFO_SIZE; i++)
		model_fifo[model_fifo_len++] = (uint8_t)(first + i);
}

/* cc2400_fifo_read(): the oldest bytes the radio holds, oldest first. */
static void model_fifo_read(unsigned n, uint8_t *out)
{
	unsigned have = n < model_fifo_len ? n : model_fifo_len;
	unsigned i;

	memcpy(out, model_fifo, have);
	for (i = have; i < n; i++)
		out[i] = MODEL_STRAY;
	memmove(model_fifo, model_fifo + have, model_fifo_len - have);
	model_fifo_len -= have;
}

/* GIO6 with the FIFO-empty signal on it. In receive the signal goes low only
 * above one byte, so one byte left reads as empty: the firmware says so too. */
static int model_fifo_empty_pin(void)
{
	return model_fifo_len <= 1;
}

/*
 * One turn of the capture loop: the record's read, then the bytes the radio
 * wrote while that read ran (`during` of them, counting up from `first`), then
 * the mark, then the flush. The flush is under a switch so the test can run
 * the same packets with it and without it.
 */
static void model_capture_one(uint8_t *record, unsigned during, unsigned first,
                              int flush)
{
	const unsigned data = (unsigned)s.data_len;
	const unsigned off = (unsigned)(s.syncword_len + s.data_len);
	uint8_t waste[CC2400_FIFO_SIZE];

	model_fifo_read(data, record + s.syncword_len);
	model_fifo_write(first, during);
	record[off] = model_fifo_empty_pin() ? 0 : (uint8_t)s.truncated;
	if (flush)
		model_fifo_read(CC2400_FIFO_SIZE, waste);
}

/* The record's lengths and the mark come from the firmware, so a model run
 * against a tree whose scan found neither would assert nothing. */
static void model_needs_the_firmware(void)
{
	mu_assert(s.syncword_len == 4 && s.data_len == 32 && s.truncated > 0,
	          "the model needs the record's lengths and the mark from the "
	          "firmware");
}

/* Nothing left in the FIFO when the record was taken: the record holds the
 * packet and says it is whole. */
MU_TEST(test_a_packet_that_fits_is_whole_and_unmarked)
{
	uint8_t record[DMA_SIZE] = { 0 };
	unsigned i;

	model_needs_the_firmware();

	model_fifo_len = 0;
	model_fifo_write(1, 32);
	model_capture_one(record, 0, 0, 1);

	for (i = 0; i < 32; i++)
		mu_assert_int_eq((int)(i + 1), record[4 + i]);
	mu_assert_int_eq(0, record[36]);
	mu_assert_int_eq(0, (int)model_fifo_len);
}

/*
 * A packet the record did not hold all of: the radio wrote eight more bytes
 * while the record was read out of the FIFO. The record says it is cut short,
 * and the next packet is still read from its own first byte. Without the flush
 * that next record starts eight bytes into the packet before it, which is the
 * shift.
 */
MU_TEST(test_a_packet_too_long_is_marked_and_does_not_shift_the_next)
{
	uint8_t first[DMA_SIZE] = { 0 };
	uint8_t second[DMA_SIZE] = { 0 };
	unsigned i;

	model_needs_the_firmware();

	model_fifo_len = 0;
	model_fifo_write(1, 32);
	model_capture_one(first, 8, 33, 1);

	for (i = 0; i < 32; i++)
		mu_assert_int_eq((int)(i + 1), first[4 + i]);
	mu_assert_int_eq((int)s.truncated, first[36]);
	mu_assert_int_eq(0, (int)model_fifo_len);

	/* The next packet, 101 upwards so a leftover shows in the value. */
	model_fifo_write(101, 32);
	model_capture_one(second, 0, 0, 1);
	mu_assert_int_eq(101, second[4]);
	mu_assert_int_eq(0, second[36]);

	/* The same two packets with no flush. */
	memset(second, 0, sizeof(second));
	model_fifo_len = 0;
	model_fifo_write(1, 32);
	model_capture_one(first, 8, 33, 0);
	mu_assert_int_eq(8, (int)model_fifo_len);
	model_fifo_write(101, 32);
	model_capture_one(second, 0, 0, 0);
	mu_assert_int_eq(33, second[4]);
	mu_assert(second[4] != 101,
	          "without the flush the next record must start late, which is "
	          "the shift the flush is for");
}

/*
 * One byte left behind is the case the radio cannot report: its FIFO-empty
 * signal does not distinguish one byte from none. The flush still clears the
 * byte, so only the mark is missing, not the fix.
 */
MU_TEST(test_one_byte_left_behind_reads_as_whole_and_is_still_cleared)
{
	uint8_t record[DMA_SIZE] = { 0 };
	uint8_t next[DMA_SIZE] = { 0 };

	model_needs_the_firmware();

	model_fifo_len = 0;
	model_fifo_write(1, 32);
	model_capture_one(record, 1, 33, 1);
	mu_assert_int_eq(0, record[36]);
	mu_assert_int_eq(0, (int)model_fifo_len);

	model_fifo_write(101, 32);
	model_capture_one(next, 0, 0, 1);
	mu_assert_int_eq(101, next[4]);
}

/* ---- the registers tx_generic() puts back ------------------------------ */

/*
 * A generic transmit sets the radio up for its own packet, and nothing sets
 * the radio up again afterwards: the generic modes take the radio as the host
 * left it, through UBERTOOTH_SET_* and UBERTOOTH_WRITE_REGISTERS, so whatever
 * the transmit leaves behind is what the next mode reads.
 *
 * GRMDM is the register that decides something. rx_generic() reads its
 * packet-mode bit to choose between the packet receive and the raw symbol
 * stream, and the value the transmit writes has that bit set, so a receive
 * entered after a transmit used to take the packet path whatever the host had
 * asked for. MDMCTRL, MDMTST0 and FSDIV are the modem settings and the
 * channel, which the same receive would otherwise inherit from the packet.
 *
 * Rather than name the registers, this asks of every one the function writes:
 * was it saved first, and does its last write put a saved value back? That
 * holds for a register added later too.
 */
MU_TEST(test_tx_generic_puts_every_register_it_touches_back)
{
	char msg[1024];
	int i, configured = 0;

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find tx_generic()'s definition, so the "
	         "checks below would pass without reading anything", path);
	mu_assert(s.opened && s.tx_generic > 0, msg);

	for (i = 0; i < s.reg_count; i++) {
		struct reg_use *r = &s.regs[i];

		if (r->writes == 0)
			continue;
		configured++;

		snprintf(msg, sizeof(msg),
		         "%s:%d: tx_generic() writes %s without saving it with "
		         "cc2400_get() first, so it cannot put it back", path,
		         r->first_write, r->name);
		mu_assert(r->saved > 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s:%d: tx_generic() saves %s at line %d, after it has "
		         "already written it: what it puts back is then the "
		         "value the transmit itself chose", path,
		         r->first_write, r->name, r->saved);
		mu_assert(r->saved < r->first_write, msg);

		snprintf(msg, sizeof(msg),
		         "%s:%d: tx_generic()'s last write of %s is `%s`, which "
		         "sets the register up rather than putting the saved "
		         "value back. The mode the board returns to reads what "
		         "this leaves behind", path, r->last_write, r->name,
		         r->last_write_text);
		mu_assert(r->last_write_restores, msg);

		snprintf(msg, sizeof(msg),
		         "%s:%d: tx_generic()'s last write of %s is `%s`, which "
		         "does not name `%s`, the variable line %d saved %s "
		         "into. A register put back from another one's saved "
		         "value is not put back", path, r->last_write, r->name,
		         r->last_write_text, r->saved_in, r->saved, r->name);
		mu_assert(r->saved_in[0] != '\0' &&
		          contains_identifier(r->last_write_text, r->saved_in),
		          msg);

		snprintf(msg, sizeof(msg),
		         "%s:%d: tx_generic() writes %s once only, so it is "
		         "either set up and not restored or restored and not "
		         "set up", path, r->last_write, r->name);
		mu_assert(r->writes >= 2, msg);
	}

	/* A scan that found no registers would pass the loop above without
	 * reading anything. The function writes eight today. */
	snprintf(msg, sizeof(msg),
	         "%s:%d: the scan found %d registers written in tx_generic(); "
	         "it writes at least the syncword, the modem, the amplifier and "
	         "the channel", path, s.tx_generic, configured);
	mu_assert(configured >= 6, msg);

	/* And GRMDM by name, because it is the one whose value changes which
	 * receive the next mode runs. */
	for (i = 0; i < s.reg_count; i++)
		if (strcmp(s.regs[i].name, "GRMDM") == 0)
			break;
	snprintf(msg, sizeof(msg),
	         "%s:%d: tx_generic() does not touch GRMDM at all, so this scan "
	         "is reading the wrong function", path, s.tx_generic);
	mu_assert(i < s.reg_count, msg);
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
	MU_RUN(test_the_capture_waits_for_the_first_byte_before_it_reads);
	MU_RUN(test_the_wait_is_a_bounded_wait_on_the_counter);
	MU_RUN(test_the_lead_is_a_few_byte_times);
	MU_RUN(test_the_wait_adds_up_steps_across_the_counters_reset);
	MU_RUN(test_the_capture_clears_the_fifo_on_entry_and_after_each_record);
	MU_RUN(test_a_record_the_packet_did_not_fit_is_marked);
	MU_RUN(test_a_packet_that_fits_is_whole_and_unmarked);
	MU_RUN(test_a_packet_too_long_is_marked_and_does_not_shift_the_next);
	MU_RUN(test_one_byte_left_behind_reads_as_whole_and_is_still_cleared);
	MU_RUN(test_tx_generic_puts_every_register_it_touches_back);
	return mu_report();
}
