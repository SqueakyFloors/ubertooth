/*
 * Guard for the order in which low-energy promiscuous recovery hands a
 * connection to the hopping follow.
 *
 * Two paths in firmware/bluetooth_rxtx/bluetooth_rxtx.c do that handoff: one
 * for a connection using channel selection algorithm 1, which is recovered
 * from its hop increment, and one for a connection using algorithm 2, which
 * is recovered from its event counter. Both set the same state, and
 * TIMER0_IRQHandler() runs between any two of those statements.
 *
 * The interrupt asks for a hop on whichever branch hop_mode selects. Its
 * low-energy branch asks only once le.link_state says connected, and the
 * branch for no hopping or a band sweep asks on a bare clock tick. So the
 * handoff has to set the hop mode first, which parks the interrupt on the
 * branch that waits, and the link state last, which is what starts the
 * hopping. In the other order the interrupt can ask for a hop while the mode
 * in force is still the one that needs no connection, and the follow then
 * takes that hop as its first: it spends an event counter and retunes at
 * once, so the capture starts an event away from the connection.
 *
 * Neither path can run here -- it is hardware code reached from a capture
 * loop -- so this test scans the source, the way test_le_phy_order.c scans
 * the PHY's bring-up. It reads the three marks of each handoff and the
 * interrupt's two branches, and it fails if any of them moves out of order
 * or goes missing.
 *
 * It also reads bluetooth_le.h for the volatile on link_state. Without it the
 * store of the link state is a plain store that the compiler may move across
 * the volatile store of the hop mode, which would undo the order this test
 * pins in the source. The emitted code was checked by hand as well; the
 * qualifier is what makes the order binding rather than observed.
 *
 * The same goes for the connection's own fields, which are plain stores that
 * no qualifier orders against the link state, so each handoff holds them
 * above it with a barrier. The scan reads that too: the order in the source
 * is only worth pinning while it is also the order the board runs.
 */
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

static const char *path = FW_RXTX_DIR "/bluetooth_rxtx.c";
static const char *header = FW_RXTX_DIR "/bluetooth_le.h";

/*
 * Line numbers of what the scan looks for, 0 for not found. One set per
 * handoff, plus the interrupt's branches and the header.
 */
struct handoff {
	int hop_mode;      /* hop_mode = HOP_BTLE */
	int do_hop;        /* do_hop = 0 */
	int link_state;    /* le.link_state = LINK_CONNECTED */
	int barrier;       /* le_handoff_barrier() */
	int last_field;    /* the last le.<field> the handoff sets */
	int fields;        /* how many le.<field> assignments it makes */
};

struct promisc_scan {
	struct handoff algorithm_1;   /* promisc_recover_hop_increment() */
	struct handoff algorithm_2;   /* promisc_try_csa2() */
	/* In TIMER0_IRQHandler(): the low-energy branch, the link-state guard
	 * inside it, and the hop that guard allows. */
	int btle_branch;
	int btle_guard;
	int btle_hop;
	/* The same handler's branch for no hopping or a band sweep: the clock
	 * test and the hop it allows, which is the hop the old order let
	 * through. Nothing about a connection stands in its way, which is why
	 * the mode has to be moved off it first. */
	int sweep_clock_test;
	int sweep_hop;
	/* In bluetooth_le.h: the declaration of link_state, and whether it is
	 * volatile. */
	int declaration;
	int declared_volatile;
};

/* The fields of le the follow needs before it is told to hop. */
static const char *const needed_fields[] = {
	"le.interval_timer", "le.conn_count", "le.conn_epoch",
	"le.channel_idx", "le.channel_increment",
	"le.conn_interval", "le.chan_sel_2", "le.event_counter",
};

static int is_needed_field(const char *line, const char **which)
{
	unsigned i;

	for (i = 0; i < sizeof(needed_fields) / sizeof(needed_fields[0]); i++) {
		if (is_assignment_to(line, needed_fields[i])) {
			*which = needed_fields[i];
			return 1;
		}
	}
	return 0;
}

static void note_handoff_line(struct handoff *h, const char *line, int lineno)
{
	const char *which;

	if (is_assignment_of(line, "hop_mode", "HOP_BTLE") && h->hop_mode == 0)
		h->hop_mode = lineno;
	if (is_assignment_of(line, "do_hop", "0") && h->do_hop == 0)
		h->do_hop = lineno;
	if (is_assignment_of(line, "le.link_state", "LINK_CONNECTED") &&
	    h->link_state == 0)
		h->link_state = lineno;
	if (strstr(line, "le_handoff_barrier(") != NULL && h->barrier == 0)
		h->barrier = lineno;
	if (is_needed_field(line, &which)) {
		h->last_field = lineno;
		h->fields++;
	}
}

static int scan_promisc(const char *source, struct promisc_scan *s)
{
	char line[SCAN_LINE_MAX];
	int lineno = 0, in_block = 0;
	int in_alg1 = 0, in_alg2 = 0, in_timer = 0, in_btle = 0, in_sweep = 0;
	FILE *f = fopen(source, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		strip_string_literals(line);

		/* A function definition starts in the first column and its
		 * closing brace is the next line that does, which is how every
		 * scan in this directory finds the end of one. */
		if (line[0] == '}') {
			in_alg1 = in_alg2 = in_timer = 0;
			in_btle = in_sweep = 0;
		} else if (strstr(line, "promisc_recover_hop_increment(") != NULL &&
		           strchr(line, '{') != NULL && line[0] != '\t') {
			in_alg1 = 1;
		} else if (strstr(line, "promisc_try_csa2(") != NULL &&
		           strchr(line, '{') != NULL && line[0] != '\t') {
			in_alg2 = 1;
		} else if (strstr(line, "TIMER0_IRQHandler(") != NULL &&
		           line[0] != '\t' && strchr(line, ';') == NULL) {
			/* The handler's definition opens its brace on the next line,
			 * so there is no '{' to look for here. A declaration of it
			 * ends in ';' and is not the body. */
			in_timer = 1;
		}

		if (in_alg1)
			note_handoff_line(&s->algorithm_1, line, lineno);
		if (in_alg2)
			note_handoff_line(&s->algorithm_2, line, lineno);

		if (!in_timer)
			continue;

		/* Which of the handler's hop branches the following lines are
		 * in. Each branch tests hop_mode, so the test itself marks the
		 * start of one and the start of the next ends it. */
		if (strstr(line, "hop_mode == HOP_BTLE") != NULL) {
			in_btle = 1;
			in_sweep = 0;
			if (s->btle_branch == 0)
				s->btle_branch = lineno;
		} else if (strstr(line, "hop_mode == HOP_NONE") != NULL ||
		           strstr(line, "hop_mode == HOP_SWEEP") != NULL) {
			in_sweep = 1;
			in_btle = 0;
		} else if (strstr(line, "hop_mode ==") != NULL) {
			in_btle = in_sweep = 0;
		}

		if (in_btle) {
			if (s->btle_guard == 0 &&
			    strstr(line, "le.link_state == LINK_CONNECTED") != NULL)
				s->btle_guard = lineno;
			if (s->btle_hop == 0 && is_assignment_of(line, "do_hop", "1"))
				s->btle_hop = lineno;
		}
		if (in_sweep) {
			if (s->sweep_clock_test == 0 &&
			    contains_identifier(line, "clkn") &&
			    strchr(line, '&') != NULL)
				s->sweep_clock_test = lineno;
			if (s->sweep_hop == 0 && is_assignment_of(line, "do_hop", "1"))
				s->sweep_hop = lineno;
		}
	}

	fclose(f);
	return 1;
}

static int scan_header(const char *source, struct promisc_scan *s)
{
	char line[SCAN_LINE_MAX];
	int lineno = 0, in_block = 0;
	FILE *f = fopen(source, "r");

	if (f == NULL)
		return 0;

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		/* contains_identifier() matches a whole identifier, so the end of
		 * the type's own definition -- "} link_state_t;" -- is not a line
		 * this takes an interest in. */
		if (!contains_identifier(line, "link_state") ||
		    strchr(line, ';') == NULL)
			continue;
		if (s->declaration == 0 && strstr(line, "link_state;") != NULL) {
			s->declaration = lineno;
			s->declared_volatile = contains_identifier(line, "volatile");
		}
	}

	fclose(f);
	return 1;
}

static struct promisc_scan s;
static int scanned, scanned_header;

MU_TEST(test_both_sources_were_scanned)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "%s would not open", path);
	mu_assert_fatal(scanned, msg);
	snprintf(msg, sizeof(msg), "%s would not open", header);
	mu_assert_fatal(scanned_header, msg);
}

/*
 * Both handoffs were found. A scan that found neither would pass every
 * ordering case below on nothing, so this runs first.
 */
MU_TEST(test_both_handoffs_were_found)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the algorithm-1 handoff's four marks were not all found "
	         "(hop_mode %d, do_hop %d, barrier %d, link_state %d); if the "
	         "recovery was rewritten, this test has to be rewritten with it",
	         path, s.algorithm_1.hop_mode, s.algorithm_1.do_hop,
	         s.algorithm_1.barrier, s.algorithm_1.link_state);
	mu_assert_fatal(s.algorithm_1.hop_mode > 0 && s.algorithm_1.do_hop > 0 &&
	                s.algorithm_1.barrier > 0 &&
	                s.algorithm_1.link_state > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the algorithm-2 handoff's four marks were not all found "
	         "(hop_mode %d, do_hop %d, barrier %d, link_state %d); if the "
	         "search was rewritten, this test has to be rewritten with it",
	         path, s.algorithm_2.hop_mode, s.algorithm_2.do_hop,
	         s.algorithm_2.barrier, s.algorithm_2.link_state);
	mu_assert_fatal(s.algorithm_2.hop_mode > 0 && s.algorithm_2.do_hop > 0 &&
	                s.algorithm_2.barrier > 0 &&
	                s.algorithm_2.link_state > 0, msg);
}

/*
 * The hop mode is set before the hop request is dropped, in both handoffs.
 *
 * Dropping the request first leaves the mode in force until now able to ask
 * for another before the mode moves, and that request outlives the handoff.
 */
MU_TEST(test_the_hop_mode_is_set_before_the_request_is_dropped)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: the algorithm-1 handoff drops the hop request at line "
	         "%d, before it sets the hop mode at line %d. A timer interrupt "
	         "between the two asks for a hop on the mode in force until now, "
	         "which needs no connection, and the follow takes that hop as "
	         "its first",
	         path, s.algorithm_1.do_hop, s.algorithm_1.do_hop,
	         s.algorithm_1.hop_mode);
	mu_assert(s.algorithm_1.hop_mode < s.algorithm_1.do_hop, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the algorithm-2 handoff drops the hop request at line "
	         "%d, before it sets the hop mode at line %d. A timer interrupt "
	         "between the two asks for a hop on the mode in force until now, "
	         "which needs no connection, and the follow takes that hop as "
	         "its first",
	         path, s.algorithm_2.do_hop, s.algorithm_2.do_hop,
	         s.algorithm_2.hop_mode);
	mu_assert(s.algorithm_2.hop_mode < s.algorithm_2.do_hop, msg);
}

/*
 * The link state is set last, in both handoffs.
 *
 * It is what starts the hopping, so everything the follow hops by -- the
 * interval timer, the counters, the channel -- has to be in place first, and
 * the mode has to have moved off the branch that needs none of them.
 */
MU_TEST(test_the_link_state_is_set_last)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: the algorithm-1 handoff says connected at line %d but "
	         "sets the hop mode at line %d. Until the mode moves, the timer "
	         "interrupt is on a branch that asks for a hop without one being "
	         "due",
	         path, s.algorithm_1.link_state, s.algorithm_1.link_state,
	         s.algorithm_1.hop_mode);
	mu_assert(s.algorithm_1.hop_mode < s.algorithm_1.link_state, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the algorithm-2 handoff says connected at line %d but "
	         "sets the hop mode at line %d. Until the mode moves, the timer "
	         "interrupt is on a branch that asks for a hop without one being "
	         "due",
	         path, s.algorithm_2.link_state, s.algorithm_2.link_state,
	         s.algorithm_2.hop_mode);
	mu_assert(s.algorithm_2.hop_mode < s.algorithm_2.link_state, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the algorithm-1 handoff sets a field of the connection "
	         "at line %d, after saying connected at line %d. The timer "
	         "interrupt hops by those fields from the moment the state says "
	         "connected",
	         path, s.algorithm_1.last_field, s.algorithm_1.last_field,
	         s.algorithm_1.link_state);
	mu_assert(s.algorithm_1.fields > 0 &&
	          s.algorithm_1.last_field < s.algorithm_1.link_state, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the algorithm-2 handoff sets a field of the connection "
	         "at line %d, after saying connected at line %d. The timer "
	         "interrupt hops by those fields from the moment the state says "
	         "connected",
	         path, s.algorithm_2.last_field, s.algorithm_2.last_field,
	         s.algorithm_2.link_state);
	mu_assert(s.algorithm_2.fields > 0 &&
	          s.algorithm_2.last_field < s.algorithm_2.link_state, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the algorithm-1 handoff drops the hop request at line "
	         "%d, after saying connected at line %d. A request left by the "
	         "mode in force until now then outlives the handoff, and the "
	         "capture loop takes it as the follow's first hop",
	         path, s.algorithm_1.do_hop, s.algorithm_1.do_hop,
	         s.algorithm_1.link_state);
	mu_assert(s.algorithm_1.do_hop < s.algorithm_1.link_state, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the algorithm-2 handoff drops the hop request at line "
	         "%d, after saying connected at line %d. A request left by the "
	         "mode in force until now then outlives the handoff, and the "
	         "capture loop takes it as the follow's first hop",
	         path, s.algorithm_2.do_hop, s.algorithm_2.do_hop,
	         s.algorithm_2.link_state);
	mu_assert(s.algorithm_2.do_hop < s.algorithm_2.link_state, msg);
}

/*
 * The barrier sits between the connection's fields and the link state, in
 * both handoffs.
 *
 * The order above is the order of the source. Three of these stores are
 * volatile and keep their order on their own, but the connection's other
 * fields are plain, and two of those are read by the timer interrupt. Without
 * the barrier the compiler may move a plain store below the store of the link
 * state, which hands the interrupt a connection with a stale epoch or
 * interval while the source still reads in the safe order.
 */
MU_TEST(test_the_barrier_sits_before_the_link_state)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s:%d: the algorithm-1 handoff's barrier is at line %d, not "
	         "between its last field at line %d and the link state at line "
	         "%d. The fields it holds above the link state are plain stores "
	         "the compiler may otherwise move below it",
	         path, s.algorithm_1.barrier, s.algorithm_1.barrier,
	         s.algorithm_1.last_field, s.algorithm_1.link_state);
	mu_assert(s.algorithm_1.last_field < s.algorithm_1.barrier &&
	          s.algorithm_1.barrier < s.algorithm_1.link_state, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the algorithm-2 handoff's barrier is at line %d, not "
	         "between its last field at line %d and the link state at line "
	         "%d. The fields it holds above the link state are plain stores "
	         "the compiler may otherwise move below it",
	         path, s.algorithm_2.barrier, s.algorithm_2.barrier,
	         s.algorithm_2.last_field, s.algorithm_2.link_state);
	mu_assert(s.algorithm_2.last_field < s.algorithm_2.barrier &&
	          s.algorithm_2.barrier < s.algorithm_2.link_state, msg);
}

/*
 * The premise the order rests on: the timer interrupt's low-energy branch
 * asks for a hop only once the link state says connected.
 *
 * Setting the hop mode first is only safe because of this guard. A hop
 * request moved out from behind it would be asked for with the connection's
 * fields half written, which is the window the order exists to close.
 */
MU_TEST(test_the_interrupts_low_energy_hop_waits_for_the_link_state)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the timer interrupt's low-energy branch was not found; "
	         "the handoff order below it rests on what that branch does",
	         path);
	mu_assert_fatal(s.btle_branch > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the low-energy branch at line %d asks for a hop at line "
	         "%d without first reading the link state. The handoff sets the "
	         "hop mode before the link state because this branch waits for "
	         "it; with the guard gone, that order asks for a hop while the "
	         "connection's fields are half written",
	         path, s.btle_branch, s.btle_branch, s.btle_hop);
	mu_assert(s.btle_guard > 0 && s.btle_hop > 0 &&
	          s.btle_guard < s.btle_hop, msg);
}

/*
 * The other premise: the branch for no hopping or a band sweep asks for a hop
 * on a clock tick alone.
 *
 * This is the hop the old order let through, and it is why the mode has to be
 * moved first rather than the state being set first. It is recorded here so
 * that a reader of the test can see both halves of the reasoning.
 */
MU_TEST(test_the_sweep_branch_asks_for_a_hop_on_a_clock_tick_alone)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the timer interrupt's branch for no hopping or a band "
	         "sweep no longer asks for a hop from a clock tick (test %d, "
	         "hop %d). If it has stopped asking for one, the handoff order "
	         "no longer turns on it and this test should be revisited",
	         path, s.sweep_clock_test, s.sweep_hop);
	mu_assert(s.sweep_clock_test > 0 && s.sweep_hop > 0 &&
	          s.sweep_clock_test < s.sweep_hop, msg);
}

/*
 * link_state is volatile, which is what makes the handoff's store order
 * binding.
 *
 * hop_mode and do_hop are volatile, so the compiler keeps those two stores in
 * order. A plain store of link_state is not ordered against them: the
 * compiler may move it earlier, and in this file it does move plain stores
 * across volatile ones. Then the source would read in the safe order and the
 * emitted code would not.
 */
MU_TEST(test_the_link_state_is_a_volatile_field)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the declaration of link_state was not found", header);
	mu_assert_fatal(s.declaration > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: link_state is not volatile. The handoffs set the hop "
	         "mode before it and rely on the stores reaching memory in that "
	         "order; a plain store may be moved in front of the volatile "
	         "store of the hop mode, which puts the window back",
	         header, s.declaration);
	mu_assert(s.declared_volatile, msg);
}

int main(void)
{
	printf("low-energy promiscuous handoff order tests\n");
	scanned = scan_promisc(path, &s);
	scanned_header = scan_header(header, &s);
	MU_RUN(test_both_sources_were_scanned);
	MU_RUN(test_both_handoffs_were_found);
	MU_RUN(test_the_hop_mode_is_set_before_the_request_is_dropped);
	MU_RUN(test_the_link_state_is_set_last);
	MU_RUN(test_the_barrier_sits_before_the_link_state);
	MU_RUN(test_the_interrupts_low_energy_hop_waits_for_the_link_state);
	MU_RUN(test_the_sweep_branch_asks_for_a_hop_on_a_clock_tick_alone);
	MU_RUN(test_the_link_state_is_a_volatile_field);
	return mu_report();
}
