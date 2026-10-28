/*
 * Unit tests for three offset and size calculations in the BR/EDR image:
 * the air buffer's byte offset in null_decode() and null_encode(), the input
 * bound in fec23() (all in firmware/btbr/src/codec/bitstream_proc.c), and the
 * syncword window in firmware/btbr/src/btphy/scan_task.c.
 *
 * bitstream_proc.c is pure logic, so the real one is linked here and the
 * codec's own chunk sizes (firmware/btbr/include/ubtbr/codec.h) say which
 * offsets a real packet reaches:
 *
 *  - the receive side decodes CODEC_RX_CHUNK_SIZE (10) bytes at a time from
 *    air bit 58 onwards, so chunk 25 of a long payload starts at air bit
 *    2058, which is byte 257 -- past what a byte can hold. Chunks are
 *    numbered from 0 here and below, as the codec counts them;
 *  - the transmit side encodes CODEC_TX_CHUNK_SIZE (80) bytes at a time, so
 *    fec23() is called with 640 input bits, from an 80-byte temporary.
 *
 * The scan window is a #define inside a hardware file -- scan_wait() spins on
 * TIMER0 and reads the radio's packet strobe -- so that part is read as text,
 * the way test_cc2400_wait.c reads the same file, and checked against the
 * clock the firmware's own headers define.
 *
 * This file is part of Project Ubertooth and is released under the terms of the
 * GPL. Refer to COPYING for more information.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The anonymous mapping the guarded input below uses is spelled MAP_ANON on
 * the BSDs. */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#include "minunit.h"
#include "source_scan.h"

#include <ubtbr/bb.h>
#include <ubtbr/btphy.h>
#include <ubtbr/codec.h>

/* The two diagnostics every btbr source can reach, declared by
 * stubs-btbr/ubtbr/debug.h. The codec calls DIE() on an unaligned offset it
 * does not implement, which in a test means the test asked for something the
 * firmware never asks for, so failing there is the point. */
void cprintf(char *fmt, ...)
{
	(void)fmt;
}

void __attribute__((noreturn)) die(char *fmt, ...)
{
	va_list args;

	fflush(stdout);
	fputs("die: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	exit(1);
}

/* ---- test data ---- */

/*
 * Filler with no period of 256, so a read that lands 256 bytes early gives a
 * different answer from the right one. A counter would not: (i*k)%256 repeats
 * every 256 bytes, which is exactly the distance a truncated byte offset is
 * wrong by.
 */
static void fill_pattern(uint8_t *buf, unsigned size, uint32_t seed)
{
	uint32_t x = seed | 1;
	unsigned i;

	for (i = 0; i < size; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		buf[i] = (uint8_t)(x >> 7);
	}
}

/* The air offset the Nth receive chunk of a payload with no 2/3 FEC starts at.
 * The header ends at bit 58 and each chunk is CODEC_RX_CHUNK_SIZE bytes. */
#define RX_CHUNK_AIR_BIT(n)	(58u + 8u * CODEC_RX_CHUNK_SIZE * (n))

/* ---- null_decode(): the air buffer's byte offset ---- */

/* The first chunk, where the offset is small: the same code path, as a check
 * that the expected values below are computed the way the codec reads. */
MU_TEST(test_null_decode_reads_the_first_chunk)
{
	uint8_t air[512], out[CODEC_RX_CHUNK_SIZE], want[CODEC_RX_CHUNK_SIZE];
	unsigned of = RX_CHUNK_AIR_BIT(0);
	unsigned byte = of / 8;
	unsigned i;

	fill_pattern(air, sizeof(air), 0x2fa31);
	memset(out, 0, sizeof(out));
	/* The payload starts 2 bits into air byte 7. */
	mu_assert_int_eq(2, (int)(of & 7));
	for (i = 0; i < sizeof(want); i++)
		want[i] = (uint8_t)((air[byte + i] >> 2) | (air[byte + i + 1] << 6));

	null_decode(out, air, of, sizeof(out));
	mu_assert(memcmp(out, want, sizeof(out)) == 0,
		"first chunk decoded from the wrong place");
}

/* Chunk 25 of a long payload starts at air bit 2058, so the byte offset is
 * 257. Truncated to a byte that is 1, and the chunk is decoded 256 bytes
 * earlier in the air buffer. */
MU_TEST(test_null_decode_crosses_air_bit_2048)
{
	uint8_t air[512], out[CODEC_RX_CHUNK_SIZE], want[CODEC_RX_CHUNK_SIZE];
	uint8_t truncated[CODEC_RX_CHUNK_SIZE];
	unsigned of = RX_CHUNK_AIR_BIT(25);
	unsigned byte = of / 8;
	unsigned i;

	mu_assert_int_eq(2058, (int)of);
	mu_assert_int_eq(257, (int)byte);
	mu_assert_int_eq(2, (int)(of & 7));

	fill_pattern(air, sizeof(air), 0x2fa31);
	memset(out, 0, sizeof(out));
	for (i = 0; i < sizeof(want); i++) {
		want[i] = (uint8_t)((air[byte + i] >> 2) | (air[byte + i + 1] << 6));
		/* What a byte-wide offset would have read: 257 & 0xff = 1. */
		truncated[i] = (uint8_t)((air[1 + i] >> 2) | (air[1 + i + 1] << 6));
	}
	mu_assert(memcmp(want, truncated, sizeof(want)) != 0,
		"test data cannot tell the two offsets apart");

	null_decode(out, air, of, sizeof(out));
	mu_assert(memcmp(out, want, sizeof(out)) == 0,
		"chunk past air bit 2048 decoded from the wrong place");
	mu_assert(memcmp(out, truncated, sizeof(out)) != 0,
		"chunk decoded 256 bytes early");
}

/*
 * The very last chunk of the longest payload, which is where the offset gets
 * as big as it ever does. A DH5 carries a 2-byte payload header, 339 payload
 * bytes and a 2-byte checksum, all of which the codec counts: 343 bytes in 35
 * chunks of 10, so the last chunk is chunk 34, three bytes, starting at air
 * bit 2778 -- byte 347.
 */
MU_TEST(test_null_decode_reaches_the_end_of_the_longest_payload)
{
	uint8_t air[512], out[3], want[3];
	unsigned of = RX_CHUNK_AIR_BIT(34);
	unsigned byte = of / 8;
	unsigned i;

	mu_assert_int_eq(2778, (int)of);
	mu_assert_int_eq(347, (int)byte);
	fill_pattern(air, sizeof(air), 0x51c07);
	memset(out, 0, sizeof(out));
	for (i = 0; i < sizeof(want); i++)
		want[i] = (uint8_t)((air[byte + i] >> 2) | (air[byte + i + 1] << 6));

	null_decode(out, air, of, sizeof(out));
	mu_assert(memcmp(out, want, sizeof(out)) == 0,
		"last chunk of a 339-byte payload decoded from the wrong place");
}

/* ---- null_encode(): the same offset on the way out ---- */

/*
 * The transmit path in codec.c advances the air pointer itself and hands this
 * function only the low three bits of the offset, so a board does not reach
 * the truncation here today. The offset is still the one in the interface
 * (bb.h takes an unsigned bit offset), and it is the same line of code as the
 * receive side's, so it is held to the same width.
 */
MU_TEST(test_null_encode_crosses_air_bit_2048)
{
	/* A transmit chunk, which is what this direction encodes. */
	uint8_t air[512], want[512], in[CODEC_TX_CHUNK_SIZE];
	unsigned of = 2618;
	unsigned byte = of / 8;
	unsigned i;

	mu_assert_int_eq(327, (int)byte);
	mu_assert_int_eq(2, (int)(of & 7));

	fill_pattern(in, sizeof(in), 0x9e8b3);
	memset(air, 0, sizeof(air));
	memset(want, 0, sizeof(want));

	/* The payload lands 2 bits into air byte 327, so each air byte carries
	 * the low 6 bits of one input byte and the high 2 bits of the one
	 * before. Nothing else in the buffer is touched -- in particular not
	 * byte 71, where a byte-wide offset would have put all of this. */
	want[byte] = (uint8_t)(in[0] << 2);
	for (i = 0; i + 1 < sizeof(in); i++)
		want[byte + 1 + i] = (uint8_t)((in[i] >> 6) | (in[i + 1] << 2));
	want[byte + sizeof(in)] = (uint8_t)(in[sizeof(in) - 1] >> 6);

	null_encode(air, in, of, sizeof(in));
	mu_assert(memcmp(air, want, sizeof(air)) == 0,
		"chunk past air bit 2048 encoded into the wrong place");
}

/* ---- fec23(): one group per ten input bits ---- */

/* What one transmit chunk asks of fec23(): 80 bytes in, 120 bytes out. */
#define FEC23_CHUNK_BITS	(8u * CODEC_TX_CHUNK_SIZE)
#define FEC23_CHUNK_GROUPS	(FEC23_CHUNK_BITS / 10u)
#define FEC23_CHUNK_OUT_BITS	(FEC23_CHUNK_GROUPS * 15u)

/* The air offset a chunk is written at is 2 bits into a byte, as the codec's
 * own offsets are (the header ends at bit 58). */
#define FEC23_OUT_OF		2u

/*
 * What 2/3 FEC of nbits input bits should put in the air buffer, written here
 * one bit at a time: a group of 10 input bits becomes those 10 bits plus the
 * 5-bit parity word from the table, at 15 bits per group from out_of. The
 * codec packs the same thing with byte-wide macros chosen by the offset, so
 * this is an independent account of how many groups there are and where each
 * one lands. The parity table itself is the firmware's; it is not what these
 * cases are about.
 */
extern const uint8_t fec23_tbl[1024];

static void fec23_reference(uint8_t *out, const uint8_t *in, unsigned out_of,
                            unsigned nbits)
{
	/* A last group with fewer than 10 bits left still goes out whole. */
	unsigned groups = (nbits + 9) / 10;
	unsigned g, b;

	for (g = 0; g < groups; g++) {
		unsigned data = 0, code;

		for (b = 0; b < 10; b++) {
			unsigned bit = g * 10 + b;

			data |= (unsigned)((in[bit / 8] >> (bit % 8)) & 1) << b;
		}
		code = data | ((unsigned)fec23_tbl[data] << 10);
		for (b = 0; b < 15; b++) {
			unsigned bit = out_of + g * 15 + b;

			if ((code >> b) & 1)
				out[bit / 8] |= (uint8_t)(1u << (bit % 8));
		}
	}
}

MU_TEST(test_fec23_writes_one_group_per_ten_input_bits)
{
	uint8_t in[CODEC_TX_CHUNK_SIZE], out[256], want[256];

	mu_assert_int_eq(640, (int)FEC23_CHUNK_BITS);
	mu_assert_int_eq(64, (int)FEC23_CHUNK_GROUPS);
	mu_assert_int_eq(960, (int)FEC23_CHUNK_OUT_BITS);

	fill_pattern(in, sizeof(in), 0x7c0de);
	memset(out, 0, sizeof(out));
	memset(want, 0, sizeof(want));

	fec23(out, in, FEC23_OUT_OF, FEC23_CHUNK_BITS);
	fec23_reference(want, in, FEC23_OUT_OF, FEC23_CHUNK_BITS);
	mu_assert(memcmp(out, want, sizeof(out)) == 0,
		"a transmit chunk is not 64 groups of 15 bits");
}

/*
 * A chunk that is not a whole number of groups -- an 18-byte DM1 is 144 bits,
 * 14 groups and a part one -- still ends after its last group, 15 of them.
 *
 * What the part group's top 6 bits should be is a separate question: the
 * encoder takes them from the input buffer, where the Bluetooth specification
 * pads with zeros, and the codec then accounts for 9 fewer air bits than the
 * group occupies. Both are older than the loop bound this case is about and
 * are tracked separately; this case pins the group count only, so the input
 * is filled past the chunk rather than left for the encoder to find.
 */
MU_TEST(test_fec23_rounds_a_part_group_up)
{
	uint8_t in[CODEC_TX_CHUNK_SIZE], out[256], want[256];
	unsigned nbits = 18 * 8;

	fill_pattern(in, sizeof(in), 0x3ab91);
	memset(out, 0, sizeof(out));
	memset(want, 0, sizeof(want));

	fec23(out, in, FEC23_OUT_OF, nbits);
	fec23_reference(want, in, FEC23_OUT_OF, nbits);
	mu_assert(memcmp(out, want, sizeof(out)) == 0,
		"a part-group chunk is not 15 groups of 15 bits");
}

/* The chunk after this one starts 2 bits into the same byte and ORs itself in,
 * so the bits above the last group's must be left alone. */
MU_TEST(test_fec23_leaves_the_next_chunks_bits_alone)
{
	/* The chunk is the first 80 bytes. What follows it is set, because on
	 * the board what follows the transmit temporary is the rest of the
	 * stack: a chunk's worth of groups too many, read from zeroed memory,
	 * would write nothing and leave this looking right. */
	uint8_t in[2 * CODEC_TX_CHUNK_SIZE], out[256];
	unsigned end = FEC23_OUT_OF + FEC23_CHUNK_OUT_BITS;
	unsigned stray = 0, i;

	fill_pattern(in, CODEC_TX_CHUNK_SIZE, 0x7c0de);
	memset(in + CODEC_TX_CHUNK_SIZE, 0xff, CODEC_TX_CHUNK_SIZE);
	memset(out, 0, sizeof(out));
	fec23(out, in, FEC23_OUT_OF, FEC23_CHUNK_BITS);

	/* 2 + 960 bits is 2 bits into byte 120. */
	mu_assert_int_eq(120, (int)(end / 8));
	mu_assert_int_eq(0, out[end / 8] >> (end & 7));
	/* Counted rather than asserted byte by byte, so an encoder that ran on
	 * reports one number instead of a page of them. */
	for (i = end / 8 + 1; i < sizeof(out); i++)
		stray += out[i] != 0;
	mu_assert_int_eq(0, (int)stray);
}

/* 2/3 FEC and back, so the groups the loop wrote carry the input they should.
 * unfec23() is the decoder the receive side uses, in the same file. */
MU_TEST(test_fec23_round_trips_through_unfec23)
{
	uint8_t in[CODEC_TX_CHUNK_SIZE], out[256], back[CODEC_TX_CHUNK_SIZE];

	fill_pattern(in, sizeof(in), 0x1f3b7);
	memset(out, 0, sizeof(out));
	memset(back, 0, sizeof(back));

	fec23(out, in, FEC23_OUT_OF, FEC23_CHUNK_BITS);
	unfec23(back, out, FEC23_OUT_OF, FEC23_CHUNK_BITS);
	mu_assert(memcmp(in, back, sizeof(in)) == 0,
		"a transmit chunk did not survive 2/3 FEC and back");
}

/*
 * Nothing is read past the input. The transmit path's input is an 80-byte
 * temporary on the stack, so reading past it reads whatever else is there;
 * here the 80 bytes end on a page boundary with the next page unmapped, so a
 * read past them is a fault instead. The handler below turns that fault into
 * a failed assertion rather than a crash, which keeps the rest of the cases
 * running (and, on macOS, keeps the crash reporter out of it).
 */
static sigjmp_buf fault_jump;
static volatile sig_atomic_t faulted;

static void fault_handler(int sig)
{
	(void)sig;
	faulted = 1;
	siglongjmp(fault_jump, 1);
}

MU_TEST(test_fec23_reads_nothing_past_its_input)
{
	long page = sysconf(_SC_PAGESIZE);
	struct sigaction act, old_segv, old_bus;
	uint8_t *region, *in;
	uint8_t out[256];

	mu_assert_fatal(page >= CODEC_TX_CHUNK_SIZE, "page smaller than a chunk");
	region = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mu_assert_fatal(region != MAP_FAILED, "cannot map the guarded input");
	mu_assert_fatal(mprotect(region + page, (size_t)page, PROT_NONE) == 0,
		"cannot unmap the guard page");

	/* The input ends where the guard page starts. */
	in = region + page - CODEC_TX_CHUNK_SIZE;
	memset(in, 0xff, CODEC_TX_CHUNK_SIZE);
	memset(out, 0, sizeof(out));

	faulted = 0;
	memset(&act, 0, sizeof(act));
	act.sa_handler = fault_handler;
	sigemptyset(&act.sa_mask);
	sigaction(SIGSEGV, &act, &old_segv);
	sigaction(SIGBUS, &act, &old_bus);
	if (sigsetjmp(fault_jump, 1) == 0)
		fec23(out, in, FEC23_OUT_OF, FEC23_CHUNK_BITS);
	sigaction(SIGSEGV, &old_segv, NULL);
	sigaction(SIGBUS, &old_bus, NULL);

	mu_assert(!faulted, "fec23 read past the end of its input");
	munmap(region, (size_t)page * 2);
}

/* ---- the scan task's syncword window ---- */

/*
 * The value of one of these windows: a sum of integers, with optional
 * parentheses, as both are written ("2500", "(610+100)"). Anything else in
 * there is not understood, and says so by returning 0.
 */
static int parse_sum(const char *text, const char **after, long *value)
{
	long total = 0;
	int terms = 0;

	*value = 0;
	while (*text == ' ' || *text == '\t' || *text == '(')
		text++;
	for (;;) {
		char *end;
		long term = strtol(text, &end, 0);

		if (end == text)
			return 0;
		total += term;
		terms++;
		text = end;
		while (*text == ' ' || *text == '\t')
			text++;
		if (*text != '+')
			break;
		text++;
		while (*text == ' ' || *text == '\t')
			text++;
	}
	while (*text == ')' || *text == ' ' || *text == '\t')
		text++;
	*after = text;
	*value = total;
	return terms > 0;
}

/*
 * The value of a #define, and the microseconds its comment claims, from a
 * source file. The comment is part of what is being checked here, so this
 * reads the raw line rather than using strip_comments(). claimed_usec is -1
 * when the line's comment names no microseconds.
 */
static int read_define_usec(const char *path, const char *name,
                            long *value, long *claimed_usec)
{
	char line[SCAN_LINE_MAX];
	FILE *f = fopen(path, "r");
	int found = 0;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p, *rest;
		char prev;

		if (strstr(line, "#define") == NULL)
			continue;
		p = strstr(line, name);
		if (p == NULL)
			continue;
		prev = (p == line) ? ' ' : p[-1];
		if (is_ident_char(prev) || is_ident_char(p[strlen(name)]))
			continue;
		p += strlen(name);
		if (!parse_sum(p, &rest, value))
			continue;
		/* "...250usec..." in the trailing comment. */
		*claimed_usec = -1;
		p = strstr(rest, "usec");
		if (p != NULL) {
			const char *digits = p;

			while (digits > rest && isdigit((unsigned char)digits[-1]))
				digits--;
			if (digits != p)
				*claimed_usec = strtol(digits, NULL, 10);
		}
		found = 1;
		break;
	}
	fclose(f);
	return found;
}

/*
 * The offset in the slot a packet is expected at, from the image's radio
 * header. That header is read as text too: its inline helpers write radio
 * registers and board LEDs, so it cannot be compiled here.
 */
static long expected_rx_offset(void)
{
	long value = 0, usec = 0;

	if (!read_define_usec(FW_BTBR_DIR "/include/ubtbr/rf.h",
			"RF_EXPECTED_RX_CLKN_OFFSET", &value, &usec))
		return -1;
	return value;
}

/* What the slot clock counts, from the image's own headers: TIMER0 is
 * prescaled to 100ns steps and reset once per 312.5usec slot. */
MU_TEST(test_slot_clock_counts_hundred_nanosecond_steps)
{
	mu_assert_int_eq(100, CLKN_OFFSET_PRESCALE_VAL * PERIPH_CLK_NS);
	mu_assert_int_eq(3125, CLKN_OFFSET_RESET_VAL);
	mu_assert_int_eq(312500, CLKN_OFFSET_RESET_VAL * 100);
}

/* The window the scan spins in, in those steps. A scan has no clock to go by,
 * so it has to watch nearly the whole slot: anything shorter than the offset a
 * packet is expected at closes before the packet it is waiting for. */
MU_TEST(test_scan_syncword_window_is_250_microseconds)
{
	const char *path = FW_BTBR_DIR "/src/btphy/scan_task.c";
	long window = 0, claimed = 0;
	long expected_rx = expected_rx_offset();

	mu_assert_fatal(read_define_usec(path, "SCAN_WAIT_SYNCWORD",
			&window, &claimed),
		"SCAN_WAIT_SYNCWORD not found in scan_task.c");
	mu_assert_fatal(expected_rx == 610,
		"RF_EXPECTED_RX_CLKN_OFFSET not read from rf.h");

	/* The comment says microseconds; the value is in 100ns steps. */
	mu_assert_int_eq(250, (int)claimed);
	mu_assert_int_eq((int)(claimed * 1000 / 100), (int)window);
	mu_assert_int_eq(2500, (int)window);

	/* Inside one slot, and past the offset a packet is expected at. The
	 * receive task next door allows 10usec of slack on that offset. */
	mu_assert(window < CLKN_OFFSET_RESET_VAL, "window outlasts the slot");
	mu_assert(window > expected_rx + 100,
		"window closes before a packet is expected");
}

/* The receive task's own window, which sets the convention the one above is
 * read against: a packet it is synchronised to arrives at a known offset. */
MU_TEST(test_receive_syncword_window_follows_the_same_units)
{
	const char *path = FW_BTBR_DIR "/src/btphy/rx_task.c";
	long window = 0, claimed = 0;

	mu_assert_fatal(read_define_usec(path, "WAIT_SYNCWORD", &window, &claimed),
		"WAIT_SYNCWORD not found in rx_task.c");
	/* 61usec, plus 10usec of slack, in 100ns steps. */
	mu_assert_int_eq(610, (int)expected_rx_offset());
	mu_assert_int_eq(710, (int)window);
	mu_assert_int_eq((int)(expected_rx_offset() + 100), (int)window);
	mu_assert(window < CLKN_OFFSET_RESET_VAL, "window outlasts the slot");
}

int main(void)
{
	printf("btbr codec offset and scan window tests\n");
	MU_RUN(test_null_decode_reads_the_first_chunk);
	MU_RUN(test_null_decode_crosses_air_bit_2048);
	MU_RUN(test_null_decode_reaches_the_end_of_the_longest_payload);
	MU_RUN(test_null_encode_crosses_air_bit_2048);
	MU_RUN(test_fec23_writes_one_group_per_ten_input_bits);
	MU_RUN(test_fec23_rounds_a_part_group_up);
	MU_RUN(test_fec23_leaves_the_next_chunks_bits_alone);
	MU_RUN(test_fec23_round_trips_through_unfec23);
	MU_RUN(test_fec23_reads_nothing_past_its_input);
	MU_RUN(test_slot_clock_counts_hundred_nanosecond_steps);
	MU_RUN(test_scan_syncword_window_is_250_microseconds);
	MU_RUN(test_receive_syncword_window_follows_the_same_units);
	return mu_report();
}
