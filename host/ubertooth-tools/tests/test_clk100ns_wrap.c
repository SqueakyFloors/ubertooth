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
 * How many times the clock started over between two packets comes from one of
 * three places, and all three are exercised here:
 *
 *  - the count a Classic packet carries, which is exact;
 *  - for a live capture whose packets carry none, how much real time passed
 *    between the two packets, which covers a quiet gap of any length and a
 *    record out of place alike;
 *  - for a replay, which has no such clock, the clock reading going down,
 *    which loses a wrap that falls in a gap longer than one period. The loss
 *    is reported against the host time the records carry.
 *
 * The host's two clocks are driven by the cases below rather than by the
 * machine, because a live capture's times cannot be checked against clocks
 * that run on their own.
 *
 * Only differences between reconstructed times are checked. Where a capture
 * starts is a separate matter (the anchor and the start offset), so these
 * cases hold whatever the first packet's clock is.
 */
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
/* The host wall clock at the start of a capture: a real time, so a capture
 * whose times fall a period short of it is still a time. */
#define WALL_BASE 1700000000000000000ull

/* The host clocks the reconstruction reads. */
static uint64_t host_elapsed_ns;
static uint64_t host_wall_ns;

static uint64_t fake_elapsed_ns( void )
{
	return host_elapsed_ns;
}

static uint64_t fake_wall_ns( void )
{
	return host_wall_ns;
}

/* What the host wall clock has been stepped by since a capture started, as a
 * time sync or a hand-set clock would step it. The elapsed-time clock is not
 * stepped, which is the point of reading it. */
static int64_t wall_step_ns;

/* The reconstructed time of a packet captured at this clock value, with the
 * count it carries and the real time since the capture started both given
 * outright.
 *
 * The count can disagree with the clock reading: the firmware latches the two
 * with separate reads of CLKN, and a saved capture can be edited or replayed
 * out of order. The elapsed time can disagree with both, which is what the
 * cases for a record out of place are built on. */
static uint64_t at_clock_host_count(ubertooth_t* ut, uint64_t ticks,
                                    uint64_t elapsed_ns, uint8_t count)
{
	usb_pkt_rx rx;

	memset(&rx, 0, sizeof(rx));
	rx.clk100ns = htole32((uint32_t)(ticks % ut->clk100ns_period));
	rx.clkn_high = count;
	host_elapsed_ns = elapsed_ns;
	host_wall_ns = WALL_BASE + elapsed_ns + (uint64_t) wall_step_ns;
	return now_ns_from_clk100ns( ut, &rx );
}

/* The same for a capture that is keeping up: a packet this far into the board
 * clock arrived this long after the capture started. */
static uint64_t at_clock_with_count(ubertooth_t* ut, uint64_t ticks,
                                    uint8_t count)
{
	return at_clock_host_count( ut, ticks, 100 * ticks, count );
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

/* A capture that has seen no packet yet, stamped with this clock, with the
 * host clocks of this test in place of the machine's. */
static void start_capture(ubertooth_t* ut, uint64_t period)
{
	memset(ut, 0, sizeof(*ut));
	ut->clk100ns_period = period;
	read_elapsed_ns = fake_elapsed_ns;
	read_now_ns = fake_wall_ns;
	host_elapsed_ns = 0;
	host_wall_ns = WALL_BASE;
	wall_step_ns = 0;
}

/* Standard error, redirected to a temporary file so a case can read what the
 * library wrote there. */
static FILE* captured_stderr;
static int real_stderr = -1;

static int capture_stderr(void)
{
	captured_stderr = tmpfile();
	real_stderr = dup(fileno(stderr));
	if (captured_stderr == NULL || real_stderr < 0)
		return 0;
	fflush(stderr);
	dup2(fileno(captured_stderr), fileno(stderr));
	return 1;
}

static void read_captured_stderr(char* text, size_t size)
{
	size_t n;

	text[0] = '\0';
	if (captured_stderr == NULL)
		return;
	fflush(stderr);
	if (real_stderr >= 0) {
		dup2(real_stderr, fileno(stderr));
		close(real_stderr);
		real_stderr = -1;
	}
	rewind(captured_stderr);
	n = fread(text, 1, size - 1, captured_stderr);
	text[n] = '\0';
	fclose(captured_stderr);
	captured_stderr = NULL;
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

/* Two packets of a live capture a quiet gap apart, with no count in them:
 * whatever the gap, the time between them has to come out as the gap. */
static void live_gap(uint64_t period, uint64_t first, uint64_t gap)
{
	ubertooth_t ut;
	uint64_t before, after;

	start_capture(&ut, period);
	before = at_clock(&ut, first);
	after = at_clock(&ut, first + gap);

	mu_assert_int_eq(100 * gap, after - before);
}

/* The same gap from two starting points, because the clock reading on its own
 * says different things about the same gap depending on where it starts: from
 * one of these the reading comes out lower than it was, which the old rule
 * read as exactly one wrap, and from the other it comes out higher, which the
 * old rule read as no wrap at all. */
static void live_gap_from_either_side_of_a_wrap(uint64_t period, uint64_t gap)
{
	uint64_t into_period = gap % period;

	/* Far enough in that a start before a wrap is still a positive clock
	 * reading, whatever the gap. */
	live_gap(period, 11 * period - into_period / 2, gap);
	live_gap(period, 10 * period, gap);
}

/* A quiet gap in a live capture, which the clock reading alone cannot
 * measure: under a period it may or may not come out lower than it was, and
 * over a period it hides whole wraps. The real time between the two packets
 * settles it either way. A gap of seconds on an idle channel is ordinary; a
 * gap of minutes takes only a quiet channel and a narrow filter. */
MU_TEST(a_quiet_gap_in_a_live_capture_is_counted_from_real_time)
{
	uint64_t period = CLK100NS_PERIOD;

	/* Either side of the half period a threshold would have to split. */
	live_gap_from_either_side_of_a_wrap(period, 4 * period / 10);
	live_gap_from_either_side_of_a_wrap(period, 6 * period / 10);
	/* Longer than a period, where a reading that comes out lower cannot
	 * say how many went by and one that comes out higher hides them
	 * completely. */
	live_gap_from_either_side_of_a_wrap(period, 3 * period / 2);
	live_gap_from_either_side_of_a_wrap(period, 3 * period);
	/* The low-energy engine's clock, whose packets never carry a count. */
	period = CLK100NS_TIMER1_PERIOD;
	live_gap_from_either_side_of_a_wrap(period, 3 * period / 2);
	live_gap_from_either_side_of_a_wrap(period, 3 * period);
}

/* A record out of place in a live capture, the other half of the problem: the
 * clock reads lower than it did, but no real time passed, so it is not a
 * wrap. Counted as one, this packet and every packet after it in the capture
 * came out a whole period late. */
MU_TEST(a_record_out_of_place_in_a_live_capture_is_not_a_wrap)
{
	ubertooth_t ut;
	uint64_t period = CLK100NS_TIMER1_PERIOD;
	uint64_t first = FIRST_TICKS + period / 5;
	uint64_t back = period / 10;
	uint64_t times[3];

	start_capture(&ut, period);
	times[0] = at_clock(&ut, first);
	/* A tenth of a period earlier on the board clock, arriving in the same
	 * millisecond as the packet before it. */
	times[1] = at_clock_host_count(&ut, first - back, 100 * first + 1000000,
	                               0);
	times[2] = at_clock(&ut, first + STEP_TICKS);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(100 * back, times[0] - times[1]);
	mu_assert_int_eq(100 * STEP_TICKS, times[2] - times[0]);
}

/* A live capture that opens on a record left over from the mode before it,
 * whose clock reads seconds ahead of the packets that follow. The drop is
 * read for what it is, so the real packets keep their own spacing; counted as
 * a wrap, they were spread over the whole 327.68 s period, which is why a
 * twelve-second capture came out of Wireshark five and a half minutes long.
 *
 * Where a capture is anchored is unchanged: the stale record's host time is
 * still the capture's start, so the records after it are stamped before it. */
MU_TEST(a_stale_first_record_does_not_stretch_a_live_capture)
{
	ubertooth_t ut;
	/* Twelve seconds of board clock, as the real dump in the report was. */
	uint64_t ahead = 120000000ull;
	uint64_t real = FIRST_TICKS;
	uint64_t times[3];

	start_capture(&ut, CLK100NS_PERIOD);
	times[0] = at_clock(&ut, real + ahead);
	/* The real packets, arriving in the milliseconds after it. */
	times[1] = at_clock_host_count(&ut, real, 1000000, 0);
	times[2] = at_clock_host_count(&ut, real + STEP_TICKS,
	                               1000000 + 100 * STEP_TICKS, 0);

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(100 * ahead, times[0] - times[1]);
	mu_assert_int_eq(100 * STEP_TICKS, times[2] - times[1]);
}

/* A host clock stepped during a live capture - a time sync, or someone
 * setting the clock - must not move packet times, so the periods are counted
 * from elapsed time, which a step does not touch. The step is still reported,
 * because from here it looks exactly like a lost wrap. */
MU_TEST(a_stepped_host_clock_does_not_move_packet_times)
{
	ubertooth_t ut;
	uint64_t period_ns = 100 * CLK100NS_TIMER1_PERIOD;
	uint64_t times[3];
	char text[400];

	start_capture(&ut, CLK100NS_TIMER1_PERIOD);
	mu_assert(capture_stderr(), "cannot capture standard error");
	times[0] = at_clock(&ut, FIRST_TICKS);
	/* Two periods forwards, from here to the end of the capture. */
	wall_step_ns = (int64_t) (2 * period_ns);
	times[1] = at_clock(&ut, FIRST_TICKS + STEP_TICKS);
	times[2] = at_clock(&ut, FIRST_TICKS + 2 * STEP_TICKS);
	read_captured_stderr(text, sizeof(text));

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(100 * STEP_TICKS, times[1] - times[0]);
	mu_assert_int_eq(100 * STEP_TICKS, times[2] - times[1]);
	mu_assert(strstr(text, "warning: packet times are behind the host clock "
	                 "by more than half the capture clock's 429.50 s period")
	          != NULL, text);
}

/* A step backwards leaves the times alone as well, and says nothing: packet
 * times ahead of the host clock are what a capture that is keeping up looks
 * like, down to its last packet's latency. */
MU_TEST(a_host_clock_stepped_backwards_is_not_reported)
{
	ubertooth_t ut;
	uint64_t times[2];
	char text[400];

	start_capture(&ut, CLK100NS_TIMER1_PERIOD);
	mu_assert(capture_stderr(), "cannot capture standard error");
	times[0] = at_clock(&ut, FIRST_TICKS);
	wall_step_ns = -(int64_t) (100 * STEP_TICKS);
	times[1] = at_clock(&ut, FIRST_TICKS + STEP_TICKS);
	read_captured_stderr(text, sizeof(text));

	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(100 * STEP_TICKS, times[1] - times[0]);
	mu_assert_int_eq(0, (int)strlen(text));
}

/* Feeds a replay of two packets a gap apart, where each packet's host time is
 * the one its record carries, and returns what the library wrote to standard
 * error. */
static void replay_gap(ubertooth_t* ut, uint64_t period, int counted,
                       uint64_t first, uint64_t gap, uint64_t times[2],
                       char* text, size_t size)
{
	/* The host time of the first record, as a whole second, which is all a
	 * record stores. */
	uint32_t base = (uint32_t)(WALL_BASE / 1000000000ull);
	FILE* replay = tmpfile();

	text[0] = '\0';
	mu_assert(replay != NULL, "cannot stand in for a capture file");
	if (replay == NULL)
		return;

	/* Only whether a replay is running is read from infile, never the
	 * file itself. */
	infile = replay;
	start_capture(ut, period);
	mu_assert(capture_stderr(), "cannot capture standard error");
	systime = base;
	times[0] = at_clock_counted(ut, first, counted);
	/* The host clock keeps running through the gap, whatever the capture
	 * clock did: 100 ns a tick. */
	systime = base + (uint32_t)(gap / 10000000ull);
	times[1] = at_clock_counted(ut, first + gap, counted);
	infile = NULL;
	fclose(replay);
	read_captured_stderr(text, size);
}

/* A replay of records that carry no count leaves the wrap to be inferred, so
 * a gap longer than one period still loses it: a record stores the host time
 * it was captured with only to the second, and nothing of how long the
 * capture had been running, so there is no real time to count the periods
 * from. The host time the records carry is the only evidence that it
 * happened, and it is reported rather than used to correct the times. */
MU_TEST(a_lost_wrap_is_reported_where_a_replay_cannot_count_it)
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

/* One lost wrap leaves the times exactly one period short, so the report
 * cannot wait for a second. */
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

/* The two rules side by side on the same gap, deliberately: a replay of a
 * saved file keeps the reading-goes-down rule, because the host clock of the
 * capture that wrote the file is not in it. A gap of one and a half periods
 * is one wrap for the replay and two for a live capture, so the replay's
 * times come out a period short - and say so. */
MU_TEST(a_replay_keeps_the_old_rule_where_a_live_capture_counts_the_periods)
{
	ubertooth_t ut;
	uint64_t period = CLK100NS_TIMER1_PERIOD;
	uint64_t gap = 3 * period / 2;
	uint64_t times[2];
	char text[400];

	live_gap(period, FIRST_TICKS, gap);

	replay_gap(&ut, period, 0, FIRST_TICKS, gap, times, text, sizeof(text));

	mu_assert_int_eq(100 * (gap - period), times[1] - times[0]);
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

	/* No periods counted, so the whole gap is missing from the times and
	 * the second packet reads a step before the first rather than the
	 * single period an inference would have added. */
	mu_assert_int_eq(0, ut.clk100ns_upper);
	mu_assert_int_eq(100 * STEP_TICKS, times[0] - times[1]);
	mu_assert(strstr(text, "warning: packet times are behind the host clock "
	                 "by more than half the capture clock's 327.68 s period")
	          != NULL, text);
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
	mu_assert_int_eq(0, ut.last_elapsed_ns);
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
	MU_RUN(several_wraps_stay_continuous);
	MU_RUN(a_gap_longer_than_a_period_is_taken_from_the_packet);
	MU_RUN(the_count_in_the_packet_wraps_without_losing_a_period);
	MU_RUN(a_quiet_gap_in_a_live_capture_is_counted_from_real_time);
	MU_RUN(a_record_out_of_place_in_a_live_capture_is_not_a_wrap);
	MU_RUN(a_stale_first_record_does_not_stretch_a_live_capture);
	MU_RUN(a_stepped_host_clock_does_not_move_packet_times);
	MU_RUN(a_host_clock_stepped_backwards_is_not_reported);
	MU_RUN(a_lost_wrap_is_reported_where_a_replay_cannot_count_it);
	MU_RUN(a_single_lost_wrap_is_reported_too);
	MU_RUN(a_replay_keeps_the_old_rule_where_a_live_capture_counts_the_periods);
	MU_RUN(a_capture_that_keeps_up_reports_nothing);
	MU_RUN(a_gap_of_a_whole_multiple_of_256_periods_is_reported_not_guessed);
	MU_RUN(a_count_that_reads_backwards_does_not_move_the_clock);
	MU_RUN(a_count_latched_a_period_early_costs_only_its_own_packet);
	MU_RUN(a_reused_handle_starts_the_next_capture_fresh);
	MU_RUN(stopping_a_capture_leaves_the_chosen_clock_alone);
	MU_RUN(the_low_energy_engine_is_started_with_its_own_period);
	return mu_report();
}
