/*
 * Hardware-independent tests for ubertooth-debug's register reads: the exit
 * status reflects every read, a failed read names its register, and the other
 * registers are still read and decoded.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
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

int main(void)
{
	MU_RUN(success_exits_zero_even_for_a_nonzero_register_value);
	MU_RUN(an_early_failure_is_not_masked_by_a_later_success);
	MU_RUN(a_short_reply_adds_no_second_line);
	return mu_report();
}
