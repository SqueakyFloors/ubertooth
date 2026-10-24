/*
 * Unit tests for firmware/bluetooth_rxtx/cc2400_wait.c
 *
 * These run on the host (plain gcc). cc2400_wait_fs_unlock() is pure control
 * flow over two radio primitives, so the test supplies fake cc2400_status()
 * and cc2400_strobe() implementations and checks the loop's contract.
 *
 * The regression under test is issue #158: the firmware waited for the
 * synthesizer to unlock with `while ((cc2400_status() & FS_LOCK));`. Because
 * the CC2400 SPI bus is bit-banged and shared with interrupt handlers that
 * also strobe the radio, that status read can stay stuck with FS_LOCK set --
 * and USB in this firmware is polled, not interrupt-driven, so a main loop
 * stuck in that wait makes the board answer no control request at all until it
 * is re-plugged. The wait must therefore always terminate.
 */
#include "minunit.h"
#include "cc2400_wait.h"

/* ---- fake radio ---- */

/* Status bytes handed out in order; the last one repeats forever. */
static u8 fake_status_seq[8];
static unsigned fake_status_len;
static unsigned status_reads;
static unsigned srfoff_strobes;
static unsigned other_strobes;

static void fake_radio_reset(void)
{
	fake_status_len = 0;
	status_reads = 0;
	srfoff_strobes = 0;
	other_strobes = 0;
}

static void fake_radio_status_sequence(const u8 *seq, unsigned len)
{
	unsigned i;
	for (i = 0; i < len && i < 8; ++i)
		fake_status_seq[i] = seq[i];
	fake_status_len = (len < 8) ? len : 8;
}

u8 cc2400_status(void)
{
	unsigned i = status_reads++;
	if (fake_status_len == 0)
		return 0;
	if (i >= fake_status_len)
		i = fake_status_len - 1; /* last value repeats */
	return fake_status_seq[i];
}

u8 cc2400_strobe(u8 reg)
{
	if (reg == SRFOFF)
		++srfoff_strobes;
	else
		++other_strobes;
	return 0;
}

/* ---- tests ---- */

/* Already unlocked: return success on the first read, strobe nothing. */
MU_TEST(test_returns_immediately_when_unlocked)
{
	const u8 seq[] = { 0x00 };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(1, cc2400_wait_fs_unlock());
	mu_assert_int_eq(1, (int)status_reads);
	mu_assert_int_eq(0, (int)srfoff_strobes);
}

/* Locked for a few polls, then unlocked: still success, and no more polls
 * than it took. XOSC16M_STABLE set alongside FS_LOCK must not confuse it. */
MU_TEST(test_returns_success_once_unlocked)
{
	const u8 seq[] = { FS_LOCK, FS_LOCK | XOSC16M_STABLE, XOSC16M_STABLE };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 3);

	mu_assert_int_eq(1, cc2400_wait_fs_unlock());
	mu_assert_int_eq(3, (int)status_reads);
}

/* The regression: a synthesizer that never reports unlocked must not hang the
 * caller. The wait gives up after a bounded number of polls and says so. */
MU_TEST(test_gives_up_when_lock_never_clears)
{
	const u8 seq[] = { FS_LOCK };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(0, cc2400_wait_fs_unlock());
	mu_assert_int_eq(FS_UNLOCK_POLLS, (int)status_reads);
}

/* While waiting it re-asserts SRFOFF, so a strobe lost to SPI bus contention
 * (or a synthesizer an interrupt handler switched back on) is retried. Only
 * SRFOFF is ever strobed: the wait must not turn the synthesizer on. */
MU_TEST(test_restrobes_srfoff_while_waiting)
{
	const u8 seq[] = { FS_LOCK };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(0, cc2400_wait_fs_unlock());
	mu_assert_int_eq(FS_UNLOCK_POLLS / FS_UNLOCK_RESTROBE_INTERVAL,
	                 (int)srfoff_strobes);
	mu_assert_int_eq(0, (int)other_strobes);
}

/* A short wait that succeeds before the first re-strobe interval must not
 * strobe at all -- the fast path stays as cheap as it was. */
MU_TEST(test_no_strobe_on_short_wait)
{
	const u8 seq[] = { FS_LOCK, FS_LOCK, 0x00 };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 3);

	mu_assert_int_eq(1, cc2400_wait_fs_unlock());
	mu_assert_int_eq(0, (int)srfoff_strobes);
}

int main(void)
{
	printf("CC2400 bounded synthesizer-unlock wait tests\n");
	MU_RUN(test_returns_immediately_when_unlocked);
	MU_RUN(test_returns_success_once_unlocked);
	MU_RUN(test_gives_up_when_lock_never_clears);
	MU_RUN(test_restrobes_srfoff_while_waiting);
	MU_RUN(test_no_strobe_on_short_wait);
	return mu_report();
}
