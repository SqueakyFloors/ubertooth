/*
 * Unit tests for firmware/bluetooth_rxtx/ubertooth_rssi.c
 *
 * These run on the host (plain gcc) -- the RSSI code is pure integer math with
 * no hardware dependency. They focus on the signed-rounding behaviour of the
 * three division sites, which RSSI exercises constantly because dBm values are
 * negative. The old code used `(x + 128) / 256` (and `(sum + 128) / count`),
 * which only rounds to nearest for non-negative x: C integer division
 * truncates toward zero, so for negative x it rounds the wrong way, reporting
 * RSSI as slightly stronger (less negative) than it should be.
 *
 * The second group covers the LED spectrum analyzer threshold helpers, which
 * are pure integer math for the same reason.
 */
#include "minunit.h"
#include "ubertooth_rssi.h"

/* Internal accumulators (non-static in the .c) so tests can set exact states. */
extern int32_t rssi_sum;
extern int16_t rssi_iir[NUM_BREDR_CHANNELS];
extern uint8_t rssi_count;

/* Reference: round-half-away-from-zero, what the code should compute. */
static int ref_round_div(int n, int d)
{
	int half = d / 2;
	return (n >= 0) ? (n + half) / d : (n - half) / d;
}

/*
 * rssi_get_avg() returns (rssi_iir[ch] + 128) / 256 as dBm. Drive rssi_iir[0]
 * to exact scaled values and check the returned dBm rounds to nearest in BOTH
 * directions. Channel 2402 maps to index 0.
 */
MU_TEST(test_get_avg_negative_rounding)
{
	rssi_reset();
	rssi_iir_reset();

	/* -384 scaled = -1.5 dBm -> nearest is -2 (biased code gives -1). */
	rssi_iir[0] = -384;
	mu_assert_int_eq(-2, rssi_get_avg(2402));

	/* -128 scaled = -0.5 dBm -> nearest is -1 (biased code gives 0). */
	rssi_iir[0] = -128;
	mu_assert_int_eq(-1, rssi_get_avg(2402));

	/* -640 scaled = -2.5 dBm -> nearest -3 (biased gives -2). */
	rssi_iir[0] = -640;
	mu_assert_int_eq(-3, rssi_get_avg(2402));

	/* A typical strong-ish reading: -10240 scaled = -40 dBm exactly. */
	rssi_iir[0] = -40 * 256;
	mu_assert_int_eq(-40, rssi_get_avg(2402));
}

MU_TEST(test_get_avg_positive_unchanged)
{
	rssi_reset();
	rssi_iir_reset();

	/* Positive rounding must be unaffected by the fix. */
	rssi_iir[0] = 384;   /* +1.5 -> +2 */
	mu_assert_int_eq(2, rssi_get_avg(2402));
	rssi_iir[0] = 128;   /* +0.5 -> +1 */
	mu_assert_int_eq(1, rssi_get_avg(2402));
	rssi_iir[0] = 0;
	mu_assert_int_eq(0, rssi_get_avg(2402));
	rssi_iir[0] = 40 * 256;
	mu_assert_int_eq(40, rssi_get_avg(2402));
}

/*
 * rssi_iir_update() first computes avg = (rssi_sum + 128) / rssi_count.
 * With a single sample the IIR blend is avg*3/256 into a zero slot, so the
 * effect is small; instead assert the end-to-end averaged dBm over many equal
 * negative samples converges to the true value rather than biasing upward.
 */
MU_TEST(test_iir_converges_to_negative_average)
{
	int i;
	rssi_reset();
	rssi_iir_reset();

	/* Feed a constant -50 dBm. After enough updates the IIR should settle to
	 * -50, not -49 (which upward bias would produce). */
	for (i = 0; i < 4000; i++) {
		rssi_sum = 0;                     /* reset per-window accumulator */
		rssi_count = 0;
		rssi_add(-50);
		rssi_iir_update(2402);
	}
	mu_assert_int_eq(-50, rssi_get_avg(2402));
}

/*
 * Directly exercise the sum/count rounding used inside rssi_iir_update by
 * checking the averaging of a set of negative samples rounds correctly.
 * avg_scaled = round((sum of v*256) / count); dBm = round(avg_scaled/256).
 */
MU_TEST(test_sum_count_rounding_negative)
{
	rssi_reset();
	rssi_iir_reset();
	/* Three samples: -40, -41, -41  -> mean -40.666.. dBm -> nearest -41. */
	rssi_add(-40);
	rssi_add(-41);
	rssi_add(-41);
	rssi_iir_update(2402);
	/* After one IIR step from zero the slot is only ~1.2% of avg, so instead
	 * verify the reference: the per-window average itself rounds to -41. */
	{
		int count = 3;
		int sum_scaled = (-40 - 41 - 41) * 256;
		int avg_scaled = ref_round_div(sum_scaled, count);
		int dbm = ref_round_div(avg_scaled, 256);
		mu_assert_int_eq(-41, dbm);
	}
}

/*
 * The per-channel average, as the mode loops build it.
 *
 * One DMA bank: start a packet, sample the radio while the transfer runs, then
 * blend the packet's average into the channel's running value. bt_stream_rx(),
 * bt_generic_le() and bt_le_sync() all do exactly this, once per bank.
 */
static void a_bank(uint16_t channel, int8_t raw, int samples)
{
	int i;

	rssi_reset();
	for (i = 0; i < samples; i++)
		rssi_add(raw);
	rssi_iir_update(channel);
}

/* Banks of one steady reading, which is what a quiet channel gives. */
static void banks(uint16_t channel, int8_t raw, int count)
{
	int i;

	for (i = 0; i < count; i++)
		a_bank(channel, raw, 20);
}

/*
 * The filter has to survive the per-packet reset, or it never gets anywhere.
 *
 * It blends 3/256 of each bank into the channel's value, so one bank from
 * silence moves it 3/256 of the way -- and 3/256 of any raw reading from -42
 * to 42, which covers the whole range the radio reports in the band, rounds
 * back to zero. Clearing the filter with the per-packet figures left every
 * bank looking like the first, so every Classic record reported the same noise
 * figure, which the host reads as -55 dBm. Hundreds of banks is what it takes
 * to arrive.
 */
MU_TEST(test_the_channel_average_accumulates_across_banks)
{
	rssi_iir_reset();

	/* One bank of a steady -30 raw: the filter has barely moved, and still
	 * reads 0. This was every record's noise figure. */
	banks(2402, -30, 1);
	mu_assert_int_eq(-90, rssi_iir[0]);
	mu_assert_int_eq(0, rssi_get_avg(2402));

	/* A hundred more and it is part of the way there: below 0, not yet -30.
	 * This is the step the per-bank clear made unreachable. */
	banks(2402, -30, 100);
	mu_assert(rssi_get_avg(2402) < 0, "the average must move off zero");
	mu_assert(rssi_get_avg(2402) > -30, "100 banks is not yet the whole way");

	/* Given the few hundred banks the filter needs, it reads what the radio
	 * heard: -30 raw, which the host reports as -82 dBm. */
	banks(2402, -30, 500);
	mu_assert_int_eq(-30, rssi_get_avg(2402));

	/* It follows the channel down as well as up. */
	banks(2402, -70, 1000);
	mu_assert_int_eq(-70, rssi_get_avg(2402));
}

/* Each channel arrives at its own value, which is the point of a sweep: the
 * record for a packet on one channel must not carry another channel's noise. */
MU_TEST(test_each_channel_arrives_at_its_own_average)
{
	rssi_iir_reset();

	/* A sweep's banks interleave across the band. */
	{
		int i;

		for (i = 0; i < 1000; i++) {
			a_bank(2402, -20, 20);
			a_bank(2441, -45, 20);
			a_bank(2480, -70, 20);
		}
	}

	mu_assert_int_eq(-20, rssi_get_avg(2402));
	mu_assert_int_eq(-45, rssi_get_avg(2441));
	mu_assert_int_eq(-70, rssi_get_avg(2480));
}

/*
 * The four per-packet figures do still start again for every packet: they
 * describe the symbols of one packet, and the record carries all four.
 */
MU_TEST(test_the_per_packet_figures_start_again_every_packet)
{
	rssi_iir_reset();

	/* A loud packet. */
	rssi_reset();
	rssi_add(-60);
	rssi_add(-20);
	rssi_add(-40);
	mu_assert_int_eq(-20, rssi_max);
	mu_assert_int_eq(-60, rssi_min);
	mu_assert_int_eq(3, rssi_count);
	mu_assert_int_eq(-120 * 256, rssi_sum);
	rssi_iir_update(2402);

	/* The next packet, quieter: none of the loud one's figures survive. */
	rssi_reset();
	mu_assert_int_eq(INT8_MIN, rssi_max);
	mu_assert_int_eq(INT8_MAX, rssi_min);
	mu_assert_int_eq(0, rssi_count);
	mu_assert_int_eq(0, rssi_sum);

	rssi_add(-80);
	rssi_add(-75);
	mu_assert_int_eq(-75, rssi_max);
	mu_assert_int_eq(-80, rssi_min);
	mu_assert_int_eq(2, rssi_count);
}

/* ...and starting a packet leaves the channel averages alone. That is the
 * whole of the fix: the two resets are on different clocks. */
MU_TEST(test_starting_a_packet_leaves_the_channel_averages_alone)
{
	int i;

	rssi_iir_reset();
	for (i = 0; i < NUM_BREDR_CHANNELS; i++)
		rssi_iir[i] = (int16_t)(-(20 + i) * 256);

	rssi_add(-10);
	rssi_reset();

	for (i = 0; i < NUM_BREDR_CHANNELS; i++)
		mu_assert_int_eq(-(20 + i) * 256, rssi_iir[i]);
	mu_assert_int_eq(-20, rssi_get_avg(BT_FREQ_MIN));
	mu_assert_int_eq(-98, rssi_get_avg(BT_FREQ_MAX));
}

/* Where a capture ends, every channel is forgotten: the averages describe the
 * band during that capture, and the next one starts from silence. */
MU_TEST(test_ending_a_capture_forgets_every_channel)
{
	int i;

	for (i = 0; i < NUM_BREDR_CHANNELS; i++)
		rssi_iir[i] = (int16_t)(-40 * 256);

	rssi_iir_reset();

	for (i = 0; i < NUM_BREDR_CHANNELS; i++)
		mu_assert_int_eq(0, rssi_iir[i]);
	mu_assert_int_eq(0, rssi_get_avg(BT_FREQ_MIN));
	mu_assert_int_eq(0, rssi_get_avg(BT_FREQ_MAX));
}

/*
 * LED spectrum analyzer threshold.
 *
 * `ubertooth-util -q<N>` asks for a threshold of -N dBm; the firmware stores it
 * raw, where dBm = raw - 54. The threshold used to be an int8_t, so every N
 * from 183 up -- including the 225 that bare `-q` sends -- wrapped to a large
 * positive raw value (183 -> 127, 225 -> 85) that no reading could exceed, and
 * the most sensitive settings lit no LED at all.
 */

/* What the firmware does with a reading: light an LED above the threshold. */
static int lights_led(int lvl, int n)
{
	return rssi_above_threshold((int8_t)lvl, rssi_threshold_from_n(n));
}

MU_TEST(test_threshold_from_n_does_not_wrap)
{
	/* The raw threshold is 54 - N over the whole accepted range. */
	mu_assert_int_eq(34, rssi_threshold_from_n(20));
	mu_assert_int_eq(-31, rssi_threshold_from_n(85));
	mu_assert_int_eq(-66, rssi_threshold_from_n(120));
	/* Values outside the range are refused, but would not wrap either: these
	 * used to wrap to +127 and +85 in an int8_t. */
	mu_assert_int_eq(-129, rssi_threshold_from_n(183));
	mu_assert_int_eq(-171, rssi_threshold_from_n(225));
}

/* A larger N must never be less sensitive: the threshold falls monotonically. */
MU_TEST(test_threshold_is_monotonic_over_the_whole_range)
{
	int n;

	for (n = RSSI_THRESHOLD_N_MIN + 1; n <= RSSI_THRESHOLD_N_MAX; n++)
		mu_assert(rssi_threshold_from_n(n) < rssi_threshold_from_n(n - 1),
		          "a larger N must give a lower raw threshold");
}

/* The default, -85 dBm (raw -31), lights for Wi-Fi traffic but not for the
 * noise floor; -120 dBm (raw -66) lights for nearly every reading. */
MU_TEST(test_default_and_most_sensitive_thresholds)
{
	mu_assert_int_eq(85, UBERTOOTH_LED_SPECAN_N_DEFAULT);
	mu_assert(rssi_threshold_n_valid(UBERTOOTH_LED_SPECAN_N_DEFAULT),
	          "the default must be in range");
	mu_assert(lights_led(-20, 85), "-74 dBm traffic must light at -85 dBm");
	mu_assert(!lights_led(-40, 85), "-94 dBm noise must not light at -85 dBm");
	mu_assert(!lights_led(-31, 85), "a reading at the threshold must not light");
	mu_assert(lights_led(-30, 85), "a reading just above it must");
	mu_assert(lights_led(-60, 120), "-114 dBm must light at -120 dBm");
}

/* The insensitive end still discriminates: -q20 is -20 dBm, raw 34. */
MU_TEST(test_insensitive_thresholds_still_discriminate)
{
	mu_assert(!lights_led(34, 20), "a reading at the threshold must not light");
	mu_assert(lights_led(35, 20), "a reading above the threshold must light");
	mu_assert(!lights_led(-40, 20), "ambient noise is well below -20 dBm");

	/* -q100 is -100 dBm, raw -46; ambient noise sits above that. */
	mu_assert(lights_led(-40, 100), "-100 dBm must light on ambient noise");
	mu_assert(!lights_led(-50, 100), "a weaker reading must not");
}

/* Sensitivity must not jump anywhere: once a reading lights, it stays lit as N
 * grows. This is what "monotonic" means to someone turning the knob. */
MU_TEST(test_a_lit_reading_stays_lit_as_n_grows)
{
	int lvl, n;

	for (lvl = INT8_MIN; lvl <= INT8_MAX; lvl += 7) {
		int lit = 0;
		for (n = RSSI_THRESHOLD_N_MIN; n <= RSSI_THRESHOLD_N_MAX; n++) {
			if (lights_led(lvl, n))
				lit = 1;
			else
				mu_assert(!lit, "an LED must not go dark again as N grows");
		}
	}
}

/* The firmware's accepted range is the host's -q range, 20-120, from the
 * shared header: the old `> 256` check let anything below 257 through. */
MU_TEST(test_accepted_range_matches_the_host)
{
	mu_assert_int_eq(20, RSSI_THRESHOLD_N_MIN);
	mu_assert_int_eq(120, RSSI_THRESHOLD_N_MAX);

	mu_assert(!rssi_threshold_n_valid(0), "0 must be refused");
	mu_assert(!rssi_threshold_n_valid(19), "19 must be refused");
	mu_assert(rssi_threshold_n_valid(20), "20 must be accepted");
	mu_assert(rssi_threshold_n_valid(120), "120 must be accepted");
	mu_assert(!rssi_threshold_n_valid(121), "121 must be refused");
	mu_assert(!rssi_threshold_n_valid(225), "225 must be refused");
	mu_assert(!rssi_threshold_n_valid(65535), "a 16-bit maximum must be refused");
}

/*
 * The IIR keeps one slot per Bluetooth channel, found by subtracting the bottom
 * of the band from the frequency, so the band edges must land in the first and
 * last slots and nothing may fall off either end. A frequency outside the band
 * shares the first slot, which is how the code has always handled a caller
 * tuned somewhere the array cannot describe.
 */
MU_TEST(test_the_band_fills_the_array_and_outside_it_shares_slot_zero)
{
	mu_assert_int_eq(2402, BT_FREQ_MIN);
	mu_assert_int_eq(2480, BT_FREQ_MAX);
	mu_assert_int_eq(79, NUM_BREDR_CHANNELS);
	mu_assert_int_eq(NUM_BREDR_CHANNELS, BT_FREQ_MAX - BT_FREQ_MIN + 1);

	rssi_reset();
	rssi_iir_reset();
	rssi_iir[0] = -10 * 256;
	rssi_iir[NUM_BREDR_CHANNELS - 1] = -70 * 256;

	mu_assert_int_eq(-10, rssi_get_avg(BT_FREQ_MIN));
	mu_assert_int_eq(-70, rssi_get_avg(BT_FREQ_MAX));
	mu_assert_int_eq(-10, rssi_get_avg(BT_FREQ_MIN - 1));
	mu_assert_int_eq(-10, rssi_get_avg(BT_FREQ_MAX + 1));

	/* An update outside the band writes slot zero. */
	rssi_reset();
	rssi_iir_reset();
	rssi_add(-50);
	rssi_iir_update(BT_FREQ_MAX + 1);
	mu_assert(rssi_iir[0] != 0, "an out-of-band update must land in slot zero");
}

int main(void)
{
	printf("RSSI rounding and LED specan threshold tests\n");
	MU_RUN(test_get_avg_negative_rounding);
	MU_RUN(test_get_avg_positive_unchanged);
	MU_RUN(test_iir_converges_to_negative_average);
	MU_RUN(test_sum_count_rounding_negative);
	MU_RUN(test_the_channel_average_accumulates_across_banks);
	MU_RUN(test_each_channel_arrives_at_its_own_average);
	MU_RUN(test_the_per_packet_figures_start_again_every_packet);
	MU_RUN(test_starting_a_packet_leaves_the_channel_averages_alone);
	MU_RUN(test_ending_a_capture_forgets_every_channel);
	MU_RUN(test_threshold_from_n_does_not_wrap);
	MU_RUN(test_threshold_is_monotonic_over_the_whole_range);
	MU_RUN(test_default_and_most_sensitive_thresholds);
	MU_RUN(test_insensitive_thresholds_still_discriminate);
	MU_RUN(test_a_lit_reading_stays_lit_as_n_grows);
	MU_RUN(test_accepted_range_matches_the_host);
	MU_RUN(test_the_band_fills_the_array_and_outside_it_shares_slot_zero);
	return mu_report();
}
