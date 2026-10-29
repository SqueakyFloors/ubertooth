/*
 * The connection-interval estimator in the low-energy promiscuous mode, and
 * the capture clock's rollover.
 *
 * ubertooth-btle -p works out a connection interval by timing how long the
 * connection takes to come back to the channel it is listening on, and keeping
 * the smallest gap it has seen. The capture clock wraps every CLK100NS_PERIOD
 * ticks (3125 * 2^20, about 327.68 s), so a gap that straddles the wrap has to
 * have the period added back.
 *
 * That addition used the literal 3267800000, two digits transposed from
 * 3276800000, which is 9,000,000 ticks -- 0.9 s -- short. Because the
 * estimator only ever takes a minimum, a gap computed 0.9 s short is latched
 * and reported for the rest of the run.
 *
 * promisc_recover_hop_interval() is hardware code -- it reads the clock
 * registers -- so this models it, the way test_mode_state_reset.c models the
 * mode dispatch, and takes the two numbers the model needs out of the firmware
 * source: the value added at each rollover site, and LE_BASECLK. A model
 * carrying its own copy of the period would pass whatever the firmware did.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"

/* How many rollover sites bluetooth_rxtx.c has today, so a scan that matched
 * nothing cannot pass. Update it when one is added or removed. */
#define ROLLOVER_SITES 3

/* The channels a low-energy connection hops over, which is how many
 * connection events pass between two sightings on one channel. */
#define LE_CHANNELS 37

#define MAX_SITES 8

/* One `x += <period>;` the firmware does to undo a clock rollover. */
struct site {
	char target[48];  /* what it adds to */
	char token[64];   /* what it adds */
	int line;
};

struct scan {
	struct site sites[MAX_SITES];
	int site_count;
	int le_baseclk;          /* LE_BASECLK, from ubertooth_clock.h */
	/* The site inside promisc_recover_hop_interval(), which is the one this
	 * test's model runs on. -1 until it is found: site 0 is a real site, so
	 * leaving it at zero would read a neighbour's constant and pass. */
	int interval_site;
	/* Where the estimator keeps its last sighting. A model cannot tell a
	 * function static from a member of the mode's state struct, and only
	 * the second is cleared when a pass starts, so the source says which
	 * it is. */
	int static_in_function;  /* a `static` stamp inside the estimator */
	int stamp_in_state;      /* the stamp, in le_promisc_state_t */
	/* Whether bt_promisc_le() clears the mode's state at each pass, and
	 * whether that clearing is a whole-struct wipe. The stamp being a
	 * member of that state only helps if both hold: a reset rewritten
	 * field by field could miss it and every model here would still
	 * pass. */
	int reset_in_promisc_loop;
	int reset_wipes_whole_state;
	/* The estimator's two moves that a model cannot see: returning on the
	 * first packet of a pass instead of measuring from a cleared stamp,
	 * and storing the clock reading rather than the value a rollover was
	 * undone on. */
	int guard_returns_on_first_packet;
	int stores_the_reading;
	/* The hop-increment stamp, which had the same fault as prev_clk. */
	int increment_static_in_function;
	int increment_stamp_in_state;
	/* Where the access address is announced to the host. The record is
	 * dropped unless it is queued after bt_le_sync() has emptied the
	 * queue, and announcing it twice would print the line twice. Being
	 * inside bt_le_sync() is not enough on its own: above the queue reset
	 * is where the record used to be lost, so the line number matters,
	 * and without the mode test the other two sniffing modes would
	 * announce an access address of their own. */
	int announce_sites;
	int announce_in_sync;
	int announce_line;
	int queue_init_line;
	int announce_guarded_by_mode;
	int flag_in_state;       /* and the flag that says it is set */
};

static struct scan s;
static const char *path = FW_RXTX_DIR "/bluetooth_rxtx.c";
static const char *clock_path = FW_RXTX_DIR "/ubertooth_clock.h";

/*
 * Is this token a period-sized amount -- the named constant, or a literal of
 * the same magnitude? Anything smaller is some other `+=` and not a rollover.
 */
static int is_period_sized(const char *token)
{
	char *end;
	unsigned long value;

	if (strcmp(token, "CLK100NS_PERIOD") == 0)
		return 1;
	value = strtoul(token, &end, 10);
	return *end == '\0' && value >= 1000000000UL;
}

/* What the token is worth. 0 for a name this test does not know. */
static uint32_t token_value(const char *token)
{
	if (strcmp(token, "CLK100NS_PERIOD") == 0)
		return CLK100NS_PERIOD;
	return (uint32_t)strtoul(token, NULL, 10);
}

/* The `name += token;` on a line, if it has one. */
static int compound_add(const char *line, char *target, size_t target_size,
                        char *token, size_t token_size)
{
	const char *plus = strstr(line, "+=");
	const char *start, *end;
	size_t len;

	if (plus == NULL)
		return 0;

	/* The identifier in front of the operator. */
	end = plus;
	while (end > line && isspace((unsigned char)end[-1]))
		end--;
	start = end;
	while (start > line && is_ident_char(start[-1]))
		start--;
	len = (size_t)(end - start);
	if (len == 0 || len >= target_size)
		return 0;
	memcpy(target, start, len);
	target[len] = '\0';

	/* The one word behind it, up to the semicolon. */
	start = plus + 2;
	while (*start != '\0' && isspace((unsigned char)*start))
		start++;
	end = start;
	while (is_ident_char(*end))
		end++;
	len = (size_t)(end - start);
	if (len == 0 || len >= token_size)
		return 0;
	memcpy(token, start, len);
	token[len] = '\0';
	return 1;
}

static int scan_sources(struct scan *out)
{
	char line[SCAN_LINE_MAX];
	char target[48], token[64], fn[64] = "", name[64];
	int in_block = 0, lineno = 0, in_state_struct = 0, saw_guard = 0;
	int saw_mode_test = 0;
	FILE *f = fopen(path, "r");

	memset(out, 0, sizeof(*out));
	out->interval_site = -1;
	if (f == NULL)
		return 0;

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		strip_string_literals(line);

		/* The mode's state struct, and the two members of it the
		 * estimator's stamp needs to be in. */
		if (strstr(line, "struct _le_promisc_state_t") != NULL)
			in_state_struct = 1;
		else if (in_state_struct && line[0] == '}')
			in_state_struct = 0;
		else if (in_state_struct) {
			if (contains_identifier(line, "prev_clk"))
				out->stamp_in_state = 1;
			if (contains_identifier(line, "seen_a_packet"))
				out->flag_in_state = 1;
			if (contains_identifier(line, "first_ts"))
				out->increment_stamp_in_state = 1;
		}
		if (strcmp(fn, "promisc_recover_hop_interval") == 0 &&
		    strstr(line, "static") != NULL &&
		    contains_identifier(line, "prev_clk"))
			out->static_in_function = 1;

		if (strcmp(fn, "bt_promisc_le") == 0 &&
		    strstr(line, "reset_le_promisc()") != NULL)
			out->reset_in_promisc_loop = 1;
		if (strcmp(fn, "reset_le_promisc") == 0 &&
		    strstr(line, "memset(&le_promisc, 0, sizeof(le_promisc))")
		        != NULL)
			out->reset_wipes_whole_state = 1;

		/* The guard: nothing is measured until a packet of this pass has
		 * set the flag, so the branch that sees it unset returns. */
		if (strcmp(fn, "promisc_recover_hop_interval") == 0) {
			if (strstr(line, "!le_promisc.seen_a_packet") != NULL)
				saw_guard = 1;
			else if (saw_guard && strstr(line, "return") != NULL) {
				out->guard_returns_on_first_packet = 1;
				saw_guard = 0;
			}
			/* `le_promisc.prev_clk = cur_clk;`, not `= measured;`. */
			if (strstr(line, "le_promisc.prev_clk = cur_clk") != NULL)
				out->stores_the_reading = 1;
		}

		if (strcmp(fn, "promisc_recover_hop_increment") == 0 &&
		    strstr(line, "static") != NULL &&
		    contains_identifier(line, "first_ts"))
			out->increment_static_in_function = 1;

		if (strcmp(fn, "bt_le_sync") == 0 &&
		    strstr(line, "usb_queue_init()") != NULL)
			out->queue_init_line = lineno;

		/* `le_promisc_state(0, ...)`: the record type that carries the
		 * access address. The other types carry the parameters the
		 * mode recovers afterwards. */
		if (strstr(line, "le_promisc_state(0,") != NULL) {
			out->announce_sites++;
			if (strcmp(fn, "bt_le_sync") == 0) {
				out->announce_in_sync = 1;
				out->announce_line = lineno;
				if (saw_mode_test)
					out->announce_guarded_by_mode = 1;
			}
		}
		saw_mode_test = strstr(line, "active_mode == MODE_BT_PROMISC_LE")
		                != NULL;

		/* Whose body the lines below are: a definition starts in column 1
		 * and is not a statement. */
		if (line[0] != '\0' && !isspace((unsigned char)line[0]) &&
		    line[0] != '#' && line[0] != '}' &&
		    strchr(line, ';') == NULL &&
		    call_name(line, name, sizeof(name)))
			snprintf(fn, sizeof(fn), "%s", name);

		if (!compound_add(line, target, sizeof(target), token,
		                  sizeof(token)))
			continue;
		if (!is_period_sized(token))
			continue;
		if (out->site_count == MAX_SITES)
			continue;
		snprintf(out->sites[out->site_count].target,
		         sizeof(out->sites[0].target), "%s", target);
		snprintf(out->sites[out->site_count].token,
		         sizeof(out->sites[0].token), "%s", token);
		out->sites[out->site_count].line = lineno;
		if (strcmp(fn, "promisc_recover_hop_interval") == 0)
			out->interval_site = out->site_count;
		out->site_count++;
	}
	fclose(f);

	/* LE_BASECLK: 1.25 ms, the unit a connection interval is counted in. */
	f = fopen(clock_path, "r");
	if (f == NULL)
		return 0;
	in_block = 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		int value;

		strip_comments(line, &in_block);
		if (sscanf(line, " #define LE_BASECLK (%d)", &value) == 1 ||
		    sscanf(line, " #define LE_BASECLK %d", &value) == 1)
			out->le_baseclk = value;
	}
	fclose(f);
	return 1;
}

/* ------------------------------------------------------------------------
 * The estimator, modelled
 * ------------------------------------------------------------------------ */

/* DIVIDE_ROUND(), from bluetooth_rxtx.c. */
#define DIVIDE_ROUND(N, D) ((N) + (D)/2) / (D)

struct estimator {
	uint32_t prev_clk;
	int seen_a_packet;
	uint32_t smallest_hop_interval;
	uint16_t conn_interval;
	int consec_intervals;
};

/* reset_le_promisc(), which clears the whole of the mode's state. */
static void estimator_init(struct estimator *e)
{
	memset(e, 0, sizeof(*e));
	e->smallest_hop_interval = 0xffffffff;
}

/*
 * A connection already being followed, whose last sighting on this channel was
 * at `last`: seeding that stamp is what puts the model where the interesting
 * packet arrives, rather than at the first packet of a pass.
 */
static void following_since(struct estimator *e, uint32_t last)
{
	estimator_init(e);
	e->prev_clk = last;
	e->seen_a_packet = 1;
}

/*
 * promisc_recover_hop_interval(), with the clock reading and the period passed
 * in. Returns the connection interval it would report.
 */
static uint16_t a_packet_at(struct estimator *e, uint32_t now, uint32_t period)
{
	uint32_t cur_clk = now;
	uint32_t measured, clk_diff, rounded;
	uint16_t obsv_hop_interval;

	if (!e->seen_a_packet) {
		e->seen_a_packet = 1;
		e->prev_clk = cur_clk;
		return e->conn_interval;
	}

	measured = cur_clk;
	if (measured < e->prev_clk)
		measured += period;
	clk_diff = measured - e->prev_clk;

	/* probably consecutive data packets on the same channel */
	if (clk_diff < 2u * (uint32_t)s.le_baseclk)
		return e->conn_interval;

	if (clk_diff < e->smallest_hop_interval)
		e->smallest_hop_interval = clk_diff;

	/* DIVIDE_ROUND() has no outer parentheses, so its result is taken in a
	 * variable first: a cast written in front of it would bind to the
	 * numerator and not to the quotient. */
	rounded = DIVIDE_ROUND(e->smallest_hop_interval,
	                       LE_CHANNELS * (uint32_t)s.le_baseclk);
	obsv_hop_interval = (uint16_t)rounded;
	if (e->conn_interval == obsv_hop_interval) {
		++e->consec_intervals;
	} else {
		e->conn_interval = obsv_hop_interval;
		e->consec_intervals = 0;
	}

	e->prev_clk = cur_clk;
	return e->conn_interval;
}

/* How long a connection of this interval takes to come back to one channel. */
static uint32_t round_the_channels(int conn_interval)
{
	return (uint32_t)LE_CHANNELS * (uint32_t)conn_interval *
	       (uint32_t)s.le_baseclk;
}

/*
 * The period the firmware adds in the estimator, as the source has it. Zero if
 * the scan never found the site, which the first test reports; returning a
 * working period there would let the rest of the tests pass on nothing.
 */
static uint32_t firmware_period(void)
{
	if (s.interval_site < 0)
		return 0;
	return token_value(s.sites[s.interval_site].token);
}

/* The transposed literal the estimator used to add. */
#define TRANSPOSED_LITERAL 3267800000u

/* ------------------------------------------------------------------------
 * The tests
 * ------------------------------------------------------------------------ */

MU_TEST(test_the_sources_were_scanned)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: found %d rollover additions, expected %d -- the scan "
	         "matched nothing, or a site was added without updating this "
	         "test", path, s.site_count, ROLLOVER_SITES);
	mu_assert(s.site_count == ROLLOVER_SITES, msg);

	snprintf(msg, sizeof(msg),
	         "%s: no rollover addition inside "
	         "promisc_recover_hop_interval(), so the model below would run "
	         "on a neighbouring site's constant", path);
	mu_assert(s.interval_site >= 0, msg);

	snprintf(msg, sizeof(msg), "%s: LE_BASECLK not found", clock_path);
	mu_assert(s.le_baseclk > 0, msg);
	mu_assert_int_eq(12500, s.le_baseclk);
}

/* Every rollover site adds the named period. A literal is how a transposed
 * digit got in, and how it stayed: the neighbouring sites were already right,
 * so nothing compared them. */
MU_TEST(test_every_rollover_site_adds_the_named_period)
{
	char msg[512];
	int i;

	for (i = 0; i < s.site_count; i++) {
		snprintf(msg, sizeof(msg),
		         "%s:%d: `%s += %s;` undoes a clock rollover with a "
		         "literal; the period is CLK100NS_PERIOD", path,
		         s.sites[i].line, s.sites[i].target, s.sites[i].token);
		mu_assert(strcmp(s.sites[i].token, "CLK100NS_PERIOD") == 0, msg);
	}
}

/* ...and the named period is the clock's: 3125 ticks per CLKN tick over the
 * low 20 bits of CLKN. */
MU_TEST(test_the_period_is_the_clocks_own)
{
	mu_assert_int_eq(3125, CLK100NS_PER_CLKN);
	mu_assert(CLK100NS_PERIOD == 3276800000u,
	          "the capture clock wraps every 3125 * 2^20 ticks");
	mu_assert(CLK100NS_PERIOD - TRANSPOSED_LITERAL == 9000000u,
	          "the transposed literal was 9,000,000 ticks -- 0.9 s -- short");
}

/*
 * A connection event seen either side of the rollover must come out as the gap
 * that really passed. A 20-unit interval takes 37 * 20 * 1.25 ms = 9,250,000
 * ticks to come back to one channel; put the two sightings 100,000 ticks
 * before and after the wrap.
 */
MU_TEST(test_a_gap_across_the_rollover_is_the_gap_that_passed)
{
	struct estimator e;
	uint32_t period = firmware_period();
	uint32_t gap = round_the_channels(20);
	uint32_t before = CLK100NS_PERIOD - 100000u;
	uint32_t after = before + gap - CLK100NS_PERIOD;

	following_since(&e, before);
	mu_assert_int_eq(20, a_packet_at(&e, after, period));
	mu_assert(e.smallest_hop_interval == gap,
	          "the smallest gap seen is the 9,250,000 ticks that passed");

	/* Ordinary sightings after it agree, so the reported interval holds and
	 * the mode moves on to recovering the hop increment. */
	{
		uint32_t at = after;
		int i;

		for (i = 0; i < 5; i++) {
			at += gap;
			mu_assert_int_eq(20, a_packet_at(&e, at, period));
		}
		mu_assert_int_eq(5, e.consec_intervals);
	}
}

/*
 * What the transposed literal did with the same two sightings: the gap comes
 * out 9,000,000 ticks short, which for any interval of 20 units or more is a
 * small positive number. The estimator keeps the smallest gap it has seen, so
 * nothing later can replace it: the wrong interval is reported for the rest of
 * the run.
 */
MU_TEST(test_the_short_period_latched_a_wrong_interval_forever)
{
	struct estimator e;
	uint32_t gap = round_the_channels(20);
	uint32_t before = CLK100NS_PERIOD - 100000u;
	uint32_t after = before + gap - CLK100NS_PERIOD;
	uint32_t at = after;
	int i;

	following_since(&e, before);
	mu_assert_int_eq(1, a_packet_at(&e, after, TRANSPOSED_LITERAL));
	mu_assert(e.smallest_hop_interval == gap - 9000000u,
	          "the gap came out 0.9 s short: 250,000 ticks instead of "
	          "9,250,000");

	/* Hundreds of correct sightings afterwards cannot shift it. */
	for (i = 0; i < 500; i++) {
		at += gap;
		mu_assert_int_eq(1, a_packet_at(&e, at, TRANSPOSED_LITERAL));
	}
}

/*
 * The issue's own worked example, for the record: a previous stamp of
 * 3,276,700,000 and a new one of 100,000, a true gap of 200,000 ticks. The
 * short period turns that into 4,286,167,296 by wrapping an unsigned
 * subtraction, which beats the starting 0xffffffff and is reported as 9,267.
 * That one does recover -- the next ordinary sighting is smaller, so it
 * replaces the minimum -- which is why the case above, where the short gap
 * stays the smallest, is the one that matters.
 */
MU_TEST(test_a_gap_shorter_than_the_shortfall_wraps_instead)
{
	struct estimator e;

	following_since(&e, 3276700000u);
	mu_assert_int_eq(9267, a_packet_at(&e, 100000u, TRANSPOSED_LITERAL));
	mu_assert(e.smallest_hop_interval == 4286167296u,
	          "the unsigned subtraction wrapped");

	/* With the period the firmware now uses, the same two stamps are the
	 * 200,000 ticks that passed. */
	following_since(&e, 3276700000u);
	a_packet_at(&e, 100000u, firmware_period());
	mu_assert(e.smallest_hop_interval == 200000u,
	          "the gap across the wrap is the gap that passed");
}

/* A clock that has not wrapped is left alone, whatever the period is: the
 * addition only runs when the new stamp is behind the old one. */
MU_TEST(test_an_ordinary_gap_is_untouched)
{
	struct estimator e;
	uint32_t gap = round_the_channels(16);
	uint32_t at = 1000000u;

	following_since(&e, at);
	at += gap;
	mu_assert_int_eq(16, a_packet_at(&e, at, firmware_period()));
	mu_assert(e.smallest_hop_interval == gap, "the gap is the gap");
}

/*
 * Two packets of one connection event arrive together on the channel being
 * listened to, so a gap under two connection-interval units is not a trip
 * round the channels and is ignored. It must not become the minimum.
 */
MU_TEST(test_packets_from_one_connection_event_are_ignored)
{
	struct estimator e;
	uint32_t gap = round_the_channels(16);
	uint32_t at = 1000000u;

	following_since(&e, at);
	at += gap;
	a_packet_at(&e, at, firmware_period());

	/* A second packet 1 ms later: same connection event. */
	at += 10000u;
	mu_assert_int_eq(16, a_packet_at(&e, at, firmware_period()));
	mu_assert(e.smallest_hop_interval == gap,
	          "a gap within one connection event is not a hop interval");
}

/*
 * The stamp is part of the mode's state, so reset_le_promisc() clears it when
 * a pass starts. It used to be a static inside the estimator, which nothing
 * cleared.
 */
MU_TEST(test_the_stamp_belongs_to_the_mode_not_to_the_function)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: promisc_recover_hop_interval() keeps its last sighting in "
	         "a static, which no pass clears", path);
	mu_assert(!s.static_in_function, msg);

	snprintf(msg, sizeof(msg),
	         "%s: le_promisc_state_t has no prev_clk, so the stamp is not "
	         "part of the state reset_le_promisc() clears", path);
	mu_assert(s.stamp_in_state, msg);

	snprintf(msg, sizeof(msg),
	         "%s: le_promisc_state_t has no seen_a_packet, so a cleared "
	         "stamp cannot be told from a sighting at clock zero", path);
	mu_assert(s.flag_in_state, msg);

	snprintf(msg, sizeof(msg),
	         "%s: bt_promisc_le() does not call reset_le_promisc(), so the "
	         "state the stamp lives in is not cleared at a pass", path);
	mu_assert(s.reset_in_promisc_loop, msg);

	snprintf(msg, sizeof(msg),
	         "%s: reset_le_promisc() does not wipe the whole of "
	         "le_promisc, so a member added to it is not cleared by being "
	         "there", path);
	mu_assert(s.reset_wipes_whole_state, msg);

	snprintf(msg, sizeof(msg),
	         "%s: promisc_recover_hop_increment() keeps the sighting it "
	         "counts from in a static, the fault this change removed from "
	         "the estimator beside it", path);
	mu_assert(!s.increment_static_in_function, msg);

	snprintf(msg, sizeof(msg),
	         "%s: le_promisc_state_t has no first_ts, so the hop "
	         "increment's stamp is not cleared at a pass either", path);
	mu_assert(s.increment_stamp_in_state, msg);
}

/*
 * The two moves a model of the estimator cannot see, because a model that did
 * not make them would not be a model of this code: the first packet of a pass
 * returns instead of measuring, and what a sighting leaves behind is the clock
 * reading. Undo either in the firmware and the models above still pass.
 */
MU_TEST(test_the_estimator_itself_guards_and_stores_a_reading)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: promisc_recover_hop_interval() does not return on the "
	         "first packet of a pass, so it measures a gap from a cleared "
	         "stamp -- the whole time since the clock restarted", path);
	mu_assert(s.guard_returns_on_first_packet, msg);

	snprintf(msg, sizeof(msg),
	         "%s: promisc_recover_hop_interval() does not store the clock "
	         "reading; a stamp with a rollover undone on it is above the "
	         "clock's range, so the next gap is measured from a time the "
	         "clock never shows", path);
	mu_assert(s.stores_the_reading, msg);
}

/*
 * The access address is announced once, from inside bt_le_sync(). Announcing
 * it before that call loses the record to the queue reset on the way in, which
 * is why the line was missing; announcing it in both places would print it
 * twice for a record that escaped the earlier one before the reset.
 */
MU_TEST(test_the_access_address_is_announced_once_and_late_enough)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: the access address is announced at %d places, not one: "
	         "a record queued before bt_le_sync() is dropped by its queue "
	         "reset, and one that escapes first is then reported twice",
	         path, s.announce_sites);
	mu_assert(s.announce_sites == 1, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the access address is not announced from bt_le_sync(), "
	         "so it is queued before the queue the host reads exists", path);
	mu_assert(s.announce_in_sync, msg);

	snprintf(msg, sizeof(msg),
	         "%s: bt_le_sync() has no usb_queue_init(), so there is no "
	         "queue reset for the announcement to be placed after", path);
	mu_assert(s.queue_init_line > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the access address is announced at line %d, before the "
	         "queue reset at line %d: the record goes into a queue that "
	         "reset then empties, which is the whole of the fault",
	         path, s.announce_line, s.queue_init_line);
	mu_assert(s.announce_line > s.queue_init_line, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the announcement is not guarded by the mode, so -f and "
	         "-n would report an access address as a promiscuous-mode "
	         "state line", path);
	mu_assert(s.announce_guarded_by_mode, msg);
}

/*
 * A pass starts with nothing to measure from. Its first packet only records
 * when it arrived; the gap back to the pass before it is not a trip round the
 * channels, and the estimator keeps the smallest gap it is given, so a short
 * one would be reported for the rest of the pass.
 */
MU_TEST(test_a_pass_does_not_measure_from_the_pass_before_it)
{
	struct estimator e;
	uint32_t period = firmware_period();
	uint32_t gap = round_the_channels(40);
	uint32_t at;
	int i;

	/* The pass before it saw a packet, and then 40 ms passed: the mode
	 * stopped and started again, which is the gap the estimator must not
	 * take. 40 ms is far shorter than the 1.85 s a 40-unit connection
	 * takes to come back, and well over the two units that mark two
	 * packets of one connection event. */
	estimator_init(&e);
	at = 1000000u;
	a_packet_at(&e, at, period);
	mu_assert(e.smallest_hop_interval == 0xffffffff,
	          "the first packet of a pass is not the end of a gap");

	at += 400000u;
	mu_assert_int_eq(1, a_packet_at(&e, at, period));
	mu_assert(e.smallest_hop_interval == 400000u,
	          "the 40 ms is measured, because this pass saw both packets");

	/* The same 40 ms across a restart: the estimator is cleared, so the
	 * sightings that follow give the connection's own interval. */
	estimator_init(&e);
	at += 400000u;
	a_packet_at(&e, at, period);
	for (i = 0; i < 6; i++) {
		at += gap;
		mu_assert_int_eq(40, a_packet_at(&e, at, period));
	}
	mu_assert_int_eq(5, e.consec_intervals);
}

/*
 * The stamp a sighting leaves is a clock reading. Undoing a rollover produces
 * a number above the clock's range, and storing that would measure the next
 * gap from a time the clock never shows -- one spuriously large gap per wrap.
 */
MU_TEST(test_the_stamp_left_behind_is_a_clock_reading)
{
	struct estimator e;
	uint32_t period = firmware_period();
	uint32_t gap = round_the_channels(20);
	uint32_t before = CLK100NS_PERIOD - 100000u;
	uint32_t after = before + gap - CLK100NS_PERIOD;

	following_since(&e, before);
	a_packet_at(&e, after, period);
	mu_assert(e.prev_clk == after,
	          "the stamp is the reading, not the reading plus a period");
	mu_assert(e.prev_clk < CLK100NS_PERIOD,
	          "a stamp above the clock's range is a time the clock never "
	          "shows");

	/* So the next sighting is the gap that passed, not one inflated by a
	 * period. */
	a_packet_at(&e, after + gap, period);
	mu_assert(e.smallest_hop_interval == gap,
	          "the gap after the wrap is the gap that passed");
}

int main(void)
{
	if (!scan_sources(&s)) {
		printf("could not read %s or %s\n", path, clock_path);
		return 1;
	}

	printf("low-energy promiscuous connection-interval tests\n");
	MU_RUN(test_the_sources_were_scanned);
	MU_RUN(test_every_rollover_site_adds_the_named_period);
	MU_RUN(test_the_period_is_the_clocks_own);
	MU_RUN(test_a_gap_across_the_rollover_is_the_gap_that_passed);
	MU_RUN(test_the_short_period_latched_a_wrong_interval_forever);
	MU_RUN(test_a_gap_shorter_than_the_shortfall_wraps_instead);
	MU_RUN(test_an_ordinary_gap_is_untouched);
	MU_RUN(test_packets_from_one_connection_event_are_ignored);
	MU_RUN(test_the_stamp_belongs_to_the_mode_not_to_the_function);
	MU_RUN(test_the_estimator_itself_guards_and_stores_a_reading);
	MU_RUN(test_the_access_address_is_announced_once_and_late_enough);
	MU_RUN(test_a_pass_does_not_measure_from_the_pass_before_it);
	MU_RUN(test_the_stamp_left_behind_is_a_clock_reading);
	return mu_report();
}
