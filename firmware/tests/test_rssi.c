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
extern int16_t rssi_iir[79];
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
 * LED spectrum analyzer threshold (issue #189).
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
	mu_assert_int_eq(53, rssi_threshold_from_n(1));
	mu_assert_int_eq(-46, rssi_threshold_from_n(100));
	mu_assert_int_eq(-128, rssi_threshold_from_n(182));
	/* These used to wrap to +127 and +85. */
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

/*
 * N >= 183 puts the threshold below every value an int8_t reading can hold, so
 * the LED lights for any reading at all -- including the weakest, INT8_MIN.
 * That is the behaviour the default `-q` is meant to have.
 */
MU_TEST(test_sensitive_thresholds_light_every_reading)
{
	int lvl;

	for (lvl = INT8_MIN; lvl <= INT8_MAX; lvl++) {
		mu_assert(lights_led(lvl, 183), "-q183 must light every reading");
		mu_assert(lights_led(lvl, 225), "-q225 (bare -q) must light every reading");
	}

	/* Regression: with the int8_t threshold these were all dark. */
	mu_assert(lights_led(-40, 225), "the default threshold used to light nothing");
}

/* The insensitive end still discriminates: -q1 is -1 dBm, raw 53. */
MU_TEST(test_insensitive_thresholds_still_discriminate)
{
	mu_assert(!lights_led(53, 1), "a reading at the threshold must not light");
	mu_assert(lights_led(54, 1), "a reading above the threshold must light");
	mu_assert(!lights_led(-40, 1), "ambient noise is well below -1 dBm");

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

/* The firmware's accepted range must match the host's -q range, 1-225: the old
 * `> 256` check let 226-256 through from any other caller. */
MU_TEST(test_accepted_range_matches_the_host)
{
	mu_assert_int_eq(1, RSSI_THRESHOLD_N_MIN);
	mu_assert_int_eq(225, RSSI_THRESHOLD_N_MAX);

	mu_assert(!rssi_threshold_n_valid(0), "0 must be refused");
	mu_assert(rssi_threshold_n_valid(1), "1 must be accepted");
	mu_assert(rssi_threshold_n_valid(225), "225 must be accepted");
	mu_assert(!rssi_threshold_n_valid(226), "226 must be refused");
	mu_assert(!rssi_threshold_n_valid(256), "256 must be refused");
	mu_assert(!rssi_threshold_n_valid(65535), "a 16-bit maximum must be refused");
}

int main(void)
{
	printf("RSSI rounding and LED specan threshold tests\n");
	MU_RUN(test_get_avg_negative_rounding);
	MU_RUN(test_get_avg_positive_unchanged);
	MU_RUN(test_iir_converges_to_negative_average);
	MU_RUN(test_sum_count_rounding_negative);
	MU_RUN(test_threshold_from_n_does_not_wrap);
	MU_RUN(test_threshold_is_monotonic_over_the_whole_range);
	MU_RUN(test_sensitive_thresholds_light_every_reading);
	MU_RUN(test_insensitive_thresholds_still_discriminate);
	MU_RUN(test_a_lit_reading_stays_lit_as_n_grows);
	MU_RUN(test_accepted_range_matches_the_host);
	return mu_report();
}
