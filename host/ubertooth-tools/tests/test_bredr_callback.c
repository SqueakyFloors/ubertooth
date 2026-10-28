/*
 * The arithmetic in the Basic Rate receive callbacks: how many symbols the
 * decoder is told it has, the CLKN a packet's timestamp comes to, the signal
 * level a channel's first packets report, the clock trim a piconet follow
 * sends to the board, and which channels AFH detection marks used.
 *
 * Each of those was wrong in its own way, and none of them was reachable from
 * a test before, because they all live inside the callbacks. This test
 * compiles ubertooth_callback.c into itself, so the real callbacks and the
 * real helpers run, with the ring buffer, the symbol unpacking, the decoder
 * entry points and the control requests replaced by the fakes below. Nothing
 * here touches a device.
 *
 * libbtbb is still linked, because the callbacks allocate a real packet and
 * read fields back out of it; only the three entry points whose arguments are
 * what this test is about are replaced.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
#include "../../libubertooth/src/ubertooth_callback.c"

/* The two symbol values the faked unpacking writes. Neither is zero, so a
 * zero symbol reaching the decoder came from past the symbols the record
 * carried, which is the bug this file starts with. */
#define SYM_EVEN 1
#define SYM_ODD  2

/* ---- globals the library would define ----------------------------------- */

uint32_t systime;
FILE* infile;
FILE* dumpfile;
int max_ac_errors = 2;
unsigned int packet_counter_max;

/* ---- fakes -------------------------------------------------------------- */

/* The records the faked capture hands out, in order. */
static usb_pkt_rx next_packets[8];
static unsigned next_count;
static unsigned next_index;

/* What the faked decoder entry points were given on the last call, and what
 * the faked control requests carried. */
static struct {
	int find_ac_search_length;
	int set_data_length;
	uint32_t set_data_clkn;
	int set_data_zero_symbols; /* symbols nothing wrote, handed over anyway */
	char set_data_first_symbol;
	int set_data_calls;

	int trims;
	uint16_t trim_field;
	int drifts;
	int16_t drift_ppm_sent;
	int hopping_restarts;
	int afh_maps_sent;
} seen;

/* What the faked access-code search reports: where in the bank it found one,
 * or a negative number for nothing found. */
static int find_ac_offset;

/* What the faked decoder makes of the packet. A negative result is how the
 * real one says it could not place the packet in the piconet. */
static int process_packet_result;

usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	(void)fifo;
	if (next_index < next_count)
		return next_packets[next_index++];
	return next_packets[0];
}

uint64_t fifo_popped_arrival_ns(const fifo_t* fifo) { (void)fifo; return 0; }

/* Fills the symbols a record carries, as the real unpacking does: one byte per
 * captured symbol, BANK_LEN of them and not one more. The values alternate so
 * a caller that hands the decoder a buffer longer than this is caught by the
 * zeros past the end rather than by luck. */
void ubertooth_unpack_symbols(const uint8_t* buf, char* unpacked)
{
	int i;

	(void)buf;
	for (i = 0; i < BANK_LEN; i++)
		unpacked[i] = (char)((i % 2) ? SYM_ODD : SYM_EVEN);
}

/* cb_btle() is compiled in with the rest of the file and needs this to link;
 * no case here goes near it. */
int le_usb_reassemble(le_usb_reassembly* state, const usb_pkt_rx* frame,
                      uint64_t arrival_ns, const uint8_t** data,
                      uint16_t* length, const usb_pkt_rx** metadata,
                      uint64_t* metadata_arrival_ns)
{
	(void)state; (void)frame; (void)arrival_ns; (void)data;
	(void)length; (void)metadata; (void)metadata_arrival_ns;
	return LE_REASSEMBLY_WAIT;
}

void cmd_trim_clock(struct libusb_device_handle* devh, uint16_t offset)
{
	(void)devh;
	seen.trims++;
	seen.trim_field = offset;
}

void cmd_fix_clock_drift(struct libusb_device_handle* devh, int16_t ppm)
{
	(void)devh;
	seen.drifts++;
	seen.drift_ppm_sent = ppm;
}

int cmd_start_hopping(struct libusb_device_handle* devh, int clkn_offset,
                      int clk100ns_offset)
{
	(void)devh; (void)clkn_offset; (void)clk100ns_offset;
	seen.hopping_restarts++;
	return 0;
}

int cmd_set_afh_map(struct libusb_device_handle* devh, u8* afh_map)
{ (void)devh; (void)afh_map; seen.afh_maps_sent++; return 0; }

int cmd_hop(struct libusb_device_handle* devh) { (void)devh; return 0; }

/* Stands in for libbtbb's access-code search. It reports the offset the case
 * chose and records the search length it was given: a search allowed to run
 * past the symbols the record carried is the bug on this line. */
int btbb_find_ac(char* stream, int search_length, uint32_t lap,
                 int max_errors, btbb_packet** pkt)
{
	(void)stream; (void)lap; (void)max_errors;
	seen.find_ac_search_length = search_length;
	if (find_ac_offset < 0)
		return find_ac_offset;
	/* The real search tries start positions 0 to search_length - 1 and
	 * reports nothing outside them, so a position the caller's bound does
	 * not reach has to come back as not found here too. Without this, a
	 * bound that is too small still passes every test that places an
	 * access code near the end of the bank. */
	if (find_ac_offset >= search_length)
		return -1;
	*pkt = btbb_packet_new();
	return find_ac_offset;
}

/* Stands in for handing the packet to the decoder. It records the length and
 * the clock it was given, and counts the symbols in that length that nothing
 * wrote: the real decoder would decode a payload out of those. */
void btbb_packet_set_data(btbb_packet* pkt, char* syms, int length,
                          uint8_t channel, uint32_t clkn)
{
	int i;

	(void)pkt; (void)channel;
	seen.set_data_calls++;
	seen.set_data_length = length;
	seen.set_data_clkn = clkn;
	seen.set_data_first_symbol = length > 0 ? syms[0] : 0;
	seen.set_data_zero_symbols = 0;
	for (i = 0; i < length; i++)
		if (syms[i] == 0)
			seen.set_data_zero_symbols++;
}

int btbb_process_packet(btbb_packet* pkt, btbb_piconet* pn)
{
	(void)pkt; (void)pn;
	return process_packet_result;
}

/* ---- helpers ------------------------------------------------------------ */

/* A Basic Rate record with the given timestamp, period count and channel. */
static void fill_packet(usb_pkt_rx* rx, uint32_t clk100ns, uint8_t periods,
                        uint8_t channel)
{
	memset(rx, 0, sizeof(*rx));
	rx->pkt_type = BR_PACKET;
	rx->channel = channel;
	rx->clkn_high = periods;
	rx->clk100ns = htole32(clk100ns);
	rx->rssi_max = -40;
	rx->rssi_min = -60;
	rx->rssi_avg = 0;
	rx->rssi_count = 8;
}

/* The handle the last run_callback() used, so a case can read what the
 * callbacks recorded on it, and what they wrote to standard error. */
static ubertooth_t last_ut;
static char last_stderr[4096];

/* Runs the queued records through one callback. Standard output goes to
 * /dev/null -- the callbacks print a line per packet and the follow prints its
 * notes, and neither belongs in a test's output -- and standard error is kept,
 * so a case can pin a warning and how often it was printed. */
static void run_callback(void (*cb)(ubertooth_t*, void*), void* args)
{
	int saved_out, saved_err, devnull;
	FILE* errors = tmpfile();
	size_t read_bytes = 0;

	memset(&last_ut, 0, sizeof(last_ut));
	memset(last_stderr, 0, sizeof(last_stderr));
	last_ut.clk100ns_period = CLK100NS_PERIOD;
	next_index = 0;

	fflush(NULL);
	saved_out = dup(fileno(stdout));
	saved_err = dup(fileno(stderr));
	devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, fileno(stdout));
		close(devnull);
	}
	if (errors != NULL)
		dup2(fileno(errors), fileno(stderr));

	while (next_index < next_count)
		cb(&last_ut, args);

	fflush(NULL);
	dup2(saved_out, fileno(stdout));
	dup2(saved_err, fileno(stderr));
	close(saved_out);
	close(saved_err);

	if (errors != NULL) {
		rewind(errors);
		read_bytes = fread(last_stderr, 1, sizeof(last_stderr) - 1, errors);
		last_stderr[read_bytes] = '\0';
		fclose(errors);
	}
}

/* How many times `needle` appears in what the last run wrote to standard
 * error: a warning that is meant to be said once has to be counted, not just
 * found. */
static int stderr_lines_with(const char* needle)
{
	const char* p = last_stderr;
	int count = 0;

	for (p = strstr(p, needle); p != NULL; p = strstr(p + 1, needle))
		count++;
	return count;
}

/* The state the callbacks keep between packets, back to what a fresh process
 * has: the RSSI table, and the record queue and recordings. The clock trim's
 * own counters are only reachable through cb_rx() itself, so the follow case
 * below is the last to run. */
static void reset_state(void)
{
	memset(rssi_history, 0, sizeof(rssi_history));
	rssi_history_filled = 0;
	memset(&seen, 0, sizeof(seen));
	memset(next_packets, 0, sizeof(next_packets));
	next_count = 0;
	next_index = 0;
	find_ac_offset = 0;
	process_packet_result = 0;
	infile = NULL;
	dumpfile = NULL;
}

/* The timestamp a record needs to carry for its access code, found at the
 * start of the bank, to sit a chosen offset after the CLKN edge, `slots` slots
 * into the clock. cb_rx() measures that offset as (clk100ns + offset*10 + one
 * slot - one bank) modulo one slot. */
static uint32_t clk100ns_for_clk_offset(uint16_t clk_offset, uint32_t slots)
{
	return (uint32_t)(clk_offset + BANK_100NS - CLK100NS_PER_SLOT +
	                  CLK100NS_PER_SLOT * (uint64_t)slots);
}

/* ---- the symbols the decoder is told it has ----------------------------- */

MU_TEST(the_decoder_is_told_only_the_symbols_the_record_holds)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_count = 1;
	find_ac_offset = 0;
	run_callback(cb_rx, NULL);

	mu_assert_int_eq(1, seen.set_data_calls);
	/* The 400 symbols a record carries. It used to claim BANK_LEN*10 -
	 * 4000 - which the decoder clamps to 3125, so a DH5's 2712 payload
	 * symbols were decoded, and its CRC verdict reached the screen and the
	 * capture file, out of about 2700 bytes the buffer started as. */
	mu_assert_int_eq(BANK_LEN, seen.set_data_length);
	mu_assert_int_eq(0, seen.set_data_zero_symbols);
	mu_assert_int_eq(SYM_EVEN, seen.set_data_first_symbol);
}

MU_TEST(the_decoder_is_given_the_symbols_from_the_access_code_on)
{
	/* An access code found at the last offset the search can report: the
	 * one whose 64th symbol is the bank's last. */
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_count = 1;
	find_ac_offset = BANK_LEN - AC_SYMBOLS;
	run_callback(cb_rx, NULL);

	/* 64 symbols: the access code and nothing after it. */
	mu_assert_int_eq(AC_SYMBOLS, seen.set_data_length);
	mu_assert_int_eq(0, seen.set_data_zero_symbols);
	mu_assert_int_eq(SYM_EVEN, seen.set_data_first_symbol);

	/* And at the first. */
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_count = 1;
	find_ac_offset = 1;
	run_callback(cb_rx, NULL);

	mu_assert_int_eq(BANK_LEN - 1, seen.set_data_length);
	mu_assert_int_eq(0, seen.set_data_zero_symbols);
	mu_assert_int_eq(SYM_ODD, seen.set_data_first_symbol);
}

MU_TEST(the_access_code_search_reaches_the_last_position_in_the_bank)
{
	/* Every start position whose whole access code was captured, and not
	 * one more: a correlator allowed to run to the end of the bank reads
	 * 64 symbols past what arrived, and a bound of BANK_LEN - AC_SYMBOLS
	 * misses the last position that did arrive whole. cb_rx() used to pass
	 * the whole bank. */
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_count = 1;
	run_callback(cb_rx, NULL);
	/* 337, written out: the same expression as the code under test would
	 * hold however AC_SYMBOLS were defined. */
	mu_assert_int_eq(337, seen.find_ac_search_length);

	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_count = 1;
	run_callback(cb_scan, NULL);
	mu_assert_int_eq(337, seen.find_ac_search_length);
	mu_assert_int_eq(400, seen.set_data_length);

	/* The three AFH callbacks pass the same constant and have no fake to
	 * run them through here, so the constant itself is pinned: that covers
	 * every call site at once, including any added later. */
	mu_assert_int_eq(337, AC_SEARCH_POSITIONS);
}

/* ---- the CLKN a timestamp comes to -------------------------------------- */

MU_TEST(a_timestamp_at_the_start_of_the_clock_gives_clkn_zero)
{
	/* A bank whose stamp is exactly one bank in started at the clock's
	 * zero. */
	mu_assert_int_eq(0, (int)clkn_at_access_code(0, BANK_100NS, 0));
	/* Anything inside that first tick is still tick zero, up to the last
	 * 100 ns of it. */
	mu_assert_int_eq(0, (int)clkn_at_access_code(0, BANK_100NS +
	                                             CLK100NS_PER_CLKN - 1, 0));
	mu_assert_int_eq(1, (int)clkn_at_access_code(0, BANK_100NS +
	                                             CLK100NS_PER_CLKN, 0));
}

MU_TEST(a_timestamp_inside_the_first_bank_does_not_jump_the_clock_forwards)
{
	/* The case that was seven minutes out: the first 400 µs after the
	 * firmware zeroes CLKN at mode start, and the same window at every
	 * 327.68 s wrap. Unsigned arithmetic made 1000 - 4000 about 2^32, so
	 * CLKN came to 1374388 ticks - seven minutes - ahead of a true zero.
	 * The access code is 3000 ticks before the clock's zero, which is
	 * inside the tick before it, and on a counter that starts over that is
	 * the top of its range. */
	mu_assert_int_eq(0x0fffffff, (int)clkn_at_access_code(0, 1000, 0));
	mu_assert(clkn_at_access_code(0, 1000, 0) != 1374388,
	          "the timestamp still underflows into next week");

	/* One 100 ns tick short of the bank, which is the last stamp that
	 * borrows at all, and the oldest stamp that borrows: a whole bank
	 * before the clock's zero is two ticks back. */
	mu_assert_int_eq(0x0fffffff,
	                 (int)clkn_at_access_code(0, BANK_100NS - 1, 0));
	mu_assert_int_eq(0x0ffffffe, (int)clkn_at_access_code(0, 0, 0));

	/* The same borrow one period in takes a period off the count instead
	 * of wrapping: the tick before (1 << 20). */
	mu_assert_int_eq((1 << 20) - 1,
	                 (int)clkn_at_access_code(1, BANK_100NS - 1, 0));
}

MU_TEST(the_clkn_counts_from_the_access_code_not_the_bank)
{
	/* cb_scan() divided the raw stamp with no bank length taken off, which
	 * put every packet a whole slot late: this stamp reported CLKN 3, so
	 * CLK1 1, where the access code's first bit is at 6000 and CLKN is 1.
	 * ubertooth-scan prints that CLK1 and its survey mode builds clock
	 * candidates out of differences between these. */
	mu_assert_int_eq(1, (int)clkn_at_access_code(0, 10000, 0));
	mu_assert_int_eq(3, (int)(10000 / CLK100NS_PER_CLKN));

	/* Where in the bank the access code was found counts too, at one
	 * 100 ns tick per ten symbols. */
	mu_assert_int_eq(0, (int)clkn_at_access_code(0, BANK_100NS, 312));
	mu_assert_int_eq(1, (int)clkn_at_access_code(0, BANK_100NS, 313));

	/* The period count is CLKN's top eight bits. */
	mu_assert_int_eq((7 << 20) + 2,
	                 (int)clkn_at_access_code(7, BANK_100NS +
	                                          2 * CLK100NS_PER_CLKN, 0));
}

MU_TEST(the_callbacks_hand_the_decoder_that_clkn)
{
	reset_state();
	fill_packet(&next_packets[0], 1000, 0, 10);
	next_count = 1;
	run_callback(cb_rx, NULL);
	mu_assert_int_eq(0x0fffffff, (int)seen.set_data_clkn);

	reset_state();
	fill_packet(&next_packets[0], 10000, 0, 10);
	next_count = 1;
	run_callback(cb_scan, NULL);
	mu_assert_int_eq(1, (int)seen.set_data_clkn);
}

/* ---- the signal level a channel's first packets report ------------------ */

MU_TEST(the_first_packet_on_a_channel_reports_its_own_signal)
{
	usb_pkt_rx rx;
	int8_t sig, noise;

	reset_state();
	/* Raw -40 is -91 dBm. The table used to start zeroed except for its
	 * very first entry, and the level is the largest of a channel's ten,
	 * so raw 0 - about -55 dBm - won for the first ten packets on every
	 * channel. The signal field of the record written to a capture file,
	 * and the signal-to-noise ratio printed beside it, were out by the
	 * same 36 dB. */
	fill_packet(&rx, 40000, 0, 0);
	determine_signal_and_noise(&rx, &sig, &noise);
	mu_assert_int_eq(-91, sig);
	mu_assert_int_eq(-55, noise);

	/* Every channel, not just the first: the old initialiser reached one
	 * entry of one channel. */
	fill_packet(&rx, 40000, 0, NUM_BREDR_CHANNELS - 1);
	determine_signal_and_noise(&rx, &sig, &noise);
	mu_assert_int_eq(-91, sig);

	/* A channel whose only packet is as weak as the table's empty value
	 * reports that, rather than being hidden by it. */
	reset_state();
	fill_packet(&rx, 40000, 0, 40);
	rx.rssi_max = INT8_MIN;
	determine_signal_and_noise(&rx, &sig, &noise);
	mu_assert_int_eq(-120, sig);
}

MU_TEST(the_level_is_the_strongest_of_a_channels_last_ten_packets)
{
	usb_pkt_rx rx;
	int8_t sig, noise;
	int i;

	reset_state();
	fill_packet(&rx, 40000, 0, 3);

	/* Ten packets getting weaker: the level stays at the strongest of
	 * them, and the eleventh drops the first out of the history. */
	for (i = 0; i < RSSI_HISTORY_LEN; i++) {
		rx.rssi_max = (int8_t)(-40 - i);
		determine_signal_and_noise(&rx, &sig, &noise);
		mu_assert_int_eq(-91, sig);
	}
	rx.rssi_max = (int8_t)(-40 - RSSI_HISTORY_LEN);
	determine_signal_and_noise(&rx, &sig, &noise);
	/* Raw -41, the strongest still in the history, is -92 dBm. */
	mu_assert_int_eq(-92, sig);

	/* One channel's history is its own. */
	fill_packet(&rx, 40000, 0, 4);
	rx.rssi_max = -30;
	determine_signal_and_noise(&rx, &sig, &noise);
	mu_assert_int_eq(-82, sig);
}

/* ---- which channels AFH detection marks used ---------------------------- */

/* A piconet whose map starts empty, with the LAP the faked access-code search
 * is given. */
static btbb_piconet* fresh_piconet(void)
{
	btbb_piconet* pn = btbb_piconet_new();

	btbb_init_piconet(pn, 0x123456);
	return pn;
}

/* How many of the 79 channels the map marks used. */
static int channels_used(btbb_piconet* pn)
{
	int channel, used = 0;

	for (channel = 0; channel < NUM_BREDR_CHANNELS; channel++)
		if (btbb_piconet_get_channel_seen(pn, (uint8_t)channel))
			used++;
	return used;
}

/* Runs one packet on `channel` through the detection callback. */
static void one_afh_packet(btbb_piconet* pn, uint8_t channel)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, channel);
	next_count = 1;
	run_callback(cb_afh_initial, pn);
}

MU_TEST(a_lone_unused_channel_between_used_ones_is_marked_used)
{
	btbb_piconet* pn = fresh_piconet();

	/* Channel 12 is known used, 11 is not, and a packet lands on 10: 11 is
	 * a single gap in a used run, so detection fills it. */
	btbb_piconet_set_channel_seen(pn, 12);
	one_afh_packet(pn, 10);

	mu_assert_int_eq(1, btbb_piconet_get_channel_seen(pn, 10));
	mu_assert_int_eq(1, btbb_piconet_get_channel_seen(pn, 11));
	mu_assert_int_eq(3, channels_used(pn));
	mu_assert_int_eq(1, seen.afh_maps_sent);

	/* And the same below the packet's channel. */
	one_afh_packet(pn, 14);
	mu_assert_int_eq(1, btbb_piconet_get_channel_seen(pn, 13));
	mu_assert_int_eq(5, channels_used(pn));

	/* A gap two channels wide is not filled: 16 and 17 are both unused, so
	 * neither is evidence of the other. */
	one_afh_packet(pn, 18);
	mu_assert_int_eq(0, btbb_piconet_get_channel_seen(pn, 16));
	mu_assert_int_eq(0, btbb_piconet_get_channel_seen(pn, 17));
	mu_assert_int_eq(6, channels_used(pn));

	btbb_piconet_unref(pn);
}

MU_TEST(the_channels_at_the_edges_of_the_band_need_a_packet_of_their_own)
{
	btbb_piconet* pn = fresh_piconet();

	/* Basic Rate has channels 0 to 78. A packet on channel 1 asks about
	 * channel -1, which has no evidence either way, so channel 0 stays
	 * unused until a packet arrives on it. libbtbb reports a channel off
	 * the end of the band as used, which used to be read as the far side
	 * of a gap and marked channel 0 used on this packet alone. */
	one_afh_packet(pn, 1);
	mu_assert_int_eq(1, btbb_piconet_get_channel_seen(pn, 1));
	mu_assert_int_eq(0, btbb_piconet_get_channel_seen(pn, 0));
	mu_assert_int_eq(1, channels_used(pn));

	/* The top of the band, where channel 79 does not exist either. */
	one_afh_packet(pn, 77);
	mu_assert_int_eq(1, btbb_piconet_get_channel_seen(pn, 77));
	mu_assert_int_eq(0, btbb_piconet_get_channel_seen(pn, 78));
	mu_assert_int_eq(2, channels_used(pn));

	/* The outermost channels themselves mark only themselves. */
	one_afh_packet(pn, 0);
	mu_assert_int_eq(3, channels_used(pn));
	one_afh_packet(pn, 78);
	mu_assert_int_eq(4, channels_used(pn));

	/* With a packet of its own on each side, the edge rule still fills a
	 * gap that has evidence: 2 is used, so a packet on 4 fills 3. */
	btbb_piconet_set_channel_seen(pn, 2);
	one_afh_packet(pn, 4);
	mu_assert_int_eq(1, btbb_piconet_get_channel_seen(pn, 3));

	btbb_piconet_unref(pn);

	/* The rule reaches inwards from the outermost channels themselves, the
	 * direction the band does have: a packet on channel 0 fills channel 1
	 * when channel 2 is used. */
	pn = fresh_piconet();
	btbb_piconet_set_channel_seen(pn, 2);
	one_afh_packet(pn, 0);
	mu_assert_int_eq(1, btbb_piconet_get_channel_seen(pn, 1));
	mu_assert_int_eq(3, channels_used(pn));
	btbb_piconet_unref(pn);

	/* And the same at the top: a packet on 78 fills 77 when 76 is used. */
	pn = fresh_piconet();
	btbb_piconet_set_channel_seen(pn, 76);
	one_afh_packet(pn, 78);
	mu_assert_int_eq(1, btbb_piconet_get_channel_seen(pn, 77));
	mu_assert_int_eq(3, channels_used(pn));
	btbb_piconet_unref(pn);
}

/* ---- the clock trim a follow sends -------------------------------------- */

MU_TEST(a_delay_goes_out_as_itself_and_an_advance_as_one_slot_less)
{
	/* Nothing to correct. */
	mu_assert_int_eq(0, clock_trim_field(0));

	/* A delay, which the firmware adds to its next tick's reload. */
	mu_assert_int_eq(1, clock_trim_field(1));
	mu_assert_int_eq(CLOCK_TRIM_MAX_DELAY,
	                 clock_trim_field(CLOCK_TRIM_MAX_DELAY));

	/* An advance: one slot less the amount, which is how the firmware
	 * reads anything above its largest delay. -150 used to be handed
	 * straight to the request's unsigned parameter as 65386, which the
	 * firmware added to its tick reload: that CLKN tick lasted 6.85 ms
	 * while the count advanced by three, leaving the clock 5.91 ms behind
	 * where 15 µs forward was wanted. */
	mu_assert_int_eq(CLK100NS_PER_SLOT - 1, clock_trim_field(-1));
	mu_assert_int_eq(6100, clock_trim_field(-150));
	mu_assert_int_eq(CLK100NS_PER_SLOT - CLOCK_TRIM_MAX_ADVANCE,
	                 clock_trim_field(-CLOCK_TRIM_MAX_ADVANCE));
}

MU_TEST(the_correction_takes_the_shorter_way_round_the_slot)
{
	uint32_t offset;

	/* On target. */
	mu_assert_int_eq(0, clock_correction_100ns(CLK_TUNE_TIME));

	/* Early: the clock is behind the piconet and has to be advanced. */
	mu_assert_int_eq(-150, clock_correction_100ns(2100));
	mu_assert_int_eq(-CLK_TUNE_TIME, clock_correction_100ns(0));

	/* Late, up to the largest delay the firmware's field holds. */
	mu_assert_int_eq(150, clock_correction_100ns(2400));
	mu_assert_int_eq(CLOCK_TRIM_MAX_DELAY,
	                 clock_correction_100ns(CLK_TUNE_TIME +
	                                        CLOCK_TRIM_MAX_DELAY));

	/* Later than that is sooner the other way: the offset runs through one
	 * slot and starts over, so these land the clock in the same place as a
	 * delay would and move it less. The largest a raw difference can be is
	 * 3999, which is an advance of 2251. */
	mu_assert_int_eq(-CLOCK_TRIM_MAX_ADVANCE,
	                 clock_correction_100ns(CLK_TUNE_TIME +
	                                        CLOCK_TRIM_MAX_DELAY + 1));
	mu_assert_int_eq(-2251, clock_correction_100ns(CLK100NS_PER_SLOT - 1));

	/* Every offset a packet can be measured at gives a correction the
	 * firmware's field carries exactly, and one the firmware reads back
	 * with the sign it was meant to have: a delay from a field at or below
	 * its largest delay, an advance from one above it. */
	for (offset = 0; offset < CLK100NS_PER_SLOT; offset++) {
		int32_t c = clock_correction_100ns((uint16_t)offset);
		uint16_t f = clock_trim_field(c);

		mu_assert(c >= -CLOCK_TRIM_MAX_ADVANCE &&
		          c <= CLOCK_TRIM_MAX_DELAY,
		          "the correction is outside what the field carries");
		mu_assert(f < CLK100NS_PER_SLOT,
		          "the trim field is outside one slot");
		if (c < 0)
			mu_assert((int32_t)f - CLK100NS_PER_SLOT == c,
			          "the firmware would read a different advance");
		else
			mu_assert((int32_t)f == c,
			          "the firmware would read a different delay");
	}
}

MU_TEST(the_drift_keeps_its_sign_and_stays_inside_what_the_board_can_act_on)
{
	/* The case that found this: 2100 against a tune time of 2250 is a
	 * correction of -150, and one second is 3200 CLKN ticks, so the board
	 * is running 15 PPM slow. The division used to be by an unsigned tick
	 * count, which converted the negative numerator first: -15 came out as
	 * 1342162, was printed as that, and was narrowed to 31442 for the
	 * request. A positive figure makes the firmware lengthen every tick,
	 * so the board was slowed by a further 320 PPM - the same direction as
	 * the error it was asked to correct - for the rest of the run. */
	mu_assert_int_eq(-15, drift_ppm(-150, CLKN_PER_SECOND));
	mu_assert_int_eq(15, drift_ppm(150, CLKN_PER_SECOND));
	mu_assert_int_eq(0, drift_ppm(0, CLKN_PER_SECOND));

	/* Smaller than the correction can express over that many ticks. */
	mu_assert_int_eq(0, drift_ppm(1, CLKN_PER_SECOND));
	mu_assert_int_eq(1, drift_ppm(10, CLKN_PER_SECOND));

	/* A trim in the same CLKN tick as the last one reports nothing rather
	 * than dividing by zero. */
	mu_assert_int_eq(0, drift_ppm(-150, 0));

	/* Faster than the firmware's correction can express: it adds or
	 * removes one 100 ns tick every 320/PPM ticks, so beyond 320 PPM its
	 * division gives nothing and it corrects on every tick, which is that
	 * 320 PPM and no more. Two trims a tenth of a second apart measure a
	 * figure past it. */
	mu_assert_int_eq(DRIFT_PPM_MAX, drift_ppm(3999, CLKN_PER_SECOND / 10));
	mu_assert_int_eq(-DRIFT_PPM_MAX, drift_ppm(-3999, CLKN_PER_SECOND / 10));
	mu_assert_int_eq(DRIFT_PPM_MAX, drift_ppm(DRIFT_PPM_MAX, 1));
}

MU_TEST(a_calibrated_follow_sends_the_trim_the_measured_offset_calls_for)
{
	btbb_piconet* pn = btbb_piconet_new();
	unsigned i;

	/* The sequence that found this, through the real cb_rx(): a packet
	 * calibrates the follow, three late ones push its counter past the
	 * threshold, and the fourth arrives early. Which form the trim took
	 * was decided by that counter alone, so this packet - a correction of
	 * -150 - went out through the branch for a delay. */
	reset_state();
	btbb_init_piconet(pn, 0x123456);

	fill_packet(&next_packets[0], clk100ns_for_clk_offset(2500, 2), 0, 10);
	for (i = 1; i < 4; i++)
		fill_packet(&next_packets[i],
		            clk100ns_for_clk_offset(2500, 100 * i), 0, 10);
	/* One second of CLKN ticks after the first packet, and 150 ticks
	 * early. */
	fill_packet(&next_packets[4], clk100ns_for_clk_offset(2100, 1602), 0, 10);
	next_count = 5;
	run_callback(cb_rx, pn);

	/* The first packet and the last one trim; the three in between only
	 * move the counter. */
	mu_assert_int_eq(2, seen.trims);
	mu_assert_int_eq(6100, (int)seen.trim_field);
	/* And the drift the second trim measured over that second. */
	mu_assert_int_eq(1, seen.drifts);
	mu_assert_int_eq(-15, (int)seen.drift_ppm_sent);

	btbb_piconet_unref(pn);
}

MU_TEST(a_replay_and_a_capture_with_no_piconet_send_no_trim)
{
	btbb_piconet* pn = btbb_piconet_new();

	/* A follow reading a file must not send requests to a board. */
	reset_state();
	btbb_init_piconet(pn, 0x123456);
	infile = stdin;
	fill_packet(&next_packets[0], clk100ns_for_clk_offset(2100, 2), 0, 10);
	next_count = 1;
	run_callback(cb_rx, pn);
	mu_assert_int_eq(0, seen.trims);
	mu_assert_int_eq(0, seen.drifts);
	mu_assert_int_eq(1, seen.set_data_calls);

	/* Nor does a capture that is not following a piconet. */
	reset_state();
	fill_packet(&next_packets[0], clk100ns_for_clk_offset(2100, 2), 0, 10);
	next_count = 1;
	run_callback(cb_rx, NULL);
	mu_assert_int_eq(0, seen.trims);

	btbb_piconet_unref(pn);
}


/* ---- the gaps a full queue leaves ------------------------------------- */

/*
 * The board sets FIFO_OVERFLOW on the first record it sends after it had no
 * room in its queue for a packet, which is the only trace a lost packet
 * leaves. Every callback counts it, so a tool can say at the end of a run that
 * the capture has gaps, and warns the first time, so a capture that runs for
 * an hour says so while it runs. Before this, a capture losing packets looked
 * exactly like a quiet band.
 */
#define GAP_WARNING "the board's packet queue filled up"

MU_TEST(a_record_that_says_the_board_dropped_a_packet_is_counted_and_warned)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	fill_packet(&next_packets[1], 40000, 0, 11);
	next_packets[0].status |= FIFO_OVERFLOW;
	next_packets[1].status |= FIFO_OVERFLOW;
	next_count = 2;

	run_callback(cb_rx, NULL);

	/* Two gaps, because two records said the queue had filled up: the
	 * count is of gaps, not of the packets lost in them. */
	mu_assert_int_eq(2, (int)last_ut.queue_gaps);
	mu_assert_int_eq(2, (int)ubertooth_queue_gaps(&last_ut));

	/* Warned once, however many gaps follow. */
	mu_assert_int_eq(1, stderr_lines_with(GAP_WARNING));
}

MU_TEST(a_capture_with_no_gap_says_nothing)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_count = 1;

	run_callback(cb_rx, NULL);

	mu_assert_int_eq(0, (int)last_ut.queue_gaps);
	mu_assert_int_eq(0, stderr_lines_with(GAP_WARNING));
}

/* A keep-alive is one byte on the wire, so its other fields hold whatever the
 * record before left in the buffer. Counting one would report the gap that
 * record already reported, on a quiet channel over and over. */
MU_TEST(a_keep_alive_does_not_count_as_a_gap)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_packets[0].pkt_type = KEEP_ALIVE;
	next_packets[0].status |= FIFO_OVERFLOW;
	next_count = 1;

	run_callback(cb_rx, NULL);

	mu_assert_int_eq(0, (int)last_ut.queue_gaps);
	mu_assert_int_eq(0, stderr_lines_with(GAP_WARNING));
}

/* A record the callback goes on to discard still carries the board's word
 * that packets were lost before it, so it is counted first. This one names a
 * channel outside the band, which cb_rx() drops. */
MU_TEST(a_discarded_record_still_counts_its_gap)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, NUM_BREDR_CHANNELS + 5);
	next_packets[0].status |= FIFO_OVERFLOW;
	next_count = 1;

	run_callback(cb_rx, NULL);

	mu_assert_int_eq(1, (int)last_ut.queue_gaps);
}

/* The other modes' callbacks count it too: a survey, AFH detection and the
 * eGo capture all read records the same board queue hands over. */
MU_TEST(every_mode_counts_the_gaps_in_its_own_capture)
{
	btbb_piconet* pn;

	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_packets[0].status |= FIFO_OVERFLOW;
	next_count = 1;
	run_callback(cb_scan, NULL);
	mu_assert_int_eq(1, (int)last_ut.queue_gaps);

	reset_state();
	pn = fresh_piconet();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_packets[0].status |= FIFO_OVERFLOW;
	next_count = 1;
	run_callback(cb_afh_initial, pn);
	mu_assert_int_eq(1, (int)last_ut.queue_gaps);

	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_packets[0].status |= FIFO_OVERFLOW;
	next_count = 1;
	run_callback(cb_afh_monitor, pn);
	mu_assert_int_eq(1, (int)last_ut.queue_gaps);

	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_packets[0].status |= FIFO_OVERFLOW;
	next_count = 1;
	run_callback(cb_afh_r, pn);
	mu_assert_int_eq(1, (int)last_ut.queue_gaps);

	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_packets[0].status |= FIFO_OVERFLOW;
	next_count = 1;
	{
		uint8_t len = 4;

		run_callback(cb_ego, &len);
	}
	mu_assert_int_eq(1, (int)last_ut.queue_gaps);

	btbb_piconet_unref(pn);
}

int main(void)
{
	MU_RUN(the_decoder_is_told_only_the_symbols_the_record_holds);
	MU_RUN(the_decoder_is_given_the_symbols_from_the_access_code_on);
	MU_RUN(the_access_code_search_reaches_the_last_position_in_the_bank);
	MU_RUN(a_timestamp_at_the_start_of_the_clock_gives_clkn_zero);
	MU_RUN(a_timestamp_inside_the_first_bank_does_not_jump_the_clock_forwards);
	MU_RUN(the_clkn_counts_from_the_access_code_not_the_bank);
	MU_RUN(the_callbacks_hand_the_decoder_that_clkn);
	MU_RUN(the_first_packet_on_a_channel_reports_its_own_signal);
	MU_RUN(the_level_is_the_strongest_of_a_channels_last_ten_packets);
	MU_RUN(a_lone_unused_channel_between_used_ones_is_marked_used);
	MU_RUN(the_channels_at_the_edges_of_the_band_need_a_packet_of_their_own);
	MU_RUN(a_delay_goes_out_as_itself_and_an_advance_as_one_slot_less);
	MU_RUN(the_correction_takes_the_shorter_way_round_the_slot);
	MU_RUN(the_drift_keeps_its_sign_and_stays_inside_what_the_board_can_act_on);
	MU_RUN(a_record_that_says_the_board_dropped_a_packet_is_counted_and_warned);
	MU_RUN(a_capture_with_no_gap_says_nothing);
	MU_RUN(a_keep_alive_does_not_count_as_a_gap);
	MU_RUN(a_discarded_record_still_counts_its_gap);
	MU_RUN(every_mode_counts_the_gaps_in_its_own_capture);
	/* These two run cb_rx() with a piconet, which keeps counters between
	 * packets: they come last so nothing above is judged by them. */
	MU_RUN(a_replay_and_a_capture_with_no_piconet_send_no_trim);
	MU_RUN(a_calibrated_follow_sends_the_trim_the_measured_offset_calls_for);
	return mu_report();
}
