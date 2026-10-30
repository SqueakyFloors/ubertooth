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
	/* Reaching the match-enable register for one bit rather than the whole
	 * of it: where it is done outside the guarded helpers, and how many
	 * times inside them. */
	int bare_match_use;
	int guarded_match_uses;
	/* Writing the moment a match fires at anywhere but inside the helper
	 * that arms it in the same breath. */
	int bare_moment_write;
	/* Handing a match a moment worked out from the counter, from outside the
	 * guard that arms it. */
	int bare_moment_from_counter;
	/* The LED blink, which is on timer2: any mention of timer1's match 1,
	 * which the blink used to arm, and any unmasking of timer2's own
	 * interrupt. */
	int timer1_blink_match;
	int timer2_unmask;
	/* In le_phy_main(): where the blink's counter is started and stopped,
	 * and where a finished blink is turned off. The last is counted only
	 * inside the mode's own loop: called after the loop instead, it would
	 * leave the LED latched on from the first packet to the end of the
	 * mode. */
	int blink_timer_start;
	int blink_timer_stop;
	int blink_expire_in_loop;
	/* In le_dma_interrupt(), where a finished packet is handed over: the
	 * first of each of these after the function starts. */
	int handover_take;      /* buffer_get() */
	int handover_mask;      /* irq_save_disable() */
	int handover_queue;     /* queue_insert() */
	int handover_restore;   /* irq_restore() */
	/* Also in le_dma_interrupt(): reaching the connection event's own state,
	 * and dropping the match the event is timed by, which the timer
	 * interrupt owns the other half of. */
	int bare_event_use;     /* conn_event outside a guard */
	int guarded_event_uses; /* conn_event inside one */
	int bare_deadline_clear;/* timer1_clear_match() */
	/* Reaching the SSP's receive request for one bit: le_phy.c goes through
	 * the guarded helper in ubertooth_dma.c instead. */
	int bare_rx_request_use;
	/* Where the two handlers that run in the mode's loop publish a change
	 * for the timer interrupt to apply: the flag down, the fields, the flag
	 * up. */
	int map_cleared, map_instant, map_parsed, map_published;
	int update_cleared, update_instant, update_published;
};

static int scan_le_phy(const char *path, struct le_phy_scan *s)
{
	char line[512];
	int lineno = 0, in_block = 0, in_main = 0, in_timer1_start = 0;
	int in_mask_helper = 0, in_dma = 0, in_grid_caller = 0, in_loop = 0;
	int in_map_handler = 0, in_update_handler = 0, dma_guard_depth = 0;
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

		/* The LED blink's two marks, anywhere in the file: timer1's match 1,
		 * which the blink used to arm, and timer2's interrupt, which it
		 * must not start using instead. */
		if (s->timer1_blink_match == 0 &&
		    (contains_identifier(line, "T1MR1") ||
		     contains_identifier(line, "TMCR_MR1I") ||
		     contains_identifier(line, "TIR_MR1_Interrupt")))
			s->timer1_blink_match = lineno;

		if (contains_identifier(line, "ISER0_ISE_TIMER2"))
			s->timer2_unmask = lineno;

		/* Every mention of T1MCR in the file, and whether it is inside one
		 * of the helpers that hold interrupts off. A write of the whole
		 * register is not counted: it cannot be built on a stale read.
		 * This runs before the gates below, so it covers the whole file
		 * rather than one function. */
		if (strstr(line, "timer1_arm_match(") != NULL ||
		    strstr(line, "timer1_arm_match_in(") != NULL ||
		    strstr(line, "timer1_disarm_match_interrupt(") != NULL ||
		    strstr(line, "timer1_set_match_in_if_unarmed(") != NULL) {
			if (strchr(line, ';') == NULL && strchr(line, '{') != NULL)
				in_mask_helper = 1;
		} else if (in_mask_helper && line[0] == '}') {
			in_mask_helper = 0;
		}

		if (contains_identifier(line, "T1MCR") &&
		    !is_assignment_of(line, "T1MCR", "0")) {
			if (in_mask_helper)
				s->guarded_match_uses++;
			else if (s->bare_match_use == 0)
				s->bare_match_use = lineno;
		}

		/* The moment itself, in whichever of the four registers. Written
		 * outside the helper it is a separate write from the one that arms
		 * it, which is the hazard above in the other order. */
		if (!in_mask_helper && s->bare_moment_write == 0 &&
		    (is_assignment_to(line, "T1MR0") ||
		     is_assignment_to(line, "T1MR1") ||
		     is_assignment_to(line, "T1MR2") ||
		     is_assignment_to(line, "T1MR3")))
			s->bare_moment_write = lineno;

		/* A moment worked out from the counter and then handed to one of
		 * the arming calls. Reading the counter for anything else -- a
		 * packet's timestamp, how long since the last packet -- is not
		 * this, so the test is the two together on one line. */
		if (!in_mask_helper && !in_grid_caller &&
		    s->bare_moment_from_counter == 0 &&
		    contains_identifier(line, "NOW") &&
		    (calls_identifier(line, "timer1_set_match") ||
		     calls_identifier(line, "timer1_arm_match")))
			s->bare_moment_from_counter = lineno;

		/* The two callers whose moment comes from the connection's grid
		 * rather than the counter: they read it only to ask whether that
		 * moment has gone by, and they do it inside a guard of their own. */
		if (strstr(line, "void timer1_arm_conn_event(") != NULL ||
		    strstr(line, "void timer1_set_match_ahead(") != NULL) {
			if (strchr(line, ';') == NULL)
				in_grid_caller = 1;
		} else if (in_grid_caller && line[0] == '}') {
			in_grid_caller = 0;
		}

		/* The SSP's receive request, which three contexts change a bit at
		 * a time. le_phy.c reaches it only through the guarded helper, so
		 * the register's name does not belong in this file at all. */
		if (contains_identifier(line, "DIO_SSP_DMACR") &&
		    s->bare_rx_request_use == 0)
			s->bare_rx_request_use = lineno;

		/* The handover of a finished packet, in the byte interrupt, and
		 * the rest of what that interrupt does with state the timer
		 * interrupt shares. */
		if (strstr(line, "void le_dma_interrupt(") != NULL &&
		    strchr(line, ';') == NULL)
			in_dma = 1;
		if (in_dma) {
			if (calls_identifier(line, "buffer_get") &&
			    s->handover_take == 0)
				s->handover_take = lineno;
			if (calls_identifier(line, "irq_save_disable") &&
			    s->handover_mask == 0)
				s->handover_mask = lineno;
			if (calls_identifier(line, "queue_insert") &&
			    s->handover_queue == 0)
				s->handover_queue = lineno;
			if (calls_identifier(line, "irq_restore") &&
			    s->handover_restore == 0)
				s->handover_restore = lineno;

			/* Whether this line sits inside a guarded section. The
			 * calls are on lines of their own, so counting them
			 * before and after the test below keeps the boundary
			 * lines out of the section. */
			if (calls_identifier(line, "irq_restore"))
				dma_guard_depth--;
			if (contains_identifier(line, "conn_event")) {
				if (dma_guard_depth > 0)
					s->guarded_event_uses++;
				else if (s->bare_event_use == 0)
					s->bare_event_use = lineno;
			}
			if (calls_identifier(line, "timer1_clear_match") &&
			    s->bare_deadline_clear == 0)
				s->bare_deadline_clear = lineno;
			if (calls_identifier(line, "irq_save_disable"))
				dma_guard_depth++;

			if (line[0] == '}')
				in_dma = 0;
		}

		/* The two update handlers, which run in the mode's loop while the
		 * timer interrupt reads what they write. */
		if (strstr(line, "static void channel_map_update_handler(") != NULL &&
		    strchr(line, ';') == NULL)
			in_map_handler = 1;
		else if (in_map_handler) {
			if (is_assignment_of(line, "conn.channel_map_update_pending",
			                     "0") && s->map_cleared == 0)
				s->map_cleared = lineno;
			if (is_assignment_to(line, "conn.channel_map_update_instant") &&
			    s->map_instant == 0)
				s->map_instant = lineno;
			if (calls_identifier(line, "le_parse_channel_map") &&
			    s->map_parsed == 0)
				s->map_parsed = lineno;
			if (is_assignment_of(line, "conn.channel_map_update_pending",
			                     "1") && s->map_published == 0)
				s->map_published = lineno;
			if (line[0] == '}')
				in_map_handler = 0;
		}

		if (strstr(line, "static void connection_update_handler(") != NULL &&
		    strchr(line, ';') == NULL)
			in_update_handler = 1;
		else if (in_update_handler) {
			if (is_assignment_of(line, "conn.conn_update_pending", "0") &&
			    s->update_cleared == 0)
				s->update_cleared = lineno;
			if (is_assignment_to(line, "conn.conn_update_instant") &&
			    s->update_instant == 0)
				s->update_instant = lineno;
			if (is_assignment_of(line, "conn.conn_update_pending", "1") &&
			    s->update_published == 0)
				s->update_published = lineno;
			if (line[0] == '}')
				in_update_handler = 0;
		}

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
			if (strcmp(name, "blink_timer_start") == 0 &&
			    s->blink_timer_start == 0)
				s->blink_timer_start = lineno;
			if (strcmp(name, "blink_timer_stop") == 0 &&
			    s->blink_timer_stop == 0)
				s->blink_timer_stop = lineno;
			if (in_loop && strcmp(name, "blink_expire") == 0 &&
			    s->blink_expire_in_loop == 0)
				s->blink_expire_in_loop = lineno;
		}

		/* The mode's own loop, so a call can be placed inside it rather
		 * than merely somewhere in the function. The loop is written at one
		 * tab and its body at two, so its closing brace is the next line
		 * that is a tab and a brace alone. */
		if (!in_loop && strstr(line, "while (requested_mode") != NULL)
			in_loop = 1;
		else if (in_loop && strncmp(line, "\t}", 2) == 0)
			in_loop = 0;

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

/*
 * The other half of the SSP's receive request: the driver that owns the
 * register. Every partial write of it there sits inside a guarded section, so
 * "the request bit is never written on a value another context has moved on
 * from" is a statement about the whole image rather than about le_phy.c alone.
 */
struct ssp_scan {
	int bare_use;      /* a partial write outside a guarded section */
	int guarded_uses;  /* partial writes inside one */
};

static int scan_ssp_driver(const char *file, struct ssp_scan *s)
{
	char line[SCAN_LINE_MAX];
	int lineno = 0, in_block = 0, depth = 0;
	FILE *f = fopen(file, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		int touches, partial;

		lineno++;
		strip_comments(line, &in_block);

		/* As in le_dma_interrupt() above: the calls are on lines of their
		 * own, so counting them either side of the test keeps the
		 * boundary lines outside the section. */
		if (calls_identifier(line, "irq_restore") && depth > 0)
			depth--;

		touches = contains_identifier(line, "DIO_SSP_DMACR") ||
		          contains_identifier(line, "DIO_SSP_CR1");
		partial = touches && (strstr(line, "|=") != NULL ||
		                      strstr(line, "&=") != NULL);
		if (partial) {
			if (depth > 0)
				s->guarded_uses++;
			else if (s->bare_use == 0)
				s->bare_use = lineno;
		}

		if (calls_identifier(line, "irq_save_disable"))
			depth++;
	}

	fclose(f);
	return 1;
}

static struct le_phy_scan s;
static int scanned;
static const char *path = FW_RXTX_DIR "/le_phy.c";
static struct ssp_scan d;
static int scanned_dma;
static const char *dma_path = FW_RXTX_DIR "/ubertooth_dma.c";

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

/*
 * Anything that reaches T1MCR for one match rather than the whole register
 * goes through one of the three helpers that hold interrupts off while they do
 * it.
 *
 * All three matches live in that one register -- the connection event, the
 * synthesizer-lock wait, and the buffer wait -- and no single context owns
 * them: the main loop arms the event and the lock wait while a connection is
 * being followed, and both interrupt handlers arm and disarm the waits.
 * Setting a bit is a read-modify-write, so an interrupt
 * landing between the read and the write costs the interrupt its bit. When the
 * bit lost is the lock wait's, nothing arms receive again: the loop runs on,
 * feeding the watchdog and answering the host, while the capture stays silent
 * until the mode is restarted.
 *
 * Reads count as well as writes. The transfer-error recovery arms the event
 * match only if nothing has armed it, and the interrupt that arms it for the
 * real next event preempts the one the recovery runs in: a decision made from
 * a read outside the guard is as wrong as a write built on one, and opens the
 * next event an interval early.
 *
 * This is a scan because the register is hardware. What the guard does is run
 * in test_le_phy_logic.c, which stands an interrupt in at the end of a guarded
 * section; the scan is what notices a new site added later without one.
 */
MU_TEST(test_every_match_bit_is_changed_with_interrupts_held_off)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: T1MCR is read or part-written here without holding "
	         "interrupts off; go through timer1_arm_match(), "
	         "timer1_arm_match_in(), "
	         "timer1_disarm_match_interrupt() or "
	         "timer1_set_match_in_if_unarmed(), or an interrupt landing in "
	         "middle loses the bit it armed", path, s.bare_match_use);
	mu_assert(s.bare_match_use == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan found %d guarded uses of T1MCR, expected one in "
	         "each of the four helpers -- the scan has stopped reading "
	         "them", path, s.guarded_match_uses);
	mu_assert(s.guarded_match_uses == 4, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the moment a match fires at is written here, apart from "
	         "the bit that arms it; pass it to timer1_arm_match() or "
	         "timer1_arm_match_in() instead, "
	         "or an interrupt between the two leaves the match armed against "
	         "a time the counter has gone past", path, s.bare_moment_write);
	mu_assert(s.bare_moment_write == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: this works a match's moment out from the counter before "
	         "the guard that arms it; give the interval to one of the _in "
	         "forms and let it read the counter, or an interrupt taken in "
	         "between can outlast the interval and the match goes in behind "
	         "the counter", path, s.bare_moment_from_counter);
	mu_assert(s.bare_moment_from_counter == 0, msg);
}

/*
 * The LED blink stays off timer1.
 *
 * Timer1 is the radio's clock: every deadline the capture keeps is a match of
 * it, and its counter is what they are measured against. The blink marks a
 * packet handed to the host, so it is armed from the mode's loop, and while it
 * was a fourth match of timer1 that loop had to write the register the
 * radio's three matches live in. It runs on timer2 now, which no other mode
 * can be using at the same time.
 *
 * Timer2's own interrupt stays masked as well, so the blink costs the radio's
 * timing nothing: a handler of its own could cut into the capture the same way
 * the match did. The loop polls the counter instead, which is a plain read.
 *
 * What the blink does with that counter runs in test_le_phy_logic.c. This is
 * the scan that notices it being put back on timer1, or given an interrupt.
 */
MU_TEST(test_the_blink_is_not_on_the_radios_timer)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: timer1's match 1 is named here, which is the LED blink's "
	         "old home; the blink belongs on timer2 so the mode's loop never "
	         "writes the register the radio's own matches live in",
	         path, s.timer1_blink_match);
	mu_assert(s.timer1_blink_match == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: timer2's interrupt is unmasked here; the blink is polled "
	         "from the loop on purpose, so no handler of its own can cut into "
	         "the capture", path, s.timer2_unmask);
	mu_assert(s.timer2_unmask == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: le_phy_main() never starts the blink's counter, so the "
	         "deadline is measured against a counter that is not running",
	         path);
	mu_assert(s.blink_timer_start != 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: le_phy_main() never stops the blink's counter, so timer2 is "
	         "left running and powered for the modes that follow", path);
	mu_assert(s.blink_timer_stop != 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the mode's loop never calls blink_expire(), so nothing "
	         "turns the LED off and it stays on from the first packet to the "
	         "end of the mode", path);
	mu_assert(s.blink_expire_in_loop != 0, msg);
}

/*
 * A finished packet is handed to the host under one guard, together with
 * taking the buffer for the next packet, and the new buffer is taken before
 * the finished one is queued.
 *
 * The order is the point. current_rxbuf is what the timer interrupt acts on --
 * the channel change clears the buffer it names, the waits receive into it --
 * so while the pointer still named a finished packet, a match reached it and
 * the host was given zeroes, or bytes from a later moment in the air. Both
 * steps have to be inside the guard: with the buffer taken outside it, the
 * pointer still names the finished packet while the new one is cleared, which
 * is the same window a few instructions narrower.
 *
 * What the guard does is run in test_le_phy_logic.c. This is the part a scan
 * can see: that nothing in the handover sits outside the mask.
 */
MU_TEST(test_a_finished_packet_is_handed_over_in_one_guarded_step)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not read the handover in le_dma_interrupt(): "
	         "buffer_get %d, irq_save_disable %d, queue_insert %d, "
	         "irq_restore %d", path, s.handover_take, s.handover_mask,
	         s.handover_queue, s.handover_restore);
	mu_assert_fatal(s.handover_take != 0 && s.handover_mask != 0 &&
	                s.handover_queue != 0 && s.handover_restore != 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the handover is not all inside the guard: mask at line %d, "
	         "buffer taken at %d, packet queued at %d, mask off at %d -- a "
	         "timer match taken outside it finds current_rxbuf naming a "
	         "packet that is on its way to the host", path, s.handover_mask,
	         s.handover_take, s.handover_queue, s.handover_restore);
	mu_assert(s.handover_mask < s.handover_take &&
	          s.handover_take < s.handover_queue &&
	          s.handover_queue < s.handover_restore, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the buffer is taken before the finished packet is "
	         "queued (line %d), or the pointer would name a queued buffer",
	         path, s.handover_take, s.handover_queue);
	mu_assert(s.handover_take < s.handover_queue, msg);
}

/*
 * Counting a received packet into the connection event it arrived in is done
 * with interrupts held off, and the event's deadline is never dropped from
 * here without first asking whether the event is still open.
 *
 * Both halves of the event's bookkeeping belong to two contexts. The byte
 * interrupt counts packets into the event and anchors it; the timer interrupt
 * closes it through finish_conn_event(), which resets the count and arms the
 * next event in the same match register. The timer interrupt preempts the byte
 * interrupt, and a packet is allowed to begin as late as its event's deadline,
 * so a packet can reach the end of this handler with its event already closed.
 *
 * Counted into the event that follows, such a packet put its own time into an
 * anchor that event had not reached and armed a 300 microsecond inter-frame
 * timeout over that event's own match, which cost the follow the event and
 * left the connection's event counter one ahead of the peer's -- so every
 * connection update and channel map instant after it was matched an event
 * early.
 *
 * A scan because the interrupt priorities are hardware. What the engine does
 * with a packet that arrives after its event has closed is run in
 * test_le_phy_logic.c.
 */
MU_TEST(test_a_packet_is_counted_into_its_event_with_interrupts_held_off)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the scan found %d guarded mentions of conn_event in "
	         "le_dma_interrupt(), so it has stopped reading the function",
	         path, s.guarded_event_uses);
	mu_assert_fatal(s.guarded_event_uses >= 3, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: le_dma_interrupt() reaches the connection event here "
	         "without holding interrupts off; the timer interrupt closes the "
	         "same event and arms the next one, so a count or an anchor set "
	         "here can land in an event this packet did not arrive in",
	         path, s.bare_event_use);
	mu_assert(s.bare_event_use == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: le_dma_interrupt() drops the connection event's "
	         "deadline here without asking whether the event is still open; "
	         "go through timer1_clear_match_if_event_open(), or a packet that "
	         "began at its deadline drops the next event's own match",
	         path, s.bare_deadline_clear);
	mu_assert(s.bare_deadline_clear == 0, msg);
}

/*
 * A change the mode's loop schedules for the timer interrupt to apply is
 * published by its flag, and the flag goes up only once everything the
 * interrupt will read is in place -- and goes down first, so an update already
 * waiting for its instant is not applied part way through being replaced.
 *
 * The connection update and the channel map update are both read on every
 * connection event by the timer interrupt, which preempts the loop. With the
 * flag set first, that interrupt could reach an instant left over from an
 * earlier update, or the zero a connection starts from, which every event
 * counter under 32768 has already reached. The channel map is the worse of the
 * two: le_parse_channel_map() clears the map before filling it, so what the
 * follow would hop on is a map holding some of the update's channels and
 * missing the rest, and the receiver tunes where the peer is not for the rest
 * of the connection.
 */
MU_TEST(test_a_scheduled_change_is_published_only_once_it_is_whole)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not read channel_map_update_handler(): flag "
	         "cleared at %d, instant at %d, map parsed at %d, flag set at %d",
	         path, s.map_cleared, s.map_instant, s.map_parsed,
	         s.map_published);
	mu_assert_fatal(s.map_cleared > 0 && s.map_instant > 0 &&
	                s.map_parsed > 0 && s.map_published > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the channel map update is marked pending before its "
	         "instant (line %d) and its map (line %d) are written, so the "
	         "timer interrupt can apply a map that is half parsed",
	         path, s.map_published, s.map_instant, s.map_parsed);
	mu_assert(s.map_published > s.map_instant &&
	          s.map_published > s.map_parsed, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the pending flag is not cleared before the instant "
	         "(line %d) and the map (line %d) are rewritten, so an update "
	         "still waiting for its instant can be applied as a mixture of "
	         "the two", path, s.map_cleared, s.map_instant, s.map_parsed);
	mu_assert(s.map_cleared < s.map_instant &&
	          s.map_cleared < s.map_parsed, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not read connection_update_handler(): flag "
	         "cleared at %d, instant at %d, flag set at %d",
	         path, s.update_cleared, s.update_instant, s.update_published);
	mu_assert_fatal(s.update_cleared > 0 && s.update_instant > 0 &&
	                s.update_published > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the connection update is marked pending before its "
	         "instant (line %d), so the timer interrupt can apply it at an "
	         "instant from an earlier update",
	         path, s.update_published, s.update_instant);
	mu_assert(s.update_published > s.update_instant, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the pending flag is not cleared before the new "
	         "parameters (instant at line %d) are written, so an update "
	         "still waiting for its instant can be applied with the new "
	         "interval against the old instant",
	         path, s.update_cleared, s.update_instant);
	mu_assert(s.update_cleared < s.update_instant, msg);
}

/*
 * Arming and disarming the SSP's receive DMA request is done with interrupts
 * held off, wherever it is done.
 *
 * That register carries the transmit request alongside the receive one, so
 * changing one bit is a read-modify-write, and three contexts change it while
 * the LE engine runs: the mode's loop as it starts, the timer interrupt on
 * every hop, and the byte interrupt at the end of every packet. The timer
 * interrupt preempts the byte interrupt.
 *
 * Losing the bit the timer interrupt set is a silent capture: the radio is
 * strobed into receive immediately after the request is armed, so the receiver
 * then clocks symbols into the SSP with nothing collecting them. No byte
 * reaches a buffer and no interrupt is raised, while the loop goes on feeding
 * the watchdog and answering the host.
 *
 * The register is hardware, so this is a scan. le_phy.c does not name it at
 * all -- it goes through the driver's helper -- and in the driver every
 * partial write is inside a guard.
 */
MU_TEST(test_the_receive_request_is_changed_with_interrupts_held_off)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: the SSP's receive request is changed here a bit at a "
	         "time; call dio_ssp_rx_dma_disable() or dio_ssp_start(), which "
	         "hold interrupts off, or an interrupt landing in the middle "
	         "leaves receive armed with nothing collecting the bytes",
	         path, s.bare_rx_request_use);
	mu_assert(s.bare_rx_request_use == 0, msg);

	snprintf(msg, sizeof(msg), "cannot open %s to scan", dma_path);
	mu_assert_fatal(scanned_dma, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan found %d guarded partial writes of the SSP's "
	         "registers, expected two in dio_ssp_start(), one in "
	         "dio_ssp_rx_dma_disable() and two in dio_ssp_stop() -- the scan "
	         "has stopped reading them", dma_path, d.guarded_uses);
	mu_assert(d.guarded_uses == 5, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: an SSP register is changed a bit at a time here without "
	         "holding interrupts off, so the bit another context set can be "
	         "written away", dma_path, d.bare_use);
	mu_assert(d.bare_use == 0, msg);
}

int main(void)
{
	printf("le_phy bring-up and teardown order tests\n");
	scanned = scan_le_phy(path, &s);
	scanned_dma = scan_ssp_driver(dma_path, &d);
	MU_RUN(test_source_was_scanned);
	MU_RUN(test_a_run_starts_with_no_dropped_packet_to_report);
	MU_RUN(test_the_run_starts_with_nothing_pending);
	MU_RUN(test_active_is_set_after_the_setup);
	MU_RUN(test_the_interrupt_is_unmasked_last);
	MU_RUN(test_active_is_cleared_before_the_teardown);
	MU_RUN(test_every_match_bit_is_changed_with_interrupts_held_off);
	MU_RUN(test_the_blink_is_not_on_the_radios_timer);
	MU_RUN(test_a_finished_packet_is_handed_over_in_one_guarded_step);
	MU_RUN(test_a_packet_is_counted_into_its_event_with_interrupts_held_off);
	MU_RUN(test_a_scheduled_change_is_published_only_once_it_is_whole);
	MU_RUN(test_the_receive_request_is_changed_with_interrupts_held_off);
	return mu_report();
}
