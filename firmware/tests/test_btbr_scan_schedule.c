/*
 * Unit tests for what the BR/EDR image's scan task does when the scheduler
 * cannot take its next listening slot: firmware/btbr/src/btphy/scan_task.c.
 *
 * `page_scan`, `inquiry_scan` and `monitor` all listen through this one task. It puts one slot task on the scheduler at a time, and
 * that task is the only thing that ever reaches the caller's callback. The
 * scheduler takes four tasks per slot and refuses the fifth, so an arm can be
 * refused -- and a caller that was not told would sit in its scan state with
 * no task left to run, listening to nothing and never saying why.
 *
 * Two arms can be refused. The one that starts a window is reported back to
 * the caller, which ends its session; the one that waits another slot inside a
 * window ends the window with no packet, which is what the caller already does
 * something with.
 *
 * scan_task.c is included into this translation unit, which is how its slot
 * tasks can be reached at all: they are static. The scheduler underneath is
 * the firmware's own -- tdma_sched.c is included as well -- so a full slot
 * here is a real full slot. Everything else is faked: the radio, the slot
 * clock, the packet strobe and the hop sequence.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"

/* The two diagnostics every btbr source can reach. They are declared by
 * stubs-btbr/ubtbr/debug.h, which the headers below pull in, and defined among
 * the fakes. */
void cprintf(char *fmt, ...);
void __attribute__((noreturn)) die(char *fmt, ...);

#include <ubtbr/btphy.h>
#include <ubtbr/hop.h>
#include <ubtbr/rf.h>
#include <ubtbr/scan_task.h>
#include <ubtbr/tdma_sched.h>

/* ---- the fake slot clock ---- */

/*
 * The slot clock counts 100ns steps, as TIMER0 does, and advances on every
 * read, so a wait that polls it makes progress.
 */
static uint32_t fake_clock;
uint32_t fake_clock_match = CLKN_OFFSET_RESET_VAL - 1;

uint32_t fake_clock_read(void)
{
	uint32_t now = fake_clock;

	fake_clock += 10;
	return now;
}

/* The CC2400's packet strobe, which the scan reads as "a sync word arrived":
 * it is active low, so 1 is a slot that heard nothing. */
static int fake_sync_word;

int fake_gio6(void)
{
	return fake_sync_word ? 0 : 1;
}

/* ---- the rest of the scan task's world ---- */

static char console[512];
static unsigned console_len;

void cprintf(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	console_len += vsnprintf(console + console_len,
	                         sizeof(console) - console_len, fmt, args);
	va_end(args);
	/* vsnprintf() returns the length it would have written, so a line that
	 * filled the buffer would otherwise leave console_len past the end and
	 * the next call's remaining size negative. */
	if (console_len > sizeof(console) - 1)
		console_len = sizeof(console) - 1;
}

void __attribute__((noreturn)) die(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	abort();
}

btphy_t btphy;
volatile rf_state_t rf_state;
hop_state_t hop_state;

/* The radio. The strobes are counted so a test can see whether it was touched
 * at all, and whether the receiver was ever turned on. */
static unsigned fake_srx_strobes;
static unsigned fake_sfson_strobes;
static unsigned fake_srfoff_strobes;

u8 cc2400_strobe(u8 reg)
{
	switch (reg) {
	case SRX:
		fake_srx_strobes++;
		break;
	case SFSON:
		fake_sfson_strobes++;
		break;
	case SRFOFF:
		fake_srfoff_strobes++;
		break;
	default:
		break;
	}
	return 0;
}

u8 cc2400_status(void) { return 0; }

void cc2400_set(u8 reg, u16 val) { (void)reg; (void)val; }

/*
 * The radio's state machine. A scan waits for the receiver to be on, which is
 * the state the fake is in, so such a wait returns on its first read.
 */
static u16 fake_fsm_state = STATE_STROBE_RX;

u16 cc2400_get(u8 reg)
{
	if (reg == FSMSTATE)
		return fake_fsm_state;
	return 0;
}

uint8_t watchdog_mark_wait(uint8_t site)
{
	(void)site;
	return WATCHDOG_SITE_NONE;
}

void watchdog_leave_wait(uint8_t site) { (void)site; }

void cc2400_wait_count_timeout(enum cc2400_wait_shape shape) { (void)shape; }

static unsigned fake_tunes;

void btphy_rf_tune_chan(uint16_t channel, int tx)
{
	(void)channel;
	(void)tx;
	fake_tunes++;
}

uint8_t hop_channel(uint32_t clkn) { (void)clkn; return 0; }

void btphy_adj_clkn_delay(int delay) { (void)delay; }

#include "../btbr/src/btphy/scan_task.c"

/*
 * The scheduler underneath, so a full slot is a real full slot. It marks a
 * parameter it does not read with `__unused`, which the ARM toolchain's
 * headers supply and a host's C library may not, so it is spelled out here --
 * after the host headers above, which on some platforms use the same name for
 * a structure member. Included rather than linked for that reason alone.
 */
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#include "../btbr/src/btphy/tdma_sched.c"

/* ---- driving whole slots ---- */

/*
 * One TDMA frame: run what the scheduler holds for this frame, then move on to
 * the next, as btphy_handler() does on each timer interrupt. The slot clock
 * starts each frame at zero, where the hardware restarts it.
 */
static void a_slot(void)
{
	fake_clock = 0;
	tdma_sched_execute();
	tdma_sched_advance();
}

static void some_slots(int slots)
{
	int i;

	for (i = 0; i < slots; i++)
		a_slot();
}

/* What the caller is told. */
static unsigned reports;
static int last_sw_detected;
static void *last_report_arg;
/* A caller that asks for another window from its own report, as every scan
 * state machine does. -1 means it has not asked yet. */
static int rearm_from_report;
static int rearm_result = -1;

static void count_report(int sw_detected, void *arg)
{
	reports++;
	last_sw_detected = sw_detected;
	last_report_arg = arg;

	if (rearm_from_report)
		rearm_result = scan_task_schedule(count_report, arg);
}

/* Something harmless to fill a slot with, so a slot can be made full the way
 * the firmware fills one: through the scheduler. */
static unsigned fillers_run;

static int a_filler(uint8_t p1, uint8_t p2, uint16_t p3)
{
	(void)p1;
	(void)p2;
	(void)p3;
	fillers_run++;
	return 0;
}

/* Put `items` tasks in the slot `frame_offset` frames ahead of the one the
 * scheduler is standing in. Every one is accepted: a slot holds
 * TDMASCHED_NUM_CB of them. */
static void fill_a_slot(uint8_t frame_offset, int items)
{
	int i;

	for (i = 0; i < items; i++) {
		if (tdma_schedule(frame_offset, a_filler, 0, 0, 0, 10) != 0)
			die("fill_a_slot: the slot was already full\n");
	}
}

static void a_board(void)
{
	int i;

	memset(&btphy, 0, sizeof(btphy));
	memset(&scan_task, 0, sizeof(scan_task));

	/* Start from an empty scheduler. tdma_sched_reset() leaves the slot it
	 * is standing in alone, so the slots are run out once to clear that
	 * one as well; the counters below are zeroed afterwards. */
	tdma_sched_reset();
	for (i = 0; i < TDMASCHED_NUM_FRAMES; i++)
		a_slot();

	fake_clock = 0;
	fake_sync_word = 0;
	fake_fsm_state = STATE_STROBE_RX;
	fake_srx_strobes = 0;
	fake_sfson_strobes = 0;
	fake_srfoff_strobes = 0;
	fake_tunes = 0;
	fillers_run = 0;
	console[0] = '\0';
	console_len = 0;
	reports = 0;
	last_sw_detected = -1;
	last_report_arg = NULL;
	rearm_from_report = 0;
	rearm_result = -1;
}

/* ---- the cases ---- */

/* The ordinary scan: the arm is accepted, the window listens, and the silence
 * is reported once. SCAN_MAX_SLOTS slots of it, each armed from the last. */
MU_TEST(an_accepted_arm_listens_and_reports_once)
{
	int arg = 0;

	a_board();

	mu_assert_int_eq(0, scan_task_schedule(count_report, &arg));
	some_slots(2 + SCAN_MAX_SLOTS);

	mu_assert_int_eq(1, reports);
	mu_assert_int_eq(0, last_sw_detected);
	mu_assert(last_report_arg == &arg, "the callback got another caller's argument");
	/* The window tunes and turns the receiver on once and then listens
	 * across its slots. */
	mu_assert_int_eq(1, (int)fake_srx_strobes);
	mu_assert_int_eq(1, (int)fake_tunes);
	mu_assert(console[0] == '\0', "the console said something about a scan that was fine");
}

/* A window that hears a page reports that instead, which is the path the fix
 * must leave alone. */
MU_TEST(a_window_that_hears_a_page_reports_it)
{
	a_board();
	fake_sync_word = 1;

	mu_assert_int_eq(0, scan_task_schedule(count_report, NULL));
	some_slots(2);

	mu_assert_int_eq(1, reports);
	mu_assert_int_eq(1, last_sw_detected);
	mu_assert(console[0] == '\0', "the console said something about a scan that was fine");
}

/* The slot the window would be armed in is full, so there is no window. The
 * caller is told so it can end its session. */
MU_TEST(a_refused_arm_is_reported_to_the_caller)
{
	a_board();
	fill_a_slot(1, TDMASCHED_NUM_CB);

	mu_assert_int_eq(-1, scan_task_schedule(count_report, NULL));
	mu_assert(strstr(console, "cannot schedule scan") != NULL,
		"the refused arm was not said on the console");
}

/* ... and nothing is left believing a window is armed: no slot task of the
 * scan ever runs, so nothing reaches the callback. */
MU_TEST(a_refused_arm_leaves_nothing_armed)
{
	a_board();
	fill_a_slot(1, TDMASCHED_NUM_CB);

	mu_assert_int_eq(-1, scan_task_schedule(count_report, NULL));
	some_slots(2 * TDMASCHED_NUM_FRAMES);

	mu_assert_int_eq(0, reports);
	/* The four tasks that filled the slot did run, so the slot was full of
	 * real work rather than merely marked full. */
	mu_assert_int_eq(TDMASCHED_NUM_CB, (int)fillers_run);
}

/* A refused arm never reaches the radio: scan_start() is what tunes it and
 * turns the receiver on, and scan_start() did not run. */
MU_TEST(a_refused_arm_leaves_the_radio_alone)
{
	a_board();
	fill_a_slot(1, TDMASCHED_NUM_CB);

	mu_assert_int_eq(-1, scan_task_schedule(count_report, NULL));
	some_slots(2 * TDMASCHED_NUM_FRAMES);

	mu_assert_int_eq(0, (int)fake_tunes);
	mu_assert_int_eq(0, (int)fake_srx_strobes);
	mu_assert_int_eq(0, (int)fake_sfson_strobes);
}

/* A window that cannot wait another slot ends there. It reports the silence,
 * which is what the window reports when it hears nothing, rather than leaving
 * the scan with no task left to run. */
MU_TEST(a_refused_extra_slot_ends_the_window)
{
	a_board();
	/* The slot the first listening slot would ask for. */
	fill_a_slot(2, TDMASCHED_NUM_CB);

	mu_assert_int_eq(0, scan_task_schedule(count_report, NULL));
	some_slots(2 * TDMASCHED_NUM_FRAMES);

	mu_assert_int_eq(1, reports);
	mu_assert_int_eq(0, last_sw_detected);
	mu_assert(strstr(console, "cannot wait another slot") != NULL,
		"the refused extra slot was not said on the console");
	/* The radio was stopped, as it is at the end of any window. */
	mu_assert_int_eq(1, (int)fake_srfoff_strobes);
}

/*
 * A caller that asks for the next window from its own report -- which every
 * scan state machine does -- is told about that arm too, in one step: the
 * report comes from inside the slot whose successor is full, so the new arm
 * asks for the same full slot and is refused. The session ends there instead
 * of the task calling itself.
 */
MU_TEST(a_caller_that_rearms_from_the_report_is_told_too)
{
	a_board();
	fill_a_slot(2, TDMASCHED_NUM_CB);
	rearm_from_report = 1;

	mu_assert_int_eq(0, scan_task_schedule(count_report, NULL));
	some_slots(2 * TDMASCHED_NUM_FRAMES);

	mu_assert_int_eq(1, reports);
	mu_assert_int_eq(-1, rearm_result);
}

/* A slot that was full is only full until it has run, so a scan the scheduler
 * refuses once is not a board that can never scan again. */
MU_TEST(an_arm_is_accepted_once_the_slot_has_run)
{
	a_board();
	fill_a_slot(1, TDMASCHED_NUM_CB);

	mu_assert_int_eq(-1, scan_task_schedule(count_report, NULL));
	/* Run the full slot out, which empties it. */
	some_slots(2);
	mu_assert_int_eq(TDMASCHED_NUM_CB, (int)fillers_run);

	mu_assert_int_eq(0, scan_task_schedule(count_report, NULL));
	some_slots(2 + SCAN_MAX_SLOTS);
	mu_assert_int_eq(1, reports);
}

int main(void)
{
	printf("BR/EDR scan tests: no slot for the next listening window\n");
	MU_RUN(an_accepted_arm_listens_and_reports_once);
	MU_RUN(a_window_that_hears_a_page_reports_it);
	MU_RUN(a_refused_arm_is_reported_to_the_caller);
	MU_RUN(a_refused_arm_leaves_nothing_armed);
	MU_RUN(a_refused_arm_leaves_the_radio_alone);
	MU_RUN(a_refused_extra_slot_ends_the_window);
	MU_RUN(a_caller_that_rearms_from_the_report_is_told_too);
	MU_RUN(an_arm_is_accepted_once_the_slot_has_run);
	return mu_report();
}
