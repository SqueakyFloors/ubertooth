/*
 * Seven pieces of arithmetic that are wrong but that nothing in the tree can
 * reach today, so that a change which does make one reachable fails here
 * instead of on a board.
 *
 * None of the seven can be run here: each lives in a translation unit that
 * talks to the radio, the DMA controller or the USB stack, and none of those
 * compiles on a host. So each case has two halves, the way
 * test_tx_generic_bounds.c and test_btbr_arithmetic.c do it: the contract is
 * written out and checked on its own, and the firmware source is then scanned
 * to show it is still written that way. A reformat of a scanned line fails the
 * scan, which is the right direction -- the test says what it could not find
 * rather than passing on nothing.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"

#ifndef FW_RXTX_DIR
#define FW_RXTX_DIR "../bluetooth_rxtx"
#endif
#ifndef FW_COMMON_DIR
#define FW_COMMON_DIR "../common"
#endif
#ifndef FW_BTBR_DIR
#define FW_BTBR_DIR "../btbr"
#endif

#define RXTX_SOURCE   FW_RXTX_DIR "/bluetooth_rxtx.c"
#define EGO_SOURCE    FW_RXTX_DIR "/ego.c"
#define COMMON_SOURCE FW_COMMON_DIR "/ubertooth.c"
#define LE_PHY_SOURCE  FW_RXTX_DIR "/le_phy.c"
#define RX_TASK_SOURCE FW_BTBR_DIR "/src/btphy/rx_task.c"
#define CC2400_HEADER  FW_COMMON_DIR "/cc2400.h"

/* ---- scanning ---------------------------------------------------------- */

/*
 * A whole source with its comments gone and every run of whitespace, newlines
 * included, collapsed to one space. Joining the lines is what lets a case
 * name an expression the source wraps or indents however it likes; dropping
 * the comments is what stops the prose beside an invariant from standing in
 * for the code. Returns NULL if the file will not open; the caller frees it.
 */
static char *source_flattened(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[SCAN_LINE_MAX];
	size_t size = 1 << 16, len = 0;
	char *raw, *flat;
	int in_block = 0;

	if (f == NULL)
		return NULL;
	raw = (char *)malloc(size);
	if (raw == NULL) {
		fclose(f);
		return NULL;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		size_t add;

		strip_comments(line, &in_block);
		add = strlen(line);
		while (len + add + 2 > size) {
			char *bigger;

			size *= 2;
			bigger = (char *)realloc(raw, size);
			if (bigger == NULL) {
				free(raw);
				fclose(f);
				return NULL;
			}
			raw = bigger;
		}
		memcpy(raw + len, line, add);
		len += add;
		raw[len++] = ' ';
	}
	raw[len] = '\0';
	fclose(f);

	flat = (char *)malloc(len + 1);
	if (flat == NULL) {
		free(raw);
		return NULL;
	}
	collapse_spaces(raw, flat, len + 1);
	free(raw);
	return flat;
}

/* The sources every case below reads, flattened once in main(). */
static char *rxtx_source;
static char *le_phy_source;
static char *ego_source;
static char *common_source;
static char *rx_task_source;
static char *cc2400_header;

/* How many times this code appears in a flattened source. */
static int holds_count(const char *flat, const char *code)
{
	int count = 0;
	const char *p = flat;

	if (flat == NULL)
		return -1;
	while ((p = strstr(p, code)) != NULL) {
		count++;
		p++;
	}
	return count;
}

static int holds(const char *flat, const char *code)
{
	return holds_count(flat, code) > 0;
}

/* Every scan below reads one of these, so a source that will not open is
 * reported once rather than as a run of missing lines. */
MU_TEST(test_every_scanned_source_opens)
{
	mu_assert_fatal(rxtx_source != NULL, RXTX_SOURCE " would not open");
	mu_assert_fatal(le_phy_source != NULL, LE_PHY_SOURCE " would not open");
	mu_assert_fatal(ego_source != NULL, EGO_SOURCE " would not open");
	mu_assert_fatal(common_source != NULL, COMMON_SOURCE " would not open");
	mu_assert_fatal(rx_task_source != NULL, RX_TASK_SOURCE " would not open");
	mu_assert_fatal(cc2400_header != NULL, CC2400_HEADER " would not open");
}

/* ---- the advertising record the board transmits ------------------------- */

/*
 * MODE_BT_PERIPHERAL_LE builds one ADV_IND and transmits it over and over: two
 * header bytes, the six address bytes, the advertising data the host set, and
 * three CRC bytes written in after them.
 *
 * The buffer was 32 bytes and the length was checked against a 255-byte
 * bound -- the limit for extended advertising, not for the legacy ADV_IND
 * this sends. Advertising data of 22 bytes or more therefore wrote the CRC
 * past the end of the buffer, and the running length, a uint8_t, wrapped at
 * 250 bytes of data.
 *
 * Both in-tree senders use 3 bytes (the default) and 18 (ubertooth-btle -a
 * and ubertooth-ducky), so neither reaches it. The request that sets the data
 * now bounds it by what an ADV_IND can carry as well, so a program that asks
 * for more is told so rather than having the mode go idle on it; the check in
 * the mode stays as the guard on the record it builds.
 */
#define ADV_IND_RECORD_BYTES (2 + 6 + ADV_IND_MAX_DATA + 3)

MU_TEST(test_the_advertising_record_fits_the_data_the_mode_accepts)
{
	unsigned data_len;

	/* The legacy limit, and the six-bit length field that has to hold the
	 * address bytes and the data together. */
	mu_assert_int_eq(31, ADV_IND_MAX_DATA);
	mu_assert(6 + ADV_IND_MAX_DATA <= 0x3f,
	          "a full ADV_IND payload does not fit the PDU length field");

	/* Every length the mode accepts leaves the three CRC bytes inside the
	 * record. 42 bytes is where the old 32-byte buffer fell short. */
	mu_assert_int_eq(42, ADV_IND_RECORD_BYTES);
	for (data_len = 0; data_len <= ADV_IND_MAX_DATA; data_len++) {
		const unsigned len = 2 + 6 + data_len;   /* before the CRC */

		mu_assert(len + 3 <= ADV_IND_RECORD_BYTES,
		          "an accepted advertising length writes its CRC past "
		          "the end of the record");
		/* And the length the radio is given stays a byte. */
		mu_assert(len + 3 <= 0xff,
		          "the record is longer than le_transmit() can be told");
	}
}

MU_TEST(test_the_advertising_mode_bounds_its_data_by_the_legacy_limit)
{
	mu_assert(holds(rxtx_source,
	                "uint8_t adv_ind[2 + 6 + ADV_IND_MAX_DATA + 3]"),
	          "the advertising record is not sized from ADV_IND_MAX_DATA");
	mu_assert(holds(rxtx_source, "if (le_adv_len > ADV_IND_MAX_DATA)"),
	          "the advertising length is not checked against the legacy limit");
	mu_assert(holds(rxtx_source, "unsigned adv_ind_len;"),
	          "the record's running length is narrow enough to wrap");

	/* The request that sets the data is bounded by the same limit, so the
	 * program that sent it is where an oversized payload is refused, and
	 * the buffer it copies into is sized for what it accepts. */
	mu_assert(holds(rxtx_source,
	                "if (!data_from_host || data_in_len > ADV_IND_MAX_DATA)"),
	          "the request that sets advertising data is not bounded by the "
	          "legacy limit");
	mu_assert(holds(rxtx_source, "uint8_t le_adv_data[ADV_IND_MAX_DATA]"),
	          "the advertising data buffer is not sized from ADV_IND_MAX_DATA");
}

/* ---- the E-GO capture length -------------------------------------------- */

/*
 * The E-GO mode reads packet_len bytes out of the radio into a buffer of
 * EGO_MAX_PACKET_LEN (48), and the subcommand that sets it took the host's
 * byte as it came: 255 wrote 207 bytes past the end. ubertooth-ego clamps its
 * -l to 1..48 before sending it, which is why nothing reaches it.
 */
MU_TEST(test_the_ego_capture_length_is_bounded_by_its_buffer)
{
	/* The host tool's own range, EGO_PACKET_LENGTH_MIN..MAX in
	 * host/ubertooth-tools/src/ego_options.h, is the buffer's. */
	mu_assert(holds(ego_source, "#define EGO_MAX_PACKET_LEN 48"),
	          "the E-GO buffer is no longer 48 bytes, so the bound below "
	          "and the host tool's range need checking together");
	mu_assert(holds(ego_source,
	                "if (body[0] == 0 || body[0] > EGO_MAX_PACKET_LEN) return 0;"),
	          "the E-GO capture length is not bounded by its buffer");
}

/* ---- the CRCInit reversal ----------------------------------------------- */

/* The reversal of the low 24 bits of a word, which is what a reversed CRCInit
 * is: rbit() reverses all 32, so its result has to come down by a byte. */
static uint32_t reverse24(uint32_t value)
{
	uint32_t out = 0;
	unsigned i;

	for (i = 0; i < 24; i++)
		out |= ((value >> i) & 1) << (23 - i);
	return out;
}

/* rbit(), written out: the firmware's is one ARM instruction. */
static uint32_t reverse32(uint32_t value)
{
	uint32_t out = 0;
	unsigned i;

	for (i = 0; i < 32; i++)
		out |= ((value >> i) & 1) << (31 - i);
	return out;
}

/*
 * The connection handler in bluetooth_rxtx.c read a CRCInit off a CONNECT_IND
 * and reversed all 32 bits of it, which leaves the field shifted up a byte
 * with a zero byte in the bottom: 0x123456 needs 0x6a2c48 and became
 * 0x2c4800. CRC verification is switched off two lines above and the follow
 * goes through le_phy_main() now, so the value is never used -- but the
 * promiscuous path's own reversal, and reset_le()'s, are right, and this one
 * has to agree with them.
 */
MU_TEST(test_a_reversed_crc_init_is_twenty_four_bits_wide)
{
	static const uint32_t cases[] = {
		0x555555,   /* the advertising CRCInit */
		0x123456,
		0x000001,
		0xffffff,
	};
	unsigned i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		mu_assert_int_eq(reverse24(cases[i]),
		                 reverse32(cases[i]) >> 8);
		/* And the unshifted reversal is a different number, so the
		 * case above is checking something. */
		mu_assert(reverse32(cases[i]) != reverse24(cases[i]),
		          "a 32-bit reversal of this value is already the "
		          "24-bit one, so this case checks nothing");
	}

	/* reset_le()'s constant is the reversal of the advertising CRCInit. */
	mu_assert_int_eq(0xAAAAAA, reverse24(0x555555));

	mu_assert(holds(rxtx_source,
	                "le.crc_init_reversed = rbit(le.crc_init) >> 8;"),
	          "the connection handler reverses a 24-bit CRCInit over 32 bits");
}

/* ---- the clock drift correction interval -------------------------------- */

/*
 * How many CLKN ticks apart the clock corrections go, from the drift the host
 * reports in parts per million. A drift above 320 PPM divided to zero, and
 * zero is how the handler's own "do not correct" branch is spelled, so a large
 * drift switched correction off instead of correcting as fast as it can. The
 * host computes tens of PPM from a measured offset, so it does not reach it.
 */
static uint32_t drift_interval(uint32_t ppm)
{
	uint32_t ticks = 320 / ppm;

	return (ticks == 0) ? 1 : ticks;
}

MU_TEST(test_a_large_drift_corrects_every_tick_rather_than_never)
{
	/* What the host asks for today, unchanged: the division truncates, and
	 * leaving that alone is what keeps the correction rate as it is. */
	mu_assert_int_eq(32, drift_interval(10));
	mu_assert_int_eq(12, drift_interval(25));
	mu_assert_int_eq(6, drift_interval(50));

	/* The boundary, and past it. */
	mu_assert_int_eq(1, drift_interval(320));
	mu_assert_int_eq(1, drift_interval(321));
	mu_assert_int_eq(1, drift_interval(1000));
	mu_assert_int_eq(1, drift_interval(32767));

	/* Both branches, the too-slow one and the too-fast one. */
	mu_assert_int_eq(2, holds_count(rxtx_source,
	                                "if (clk_drift_correction == 0) "
	                                "clk_drift_correction = 1;"));
}

/*
 * The drift figure the board keeps, after a request has been added to it.
 *
 * Each UBERTOOTH_FIX_CLOCK_DRIFT request adds to a running figure. The figure
 * is 16 bits wide, so without a bound a run of requests walks past what it
 * holds and comes back with the other sign: the board then corrects its clock
 * in the direction opposite to the one the host asked for. Nothing beyond
 * CLK_DRIFT_PPM_MAX corrects any faster either, which is where the bound is.
 */
#define CLK_DRIFT_PPM_MAX 320

static int16_t drift_accumulate(int16_t running, int16_t request)
{
	int32_t sum = (int32_t)running + request;

	if (sum > CLK_DRIFT_PPM_MAX)
		sum = CLK_DRIFT_PPM_MAX;
	else if (sum < -CLK_DRIFT_PPM_MAX)
		sum = -CLK_DRIFT_PPM_MAX;
	return (int16_t)sum;
}

MU_TEST(test_the_accumulated_drift_figure_stays_within_what_it_can_correct)
{
	int16_t ppm = 0;
	int i;

	/* What a capture does today: a handful of tens of PPM, added up. */
	ppm = drift_accumulate(ppm, 25);
	mu_assert_int_eq(25, ppm);
	ppm = drift_accumulate(ppm, -10);
	mu_assert_int_eq(15, ppm);
	mu_assert_int_eq(21, drift_interval(15));

	/* The bound, from either side, and the correction it gives. */
	mu_assert_int_eq(320, drift_accumulate(0, 320));
	mu_assert_int_eq(320, drift_accumulate(0, 400));
	mu_assert_int_eq(-320, drift_accumulate(0, -400));
	mu_assert_int_eq(1, drift_interval(320));

	/* A run of requests at the largest value the host can send. Unbounded,
	 * the figure passes 32767 after 103 of them and comes back negative:
	 * the correction then runs the clock the other way. */
	ppm = 0;
	for (i = 0; i < 1000; i++) {
		ppm = drift_accumulate(ppm, 320);
		mu_assert(ppm > 0, "the accumulated figure changed sign");
		mu_assert(ppm <= CLK_DRIFT_PPM_MAX,
		          "the accumulated figure passed what it can correct");
	}
	mu_assert_int_eq(320, ppm);

	/* The same the other way, and a request back towards zero is still
	 * taken: the figure saturates, it does not stick. */
	ppm = 0;
	for (i = 0; i < 1000; i++)
		ppm = drift_accumulate(ppm, -320);
	mu_assert_int_eq(-320, ppm);
	ppm = drift_accumulate(ppm, 300);
	mu_assert_int_eq(-20, ppm);

	/* The handler adds in 32 bits and stores the bounded figure, rather
	 * than adding into the 16-bit figure itself. */
	mu_assert(holds(rxtx_source,
	                "drift_ppm = (int32_t)clk_drift_ppm + "
	                "(int16_t)((data[0] << 8) | (data[1] << 0));"),
	          "the drift request adds the two in 32 bits");
	mu_assert(holds(rxtx_source,
	                "if (drift_ppm > CLK_DRIFT_PPM_MAX) "
	                "drift_ppm = CLK_DRIFT_PPM_MAX; "
	                "else if (drift_ppm < -CLK_DRIFT_PPM_MAX) "
	                "drift_ppm = -CLK_DRIFT_PPM_MAX;"),
	          "the drift request bounds the figure both ways");
	mu_assert(holds(rxtx_source, "clk_drift_ppm = (int16_t)drift_ppm;"),
	          "the drift request stores the bounded figure");
}

/* ---- the channel a record reports --------------------------------------- */

/*
 * usb_pkt_rx.channel is a one-byte offset from 2402 MHz, so it only reaches
 * channel 78, while the channel request accepts any frequency the radio tunes
 * -- MIN_FREQ 2268 to MAX_FREQ 2794 MHz. Subtracted and narrowed, 2658 to
 * 2736 MHz land on channels 0 to 78, where the host takes them for real
 * channels and indexes its per-channel arrays with them. Every shipped tool
 * parses its channel against 2402..2480, so nothing reaches it.
 */
#define RADIO_FREQ_MIN 2268
#define RADIO_FREQ_MAX 2794

/* ubertooth_channel_byte() is a static inline in the shared interface header,
 * so unlike the rest of this file the real one runs below. */

MU_TEST(test_a_frequency_outside_the_band_reports_no_channel)
{
	unsigned freq;

	/* In the band, the channel index, ends included. */
	mu_assert_int_eq(0, ubertooth_channel_byte(BT_FREQ_MIN));
	mu_assert_int_eq(78, ubertooth_channel_byte(BT_FREQ_MAX));
	for (freq = BT_FREQ_MIN; freq <= BT_FREQ_MAX; freq++)
		mu_assert_int_eq(freq - BT_FREQ_MIN,
		                 ubertooth_channel_byte((uint16_t)freq));

	/* Outside it, nothing the host would take for a channel. The host
	 * discards every value above channel 78. */
	mu_assert(CHANNEL_NONE > NUM_BREDR_CHANNELS - 1,
	          "the out-of-band value is one the host takes for a channel");
	for (freq = RADIO_FREQ_MIN; freq < BT_FREQ_MIN; freq++)
		mu_assert_int_eq(CHANNEL_NONE,
		                 ubertooth_channel_byte((uint16_t)freq));
	for (freq = BT_FREQ_MAX + 1; freq <= RADIO_FREQ_MAX; freq++)
		mu_assert_int_eq(CHANNEL_NONE,
		                 ubertooth_channel_byte((uint16_t)freq));

	/* The frequencies that used to masquerade as in-band channels. */
	mu_assert_int_eq(0, (uint8_t)((2658 - BT_FREQ_MIN) & 0xff));
	mu_assert_int_eq(78, (uint8_t)((2736 - BT_FREQ_MIN) & 0xff));
	mu_assert_int_eq(CHANNEL_NONE, ubertooth_channel_byte(2658));
	mu_assert_int_eq(CHANNEL_NONE, ubertooth_channel_byte(2736));
}

/* Both records that carry a channel go through it, and the radio's range is
 * still the one the contract above is written against. */
MU_TEST(test_every_record_reports_its_channel_the_same_way)
{
	/* All four places a record's channel is filled: enqueue(),
	 * enqueue_with_ts() and the low-energy metadata in each image. */
	mu_assert_int_eq(3, holds_count(rxtx_source, "= ubertooth_channel_byte("));
	mu_assert_int_eq(1, holds_count(le_phy_source, "= ubertooth_channel_byte("));
	mu_assert(!holds(rxtx_source, "- 2402) & 0xff"),
	          "a record still narrows a frequency into its channel byte");
	mu_assert(!holds(le_phy_source, "- 2402) & 0xff"),
	          "the low-energy engine still narrows a frequency into its "
	          "channel byte");
	mu_assert(holds(cc2400_header, "#define MIN_FREQ 2268"),
	          "the radio's lowest frequency moved");
	mu_assert(holds(cc2400_header, "#define MAX_FREQ 2794"),
	          "the radio's highest frequency moved");
}

/* ---- the microsecond wait ----------------------------------------------- */

/*
 * wait_us() turns microseconds into a loop count with six shifts, and the
 * loop decrements before it tests. Fewer than four microseconds shift away to
 * nothing, and a count of zero wraps and spins through all 2^32 of them --
 * close to two hours on the 4 MHz oscillator this is calibrated for, with
 * nothing feeding the watchdog. The only callers pass 50 microseconds and one
 * millisecond.
 */
static uint32_t wait_us_count(uint32_t us)
{
	uint32_t count = (us>>2) + (us>>3) + (us>>6) + (us>>7) + (us>>10) + (us>>11);

	return (count == 0) ? 1 : count;
}

MU_TEST(test_the_shortest_wait_is_one_pass_not_four_billion)
{
	uint32_t us;

	/* Where the shifts come to nothing. One pass is the shortest wait
	 * there is, and it is far shorter than the microseconds asked for. */
	mu_assert_int_eq(1, wait_us_count(0));
	mu_assert_int_eq(1, wait_us_count(1));
	mu_assert_int_eq(1, wait_us_count(2));
	mu_assert_int_eq(1, wait_us_count(3));

	/* From four microseconds up the shifts already give at least one, so
	 * the floor changes nothing the callers ask for. */
	mu_assert_int_eq(1, wait_us_count(4));
	mu_assert_int_eq(18, wait_us_count(50));
	mu_assert_int_eq(397, wait_us_count(1000));
	for (us = 4; us <= 4096; us++)
		mu_assert(wait_us_count(us) ==
		          ((us>>2) + (us>>3) + (us>>6) + (us>>7) + (us>>10) + (us>>11)),
		          "the floor changed a wait the callers ask for");

	mu_assert(holds(common_source, "if (count == 0) count = 1;"),
	          "the microsecond wait has no floor on its loop count");
	mu_assert(holds(common_source, "while(--wait_us_counter);"),
	          "the wait loop is not the one the floor is there for");
}

/* ---- the receive task's slot count -------------------------------------- */

/*
 * The BR/EDR receive task counts down the slots a packet may take, and
 * decremented a uint8_t with no floor: a count already at zero wrapped to 255
 * and then bounded nothing for 255 more slots. The DMA reaches the packet's
 * air byte count first today, which is the other way the last slot is
 * recognised, so the count never runs out.
 */
MU_TEST(test_the_slot_count_has_a_floor)
{
	mu_assert(holds(rx_task_source,
	                "if (rx_task.slot_num > 0) rx_task.slot_num--;"),
	          "the slot count is decremented without a floor");
	mu_assert(holds(rx_task_source, "last = rx_task.slot_num == 0;"),
	          "the last slot is not recognised from the floored count");
	mu_assert(!holds(rx_task_source, "--rx_task.slot_num == 0"),
	          "the slot count still wraps below zero");
}

int main(void)
{
	printf("latent arithmetic, type and unit tests\n");

	rxtx_source = source_flattened(RXTX_SOURCE);
	le_phy_source = source_flattened(LE_PHY_SOURCE);
	ego_source = source_flattened(EGO_SOURCE);
	common_source = source_flattened(COMMON_SOURCE);
	rx_task_source = source_flattened(RX_TASK_SOURCE);
	cc2400_header = source_flattened(CC2400_HEADER);

	MU_RUN(test_every_scanned_source_opens);
	MU_RUN(test_the_advertising_record_fits_the_data_the_mode_accepts);
	MU_RUN(test_the_advertising_mode_bounds_its_data_by_the_legacy_limit);
	MU_RUN(test_the_ego_capture_length_is_bounded_by_its_buffer);
	MU_RUN(test_a_reversed_crc_init_is_twenty_four_bits_wide);
	MU_RUN(test_a_large_drift_corrects_every_tick_rather_than_never);
	MU_RUN(test_the_accumulated_drift_figure_stays_within_what_it_can_correct);
	MU_RUN(test_a_frequency_outside_the_band_reports_no_channel);
	MU_RUN(test_every_record_reports_its_channel_the_same_way);
	MU_RUN(test_the_shortest_wait_is_one_pass_not_four_billion);
	MU_RUN(test_the_slot_count_has_a_floor);

	return mu_report();
}
