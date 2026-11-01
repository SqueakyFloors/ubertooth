/*
 * Passive recovery of a connection that hops with channel selection
 * algorithm 2, from packets heard on one data channel.
 *
 * ubertooth-btle -p listens on one data channel, recovers the access address
 * and CRCInit of a connection already in progress, and then has to work out
 * where that connection will be next. Under algorithm 1 it times one hop and
 * looks the hop increment up. Algorithm 2 has no increment: the channel of a
 * connection event is a hash of the access address and the 16-bit event
 * counter, reduced to one of the channels the connection's map keeps. So
 * le_csa2_recover() finds the connection interval and the event counter
 * instead, from the times the connection was heard on the one channel.
 *
 * That search is pure arithmetic, so this runs the firmware's own code. It
 * builds the sightings a board would have collected -- for a given access
 * address, interval, event counter and channel map -- and checks the search
 * gets the interval and the counter back, including the awkward cases: a
 * channel map smaller than 37, which puts sightings on this channel that no
 * counter predicts; a clock running fast or slow; a visit nobody heard; and a
 * connection that was away for a while.
 *
 * It also checks the search declines to answer, because a wrong answer is
 * worse than none: the mode would follow a sequence no connection uses and
 * report a capture of nothing. So an algorithm-1 connection's sightings, and
 * random ones, have to come back empty.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "bluetooth_le.h"

#ifndef FW_RXTX_DIR
#error "FW_RXTX_DIR must name firmware/bluetooth_rxtx"
#endif

/*
 * The ARM `rbit` instruction, which the firmware's rbit() is one line of
 * inline assembly for. Nothing here calls it; bluetooth_le.c's dewhitening
 * does, so the link needs it.
 */
u32 rbit(u32 value)
{
	u32 out = 0;
	unsigned i;

	for (i = 0; i < 32; ++i)
		out |= ((value >> i) & 1u) << (31 - i);
	return out;
}

/* The data channel the promiscuous mode parks on, 2440 MHz. */
#define LISTEN_CHANNEL 17

/* A connection's access address, and a second one so that no result rests on
 * a property of one address. */
#define AA_A 0x8e89bed6u
#define AA_B 0x50655dabu

/* How many gaps a full run of sightings gives. */
#define GAPS (LE_CSA2_SIGHTINGS - 1)

/* ------------------------------------------------------------------------
 * What a board would have heard
 * ------------------------------------------------------------------------ */

struct sightings {
	u32 gap[GAPS];
	unsigned gap_count;
	uint16_t newest_counter;  /* the event counter of the last sighting */
};

/* A channel map with `used` channels in it, taken from the bottom up:
 * channels 0 to used-1 are kept and the rest dropped. LISTEN_CHANNEL has to
 * be among them, or the connection never comes to the channel being listened
 * to and there is nothing to recover. */
static void map_of(unsigned used, le_channel_remapping_t *remapping)
{
	uint8_t channel_map[5] = { 0, 0, 0, 0, 0 };
	unsigned i;

	for (i = 0; i < used; ++i)
		channel_map[i / 8] |= (uint8_t)(1u << (i % 8));
	le_parse_channel_map(channel_map, remapping);
}

static void full_map(le_channel_remapping_t *remapping)
{
	map_of(37, remapping);
}

/*
 * Run a connection past the listening channel and keep the gaps between the
 * last LE_CSA2_SIGHTINGS sightings, the way promisc_recover_hop_interval()
 * does.
 *
 * `interval` is in units of 1.25 ms and `first_counter` is where the
 * connection's event counter stands when the collecting starts. `ppm` bends
 * the connection's clock against the board's. `drop_nth` silences one
 * sighting in that many, standing for a packet the board did not hear; 0
 * silences none.
 */
static void collect_from(struct sightings *s, u32 access_address,
                         uint16_t interval, uint16_t first_counter,
                         le_channel_remapping_t *remapping, int ppm,
                         unsigned drop_nth, unsigned skip)
{
	unsigned event;
	unsigned seen = 0;
	double unit = (double)interval * LE_CONN_INTERVAL_TICKS *
			(1.0 + ppm / 1e6);
	/* Wider than the capture clock, which wraps every 327 s: the firmware
	 * undoes the wrap as it measures each gap, so what it works from is
	 * the gaps, and a gap is what this hands it. At the longest interval
	 * the run below spans days. */
	uint64_t last = 0;
	int have_last = 0;

	memset(s, 0, sizeof(*s));

	/* Enough events for LE_CSA2_SIGHTINGS visits with room to spare: the
	 * channel comes up about once in 37 events on a full map. */
	for (event = 0; event < 60000; ++event) {
		uint16_t counter = (uint16_t)(first_counter + event);
		uint64_t stamp;

		if (le_csa2_channel(counter, access_address, remapping) !=
				LISTEN_CHANNEL)
			continue;

		++seen;
		if (drop_nth != 0 && seen % drop_nth == 0)
			continue;

		stamp = (uint64_t)((double)event * unit + 0.5);

		/* Sightings the mode has already searched from and found
		 * nothing in: the ring has slid past them. */
		if (seen <= skip) {
			s->newest_counter = counter;
			last = stamp;
			have_last = 1;
			continue;
		}

		if (have_last) {
			u32 gap = (u32)(stamp - last);

			/* The mode's own two filters: two packets of one
			 * connection event are not a visit, and a gap longer
			 * than a trip round the channels at the longest
			 * interval says nothing. */
			if (gap < (u32)LE_MIN_CONN_INTERVAL *
					LE_CONN_INTERVAL_TICKS)
				continue;

			/* The mode takes this sighting as the one the next gap
			 * is measured from even when it drops the gap, so the
			 * sighting is recorded before the second filter. */
			last = stamp;
			s->newest_counter = counter;
			have_last = 1;

			if (gap > (u32)LE_HOP_CHANNELS * LE_MAX_CONN_INTERVAL *
					LE_CONN_INTERVAL_TICKS)
				continue;

			if (s->gap_count < GAPS) {
				s->gap[s->gap_count++] = gap;
			} else {
				memmove(&s->gap[0], &s->gap[1],
						(GAPS - 1) * sizeof(s->gap[0]));
				s->gap[GAPS - 1] = gap;
			}
		}

		s->newest_counter = counter;
		last = stamp;
		have_last = 1;

		if (s->gap_count == GAPS)
			return;
	}
}

static void collect(struct sightings *s, u32 access_address, uint16_t interval,
                    uint16_t first_counter, le_channel_remapping_t *remapping,
                    int ppm, unsigned drop_nth)
{
	collect_from(s, access_address, interval, first_counter, remapping,
	             ppm, drop_nth, 0);
}

/* ------------------------------------------------------------------------
 * The channel algorithm 2 picks before the map has its say
 * ------------------------------------------------------------------------ */

/*
 * The search works from le_csa2_unmapped_channel(), so that is what has to
 * agree with the algorithm itself: on a connection keeping all 37 channels
 * the two give the same channel for every counter.
 */
MU_TEST(unmapped_is_the_full_map_channel)
{
	le_channel_remapping_t remapping;
	u32 counter;
	unsigned mismatches = 0, out_of_range = 0;

	full_map(&remapping);
	for (counter = 0; counter < 65536; ++counter) {
		if (le_csa2_channel((uint16_t)counter, AA_A, &remapping) !=
				le_csa2_unmapped_channel((uint16_t)counter,
						AA_A))
			++mismatches;
		if (le_csa2_unmapped_channel((uint16_t)counter, AA_B) >=
				LE_HOP_CHANNELS)
			++out_of_range;
	}

	mu_assert_int_eq(0, mismatches);
	mu_assert_int_eq(0, out_of_range);
}

/*
 * How often the listening channel comes up, which is what the search's hit
 * list is sized for.
 *
 * It is exact, not statistical: le_csa2_prn_e() is one to one in the event
 * counter, so over the counter's whole range the pseudo-random number takes
 * every value once, and 65536 = 37 * 1771 + 9. Every channel therefore has
 * 1771 counters and nine of them have 1772, whatever the access address. A
 * list of 1772 can never be truncated, and that is checked here rather than
 * taken on trust.
 */
MU_TEST(every_channel_has_1771_or_1772_counters)
{
	u32 aa[5] = { AA_A, AA_B, 0x12345678u, 0xaffe1234u, 0x0000ffffu };
	unsigned i, ch;

	for (i = 0; i < 5; ++i) {
		unsigned total = 0, bigger = 0;

		for (ch = 0; ch < LE_HOP_CHANNELS; ++ch) {
			u32 counter;
			unsigned hits = 0;
			char msg[128];

			for (counter = 0; counter < 65536; ++counter)
				if (le_csa2_unmapped_channel((uint16_t)counter,
						aa[i]) == ch)
					++hits;

			snprintf(msg, sizeof(msg),
			         "access address %08lx channel %u: %u counters",
			         (unsigned long)aa[i], ch, hits);
			mu_assert(hits == 1771 || hits == 1772, msg);
			total += hits;
			if (hits == 1772)
				++bigger;
		}

		mu_assert_int_eq(65536, total);
		mu_assert_int_eq(9, bigger);
	}
}

/* ------------------------------------------------------------------------
 * The connection interval
 * ------------------------------------------------------------------------ */

MU_TEST(interval_comes_out_of_the_sightings)
{
	/* 7.5 ms, 15 ms, 30 ms, 48.75 ms (the odd one: not a whole number of
	 * the others), 400 ms and 4 s, the ends of the range included. */
	uint16_t intervals[6] = { 6, 12, 24, 39, 320, 3200 };
	le_channel_remapping_t remapping;
	unsigned i;

	full_map(&remapping);
	for (i = 0; i < 6; ++i) {
		struct sightings s;
		char msg[128];
		uint16_t got;

		collect(&s, AA_A, intervals[i], 4000, &remapping, 0, 0);
		got = le_csa2_interval(s.gap, s.gap_count);
		snprintf(msg, sizeof(msg), "interval %u: got %u from %u gaps",
		         intervals[i], got, s.gap_count);
		mu_assert(got == intervals[i], msg);
	}
}

/* A clock 400 ppm out either way is inside what the specification allows the
 * two ends between them, and the gaps still say the interval. */
MU_TEST(interval_survives_clock_error)
{
	le_channel_remapping_t remapping;
	int ppm[5] = { -400, -100, 0, 100, 400 };
	unsigned i;

	full_map(&remapping);
	for (i = 0; i < 5; ++i) {
		struct sightings s;
		char msg[96];
		uint16_t got;

		collect(&s, AA_B, 24, 100, &remapping, ppm[i], 0);
		got = le_csa2_interval(s.gap, s.gap_count);
		snprintf(msg, sizeof(msg), "%d ppm: got %u, wanted 24",
		         ppm[i], got);
		mu_assert(got == 24, msg);
	}
}

/* Nothing is claimed from too few gaps to tell an interval from a
 * coincidence. */
MU_TEST(interval_needs_enough_gaps)
{
	struct sightings s;
	le_channel_remapping_t remapping;

	full_map(&remapping);
	collect(&s, AA_A, 24, 0, &remapping, 0, 0);

	mu_assert(le_csa2_interval(s.gap, 0) == 0, "no gaps at all");
	mu_assert(le_csa2_interval(s.gap, LE_CSA2_INTERVAL_MISSES) == 0,
	          "as many gaps as the search is allowed to miss");
}

/*
 * A gap is multiplied back out of the events it spans, and the product is
 * about the gap itself, so a gap near the top of a 32-bit count would carry
 * it past what fits. The search drops any gap longer than the connection can
 * be away and still be coming back, which is the longest the mode that
 * collects them will pass. These check both sides of that bound, with the
 * worst gap a 32-bit count holds among them.
 */
MU_TEST(an_impossible_gap_is_dropped)
{
	le_channel_remapping_t remapping;
	struct sightings s;
	le_csa2_recovery_t rec;
	u32 gap[GAPS];
	unsigned i;

	/* The longest gap it does accept, repeated: 37 events of the longest
	 * connection interval, so the interval it says is that one. */
	for (i = 0; i < GAPS; ++i)
		gap[i] = LE_PROMISC_MAX_GAP;
	mu_assert_int_eq(LE_MAX_CONN_INTERVAL,
	                 le_csa2_interval(gap, GAPS));

	/* One tick longer, and every gap is dropped, so no interval is left to
	 * believe. */
	for (i = 0; i < GAPS; ++i)
		gap[i] = LE_PROMISC_MAX_GAP + 1;
	mu_assert_int_eq(0, le_csa2_interval(gap, GAPS));

	for (i = 0; i < GAPS; ++i)
		gap[i] = 0xffffffffu;
	mu_assert_int_eq(0, le_csa2_interval(gap, GAPS));
	mu_assert(!le_csa2_recover(gap, GAPS, LISTEN_CHANNEL, AA_A, &rec),
	          "the search answered from gaps no connection can produce");

	/* The newest gap impossible, the rest a real connection: the offsets
	 * of every sighting are measured through that gap, so there is nothing
	 * left to search from and the answer is none. */
	full_map(&remapping);
	collect(&s, AA_A, 24, 321, &remapping, 0, 0);
	s.gap[s.gap_count - 1] = 0xffffffffu;
	mu_assert(!le_csa2_recover(s.gap, s.gap_count, LISTEN_CHANNEL, AA_A,
	                           &rec),
	          "the newest sighting was reached through an impossible gap");

	/* The oldest gap impossible: the sightings newer than it are still a
	 * run, and the newest is still the one the answer belongs to. */
	collect(&s, AA_A, 24, 321, &remapping, 0, 0);
	s.gap[0] = 0xffffffffu;
	mu_assert_fatal(le_csa2_recover(s.gap, s.gap_count, LISTEN_CHANNEL,
	                                AA_A, &rec),
	                "the newer sightings were not used");
	mu_assert_int_eq(s.newest_counter, rec.event_counter);
	mu_assert_int_eq(24, rec.conn_interval);
}

/* ------------------------------------------------------------------------
 * The event counter
 * ------------------------------------------------------------------------ */

/* The whole recovery on a connection keeping all 37 channels. */
MU_TEST(recovers_a_full_map_connection)
{
	uint16_t counters[4] = { 0, 1, 31415, 65500 };
	le_channel_remapping_t remapping;
	unsigned i;

	full_map(&remapping);
	for (i = 0; i < 4; ++i) {
		struct sightings s;
		le_csa2_recovery_t rec;
		char msg[192];

		collect(&s, AA_A, 24, counters[i], &remapping, 0, 0);
		mu_assert_fatal(le_csa2_recover(s.gap, s.gap_count,
		                                LISTEN_CHANNEL, AA_A, &rec),
		                "the search found nothing");

		snprintf(msg, sizeof(msg),
		         "counter from %u: got %u, wanted %u (interval %u, "
		         "score %u of %u, runner-up %u)",
		         counters[i], rec.event_counter, s.newest_counter,
		         rec.conn_interval, rec.score, rec.sightings,
		         rec.runner_up);
		mu_assert(rec.event_counter == s.newest_counter &&
		          rec.conn_interval == 24, msg);
		/* Every sighting is a direct visit here, so the answer
		 * accounts for all of them. */
		mu_assert_int_eq(rec.sightings, rec.score);
	}
}

/*
 * A connection using fewer than the 37 channels. Its map moves visits from
 * the channels it dropped onto the ones it kept, so some sightings on the
 * listening channel belong to events whose unmapped channel is a different
 * one -- and the map cannot be known from one channel, so those sightings
 * cannot be predicted. They are points not scored, and the right counter
 * still wins.
 *
 * 18 and 19 channels are what the connections on the test bench negotiate.
 */
MU_TEST(recovers_a_reduced_map_connection)
{
	unsigned used[5] = { 37, 30, 24, 19, 18 };
	unsigned i;

	for (i = 0; i < 5; ++i) {
		le_channel_remapping_t remapping;
		struct sightings s;
		le_csa2_recovery_t rec;
		char msg[224];

		map_of(used[i], &remapping);
		collect(&s, AA_B, 30, 777, &remapping, 0, 0);

		snprintf(msg, sizeof(msg), "%u channels: the search found "
		         "nothing from %u gaps", used[i], s.gap_count);
		mu_assert_fatal(le_csa2_recover(s.gap, s.gap_count,
		                                LISTEN_CHANNEL, AA_B, &rec),
		                msg);

		snprintf(msg, sizeof(msg),
		         "%u channels: counter %u, wanted %u; interval %u, "
		         "wanted 30; score %u of %u, runner-up %u",
		         used[i], rec.event_counter, s.newest_counter,
		         rec.conn_interval, rec.score, rec.sightings,
		         rec.runner_up);
		mu_assert(rec.event_counter == s.newest_counter &&
		          rec.conn_interval == 30, msg);
	}
}

/* A visit the board did not hear leaves a gap of two visits in its place. The
 * search counts in connection events rather than visits, so such a gap is
 * still a whole number of intervals and still carries its sightings. */
MU_TEST(recovers_through_missed_visits)
{
	unsigned drop[3] = { 7, 4, 3 };
	le_channel_remapping_t remapping;
	unsigned i;

	full_map(&remapping);
	for (i = 0; i < 3; ++i) {
		struct sightings s;
		le_csa2_recovery_t rec;
		char msg[160];

		collect(&s, AA_A, 12, 2024, &remapping, 0, drop[i]);

		snprintf(msg, sizeof(msg),
		         "one visit in %u missed: the search found nothing",
		         drop[i]);
		mu_assert_fatal(le_csa2_recover(s.gap, s.gap_count,
		                                LISTEN_CHANNEL, AA_A, &rec),
		                msg);

		snprintf(msg, sizeof(msg),
		         "one visit in %u missed: counter %u, wanted %u; "
		         "interval %u, wanted 12",
		         drop[i], rec.event_counter, s.newest_counter,
		         rec.conn_interval);
		mu_assert(rec.event_counter == s.newest_counter &&
		          rec.conn_interval == 12, msg);
	}
}

/*
 * A connection that was away: one gap in the middle is far longer than the
 * events either side of it account for. Every sighting older than that gap
 * has its offset measured through it, so the search drops them and works from
 * the newer ones -- which are the ones the hand-over to the follow needs.
 */
MU_TEST(recovers_past_an_absence)
{
	le_channel_remapping_t remapping;
	struct sightings s;
	le_csa2_recovery_t rec;
	char msg[160];

	full_map(&remapping);
	collect(&s, AA_A, 24, 500, &remapping, 0, 0);
	/* Not a whole number of intervals, and far more events than
	 * LE_CSA2_MAX_EVENT_GAP: the search cannot measure through it. */
	s.gap[2] += 900u * 24u * LE_CONN_INTERVAL_TICKS + 3000u;

	mu_assert_fatal(le_csa2_recover(s.gap, s.gap_count, LISTEN_CHANNEL,
	                                AA_A, &rec),
	                "the search found nothing");
	snprintf(msg, sizeof(msg),
	         "counter %u, wanted %u; sightings %u of %u",
	         rec.event_counter, s.newest_counter, rec.sightings,
	         s.gap_count + 1);
	mu_assert(rec.event_counter == s.newest_counter, msg);
	mu_assert(rec.sightings < s.gap_count + 1,
	          "the sightings behind the absence were used anyway");
}

/* The counter near its wrap, so that the search's arithmetic has to wrap with
 * it. */
MU_TEST(recovers_across_the_counter_wrap)
{
	uint16_t intervals[3] = { 6, 24, 400 };
	le_channel_remapping_t remapping;
	unsigned i;

	full_map(&remapping);
	for (i = 0; i < 3; ++i) {
		struct sightings s;
		le_csa2_recovery_t rec;
		char msg[160];

		/* Far enough back that the run of sightings crosses 65535. */
		collect(&s, AA_B, intervals[i], (uint16_t)(65536 - 600),
		        &remapping, 0, 0);
		mu_assert_fatal(s.newest_counter < 60000,
		                "the run did not cross the wrap");

		snprintf(msg, sizeof(msg), "interval %u: found nothing",
		         intervals[i]);
		mu_assert_fatal(le_csa2_recover(s.gap, s.gap_count,
		                                LISTEN_CHANNEL, AA_B, &rec),
		                msg);

		snprintf(msg, sizeof(msg),
		         "interval %u: counter %u, wanted %u; interval %u",
		         intervals[i], rec.event_counter, s.newest_counter,
		         rec.conn_interval);
		mu_assert(rec.event_counter == s.newest_counter &&
		          rec.conn_interval == intervals[i], msg);
	}
}

/* ------------------------------------------------------------------------
 * When it must decline
 * ------------------------------------------------------------------------ */

/* An algorithm-1 connection's sightings. It hops by a fixed increment, so its
 * visits to one channel repeat on the 37-event cycle the other estimator
 * looks for, and no event counter accounts for them. */
MU_TEST(declines_an_algorithm_1_connection)
{
	unsigned increment;

	for (increment = LE_MIN_HOP_INCREMENT;
			increment <= LE_MAX_HOP_INCREMENT; ++increment) {
		u32 gap[GAPS];
		le_csa2_recovery_t rec;
		unsigned idx = increment;  /* the channel after the first hop */
		unsigned count = 0, event = 0;
		int have_last = 0;
		u32 last = 0;
		char msg[192];

		while (count < GAPS && event < 4000) {
			if (idx == LISTEN_CHANNEL) {
				u32 stamp = event * 24u *
						LE_CONN_INTERVAL_TICKS;

				if (have_last)
					gap[count++] = stamp - last;
				last = stamp;
				have_last = 1;
			}
			idx = (idx + increment) % LE_HOP_CHANNELS;
			++event;
		}

		if (le_csa2_recover(gap, count, LISTEN_CHANNEL, AA_A, &rec)) {
			snprintf(msg, sizeof(msg),
			         "hop increment %u: adopted counter %u, "
			         "interval %u, score %u of %u, runner-up %u",
			         increment, rec.event_counter,
			         rec.conn_interval, rec.score, rec.sightings,
			         rec.runner_up);
			mu_assert(0, msg);
		} else {
			mu_assert(1, "");
		}
	}
}

/* Gaps that are not a connection at all. */
MU_TEST(declines_random_gaps)
{
	unsigned trial;
	unsigned seed = 1;
	unsigned adopted = 0;

	for (trial = 0; trial < 200; ++trial) {
		u32 gap[GAPS];
		le_csa2_recovery_t rec;
		unsigned i;

		for (i = 0; i < GAPS; ++i) {
			seed = seed * 1103515245u + 12345u;
			/* Between about 2 ms and 1 s. */
			gap[i] = 20000u + (seed >> 12) % 10000000u;
		}

		if (le_csa2_recover(gap, GAPS, LISTEN_CHANNEL, AA_A, &rec))
			++adopted;
	}

	mu_assert_int_eq(0, adopted);
}

/* An advertising channel index is not one a connection hops over. */
MU_TEST(declines_a_channel_off_the_hop_sequence)
{
	le_channel_remapping_t remapping;
	struct sightings s;
	le_csa2_recovery_t rec;

	full_map(&remapping);
	collect(&s, AA_A, 24, 0, &remapping, 0, 0);

	mu_assert(!le_csa2_recover(s.gap, s.gap_count, 37, AA_A, &rec),
	          "channel 37 is the advertising channel at 2402 MHz");
	mu_assert(!le_csa2_recover(s.gap, s.gap_count, 39, AA_A, &rec),
	          "channel 39 is the advertising channel at 2480 MHz");
}

/* The answer belongs to the connection it was searched for, so the same
 * sightings read against another access address come back empty. */
MU_TEST(declines_the_wrong_access_address)
{
	le_channel_remapping_t remapping;
	struct sightings s;
	le_csa2_recovery_t rec;

	full_map(&remapping);
	collect(&s, AA_A, 24, 9000, &remapping, 0, 0);

	mu_assert(!le_csa2_recover(s.gap, s.gap_count, LISTEN_CHANNEL, AA_B,
	                           &rec),
	          "the search answered for another connection's address");
}

/* ------------------------------------------------------------------------
 * How often it works
 * ------------------------------------------------------------------------ */

/*
 * A sweep over connections a board might meet: different access addresses,
 * intervals, event counters and channel maps, with the clock off and visits
 * missed. It prints what it found, because that figure is the point, and
 * fails if any answer is wrong -- a wrong answer sends the follow after a
 * sequence no connection uses -- or if it declines too often to be useful.
 */
MU_TEST(how_often_it_works)
{
	static const uint16_t intervals[6] = { 6, 12, 24, 30, 80, 400 };
	static const unsigned used[5] = { 37, 30, 24, 19, 18 };
	unsigned seed = 20261101;
	unsigned trials = 0, found = 0, wrong = 0;
	unsigned i, j, k, rep;
	char msg[224];

	for (i = 0; i < 6; ++i) {
		for (j = 0; j < 5; ++j) {
			for (k = 0; k < 2; ++k) {
				u32 aa = k == 0 ? AA_A : AA_B;

				for (rep = 0; rep < 4; ++rep) {
					le_channel_remapping_t remapping;
					struct sightings s;
					le_csa2_recovery_t rec;
					uint16_t counter;
					int ppm;
					unsigned drop;

					seed = seed * 1103515245u + 12345u;
					counter = (uint16_t)(seed >> 11);
					ppm = (int)((seed >> 5) % 801) - 400;
					drop = rep == 3 ? 5 : 0;

					map_of(used[j], &remapping);
					collect(&s, aa, intervals[i], counter,
					        &remapping, ppm, drop);
					++trials;

					if (!le_csa2_recover(s.gap,
							s.gap_count,
							LISTEN_CHANNEL, aa,
							&rec))
						continue;
					++found;
					if (rec.event_counter ==
							s.newest_counter &&
							rec.conn_interval ==
							intervals[i])
						continue;

					++wrong;
					snprintf(msg, sizeof(msg),
					         "interval %u, %u channels, "
					         "counter from %u: got counter "
					         "%u (wanted %u), interval %u",
					         intervals[i], used[j], counter,
					         rec.event_counter,
					         s.newest_counter,
					         rec.conn_interval);
					mu_assert(0, msg);
				}
			}
		}
	}

	printf("    recovered %u of %u connections on the first search, "
	       "%u of them wrong\n", found, trials, wrong);

	snprintf(msg, sizeof(msg), "recovered only %u of %u", found, trials);
	mu_assert(found * 10 >= trials * 9, msg);
	mu_assert_int_eq(0, wrong);
}

/*
 * The same sweep as the mode runs it. A search that declines costs nothing
 * but the time until there are more sightings: the mode starts both its
 * estimators again and searches on the next full set, so each search works
 * from sightings the one before it did not see.
 *
 * This is where the connections on the test bench sit. Their maps keep 18 or
 * 19 of the 37 channels, so about half of each set of sightings is visits the
 * map moved to this channel, which no counter predicts, and a set that lands
 * below the threshold is normal rather than a failure. Every connection in
 * the sweep is recovered within a few searches.
 */
MU_TEST(a_declined_search_is_recovered_by_the_next)
{
	static const uint16_t intervals[6] = { 6, 12, 24, 30, 80, 400 };
	static const unsigned used[5] = { 37, 30, 24, 19, 18 };
	/* A whole fresh set of sightings between searches, as the mode takes
	 * it: after a search both estimators start again. */
	static const unsigned every = LE_CSA2_SIGHTINGS;
	unsigned seed = 4711;
	unsigned trials = 0, found = 0, searches = 0;
	unsigned i, j, k, rep;
	char msg[224];

	for (i = 0; i < 6; ++i) {
		for (j = 0; j < 5; ++j) {
			for (k = 0; k < 2; ++k) {
				u32 aa = k == 0 ? AA_A : AA_B;

				for (rep = 0; rep < 4; ++rep) {
					le_channel_remapping_t remapping;
					uint16_t counter;
					int ppm;
					unsigned drop, attempt;

					seed = seed * 1103515245u + 12345u;
					counter = (uint16_t)(seed >> 11);
					ppm = (int)((seed >> 5) % 801) - 400;
					drop = rep == 3 ? 5 : 0;

					map_of(used[j], &remapping);
					++trials;

					for (attempt = 0; attempt < 6;
							++attempt) {
						struct sightings s;
						le_csa2_recovery_t rec;

						collect_from(&s, aa,
						        intervals[i], counter,
						        &remapping, ppm, drop,
						        attempt * every);
						++searches;
						if (!le_csa2_recover(s.gap,
								s.gap_count,
								LISTEN_CHANNEL,
								aa, &rec))
							continue;

						snprintf(msg, sizeof(msg),
						         "interval %u, %u "
						         "channels: counter %u,"
						         " wanted %u; interval "
						         "%u",
						         intervals[i], used[j],
						         rec.event_counter,
						         s.newest_counter,
						         rec.conn_interval);
						mu_assert(rec.event_counter ==
						          s.newest_counter &&
						          rec.conn_interval ==
						          intervals[i], msg);
						++found;
						break;
					}
				}
			}
		}
	}

	printf("    recovered %u of %u connections within six searches "
	       "(%u searches in all)\n", found, trials, searches);

	snprintf(msg, sizeof(msg), "recovered only %u of %u", found, trials);
	mu_assert(found == trials, msg);
}

/* ------------------------------------------------------------------------
 * The one number this header keeps a copy of
 * ------------------------------------------------------------------------ */

/*
 * LE_CONN_INTERVAL_TICKS is LE_BASECLK written again in bluetooth_le.h, so
 * that the arithmetic there needs none of the clock's hardware header. The two
 * have to be the same number: a search measuring gaps in a different unit
 * from the mode that collected them would get every interval wrong.
 */
MU_TEST(the_interval_unit_matches_the_clock)
{
	char path[512];
	char line[512];
	FILE *f;
	int baseclk = 0;

	snprintf(path, sizeof(path), "%s/ubertooth_clock.h", FW_RXTX_DIR);
	f = fopen(path, "r");
	mu_assert_fatal(f != NULL, "cannot read ubertooth_clock.h");

	while (fgets(line, sizeof(line), f) != NULL) {
		int value;

		if (sscanf(line, " #define LE_BASECLK (%d)", &value) == 1 ||
		    sscanf(line, " #define LE_BASECLK %d", &value) == 1)
			baseclk = value;
	}
	fclose(f);

	mu_assert_fatal(baseclk != 0,
	                "LE_BASECLK not found in ubertooth_clock.h");
	mu_assert_int_eq(baseclk, LE_CONN_INTERVAL_TICKS);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
	printf("LE promiscuous algorithm-2 recovery tests\n");
	MU_RUN(unmapped_is_the_full_map_channel);
	MU_RUN(every_channel_has_1771_or_1772_counters);
	MU_RUN(interval_comes_out_of_the_sightings);
	MU_RUN(interval_survives_clock_error);
	MU_RUN(interval_needs_enough_gaps);
	MU_RUN(an_impossible_gap_is_dropped);
	MU_RUN(recovers_a_full_map_connection);
	MU_RUN(recovers_a_reduced_map_connection);
	MU_RUN(recovers_through_missed_visits);
	MU_RUN(recovers_past_an_absence);
	MU_RUN(recovers_across_the_counter_wrap);
	MU_RUN(declines_an_algorithm_1_connection);
	MU_RUN(declines_random_gaps);
	MU_RUN(declines_a_channel_off_the_hop_sequence);
	MU_RUN(declines_the_wrong_access_address);
	MU_RUN(the_interval_unit_matches_the_clock);
	MU_RUN(how_often_it_works);
	MU_RUN(a_declined_search_is_recovered_by_the_next);
	return mu_report();
}
