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
	/* The estimator's own two numbers, so the model below cannot drift
	 * from the firmware without this test saying so: how many gaps it
	 * keeps, and how many places le_gap_tolerance() shifts a span down
	 * by. */
	int gaps_kept;
	int tolerance_shift;
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
	/* Whether the hop increment checks the interval it is about to divide
	 * by. Nothing else protects that division: with DIV_0_TRP clear, UDIV
	 * answers 0 rather than faulting, and hop_interval_lut[0] is 0 -- a
	 * hop increment that stops the board hopping. */
	int increment_guards_the_interval;
	int increment_guards_no_hops;
	/* The estimator's gap ring, which has to be part of the mode's state
	 * so that a pass starts with nothing in it. */
	int gap_table_in_state;
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
/* le_gap_tolerance() and le_same_span() are arithmetic both of the mode's
 * estimators need, so they live in bluetooth_le.c with the rest of the
 * low-energy arithmetic rather than in the hardware file. */
static const char *arith_path = FW_RXTX_DIR "/bluetooth_le.c";

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
			if (contains_identifier(line, "gap_count"))
				out->gap_table_in_state = 1;
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

		if (strcmp(fn, "promisc_recover_hop_increment") == 0 &&
		    strstr(line, "le.conn_interval <") != NULL &&
		    strstr(line, "LE_MIN_CONN_INTERVAL") != NULL)
			out->increment_guards_the_interval = 1;

		if (strcmp(fn, "promisc_recover_hop_increment") == 0 &&
		    strstr(line, "channels_hopped >= 1") != NULL)
			out->increment_guards_no_hops = 1;

		{
			int value;

			if (sscanf(line, " #define LE_PROMISC_GAPS %d",
			           &value) == 1)
				out->gaps_kept = value;
		}

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

	/* le_gap_tolerance(): how far two readings of one span may be apart. */
	f = fopen(arith_path, "r");
	if (f == NULL)
		return 0;
	in_block = 0;
	fn[0] = '\0';
	while (fgets(line, sizeof(line), f) != NULL) {
		int value;

		strip_comments(line, &in_block);

		/* Whose body this line is, as in the loop above. */
		if (line[0] != '\0' && !isspace((unsigned char)line[0]) &&
		    line[0] != '#' && line[0] != '}' &&
		    strchr(line, ';') == NULL &&
		    call_name(line, name, sizeof(name)))
			snprintf(fn, sizeof(fn), "%s", name);

		if (strcmp(fn, "le_gap_tolerance") == 0 &&
		    sscanf(line, " return (span >> %d)", &value) == 1)
			out->tolerance_shift = value;
	}
	fclose(f);
	return 1;
}

/* ------------------------------------------------------------------------
 * The estimator, modelled
 * ------------------------------------------------------------------------ */

/* LE_DIVIDE_ROUND(), from bluetooth_le.h. */
#define DIVIDE_ROUND(N, D) (((N) + (D)/2) / (D))

/* As many gaps as the estimator could ever keep. The number it does keep is
 * read out of the firmware source, so a change there is a test failure rather
 * than a model that has quietly drifted. */
#define LE_PROMISC_GAPS 16

/* The range a connection interval may take, in units of 1.25 ms, from
 * bluetooth_le.h. */
#define LE_MIN_CONN_INTERVAL 6

/* LE_SAME_EVENT_GAP, as bluetooth_le.h has it: the longest two packets of one
 * connection event can be apart, which is what the estimator tells a
 * continuation from the first packet of the next event by. */
#define LE_SAME_EVENT_GAP 25000u
#define LE_MAX_CONN_INTERVAL 3200

struct estimator {
	uint32_t prev_clk;
	/* When any packet was last seen, as against prev_clk, which is the
	 * last one taken as the start of a connection event. */
	uint32_t last_clk;
	int seen_a_packet;
	/* The gap lengths seen and how often each has been seen. */
	uint32_t gap[LE_PROMISC_GAPS];
	int gap_count;
	uint16_t conn_interval;
	int consec_intervals;
};

/* reset_le_promisc(), which clears the whole of the mode's state. */
static void estimator_init(struct estimator *e)
{
	memset(e, 0, sizeof(*e));
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
	e->last_clk = last;
	e->seen_a_packet = 1;
}

/* LE_PROMISC_MAX_GAP: the longest trip round the channels. */
#define LONGEST_GAP ((uint32_t)LE_CHANNELS * LE_MAX_CONN_INTERVAL * 12500u)

/* le_gap_tolerance(): how far two readings of one span may be apart. */
static uint32_t gap_tolerance(uint32_t span)
{
	return (span >> s.tolerance_shift) + (uint32_t)s.le_baseclk / 2;
}

static int same_span(uint32_t a, uint32_t b)
{
	uint32_t shorter = a < b ? a : b;
	uint32_t apart = a < b ? b - a : a - b;

	return apart <= gap_tolerance(shorter);
}

/* promisc_note_gap(). */
static void note_gap(struct estimator *e, uint32_t gap)
{
	int i;

	if (e->gap_count < s.gaps_kept) {
		e->gap[e->gap_count++] = gap;
		return;
	}
	for (i = 0; i + 1 < s.gaps_kept; ++i)
		e->gap[i] = e->gap[i + 1];
	e->gap[s.gaps_kept - 1] = gap;
}

/* How many of the gaps in the ring are this span. */
static int seen_count(const struct estimator *e, uint32_t span)
{
	int i, count = 0;

	for (i = 0; i < e->gap_count; ++i)
		if (same_span(e->gap[i], span))
			count++;
	return count;
}

static uint32_t gap_back(const struct estimator *e, unsigned back)
{
	return e->gap[e->gap_count - 1 - back];
}

/* promisc_interval(). */
static uint16_t interval_from_gaps(const struct estimator *e)
{
	unsigned period, i;

	for (period = 1; period <= (unsigned)s.gaps_kept / 2; ++period) {
		uint32_t total = 0, interval, unit;

		if ((unsigned)e->gap_count < 2 * period)
			break;

		for (i = 0; i < period; ++i)
			if (!same_span(gap_back(e, i), gap_back(e, i + period)))
				break;
		if (i < period)
			continue;

		for (i = 0; i < period; ++i)
			total += gap_back(e, i);

		if (total > LONGEST_GAP || total < gap_back(e, 0))
			continue;

		interval = DIVIDE_ROUND(total,
		                        LE_CHANNELS * (uint32_t)s.le_baseclk);
		if (interval < LE_MIN_CONN_INTERVAL ||
		    interval > LE_MAX_CONN_INTERVAL)
			return 0;

		unit = interval * (uint32_t)s.le_baseclk;
		for (i = 0; i < period; ++i) {
			uint32_t gap = gap_back(e, i);
			uint32_t events = DIVIDE_ROUND(gap, unit);

			if (events < 1 || events > LE_CHANNELS)
				return 0;
			if (!same_span(gap, events * unit))
				return 0;
		}

		return (uint16_t)interval;
	}
	return 0;
}

/*
 * promisc_recover_hop_interval(), with the clock reading and the period passed
 * in. Returns the connection interval it would report, 0 for none.
 */
static uint16_t a_packet_at(struct estimator *e, uint32_t now, uint32_t period)
{
	uint32_t cur_clk = now;
	uint32_t measured, clk_diff;
	uint16_t obsv_hop_interval;

	if (!e->seen_a_packet) {
		e->seen_a_packet = 1;
		e->prev_clk = cur_clk;
		e->last_clk = cur_clk;
		return e->conn_interval;
	}

	measured = cur_clk;
	if (measured < e->prev_clk)
		measured += period;
	clk_diff = measured - e->prev_clk;

	/* Closer to the packet before it than two packets of one event can be,
	 * so it is a later packet of the event now running rather than the
	 * start of the next one. Measured from the previous packet, whatever
	 * it was, so that a busy event does not start an event of its own part
	 * way through. */
	if (measured - e->last_clk < LE_SAME_EVENT_GAP) {
		e->last_clk = cur_clk;
		return e->conn_interval;
	}

	e->last_clk = cur_clk;
	e->prev_clk = cur_clk;

	if (clk_diff > (uint32_t)LE_CHANNELS * LE_MAX_CONN_INTERVAL *
	               (uint32_t)s.le_baseclk)
		return e->conn_interval;

	note_gap(e, clk_diff);
	obsv_hop_interval = interval_from_gaps(e);

	if (obsv_hop_interval == 0) {
		e->conn_interval = 0;
		e->consec_intervals = 0;
		return 0;
	}

	if (e->conn_interval == obsv_hop_interval) {
		++e->consec_intervals;
	} else {
		e->conn_interval = obsv_hop_interval;
		e->consec_intervals = 0;
	}

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

/* A connection of this interval seen coming back `times` times in a row,
 * starting from `*at`, which is advanced to the last sighting. Returns what
 * the estimator reports at the end. */
static uint16_t trips(struct estimator *e, uint32_t *at, int conn_interval,
                      int times)
{
	uint32_t gap = round_the_channels(conn_interval);
	uint16_t reported = 0;
	int i;

	for (i = 0; i < times; i++) {
		*at += gap;
		reported = a_packet_at(e, *at, firmware_period());
	}
	return reported;
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

	snprintf(msg, sizeof(msg),
	         "%s: LE_PROMISC_GAPS not found, so the model below would keep "
	         "a different number of gaps from the firmware", path);
	mu_assert(s.gaps_kept >= 2, msg);
	snprintf(msg, sizeof(msg),
	         "%s: LE_PROMISC_GAPS is %d, more than this test allows for",
	         path, s.gaps_kept);
	mu_assert(s.gaps_kept <= LE_PROMISC_GAPS, msg);

	snprintf(msg, sizeof(msg),
	         "%s: le_gap_tolerance() does not shift the span, so the model "
	         "below would allow a different spread", arith_path);
	mu_assert(s.tolerance_shift > 0, msg);
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
	uint32_t at = after;

	following_since(&e, before);
	a_packet_at(&e, after, period);
	mu_assert_int_eq(1, seen_count(&e, gap));
	mu_assert(e.gap[0] == gap,
	          "the gap recorded is the 9,250,000 ticks that passed");

	/* A second return repeats the pattern, and the interval comes out as
	 * the connection's own. */
	mu_assert_int_eq(20, trips(&e, &at, 20, 1));

	/* Sightings after that agree, so the mode moves on to recovering the
	 * hop increment. */
	mu_assert_int_eq(20, trips(&e, &at, 20, 5));
	mu_assert_int_eq(5, e.consec_intervals);
}

/*
 * What the transposed literal did with the same two sightings: the gap comes
 * out 9,000,000 ticks short, which for a 20-unit interval is 250,000 ticks --
 * 25 ms, exactly a thirty-seventh of the real one. The estimator used to keep
 * the smallest gap it had seen, so that one was reported as a 1-unit interval
 * for the rest of the run however many correct sightings followed.
 *
 * With the period the firmware now adds this cannot arise, and the plausible
 * range is what would stop it: 25 ms is far short of the 277.5 ms a trip round
 * 37 channels takes at the shortest interval, so no interval is reported from
 * it at all.
 */
MU_TEST(test_a_gap_too_short_for_a_trip_round_the_channels_reports_nothing)
{
	struct estimator e;
	uint32_t gap = round_the_channels(20);
	uint32_t before = CLK100NS_PERIOD - 100000u;
	uint32_t after = before + gap - CLK100NS_PERIOD;
	uint32_t short_gap = gap - 9000000u; /* 250,000 ticks: 25 ms */
	uint32_t at = after;
	int i;

	following_since(&e, before);
	mu_assert_int_eq(0, a_packet_at(&e, after, TRANSPOSED_LITERAL));
	mu_assert(e.gap[0] == short_gap,
	          "the gap came out 0.9 s short: 250,000 ticks instead of "
	          "9,250,000");

	/* However often a 25 ms gap recurs it is not a trip round the
	 * channels, so there is no interval to take from it. */
	for (i = 0; i < 20; i++) {
		at += short_gap;
		mu_assert_int_eq(0, a_packet_at(&e, at, firmware_period()));
	}
	mu_assert_int_eq(s.gaps_kept, seen_count(&e, short_gap));
}

/*
 * The issue's own worked example, for the record: a previous stamp of
 * 3,276,700,000 and a new one of 100,000, a true gap of 200,000 ticks. The
 * short period turns that into 4,286,167,296 by wrapping an unsigned
 * subtraction, which used to beat the starting 0xffffffff and be reported as
 * 9,267 units. A gap that long is now above anything a trip round the channels
 * can take -- 148 s -- so it is dropped.
 */
MU_TEST(test_a_gap_shorter_than_the_shortfall_wraps_instead)
{
	struct estimator e;

	following_since(&e, 3276700000u);
	mu_assert_int_eq(0, a_packet_at(&e, 100000u, TRANSPOSED_LITERAL));
	mu_assert_int_eq(0, seen_count(&e, 4286167296u));

	/* With the period the firmware now uses, the same two stamps are the
	 * 200,000 ticks that passed. */
	following_since(&e, 3276700000u);
	a_packet_at(&e, 100000u, firmware_period());
	mu_assert(e.gap[0] == 200000u,
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
	mu_assert_int_eq(16, trips(&e, &at, 16, 2));
	mu_assert(e.gap[0] == gap, "the gap is the gap");
	mu_assert_int_eq(2, seen_count(&e, gap));
}

/*
 * The central's packet and the peripheral's answer arrive together on the
 * channel being listened to, and a long exchange can put a second pair in the
 * same connection event. A gap shorter than the shortest connection interval
 * cannot span two events, so it is not a visit to this channel: it is not
 * recorded, and it does not become the sighting the next gap is measured
 * from either.
 */
MU_TEST(test_packets_from_one_connection_event_are_ignored)
{
	struct estimator e;
	uint32_t gap = round_the_channels(16);
	uint32_t at = 1000000u;

	following_since(&e, at);
	trips(&e, &at, 16, 2);
	mu_assert_int_eq(2, seen_count(&e, gap));

	/* The answer 1 ms later, and a second exchange 2.2 ms after that: the
	 * same connection event both times. */
	at += 10000u;
	mu_assert_int_eq(16, a_packet_at(&e, at, firmware_period()));
	at += 22000u;
	mu_assert_int_eq(16, a_packet_at(&e, at, firmware_period()));
	mu_assert_int_eq(0, seen_count(&e, 10000u));
	mu_assert_int_eq(0, seen_count(&e, 32000u));

	/* The next event's packet is a trip measured from the first packet of
	 * the event before it, not from the extras inside it. */
	at -= 32000u;
	mu_assert_int_eq(16, trips(&e, &at, 16, 1));
	mu_assert_int_eq(3, seen_count(&e, gap));
}

/*
 * A busy connection event runs longer than the shortest connection interval,
 * and none of it is a visit to this channel either.
 *
 * Two exchanges of full-length packets on the 1M PHY take about 9 ms, against
 * the 7.5 ms the shortest interval allows, so measuring the silence from the
 * event's start rather than from the packet before would make the third packet
 * of such an event the start of an event of its own -- 9 ms into the real one.
 * The run of gaps would then hold spans that are not a whole number of
 * connection intervals, and the follow the algorithm-2 search sets up would be
 * anchored that far into an event.
 *
 * So the silence is measured from the previous packet, whatever it was, and a
 * run of packets no more than one interval apart is one event however long it
 * goes on.
 */
MU_TEST(test_a_busy_event_is_still_one_event)
{
	struct estimator e;
	uint32_t gap = round_the_channels(40);   /* a 50 ms connection */
	uint32_t at = 1000000u;
	uint32_t start;
	int i;

	following_since(&e, at);
	trips(&e, &at, 40, 2);
	mu_assert_int_eq(2, seen_count(&e, gap));

	/* Eight full-length packets back to back, each 2.25 ms after the one
	 * before it, so the event runs 18 ms: far longer than the shortest
	 * connection interval, and well inside this connection's own 50 ms. */
	start = at;
	for (i = 1; i <= 8; i++) {
		at = start + i * 22500u;
		mu_assert_int_eq(40, a_packet_at(&e, at, firmware_period()));
	}
	mu_assert_int_eq(0, seen_count(&e, 22500u));
	mu_assert_int_eq(0, seen_count(&e, 90000u));
	mu_assert_int_eq(2, e.gap_count);

	/* The next visit is a trip measured from the first packet of the busy
	 * event, not from where it ran to. */
	at = start + gap;
	mu_assert_int_eq(40, a_packet_at(&e, at, firmware_period()));
	mu_assert_int_eq(3, seen_count(&e, gap));
}

/*
 * Two connection events in a row on this channel, at the shortest interval a
 * connection may use. A reduced channel map puts them there: it moves the
 * events of the channels it dropped onto the channels it kept, so one channel
 * can come up twice running.
 *
 * Then the answer of the first event and the first packet of the second are
 * less than one connection interval apart, and the second still has to be a
 * sighting. This is why the threshold is the longest two packets of one event
 * can be apart and not one connection interval: measured from the previous
 * packet against a threshold of one interval, the second event's packet would
 * read as a continuation of the first, the chain would never end, and nothing
 * would be recorded again.
 *
 * Two offsets for the answer: just after its event's start, as an empty PDU
 * gives, and 2.34 ms in, as an answer to a full-length packet gives.
 */
MU_TEST(test_two_events_in_a_row_are_two_sightings)
{
	static const uint32_t answer_at[2] = { 3000u, 23400u };
	uint32_t unit = LE_MIN_CONN_INTERVAL * 12500u;
	unsigned k;

	for (k = 0; k < 2; k++) {
		struct estimator e;
		uint32_t at = 1000000u;
		char msg[160];

		following_since(&e, at);

		/* The first event and its answer. */
		at += unit;
		a_packet_at(&e, at, firmware_period());
		a_packet_at(&e, at + answer_at[k], firmware_period());
		mu_assert_int_eq(1, e.gap_count);

		/* The next event, one interval later, and its answer. */
		at += unit;
		a_packet_at(&e, at, firmware_period());
		a_packet_at(&e, at + answer_at[k], firmware_period());

		snprintf(msg, sizeof(msg),
		         "an answer %lu ticks into its event: %d gaps from two "
		         "events in a row", (unsigned long)answer_at[k],
		         e.gap_count);
		mu_assert(e.gap_count == 2, msg);
		snprintf(msg, sizeof(msg),
		         "an answer %lu ticks into its event: %d of the gaps "
		         "are one connection interval",
		         (unsigned long)answer_at[k], seen_count(&e, unit));
		mu_assert(seen_count(&e, unit) == 2, msg);
	}
}

/*
 * A connection whose channel map is not the full 37 comes back to this channel
 * several times a trip, at uneven spacings, so dividing by 37 flat would put
 * the interval out by whatever that spacing was. The gaps are still whole
 * numbers of connection intervals, and with the map reduced they are different
 * whole numbers, so the longest span that measures all of them is the interval
 * itself.
 */
MU_TEST(test_a_reduced_channel_map_still_gives_the_interval)
{
	struct estimator e;
	uint32_t period = firmware_period();
	uint32_t interval = 24u * (uint32_t)s.le_baseclk; /* 30 ms */
	uint32_t at = 1000000u;
	/* Three of the spacings such a map produces, 11, 7 and 19 events,
	 * which add up to one trip round the 37 positions. */
	int spacing[] = { 11, 7, 19, 11, 7, 19 };
	unsigned i;

	following_since(&e, at);
	for (i = 0; i < sizeof(spacing) / sizeof(spacing[0]); i++) {
		at += (uint32_t)spacing[i] * interval;
		a_packet_at(&e, at, period);
	}

	mu_assert_int_eq(24, e.conn_interval);

	/* Dividing the shortest of those gaps by 37 flat, as the arithmetic
	 * used to, gives 7 * 30 ms / 37 -- below the shortest interval a
	 * connection may use, so it says nothing at all about this link. */
	mu_assert(DIVIDE_ROUND(7u * interval,
	                       LE_CHANNELS * (uint32_t)s.le_baseclk)
	          < LE_MIN_CONN_INTERVAL,
	          "dividing a part-trip gap by 37 lands below the shortest "
	          "interval a connection may use");
}

/*
 * Two of a reduced map's spacings in a row can be the same, and a run of a
 * longer pattern repeats on its own: the last two of 11, 11, 15 are 15, 11
 * twice over from the trip before. Only the whole pattern totals 37
 * connection events, which is what tells the part from the whole.
 */
MU_TEST(test_part_of_a_pattern_is_not_the_pattern)
{
	struct estimator e;
	uint32_t period_ticks = firmware_period();
	uint32_t interval = 24u * (uint32_t)s.le_baseclk; /* 30 ms */
	uint32_t at = 1000000u;
	int spacing[] = { 11, 11, 15, 11, 11, 15, 11, 11, 15 };
	unsigned i;

	following_since(&e, at);
	for (i = 0; i < sizeof(spacing) / sizeof(spacing[0]); i++) {
		at += (uint32_t)spacing[i] * interval;
		a_packet_at(&e, at, period_ticks);
	}
	mu_assert_int_eq(24, e.conn_interval);
}

/*
 * A length seen once is not evidence: the radio allows two bit errors in the
 * sync word, so noise matches the access address a few times a minute on a
 * receiver that is always open. One such sighting used to become the smallest
 * gap and be reported for the rest of the pass.
 */
MU_TEST(test_a_single_odd_gap_is_not_the_answer)
{
	struct estimator e;
	uint32_t period = firmware_period();
	uint32_t gap = round_the_channels(24); /* 30 ms: 1.11 s a trip */
	uint32_t at = 1000000u;

	following_since(&e, at);
	mu_assert_int_eq(24, trips(&e, &at, 24, 3));

	/* A sighting 400 ms after the last one: not this connection, and well
	 * inside the plausible range for a gap. */
	at += 4000000u;
	mu_assert_int_eq(0, a_packet_at(&e, at, period));
	mu_assert_int_eq(1, seen_count(&e, 4000000u));

	/* The next real return is measured from that sighting, so that gap is
	 * short too: two gaps in a row are not this connection's and nothing
	 * is reported from them. */
	mu_assert_int_eq(0, trips(&e, &at, 24, 1));

	/* Once the pattern is back the interval is too. */
	mu_assert_int_eq(24, trips(&e, &at, 24, 2));
	mu_assert(seen_count(&e, gap) >= 2,
	          "the trip length is what keeps being seen");
}

/*
 * A figure the gaps no longer support is given up rather than kept. The old
 * estimator could not do this: its minimum never rose, so a value latched
 * once was reported until the pass ended.
 */
MU_TEST(test_a_reported_interval_is_given_up_when_the_gaps_change)
{
	struct estimator e;
	uint32_t at = 1000000u;
	int i;

	following_since(&e, at);
	mu_assert_int_eq(24, trips(&e, &at, 24, 4));

	/* The connection's interval is updated to 60 ms, so it comes back
	 * every 2.22 s instead of every 1.11 s. The old gaps fall off the end
	 * of the ring as the new ones arrive. */
	for (i = 0; i < 2 * LE_PROMISC_GAPS; i++)
		trips(&e, &at, 48, 1);
	mu_assert_int_eq(48, e.conn_interval);
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

	snprintf(msg, sizeof(msg),
	         "%s: le_promisc_state_t has no gap_count, so the gaps one pass "
	         "collected are still there for the next one to report from",
	         path);
	mu_assert(s.gap_table_in_state, msg);
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
	 * takes to come back, and well over the shortest connection interval
	 * that marks two packets of one connection event. */
	estimator_init(&e);
	at = 1000000u;
	a_packet_at(&e, at, period);
	mu_assert_int_eq(0, e.gap_count);
	mu_assert(e.gap[0] == 0,
	          "the first packet of a pass is not the end of a gap");

	at += 400000u;
	mu_assert_int_eq(0, a_packet_at(&e, at, period));
	mu_assert_int_eq(1, seen_count(&e, 400000u));

	/* The same 40 ms across a restart: the estimator is cleared, so the
	 * sightings that follow give the connection's own interval and nothing
	 * else. */
	estimator_init(&e);
	at += 400000u;
	a_packet_at(&e, at, period);
	for (i = 0; i < 9; i++) {
		at += gap;
		a_packet_at(&e, at, period);
	}
	mu_assert_int_eq(40, e.conn_interval);
	mu_assert_int_eq(0, seen_count(&e, 400000u));
	mu_assert(e.consec_intervals >= 5,
	          "five sightings running agreed, so the mode moves on to the "
	          "hop increment");
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
	mu_assert_int_eq(2, seen_count(&e, gap));
}

/*
 * The hop increment is counted by dividing a measured span by the connection
 * interval, so a zero interval there divides by zero. The LPC17xx leaves
 * DIV_0_TRP clear, so UDIV answers 0 instead of faulting: the board would take
 * hop_interval_lut[0], hop increment 0, and stop hopping. The estimator cannot
 * hand one over now, and the division refuses one anyway.
 */
MU_TEST(test_the_hop_increment_never_divides_by_a_zero_interval)
{
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "%s: promisc_recover_hop_increment() divides by "
	         "le.conn_interval without checking it against "
	         "LE_MIN_CONN_INTERVAL first", path);
	mu_assert(s.increment_guards_the_interval, msg);

	snprintf(msg, sizeof(msg),
	         "%s: promisc_recover_hop_increment() does not require at "
	         "least one connection event between the two sightings, so a "
	         "span shorter than half an interval rounds to none and takes "
	         "hop_interval_lut[0], which is 0", path);
	mu_assert(s.increment_guards_no_hops, msg);
}

/*
 * A return the board did not hear makes one gap the sum of two, and the
 * estimator must not take that sum for a trip round the channels. Nothing is
 * reported until the gaps repeat, and a run with a missed return in it does
 * not repeat.
 */
MU_TEST(test_a_missed_return_is_not_an_interval)
{
	struct estimator e;
	uint32_t period = firmware_period();
	uint32_t gap = round_the_channels(24);
	uint32_t at = 1000000u;
	int i;

	/* Returns heard unevenly, which is what an intermittent capture gives:
	 * the trips between them run 1, 3, 2, 5, 1, 4, 2, 7. No pattern
	 * repeats, so there is nothing to report. */
	int missed[] = { 1, 3, 2, 5, 1, 4, 2, 7 };

	estimator_init(&e);
	a_packet_at(&e, at, period);
	for (i = 0; i < (int)(sizeof(missed) / sizeof(missed[0])); i++) {
		at += (uint32_t)missed[i] * gap;
		mu_assert_int_eq(0, a_packet_at(&e, at, period));
	}

	/* What it cannot tell apart, for the record. A board that hears every
	 * return reports the connection's own interval; one that hears exactly
	 * every second return, every time, sees one spacing of two trips and
	 * reports twice the interval, because two trips of a 30 ms connection
	 * and one trip of a 60 ms one are the same span. Nothing in the gaps
	 * separates them, which is why the man page says so. A real capture
	 * loses returns unevenly, as above, and then reports nothing. */
	estimator_init(&e);
	a_packet_at(&e, at, period);
	for (i = 0; i < 6; i++) {
		at += 2 * gap;
		a_packet_at(&e, at, period);
	}
	mu_assert_int_eq(48, e.conn_interval);
}

int main(void)
{
	if (!scan_sources(&s)) {
		printf("could not read %s, %s or %s\n", path, clock_path,
		       arith_path);
		return 1;
	}

	printf("low-energy promiscuous connection-interval tests\n");
	MU_RUN(test_the_sources_were_scanned);
	MU_RUN(test_every_rollover_site_adds_the_named_period);
	MU_RUN(test_the_period_is_the_clocks_own);
	MU_RUN(test_a_gap_across_the_rollover_is_the_gap_that_passed);
	MU_RUN(test_a_gap_too_short_for_a_trip_round_the_channels_reports_nothing);
	MU_RUN(test_a_gap_shorter_than_the_shortfall_wraps_instead);
	MU_RUN(test_an_ordinary_gap_is_untouched);
	MU_RUN(test_packets_from_one_connection_event_are_ignored);
	MU_RUN(test_a_busy_event_is_still_one_event);
	MU_RUN(test_two_events_in_a_row_are_two_sightings);
	MU_RUN(test_a_reduced_channel_map_still_gives_the_interval);
	MU_RUN(test_part_of_a_pattern_is_not_the_pattern);
	MU_RUN(test_a_single_odd_gap_is_not_the_answer);
	MU_RUN(test_a_reported_interval_is_given_up_when_the_gaps_change);
	MU_RUN(test_the_hop_increment_never_divides_by_a_zero_interval);
	MU_RUN(test_a_missed_return_is_not_an_interval);
	MU_RUN(test_the_stamp_belongs_to_the_mode_not_to_the_function);
	MU_RUN(test_the_estimator_itself_guards_and_stores_a_reading);
	MU_RUN(test_the_access_address_is_announced_once_and_late_enough);
	MU_RUN(test_a_pass_does_not_measure_from_the_pass_before_it);
	MU_RUN(test_the_stamp_left_behind_is_a_clock_reading);
	return mu_report();
}
