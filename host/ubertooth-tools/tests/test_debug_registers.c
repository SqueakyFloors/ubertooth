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

/* Register 0x15 is MDMTST1; it used to carry 0x14's name, so no name of its
 * own reached it and -r MDMTST1 could not name it. */
MU_TEST(every_register_name_in_the_table_is_its_own)
{
	mu_assert_int_eq(0x14, cc2400_name2reg("%MDMTST0"));
	mu_assert_int_eq(0x15, cc2400_name2reg("%MDMTST1"));
	mu_assert(strcmp(cc2400_reg2name(0x15), "%MDMTST1") == 0, cc2400_reg2name(0x15));
}

int main(void)
{
	MU_RUN(success_exits_zero_even_for_a_nonzero_register_value);
	MU_RUN(an_early_failure_is_not_masked_by_a_later_success);
	MU_RUN(a_short_reply_adds_no_second_line);
	MU_RUN(a_field_is_printed_from_the_bits_its_picture_draws);
	MU_RUN(a_breakpoint_state_with_its_top_bit_set_is_not_disabled);
	MU_RUN(a_field_draws_the_bits_it_reports);
	MU_RUN(every_register_name_in_the_table_is_its_own);
	return mu_report();
}
