/*
 * Unit tests for find_access_code() in bluetooth_rxtx/bluetooth.c.
 *
 * find_access_code() slides a 64-bit window through one demodulated symbol
 * buffer (most significant bit of each byte first) and returns the bit offset
 * just past a window that matches target.syncword within MAX_SYNCWORD_ERRS,
 * or -1. Each call must search only the buffer it is given: an earlier
 * version kept the window in a file-scope global and reseeded it only when it
 * was zero, so a second call reused the first buffer's bits.
 *
 * bluetooth.c is included directly so the test does not need test-only
 * symbols in production firmware. ubertooth.h in this directory supplies the
 * hardware-free types and constants that bluetooth.h needs.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "../bluetooth_rxtx/bluetooth.c"

#define BUF_BITS (8 * DMA_SIZE)

static const u64 AC_A = UINT64_C(0x0123456789abcdef);
static const u64 AC_B = UINT64_C(0x9c3a5f0e2b7d4168);

/* Write a 64-bit value into buf starting at stream bit `offset`, MSB first. */
static void place_ac(u8 *buf, int offset, u64 value)
{
	int k;
	for (k = 0; k < 64; k++) {
		int pos = offset + k;
		u8 mask = (u8)(0x80 >> (pos % 8));
		if ((value >> (63 - k)) & 1)
			buf[pos / 8] |= mask;
		else
			buf[pos / 8] &= (u8)~mask;
	}
}

static void reset_target(u64 ac)
{
	memset(&target, 0, sizeof(target));
	target.syncword = ac;
	syncword = 0;  /* what precalc() leaves behind after SET_BDADDR */
}

/* A match in the first 64 bits is reported at offset 64 (end of window). */
MU_TEST(test_finds_initial_window)
{
	u8 buf[DMA_SIZE] = {0};
	reset_target(AC_A);
	place_ac(buf, 0, AC_A);
	mu_assert_int_eq(64, find_access_code(buf));
}

/*
 * Two consecutive calls with different buffers: the second call must search
 * only its own buffer, not the window left behind by the first.
 */
MU_TEST(test_second_call_ignores_previous_buffer)
{
	u8 first[DMA_SIZE] = {0};
	u8 second[DMA_SIZE] = {0};
	reset_target(AC_A);

	place_ac(first, 0, AC_A);
	mu_assert_int_eq(64, find_access_code(first));

	/* No access code anywhere in the second buffer. */
	mu_assert_int_eq(-1, find_access_code(second));

	/* Access code later in a third buffer: reported at its own offset. */
	place_ac(second, 100, AC_A);
	mu_assert_int_eq(164, find_access_code(second));
}

/* A no-match call must not leave state that changes the next call's result. */
MU_TEST(test_result_independent_of_call_history)
{
	u8 noise[DMA_SIZE];
	u8 buf[DMA_SIZE] = {0};
	int i;
	reset_target(AC_B);
	for (i = 0; i < DMA_SIZE; i++)
		noise[i] = (u8)(0x5a ^ (i * 37));
	place_ac(buf, 37, AC_B);

	int fresh = find_access_code(buf);
	mu_assert_int_eq(101, fresh);
	(void)find_access_code(noise);
	mu_assert_int_eq(fresh, find_access_code(buf));
}

/*
 * Every bit of the buffer is examined in order: matches starting at any bit
 * offset, including inside byte 8 (the first byte after the seed) and at
 * non-byte-aligned positions, are found at offset + 64.
 */
MU_TEST(test_finds_every_start_offset)
{
	int off;
	for (off = 0; off + 64 <= BUF_BITS - 65; off++) {
		u8 buf[DMA_SIZE] = {0};
		reset_target(AC_B);
		place_ac(buf, off, AC_B);
		mu_assert_int_eq(off + 64, find_access_code(buf));
	}
}

/* Matches ending in the last 64 bits are not searched (header must follow). */
MU_TEST(test_tail_not_searched)
{
	u8 buf[DMA_SIZE] = {0};
	reset_target(AC_B);
	place_ac(buf, BUF_BITS - 64, AC_B);
	mu_assert_int_eq(-1, find_access_code(buf));
}

/* Error threshold: fewer than MAX_SYNCWORD_ERRS differing bits matches. */
MU_TEST(test_error_threshold_is_strict)
{
	u8 buf[DMA_SIZE] = {0};
	reset_target(UINT64_C(0x0f));          /* 4 bit errors vs all-zero */
	mu_assert_int_eq(64, find_access_code(buf));
	reset_target(UINT64_C(0x1f));          /* 5 bit errors */
	mu_assert_int_eq(-1, find_access_code(buf));
}

int main(void)
{
	printf("find_access_code tests\n");
	MU_RUN(test_finds_initial_window);
	MU_RUN(test_second_call_ignores_previous_buffer);
	MU_RUN(test_result_independent_of_call_history);
	MU_RUN(test_finds_every_start_offset);
	MU_RUN(test_tail_not_searched);
	MU_RUN(test_error_threshold_is_strict);
	return mu_report();
}
