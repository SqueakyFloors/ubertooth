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

/* The reconstructed time of a packet captured at this clock value, with the
 * count it carries given outright: the firmware can send one that does not
 * match its own clock reading, because the two are latched by separate reads
 * of CLKN, and a saved capture can be edited or replayed out of order. */
static uint64_t at_clock_with_count(ubertooth_t* ut, uint64_t ticks,
                                    uint8_t count)
{
	usb_pkt_rx rx;

	memset(&rx, 0, sizeof(rx));
	rx.clk100ns = htole32((uint32_t)(ticks % ut->clk100ns_period));
	rx.clkn_high = count;
	return now_ns_from_clk100ns( ut, &rx );
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

/* A capture that has seen no packet yet, stamped with this clock. */
static void start_capture(ubertooth_t* ut, uint64_t period)
{
	memset(ut, 0, sizeof(*ut));
	ut->clk100ns_period = period;
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

	/* Only whether a replay is running is read from infile, never the
	 * file itself. */
	infile = replay;
	start_capture(ut, period);
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
 * period, or every time after it is a period too high for good. */
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
	MU_RUN(a_lost_wrap_is_reported_where_the_packets_carry_no_count);
	MU_RUN(a_single_lost_wrap_is_reported_too);
	MU_RUN(a_capture_that_keeps_up_reports_nothing);
	MU_RUN(a_gap_of_a_whole_multiple_of_256_periods_is_reported_not_guessed);
	MU_RUN(a_count_that_reads_backwards_does_not_move_the_clock);
	MU_RUN(a_count_latched_a_period_early_costs_only_its_own_packet);
	MU_RUN(a_reused_handle_starts_the_next_capture_fresh);
	MU_RUN(stopping_a_capture_leaves_the_chosen_clock_alone);
	MU_RUN(the_low_energy_engine_is_started_with_its_own_period);
	return mu_report();
}
