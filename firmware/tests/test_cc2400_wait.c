/*
 * Unit tests for firmware/bluetooth_rxtx/cc2400_wait.c
 *
 * These run on the host (plain gcc). cc2400_wait_fs() is pure control flow
 * over two radio primitives and an optional callback, so the test supplies
 * fake cc2400_status() and cc2400_strobe() implementations and checks the
 * loop's contract.
 *
 * Scope: the bounded wait only. The rest of the LE PHY wedge fix -- that
 * mode's teardown and the NULL-buffer guards in le_phy.c -- is register and
 * interrupt sequencing against real hardware, verified on a board rather than
 * here.
 *
 * What the wait has to guarantee: the firmware used to spin on
 * `while ((cc2400_status() & FS_LOCK));`. Because the CC2400 SPI bus is
 * bit-banged and shared with interrupt handlers that also strobe the radio,
 * that status read can stay stuck with FS_LOCK set -- and USB in this firmware
 * is polled, not interrupt-driven, so a main loop stuck in that wait makes the
 * board answer no control request at all until it is re-plugged. The wait must
 * therefore always terminate.
 */
#include "minunit.h"
#include "cc2400_wait.h"

/* ---- fake radio ---- */

/* Status bytes handed out in order; the last one repeats forever. */
#define FAKE_STATUS_MAX 8
static u8 fake_status_seq[FAKE_STATUS_MAX];
static unsigned fake_status_len;
static unsigned status_reads;
static unsigned srfoff_strobes;
static unsigned other_strobes;

static unsigned sfson_strobes;

/* When non-zero, report FS_LOCK clear on this poll number (1-based) and
 * locked on every poll before it, overriding the status sequence. */
static unsigned unlock_poll;

/* The mirror image: report FS_LOCK set on this poll number and clear before
 * it, for the lock wait. */
static unsigned lock_poll;

static void fake_radio_reset(void)
{
	fake_status_len = 0;
	status_reads = 0;
	srfoff_strobes = 0;
	sfson_strobes = 0;
	other_strobes = 0;
	unlock_poll = 0;
	lock_poll = 0;
}

static void fake_radio_status_sequence(const u8 *seq, unsigned len)
{
	unsigned i;
	if (len > FAKE_STATUS_MAX) {
		printf("  FAIL test bug: status sequence longer than %d\n",
		       FAKE_STATUS_MAX);
		len = FAKE_STATUS_MAX;
	}
	for (i = 0; i < len; ++i)
		fake_status_seq[i] = seq[i];
	fake_status_len = len;
}

static void fake_radio_unlock_at_poll(unsigned n)
{
	unlock_poll = n;
}

static void fake_radio_lock_at_poll(unsigned n)
{
	lock_poll = n;
}

u8 cc2400_status(void)
{
	unsigned i = status_reads++;
	if (unlock_poll != 0)
		return (status_reads == unlock_poll) ? 0 : FS_LOCK;
	if (lock_poll != 0)
		return (status_reads == lock_poll) ? FS_LOCK : 0;
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
	else if (reg == SFSON)
		++sfson_strobes;
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

/* The case the re-strobe exists for: one corrective SRFOFF, then the
 * synthesizer reports unlocked. Success, exactly one strobe, nothing else. */
MU_TEST(test_succeeds_after_one_restrobe)
{
	fake_radio_reset();
	fake_radio_unlock_at_poll(FS_UNLOCK_RESTROBE_INTERVAL + 8);

	mu_assert_int_eq(1, cc2400_wait_fs_unlock());
	mu_assert_int_eq(FS_UNLOCK_RESTROBE_INTERVAL + 8, (int)status_reads);
	mu_assert_int_eq(1, (int)srfoff_strobes);
	mu_assert_int_eq(0, (int)other_strobes);
}

/* Boundary: unlock reported on the very last permitted poll still counts as
 * success, so the bound is not off by one. */
MU_TEST(test_unlock_on_last_poll_succeeds)
{
	fake_radio_reset();
	fake_radio_unlock_at_poll(FS_UNLOCK_POLLS);

	mu_assert_int_eq(1, cc2400_wait_fs_unlock());
	mu_assert_int_eq(FS_UNLOCK_POLLS, (int)status_reads);
}

/* One poll later is a timeout: the loop does not take an extra poll. */
MU_TEST(test_unlock_one_poll_too_late_times_out)
{
	fake_radio_reset();
	fake_radio_unlock_at_poll(FS_UNLOCK_POLLS + 1);

	mu_assert_int_eq(0, cc2400_wait_fs_unlock());
	mu_assert_int_eq(FS_UNLOCK_POLLS, (int)status_reads);
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

/* ---- the lock wait ---- */

/* Already locked: success on the first read, strobe nothing. */
MU_TEST(test_lock_returns_immediately_when_locked)
{
	const u8 seq[] = { FS_LOCK };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(1, cc2400_wait_fs(1, NULL));
	mu_assert_int_eq(1, (int)status_reads);
	mu_assert_int_eq(0, (int)sfson_strobes);
}

/* Unlocked for a few polls, then locked: success, and no more polls than it
 * took. XOSC16M_STABLE on its own must not read as locked. */
MU_TEST(test_lock_returns_success_once_locked)
{
	const u8 seq[] = { 0x00, XOSC16M_STABLE, FS_LOCK | XOSC16M_STABLE };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 3);

	mu_assert_int_eq(1, cc2400_wait_fs(1, NULL));
	mu_assert_int_eq(3, (int)status_reads);
}

/* The regression this wait exists for: a synthesizer that never reports
 * locked must not keep led_specan() from looking at requested_mode again,
 * which is what stops ubertooth-util -S from working. */
MU_TEST(test_lock_gives_up_when_lock_never_arrives)
{
	const u8 seq[] = { 0x00 };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(0, cc2400_wait_fs(1, NULL));
	mu_assert_int_eq(FS_LOCK_POLLS, (int)status_reads);
}

/* While waiting it re-asserts SFSON, so a strobe lost to SPI bus contention
 * is retried. Only SFSON is ever strobed: the wait must not turn the
 * synthesizer off. */
MU_TEST(test_lock_restrobes_sfson_while_waiting)
{
	const u8 seq[] = { 0x00 };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(0, cc2400_wait_fs(1, NULL));
	mu_assert_int_eq(FS_LOCK_POLLS / FS_LOCK_RESTROBE_INTERVAL,
	                 (int)sfson_strobes);
	mu_assert_int_eq(0, (int)srfoff_strobes);
	mu_assert_int_eq(0, (int)other_strobes);
}

/* The case the re-strobe exists for: one corrective SFSON, then locked. */
MU_TEST(test_lock_succeeds_after_one_restrobe)
{
	fake_radio_reset();
	fake_radio_lock_at_poll(FS_LOCK_RESTROBE_INTERVAL + 8);

	mu_assert_int_eq(1, cc2400_wait_fs(1, NULL));
	mu_assert_int_eq(FS_LOCK_RESTROBE_INTERVAL + 8, (int)status_reads);
	mu_assert_int_eq(1, (int)sfson_strobes);
	mu_assert_int_eq(0, (int)other_strobes);
}

/* Boundary: locked on the very last permitted poll is success, one poll later
 * is a timeout, and neither takes an extra poll. */
MU_TEST(test_lock_on_last_poll_succeeds)
{
	fake_radio_reset();
	fake_radio_lock_at_poll(FS_LOCK_POLLS);

	mu_assert_int_eq(1, cc2400_wait_fs(1, NULL));
	mu_assert_int_eq(FS_LOCK_POLLS, (int)status_reads);
}

MU_TEST(test_lock_one_poll_too_late_times_out)
{
	fake_radio_reset();
	fake_radio_lock_at_poll(FS_LOCK_POLLS + 1);

	mu_assert_int_eq(0, cc2400_wait_fs(1, NULL));
	mu_assert_int_eq(FS_LOCK_POLLS, (int)status_reads);
}

/* A wait that succeeds before the first re-strobe interval must not strobe at
 * all -- the fast path, which is every sample led_specan() takes, stays as
 * cheap as it was. */
MU_TEST(test_lock_no_strobe_on_short_wait)
{
	const u8 seq[] = { 0x00, 0x00, FS_LOCK };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 3);

	mu_assert_int_eq(1, cc2400_wait_fs(1, NULL));
	mu_assert_int_eq(0, (int)sfson_strobes);
}

/* ---- the service callback ---- */

static unsigned service_calls;

static void count_service(void) { ++service_calls; }

/*
 * The point of the callback: led_specan() passes handle_usb(), and it has to
 * run between every pair of status polls. One call per status read that did
 * not finish the wait -- not per sample, which is what left endpoint 0 a whole
 * sample behind the host.
 */
MU_TEST(test_service_runs_between_every_poll)
{
	fake_radio_reset();
	service_calls = 0;
	fake_radio_lock_at_poll(5);

	mu_assert_int_eq(1, cc2400_wait_fs(1, count_service));
	mu_assert_int_eq(5, (int)status_reads);
	mu_assert_int_eq(4, (int)service_calls);
}

/* It runs on the unlock side too: both of led_specan()'s waits use it. */
MU_TEST(test_service_runs_on_the_unlock_side)
{
	fake_radio_reset();
	service_calls = 0;
	fake_radio_unlock_at_poll(3);

	mu_assert_int_eq(1, cc2400_wait_fs(0, count_service));
	mu_assert_int_eq(3, (int)status_reads);
	mu_assert_int_eq(2, (int)service_calls);
}

/* A wait satisfied by its first read does no work at all, service included. */
MU_TEST(test_service_not_run_when_wait_is_satisfied_at_once)
{
	const u8 seq[] = { FS_LOCK };

	fake_radio_reset();
	service_calls = 0;
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(1, cc2400_wait_fs(1, count_service));
	mu_assert_int_eq(0, (int)service_calls);
}

/* A wait that times out still serviced on every poll, so a mode loop stuck on
 * a dead radio keeps answering the host long enough to be stopped. */
MU_TEST(test_service_runs_on_every_poll_of_a_timeout)
{
	const u8 seq[] = { 0x00 };

	fake_radio_reset();
	service_calls = 0;
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(0, cc2400_wait_fs(1, count_service));
	mu_assert_int_eq(FS_LOCK_POLLS, (int)service_calls);
}

int main(void)
{
	printf("CC2400 bounded synthesizer wait tests\n");
	MU_RUN(test_returns_immediately_when_unlocked);
	MU_RUN(test_returns_success_once_unlocked);
	MU_RUN(test_gives_up_when_lock_never_clears);
	MU_RUN(test_restrobes_srfoff_while_waiting);
	MU_RUN(test_succeeds_after_one_restrobe);
	MU_RUN(test_unlock_on_last_poll_succeeds);
	MU_RUN(test_unlock_one_poll_too_late_times_out);
	MU_RUN(test_no_strobe_on_short_wait);
	MU_RUN(test_lock_returns_immediately_when_locked);
	MU_RUN(test_lock_returns_success_once_locked);
	MU_RUN(test_lock_gives_up_when_lock_never_arrives);
	MU_RUN(test_lock_restrobes_sfson_while_waiting);
	MU_RUN(test_lock_succeeds_after_one_restrobe);
	MU_RUN(test_lock_on_last_poll_succeeds);
	MU_RUN(test_lock_one_poll_too_late_times_out);
	MU_RUN(test_lock_no_strobe_on_short_wait);
	MU_RUN(test_service_runs_between_every_poll);
	MU_RUN(test_service_runs_on_the_unlock_side);
	MU_RUN(test_service_not_run_when_wait_is_satisfied_at_once);
	MU_RUN(test_service_runs_on_every_poll_of_a_timeout);
	return mu_report();
}
