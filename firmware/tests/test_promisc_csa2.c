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

/*
 * A map with `used` channels spread over the band, always keeping the channel
 * being listened to; `shift` moves which others it keeps.
 *
 * A real reduced map is not a block at the bottom of the band, and the shape
 * matters here: the search only ever hears a connection whose map keeps this
 * channel, so a map built from the bottom up is never heard at all until it is
 * wide enough to reach channel 17, which hides what the search does on a
 * sparse map.
 */
static void map_spread(unsigned used, unsigned shift,
                       le_channel_remapping_t *remapping)
{
	uint8_t channel_map[5] = { 0, 0, 0, 0, 0 };
	unsigned kept = 1, i;

	channel_map[LISTEN_CHANNEL / 8] |= (uint8_t)(1u << (LISTEN_CHANNEL % 8));

	for (i = 0; kept < used && i < LE_HOP_CHANNELS; ++i) {
		unsigned channel = (i * 7 + shift) % LE_HOP_CHANNELS;

		if (channel == LISTEN_CHANNEL)
			continue;
		if (channel_map[channel / 8] & (1u << (channel % 8)))
			continue;
		channel_map[channel / 8] |= (uint8_t)(1u << (channel % 8));
		++kept;
	}

	le_parse_channel_map(channel_map, remapping);
}

static void full_map(le_channel_remapping_t *remapping)
{
	map_spread(37, 0, remapping);
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
 *
 * A sighting is stamped a little after its connection event begins, by a
 * different amount each time. What the board times is the arrival of a
 * packet, and which packet of the event it hears varies: the central's comes
 * first, at the event's start, and the peripheral's answer 150 microseconds
 * later plus however long the central's packet took -- up to about a
 * millisecond for a full one on the 1M PHY. So the gaps the search works from
 * are not exact multiples of the interval even with a perfect clock.
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

		/* Where in its event this sighting was heard. The sequence is
		 * arbitrary rather than random, so a failure is reproducible. */
		stamp = (uint64_t)((double)event * unit + 0.5) +
				(uint64_t)(((event * 7919u) % 11u) * 1000u);

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

			/* Two packets of one connection event are not a visit,
			 * and the sighting does not become the one the next
			 * gap is measured from. */
			if (gap < (u32)LE_MIN_CONN_INTERVAL *
					LE_CONN_INTERVAL_TICKS)
				continue;

			last = stamp;
			s->newest_counter = counter;
			have_last = 1;

			/* A gap longer than a trip round the channels at the
			 * longest interval says nothing, and the two sightings
			 * it lies between are not neighbours, so the mode
			 * throws the run away and starts the next from this
			 * sighting. Leaving the gap out and keeping the rest
			 * would make the ring a chain with a hole in it. */
			if (gap > LE_PROMISC_MAX_GAP) {
				s->gap_count = 0;
				have_last = 0;
				continue;
			}

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

/*
 * Is this the interval the connection was given, as the board's clock sees it?
 *
 * The answer comes from gaps the board timed, and the two clocks differ by a
 * few hundred parts per million, which at a long interval is a fair part of
 * the 1.25 ms a unit holds: a 2 s interval read 400 ppm fast is 1600.64 units,
 * and the nearest whole number is 1601. The follow hops on the board's clock,
 * so that is the figure it wants. One unit either way is therefore right, not
 * an error -- and at the short intervals the sweeps also cover, the same
 * tolerance is far smaller than a unit, so the answer there is exact.
 */
static int near(uint16_t got, uint16_t wanted)
{
	return got + 1 >= wanted && got <= wanted + 1;
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
	/* 7.5 ms, the shortest a connection may use; 15 ms; 30 ms; 48.75 ms,
	 * the odd one, not a whole number of any of the others; 400 ms; and
	 * 2 s, which is the longest the mode can collect a run of gaps at (see
	 * the_longest_intervals_are_out_of_reach). */
	uint16_t intervals[6] = { 6, 12, 24, 39, 320, 1600 };
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

/*
 * The longest interval a connection may use, with its clock running fast.
 *
 * A gap divided by its event count then lands above 3200, which no connection
 * may use, so without the step that brings it back the longest interval left
 * in the running would be half of it -- and a follow at half the interval is
 * on the wrong event every other event. The mode cannot collect a run of gaps
 * this long (see the_longest_intervals_are_out_of_reach), so these gaps are
 * built here rather than collected.
 */
MU_TEST(the_longest_interval_is_not_halved)
{
	static const unsigned events[10] = {
		37, 4, 19, 12, 31, 7, 25, 16, 33, 9,
	};
	int ppm[2] = { 200, 400 };
	unsigned i, j;

	for (j = 0; j < 2; ++j) {
		u32 gap[10];
		char msg[128];
		uint16_t got;

		for (i = 0; i < 10; ++i)
			gap[i] = (u32)((double)events[i] *
					LE_MAX_CONN_INTERVAL *
					LE_CONN_INTERVAL_TICKS *
					(1.0 + ppm[j] / 1e6) + 0.5);

		got = le_csa2_interval(gap, 10);
		snprintf(msg, sizeof(msg), "%d ppm: got %u, wanted %u",
		         ppm[j], got, LE_MAX_CONN_INTERVAL);
		mu_assert(got == LE_MAX_CONN_INTERVAL, msg);
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

/*
 * Near the top of the connection-interval range the mode cannot collect a run
 * of sightings to search at all, and this says so rather than leaving it to
 * be discovered.
 *
 * The connection comes back to one channel every 37 events on average, and a
 * gap longer than LE_PROMISC_MAX_GAP -- 37 events of the longest interval,
 * which is about as long as the capture clock can measure before it wraps --
 * breaks the run. At a 4 s interval that bound is 37 events, so more than a
 * third of the gaps break the run and a run of two dozen is hopeless. At 1 s
 * the bound is 148 events and almost every gap fits.
 */
MU_TEST(the_longest_intervals_are_out_of_reach)
{
	static const uint16_t intervals[4] = { 400, 800, 1600, 3200 };
	le_channel_remapping_t remapping;
	unsigned i, start;
	unsigned full[4] = { 0, 0, 0, 0 };
	char msg[160];

	full_map(&remapping);
	for (i = 0; i < 4; ++i) {
		for (start = 0; start < 50; ++start) {
			struct sightings s;

			collect(&s, AA_A, intervals[i],
			        (uint16_t)(start * 1301), &remapping, 0, 0);
			if (s.gap_count == GAPS)
				++full[i];
		}
		printf("    interval %4u (%6.0f ms): a full run of sightings "
		       "in %2u of 50 starts\n", intervals[i],
		       intervals[i] * 1.25, full[i]);
	}

	/* Up to half a second the run is always there; at 4 s it never is, so
	 * the search is never even reached. In between it is a matter of luck,
	 * which is why the sweeps stop at 1 s. */
	snprintf(msg, sizeof(msg), "500 ms: %u of 50", full[0]);
	mu_assert(full[0] == 50, msg);
	snprintf(msg, sizeof(msg), "4 s: %u of 50", full[3]);
	mu_assert(full[3] == 0, msg);
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

		unsigned attempt;
		int found = 0;

		/* Up to three sets of sightings, as the mode takes them. Even
		 * on a full map a set can be declined: one run in sixteen has
		 * its newest four gaps all spanning an even number of events,
		 * which is what an interval the connection has left behind
		 * looks like, and the search would rather search again than
		 * answer on it. */
		for (attempt = 0; attempt < 3 && !found; ++attempt) {
			collect_from(&s, AA_A, 24, counters[i], &remapping, 0,
			             0, attempt * LE_CSA2_SIGHTINGS);
			found = le_csa2_recover(s.gap, s.gap_count,
			                        LISTEN_CHANNEL, AA_A, &rec);
		}
		mu_assert_fatal(found, "the search found nothing in three sets"
		                " of sightings");

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

		unsigned attempt;
		int found = 0;

		map_spread(used[i], i, &remapping);

		/* Up to six sets of sightings, as the mode takes them. The
		 * answer is anchored on the newest sighting, and on a reduced
		 * map that sighting is often a visit the map moved here, which
		 * no counter predicts; each set draws a fresh one. */
		for (attempt = 0; attempt < 6 && !found; ++attempt) {
			collect_from(&s, AA_B, 30, 777, &remapping, 0, 0,
			             attempt * LE_CSA2_SIGHTINGS);
			found = le_csa2_recover(s.gap, s.gap_count,
			                        LISTEN_CHANNEL, AA_B, &rec);
		}

		snprintf(msg, sizeof(msg), "%u channels: the search found "
		         "nothing in six sets of sightings", used[i]);
		mu_assert_fatal(found, msg);

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

/*
 * A connection that renegotiates its interval part-way through the run.
 *
 * Doubling or halving an interval is ordinary, and it is the awkward case: an
 * interval the connection has left still divides every gap after the change,
 * because every multiple of twice a number is a multiple of it. Read against
 * the whole run the interval it has left therefore fits everything and the one
 * it moved to fits only the newer gaps, so the old one would win -- and the
 * event counts the walk back then reads off the newer gaps are half or double
 * what they were, which shifts every older sighting by a constant and leaves a
 * counter that explains them and is wrong.
 *
 * The answer has to be the interval the connection is on now, and the counter
 * has to belong to the newest sighting. Both come out right here, or the
 * search declines; what it must not do is answer with the old interval.
 */
MU_TEST(an_interval_change_does_not_give_a_wrong_answer)
{
	static const uint16_t pairs[10][2] = {
		/* By two, which is the common renegotiation, and by three and
		 * five: an interval the connection has left divides the gaps
		 * after the change whatever the factor is. */
		{ 24, 48 }, { 48, 24 }, { 12, 24 }, { 24, 12 },
		{ 30, 60 }, { 60, 30 }, { 10, 30 }, { 30, 10 },
		{ 12, 60 }, { 60, 12 },
	};
	unsigned i, k, at;
	unsigned answered = 0, wrong = 0, tried = 0;

	for (i = 0; i < 10; ++i) {
		for (k = 0; k < 2; ++k) {
			u32 aa = k == 0 ? AA_A : AA_B;

			/* From one gap after the change, where only the
			 * newest sighting or two belong to the connection
			 * as it now is, to most of the run. */
			for (at = 1; at <= 20; ++at) {
				le_channel_remapping_t remapping;
				struct sightings before, after;
				le_csa2_recovery_t rec;
				u32 gap[GAPS];
				unsigned n;
				char msg[224];

				map_spread(37 - 6 * k, at, &remapping);

				/* The run as the mode would hold it: the older
				 * gaps timed at the first interval, the newer
				 * ones at the second, with the change `at`
				 * gaps from the end. */
				collect(&before, aa, pairs[i][0], 900,
				        &remapping, 0, 0);
				collect(&after, aa, pairs[i][1], 900,
				        &remapping, 0, 0);
				if (before.gap_count < GAPS ||
						after.gap_count < GAPS)
					continue;

				for (n = 0; n < GAPS; ++n)
					gap[n] = n < GAPS - at
						? before.gap[n]
						: after.gap[n];

				++tried;
				if (!le_csa2_recover(gap, GAPS, LISTEN_CHANNEL,
						aa, &rec))
					continue;

				++answered;
				snprintf(msg, sizeof(msg),
				         "%u then %u units, change %u gaps from "
				         "the end: answered interval %u and "
				         "counter %u, wanted interval %u and "
				         "counter %u (score %u of %u)",
				         pairs[i][0], pairs[i][1], at,
				         rec.conn_interval, rec.event_counter,
				         pairs[i][1], after.newest_counter,
				         rec.score, rec.sightings);
				if (!near(rec.conn_interval, pairs[i][1]) ||
						rec.event_counter !=
						after.newest_counter) {
					++wrong;
					mu_assert(0, msg);
				}
			}
		}
	}

	printf("    %u runs with an interval change: %u answered, %u wrong\n",
	       tried, answered, wrong);
	mu_assert(tried > 0, "no run with an interval change was built");
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
	static const uint16_t intervals[6] = { 6, 12, 24, 30, 800, 1600 };
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

					map_spread(used[j], j, &remapping);
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
							near(rec.conn_interval,
								intervals[i]))
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

	/* The figure that matters for a user is the one the next test measures
	 * -- how many are recovered at all -- because a declined search costs
	 * only the wait for more sightings. A single set is turned down
	 * whenever the newest sighting of it is a visit the channel map moved
	 * here, which on the reduced maps in this sweep is about half the
	 * time, so this bar is here to catch the rate collapsing rather than
	 * to pin it. */
	snprintf(msg, sizeof(msg), "recovered only %u of %u", found, trials);
	mu_assert(found * 10 >= trials * 5, msg);
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
 * below the threshold is normal rather than a failure.
 *
 * All but one of the sweep's connections are recovered within a few searches.
 * The one that is not stays below the threshold however long the mode watches:
 * how often a map moves a visit onto this particular channel is a property of
 * the access address and the map rather than luck, and for that pair it is
 * high enough to keep the right counter inside the margin of the best wrong
 * one. Declining is the right answer there.
 */
MU_TEST(a_declined_search_is_recovered_by_the_next)
{
	static const uint16_t intervals[6] = { 6, 12, 24, 30, 800, 1600 };
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

					map_spread(used[j], j, &remapping);
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
						          near(rec.conn_interval,
						               intervals[i]),
						          msg);
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
	mu_assert(found + 2 >= trials, msg);
}

/* ------------------------------------------------------------------------
 * Holding the follow through the events it cannot hear
 * ------------------------------------------------------------------------ */

/*
 * A follow set up by this search has no channel map, so it tunes to the
 * channel algorithm 2 picks before a map has its say and hears only the
 * events whose channel the connection's own map left there. The mode calls a
 * connection over after LE_CSA2_SILENCE_EVENTS events with nothing heard, so
 * that figure has to outlast the runs a connection it is following correctly
 * puts it through.
 *
 * Those runs are not a matter of chance: for a given access address and map,
 * which events the follow can hear is fixed, and a sparse map can hide dozens
 * of events in a row. So this counts them on the real channel sequences, and
 * ties the two things together: whenever the search will adopt an answer for a
 * connection, the allowance has to cover that connection's worst run. A map
 * too sparse for the allowance is only safe if the search declines it, and a
 * map the search accepts is only safe if the allowance covers it.
 *
 * The maps keep the channel being listened to and spread the rest over the
 * band, which is what lets a sparse map be recovered at all: a map without
 * this channel in it is never heard here.
 */
MU_TEST(the_silence_allowance_covers_every_map_adopted)
{
	static const unsigned used[8] = { 6, 8, 10, 12, 15, 18, 24, 37 };
	unsigned i, a;
	unsigned worst_adopted = 0, worst_map = 0;

	for (i = 0; i < 8; ++i) {
		unsigned adopted = 0, tries = 0;

		for (a = 0; a < 12; ++a) {
			le_channel_remapping_t remapping;
			u32 aa = AA_A * (a + 1) + AA_B * a;
			unsigned event, run = 0, longest = 0;
			unsigned t;
			int any = 0;
			char msg[192];

			map_spread(used[i], a, &remapping);

			for (event = 0; event < 65536; ++event) {
				uint16_t counter = (uint16_t)event;

				if (le_csa2_channel(counter, aa, &remapping) ==
						le_csa2_unmapped_channel(
							counter, aa)) {
					run = 0;
					continue;
				}
				if (++run > longest)
					longest = run;
			}

			for (t = 0; t < 3; ++t) {
				struct sightings s;
				le_csa2_recovery_t rec;

				collect(&s, aa, 24, (uint16_t)(t * 20011),
				        &remapping, 0, 0);
				++tries;
				if (s.gap_count == GAPS &&
						le_csa2_recover(s.gap,
							s.gap_count,
							LISTEN_CHANNEL, aa,
							&rec)) {
					++adopted;
					any = 1;
				}
			}

			if (!any)
				continue;

			if (longest > worst_adopted) {
				worst_adopted = longest;
				worst_map = used[i];
			}

			snprintf(msg, sizeof(msg),
			         "%u channels, access address %08lx: the search "
			         "adopts it and its longest unheard run is %u, "
			         "against an allowance of %u events",
			         used[i], (unsigned long)aa, longest,
			         LE_CSA2_SILENCE_EVENTS);
			mu_assert(longest < LE_CSA2_SILENCE_EVENTS, msg);
		}

		printf("    %2u channels: adopted in %2u of %2u runs\n",
		       used[i], adopted, tries);
	}

	printf("    worst unheard run among the maps adopted: %u events, at "
	       "%u channels, against an allowance of %u\n", worst_adopted,
	       worst_map, LE_CSA2_SILENCE_EVENTS);
}

/* ------------------------------------------------------------------------
 * The packet the search waits behind
 * ------------------------------------------------------------------------ */

/*
 * Nothing above runs until the mode has the connection's CRCInit, and that
 * comes from an empty data PDU: the one packet whose contents are known
 * without it. Its header is LLID 1 with a zero length, and the two
 * acknowledgment bits are whatever the connection's traffic has made them.
 *
 * promisc_follow_cb() is hardware code and cannot run here, so this reads its
 * comparison out of the source, the way test_promisc_hop_interval.c reads the
 * estimator's constants. What it has to show is that the comparison masks
 * those two bits off: a connection with data flowing rarely sends an empty
 * PDU with both of them clear, and waiting for one is how the mode used to
 * stop at the access address.
 */
MU_TEST(crc_init_comes_from_any_empty_data_pdu)
{
	char path[512];
	char line[512];
	FILE *f;
	int mask = 0, want = -1;
	unsigned header;

	snprintf(path, sizeof(path), "%s/bluetooth_rxtx.c", FW_RXTX_DIR);
	f = fopen(path, "r");
	mu_assert_fatal(f != NULL, "cannot read bluetooth_rxtx.c");

	while (fgets(line, sizeof(line), f) != NULL) {
		int value;

		if (sscanf(line, " #define LE_EMPTY_PDU_HEADER_MASK 0x%x",
		           (unsigned *)&value) == 1)
			mask = value;
		if (sscanf(line, " #define LE_EMPTY_PDU_HEADER 0x%x",
		           (unsigned *)&value) == 1)
			want = value;
	}
	fclose(f);

	mu_assert_fatal(mask != 0 && want >= 0,
	                "the empty-PDU header constants were not found");

	/* The four headers an empty data PDU can carry: LLID 1, no more data,
	 * reserved bits clear, and the two acknowledgment bits either way. All
	 * four have to pass. */
	for (header = 0; header < 256; ++header) {
		int is_empty_pdu = (header & 0x03) == 0x01 &&
				(header & 0xf0) == 0x00;
		int accepted = ((int)header & mask) == want;
		char msg[96];

		snprintf(msg, sizeof(msg), "header 0x%02x: %s", header,
		         accepted ? "accepted" : "rejected");
		mu_assert(accepted == is_empty_pdu, msg);
	}
}

/* ------------------------------------------------------------------------
 * The collector, read as text
 * ------------------------------------------------------------------------ */

/*
 * What the search is handed, rather than what it does with it.
 *
 * `collect()` above is a model of promisc_recover_hop_interval(), and a model
 * and the code under test can agree with each other while the firmware goes
 * its own way: the one line whose absence sent the search a run of gaps with a
 * hole in it was in the firmware, not here. That function reads the clock, so
 * it cannot run on the host; this reads it as text instead, the way
 * test_promisc_hop_interval.c reads the estimator's own constants, and checks
 * the three things the model rests on.
 *
 * Every path out of it that does not record a gap has to start the run again,
 * except the two that leave the run as it was: the first sighting of a pass,
 * which is an anchor and not the end of a gap, and a second packet of the same
 * connection event, which is not a visit.
 */
MU_TEST(the_collector_starts_the_run_again_on_every_break)
{
	char path[512];
	char line[1024];
	FILE *f;
	int in_function = 0, in_restart = 0, in_block = 0;
	int countdown = 0, gap_too_long_restarts = 0, failed_search_restarts = 0;
	int restart_clears = 0, restart_clears_stamp = 0;
	int run_length = 0, trigger_wants_a_full_ring = 0, gaps_kept = 0;
	int saw_restart_before_return = 0;
	char msg[192];

	snprintf(path, sizeof(path), "%s/bluetooth_rxtx.c", FW_RXTX_DIR);
	f = fopen(path, "r");
	mu_assert_fatal(f != NULL, "cannot read bluetooth_rxtx.c");

	while (fgets(line, sizeof(line), f) != NULL) {
		char *comment;

		/* Comments say what the code should do; only the code counts
		 * here. */
		if (in_block) {
			if (strstr(line, "*/") == NULL)
				continue;
			in_block = 0;
			continue;
		}
		comment = strstr(line, "/*");
		if (comment != NULL) {
			if (strstr(comment, "*/") == NULL)
				in_block = 1;
			*comment = '\0';
		}

		if (sscanf(line, " #define LE_PROMISC_CSA2_RUN %d",
		           &run_length) == 1)
			continue;
		if (sscanf(line, " #define LE_PROMISC_GAPS %d",
		           &gaps_kept) == 1)
			continue;

		if (strstr(line, "static void promisc_restart_estimators") !=
				NULL)
			in_restart = 1;
		else if (strstr(line, "void promisc_recover_hop_interval") !=
				NULL)
			in_function = 1;
		else if (line[0] == '}') {
			in_restart = 0;
			in_function = 0;
		}

		if (in_restart) {
			if (strstr(line, "le_promisc.gap_count = 0") != NULL ||
			    strstr(line, "le_promisc.csa2_gap_count = 0") !=
					NULL ||
			    strstr(line, "le_promisc.sightings = 0") != NULL ||
			    strstr(line, "le_promisc.consec_intervals = 0") !=
					NULL ||
			    strstr(line, "le.conn_interval = 0") != NULL)
				++restart_clears;
			if (strstr(line, "le_promisc.seen_a_packet = 0") !=
					NULL)
				restart_clears_stamp = 1;
		}

		if (!in_function)
			continue;

		/* The two paths that break the run: a gap too long to be the
		 * connection coming back, and a search that found nothing.
		 * Each has to start the run again within the few lines that
		 * follow it. */
		if (strstr(line, "clk_diff > LE_PROMISC_MAX_GAP") != NULL) {
			countdown = 3;
			saw_restart_before_return = 1;
		} else if (strstr(line, "promisc_try_csa2(") != NULL) {
			countdown = 6;
			saw_restart_before_return = 2;
		} else if (countdown > 0) {
			--countdown;
			if (strstr(line, "promisc_restart_estimators()") !=
					NULL) {
				if (saw_restart_before_return == 1)
					gap_too_long_restarts = 1;
				else
					failed_search_restarts = 1;
				countdown = 0;
			}
		}

		if (strstr(line, "csa2_gap_count == LE_CSA2_SIGHTINGS - 1") !=
				NULL)
			trigger_wants_a_full_ring = 1;
	}
	fclose(f);

	/* The five counters and the stamp. */
	mu_assert_int_eq(5, restart_clears);
	mu_assert(restart_clears_stamp,
	          "promisc_restart_estimators() leaves the last sighting's "
	          "stamp, so the next gap is measured across the break");

	mu_assert(gap_too_long_restarts,
	          "a gap too long to be the connection coming back leaves the "
	          "run standing, so the next gap is appended next to one that "
	          "is not its neighbour");
	mu_assert(failed_search_restarts,
	          "a search that found nothing leaves the run standing, so the "
	          "gap across the search's own deafness is appended to it");

	mu_assert(trigger_wants_a_full_ring,
	          "the search can run on a ring that is not full, so on gaps "
	          "left over from before the last break");

	/* The estimator above reads the pattern the gaps repeat: two runs of
	 * the longest pattern it looks for, and then four more reports that
	 * agree. A search that ran sooner than that, and wiped its evidence,
	 * could keep a connection it would have settled on from ever doing
	 * so. */
	mu_assert_fatal(gaps_kept > 0 && run_length > 0,
	                "LE_PROMISC_GAPS or LE_PROMISC_CSA2_RUN not found");
	snprintf(msg, sizeof(msg),
	         "LE_PROMISC_CSA2_RUN is %d, against the %d gaps the "
	         "algorithm-1 estimator can need", run_length,
	         2 * gaps_kept + 4);
	mu_assert(run_length >= 2 * gaps_kept + 4, msg);
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
	MU_RUN(the_longest_interval_is_not_halved);
	MU_RUN(interval_survives_clock_error);
	MU_RUN(interval_needs_enough_gaps);
	MU_RUN(an_impossible_gap_is_dropped);
	MU_RUN(the_longest_intervals_are_out_of_reach);
	MU_RUN(recovers_a_full_map_connection);
	MU_RUN(recovers_a_reduced_map_connection);
	MU_RUN(an_interval_change_does_not_give_a_wrong_answer);
	MU_RUN(recovers_through_missed_visits);
	MU_RUN(recovers_past_an_absence);
	MU_RUN(recovers_across_the_counter_wrap);
	MU_RUN(declines_an_algorithm_1_connection);
	MU_RUN(declines_random_gaps);
	MU_RUN(declines_a_channel_off_the_hop_sequence);
	MU_RUN(declines_the_wrong_access_address);
	MU_RUN(the_silence_allowance_covers_every_map_adopted);
	MU_RUN(the_collector_starts_the_run_again_on_every_break);
	MU_RUN(crc_init_comes_from_any_empty_data_pdu);
	MU_RUN(the_interval_unit_matches_the_clock);
	MU_RUN(how_often_it_works);
	MU_RUN(a_declined_search_is_recovered_by_the_next);
	return mu_report();
}
