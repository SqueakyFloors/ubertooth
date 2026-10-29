/*
 * The board's 100 ns capture clock across its wrap, fed straight into the
 * real time reconstruction: no device and no capture file.
 *
 * now_ns_from_clk100ns() extends clk100ns into a 64-bit tick count, adding
 * the period of the clock the mode stamps with for each wrap. Extending it as
 * if every clock were a 32-bit counter added 101.8 s to every packet after a
 * wrap of the CLKN-derived clock, so every Classic capture longer than its
 * 327.68 s period was wrong from the first wrap on; adding that period to the
 * low-energy engine's timer1 stamps, which do start over at 2^32, would take
 * 101.8 s off instead. Both clocks are exercised here. The reconstruction is
 * static in ubertooth_callback.c, so that source is compiled into this test.
 *
 * A wrap inferred from the clock reading lower than it did is also lost
 * whenever more than one period passes between two packets, so the cases here
 * include gaps that long: a Classic packet carries its own period count,
 * which covers them, and a capture whose packets carry none has the loss
 * reported against the host clock instead.
 *
 * A capture can also open with a record left over from the mode before, which
 * carries that mode's clock reading and its period count. Either one read as
 * this capture's set the times out by whole periods - as much as five hours
 * from the count - so the cases here cover both, along with the legitimate
 * disagreement between a count and its own record's reading that no check may
 * reject.
 *
 * Most cases check only differences between reconstructed times, so they hold
 * whatever the first packet's clock is. The cases about a first record out of
 * place are the exception: there the question is which record the capture is
 * anchored on.
 *
 * Most cases here run as a replay, so that the host time of each packet comes
 * from the case rather than from the clock of the machine running the test.
 * The last few run as a live capture instead: there the gap between two
 * packets is measured on the reading each one arrived with, which the ring
 * buffer carries, so a case can set when each packet arrived and then hand
 * them all to the reconstruction at once - which is what a host that stalled
 * before its first packet leaves behind.
 *
 * A stall also decides where the capture's wall-clock times start, and the
 * live cases at the end check that: the host time the first record arrived,
 * worked out from the two clocks read together, rather than the time the loop
 * got round to it. Those cases read the real clocks of the machine running
 * the test, because relating the two is the whole of what they check, and
 * allow for the moments between the readings.
 *
 * The last few cases take that as far as the files a run writes. They drive
 * the real callbacks with a dump and a capture open at once and then replay
 * the dump, so the host time the dump carries and the time the capture gives
 * the same packet are compared as a user would compare them. The last two go
 * through a packet that arrived as a chain of records: a dump stores one host
 * time per record, so a chain that crossed a whole second is stored with two
 * different seconds, and a replay has to place the packet at the first of them
 * as the live capture did.
 */
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "../../libubertooth/src/ubertooth_callback.c"

/* The firmware's own CLK100NS, evaluated here against the shared period: the
 * registers it reads stand in as variables. */
static volatile uint32_t clkn;
static volatile uint32_t T0TC;
#include "../../../firmware/bluetooth_rxtx/ubertooth_clock.h"

/* A clock step of 100 s, so four of them cross the CLKN-derived wrap. */
#define STEP_TICKS 1000000000ull
/* Where the first packet lands, well away from either wrap. */
#define FIRST_TICKS 100000000ull
/* One second of the 100 ns clock, for the records near the start of it. */
#define ONE_SECOND_TICKS 10000000ull

/* How long the host stalled before reaching the capture's first record, in
 * the anchor cases at the end. Far longer than a packet's latency, so a
 * capture anchored where the loop reached the record is plainly late. */
#define STALL_NS 5000000000ull

/* How far the host time read next to the loop may be out as a measure of
 * where the capture should be anchored. The work between the two readings is
 * a couple of clock reads and a copy out of the ring buffer, microseconds at
 * most, so this is almost all allowance for a loaded machine being
 * descheduled in the middle. Kept this tight because the prompt-host case
 * below is what would catch an anchor moved when it should not have been, and
 * a wider window would let a whole second of that through. */
#define ANCHOR_SLACK_NS 100000000ull

/* Whether each packet is stamped with a host time taken from the tick count
 * given to the helpers below, which is the true elapsed time of the case. The
 * cases whose whole point is a capture clock and a host clock that disagree
 * set their own host times and clear this. */
static int host_time_follows_the_clock = 1;

/* The reconstructed time of a packet captured at this clock value, with the
 * count it carries given outright: the firmware can send one that does not
 * match its own clock reading, because the two are latched by separate reads
 * of CLKN, and a saved capture can be edited or replayed out of order. */
static uint64_t at_clock_with_count(ubertooth_t* ut, uint64_t ticks,
                                    uint8_t count)
{
	usb_pkt_rx rx;

	if (host_time_follows_the_clock)
		systime = (uint32_t)(ticks / ONE_SECOND_TICKS);
	memset(&rx, 0, sizeof(rx));
	rx.clk100ns = htole32((uint32_t)(ticks % ut->clk100ns_period));
	rx.clkn_high = count;
	/* A replayed record arrives with the host time it carries, which is
	 * what the library pushes into the ring buffer with it. */
	return now_ns_from_clk100ns( ut, &rx, ubertooth_packet_arrival_ns( ) );
}

/* The same for a packet stamped the way a real board stamps it: the
 * firmware's enqueue() puts the number of times the CLKN-derived clock has
 * started over in clkn_high, so a Classic packet carries its own period
 * count. The low-energy physical-layer modes send a zero there instead, which
 * is counted == 0 here. */
static uint64_t at_clock_counted(ubertooth_t* ut, uint64_t ticks, int counted)
{
	uint8_t count = counted ?
	                (uint8_t)((ticks / ut->clk100ns_period) & 0xff) : 0;

	return at_clock_with_count( ut, ticks, count );
}

static uint64_t at_clock(ubertooth_t* ut, uint64_t ticks)
{
	return at_clock_counted(ut, ticks, 0);
}

/* A capture that has seen no packet yet, stamped with this clock. The host
 * time of each packet comes from the case rather than from the clock of the
 * machine running the test: a wrap is told from a packet out of place by how
 * much host time passed between the two, so a case that left that to the real
 * clock would say nothing about it. Only whether a replay is running is read
 * from infile, never the file itself. */
static void start_capture(ubertooth_t* ut, uint64_t period)
{
	memset(ut, 0, sizeof(*ut));
	ut->clk100ns_period = period;
	infile = stdin;
	host_time_follows_the_clock = 1;
}

/* A live capture that has seen no record yet, with a ring buffer of its own:
 * a fresh one per case, so a reading left behind by the case before cannot
 * stand in for one this case should have stamped. No replay, so the time each
 * record is stamped with comes from the clock of the machine running the test
 * and only differences between the reconstructed times are checked. What the
 * cases do set is when each record arrived, which is what the gap between two
 * records is measured on. */
static void start_live_capture(ubertooth_t* ut, uint64_t period)
{
	memset(ut, 0, sizeof(*ut));
	ut->clk100ns_period = period;
	ut->fifo = fifo_init();
	infile = NULL;
	host_time_follows_the_clock = 0;
}

/* Captures records live: ticks[] is where each one's clock reading lands,
 * counts[] the period count it carries - NULL where the records carry none -
 * and arrived_ns[] when it arrived, as nanoseconds from the start of the
 * capture. The reconstructed times come back in times[].
 *
 * Every record is pushed before any is popped, which is what a host that
 * stalled before its first record leaves: the loop then takes them all within
 * microseconds of each other, however far apart they really arrived. A gap
 * measured where the loop reaches a record would be almost nothing here, so
 * these cases only come out right if the reading each record arrived with is
 * what the gap is measured on. */
static void capture_records(ubertooth_t* ut, uint64_t period, int records,
                            const uint64_t ticks[], const uint64_t arrived_ns[],
                            const uint8_t counts[], uint64_t times[])
{
	/* Where this capture's monotonic clock starts. Not zero, so a case
	 * cannot pass on a reading that was never stamped. */
	const uint64_t base_ns = 4000000000ull;
	int record;

	start_live_capture(ut, period);
	mu_assert_fatal(ut->fifo != NULL, "cannot allocate the ring buffer");
	for (record = 0; record < records; record++) {
		usb_pkt_rx rx;

		memset(&rx, 0, sizeof(rx));
		rx.clk100ns = htole32((uint32_t)(ticks[record] % period));
		rx.clkn_high = counts != NULL ? counts[record] : 0;
		fifo_push(ut->fifo, &rx, base_ns + arrived_ns[record]);
	}
	for (record = 0; record < records; record++) {
		usb_pkt_rx popped = fifo_pop(ut->fifo);

		times[record] = now_ns_from_clk100ns(ut, &popped,
		                                     fifo_popped_arrival_ns(ut->fifo));
	}
	free(ut->fifo);
	ut->fifo = NULL;
}

MU_TEST(the_period_is_the_clock_the_firmware_counts)
{
	/* The firmware's CLK100NS at the top of its range: one tick below the
	 * period the host adds per wrap, whatever the numbers behind it. */
	clkn = 0xfffff;                      /* the low 20 bits CLK100NS uses */
	T0TC = CLK100NS_PER_CLKN - 1;        /* the timer, just before it matches */
	mu_assert_int_eq(CLK100NS_PERIOD - 1, CLK100NS);

	/* The low-energy engine's timer1 counts the same 100 ns ticks, free
	 * running, so it starts over one tick later than 2^32 - 1. */
	mu_assert(CLK100NS_TIMER1_PERIOD == 4294967296ull, "timer1 period");
}

MU_TEST(a_tick_is_100_ns_before_any_wrap)
{
	ubertooth_t ut;
	uint64_t first, later;

	start_capture(&ut, CLK100NS_PERIOD);
	first = at_clock(&ut, FIRST_TICKS);
	later = at_clock(&ut, FIRST_TICKS + STEP_TICKS);

	mu_assert_int_eq(100 * STEP_TICKS, later - first);
	mu_assert_int_eq(0, ut.clk100ns_upper);
}

MU_TEST(the_first_tick_after_a_wrap_follows_the_last_one)
{
	ubertooth_t ut;
	uint64_t last, wrapped;

	start_capture(&ut, CLK100NS_PERIOD);
	last = at_clock(&ut, CLK100NS_PERIOD - 1);
	wrapped = at_clock(&ut, CLK100NS_PERIOD);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100, wrapped - last);
}

MU_TEST(a_wrap_adds_the_clocks_period_not_2_32)
{
	ubertooth_t ut;
	uint64_t before, after;

	/* The packets either side of the first wrap, one step apart. */
	start_capture(&ut, CLK100NS_PERIOD);
	before = at_clock(&ut, 3 * STEP_TICKS);
	after = at_clock(&ut, 4 * STEP_TICKS);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100 * STEP_TICKS, after - before);
}

MU_TEST(a_timer1_wrap_adds_2_32)
{
	ubertooth_t ut;
	uint64_t before, after;

	/* The low-energy engine's clock starts over at 2^32, not at the
	 * CLKN-derived period, so its wrap must add 2^32. */
	start_capture(&ut, CLK100NS_TIMER1_PERIOD);
	before = at_clock(&ut, 4 * STEP_TICKS);
	after = at_clock(&ut, 5 * STEP_TICKS);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100 * STEP_TICKS, after - before);
}

/* Replays packets at the given clock readings, arriving at the given host
 * seconds from the start of the capture, each carrying the period count in
 * counts[] - NULL where the packets carry none. The host time comes from the
 * record, as a replay's does, so the capture says when each packet arrived as
 * well as what the board's clock read, and the two can be made to disagree.
 * The reconstructed times come back in times[]. */
static void replay_records(ubertooth_t* ut, uint64_t period, int records,
                           const uint64_t ticks[], const uint32_t host_s[],
                           const uint8_t counts[], uint64_t times[])
{
	uint32_t base = 1700000000;
	int packet;

	start_capture(ut, period);
	host_time_follows_the_clock = 0;
	for (packet = 0; packet < records; packet++) {
		systime = base + host_s[packet];
		times[packet] = at_clock_with_count(ut, ticks[packet],
		                                    counts != NULL ?
		                                    counts[packet] : 0);
	}
}

/* The same for three packets a second apart, which is how a capture whose
 * first record is out of place arrives. */
static void replay_a_second_apart(ubertooth_t* ut, uint64_t period,
                                  const uint64_t ticks[3], uint64_t times[3])
{
	const uint32_t host_s[3] = { 0, 1, 2 };

	replay_records(ut, period, 3, ticks, host_s, NULL, times);
}

/* A first record that reads ahead of every record after it. A packet left
 * over from the mode before carries that mode's clock, and a saved capture
 * can hold records out of order. The clock reading lower on the next record
 * must not be taken for a wrap: that put a whole period, 327.68 s, into every
 * time from there on. Nothing in the clock alone says which it is - the host
 * time does, because these records arrived a second apart and a wrap here
 * would mean 298.68 s passed. */
MU_TEST(a_record_ahead_of_the_rest_does_not_add_a_period)
{
	ubertooth_t ut;
	const uint64_t ticks[3] = { 30 * ONE_SECOND_TICKS,
	                            1 * ONE_SECOND_TICKS,
	                            2 * ONE_SECOND_TICKS };
	uint64_t times[3];

	replay_a_second_apart(&ut, CLK100NS_PERIOD, ticks, times);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	/* The two records that agree start the capture, a second apart. */
	mu_assert_int_eq(ut.abs_start_ns, times[1]);
	mu_assert_int_eq(100 * ONE_SECOND_TICKS, times[2] - times[1]);
	/* The record ahead of them is the one the capture was anchored on, so
	 * it carries the time the capture started. */
	mu_assert_int_eq(ut.abs_start_ns, times[0]);
}

/* Only between the capture's first two packets is a lower reading read any way
 * but as a wrap. Further in it is still read as one, as it always was, even
 * where the host times say otherwise. This pins where the decision stops, not
 * that a stray further in is beyond settling: settling that needs the gap read
 * as a number of wraps rather than as one wrap or none, because by then a
 * capture can have been quiet for longer than a period. */
MU_TEST(a_lower_reading_further_in_is_still_a_wrap)
{
	ubertooth_t ut;
	const uint64_t ticks[3] = { 10 * ONE_SECOND_TICKS,
	                            12 * ONE_SECOND_TICKS,
	                             5 * ONE_SECOND_TICKS };
	uint64_t times[3];

	replay_a_second_apart(&ut, CLK100NS_PERIOD, ticks, times);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(ut.abs_start_ns, times[0]);
	mu_assert_int_eq(100 * 2 * ONE_SECOND_TICKS, times[1] - times[0]);
	mu_assert_int_eq(100 * (CLK100NS_PERIOD - 5 * ONE_SECOND_TICKS),
	                 times[2] - times[0]);
}

/* How far ahead a first record out of place reads is arbitrary: a packet left
 * over from a mode whose clock was reset can read anywhere in the period. So
 * the whole range has to come out the same way, which is what going by the
 * host time gets and going by how far the clock dropped did not. */
MU_TEST(any_lead_on_the_first_record_is_read_the_same_way)
{
	static const uint64_t leads[] = { 5, 30, 164, 200, 300, 320 };
	ubertooth_t ut;
	uint64_t ticks[3], times[3];
	size_t lead;

	for (lead = 0; lead < sizeof(leads) / sizeof(leads[0]); lead++) {
		ticks[0] = leads[lead] * ONE_SECOND_TICKS;
		ticks[1] = 1 * ONE_SECOND_TICKS;
		ticks[2] = 2 * ONE_SECOND_TICKS;
		replay_a_second_apart(&ut, CLK100NS_PERIOD, ticks, times);

		mu_assert_int_eq(0, ut.clk100ns_upper);
		mu_assert_int_eq(ut.abs_start_ns, times[1]);
		mu_assert_int_eq(100 * ONE_SECOND_TICKS, times[2] - times[1]);
	}
}

/* Once the anchor has moved to the capture's second record, the records after
 * it are read against that one, not the leftover, so the rest of the capture
 * runs on from there with its own times. */
MU_TEST(records_after_a_stale_first_one_keep_their_times)
{
	ubertooth_t ut;
	const uint64_t ticks[4] = { 200 * ONE_SECOND_TICKS, /* out of place */
	                              1 * ONE_SECOND_TICKS,
	                             60 * ONE_SECOND_TICKS,
	                            210 * ONE_SECOND_TICKS };
	const uint32_t host_s[4] = { 0, 1, 60, 210 };
	uint64_t times[4];

	replay_records(&ut, CLK100NS_PERIOD, 4, ticks, host_s, NULL, times);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(ut.abs_start_ns, times[1]);
	mu_assert_int_eq(100 * 59 * ONE_SECOND_TICKS, times[2] - times[1]);
	mu_assert_int_eq(100 * 209 * ONE_SECOND_TICKS, times[3] - times[1]);
}

/* The low-energy engine's timer1 stamps carry no count at all, and its period
 * is longer, so the same shape has to come out the same way there. */
MU_TEST(a_record_ahead_of_the_rest_does_not_add_a_timer1_period)
{
	ubertooth_t ut;
	const uint64_t ticks[3] = { 30 * ONE_SECOND_TICKS,
	                            1 * ONE_SECOND_TICKS,
	                            2 * ONE_SECOND_TICKS };
	uint64_t times[3];

	replay_a_second_apart(&ut, CLK100NS_TIMER1_PERIOD, ticks, times);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(ut.abs_start_ns, times[1]);
	mu_assert_int_eq(100 * ONE_SECOND_TICKS, times[2] - times[1]);
}

/* A capture whose first record is one left over from the mode before, carrying
 * that mode's period count as well as its clock: the leftover reads 30 s into
 * the clock, and the capture's own two records read 1 s and 2 s a second apart
 * and carry the zero the firmware's reset of CLKN left. */
static void replay_after_a_leftover_count(ubertooth_t* ut, uint8_t count,
                                          uint64_t times[3])
{
	const uint64_t ticks[3] = { 30 * ONE_SECOND_TICKS,
	                             1 * ONE_SECOND_TICKS,
	                             2 * ONE_SECOND_TICKS };
	const uint32_t host_s[3] = { 0, 1, 2 };
	uint8_t counts[3] = { 0, 0, 0 };

	counts[0] = count;
	replay_records(ut, CLK100NS_PERIOD, 3, ticks, host_s, counts, times);
}

/* The count a leftover record carries is whatever the mode before it ran to,
 * 1 to 255 periods, and none of those is time this capture spent. Read as a
 * difference from the capture's own zero, 200 is 56 periods forwards - 18,321 s,
 * over five hours, into every time from there on - while 10 reads backwards,
 * which left the stale count as the baseline, so every record after it was
 * rejected too and no period of the capture was counted at all. */
MU_TEST(a_leftover_first_record_does_not_add_its_period_count)
{
	static const uint8_t leftovers[] = { 1, 10, 128, 129, 200 };
	ubertooth_t ut;
	uint64_t times[3];
	size_t leftover;

	for (leftover = 0; leftover < sizeof(leftovers) / sizeof(leftovers[0]);
	     leftover++) {
		replay_after_a_leftover_count(&ut, leftovers[leftover], times);

		mu_assert_int_eq(0, ut.clk100ns_upper);
		/* The capture's own first record starts it, and the count it
		 * carries is what the records after it are measured from. */
		mu_assert_int_eq(0, ut.last_clkn_high);
		mu_assert_int_eq(ut.abs_start_ns, times[1]);
		mu_assert_int_eq(100 * ONE_SECOND_TICKS, times[2] - times[1]);
	}
}

/* A leftover count exactly one period from the capture's own is the one shape
 * that cannot be settled: that is as far as the firmware's two reads of CLKN
 * can take a single record's count from its own clock reading, which is a
 * legitimate disagreement (below). So it is still read as a period of time,
 * and the capture still comes out 298.68 s late. Pinned here because it is the
 * limit of what the host clock can decide, not an oversight. */
MU_TEST(a_leftover_count_one_period_out_is_still_read_as_time)
{
	ubertooth_t ut;
	uint64_t times[3];

	replay_after_a_leftover_count(&ut, 255, times);

	mu_assert_int_eq(1, ut.clk100ns_upper);
}

/* The other direction of that same unsettleable shape, where the leftover's
 * count is one period above the capture's own rather than one below. The host
 * clock cannot place it, so the stale count stays the baseline and the records
 * whose counts read backwards from it are rejected - which is what used to let
 * a leftover cost a whole capture. It costs one period and no more: once the
 * capture's own count reaches the stale one the records are believed again, and
 * from there the capture runs at the right rate, a period behind.
 *
 * The leftover reads 0.5 s with a count of 1, which is where the host clock
 * runs out: the capture's own first record reads 1 s and carries 0, so the
 * count going back a period while the reading climbs claims 327.18 s against
 * the second the host saw - inside the period of latch slack. */
MU_TEST(a_leftover_count_a_period_above_costs_a_period_not_the_capture)
{
	ubertooth_t ut;
	const uint64_t ticks[5] = { ONE_SECOND_TICKS / 2,
	                            1 * ONE_SECOND_TICKS,
	                            300 * ONE_SECOND_TICKS,
	                            (uint64_t) CLK100NS_PERIOD
	                            + 10 * ONE_SECOND_TICKS,
	                            2 * (uint64_t) CLK100NS_PERIOD
	                            + 10 * ONE_SECOND_TICKS };
	const uint32_t host_s[5] = { 0, 1, 300, 338, 665 };
	const uint8_t counts[5] = { 1, 0, 0, 1, 2 };
	uint64_t times[5];

	replay_records(&ut, CLK100NS_PERIOD, 5, ticks, host_s, counts, times);

	/* The capture's own first record still anchors it, and the records
	 * before its count catches up with the stale one keep their times. */
	mu_assert_int_eq(100 * 299 * ONE_SECOND_TICKS, times[2] - times[1]);
	/* A period into the capture the times fall a period short - the cost
	 * of this shape - and they stay exactly a period short after it. */
	mu_assert_int_eq(100 * 9 * ONE_SECOND_TICKS, times[3] - times[1]);
	mu_assert_int_eq(100 * (uint64_t) CLK100NS_PERIOD,
	                 times[4] - times[3]);
}

/* The firmware latches a record's clock reading and its period count with two
 * reads of CLKN, so the capture's first record can carry a count a period away
 * from its own reading. That disagreement is legitimate and the count has to be
 * believed through it: rejecting it would throw away a period that really did
 * pass.
 *
 * Here the first record was caught just after the clock started over, with its
 * count still latched in the period before: it reads 1 s into period 4 and
 * carries 3. The next record, ten seconds on, carries the 4 its reading belongs
 * to, so the count moves a period while the reading climbs. */
MU_TEST(a_first_count_a_period_behind_its_reading_is_believed)
{
	ubertooth_t ut;
	const uint64_t ticks[2] = { 4 * (uint64_t) CLK100NS_PERIOD
	                            +  1 * ONE_SECOND_TICKS,
	                            4 * (uint64_t) CLK100NS_PERIOD
	                            + 11 * ONE_SECOND_TICKS };
	const uint32_t host_s[2] = { 0, 10 };
	const uint8_t counts[2] = { 3, 4 };
	uint64_t times[2];

	replay_records(&ut, CLK100NS_PERIOD, 2, ticks, host_s, counts, times);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(4, ut.last_clkn_high);
	mu_assert_int_eq(ut.abs_start_ns, times[0]);
	mu_assert_int_eq(100 * (CLK100NS_PERIOD + 10 * ONE_SECOND_TICKS),
	                 times[1] - times[0]);
}

/* The other order of those two reads: the first record's count is already past
 * the period its reading is in. It reads 320 s into period 3 and carries 4, and
 * the record after it, taken once the clock had started over, carries the same
 * 4. The count moves nowhere, and must still be believed. */
MU_TEST(a_first_count_a_period_ahead_of_its_reading_is_believed)
{
	ubertooth_t ut;
	const uint64_t ticks[2] = { 3 * (uint64_t) CLK100NS_PERIOD
	                            + 320 * ONE_SECOND_TICKS,
	                            4 * (uint64_t) CLK100NS_PERIOD
	                            +   1 * ONE_SECOND_TICKS };
	const uint32_t host_s[2] = { 0, 9 };
	const uint8_t counts[2] = { 4, 4 };
	uint64_t times[2];

	replay_records(&ut, CLK100NS_PERIOD, 2, ticks, host_s, counts, times);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(4, ut.last_clkn_high);
	/* That record is a period ahead of itself, so the one after it reads
	 * before it, which the reconstruction's signed arithmetic allows. */
	mu_assert_int_eq(100 * 319 * ONE_SECOND_TICKS, times[0] - times[1]);
}

/* A wrap that really did fall between the capture's first two records still
 * counts: the count says a period passed and the host clock saw the time to
 * match. 300 s into period 3, then 37.68 s later in period 4. */
MU_TEST(a_wrap_between_the_first_two_counted_records_still_counts)
{
	ubertooth_t ut;
	const uint64_t ticks[2] = { 3 * (uint64_t) CLK100NS_PERIOD
	                            + 300 * ONE_SECOND_TICKS,
	                            4 * (uint64_t) CLK100NS_PERIOD
	                            +  10 * ONE_SECOND_TICKS };
	const uint32_t host_s[2] = { 0, 38 };
	const uint8_t counts[2] = { 3, 4 };
	uint64_t times[2];

	replay_records(&ut, CLK100NS_PERIOD, 2, ticks, host_s, counts, times);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100 * (ticks[1] - ticks[0]), times[1] - times[0]);
}

/* Nor is there a ceiling on how far apart the capture's first two records may
 * be: three whole periods and 10 s here, which the count says and the host
 * clock agrees with. */
MU_TEST(a_long_counted_gap_at_the_start_still_counts)
{
	ubertooth_t ut;
	uint64_t gap = 3 * (uint64_t) CLK100NS_PERIOD + 10 * ONE_SECOND_TICKS;
	const uint64_t ticks[2] = { 1 * (uint64_t) CLK100NS_PERIOD
	                            + 10 * ONE_SECOND_TICKS,
	                            1 * (uint64_t) CLK100NS_PERIOD
	                            + 10 * ONE_SECOND_TICKS + 3
	                            * (uint64_t) CLK100NS_PERIOD
	                            + 10 * ONE_SECOND_TICKS };
	const uint32_t host_s[2] = { 0, 993 };
	const uint8_t counts[2] = { 1, 4 };
	uint64_t times[2];

	replay_records(&ut, CLK100NS_PERIOD, 2, ticks, host_s, counts, times);

	mu_assert_int_eq(3, ut.clk100ns_upper);
	mu_assert_int_eq(100 * gap, times[1] - times[0]);
}

/* Packets a step apart over several wraps, with no step of its own anywhere. */
static void continuous_over_wraps(uint64_t period, int packets, int wraps,
                                  int counted)
{
	ubertooth_t ut;
	uint64_t ticks, first = 0, previous = 0, now;
	int packet;

	start_capture(&ut, period);
	for (packet = 0; packet < packets; packet++) {
		ticks = FIRST_TICKS + (uint64_t)packet * STEP_TICKS;
		now = at_clock_counted(&ut, ticks, counted);
		if (packet == 0)
			first = now;
		else
			mu_assert_int_eq(100 * STEP_TICKS, now - previous);
		mu_assert_int_eq(100 * (ticks - FIRST_TICKS), now - first);
		previous = now;
	}
	mu_assert_int_eq(wraps, ut.clk100ns_upper);
}

MU_TEST(several_wraps_stay_continuous)
{
	/* 24 packets 100 s apart span seven CLKN-derived periods, and five of
	 * timer1's. Both the count a Classic packet carries and the fallback
	 * for packets without one have to see every wrap. */
	continuous_over_wraps(CLK100NS_PERIOD, 24, 7, 1);
	continuous_over_wraps(CLK100NS_PERIOD, 24, 7, 0);
	continuous_over_wraps(CLK100NS_TIMER1_PERIOD, 24, 5, 0);
}

/* A gap longer than one period: the clock reads higher than it did, not
 * lower, so there is nothing to infer a wrap from and the count the packet
 * carries is the only record of the periods that went by. */
MU_TEST(a_gap_longer_than_a_period_is_taken_from_the_packet)
{
	ubertooth_t ut;
	uint64_t gap = 2 * (uint64_t)CLK100NS_PERIOD + STEP_TICKS;
	uint64_t before, after;

	start_capture(&ut, CLK100NS_PERIOD);
	before = at_clock_counted(&ut, FIRST_TICKS, 1);
	after = at_clock_counted(&ut, FIRST_TICKS + gap, 1);

	mu_assert_int_eq(2, ut.clk100ns_upper);
	mu_assert_int_eq(100 * gap, after - before);
}

/* The count is one byte, so it is read as a difference: a capture that is
 * already past the 256th period keeps counting across the byte's own wrap. */
MU_TEST(the_count_in_the_packet_wraps_without_losing_a_period)
{
	ubertooth_t ut;
	uint64_t first = 255 * (uint64_t)CLK100NS_PERIOD + FIRST_TICKS;
	uint64_t before, after;

	start_capture(&ut, CLK100NS_PERIOD);
	at_clock_counted(&ut, first, 1);
	before = at_clock_counted(&ut, first + STEP_TICKS, 1);
	/* The count in this last packet is back to zero. */
	after = at_clock_counted(&ut, first + STEP_TICKS + CLK100NS_PERIOD, 1);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100 * (uint64_t)CLK100NS_PERIOD, after - before);
}

/* Feeds a replay of two packets a gap apart, where each packet's host time is
 * the one its record carries, and returns what the library wrote to standard
 * error. */
static void replay_gap(ubertooth_t* ut, uint64_t period, int counted,
                       uint64_t first, uint64_t gap, uint64_t times[2],
                       char* text, size_t size)
{
	/* The first packet's host time: a real one, so a capture whose times
	 * fall a day short of it is still a time, and a whole second, so the
	 * seconds the records carry are exact. */
	uint32_t base = 1700000000;
	size_t n;
	FILE* capture = tmpfile();
	FILE* replay = tmpfile();
	int saved = dup(fileno(stderr));

	text[0] = '\0';
	mu_assert(capture != NULL && replay != NULL && saved >= 0,
	          "cannot capture standard error");
	if (capture == NULL || replay == NULL || saved < 0)
		return;

	start_capture(ut, period);
	infile = replay;
	host_time_follows_the_clock = 0;
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
	systime = base;
	times[0] = at_clock_counted(ut, first, counted);
	/* The host clock keeps running through the gap, whatever the capture
	 * clock did: 100 ns a tick. */
	systime = base + (uint32_t)(gap / 10000000ull);
	times[1] = at_clock_counted(ut, first + gap, counted);
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	infile = NULL;
	fclose(replay);

	rewind(capture);
	n = fread(text, 1, size - 1, capture);
	text[n] = '\0';
	fclose(capture);
}

/* Packets that carry no count leave the wrap to be inferred, so a gap longer
 * than one period still loses it: the modes behind ubertooth-btle -f and -n
 * are in that position. The host clock is the only evidence that it happened,
 * and it is reported rather than used to correct the times. */
MU_TEST(a_lost_wrap_is_reported_where_the_packets_carry_no_count)
{
	ubertooth_t ut;
	uint64_t times[2];
	char text[400];

	replay_gap(&ut, CLK100NS_TIMER1_PERIOD, 0, FIRST_TICKS,
	           2 * CLK100NS_TIMER1_PERIOD + STEP_TICKS, times,
	           text, sizeof(text));

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert(strstr(text, "warning: packet times are behind the host clock "
	                 "by more than half the capture clock's 429.50 s period")
	          != NULL, text);
}

/* One lost wrap is the whole of the issue for those modes, and it leaves the
 * times exactly one period short, so the report cannot wait for a second. */
MU_TEST(a_single_lost_wrap_is_reported_too)
{
	ubertooth_t ut;
	uint64_t times[2];
	char text[400];

	replay_gap(&ut, CLK100NS_TIMER1_PERIOD, 0, FIRST_TICKS,
	           CLK100NS_TIMER1_PERIOD + STEP_TICKS, times,
	           text, sizeof(text));

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert(strstr(text, "warning: packet times are behind the host clock "
	                 "by more than half the capture clock's 429.50 s period")
	          != NULL, text);
}

/* A capture whose times keep up with the host clock says nothing, so the
 * count a Classic packet carries silences the report as well as fixing the
 * times. */
MU_TEST(a_capture_that_keeps_up_reports_nothing)
{
	ubertooth_t ut;
	uint64_t gap = 2 * (uint64_t)CLK100NS_PERIOD + STEP_TICKS;
	uint64_t times[2];
	char text[400];

	replay_gap(&ut, CLK100NS_PERIOD, 1, FIRST_TICKS, gap, times,
	           text, sizeof(text));

	mu_assert_int_eq(2, ut.clk100ns_upper);
	mu_assert_int_eq(100 * gap, times[1] - times[0]);
	mu_assert_int_eq(0, (int)strlen(text));
}

/* A wrap the capture was quiet across still counts, however little the clock
 * dropped. 300 s into the period, then 200 s later, the clock reads 172.32 s:
 * a drop of only 127.68 s, less than half the period, so how far it dropped
 * cannot be what tells a wrap from a record out of place. The host clock saw
 * the 200 s, which is what the wrap claims, so the wrap stands. */
MU_TEST(a_wrap_the_host_clock_agrees_with_still_counts)
{
	ubertooth_t ut;
	uint64_t gap = 200 * ONE_SECOND_TICKS;
	uint64_t times[2];
	char text[400];

	replay_gap(&ut, CLK100NS_PERIOD, 0, 300 * ONE_SECOND_TICKS, gap, times,
	           text, sizeof(text));

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100 * gap, times[1] - times[0]);
	mu_assert_int_eq(0, (int)strlen(text));
}

/* The count is one byte, so a gap of a whole multiple of 256 periods leaves
 * it reading as it did and cannot be counted at all. The clock reading lower
 * than it did says a period went by, but it cannot say how many, and against
 * a count that has arrived it is not evidence of anything: the packet caught
 * as the firmware latches a new count carries one a period ahead of its own
 * clock reading, which looks exactly the same. So the gap is left out of the
 * times, and the host clock reports that they are short. */
MU_TEST(a_gap_of_a_whole_multiple_of_256_periods_is_reported_not_guessed)
{
	ubertooth_t ut;
	/* Starting past the first period, so a count has arrived. The gap is
	 * 256 periods less a step, which leaves the clock reading a step lower
	 * than it did and the count reading the same. */
	uint64_t first = 3 * (uint64_t)CLK100NS_PERIOD + FIRST_TICKS + STEP_TICKS;
	uint64_t gap = 256 * (uint64_t)CLK100NS_PERIOD - STEP_TICKS;
	uint64_t times[2];
	char text[400];

	replay_gap(&ut, CLK100NS_PERIOD, 1, first, gap, times,
	           text, sizeof(text));

	/* No periods counted, so the whole gap is missing from the times. This
	 * gap falls between the capture's first two packets, where the host
	 * clock knows the count cannot be the time between them: the anchor
	 * moves to the second packet, which starts the capture rather than
	 * landing a step before the first. Either way nothing guesses at the
	 * gap, and the report below is what says it went missing. */
	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(0, times[1] - times[0]);
	mu_assert(strstr(text, "warning: packet times are behind the host clock "
	                 "by more than half the capture clock's 327.68 s period")
	          != NULL, text);
}

/* The same gap once the capture is under way. No packet is in doubt there, so
 * the anchor stays put and the packet after the gap reads a step before the
 * one before it, which is how far an uncountable gap can throw the times out
 * without any wrap being guessed at. */
MU_TEST(a_gap_of_256_periods_further_in_leaves_the_anchor_alone)
{
	ubertooth_t ut;
	uint64_t gap = 256 * (uint64_t)CLK100NS_PERIOD - STEP_TICKS;
	const uint64_t ticks[3] = { 3 * (uint64_t)CLK100NS_PERIOD + FIRST_TICKS,
	                            3 * (uint64_t)CLK100NS_PERIOD + FIRST_TICKS
	                            + STEP_TICKS,
	                            3 * (uint64_t)CLK100NS_PERIOD + FIRST_TICKS
	                            + STEP_TICKS + gap };
	const uint32_t host_s[3] = { 0, 100,
	                             (uint32_t)(100 + gap / ONE_SECOND_TICKS) };
	const uint8_t counts[3] = { 3, 3, 3 };
	uint64_t times[3];

	replay_records(&ut, CLK100NS_PERIOD, 3, ticks, host_s, counts, times);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(ut.abs_start_ns, times[0]);
	mu_assert_int_eq(100 * STEP_TICKS, times[1] - times[2]);
}

/* A count that reads lower than the last packet's is a record out of order or
 * edited, not 255 periods of time passing, which is what reading the one-byte
 * difference as a forward one would make of it. */
MU_TEST(a_count_that_reads_backwards_does_not_move_the_clock)
{
	ubertooth_t ut;
	uint64_t first = 5 * (uint64_t)CLK100NS_PERIOD + FIRST_TICKS;
	uint64_t before, after, back;

	start_capture(&ut, CLK100NS_PERIOD);
	before = at_clock_counted(&ut, first, 1);
	/* The same clock a step on, but stamped with the period before it. */
	back = at_clock_with_count(&ut, first + STEP_TICKS,
	                           (uint8_t)(first / CLK100NS_PERIOD - 1));

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(100 * STEP_TICKS, back - before);

	/* A record that is in order again is measured from the count last
	 * believed, not from the one just rejected, so the climb back out of it
	 * is not counted as a period of its own. */
	after = at_clock_counted(&ut, first + 2 * STEP_TICKS, 1);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(100 * 2 * STEP_TICKS, after - before);
}

/* The firmware latches a packet's clock reading and its period count with two
 * reads of CLKN, so a packet caught between them carries a count a period
 * ahead of its clock reading. That is one packet's time out; the clock
 * reading lower than it did on the next packet must not be taken as a second
 * period, or every time after it is a period too high, permanently. */
MU_TEST(a_count_latched_a_period_early_costs_only_its_own_packet)
{
	ubertooth_t ut;
	uint64_t anchor = 3 * (uint64_t)CLK100NS_PERIOD + FIRST_TICKS;
	uint64_t at_the_boundary = 4 * (uint64_t)CLK100NS_PERIOD - STEP_TICKS;
	uint64_t after_it = 4 * (uint64_t)CLK100NS_PERIOD + STEP_TICKS;
	uint64_t first, glitched, next;

	start_capture(&ut, CLK100NS_PERIOD);
	first = at_clock_counted(&ut, anchor, 1);
	/* The clock is still in the period before, the count already past it. */
	glitched = at_clock_with_count(&ut, at_the_boundary, 4);
	next = at_clock_counted(&ut, after_it, 1);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	/* The glitched packet is a period ahead of itself, and the one after it
	 * is back where it belongs. */
	mu_assert_int_eq(100 * (at_the_boundary - anchor + CLK100NS_PERIOD),
	                 glitched - first);
	mu_assert_int_eq(100 * (after_it - anchor), next - first);
}

/* Stopping a capture puts the clock tracking back where a capture starts, so
 * a handle used again does not carry the wrap count of the capture that
 * ended into the next one's timestamps. */
MU_TEST(a_reused_handle_starts_the_next_capture_fresh)
{
	ubertooth_t ut;
	uint64_t first, second;

	start_capture(&ut, CLK100NS_PERIOD);
	at_clock_counted(&ut, 3 * STEP_TICKS, 1);
	at_clock_counted(&ut, 4 * STEP_TICKS, 1);
	mu_assert_int_eq(1, ut.clk100ns_upper);

	/* No device was opened, so this closes nothing; libusb is initialised
	 * so the cleanup's matching exit has a context to release. */
	mu_assert_int_eq(0, libusb_init(NULL));
	ubertooth_stop(&ut);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(0, ut.clk100ns_started);
	mu_assert_int_eq(0, ut.clk100ns_counted);
	mu_assert_int_eq(0, ut.last_clkn_high);
	mu_assert_int_eq(0, ut.last_arrival_ns);
	mu_assert_int_eq(0, ut.clk100ns_anchor_open);
	mu_assert_int_eq(0, ut.clk100ns_gap_warned);

	/* The second capture is anchored on its own first packet, which starts
	 * it rather than landing a period after it, and counts its own wraps. */
	first = at_clock_counted(&ut, 3 * STEP_TICKS, 1);
	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(0, first - ut.abs_start_ns);
	second = at_clock_counted(&ut, 4 * STEP_TICKS, 1);
	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100 * STEP_TICKS, second - first);
}

/* Which clock stamps the packets is the tool's to choose, not tracking state,
 * so stopping a capture leaves it as the tool set it: a tool that sets it and
 * then stops before streaming would otherwise stamp the capture after that
 * with the wrong period. */
MU_TEST(stopping_a_capture_leaves_the_chosen_clock_alone)
{
	ubertooth_t ut;

	start_capture(&ut, CLK100NS_TIMER1_PERIOD);
	mu_assert_int_eq(0, libusb_init(NULL));
	ubertooth_stop(&ut);

	mu_assert(ut.clk100ns_period == CLK100NS_TIMER1_PERIOD,
	          "the period the tool chose");
}

/* A live capture that went quiet across a wrap, taken in by a host that was
 * not reading. The board's clock ran 200 s from 300 s into its period, so the
 * second record reads 172.32 s - lower than the first, which between a
 * capture's first two records can also mean the first was a record left over
 * from the mode before. The 200 s the records arrived apart is what says this
 * one is a wrap, and that is the whole of the evidence: by the time the host
 * got round to either record they were both waiting, microseconds apart. */
MU_TEST(a_wrap_the_host_was_not_reading_through_still_counts)
{
	ubertooth_t ut;
	uint64_t gap = 200 * ONE_SECOND_TICKS;
	const uint64_t ticks[2] = { 300 * ONE_SECOND_TICKS,
	                            300 * ONE_SECOND_TICKS + gap };
	const uint64_t arrived_ns[2] = { 0, 100 * gap };
	uint64_t times[2];

	capture_records(&ut, CLK100NS_PERIOD, 2, ticks, arrived_ns, NULL, times);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(100 * gap, times[1] - times[0]);
}

/* The same stall against the period count a record carries, which is the
 * costlier of the two decisions. The capture's first two records are 665.36 s
 * apart - two whole periods and 10 s - and their counts say so, which the host
 * clock has to agree with for the count to be believed. Measured where the host
 * reached the records, the gap would be microseconds, the count would be
 * rejected as the previous mode's, and the capture would start at its second
 * record with those 665 s gone from every time in it. */
MU_TEST(a_stall_does_not_reject_a_counted_gap_at_the_start)
{
	ubertooth_t ut;
	uint64_t gap = 2 * (uint64_t) CLK100NS_PERIOD + 10 * ONE_SECOND_TICKS;
	const uint64_t ticks[2] = { 1 * (uint64_t) CLK100NS_PERIOD
	                            + 10 * ONE_SECOND_TICKS,
	                            1 * (uint64_t) CLK100NS_PERIOD
	                            + 10 * ONE_SECOND_TICKS + gap };
	const uint64_t arrived_ns[2] = { 0, 100 * gap };
	const uint8_t counts[2] = { 1, 3 };
	uint64_t times[2];

	capture_records(&ut, CLK100NS_PERIOD, 2, ticks, arrived_ns, counts,
	                times);

	mu_assert_int_eq(2, ut.clk100ns_upper);
	mu_assert_int_eq(3, ut.last_clkn_high);
	mu_assert_int_eq(100 * gap, times[1] - times[0]);
}

/* A live capture that really did open with a record left over from the mode
 * before. The leftover reads 30 s into the clock and the capture's own two
 * records read 1 s and 2 s, so the second reads lower; what makes it a
 * leftover rather than a wrap is that it arrived a millisecond before the
 * record after it, with no room for the 298.68 s a wrap there would mean. The
 * capture is anchored on its own first record.
 *
 * This case comes out the same whichever reading the gap is measured on, since
 * a leftover record is one that arrived close to the record after it either
 * way. What it covers is that the live path reaches the decision at all, which
 * no case could check before. */
MU_TEST(a_leftover_first_record_is_found_on_a_live_capture)
{
	ubertooth_t ut;
	const uint64_t ticks[3] = { 30 * ONE_SECOND_TICKS,
	                             1 * ONE_SECOND_TICKS,
	                             2 * ONE_SECOND_TICKS };
	const uint64_t arrived_ns[3] = { 0, 1000000ull, 2000000ull };
	uint64_t times[3];

	capture_records(&ut, CLK100NS_PERIOD, 3, ticks, arrived_ns, NULL, times);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(ut.abs_start_ns, times[1]);
	mu_assert_int_eq(100 * ONE_SECOND_TICKS, times[2] - times[1]);
}

/* A live capture whose first record reads below the records after it rather
 * than above them: a record left over from a mode whose clock had not run as
 * far as this one's. Anchored on it, every later time came out up to a period
 * in the future, and nothing said so - the report below only looks for times
 * behind the host clock. The records arrived a millisecond apart, so the 290 s
 * between the first two readings never passed, whichever way round they are,
 * and the capture is anchored on its own first record. */
MU_TEST(a_first_record_reading_below_the_capture_moves_the_anchor)
{
	ubertooth_t ut;
	const uint64_t ticks[3] = { 10 * ONE_SECOND_TICKS,
	                           300 * ONE_SECOND_TICKS,
	                           301 * ONE_SECOND_TICKS };
	const uint64_t arrived_ns[3] = { 0, 1000000ull, 1001000000ull };
	uint64_t times[3];

	capture_records(&ut, CLK100NS_PERIOD, 3, ticks, arrived_ns, NULL, times);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(ut.abs_start_ns, times[1]);
	mu_assert_int_eq(100 * ONE_SECOND_TICKS, times[2] - times[1]);
}

/* ---- a live capture quiet for longer than a period --------------------- */

/* A live capture of three records carrying no count: two a second apart from
 * `start` ticks into the board's clock, then one after a quiet gap of `gap`
 * ticks. Every record is pushed before any is popped, so the host reaches
 * them microseconds apart however long the gap really was, and the readings
 * they arrived with are the whole of the evidence for it.
 *
 * The time between the last two records has to be the gap, however many times
 * the clock started over in it and whether it came back reading higher or
 * lower than it did. */
static void a_live_gap_of(uint64_t period, uint64_t start, uint64_t gap)
{
	ubertooth_t ut;
	const uint64_t ticks[3] = { start, start + ONE_SECOND_TICKS,
	                            start + ONE_SECOND_TICKS + gap };
	/* Three milliseconds later than the board's clock makes the gap, which
	 * is the sort of difference a real one has: the records arrive a
	 * packet's latency after the air, and the two clocks drift. The wrap
	 * count has to come out the same for a measure that is merely close. */
	const uint64_t arrived_ns[3] = { 0, 100 * ONE_SECOND_TICKS,
	                                 100 * (ONE_SECOND_TICKS + gap)
	                                 + 3000000ull };
	uint64_t times[3];

	capture_records(&ut, period, 3, ticks, arrived_ns, NULL, times);

	mu_assert_int_eq(ticks[2] / period - ticks[0] / period,
	                 ut.clk100ns_upper);
	mu_assert_int_eq(100 * gap, times[2] - times[1]);
	mu_assert_int_eq(100 * (ticks[2] - ticks[0]), times[2] - times[0]);
}

/* Quiet gaps of every shape against the clock's period, each tried from three
 * points in the period so that it comes back reading both higher and lower
 * than it did. A gap of more than a period that comes back reading higher is
 * the case nothing in the capture shows at all: the clock simply reads on
 * from where it was, with whole periods missing. The gap the records arrived
 * apart is what counts those periods, so all of these keep their time. */
MU_TEST(a_quiet_live_gap_keeps_its_time_however_long_it_was)
{
	/* 0.4, 0.6, 1.5 and 3 periods, in tenths of one. */
	static const uint64_t tenths[] = { 4, 6, 15, 30 };
	static const uint64_t starts[] = { 0, 3, 8 };
	size_t gap, start;

	for (gap = 0; gap < sizeof(tenths) / sizeof(tenths[0]); gap++)
		for (start = 0; start < sizeof(starts) / sizeof(starts[0]);
		     start++)
			/* A second in from each point, so the first two
			 * records are never split by a wrap of their own. */
			a_live_gap_of(CLK100NS_PERIOD,
			              starts[start] * (CLK100NS_PERIOD / 10)
			              + ONE_SECOND_TICKS,
			              tenths[gap] * (CLK100NS_PERIOD / 10));

	/* The low-energy engine's clock has its own, longer period, and the
	 * modes that stamp with it send no count at all, so a quiet gap there
	 * is this fix and nothing else. One and a half of its periods. */
	a_live_gap_of(CLK100NS_TIMER1_PERIOD, ONE_SECOND_TICKS,
	              3 * (CLK100NS_TIMER1_PERIOD / 2));
}

/* A wrap further into a live capture, with 10 s between the two records and
 * no processing gap at all: the host had both of them waiting by the time it
 * read either. Measured where the loop reaches a record the gap would be
 * microseconds, so a test of the wrap against it would refuse the wrap and
 * take a period off every time after it. */
MU_TEST(a_wrap_a_backlog_hid_still_counts)
{
	a_live_gap_of(CLK100NS_PERIOD,
	              CLK100NS_PERIOD - 6 * ONE_SECOND_TICKS,
	              10 * ONE_SECOND_TICKS);
}

/* A quiet gap of more than a period between the capture's own first two
 * records, which is where the wrap count and the leftover decision meet: the
 * gap has to be counted and the first record left as the anchor. A capture
 * that opened on a quiet channel reaches this. The host's measure is given
 * 1.9 s short of what the board's clock makes it - just inside the slack - so
 * the periods are counted on a measure that is close rather than exact. */
MU_TEST(a_long_quiet_gap_at_the_start_is_not_a_leftover_record)
{
	ubertooth_t ut;
	const uint64_t gap = 3 * ((uint64_t) CLK100NS_PERIOD / 2);
	const uint64_t ticks[2] = { 100 * ONE_SECOND_TICKS,
	                            100 * ONE_SECOND_TICKS + gap };
	const uint64_t arrived_ns[2] = { 0, 100 * gap - 1900000000ull };
	uint64_t times[2];

	capture_records(&ut, CLK100NS_PERIOD, 2, ticks, arrived_ns, NULL, times);

	mu_assert_int_eq(1, ut.clk100ns_upper);
	mu_assert_int_eq(ut.abs_start_ns, times[0]);
	mu_assert_int_eq(100 * gap, times[1] - times[0]);
}

/* A record out of place further into a live capture, reading 50 s below the
 * records either side of it and arriving a millisecond after the one before.
 * Fifty seconds is more time than the host saw pass, so no wrap is counted
 * and the records after it keep their times; only the stray's own time is
 * where its reading puts it. Read as the clock starting over, as a drop
 * further in used to be, it added 327.68 s to every record after it. */
MU_TEST(a_stray_record_further_into_a_live_capture_adds_nothing)
{
	ubertooth_t ut;
	const uint64_t ticks[4] = { 100 * ONE_SECOND_TICKS,
	                            101 * ONE_SECOND_TICKS,
	                             51 * ONE_SECOND_TICKS,   /* out of place */
	                            102 * ONE_SECOND_TICKS };
	const uint64_t arrived_ns[4] = { 0, 1000000000ull, 1001000000ull,
	                                 2000000000ull };
	uint64_t times[4];

	capture_records(&ut, CLK100NS_PERIOD, 4, ticks, arrived_ns, NULL, times);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(ut.abs_start_ns, times[0]);
	mu_assert_int_eq(100 * ONE_SECOND_TICKS, times[1] - times[0]);
	mu_assert_int_eq(100 * 2 * ONE_SECOND_TICKS, times[3] - times[0]);
}

/* A live capture's times are where it was anchored plus the board's own clock
 * from there, and the wraps of that clock are counted on the monotonic
 * reading each record arrived with. So the host time a record carries - the
 * reading a step of the host clock moves, and what a replay goes by - takes
 * no part in them. The same records come out the same here with that reading
 * stepped two hours back between the two captures, gap and all. */
MU_TEST(a_host_clock_that_steps_does_not_move_live_times)
{
	ubertooth_t ut;
	/* A gap of one and a half periods, which the clock comes back from
	 * reading half a period higher than it did. */
	const uint64_t gap = 3 * ((uint64_t) CLK100NS_PERIOD / 2);
	const uint64_t ticks[3] = { ONE_SECOND_TICKS, 2 * ONE_SECOND_TICKS,
	                            2 * ONE_SECOND_TICKS + gap };
	const uint64_t arrived_ns[3] = { 0, 100 * ONE_SECOND_TICKS,
	                                 100 * (ONE_SECOND_TICKS + gap) };
	uint64_t ahead[3], stepped[3];
	uint64_t upper;

	systime = 1700000000;
	capture_records(&ut, CLK100NS_PERIOD, 3, ticks, arrived_ns, NULL, ahead);
	upper = ut.clk100ns_upper;

	systime = 1700000000 - 7200;
	capture_records(&ut, CLK100NS_PERIOD, 3, ticks, arrived_ns, NULL,
	                stepped);

	mu_assert_int_eq(1, upper);
	mu_assert_int_eq(upper, ut.clk100ns_upper);
	/* Every time is the board's clock from the first record, in both. */
	mu_assert_int_eq(100 * (ticks[1] - ticks[0]), ahead[1] - ahead[0]);
	mu_assert_int_eq(100 * (ticks[2] - ticks[0]), ahead[2] - ahead[0]);
	mu_assert_int_eq(ahead[1] - ahead[0], stepped[1] - stepped[0]);
	mu_assert_int_eq(ahead[2] - ahead[0], stepped[2] - stepped[0]);
}

/* Captures one live record whose arrival reading the case gives outright, and
 * reports the host time read either side of the loop taking it. Where the
 * capture ended up anchored, ut.abs_start_ns, is then checked against those
 * two readings: the loop ran between them, so they bracket the answer a
 * capture anchored where the loop reached the record would give. */
static void capture_one_record_arriving_at(ubertooth_t* ut, uint64_t arrival_ns,
                                           uint64_t* before_ns,
                                           uint64_t* after_ns)
{
	usb_pkt_rx rx, popped;

	start_live_capture(ut, CLK100NS_PERIOD);
	mu_assert_fatal(ut->fifo != NULL, "cannot allocate the ring buffer");
	memset(&rx, 0, sizeof(rx));
	rx.clk100ns = htole32((uint32_t) FIRST_TICKS);
	fifo_push(ut->fifo, &rx, arrival_ns);
	*before_ns = clock_ns(CLOCK_REALTIME);
	popped = fifo_pop(ut->fifo);
	now_ns_from_clk100ns(ut, &popped, fifo_popped_arrival_ns(ut->fifo));
	*after_ns = clock_ns(CLOCK_REALTIME);
	free(ut->fifo);
	ut->fifo = NULL;
}

/* Whether the capture was anchored this far before the host time read next to
 * the loop, within the slack above. */
static int anchored_this_far_back(const ubertooth_t* ut, uint64_t read_ns,
                                  uint64_t back_ns)
{
	int64_t measured = (int64_t) read_ns - (int64_t) ut->abs_start_ns;
	int64_t apart = measured - (int64_t) back_ns;

	return (apart < 0 ? -apart : apart) <= (int64_t) ANCHOR_SLACK_NS;
}

/* A host slow to reach its first record anchors the capture where that record
 * arrived, not where it got round to it. A capture written to a pipe nothing
 * is reading yet, or taken by a process that was descheduled, reaches its
 * first record seconds after the record came in; anchored there, every
 * wall-clock time in the capture is late by the whole of that delay, and
 * nothing inside the capture shows it because the times stay consistent with
 * each other. What places the record is the monotonic reading it arrived
 * with, turned into a host time by reading both clocks together. */
MU_TEST(a_stalled_host_anchors_the_capture_where_the_record_arrived)
{
	ubertooth_t ut;
	uint64_t arrival_ns = clock_ns(CLOCK_MONOTONIC) - STALL_NS;
	uint64_t before_ns, after_ns;

	capture_one_record_arriving_at(&ut, arrival_ns, &before_ns, &after_ns);

	/* A stall back from where the loop reached the record. Anchored where
	 * the loop reached it, as it was, this would be the whole stall out. */
	mu_assert(anchored_this_far_back(&ut, before_ns, STALL_NS),
	          "the capture is not anchored where the record arrived");
	mu_assert(ut.abs_start_ns < after_ns,
	          "the anchor is not before the loop read the record");
}

/* A host that was reading promptly is left as it was: its first record
 * arrived moments before the loop reached it, so the anchor stays where the
 * host clock reads now, which is the normal case and the commonest one. */
MU_TEST(a_prompt_host_anchors_the_capture_where_it_always_did)
{
	ubertooth_t ut;
	uint64_t before_ns, after_ns;

	capture_one_record_arriving_at(&ut, clock_ns(CLOCK_MONOTONIC),
	                               &before_ns, &after_ns);

	mu_assert(anchored_this_far_back(&ut, before_ns, 0),
	          "a prompt capture's anchor moved");
	mu_assert(ut.abs_start_ns <= after_ns,
	          "the anchor is after the loop read the record");
}

/* A record with no arrival reading at all cannot be placed, so the capture is
 * anchored at the host time read next to the loop, exactly as it was before.
 * A slot nothing stamped reads back as zero, which is what a ring buffer
 * holding records from a producer that does not stamp them would give. */
MU_TEST(a_record_with_no_arrival_reading_is_anchored_where_it_is_read)
{
	ubertooth_t ut;
	uint64_t before_ns, after_ns;

	capture_one_record_arriving_at(&ut, 0, &before_ns, &after_ns);

	mu_assert(ut.abs_start_ns >= before_ns && ut.abs_start_ns <= after_ns,
	          "the capture is not anchored where the loop read the record");
}

/* Nor can a reading from after the monotonic reading just taken, which no
 * record can honestly carry: taking it off would put the anchor in the
 * future. Same fallback. */
MU_TEST(an_arrival_reading_in_the_future_is_anchored_where_it_is_read)
{
	ubertooth_t ut;
	uint64_t arrival_ns = clock_ns(CLOCK_MONOTONIC) + 10000000000ull;
	uint64_t before_ns, after_ns;

	capture_one_record_arriving_at(&ut, arrival_ns, &before_ns, &after_ns);

	mu_assert(ut.abs_start_ns >= before_ns && ut.abs_start_ns <= after_ns,
	          "the capture is not anchored where the loop read the record");
}

/* The arithmetic behind those cases, on readings the machine running the test
 * cannot be made to produce. The last one is a host whose clock has never
 * been set: its current time is a few seconds while the monotonic clock
 * counts from a boot before that, so subtracting the wait would land before
 * 1970. The host time read now is wrong by the stall, but it is at least a
 * time within the run, so that is what a reading like this falls back to. */
MU_TEST(a_reading_that_cannot_place_a_record_leaves_the_host_time_alone)
{
	const uint64_t real_ns = 1700000000000000000ull;
	const uint64_t mono_ns = 86400000000000ull;   /* a day of uptime */

	/* Placed by the two clocks together: the wait comes off. */
	mu_assert_int_eq(real_ns - STALL_NS,
	                 host_ns_at_arrival(real_ns, mono_ns,
	                                    mono_ns - STALL_NS));

	mu_assert_int_eq(real_ns, host_ns_at_arrival(real_ns, mono_ns, 0));
	mu_assert_int_eq(real_ns, host_ns_at_arrival(real_ns, mono_ns,
	                                             mono_ns + STALL_NS));
	/* Ten seconds since the epoch, with a day of uptime behind it. */
	mu_assert_int_eq(10000000000ull,
	                 host_ns_at_arrival(10000000000ull, mono_ns, 1000ull));
}

/* ---- the host time a dump carries -------------------------------------- */

/* The other half of the same question, run through the real callbacks: what a
 * `-d` dump stores with each record, which is also what the systime= column
 * prints and what a replay of that dump has to go on. It is now worked out
 * from the same arrival reading as the capture's own times, so the two
 * outputs of one run describe the same moment. Stamped from the clock read
 * where the loop reached the record, as it was, a stalled host's dump carried
 * every time late and disagreed with the capture written beside it.
 *
 * A dump stores whole seconds, so a time read back from one lands at the
 * moment it came from or up to a second before it, never after. */

/* Which callback a case drives. `ubertooth-rx -d <file> -q <file>`, the
 * command the issue is about, reaches the basic-rate one; the low-energy one
 * stamps and dumps a record the same way. The scan one writes no dump and no
 * capture, so the printed column is the whole of what it stamps. */
enum stamped_mode {
	STAMPED_BASIC_RATE,
	STAMPED_LOW_ENERGY,
	STAMPED_SCAN,
};

/* A low-energy advertising packet the decoder takes: the advertising access
 * address, little endian, then an ADV_IND header whose length matches the six
 * bytes of payload after it. */
static void advertising_record(usb_pkt_rx* rx)
{
	memset(rx, 0, sizeof(*rx));
	rx->pkt_type = LE_PACKET;
	rx->channel = 0;
	rx->clk100ns = htole32((uint32_t) FIRST_TICKS);
	rx->data[0] = 0xd6;
	rx->data[1] = 0xbe;
	rx->data[2] = 0x89;
	rx->data[3] = 0x8e;
	rx->data[4] = 0x00;
	rx->data[5] = 6;
}

/* The LAP the basic-rate record below carries, the 64-bit sequence its sync
 * word is scrambled with, the (64,30) expurgated BCH generator, and the
 * Barker sequence the LAP's top bit chooses. */
#define BASIC_RATE_LAP     0x5a3c96u
#define SYNC_WORD_SCRAMBLE 0x83848d96bbcc54fcull
#define SYNC_WORD_BCH      0260534236651ull
#define SYNC_WORD_BARKER_0 0x2cu
#define SYNC_WORD_BARKER_1 0x13u

/* The 64-bit sync word of a LAP, built from the Bluetooth Core specification
 * (Vol 2, Part B 6.3.3): the LAP and its Barker sequence, scrambled, then the
 * BCH parity of that. Bit i is the i-th symbol on air. Building it here is
 * what lets a basic-rate case run with no board and no saved capture. */
static uint64_t sync_word(uint32_t lap)
{
	uint64_t information, scrambled, parity;
	int bit;

	lap &= 0xffffffu;
	information = lap | ((uint64_t) ((lap >> 23) ? SYNC_WORD_BARKER_1 :
	                                               SYNC_WORD_BARKER_0) << 24);
	scrambled = information ^ (SYNC_WORD_SCRAMBLE >> 34);
	parity = scrambled << 34;
	for (bit = 63; bit >= 34; bit--) {
		if ((parity >> bit) & 1)
			parity ^= SYNC_WORD_BCH << (bit - 34);
	}
	return ((scrambled << 34) | parity) ^ SYNC_WORD_SCRAMBLE;
}

/* A basic-rate packet the decoder finds: silence, then the preamble and sync
 * word of one LAP, then silence again, packed most significant bit first the
 * way the board sends symbols. */
static void basic_rate_record(usb_pkt_rx* rx)
{
	/* Where the access code starts, far enough in that the decoder has
	 * room to search before it. */
	enum { PREAMBLE_AT = 16 };
	uint8_t symbols[SYM_LEN * 8] = { 0 };
	uint64_t word = sync_word(BASIC_RATE_LAP);
	int i, j;

	memset(rx, 0, sizeof(*rx));
	rx->pkt_type = BR_PACKET;
	rx->channel = 0;
	rx->clk100ns = htole32((uint32_t) FIRST_TICKS);
	rx->rssi_max = -40;
	rx->rssi_min = -90;
	rx->rssi_avg = -70;
	rx->rssi_count = 1;

	/* The preamble is the two symbols that lead into the sync word's first
	 * bit, so which pair it is follows that bit. */
	symbols[PREAMBLE_AT + 0] = (word & 1) ? 1 : 0;
	symbols[PREAMBLE_AT + 1] = (word & 1) ? 0 : 1;
	symbols[PREAMBLE_AT + 2] = (word & 1) ? 1 : 0;
	symbols[PREAMBLE_AT + 3] = (word & 1) ? 0 : 1;
	for (i = 0; i < 64; i++)
		symbols[PREAMBLE_AT + 4 + i] = (word >> i) & 1;

	for (i = 0; i < SYM_LEN; i++) {
		for (j = 0; j < 8; j++)
			rx->data[i] |= symbols[i * 8 + j] << (7 - j);
	}
}

/* The host time the record at this position in a dump carries: a big-endian
 * 32-bit value ahead of the 64 bytes of the record itself. 0 where the dump
 * holds no whole record there. */
static uint32_t dump_record_systime(FILE* dump, int position)
{
	uint8_t record[RX_FILE_RECORD_LEN];

	if (fseek(dump, (long) position * RX_FILE_RECORD_LEN, SEEK_SET) != 0 ||
	    fread(record, 1, sizeof(record), dump) != sizeof(record))
		return 0;
	return ((uint32_t) record[0] << 24) | ((uint32_t) record[1] << 16) |
	       ((uint32_t) record[2] << 8) | record[3];
}

/* The host time the packet line printed, from the systime= column of the
 * first line of a file the callback's output was sent to. 0 where no such
 * line is there. */
static uint32_t printed_systime(const char* path)
{
	FILE* fp = fopen(path, "r");
	char line[400];
	uint32_t printed = 0;

	if (fp == NULL)
		return 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (sscanf(line, "systime=%u", &printed) == 1)
			break;
		printed = 0;
	}
	fclose(fp);
	return printed;
}

/* The pcap magic number that says the times are nanoseconds. libbtbb writes
 * the header as a struct, so the file is in this machine's own byte order and
 * the fields read back without swapping. */
#define PCAP_NANOSECOND_MAGIC 0xa1b23c4du

/* The time the first packet of such a file carries, as nanoseconds. 0 where
 * the file is not one of these or holds no packet. */
static uint64_t first_capture_ns(const char* path)
{
	FILE* fp = fopen(path, "rb");
	uint32_t header[6];
	uint32_t record[4];
	int readable;

	if (fp == NULL)
		return 0;
	readable = fread(header, sizeof(header[0]), 6, fp) == 6 &&
	           header[0] == PCAP_NANOSECOND_MAGIC &&
	           fread(record, sizeof(record[0]), 4, fp) == 4;
	fclose(fp);
	if (!readable)
		return 0;
	return 1000000000ull * record[0] + record[1];
}

/* Runs one record through a real callback with a dump and a capture open at
 * once, which is what `ubertooth-rx -d <file> -q <file>` leaves it with. The
 * scan mode opens neither, as ubertooth-scan does not.
 *
 * stall_ns is how long before the callback takes it the record arrived, which
 * is what a host slow to get going leaves behind. replayed says this is a
 * replay instead: such a record has only the host time it was recorded with,
 * recorded_systime, which is also the arrival reading the library pushes for
 * it, so stall_ns does not apply.
 *
 * The host time the dump stored comes back in *stored, the one the packet
 * line printed in *printed, the time the capture gave the packet in
 * *capture_ns, and the moment the record arrived, read from the host clock,
 * in *arrived_ns (0 for a replay, which has no such moment of its own). */
static void run_one_record(enum stamped_mode mode, int replayed,
                           uint64_t stall_ns, uint32_t recorded_systime,
                           uint32_t* stored, uint32_t* printed,
                           uint64_t* capture_ns, uint64_t* arrived_ns)
{
	/* A directory of its own per run, so a file left behind by an earlier
	 * run, or one a parallel test is writing, cannot be read back here by
	 * mistake. */
	char dir[] = "/tmp/ubertooth-dump-time-XXXXXX";
	char capture_path[sizeof(dir) + 16];
	char output_path[sizeof(dir) + 16];
	ubertooth_t ut;
	usb_pkt_rx rx;
	FILE* dump;
	fifo_t* fifo;
	uint64_t arrival_ns;
	int saved_stdout, output;

	memset(&ut, 0, sizeof(ut));
	/* The period of the counter each mode's packets are stamped with, as
	 * the tool that starts that mode sets it. */
	ut.clk100ns_period = mode == STAMPED_LOW_ENERGY ? CLK100NS_TIMER1_PERIOD :
	                                                  CLK100NS_PERIOD;
	fifo = fifo_init();
	mu_assert_fatal(fifo != NULL, "cannot allocate the ring buffer");
	ut.fifo = fifo;

	mu_assert_fatal(mkdtemp(dir) != NULL, "cannot make a directory to write in");
	snprintf(capture_path, sizeof(capture_path), "%s/capture.pcap", dir);
	snprintf(output_path, sizeof(output_path), "%s/output", dir);
	dump = tmpfile();
	mu_assert_fatal(dump != NULL, "cannot open a dump to write");
	if (mode == STAMPED_LOW_ENERGY) {
		advertising_record(&rx);
		mu_assert_fatal(lell_pcap_create_file(capture_path,
		                                      &ut.h_pcap_le) == 0,
		                "cannot open a capture to write");
	} else {
		basic_rate_record(&rx);
		/* The decoder's error-correction tables, which finding an
		 * access code needs. */
		mu_assert_fatal(btbb_init(max_ac_errors) == 0,
		                "cannot set up the decoder");
		if (mode == STAMPED_BASIC_RATE)
			mu_assert_fatal(btbb_pcap_create_file(capture_path,
			                                      &ut.h_pcap_bredr) == 0,
			                "cannot open a capture to write");
	}

	/* The callback prints the packet line; send it to a file, both to keep
	 * it out of the test's own output and to read the systime= column it
	 * carries back. */
	fflush(stdout);
	saved_stdout = dup(fileno(stdout));
	mu_assert_fatal(saved_stdout >= 0, "cannot set the output aside");
	output = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	mu_assert_fatal(output >= 0, "cannot open a file for the output");
	dup2(output, fileno(stdout));
	close(output);

	/* The globals the callbacks read, set once everything they need is
	 * open: a case that stopped short of here would otherwise leave infile
	 * pointing at a file and send every later case down the replay
	 * branch. stdin stands in for a dump being replayed; nothing reads it,
	 * only its being non-NULL matters. */
	infile = replayed ? stdin : NULL;
	systime = replayed ? recorded_systime : 0;
	dumpfile = mode == STAMPED_SCAN ? NULL : dump;

	/* The two readings a live case compares, taken together so that only
	 * the moments between them are in play: when the record arrived, on
	 * the monotonic clock the ring buffer stamps records with, and the
	 * host time of that same moment. Anything slow between them would
	 * separate them and the assertions below would have to allow for it. */
	if (replayed) {
		arrival_ns = 1000000000ull * (uint64_t) recorded_systime;
		*arrived_ns = 0;
	} else {
		*arrived_ns = clock_ns(CLOCK_REALTIME) - stall_ns;
		arrival_ns = clock_ns(CLOCK_MONOTONIC) - stall_ns;
	}
	fifo_push(fifo, &rx, arrival_ns);

	switch (mode) {
	case STAMPED_LOW_ENERGY:
		cb_btle(&ut, NULL);
		break;
	case STAMPED_SCAN:
		cb_scan(&ut, NULL);
		break;
	case STAMPED_BASIC_RATE:
		cb_rx(&ut, NULL);
		break;
	}

	fflush(stdout);
	dup2(saved_stdout, fileno(stdout));
	close(saved_stdout);

	if (ut.h_pcap_le != NULL) {
		lell_pcap_close(ut.h_pcap_le);
		ut.h_pcap_le = NULL;
	}
	if (ut.h_pcap_bredr != NULL) {
		btbb_pcap_close(ut.h_pcap_bredr);
		ut.h_pcap_bredr = NULL;
	}
	*stored = dump_record_systime(dump, 0);
	*printed = printed_systime(output_path);
	*capture_ns = first_capture_ns(capture_path);

	fclose(dump);
	dumpfile = NULL;
	infile = NULL;
	free(fifo);
	ut.fifo = NULL;
	unlink(capture_path);
	unlink(output_path);
	rmdir(dir);
}

/* The whole second a host time written out as seconds was truncated to. */
#define DUMP_TRUNCATION_NS 1000000000ull

/* Whether a host time written out as whole seconds describes the moment
 * reference_ns is: truncated, it lands at that moment or up to a second
 * before it, never after. A window that allowed a second either way would
 * pass a time a second late, which is the fault these cases are about.
 * ANCHOR_SLACK_NS covers the same thing here as it does above: the moments
 * between the clock reads being related. */
static int is_the_truncation_of(uint64_t truncated_ns, uint64_t reference_ns)
{
	return truncated_ns <= reference_ns + ANCHOR_SLACK_NS &&
	       reference_ns < truncated_ns + DUMP_TRUNCATION_NS + ANCHOR_SLACK_NS;
}

/* A host slow to reach its first record stores the time that record arrived
 * in its dump, which is the time the capture written beside it gives the same
 * packet. Stamped where the loop reached the record, the dump was the whole
 * stall late while the capture was not, so the two outputs of one run
 * disagreed. */
MU_TEST(a_stalled_hosts_dump_and_capture_agree)
{
	uint32_t stored = 0, printed = 0;
	uint64_t capture_ns = 0, arrived_ns = 0;

	run_one_record(STAMPED_BASIC_RATE, 0, STALL_NS, 0, &stored, &printed,
	               &capture_ns, &arrived_ns);

	mu_assert_fatal(stored != 0, "the dump holds no record");
	mu_assert_fatal(capture_ns != 0, "the capture holds no packet");
	mu_assert_int_eq(stored, printed);
	mu_assert(is_the_truncation_of(1000000000ull * stored, capture_ns),
	          "the dump and the capture disagree about when the packet arrived");
	mu_assert(is_the_truncation_of(1000000000ull * stored, arrived_ns),
	          "the dump's host time is not where the record arrived");
}

/* The same of the low-energy callback, which stamps and dumps a record the
 * same way. */
MU_TEST(a_stalled_low_energy_dump_and_capture_agree)
{
	uint32_t stored = 0, printed = 0;
	uint64_t capture_ns = 0, arrived_ns = 0;

	run_one_record(STAMPED_LOW_ENERGY, 0, STALL_NS, 0, &stored, &printed,
	               &capture_ns, &arrived_ns);

	mu_assert_fatal(stored != 0, "the dump holds no record");
	mu_assert_fatal(capture_ns != 0, "the capture holds no packet");
	mu_assert(is_the_truncation_of(1000000000ull * stored, capture_ns),
	          "the dump and the capture disagree about when the packet arrived");
	mu_assert(is_the_truncation_of(1000000000ull * stored, arrived_ns),
	          "the dump's host time is not where the record arrived");
}

/* A scan writes nothing out, so its systime= column is the whole of what it
 * stamps: it too says where the record arrived, not where the loop reached
 * it. */
MU_TEST(a_stalled_scans_column_says_where_the_record_arrived)
{
	uint32_t stored = 0, printed = 0;
	uint64_t capture_ns = 0, arrived_ns = 0;

	run_one_record(STAMPED_SCAN, 0, STALL_NS, 0, &stored, &printed,
	               &capture_ns, &arrived_ns);

	mu_assert_int_eq(0, stored);
	mu_assert_fatal(printed != 0, "the scan printed no packet line");
	mu_assert(is_the_truncation_of(1000000000ull * printed, arrived_ns),
	          "the scan's host time is not where the record arrived");
}

/* A host that was reading promptly is left as it was: its record arrived
 * moments before the callback took it, so the dump holds the time read there,
 * which is the normal case and the commonest one. */
MU_TEST(a_prompt_hosts_dump_is_stamped_where_it_always_was)
{
	uint32_t stored = 0, printed = 0;
	uint64_t capture_ns = 0, arrived_ns = 0;

	run_one_record(STAMPED_BASIC_RATE, 0, 0, 0, &stored, &printed,
	               &capture_ns, &arrived_ns);

	mu_assert_fatal(stored != 0, "the dump holds no record");
	mu_assert(is_the_truncation_of(1000000000ull * stored, arrived_ns),
	          "a prompt capture's dump time moved");
}

/* Replaying a stalled host's dump puts the packet back where it arrived. The
 * host time the record carries is the whole of what the live run handed on,
 * so a dump stamped late replayed late too, and the replay disagreed with the
 * capture of the live run it came from. */
MU_TEST(a_replay_of_a_stalled_hosts_dump_lands_where_the_packet_arrived)
{
	uint32_t stored = 0, replayed = 0, printed = 0;
	uint64_t live_ns = 0, replay_ns = 0;
	uint64_t arrived_ns = 0, unused_ns = 0;

	run_one_record(STAMPED_BASIC_RATE, 0, STALL_NS, 0, &stored, &printed,
	               &live_ns, &arrived_ns);
	mu_assert_fatal(stored != 0, "the dump holds no record");

	run_one_record(STAMPED_BASIC_RATE, 1, 0, stored, &replayed, &printed,
	               &replay_ns, &unused_ns);

	/* A replay writes the recorded time back out unchanged, so a dump of a
	 * replay is the dump it replayed. */
	mu_assert_int_eq(stored, replayed);
	mu_assert_fatal(replay_ns != 0, "the replay's capture holds no packet");
	mu_assert(is_the_truncation_of(replay_ns, live_ns),
	          "the replay does not land where the live capture did");
	mu_assert(is_the_truncation_of(replay_ns, arrived_ns),
	          "the replay does not land where the record arrived");
}

/* ---- a packet that arrived as a chain of records ----------------------- */

/* A low-energy packet too long for one record, so it arrives as two. Only the
 * first ten bytes are a packet: the advertising access address, little endian,
 * then an ADV_IND header whose length matches the six bytes of payload after
 * it. The decoder reads no further than that header says, so the rest is
 * padding, there to make the whole thing longer than the 50 bytes a record
 * holds. */
#define CHAINED_PACKET_LEN 60
#define CHAINED_RECORDS    2

static void chained_records(usb_pkt_rx chain[CHAINED_RECORDS])
{
	uint8_t packet[CHAINED_PACKET_LEN];
	int position;

	memset(packet, 0, sizeof(packet));
	packet[0] = 0xd6;
	packet[1] = 0xbe;
	packet[2] = 0x89;
	packet[3] = 0x8e;
	packet[4] = 0x00;
	packet[5] = 6;

	for (position = 0; position < CHAINED_RECORDS; position++) {
		uint16_t offset = (uint16_t) (position * DMA_SIZE);
		uint16_t remaining = CHAINED_PACKET_LEN - offset;
		usb_pkt_rx* rx = &chain[position];

		memset(rx, 0, sizeof(*rx));
		rx->pkt_type = position == 0 ? LE_PACKET_START : LE_PACKET_CONT;
		rx->channel = 0;
		rx->clk100ns = htole32((uint32_t) FIRST_TICKS);
		/* Every record of a chain says how long the whole packet is and
		 * where it sits in the chain. */
		rx->reserved[0] = CHAINED_PACKET_LEN & 0xff;
		rx->reserved[1] = ((CHAINED_PACKET_LEN >> 8) &
		                   LE_FRAG_TOTAL_HIGH_MASK) |
		                  (uint8_t) (position << LE_FRAG_INDEX_SHIFT);
		memcpy(rx->data, packet + offset,
		       remaining < DMA_SIZE ? remaining : DMA_SIZE);
	}
}

/* A dump of these records, each with the host time given for it: the format
 * `-d` writes, which a replay reads back. Rewound and ready to replay. */
static FILE* dump_of(const usb_pkt_rx* records, const uint32_t* seconds,
                     int count)
{
	FILE* dump = tmpfile();
	int position;

	if (dump == NULL)
		return NULL;
	for (position = 0; position < count; position++) {
		uint32_t stored = htobe32(seconds[position]);

		fwrite(&stored, sizeof(stored), 1, dump);
		fwrite(&records[position], sizeof(records[position]), 1, dump);
	}
	fflush(dump);
	rewind(dump);
	return dump;
}

/* The files one run of the low-energy callback writes: a capture, and a file
 * standing in for the terminal its packet lines go to. */
struct chain_files {
	char dir[sizeof("/tmp/ubertooth-chain-XXXXXX")];
	char capture[sizeof("/tmp/ubertooth-chain-XXXXXX") + 16];
	char output[sizeof("/tmp/ubertooth-chain-XXXXXX") + 16];
	int saved_stdout;
};

/* Opens them, in a directory of its own per run so that a file left behind by
 * an earlier run cannot be read back here by mistake. */
static void open_chain_files(struct chain_files* files, ubertooth_t* ut)
{
	int output;

	snprintf(files->dir, sizeof(files->dir), "/tmp/ubertooth-chain-XXXXXX");
	mu_assert_fatal(mkdtemp(files->dir) != NULL,
	                "cannot make a directory to write in");
	snprintf(files->capture, sizeof(files->capture), "%s/capture.pcap",
	         files->dir);
	snprintf(files->output, sizeof(files->output), "%s/output", files->dir);
	mu_assert_fatal(lell_pcap_create_file(files->capture,
	                                     &ut->h_pcap_le) == 0,
	                "cannot open a capture to write");
	fflush(stdout);
	files->saved_stdout = dup(fileno(stdout));
	mu_assert_fatal(files->saved_stdout >= 0, "cannot set the output aside");
	output = open(files->output, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	mu_assert_fatal(output >= 0, "cannot open a file for the output");
	dup2(output, fileno(stdout));
	close(output);
}

/* Closes them again and reads back the systime= column of the packet line and
 * the time the capture gave the packet. */
static void close_chain_files(struct chain_files* files, ubertooth_t* ut,
                              uint32_t* printed, uint64_t* capture_ns)
{
	fflush(stdout);
	dup2(files->saved_stdout, fileno(stdout));
	close(files->saved_stdout);
	lell_pcap_close(ut->h_pcap_le);
	ut->h_pcap_le = NULL;
	*printed = printed_systime(files->output);
	*capture_ns = first_capture_ns(files->capture);
	unlink(files->capture);
	unlink(files->output);
	rmdir(files->dir);
}

/* A live capture of a chain, with a dump and a capture open at once: what
 * `ubertooth-btle -d <dump> -q <capture>` leaves the callback with.
 * arrival_ns[] is the monotonic reading each record arrived with, given
 * outright, which is what the ring buffer carries with it.
 *
 * The dump the run wrote comes back as *dump, rewound and ready to replay,
 * with the host time it stored for each record in stored[]. */
static void capture_a_chain(const usb_pkt_rx* chain,
                            const uint64_t* arrival_ns, int records,
                            FILE** dump, uint32_t* stored, uint32_t* printed,
                            uint64_t* capture_ns)
{
	struct chain_files files;
	ubertooth_t ut;
	int record;

	memset(&ut, 0, sizeof(ut));
	ut.clk100ns_period = CLK100NS_TIMER1_PERIOD;
	ut.fifo = fifo_init();
	mu_assert_fatal(ut.fifo != NULL, "cannot allocate the ring buffer");
	*dump = tmpfile();
	mu_assert_fatal(*dump != NULL, "cannot open a dump to write");
	open_chain_files(&files, &ut);

	infile = NULL;
	systime = 0;
	dumpfile = *dump;
	for (record = 0; record < records; record++)
		fifo_push(ut.fifo, &chain[record], arrival_ns[record]);
	for (record = 0; record < records; record++)
		cb_btle(&ut, NULL);

	close_chain_files(&files, &ut, printed, capture_ns);
	dumpfile = NULL;
	for (record = 0; record < records; record++)
		stored[record] = dump_record_systime(*dump, record);
	rewind(*dump);
	free(ut.fifo);
}

/* A replay of a dump, through the library's own reader and the same callback,
 * with a capture open: `ubertooth-btle -i <dump> -q <capture>`. */
static void replay_a_dump(FILE* dump, uint32_t* printed, uint64_t* capture_ns)
{
	struct chain_files files;
	ubertooth_t ut;

	memset(&ut, 0, sizeof(ut));
	ut.clk100ns_period = CLK100NS_TIMER1_PERIOD;
	ut.fifo = fifo_init();
	mu_assert_fatal(ut.fifo != NULL, "cannot allocate the ring buffer");
	open_chain_files(&files, &ut);

	infile = dump;
	dumpfile = NULL;
	stream_rx_file(&ut, dump, cb_btle, NULL);

	close_chain_files(&files, &ut, printed, capture_ns);
	infile = NULL;
	free(ut.fifo);
}

/* A dump holds one host time per record, so a packet that arrived as a chain
 * that crossed a whole second is stored with two different seconds. Replayed,
 * the packet belongs at the first of them: that is where the live capture of
 * the same run put it, because the packet's time is the time its chain
 * started. The reader overwrote the host time for every record it read, so
 * the packet landed at the last record's second, a whole second away. */
MU_TEST(a_replayed_chain_lands_at_the_first_records_second)
{
	usb_pkt_rx chain[CHAINED_RECORDS];
	const uint32_t recorded[CHAINED_RECORDS] = { 1700000000, 1700000001 };
	uint32_t printed = 0;
	uint64_t capture_ns = 0;
	FILE* dump;

	chained_records(chain);
	dump = dump_of(chain, recorded, CHAINED_RECORDS);
	mu_assert_fatal(dump != NULL, "cannot write a dump to replay");
	replay_a_dump(dump, &printed, &capture_ns);
	fclose(dump);

	mu_assert_fatal(capture_ns != 0, "the replay's capture holds no packet");
	mu_assert_int_eq(recorded[0], capture_ns / 1000000000ull);
	/* The printed column is the same time, so the two outputs of a replay
	 * do not disagree either. */
	mu_assert_int_eq(recorded[0], printed);
}

/* The whole of it as a user sees it: one live run writes a dump and a capture,
 * and a replay of that dump writes a capture of its own. The chain's two
 * records arrive 600 ms into one second and 400 ms into the next, so the dump
 * stores two different seconds for the one packet, and the replay has to pick
 * the same one the live capture did. */
MU_TEST(a_replayed_chain_lands_where_the_live_capture_put_it)
{
	usb_pkt_rx chain[CHAINED_RECORDS];
	uint64_t arrival_ns[CHAINED_RECORDS];
	uint32_t stored[CHAINED_RECORDS] = { 0, 0 };
	uint32_t printed = 0, replayed_printed = 0;
	uint64_t live_ns = 0, replay_ns = 0;
	/* The two clocks read together, so the readings below place each record
	 * at a host time of this case's choosing however long the run then
	 * takes: the callback relates the two clocks again itself, and both
	 * have moved on by the same amount. */
	uint64_t real_ns = clock_ns(CLOCK_REALTIME);
	uint64_t mono_ns = clock_ns(CLOCK_MONOTONIC);
	/* Where the second the test is running in began. */
	uint64_t second_ns = real_ns - real_ns % 1000000000ull;
	FILE* dump;

	arrival_ns[0] = mono_ns - (real_ns - (second_ns - 1400000000ull));
	arrival_ns[1] = mono_ns - (real_ns - (second_ns - 600000000ull));

	chained_records(chain);
	capture_a_chain(chain, arrival_ns, CHAINED_RECORDS, &dump, stored,
	                &printed, &live_ns);

	mu_assert_fatal(stored[0] != 0 && stored[1] != 0,
	                "the dump does not hold both records");
	mu_assert_int_eq(stored[0] + 1, stored[1]);
	mu_assert_fatal(live_ns != 0, "the live capture holds no packet");
	/* The live run places the packet where its chain started, and says so
	 * in the column as well. */
	mu_assert(is_the_truncation_of(1000000000ull * stored[0], live_ns),
	          "the live capture does not place the packet at the chain's start");
	mu_assert_int_eq(stored[0], printed);

	replay_a_dump(dump, &replayed_printed, &replay_ns);
	fclose(dump);

	mu_assert_fatal(replay_ns != 0, "the replay's capture holds no packet");
	mu_assert(is_the_truncation_of(replay_ns, live_ns),
	          "the replay does not land where the live capture did");
	mu_assert_int_eq(stored[0], replay_ns / 1000000000ull);
	mu_assert_int_eq(stored[0], replayed_printed);
}

/* Which clock a mode stamps with is known only to the tool that starts it, so
 * this one reads ubertooth-btle's source: the branch that starts the
 * low-energy engine must say that its packets come from timer1. A capture run
 * by hand is the only other way to notice it stopped doing so. */
MU_TEST(the_low_energy_engine_is_started_with_its_own_period)
{
	char text[200000];
	FILE *source = fopen(BTLE_SOURCE, "r");
	size_t length;
	char *sniffing;

	mu_assert(source != NULL, "cannot read " BTLE_SOURCE);
	if (source == NULL)
		return;
	length = fread(text, 1, sizeof(text) - 1, source);
	text[length] = '\0';
	fclose(source);

	sniffing = strstr(text, "cmd_btle_sniffing(");
	mu_assert(sniffing != NULL, "ubertooth-btle no longer starts the engine");
	if (sniffing == NULL)
		return;
	mu_assert(strstr(sniffing, "CLK100NS_TIMER1_PERIOD") != NULL,
	          "the engine's clock period is not set where its mode starts");
}

int main(void)
{
	MU_RUN(the_period_is_the_clock_the_firmware_counts);
	MU_RUN(a_tick_is_100_ns_before_any_wrap);
	MU_RUN(the_first_tick_after_a_wrap_follows_the_last_one);
	MU_RUN(a_wrap_adds_the_clocks_period_not_2_32);
	MU_RUN(a_timer1_wrap_adds_2_32);
	MU_RUN(a_record_ahead_of_the_rest_does_not_add_a_period);
	MU_RUN(any_lead_on_the_first_record_is_read_the_same_way);
	MU_RUN(records_after_a_stale_first_one_keep_their_times);
	MU_RUN(a_lower_reading_further_in_is_still_a_wrap);
	MU_RUN(a_record_ahead_of_the_rest_does_not_add_a_timer1_period);
	MU_RUN(a_leftover_first_record_does_not_add_its_period_count);
	MU_RUN(a_leftover_count_one_period_out_is_still_read_as_time);
	MU_RUN(a_leftover_count_a_period_above_costs_a_period_not_the_capture);
	MU_RUN(a_first_count_a_period_behind_its_reading_is_believed);
	MU_RUN(a_first_count_a_period_ahead_of_its_reading_is_believed);
	MU_RUN(a_wrap_between_the_first_two_counted_records_still_counts);
	MU_RUN(a_long_counted_gap_at_the_start_still_counts);
	MU_RUN(several_wraps_stay_continuous);
	MU_RUN(a_gap_longer_than_a_period_is_taken_from_the_packet);
	MU_RUN(the_count_in_the_packet_wraps_without_losing_a_period);
	MU_RUN(a_lost_wrap_is_reported_where_the_packets_carry_no_count);
	MU_RUN(a_single_lost_wrap_is_reported_too);
	MU_RUN(a_capture_that_keeps_up_reports_nothing);
	MU_RUN(a_wrap_the_host_clock_agrees_with_still_counts);
	MU_RUN(a_gap_of_a_whole_multiple_of_256_periods_is_reported_not_guessed);
	MU_RUN(a_gap_of_256_periods_further_in_leaves_the_anchor_alone);
	MU_RUN(a_count_that_reads_backwards_does_not_move_the_clock);
	MU_RUN(a_count_latched_a_period_early_costs_only_its_own_packet);
	MU_RUN(a_reused_handle_starts_the_next_capture_fresh);
	MU_RUN(stopping_a_capture_leaves_the_chosen_clock_alone);
	MU_RUN(a_wrap_the_host_was_not_reading_through_still_counts);
	MU_RUN(a_stall_does_not_reject_a_counted_gap_at_the_start);
	MU_RUN(a_leftover_first_record_is_found_on_a_live_capture);
	MU_RUN(a_first_record_reading_below_the_capture_moves_the_anchor);
	MU_RUN(a_quiet_live_gap_keeps_its_time_however_long_it_was);
	MU_RUN(a_wrap_a_backlog_hid_still_counts);
	MU_RUN(a_long_quiet_gap_at_the_start_is_not_a_leftover_record);
	MU_RUN(a_stray_record_further_into_a_live_capture_adds_nothing);
	MU_RUN(a_host_clock_that_steps_does_not_move_live_times);
	MU_RUN(a_stalled_host_anchors_the_capture_where_the_record_arrived);
	MU_RUN(a_prompt_host_anchors_the_capture_where_it_always_did);
	MU_RUN(a_record_with_no_arrival_reading_is_anchored_where_it_is_read);
	MU_RUN(an_arrival_reading_in_the_future_is_anchored_where_it_is_read);
	MU_RUN(a_reading_that_cannot_place_a_record_leaves_the_host_time_alone);
	MU_RUN(a_stalled_hosts_dump_and_capture_agree);
	MU_RUN(a_stalled_low_energy_dump_and_capture_agree);
	MU_RUN(a_stalled_scans_column_says_where_the_record_arrived);
	MU_RUN(a_prompt_hosts_dump_is_stamped_where_it_always_was);
	MU_RUN(a_replay_of_a_stalled_hosts_dump_lands_where_the_packet_arrived);
	MU_RUN(a_replayed_chain_lands_at_the_first_records_second);
	MU_RUN(a_replayed_chain_lands_where_the_live_capture_put_it);
	MU_RUN(the_low_energy_engine_is_started_with_its_own_period);
	return mu_report();
}
