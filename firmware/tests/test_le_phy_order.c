/*
 * Guard for the order in which firmware/bluetooth_rxtx/le_phy.c brings the LE
 * PHY up and takes it down, around the le_phy_active flag.
 *
 * le_phy_active is what le_DMA_IRQHandler() and TIMER1_IRQHandler() read to
 * decide whether the state they drive -- the buffer pool, current_rxbuf, the
 * DMA channel, the SSP and the radio -- belongs to them. The order of the two
 * writes to it is the whole point:
 *
 *   coming up:   take the first buffer and set the radio up, then
 *                le_phy_active = 1, then unmask the timer1 interrupt
 *   going down:  le_phy_active = 0 first, then mask and stop what feeds the
 *                interrupts, then quiet the radio
 *
 * Setting the flag before the setup left a window where a timer1 match left
 * armed by an earlier run was taken with current_rxbuf still the NULL the
 * teardown leaves behind. Clearing it after the teardown had started was
 * worse: a write to NVIC ICER0 is not self-synchronising on Cortex-M3, so an
 * already-pending interrupt is still taken after the mask, and it strobed the
 * radio and read RSSI over the bit-banged SPI bus while the main loop used it
 * -- which wedged the board until it was re-plugged.
 *
 * Coming up also starts from nothing pending: a match flag in T1IR or a
 * pending bit in the NVIC, left by the run before, would otherwise be the
 * first interrupt of this run and be taken as a live event.
 *
 * Neither path can run here: it is hardware code. So this test scans the
 * source, the way test_mode_dispatch.c scans the mode dispatch. Being a scan,
 * it pins the shape and not only the invariant: it looks for the calls by
 * name, so writing `ISER0 = ISER0_ISE_TIMER1;` inline at the right place
 * would still fail it. Keeping the two steps as named functions is part of
 * what is being checked.
 */
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

/* How many places unmask timer1's interrupt. One, so that "the flag is set
 * before the interrupt is unmasked" is a statement about the whole file. */
#define TIMER1_UNMASKS 1

/* Line numbers of what the scan looks for, 0 for not found. */
struct le_phy_scan {
	/* in le_phy_main(), coming up */
	int timer1_start;      /* timer1_start() */
	int buffer_get;        /* current_rxbuf = buffer_get() */
	int init_rf;           /* le_cc2400_init_rf() */
	int active_set;        /* le_phy_active = 1 */
	int unmask;            /* timer1_enable_interrupt() */
	/* in le_phy_main(), going down */
	int active_cleared;    /* le_phy_active = 0 */
	int mask_dma;          /* ICER0 = ICER0_ICE_DMA */
	int timer1_stop;       /* timer1_stop() */
	/* in timer1_start(), which runs before the radio is set up */
	int clear_matches;     /* T1MCR = 0 */
	int clear_flags;       /* T1IR = 0xff */
	int clear_pending;     /* ICPR0 = ICPR0_ICP_TIMER1 */
	int counter_enable;    /* T1TCR = TCR_Counter_Enable */
	/* anywhere in the file */
	int unmask_writes;     /* writes of ISER0_ISE_TIMER1 to ISER0 */
	int main_lines;        /* lines of le_phy_main() the scan read */
	int dropped_cleared;   /* le_packet_dropped = 0 */
};

static int scan_le_phy(const char *path, struct le_phy_scan *s)
{
	char line[512];
	int lineno = 0, in_block = 0, in_main = 0, in_timer1_start = 0;
	char name[64];
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);

		/* The one place the interrupt is unmasked, wherever it is. */
		if (is_assignment_to(line, "ISER0") &&
		    contains_identifier(line, "ISER0_ISE_TIMER1"))
			s->unmask_writes++;

		if (in_timer1_start) {
			if (is_assignment_of(line, "T1MCR", "0") &&
			    s->clear_matches == 0)
				s->clear_matches = lineno;
			if (is_assignment_of(line, "T1IR", "0xff") &&
			    s->clear_flags == 0)
				s->clear_flags = lineno;
			if (is_assignment_of(line, "ICPR0", "ICPR0_ICP_TIMER1") &&
			    s->clear_pending == 0)
				s->clear_pending = lineno;
			if (is_assignment_of(line, "T1TCR", "TCR_Counter_Enable") &&
			    s->counter_enable == 0)
				s->counter_enable = lineno;
			if (line[0] == '}')
				in_timer1_start = 0;
			continue;
		}

		if (!in_main) {
			if (strstr(line, "void timer1_start(") != NULL &&
			    strchr(line, ';') == NULL)
				in_timer1_start = 1;
			if (strstr(line, "void le_phy_main(") != NULL &&
			    strchr(line, ';') == NULL)
				in_main = 1;
			continue;
		}

		s->main_lines++;

		if (call_name(line, name, sizeof(name))) {
			if (strcmp(name, "timer1_start") == 0 && s->timer1_start == 0)
				s->timer1_start = lineno;
			if (strcmp(name, "buffer_get") == 0 && s->buffer_get == 0)
				s->buffer_get = lineno;
			if (strcmp(name, "le_cc2400_init_rf") == 0 && s->init_rf == 0)
				s->init_rf = lineno;
			if (strcmp(name, "timer1_enable_interrupt") == 0 &&
			    s->unmask == 0)
				s->unmask = lineno;
			if (strcmp(name, "timer1_stop") == 0 && s->timer1_stop == 0)
				s->timer1_stop = lineno;
		}
		if (is_assignment_of(line, "le_packet_dropped", "0") &&
		    s->dropped_cleared == 0)
			s->dropped_cleared = lineno;
		if (is_assignment_of(line, "le_phy_active", "1") && s->active_set == 0)
			s->active_set = lineno;
		if (is_assignment_of(line, "le_phy_active", "0") &&
		    s->active_cleared == 0)
			s->active_cleared = lineno;
		if (is_assignment_to(line, "ICER0") &&
		    contains_identifier(line, "ICER0_ICE_DMA") && s->mask_dma == 0)
			s->mask_dma = lineno;

		/* a closing brace in column 1 ends the function */
		if (line[0] == '}')
			in_main = 0;
	}

	fclose(f);
	return 1;
}

static struct le_phy_scan s;
static int scanned;
static const char *path = FW_RXTX_DIR "/le_phy.c";

/*
 * The scan runs once, in main(), and every test below reads its result. It
 * does not run from a test, so no test depends on another having run first.
 */
MU_TEST(test_source_was_scanned)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
	mu_assert(scanned, msg);

	/* Without these, a scan that matched nothing would pass every check
	 * below for want of anything to compare. */
	snprintf(msg, sizeof(msg),
	         "%s: le_phy_main() scanned as %d lines, so the scan lost track "
	         "of the function", path, s.main_lines);
	mu_assert(s.main_lines > 50, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find all of le_phy_main()'s setup: "
	         "timer1_start %d, buffer_get %d, le_cc2400_init_rf %d, "
	         "le_phy_active = 1 at %d, timer1_enable_interrupt %d",
	         path, s.timer1_start, s.buffer_get, s.init_rf, s.active_set,
	         s.unmask);
	mu_assert(s.timer1_start > 0 && s.buffer_get > 0 && s.init_rf > 0 &&
	          s.active_set > 0 && s.unmask > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find le_phy_main()'s teardown: "
	         "le_phy_active = 0 at %d, ICER0_ICE_DMA %d, timer1_stop %d",
	         path, s.active_cleared, s.mask_dma, s.timer1_stop);
	mu_assert(s.active_cleared > 0 && s.mask_dma > 0 && s.timer1_stop > 0,
	          msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not read timer1_start(): T1MCR = 0 at %d, "
	         "T1IR = 0xff at %d, ICPR0_ICP_TIMER1 at %d, counter enabled "
	         "at %d", path, s.clear_matches, s.clear_flags, s.clear_pending,
	         s.counter_enable);
	mu_assert(s.clear_matches > 0 && s.clear_flags > 0 &&
	          s.clear_pending > 0 && s.counter_enable > 0, msg);
}

/*
 * A capture starts with no dropped packet to report. The engine marks the next
 * packet that gets through after it had to drop one, so a mark left over from
 * the run before would tell the host that this capture's first packet follows
 * a gap -- a gap that belongs to someone else's capture. Scanned rather than
 * run, because le_phy_main() drives the radio and cannot run here;
 * test_le_phy_logic.c runs what the mark itself does.
 */
MU_TEST(test_a_run_starts_with_no_dropped_packet_to_report)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: le_phy_main() does not clear le_packet_dropped, so a drop "
	         "from the run before marks this capture's first packet", path);
	mu_assert_fatal(s.dropped_cleared > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: le_packet_dropped is cleared at line %d, after the "
	         "engine is live (le_phy_active = 1 at line %d), so a packet "
	         "received in between can still carry the old mark",
	         path, s.dropped_cleared, s.dropped_cleared, s.active_set);
	mu_assert(s.dropped_cleared < s.active_set, msg);
}

/*
 * The run starts with nothing left pending from the last one, and the clearing
 * happens before the counter runs -- which is before le_cc2400_init_rf() arms
 * the match this run does want, so it cannot be cleared away.
 */
MU_TEST(test_the_run_starts_with_nothing_pending)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: timer1_start() does not clear all three: armed matches "
	         "(T1MCR) at %d, match flags (T1IR) at %d, NVIC pending bit at "
	         "%d", path, s.clear_matches, s.clear_flags, s.clear_pending);
	mu_assert(s.clear_matches > 0 && s.clear_flags > 0 &&
	          s.clear_pending > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: timer1_start() enables the counter before it clears the "
	         "armed matches (T1MCR, line %d), the match flags (T1IR, line "
	         "%d) and the NVIC pending bit (line %d), so a match can latch "
	         "again in between", path, s.counter_enable, s.clear_matches,
	         s.clear_flags, s.clear_pending);
	mu_assert(s.counter_enable > s.clear_matches &&
	          s.counter_enable > s.clear_flags &&
	          s.counter_enable > s.clear_pending, msg);
}

/*
 * The flag says the PHY is active only once the state the handlers read is
 * there: the first receive buffer and the radio.
 */
MU_TEST(test_active_is_set_after_the_setup)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: `le_phy_active = 1` comes before the first "
	         "buffer_get() at line %d, so an interrupt taken in between sees "
	         "the mode active with current_rxbuf still NULL from the last run",
	         path, s.active_set, s.buffer_get);
	mu_assert(s.active_set > s.buffer_get, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: `le_phy_active = 1` comes before le_cc2400_init_rf() at "
	         "line %d, so an interrupt taken in between acts on a radio that "
	         "is not set up yet", path, s.active_set, s.init_rf);
	mu_assert(s.active_set > s.init_rf, msg);
}

/*
 * ...and the interrupt that reads it is unmasked only after it is set. The
 * counter itself has to start earlier: le_cc2400_init_rf() arms its FS_LOCK
 * match against NOW.
 */
MU_TEST(test_the_interrupt_is_unmasked_last)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: timer1's interrupt is unmasked at or before "
	         "`le_phy_active = 1` (line %d), so a match left pending by an "
	         "earlier run can be taken before the flag is set",
	         path, s.unmask, s.active_set);
	mu_assert(s.unmask > s.active_set, msg);

	snprintf(msg, sizeof(msg),
	         "%s: ISER0_ISE_TIMER1 is written in %d places; "
	         "timer1_enable_interrupt() is meant to be the only one, or the "
	         "check above says nothing about the others",
	         path, s.unmask_writes);
	mu_assert(s.unmask_writes == TIMER1_UNMASKS, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: timer1_start() comes after le_cc2400_init_rf() at line "
	         "%d, which arms a match against NOW and needs the counter "
	         "running", path, s.timer1_start, s.init_rf);
	mu_assert(s.timer1_start < s.init_rf, msg);
}

/*
 * Going down, the flag is cleared first: masking an interrupt does not stop
 * one that is already pending, and the flag is what neuters it.
 */
MU_TEST(test_active_is_cleared_before_the_teardown)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: `le_phy_active = 0` comes after the DMA interrupt is "
	         "masked at line %d; an already-pending interrupt is still taken "
	         "after the mask, and the flag is what makes it do nothing",
	         path, s.active_cleared, s.mask_dma);
	mu_assert(s.active_cleared < s.mask_dma, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: `le_phy_active = 0` comes after timer1_stop() at line "
	         "%d, so a match taken while the timer is being stopped still "
	         "runs the state machine", path, s.active_cleared, s.timer1_stop);
	mu_assert(s.active_cleared < s.timer1_stop, msg);
}

int main(void)
{
	printf("le_phy bring-up and teardown order tests\n");
	scanned = scan_le_phy(path, &s);
	MU_RUN(test_source_was_scanned);
	MU_RUN(test_a_run_starts_with_no_dropped_packet_to_report);
	MU_RUN(test_the_run_starts_with_nothing_pending);
	MU_RUN(test_active_is_set_after_the_setup);
	MU_RUN(test_the_interrupt_is_unmasked_last);
	MU_RUN(test_active_is_cleared_before_the_teardown);
	return mu_report();
}
