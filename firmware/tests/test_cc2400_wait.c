/*
 * Unit tests for firmware/common/cc2400_wait.c
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
 *
 * The last cases scan the radio source of both firmware images as text, the
 * way test_frend.c does, to check that the code that drives the radio reaches
 * it through these waits and not through a bare spin again.
 */
#include "minunit.h"
#include "source_scan.h"
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

/* FSMSTATE reads: how many, and which poll number reports the state the wait
 * is after (0 = never). */
static unsigned fsm_reads;
static unsigned fsm_match_poll;
static u8 fsm_match_state;

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
	fsm_reads = 0;
	fsm_match_poll = 0;
	fsm_match_state = 0;
}

/* Report `state` on poll number `n` (1-based), and a different state before
 * it. n == 0 means never. */
static void fake_radio_fsm_state_at_poll(u8 state, unsigned n)
{
	fsm_match_state = state;
	fsm_match_poll = n;
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

/*
 * Only FSMSTATE is faked. Its high bits are breakpoint configuration, so a
 * value with bits above the low five set is returned on the matching poll:
 * the wait has to mask them off, as the firmware's own reads do.
 */
u16 cc2400_get(u8 reg)
{
	if (reg != FSMSTATE) {
		printf("  FAIL test bug: unexpected cc2400_get(0x%02x)\n", reg);
		return 0;
	}
	++fsm_reads;
	if (fsm_match_poll != 0 && fsm_reads == fsm_match_poll)
		return 0xe000 | fsm_match_state;
	/* Some other state. STATE_STROBE_FS_ON is 15, so 0 will do. */
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

/* While waiting it re-asserts SRFOFF, so a synthesizer an interrupt handler
 * switched back on is pushed off again. Only SRFOFF is ever strobed: the wait
 * must not turn the synthesizer on. */
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

/* While waiting it re-asserts SFSON, so a synthesizer an interrupt handler
 * switched off is turned back on. Only SFSON is ever strobed: the wait must
 * not turn the synthesizer off. */
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

/* ---- the crystal oscillator wait ---- */

/* Already stable: success on the first read. */
MU_TEST(test_xosc_returns_immediately_when_stable)
{
	const u8 seq[] = { XOSC16M_STABLE };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(1, cc2400_wait_xosc_stable(NULL));
	mu_assert_int_eq(1, (int)status_reads);
}

/* A few polls, then stable: success, and FS_LOCK on its own is not stable. */
MU_TEST(test_xosc_returns_success_once_stable)
{
	const u8 seq[] = { 0x00, FS_LOCK, FS_LOCK | XOSC16M_STABLE };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 3);

	mu_assert_int_eq(1, cc2400_wait_xosc_stable(NULL));
	mu_assert_int_eq(3, (int)status_reads);
}

/* The regression: this wait sits at the top of specan() and led_specan(),
 * before the loop that services USB has started, so a radio that never
 * reports its oscillator stable used to leave the board enumerated and
 * answering nothing at all. It has to give up. */
MU_TEST(test_xosc_gives_up_when_never_stable)
{
	const u8 seq[] = { 0x00 };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(0, cc2400_wait_xosc_stable(NULL));
	mu_assert_int_eq(XOSC_STABLE_POLLS, (int)status_reads);
}

/* It only polls: there is no strobe that starts the oscillator, so strobing
 * anything here would be driving the radio behind the caller's back. */
MU_TEST(test_xosc_strobes_nothing)
{
	const u8 seq[] = { 0x00 };

	fake_radio_reset();
	fake_radio_status_sequence(seq, 1);

	mu_assert_int_eq(0, cc2400_wait_xosc_stable(NULL));
	mu_assert_int_eq(0, (int)srfoff_strobes);
	mu_assert_int_eq(0, (int)sfson_strobes);
	mu_assert_int_eq(0, (int)other_strobes);
}

/* ---- the state machine wait ---- */

/* Already in the state: success on the first read, and nothing strobed --
 * this wait only watches. */
MU_TEST(test_fsm_returns_immediately_when_in_state)
{
	fake_radio_reset();
	fake_radio_fsm_state_at_poll(STATE_STROBE_FS_ON, 1);

	mu_assert_int_eq(1, cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL));
	mu_assert_int_eq(1, (int)fsm_reads);
	mu_assert_int_eq(0, (int)srfoff_strobes);
	mu_assert_int_eq(0, (int)sfson_strobes);
	mu_assert_int_eq(0, (int)other_strobes);
}

/* A few reads, then the state: success, and no more reads than it took. The
 * breakpoint bits the fake sets above the state must not stop it matching. */
MU_TEST(test_fsm_returns_success_once_in_state)
{
	fake_radio_reset();
	fake_radio_fsm_state_at_poll(STATE_STROBE_FS_ON, 4);

	mu_assert_int_eq(1, cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL));
	mu_assert_int_eq(4, (int)fsm_reads);
}

/* The regression: br_transmit() waits here twice per packet, so a radio whose
 * state machine stops advancing used to hold that loop for good and the board
 * answered no control request. It has to give up. */
MU_TEST(test_fsm_gives_up_when_state_never_arrives)
{
	fake_radio_reset();
	fake_radio_fsm_state_at_poll(STATE_STROBE_FS_ON, 0);

	mu_assert_int_eq(0, cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL));
	mu_assert_int_eq(FSM_POLLS, (int)fsm_reads);
}

/* Boundary: the state on the last permitted read is success, one read later is
 * a timeout, and neither takes an extra read. */
MU_TEST(test_fsm_on_last_poll_succeeds)
{
	fake_radio_reset();
	fake_radio_fsm_state_at_poll(STATE_STROBE_FS_ON, FSM_POLLS);

	mu_assert_int_eq(1, cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL));
	mu_assert_int_eq(FSM_POLLS, (int)fsm_reads);
}

MU_TEST(test_fsm_one_poll_too_late_times_out)
{
	fake_radio_reset();
	fake_radio_fsm_state_at_poll(STATE_STROBE_FS_ON, FSM_POLLS + 1);

	mu_assert_int_eq(0, cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL));
	mu_assert_int_eq(FSM_POLLS, (int)fsm_reads);
}

/*
 * The two states the BR/EDR image waits for, each from a slot task running in
 * the timer interrupt: idle before it retunes, and receive after it strobes
 * the radio to receive. Both have to give up, because a task that does not
 * return blocks the USB interrupt behind it and the board answers nothing.
 */
MU_TEST(test_fsm_gives_up_waiting_for_idle)
{
	fake_radio_reset();
	fake_radio_fsm_state_at_poll(STATE_IDLE, 0);

	mu_assert_int_eq(0, cc2400_wait_fsm(STATE_IDLE, NULL));
	mu_assert_int_eq(FSM_POLLS, (int)fsm_reads);
}

MU_TEST(test_fsm_gives_up_waiting_for_receive)
{
	fake_radio_reset();
	fake_radio_fsm_state_at_poll(STATE_STROBE_RX, 0);

	mu_assert_int_eq(0, cc2400_wait_fsm(STATE_STROBE_RX, NULL));
	mu_assert_int_eq(FSM_POLLS, (int)fsm_reads);
}

/* It waits for the state it was asked for and no other: a radio parked in
 * idle must not satisfy a wait for receive. */
MU_TEST(test_fsm_does_not_match_another_state)
{
	fake_radio_reset();
	fake_radio_fsm_state_at_poll(STATE_IDLE, 1);

	mu_assert_int_eq(0, cc2400_wait_fsm(STATE_STROBE_RX, NULL));
	mu_assert_int_eq(FSM_POLLS, (int)fsm_reads);
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

/* The oscillator wait takes one too: br_transmit() reaches it once per packet,
 * inside the loop that services USB. */
MU_TEST(test_service_runs_on_the_oscillator_wait)
{
	const u8 seq[] = { 0x00, 0x00, XOSC16M_STABLE };

	fake_radio_reset();
	service_calls = 0;
	fake_radio_status_sequence(seq, 3);

	mu_assert_int_eq(1, cc2400_wait_xosc_stable(count_service));
	mu_assert_int_eq(3, (int)status_reads);
	mu_assert_int_eq(2, (int)service_calls);
}

/* And the state machine wait, which is where br_transmit() waits out a
 * transmit. */
MU_TEST(test_service_runs_on_the_fsm_wait)
{
	fake_radio_reset();
	service_calls = 0;
	fake_radio_fsm_state_at_poll(STATE_STROBE_FS_ON, 6);

	mu_assert_int_eq(1, cc2400_wait_fsm(STATE_STROBE_FS_ON, count_service));
	mu_assert_int_eq(6, (int)fsm_reads);
	mu_assert_int_eq(5, (int)service_calls);
}

/* ---- the radio source ---- */

/*
 * Every file that drives the radio from a mode in the bluetooth_rxtx image, or
 * from a slot task in the BR/EDR image. rf.h is in the list because the BR/EDR
 * image waited through a macro defined there, and a macro puts the spin back
 * in all of its callers at once.
 *
 * The bench images (assembly_test, cc2400_test) are not here. They are
 * installed with a programmer, not shipped, and a stall at a step is what tells
 * whoever is running them that the radio did not answer, so their waits stay
 * as they are.
 */
static const char *const radio_sources[] = {
	FW_RXTX_DIR "/bluetooth_rxtx.c",
	FW_RXTX_DIR "/cc2400_rangetest.c",
	FW_RXTX_DIR "/ego.c",
	FW_RXTX_DIR "/le_phy.c",
	FW_BTBR_DIR "/include/ubtbr/rf.h",
	FW_BTBR_DIR "/src/btphy/rf.c",
	FW_BTBR_DIR "/src/btphy/rx_task.c",
	FW_BTBR_DIR "/src/btphy/scan_task.c",
	FW_BTBR_DIR "/src/btphy/tx_task.c",
};

/*
 * Is this line a busy wait with an empty body -- `while (<cond>);` -- on a
 * CC2400 register? That is the shape every one of these stalls had. A wait
 * with a real body is not matched: the waits for a packet keep one, because
 * waiting for a packet has no deadline, and the body is what services USB and
 * lets the host stop the mode.
 *
 * Any read of the radio counts: the status byte, a register read, or the
 * FS_TUNED() test the BR/EDR image spells its lock check with. tx_task.c has
 * one wait that reads FSMSTATE and is bounded by the slot clock rather than by
 * a poll count, so a condition that tests `CLKN_OFFSET <` is left alone --
 * that exact shape, not any mention of the slot clock, so a spin cannot get
 * past by naming it.
 *
 * Only that shape, written on one line. A reintroduced spin with its `;` on
 * the next line gets past this. The point is to catch the pattern coming back
 * by copy and paste, not to prove it cannot.
 */
static int is_bare_radio_spin(const char *line)
{
	const char *p = strstr(line, "while");
	size_t length;

	if (p == NULL || is_ident_char(p[5]))
		return 0;
	if (!contains_identifier(line, "cc2400_status") &&
	    !contains_identifier(line, "cc2400_get") &&
	    !contains_identifier(line, "FSMSTATE") &&
	    !contains_identifier(line, "FS_TUNED"))
		return 0;
	if (strstr(line, "CLKN_OFFSET <") != NULL)
		return 0;
	/* An empty body is a ';' as the last thing on the line. */
	length = strlen(line);
	while (length > 0 && isspace((unsigned char)line[length - 1]))
		length--;
	return length > 0 && line[length - 1] == ';';
}

/* Count the bare radio spins in one file, printing each. */
static int count_bare_radio_spins(const char *path)
{
	FILE *source = fopen(path, "r");
	char line[512];
	int in_block = 0, lineno = 0, found = 0;

	if (source == NULL) {
		printf("  FAIL cannot open %s\n", path);
		return -1;
	}
	while (fgets(line, sizeof(line), source) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		if (is_bare_radio_spin(line)) {
			printf("  FAIL %s:%d spins on the radio: %s",
			       path, lineno, line);
			found++;
		}
	}
	fclose(source);
	return found;
}

/*
 * The regression this whole file is about, checked against the source: a loop
 * that spins on the radio with nothing in the loop body never looks at
 * anything else again. In the bluetooth_rxtx image that loop is a mode loop,
 * which is also what services USB, so the board answers nothing. In the BR/EDR
 * image it is a slot task running from the timer interrupt, which blocks the
 * USB interrupt behind it, with the same result. Every such wait is now one of
 * the bounded waits above.
 */
MU_TEST(test_no_radio_source_spins_on_the_radio)
{
	size_t f;

	for (f = 0; f < sizeof(radio_sources) / sizeof(radio_sources[0]); ++f)
		mu_assert_int_eq(0, count_bare_radio_spins(radio_sources[f]));
}

/*
 * firmware/common/ubertooth.c is the exception, and a narrow one: the radio
 * waits in cc2400_reset() and clock_start() stay unbounded because they run
 * during boot, before USB exists, and the 16 MHz clock everything after them
 * needs comes from the radio -- so a bound would only move the hang a few
 * lines down. There are exactly three, and they are in those two functions.
 * A fourth anywhere, or one in a function that runs after boot, fails here.
 */
MU_TEST(test_only_the_boot_path_still_spins_on_the_radio)
{
	const char *path = FW_COMMON_DIR "/ubertooth.c";
	FILE *source = fopen(path, "r");
	char line[512], function[128] = "";
	int in_block = 0, lineno = 0, boot_spins = 0, elsewhere = 0;

	mu_assert(source != NULL, path);
	while (fgets(line, sizeof(line), source) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		/* A definition at file scope: a name in the first column. */
		if (!isspace((unsigned char)line[0]) && strchr(line, '(') != NULL) {
			const char *open = strchr(line, '(');
			const char *name = open;
			size_t length;

			while (name > line && is_ident_char(name[-1]))
				name--;
			length = (size_t)(open - name);
			if (length > 0 && length < sizeof(function)) {
				memcpy(function, name, length);
				function[length] = '\0';
			}
		}
		if (!is_bare_radio_spin(line))
			continue;
		if (strcmp(function, "cc2400_reset") == 0 ||
		    strcmp(function, "clock_start") == 0) {
			boot_spins++;
			continue;
		}
		printf("  FAIL %s:%d spins on the radio in %s(): %s",
		       path, lineno, function, line);
		elsewhere++;
	}
	fclose(source);

	mu_assert_int_eq(0, elsewhere);
	mu_assert_int_eq(3, boot_spins);
}

/*
 * The one converted site whose caller must not carry on. The BR/EDR scan
 * strobes the radio to receive, waits for the state machine to get there, and
 * then reads GIO6 as "a sync word arrived" and moves the slave clock to it. If
 * the wait gives up and the scan carries on, that read is not a measurement
 * and the scan reports a packet that never came -- worse than the hang the
 * bound replaces. So the result has to be checked, and the slot ended.
 *
 * Scanned rather than run: the surrounding code reads a GPIO and runs from the
 * timer interrupt.
 */
MU_TEST(test_the_scan_slot_ends_when_the_radio_is_not_receiving)
{
	const char *path = FW_BTBR_DIR "/src/btphy/scan_task.c";
	FILE *source = fopen(path, "r");
	char line[512];
	int in_block = 0, guarded = 0, unguarded = 0, ends_the_slot = 0;

	mu_assert(source != NULL, path);
	while (fgets(line, sizeof(line), source) != NULL) {
		strip_comments(line, &in_block);
		if (strstr(line, "cc2400_wait_fsm") == NULL) {
			/* The slot has to end before the GIO6 read in scan_wait(). */
			if (guarded && strstr(line, "scan_finalize()") != NULL)
				ends_the_slot = 1;
			continue;
		}
		if (strstr(line, "if (!cc2400_wait_fsm") != NULL)
			guarded = 1;
		else
			unguarded = 1;
	}
	fclose(source);

	mu_assert_int_eq(1, guarded);
	mu_assert_int_eq(0, unguarded);
	mu_assert_int_eq(1, ends_the_slot);
}

/*
 * hop() is where follow mode's control requests stalled, so it has to keep
 * taking a callback and bt_stream_rx() has to keep passing one. Without this
 * the bug comes back silently: hop(NULL) still compiles and still works,
 * it just stops answering the host while it retunes.
 */
MU_TEST(test_the_polled_stream_loop_services_usb_across_a_hop)
{
	FILE *source = fopen(FW_RXTX_DIR "/bluetooth_rxtx.c", "r");
	char line[512];
	int in_block = 0;
	int takes_callback = 0, passes_callback = 0;

	mu_assert(source != NULL, "bluetooth_rxtx.c");
	while (fgets(line, sizeof(line), source) != NULL) {
		strip_comments(line, &in_block);
		if (strstr(line, "void hop(void (*service)(void))") != NULL)
			takes_callback = 1;
		if (strstr(line, "hop(service_usb)") != NULL)
			passes_callback = 1;
	}
	fclose(source);

	mu_assert_int_eq(1, takes_callback);
	mu_assert_int_eq(1, passes_callback);
}

/*
 * The range test is the one mode that asks for idle itself when it finishes.
 * Now that it answers control requests while it runs, the host can have asked
 * for another mode in the meantime, and that request must survive: an
 * unconditional `requested_mode = MODE_IDLE` after the test would tell the host
 * its request succeeded and then drop it.
 *
 * Scanned rather than run, because the dispatch is inside main(). The check has
 * to come before the assignment, so the scan records which it sees first.
 */
MU_TEST(test_the_range_test_keeps_a_newer_mode_request)
{
	FILE *source = fopen(FW_RXTX_DIR "/bluetooth_rxtx.c", "r");
	char line[512];
	int in_block = 0, in_case = 0;
	int guarded = 0, unguarded = 0;

	mu_assert(source != NULL, "bluetooth_rxtx.c");
	while (fgets(line, sizeof(line), source) != NULL) {
		strip_comments(line, &in_block);
		if (strstr(line, "case MODE_RANGE_TEST:") != NULL) {
			in_case = 1;
			continue;
		}
		if (!in_case)
			continue;
		if (strstr(line, "requested_mode == MODE_RANGE_TEST") != NULL)
			guarded = 1;
		if (strstr(line, "requested_mode = MODE_IDLE") != NULL) {
			if (!guarded)
				unguarded = 1;
			in_case = 0;
		}
	}
	fclose(source);

	mu_assert_int_eq(1, guarded);
	mu_assert_int_eq(0, unguarded);
}

/*
 * The range test sends 128 packets and the repeater replies with 128, in nested
 * loops of 8 amplifier levels by 16 packets. The waits inside those loops now
 * answer control requests, so the host can stop the mode part-way through a
 * burst -- and then the rest of the burst must not go out. Every one of the four
 * loops carries the mode in its condition; this scan fails if one loses it.
 */
MU_TEST(test_the_transmit_bursts_stop_when_the_mode_does)
{
	FILE *source = fopen(FW_RXTX_DIR "/cc2400_rangetest.c", "r");
	char line[512];
	int in_block = 0, lineno = 0, bursts = 0, unguarded = 0;

	mu_assert(source != NULL, "cc2400_rangetest.c");
	while (fgets(line, sizeof(line), source) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		if (strstr(line, "for (") == NULL)
			continue;
		if (strstr(line, "pa < 8") == NULL && strstr(line, "i < 16") == NULL)
			continue;
		bursts++;
		if (strstr(line, "requested_mode ==") == NULL) {
			printf("  FAIL cc2400_rangetest.c:%d burst loop does not check "
			       "the mode: %s", lineno, line);
			unguarded++;
		}
	}
	fclose(source);

	mu_assert_int_eq(4, bursts);
	mu_assert_int_eq(0, unguarded);
}

int main(void)
{
	printf("CC2400 bounded radio wait tests\n");
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
	MU_RUN(test_xosc_returns_immediately_when_stable);
	MU_RUN(test_xosc_returns_success_once_stable);
	MU_RUN(test_xosc_gives_up_when_never_stable);
	MU_RUN(test_xosc_strobes_nothing);
	MU_RUN(test_fsm_returns_immediately_when_in_state);
	MU_RUN(test_fsm_returns_success_once_in_state);
	MU_RUN(test_fsm_gives_up_when_state_never_arrives);
	MU_RUN(test_fsm_on_last_poll_succeeds);
	MU_RUN(test_fsm_one_poll_too_late_times_out);
	MU_RUN(test_fsm_gives_up_waiting_for_idle);
	MU_RUN(test_fsm_gives_up_waiting_for_receive);
	MU_RUN(test_fsm_does_not_match_another_state);
	MU_RUN(test_service_runs_between_every_poll);
	MU_RUN(test_service_runs_on_the_unlock_side);
	MU_RUN(test_service_not_run_when_wait_is_satisfied_at_once);
	MU_RUN(test_service_runs_on_every_poll_of_a_timeout);
	MU_RUN(test_service_runs_on_the_oscillator_wait);
	MU_RUN(test_service_runs_on_the_fsm_wait);
	MU_RUN(test_no_radio_source_spins_on_the_radio);
	MU_RUN(test_only_the_boot_path_still_spins_on_the_radio);
	MU_RUN(test_the_scan_slot_ends_when_the_radio_is_not_receiving);
	MU_RUN(test_the_polled_stream_loop_services_usb_across_a_hop);
	MU_RUN(test_the_range_test_keeps_a_newer_mode_request);
	MU_RUN(test_the_transmit_bursts_stop_when_the_mode_does);
	return mu_report();
}
