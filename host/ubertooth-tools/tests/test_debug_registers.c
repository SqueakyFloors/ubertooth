/*
 * Hardware-independent tests for ubertooth-debug's register reads: the exit
 * status reflects every read, a failed read names its register, the other
 * registers are still read and decoded, and each decoded field is printed
 * from the bits its picture draws.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "cc2400.h"
#include "debug_registers.h"

static struct {
	int fail_reg;  /* this register fails... */
	int fail_rc;   /* ...with this code */
	int value;     /* every other register reads this value */
	int reads;
} fake;

static int fake_read(void *context, uint8_t reg)
{
	(void)context;
	fake.reads++;
	if (reg == fake.fail_reg)
		return fake.fail_rc;
	return fake.value;
}

struct captured {
	int status;
	char out[4096];
	char err[512];
};

static void slurp(FILE *f, char *buf, size_t size)
{
	size_t n;
	rewind(f);
	n = fread(buf, 1, size - 1, f);
	buf[n] = '\0';
	fclose(f);
}

static struct captured run(const int *regs, int n)
{
	struct captured c;
	FILE *out = tmpfile(), *err = tmpfile();

	memset(&c, 0, sizeof(c));
	fake.reads = 0;
	c.status = debug_read_registers(fake_read, NULL, regs, n, 0, out, err);
	slurp(out, c.out, sizeof(c.out));
	slurp(err, c.err, sizeof(c.err));
	return c;
}

MU_TEST(success_exits_zero_even_for_a_nonzero_register_value)
{
	/* the old code returned the last value (here 0x1234) as the exit status */
	const int regs[] = { 0x05, 0x06 };
	struct captured c;

	fake.fail_reg = -1;
	fake.value = 0x1234;
	c = run(regs, 2);
	mu_assert_int_eq(0, c.status);
	mu_assert_int_eq(2, fake.reads);
	mu_assert_int_eq(0, (long)strlen(c.err));
	mu_assert(strlen(c.out) > 0, "decoded");
}

MU_TEST(an_early_failure_is_not_masked_by_a_later_success)
{
	const int regs[] = { 0x05, 0x06, 0x0d };
	struct captured c;

	fake.fail_reg = 0x05;
	fake.fail_rc = -7;
	fake.value = 0x000f;
	c = run(regs, 3);
	mu_assert_int_eq(1, c.status);
	mu_assert_int_eq(3, fake.reads);
	mu_assert(strcmp(c.err, "error: register 0x05 read failed\n") == 0, c.err);
	/* the registers after the failure were decoded, not just read */
	mu_assert(strstr(c.out, "RSSI") != NULL, "0x06 (RSSI) decoded");
	mu_assert(strstr(c.out, "0x05") == NULL && strstr(c.out, "FREND") == NULL, "failed 0x05 not decoded");
}

MU_TEST(a_short_reply_adds_no_second_line)
{
	const int regs[] = { 0x06 };
	struct captured c;

	fake.fail_reg = 0x06;
	fake.fail_rc = DEBUG_ERROR_SHORT_REPLY;
	c = run(regs, 1);
	mu_assert_int_eq(1, c.status);
	mu_assert_int_eq(0, (long)strlen(c.err));
}

/* Decode one register at verbosity 2, which prints the reserved fields too. */
static void decode(int reg, unsigned short value, char *buf, size_t size)
{
	FILE *out = tmpfile();

	cc2400_decode(out, reg, value, 2);
	slurp(out, buf, size);
}

static int has_line(const char *text, const char *line)
{
	size_t n = strlen(line);
	const char *p = text;

	while (*p) {
		if (strncmp(p, line, n) == 0 && p[n] == '\n')
			return 1;
		p = strchr(p, '\n');
		if (!p)
			break;
		p++;
	}
	return 0;
}

/*
 * Every field's value is read from the bits its picture draws. Each value
 * below sets the top bit of the field named, which a mask one or more bits
 * too narrow used to drop, printing 0 where the radio held a number.
 */
MU_TEST(a_field_is_printed_from_the_bits_its_picture_draws)
{
	static const struct {
		int reg;
		unsigned short value;
		const char *line;
	} cases[] = {
		{ 0x0e, 0x1000, "...10000 ........ (RW) FSM_STATE_BKPT (16)" },
		{ 0x0e, 0x0000, "...00000 ........ (RW) FSM_STATE_BKPT (disabled)" },
		{ 0x0f, 0x4040, ".1000000 ........ (RO) ADC_I (64)" },
		{ 0x0f, 0x4040, "........ .1000000 (RO) ADC_Q (64)" },
		{ 0x17, 0x0004, "........ .....100 (RW) AGC_ATTEMPTS (4)" },
		{ 0x18, 0x0220, "......10 00...... (RW) AGC_MULT_SLOW (8)" },
		{ 0x18, 0x0220, "........ ..1000.. (RW) AGC_SETTLE_FIXED (8)" },
		{ 0x1a, 0x0210, "......10 000..... (RW) VCO_ARRAY_O (16)" },
		{ 0x1a, 0x0210, "........ ...10000 (RO) VCO_ARRAY_RES (16)" },
		{ 0x1b, 0x0004, "........ .....100 (RW) VCO_DAC_VAL (4)" },
		{ 0x24, 0x0040, "........ .1000000 (RW) - (64)" },
		{ 0x26, 0x0200, "......10 00000000 (RW) - (512)" },
		{ 0x27, 0x0080, "........ 10000... (RW) - (16)" },
		{ 0x28, 0x0040, "........ .1000000 (RW) - (64)" },
		{ 0x2b, 0x0800, "....1000 00000000 (RO) - (2048)" },
	};
	char out[4096];
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		decode(cases[i].reg, cases[i].value, out, sizeof(out));
		mu_assert(has_line(out, cases[i].line), cases[i].line);
	}
}

/* A breakpoint state above 15 is a state, not a disabled breakpoint. */
MU_TEST(a_breakpoint_state_with_its_top_bit_set_is_not_disabled)
{
	char out[4096];

	decode(0x0e, 0x1000, out, sizeof(out));
	mu_assert(strstr(out, "disabled") == NULL, out);
}

/*
 * The two fields whose value came from the wrong bits: FSMTC's
 * TC_TXEND2PAOFF drew its picture over the field above it, and FSTST3's
 * PD_DELAY took its Long/Short text from CHP_DISABLE beside it.
 */
MU_TEST(a_field_draws_the_bits_it_reports)
{
	char out[4096];

	decode(0x0b, 0x0007, out, sizeof(out));
	mu_assert(has_line(out, "........ .....111 (RW) TC_TXEND2PAOFF (7 us)"), out);
	mu_assert(has_line(out, "........ ..000... (RW) END2SWITCH (0 us)"), out);

	decode(0x1d, 0x0400, out, sizeof(out));
	mu_assert(has_line(out, ".....1.. ........ (RW) PD_DELAY (Long)"), out);
	decode(0x1d, 0x0800, out, sizeof(out));
	mu_assert(has_line(out, ".....0.. ........ (RW) PD_DELAY (Short)"), out);
	mu_assert(has_line(out, "....1... ........ (RW) CHP_DISABLE"), out);
}

/*
 * Which of the radio's fields are signed, at the two values that tell them
 * apart. The ADC readings are unsigned, so the top of their range is 127 and
 * not -1. MOD_OFFSET is signed, so its lowest value is -32 steps of
 * 15.625 kHz and not +32.
 */
MU_TEST(a_signed_field_is_printed_signed_and_an_unsigned_one_is_not)
{
	char out[4096];

	decode(0x0f, 0x7f7f, out, sizeof(out));
	mu_assert(has_line(out, ".1111111 ........ (RO) ADC_I (127)"), out);
	mu_assert(has_line(out, "........ .1111111 (RO) ADC_Q (127)"), out);

	decode(0x03, 0x1000, out, sizeof(out));
	mu_assert(has_line(out, "...10000 0....... (RW) MOD_OFFSET (-500.000 kHz)"), out);
	decode(0x03, 0x0F80, out, sizeof(out));
	mu_assert(has_line(out, "...01111 1....... (RW) MOD_OFFSET (484.375 kHz)"), out);
}

/* Register 0x15 is MDMTST1; it used to carry 0x14's name, so no name of its
 * own reached it and -r MDMTST1 could not name it. */
MU_TEST(every_register_name_in_the_table_is_its_own)
{
	mu_assert_int_eq(0x14, cc2400_name2reg("%MDMTST0"));
	mu_assert_int_eq(0x15, cc2400_name2reg("%MDMTST1"));
	mu_assert(strcmp(cc2400_reg2name(0x15), "%MDMTST1") == 0, cc2400_reg2name(0x15));
}

/* ---- -w: the register write list ------------------------------------- */

static struct {
	int fail_reg;  /* this register's write fails... */
	int fail_rc;   /* ...with this code */
	int writes;
	uint8_t reg[DEBUG_REGISTER_WRITES_MAX];
	uint16_t value[DEBUG_REGISTER_WRITES_MAX];
} written;

static int fake_write(void *context, uint8_t reg, uint16_t value)
{
	(void)context;
	if (written.writes < DEBUG_REGISTER_WRITES_MAX) {
		written.reg[written.writes] = reg;
		written.value[written.writes] = value;
	}
	written.writes++;
	if (reg == written.fail_reg)
		return written.fail_rc;
	return 0;
}

/* What the parser accepts, and the values it reads: a %name or an address,
 * hex or decimal, and the whole 16-bit range. */
MU_TEST(a_write_list_is_parsed_into_registers_and_values)
{
	debug_register_write w[DEBUG_REGISTER_WRITES_MAX];
	FILE *err = tmpfile();
	char line[256];
	int n;

	n = debug_parse_register_writes("%grmdm=0x0f61,0x03=0x0029,20=65535",
	                                w, DEBUG_REGISTER_WRITES_MAX, err);
	slurp(err, line, sizeof(line));
	mu_assert_int_eq(3, n);
	mu_assert_int_eq(0, (long)strlen(line));
	mu_assert_int_eq(0x20, w[0].reg);     /* %GRMDM */
	mu_assert_int_eq(0x0f61, w[0].value);
	mu_assert_int_eq(0x03, w[1].reg);
	mu_assert_int_eq(0x0029, w[1].value);
	mu_assert_int_eq(20, w[2].reg);       /* decimal 20, not 0x20 */
	mu_assert_int_eq(0xffff, w[2].value);
}

/* A number without 0x is decimal, including one written with a leading zero:
 * strtol's base 0 would read 010 as octal 8, and refuse 08 and 09 as not
 * numbers at all, neither of which is what the man page says. */
MU_TEST(a_leading_zero_is_decimal_and_not_octal)
{
	debug_register_write w[DEBUG_REGISTER_WRITES_MAX];
	FILE *err = tmpfile();
	char line[256];
	int n = debug_parse_register_writes("010=010,08=09,0x10=0x10", w,
	                                    DEBUG_REGISTER_WRITES_MAX, err);

	slurp(err, line, sizeof(line));
	mu_assert_int_eq(3, n);
	mu_assert_int_eq(0, (long)strlen(line));
	mu_assert_int_eq(10, w[0].reg);
	mu_assert_int_eq(10, w[0].value);
	mu_assert_int_eq(8, w[1].reg);
	mu_assert_int_eq(9, w[1].value);
	/* and 0x is still hex */
	mu_assert_int_eq(16, w[2].reg);
	mu_assert_int_eq(16, w[2].value);
}

/* Every way a -w value can be wrong is one line on standard error and no
 * writes at all, so a half-applied configuration cannot reach the radio. */
MU_TEST(a_bad_write_list_is_refused_whole)
{
	static const char *const bad[] = {
		"",                 /* nothing asked for */
		"%grmdm",           /* no value */
		"%grmdm=",          /* an empty value */
		"=0x1234",          /* no register */
		"%nosuchreg=1",     /* a name the table does not have */
		"0x100=1",          /* past the last register address */
		"0x20=0x10000",     /* past 16 bits */
		"0x20=-1",          /* not a value at all */
		"0x20=0x",          /* a prefix with no digits */
		"0x20=0x-1",        /* the sign strtol would have taken */
		"0x20=1f",          /* hex digits without the prefix */
		"0x20=1,",          /* a trailing comma */
		"0x20=1,,0x21=2",   /* an empty item */
		"0x20=1,0x21",      /* the second item has no value */
		"0x20=1=2",         /* two values */
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		debug_register_write w[DEBUG_REGISTER_WRITES_MAX];
		FILE *err = tmpfile();
		char line[256];
		int n = debug_parse_register_writes(bad[i], w,
		                                    DEBUG_REGISTER_WRITES_MAX, err);

		slurp(err, line, sizeof(line));
		mu_assert_int_eq(-1, n);
		mu_assert(strncmp(line, "error: ", 7) == 0, bad[i]);
		/* One line, naming the value the user wrote. */
		mu_assert(strchr(line, '\n') == line + strlen(line) - 1, line);
	}
}

/* More writes than the list holds is refused rather than cut short. */
MU_TEST(a_write_list_longer_than_the_bound_is_refused)
{
	debug_register_write w[2];
	FILE *err = tmpfile();
	char line[256];
	int n = debug_parse_register_writes("0x20=1,0x21=2,0x22=3", w, 2, err);

	slurp(err, line, sizeof(line));
	mu_assert_int_eq(-1, n);
	mu_assert(strstr(line, "at most 2 register writes") != NULL, line);
}

/* The writes reach the board in the order given, and one refused register
 * neither hides the rest nor hides itself. */
MU_TEST(a_refused_write_names_its_register_and_the_rest_are_written)
{
	debug_register_write w[] = {
		{ 0x20, 0x0f61 }, { 0x03, 0x0029 }, { 0x0b, 0x0004 },
	};
	FILE *err = tmpfile();
	char line[256];
	int status;

	memset(&written, 0, sizeof(written));
	written.fail_reg = 0x03;
	written.fail_rc = -1;
	status = debug_write_registers(fake_write, NULL, w, 3, err);
	slurp(err, line, sizeof(line));

	mu_assert_int_eq(1, status);
	mu_assert_int_eq(3, written.writes);
	mu_assert_int_eq(0x20, written.reg[0]);
	mu_assert_int_eq(0x03, written.reg[1]);
	mu_assert_int_eq(0x0b, written.reg[2]);
	mu_assert(strcmp(line, "error: register 0x03 write failed\n") == 0, line);

	/* All three accepted: nothing printed, status 0. */
	memset(&written, 0, sizeof(written));
	written.fail_reg = -1;
	err = tmpfile();
	status = debug_write_registers(fake_write, NULL, w, 3, err);
	slurp(err, line, sizeof(line));
	mu_assert_int_eq(0, status);
	mu_assert_int_eq(3, written.writes);
	mu_assert_int_eq(0, (long)strlen(line));
}

int main(void)
{
	MU_RUN(a_write_list_is_parsed_into_registers_and_values);
	MU_RUN(a_leading_zero_is_decimal_and_not_octal);
	MU_RUN(a_bad_write_list_is_refused_whole);
	MU_RUN(a_write_list_longer_than_the_bound_is_refused);
	MU_RUN(a_refused_write_names_its_register_and_the_rest_are_written);
	MU_RUN(success_exits_zero_even_for_a_nonzero_register_value);
	MU_RUN(an_early_failure_is_not_masked_by_a_later_success);
	MU_RUN(a_short_reply_adds_no_second_line);
	MU_RUN(a_field_is_printed_from_the_bits_its_picture_draws);
	MU_RUN(a_breakpoint_state_with_its_top_bit_set_is_not_disabled);
	MU_RUN(a_field_draws_the_bits_it_reports);
	MU_RUN(a_signed_field_is_printed_signed_and_an_unsigned_one_is_not);
	MU_RUN(every_register_name_in_the_table_is_its_own);
	return mu_report();
}
