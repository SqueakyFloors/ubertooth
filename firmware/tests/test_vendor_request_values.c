/*
 * Which values UBERTOOTH_SET_MOD and UBERTOOTH_SET_CHANNEL accept.
 *
 * Both used to take whatever the host sent. The host tools validate their own
 * options, so this was reached by a raw USB request -- a script driving the
 * board directly -- and in both cases the board reported success:
 *
 *  - a modulation no mode configures was stored, and cc2400_rx() writes no
 *    modem registers for a value it does not recognize, so the next capture
 *    ran with whatever the previous mode had left in the radio;
 *  - a frequency below the synthesizer's range was pulled up to MIN_FREQ, so a
 *    request for 2000 MHz tuned 2268 MHz and said nothing.
 *
 * Both are refused now, by stalling the request, which the host reports in the
 * shared wording for a request a board does not support. What stays accepted
 * matters as much: the band sweep, which the host asks for with a value above
 * MAX_FREQ, and the out-of-band frequencies the radio really can reach, which
 * are a deliberate feature.
 *
 * The handler is hardware code -- it writes radio registers -- so the two
 * cases are modelled here the way test_vendor_request_bounds.c models the
 * ones it covers, and the scans at the end read the firmware source so the
 * models cannot drift away from it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"
#include "cc2400.h"

#ifndef FW_RXTX_DIR
#error "FW_RXTX_DIR must name firmware/bluetooth_rxtx"
#endif

/* The sweep value the host sends (ubertooth.c and ubertooth-scan.c). */
#define HOST_SWEEP_CHANNEL 9999

/* ------------------------------------------------------------------ *
 * The two cases, modelled
 * ------------------------------------------------------------------ */

/* The board state the two cases write. */
static uint8_t modulation;
static uint16_t requested_channel;
static uint8_t hop_mode;

static void reset_board(void)
{
	modulation = MOD_BT_BASIC_RATE;
	requested_channel = 0;
	hop_mode = HOP_NONE;
}

/* UBERTOOTH_SET_MOD. 0 stalls the request. */
static int set_mod(uint16_t value)
{
	if (value != MOD_BT_BASIC_RATE && value != MOD_BT_LOW_ENERGY)
		return 0;
	modulation = value;
	return 1;
}

/* UBERTOOTH_SET_CHANNEL, down to where it assigns requested_channel. */
static int set_channel(uint16_t value)
{
	if (value < MIN_FREQ)
		return 0;

	if (value > MAX_FREQ) {
		hop_mode = HOP_SWEEP;
		requested_channel = 2402;
	} else {
		hop_mode = HOP_NONE;
		requested_channel = value;
	}
	return 1;
}

/* ------------------------------------------------------------------ *
 * The modulation
 * ------------------------------------------------------------------ */

/* The two modulations the radio setup configures, which are the only two any
 * host tool sends. */
MU_TEST(the_two_configured_modulations_are_accepted)
{
	uint16_t defined[] = { MOD_BT_BASIC_RATE, MOD_BT_LOW_ENERGY };
	char msg[256];
	size_t i;

	for (i = 0; i < sizeof(defined) / sizeof(defined[0]); i++) {
		reset_board();
		snprintf(msg, sizeof(msg), "modulation %u was refused",
		         (unsigned)defined[i]);
		mu_assert(set_mod(defined[i]), msg);
		mu_assert_int_eq((int)defined[i], modulation);
	}
}

/*
 * Everything else is refused, including the other two values of enum
 * modulations: no mode implements MOD_80211_FHSS, and the one mode that wants
 * MOD_NONE sets it itself, so neither is anything a request can usefully ask
 * for -- and setting either left the radio configured by whatever ran last.
 */
MU_TEST(a_modulation_no_mode_configures_is_refused)
{
	uint16_t unknown[] = { MOD_80211_FHSS, MOD_NONE, MOD_NONE + 1, 7,
	                       0x80, 0xffff };
	char msg[256];
	size_t i;

	for (i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
		reset_board();
		snprintf(msg, sizeof(msg), "modulation %u was accepted",
		         (unsigned)unknown[i]);
		mu_assert(!set_mod(unknown[i]), msg);
		/* And nothing was stored: a stalled request leaves the board as
		 * it found it. */
		mu_assert_int_eq(MOD_BT_BASIC_RATE, modulation);
	}
}

/* ------------------------------------------------------------------ *
 * The channel
 * ------------------------------------------------------------------ */

/* The Bluetooth band, which is all the host tools ask for. */
MU_TEST(the_bluetooth_band_is_accepted)
{
	uint16_t in_band[] = { BT_FREQ_MIN, 2441, BT_FREQ_MAX };
	char msg[256];
	size_t i;

	for (i = 0; i < sizeof(in_band) / sizeof(in_band[0]); i++) {
		reset_board();
		snprintf(msg, sizeof(msg), "%u MHz was refused",
		         (unsigned)in_band[i]);
		mu_assert(set_channel(in_band[i]), msg);
		mu_assert_int_eq((int)in_band[i], requested_channel);
		mu_assert_int_eq(HOP_NONE, hop_mode);
	}
}

/* Out of the band but inside the synthesizer's range: a deliberate feature,
 * and the reason the refusal is drawn at MIN_FREQ rather than at the band. */
MU_TEST(a_reachable_out_of_band_frequency_is_accepted)
{
	uint16_t out_of_band[] = { MIN_FREQ, 2300, 2500, MAX_FREQ };
	char msg[256];
	size_t i;

	for (i = 0; i < sizeof(out_of_band) / sizeof(out_of_band[0]); i++) {
		reset_board();
		snprintf(msg, sizeof(msg), "%u MHz was refused",
		         (unsigned)out_of_band[i]);
		mu_assert(set_channel(out_of_band[i]), msg);
		mu_assert_int_eq((int)out_of_band[i], requested_channel);
		mu_assert_int_eq(HOP_NONE, hop_mode);
	}
}

/* Above the synthesizer's range still means the band sweep, which is how the
 * host asks for one. */
MU_TEST(the_band_sweep_is_still_asked_for_from_above)
{
	reset_board();
	mu_assert(set_channel(HOST_SWEEP_CHANNEL), "the band sweep was refused");
	mu_assert_int_eq(HOP_SWEEP, hop_mode);
	mu_assert_int_eq(2402, requested_channel);

	reset_board();
	mu_assert(set_channel(MAX_FREQ + 1), "the band sweep was refused");
	mu_assert_int_eq(HOP_SWEEP, hop_mode);
}

/* Below the synthesizer's range is refused, and nothing is left behind: the
 * channel the board was using and its hop mode are untouched, because a
 * stalled request has to leave the board as it found it. */
MU_TEST(an_untunable_frequency_is_refused)
{
	uint16_t untunable[] = { 0, 1, 2000, MIN_FREQ - 1 };
	char msg[256];
	size_t i;

	for (i = 0; i < sizeof(untunable) / sizeof(untunable[0]); i++) {
		reset_board();
		requested_channel = 2441;
		hop_mode = HOP_BLUETOOTH;

		snprintf(msg, sizeof(msg), "%u MHz was accepted",
		         (unsigned)untunable[i]);
		mu_assert(!set_channel(untunable[i]), msg);
		mu_assert_int_eq(2441, requested_channel);
		mu_assert_int_eq(HOP_BLUETOOTH, hop_mode);
	}
}

/* What the old handler did with the same values, which is the fault: a
 * frequency it could not reach became the lowest one it could. */
MU_TEST(the_old_handler_tuned_somewhere_else)
{
	uint16_t value = 2000;
	uint16_t old_result = value;

	if (old_result < MIN_FREQ)
		old_result = MIN_FREQ;
	mu_assert_int_eq(MIN_FREQ, old_result);
	mu_assert(old_result != value,
	          "this is not the case the refusal is for");
}

/* ------------------------------------------------------------------ *
 * The firmware's own shape
 * ------------------------------------------------------------------ */

static char rxtx[600000];

static void load(const char *name, char *out, size_t size)
{
	char path[512];
	char line[SCAN_LINE_MAX];
	char clean[SCAN_LINE_MAX];
	FILE *f;
	int in_block = 0;
	size_t used = 0;

	snprintf(path, sizeof(path), "%s/%s", FW_RXTX_DIR, name);
	f = fopen(path, "r");
	if (f == NULL) {
		/* The scans below read this text, so a file that cannot be read
		 * must stop the test rather than leave it checking nothing. */
		fprintf(stderr, "cannot open %s\n", path);
		exit(1);
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		strip_string_literals(line);
		collapse_spaces(line, clean, sizeof(clean));
		if (clean[0] == '\0')
			continue;
		used += (size_t)snprintf(out + used, size - used, "%s ", clean);
		if (used >= size) {
			fprintf(stderr, "%s is longer than this test holds\n", path);
			exit(1);
		}
	}
	fclose(f);
}

/* The firmware refuses the same two things, in the same places. */
MU_TEST(the_firmware_refuses_what_the_models_refuse)
{
	mu_assert(strstr(rxtx, "case UBERTOOTH_SET_MOD: if (request_params[0] !="
	                       " MOD_BT_BASIC_RATE && request_params[0] !="
	                       " MOD_BT_LOW_ENERGY) return 0; modulation ="
	                       " request_params[0];") != NULL,
	          "UBERTOOTH_SET_MOD does not refuse a modulation no mode"
	          " configures");
	mu_assert(strstr(rxtx, "case UBERTOOTH_SET_CHANNEL: if (request_params[0]"
	                       " < MIN_FREQ) return 0;"
	                       " set_channel_request(request_params[0]);") != NULL,
	          "UBERTOOTH_SET_CHANNEL does not refuse an untunable frequency");
}

/*
 * The refusal comes before anything is touched. set_channel_request() stores
 * the channel and sets the hop mode, and the lines after it keep the channel
 * for a mode that is starting, so a refusal written after either would leave
 * the board tuned -- or a mode about to start -- on a frequency the request
 * was told it could not have.
 */
MU_TEST(nothing_is_stored_before_the_request_is_checked)
{
	const char *c = strstr(rxtx, "case UBERTOOTH_SET_CHANNEL:");
	const char *refusal, *apply, *keep;

	mu_assert_fatal(c != NULL, "UBERTOOTH_SET_CHANNEL is not there to be found");
	refusal = strstr(c, "if (request_params[0] < MIN_FREQ) return 0;");
	apply = strstr(c, "set_channel_request(");
	keep = strstr(c, "stated_channel = request_params[0];");
	mu_assert(refusal != NULL, "the channel case does not refuse anything");
	mu_assert(apply != NULL, "the channel case tunes nothing");
	mu_assert(keep != NULL, "the channel case keeps nothing for a mode start");
	mu_assert(refusal < apply,
	          "the channel is applied before the request is refused");
	mu_assert(refusal < keep,
	          "the channel is kept for a starting mode before the request is"
	          " refused");
}

/* The band sweep is still reached from above MAX_FREQ, and the fixed-channel
 * branch no longer clamps: that clamp is what turned an untunable frequency
 * into MIN_FREQ instead of a refusal. */
MU_TEST(the_firmware_keeps_the_sweep_and_the_reachable_channels)
{
	const char *f = strstr(rxtx, "static void set_channel_request(");

	mu_assert_fatal(f != NULL, "set_channel_request() is not there to be found");
	mu_assert(strstr(f, "requested_channel = requested; if (requested_channel"
	                    " > MAX_FREQ) { hop_mode = HOP_SWEEP;"
	                    " requested_channel = BT_FREQ_MIN; }") != NULL,
	          "the band sweep is no longer reached from above MAX_FREQ");
	mu_assert(strstr(f, "else { hop_mode = HOP_NONE; }") != NULL,
	          "a fixed channel is not taken as it was asked for");
	mu_assert(strstr(f, "MAX(requested_channel, MIN_FREQ)") == NULL,
	          "an untunable frequency is still pulled up to MIN_FREQ");
}

/* The range the refusal is drawn from is the radio's own, out of cc2400.h, not
 * a number written twice. */
MU_TEST(the_range_is_the_radios_own)
{
	mu_assert_int_eq(2268, MIN_FREQ);
	mu_assert_int_eq(2794, MAX_FREQ);
	/* The Bluetooth band sits inside it, which is why in-band frequencies
	 * are unaffected. */
	mu_assert(MIN_FREQ < BT_FREQ_MIN && BT_FREQ_MAX < MAX_FREQ,
	          "the Bluetooth band is not inside the radio's range");
	/* And the host's sweep value is above it, so the sweep is unaffected. */
	mu_assert(HOST_SWEEP_CHANNEL > MAX_FREQ,
	          "the host's sweep value is no longer above the radio's range");
}

int main(void)
{
	printf("Vendor request value tests\n");
	load("bluetooth_rxtx.c", rxtx, sizeof(rxtx));
	MU_RUN(the_two_configured_modulations_are_accepted);
	MU_RUN(a_modulation_no_mode_configures_is_refused);
	MU_RUN(the_bluetooth_band_is_accepted);
	MU_RUN(a_reachable_out_of_band_frequency_is_accepted);
	MU_RUN(the_band_sweep_is_still_asked_for_from_above);
	MU_RUN(an_untunable_frequency_is_refused);
	MU_RUN(the_old_handler_tuned_somewhere_else);
	MU_RUN(the_firmware_refuses_what_the_models_refuse);
	MU_RUN(nothing_is_stored_before_the_request_is_checked);
	MU_RUN(the_firmware_keeps_the_sweep_and_the_reachable_channels);
	MU_RUN(the_range_is_the_radios_own);
	return mu_report();
}
