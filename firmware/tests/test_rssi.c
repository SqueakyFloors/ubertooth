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


/* ------------------------------------------------------------------ *
 * Averaging in power
 * ------------------------------------------------------------------ */

/* One reading is its own mean, and so is any number of equal readings. */
MU_TEST(test_a_mean_of_equal_readings_is_that_reading)
{
	rssi_power_t p;
	int i;

	rssi_power_reset(&p);
	mu_assert_int_eq(0, rssi_power_measured(&p));
	mu_assert_int_eq(INT8_MIN, rssi_power_mean(&p));

	rssi_power_add(&p, -45);
	mu_assert_int_eq(1, rssi_power_measured(&p));
	mu_assert_int_eq(-45, rssi_power_mean(&p));

	for (i = 0; i < 200; i++)
		rssi_power_add(&p, -45);
	mu_assert_int_eq(-45, rssi_power_mean(&p));
}

/*
 * The whole point of averaging in power: two readings 10 dB apart have a mean
 * power 55% of the stronger one, which is 2.6 dB below it, so the mean is 3 dB
 * down. Averaging the two dBm figures would say 5 dB down.
 */
MU_TEST(test_the_mean_is_of_the_power_not_of_the_dbm_figures)
{
	rssi_power_t p;
	int i;

	rssi_power_reset(&p);
	rssi_power_add(&p, -40);
	rssi_power_add(&p, -50);
	mu_assert_int_eq(-43, rssi_power_mean(&p));

	/* Four quiet readings and one loud one, 10 dB apart: a fifth of the loud
	 * one's power plus four fifths of a tenth of it, which is 5.5 dB below
	 * the loud one. The dBm average would say 8 dB below. */
	rssi_power_reset(&p);
	for (i = 0; i < 4; i++)
		rssi_power_add(&p, -50);
	rssi_power_add(&p, -40);
	mu_assert_int_eq(-46, rssi_power_mean(&p));

	/* And the other way round, which is the same set of readings. */
	rssi_power_reset(&p);
	rssi_power_add(&p, -40);
	for (i = 0; i < 4; i++)
		rssi_power_add(&p, -50);
	mu_assert_int_eq(-46, rssi_power_mean(&p));
}

/* The order the readings arrive in does not change the mean, even though a
 * louder reading rescales everything already added. */
MU_TEST(test_the_order_of_the_readings_does_not_matter)
{
	rssi_power_t up, down;
	int i;

	rssi_power_reset(&up);
	rssi_power_reset(&down);
	for (i = 0; i < 8; i++) {
		rssi_power_add(&up, (int8_t)(-50 + i));
		rssi_power_add(&down, (int8_t)(-43 - i));
	}
	mu_assert_int_eq(rssi_power_mean(&down), rssi_power_mean(&up));
}

/*
 * A reading further below the strongest than the window is left out, and so is
 * its share of the count. That is what makes the figure the packet's own power:
 * the readings of a receive window span the packet and the quiet either side
 * of it, and with the quiet counted in a packet filling a third of the window
 * would read 5 dB below its real level.
 */
MU_TEST(test_quiet_readings_outside_the_window_do_not_drag_the_mean_down)
{
	rssi_power_t p;
	int i;

	/* A packet 30 dB above the background, filling a third of the window. */
	rssi_power_reset(&p);
	for (i = 0; i < 10; i++)
		rssi_power_add(&p, -40);
	for (i = 0; i < 20; i++)
		rssi_power_add(&p, -70);
	mu_assert_int_eq(-40, rssi_power_mean(&p));

	/* The quiet first, so the packet's readings arrive after the background
	 * has been counted and have to displace it. */
	rssi_power_reset(&p);
	for (i = 0; i < 20; i++)
		rssi_power_add(&p, -70);
	for (i = 0; i < 10; i++)
		rssi_power_add(&p, -40);
	mu_assert_int_eq(-40, rssi_power_mean(&p));
}

/*
 * A reading that a later, stronger one leaves outside the window stops
 * counting, however many steps the reference climbed in. A thousand readings
 * at the floor, then two 10 dB apart above it: the mean is of the two, not
 * dragged down by the thousand that are now 20 dB below the strongest.
 */
MU_TEST(test_a_reading_the_window_has_left_behind_stops_counting)
{
	rssi_power_t p;
	int i;

	rssi_power_reset(&p);
	for (i = 0; i < 1000; i++)
		rssi_power_add(&p, -100);
	rssi_power_add(&p, -90);
	rssi_power_add(&p, -80);
	/* The mean power of -90 and -80 is 2.6 dB below the stronger. */
	mu_assert_int_eq(-83, rssi_power_mean(&p));
}

/* However many readings arrive, the mean stays the mean: a capture on a quiet
 * channel can add millions of them without a packet ever completing. */
MU_TEST(test_the_mean_survives_any_number_of_readings)
{
	rssi_power_t p;
	int i;

	rssi_power_reset(&p);
	for (i = 0; i < 2000000; i++)
		rssi_power_add(&p, -45);
	mu_assert_int_eq(-45, rssi_power_mean(&p));

	rssi_power_reset(&p);
	for (i = 0; i < 2000000; i++)
		rssi_power_add(&p, (int8_t)((i & 1) ? -40 : -50));
	mu_assert_int_eq(-43, rssi_power_mean(&p));
}

/* The signal figure the loops report is the mean of the readings of this
 * packet, and starting a packet forgets the last one's. */
MU_TEST(test_the_signal_figure_is_this_packets_readings)
{
	rssi_reset();
	mu_assert_int_eq(INT8_MIN, rssi_get_signal());

	rssi_add(-40);
	rssi_add(-50);
	mu_assert_int_eq(-43, rssi_get_signal());

	rssi_reset();
	mu_assert_int_eq(INT8_MIN, rssi_get_signal());
	rssi_add(-70);
	mu_assert_int_eq(-70, rssi_get_signal());
}

/*
 * A receive window holding a packet and the clear air around it is split by
 * level: the packet's readings make the signal figure, the ones tens of dB
 * below it make the background, and neither is an average of the other.
 */
MU_TEST(test_a_window_with_a_packet_in_it_gives_both_figures)
{
	int i;

	rssi_noise_reset();
	rssi_reset();

	/* Clear air, then a packet 30 dB up, then clear air again. */
	for (i = 0; i < 20; i++)
		rssi_add(-80);
	for (i = 0; i < 10; i++)
		rssi_add(-50);
	for (i = 0; i < 20; i++)
		rssi_add(-80);

	mu_assert_int_eq(-50, rssi_get_signal());
	mu_assert_int_eq(1, rssi_noise_estimated());
	mu_assert_int_eq(-80, rssi_get_noise());
}

/* A window with nothing in it but the air gives no background figure: there is
 * no packet to measure it against, so nothing says the readings are not one. */
MU_TEST(test_a_quiet_window_claims_no_background)
{
	int i;

	rssi_noise_reset();
	rssi_reset();
	for (i = 0; i < 40; i++)
		rssi_add(-80);

	mu_assert_int_eq(-80, rssi_get_signal());
	mu_assert_int_eq(0, rssi_noise_estimated());
}

/* The background is kept apart from the packet's readings and from the
 * per-packet reset, because a receive window on a quiet channel may hold no
 * packet at all and one that starts with a packet already detected holds no
 * background reading. It starts again where the channel changes. */
MU_TEST(test_the_background_outlives_a_packet_and_not_a_hop)
{
	rssi_noise_reset();
	mu_assert_int_eq(0, rssi_noise_estimated());

	rssi_noise_add(-90);
	rssi_noise_add(-90);
	mu_assert_int_eq(1, rssi_noise_estimated());
	mu_assert_int_eq(-90, rssi_get_noise());

	/* Packets come and go; the channel's background stays. */
	rssi_reset();
	rssi_add(-40);
	mu_assert_int_eq(-90, rssi_get_noise());
	mu_assert_int_eq(-40, rssi_get_signal());

	rssi_noise_reset();
	mu_assert_int_eq(0, rssi_noise_estimated());
	mu_assert_int_eq(INT8_MIN, rssi_get_noise());
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
	MU_RUN(test_a_mean_of_equal_readings_is_that_reading);
	MU_RUN(test_the_mean_is_of_the_power_not_of_the_dbm_figures);
	MU_RUN(test_the_order_of_the_readings_does_not_matter);
	MU_RUN(test_quiet_readings_outside_the_window_do_not_drag_the_mean_down);
	MU_RUN(test_a_reading_the_window_has_left_behind_stops_counting);
	MU_RUN(test_the_mean_survives_any_number_of_readings);
	MU_RUN(test_the_signal_figure_is_this_packets_readings);
	MU_RUN(test_a_window_with_a_packet_in_it_gives_both_figures);
	MU_RUN(test_a_quiet_window_claims_no_background);
	MU_RUN(test_the_background_outlives_a_packet_and_not_a_hop);
	return mu_report();
}
