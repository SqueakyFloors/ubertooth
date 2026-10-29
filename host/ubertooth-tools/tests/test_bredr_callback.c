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
 * read fields back out of it; only the entry points whose arguments are what
 * this test is about are replaced.
 */
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
	int symbol_captures;
	int channels_set;
	int pcap_writes;
	int pcapng_writes;
	uint16_t channel_set;
	int afh_maps_sent;
	int header_decodes;
} seen;

/* What the faked access-code search reports: where in the bank it found one,
 * or a negative number for nothing found. */
static int find_ac_offset;

/* What the faked plain symbol capture and the faked channel request report:
 * a negative value is how the real ones say the board refused the request. */
static int rx_syms_result;
static int set_channel_result;

/* The channel this capture asked for, which run_callback() puts on the handle
 * the way a tool does beside its own channel request. */
static uint16_t capture_channel;

/* What the faked capture writers report: negative is a failed write. */
static int pcap_write_result;
static int pcapng_write_result;

/* Whether run_callback() gives the handle capture files at all. */
static int capture_files;

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

/* Asking the board for a plain symbol capture is how the callback takes it off
 * a clock offset it is hopping on, so this is what the lost-clock case
 * counts. */
int cmd_rx_syms(struct libusb_device_handle* devh)
{
	(void)devh;
	seen.symbol_captures++;
	return rx_syms_result;
}

/* The two BR/EDR capture writers, which report a failed write. Each returns
 * what the case under way wants it to, and counts its calls, so a warning
 * said once per capture can be told from one said once per packet. */
int btbb_pcap_append_packet(btbb_pcap_handle* h, const uint64_t ns,
                            const int8_t sigdbm, const int8_t noisedbm,
                            const uint32_t reflap, const uint8_t refuap,
                            const btbb_packet* pkt)
{
	(void)h; (void)ns; (void)sigdbm; (void)noisedbm; (void)reflap;
	(void)refuap; (void)pkt;
	seen.pcap_writes++;
	return pcap_write_result;
}

int btbb_pcapng_append_packet(btbb_pcapng_handle* h, const uint64_t ns,
                              const int8_t sigdbm, const int8_t noisedbm,
                              const uint32_t reflap, const uint8_t refuap,
                              const btbb_packet* pkt)
{
	(void)h; (void)ns; (void)sigdbm; (void)noisedbm; (void)reflap;
	(void)refuap; (void)pkt;
	seen.pcapng_writes++;
	return pcapng_write_result;
}

/* A mode start makes the board forget its hop state, so whatever restarts a
 * mode has to state the channel or sweep again. This records what it was
 * given, which is what the lost-clock case checks. */
int cmd_set_channel(struct libusb_device_handle* devh, u16 channel)
{
	(void)devh;
	seen.channels_set++;
	seen.channel_set = channel;
	return set_channel_result;
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

/* Whether the faked decoder says the packet has a header to read. The real
 * one reads the symbols, which the faked btbb_packet_set_data() above does not
 * write, so the case says what it wants here. */
static int header_present_result;

int btbb_header_present(const btbb_packet* pkt)
{
	(void)pkt;
	return header_present_result;
}

/* Stands in for decoding the header, which is what the clock handed over
 * above is for. It records that it was asked. */
int btbb_decode_header(btbb_packet* pkt)
{
	(void)pkt;
	seen.header_decodes++;
	return 1;
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
 * callbacks recorded on it, and what they wrote to each stream. */
static ubertooth_t last_ut;
static char last_stderr[4096];
static char last_stdout[4096];

/* Both streams are collected rather than printed: the callbacks print a line
 * per packet and the follow prints its notes, and neither belongs in a test's
 * output. Keeping standard output lets a case pin that a warning is not on it,
 * which is what a capture written to standard output depends on. */
static int saved_out_fd, saved_err_fd;
static FILE* captured_errors;
static FILE* captured_output;

static void capture_streams(void)
{
	memset(last_stderr, 0, sizeof(last_stderr));
	memset(last_stdout, 0, sizeof(last_stdout));
	captured_errors = tmpfile();
	captured_output = tmpfile();

	fflush(NULL);
	saved_out_fd = dup(fileno(stdout));
	saved_err_fd = dup(fileno(stderr));
	if (captured_output != NULL)
		dup2(fileno(captured_output), fileno(stdout));
	if (captured_errors != NULL)
		dup2(fileno(captured_errors), fileno(stderr));
}

static void collect_one(FILE** from, char* into, size_t size)
{
	size_t read_bytes;

	if (*from == NULL)
		return;
	rewind(*from);
	read_bytes = fread(into, 1, size - 1, *from);
	into[read_bytes] = '\0';
	fclose(*from);
	*from = NULL;
}

static void collect_streams(void)
{
	fflush(NULL);
	dup2(saved_out_fd, fileno(stdout));
	dup2(saved_err_fd, fileno(stderr));
	close(saved_out_fd);
	close(saved_err_fd);

	collect_one(&captured_errors, last_stderr, sizeof(last_stderr));
	collect_one(&captured_output, last_stdout, sizeof(last_stdout));
}

static void run_callback(void (*cb)(ubertooth_t*, void*), void* args)
{
	memset(&last_ut, 0, sizeof(last_ut));
	last_ut.clk100ns_period = CLK100NS_PERIOD;
	last_ut.capture_channel = capture_channel;
	/* Never dereferenced: the writers above are fakes. They only have to
	 * be something other than NULL, which is what makes the callback
	 * write. */
	if (capture_files) {
		last_ut.h_pcap_bredr = (btbb_pcap_handle*)&last_ut;
		last_ut.h_pcapng_bredr = (btbb_pcapng_handle*)&last_ut;
	}
	next_index = 0;

	capture_streams();
	while (next_index < next_count)
		cb(&last_ut, args);
	collect_streams();
}

/* The summary a tool prints when its capture ends, read off the handle the
 * last run_callback() left behind, with both streams collected the same way. */
static void run_report(void)
{
	capture_streams();
	ubertooth_report_capture_losses(&last_ut);
	collect_streams();
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
	rx_syms_result = 0;
	set_channel_result = 0;
	capture_channel = 0;
	pcap_write_result = 0;
	pcapng_write_result = 0;
	capture_files = 0;
	header_present_result = 0;
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

/* ---- the packets a faulted transfer leaves behind ---------------------- */

/*
 * The board marks DMA_ERROR on a record when the chip's transfer controller
 * reported an error on the channel a capture receives into. The packet the
 * mark arrives on got through whole, but a packet the same fault cut short is
 * gone with nothing else to show for it. The Basic Rate loops have marked it
 * all along and the low-energy engine marks it now; neither mark reached the
 * user before this, so such a capture looked exactly like a clean one.
 */
#define TRANSFER_WARNING "transfer error while capturing"

MU_TEST(a_record_marked_by_a_transfer_error_is_counted_and_warned)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	fill_packet(&next_packets[1], 40000, 0, 11);
	next_packets[0].status |= DMA_ERROR;
	next_packets[1].status |= DMA_ERROR;
	next_count = 2;

	run_callback(cb_rx, NULL);

	mu_assert_int_eq(2, (int)last_ut.transfer_errors);
	mu_assert_int_eq(2, (int)ubertooth_transfer_errors(&last_ut));

	/* Warned once, however many follow. */
	mu_assert_int_eq(1, stderr_lines_with(TRANSFER_WARNING));

	/* On standard error, not on standard output: a capture written there
	 * would be corrupted by a line of English in the middle of it. */
	mu_assert(strstr(last_stdout, TRANSFER_WARNING) == NULL,
	          "the warning went to standard output");

	/* A faulted transfer is not a full queue: a capture must not be told it
	 * has gaps it does not have. */
	mu_assert_int_eq(0, (int)last_ut.queue_gaps);
	mu_assert_int_eq(0, stderr_lines_with(GAP_WARNING));
}

MU_TEST(a_capture_with_no_transfer_error_says_nothing)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_count = 1;

	run_callback(cb_rx, NULL);

	mu_assert_int_eq(0, (int)last_ut.transfer_errors);
	mu_assert_int_eq(0, stderr_lines_with(TRANSFER_WARNING));
}

/* A keep-alive is one byte on the wire, so its other fields hold whatever the
 * record before left in the buffer, this mark among them. */
MU_TEST(a_keep_alive_does_not_count_as_a_transfer_error)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_packets[0].pkt_type = KEEP_ALIVE;
	next_packets[0].status |= DMA_ERROR;
	next_count = 1;

	run_callback(cb_rx, NULL);

	mu_assert_int_eq(0, (int)last_ut.transfer_errors);
	mu_assert_int_eq(0, stderr_lines_with(TRANSFER_WARNING));
}

/* ---- the symbols the board wrote over ---------------------------------- */

/*
 * The board marks DMA_OVERFLOW when a block of symbols finished arriving before
 * its capture loop had read the block before it, so the new symbols went over
 * the old ones. Nothing ever looked in that block for a packet, so this loss
 * has no packet to name: it is a stretch of the air the capture does not have,
 * and until the host read the mark it looked exactly like a quiet band.
 */
#define OVERWRITE_WARNING "wrote new symbols over a block"

MU_TEST(a_record_that_says_symbols_were_written_over_is_counted_and_warned)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	fill_packet(&next_packets[1], 40000, 0, 11);
	next_packets[0].status |= DMA_OVERFLOW;
	next_packets[1].status |= DMA_OVERFLOW;
	next_count = 2;

	run_callback(cb_rx, NULL);

	mu_assert_int_eq(2, (int)last_ut.symbol_overwrites);
	mu_assert_int_eq(2, (int)ubertooth_symbol_overwrites(&last_ut));

	/* Warned once, however many follow. */
	mu_assert_int_eq(1, stderr_lines_with(OVERWRITE_WARNING));

	/* On standard error, not on standard output: a capture written there
	 * would be corrupted by a line of English in the middle of it. */
	mu_assert(strstr(last_stdout, OVERWRITE_WARNING) == NULL,
	          "the warning went to standard output");

	/* Symbols written over are neither a full queue nor a faulted transfer:
	 * a capture must not be told it has losses it does not have. */
	mu_assert_int_eq(0, (int)last_ut.queue_gaps);
	mu_assert_int_eq(0, stderr_lines_with(GAP_WARNING));
	mu_assert_int_eq(0, (int)last_ut.transfer_errors);
	mu_assert_int_eq(0, stderr_lines_with(TRANSFER_WARNING));

	/* The end of the capture counts the places, in the plural. */
	run_report();
	mu_assert_int_eq(1, stderr_lines_with("the capture has 2 places where "
	                                      "the board wrote new symbols over "
	                                      "a block it had not read"));
}

MU_TEST(a_capture_with_nothing_written_over_says_nothing)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_count = 1;

	run_callback(cb_rx, NULL);

	mu_assert_int_eq(0, (int)last_ut.symbol_overwrites);
	mu_assert_int_eq(0, stderr_lines_with(OVERWRITE_WARNING));
}

/* A keep-alive is one byte on the wire, so its other fields hold whatever the
 * record before left in the buffer, this mark among them. */
MU_TEST(a_keep_alive_does_not_count_as_symbols_written_over)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_packets[0].pkt_type = KEEP_ALIVE;
	next_packets[0].status |= DMA_OVERFLOW;
	next_count = 1;

	run_callback(cb_rx, NULL);

	mu_assert_int_eq(0, (int)last_ut.symbol_overwrites);
	mu_assert_int_eq(0, stderr_lines_with(OVERWRITE_WARNING));
}

/* What a tool prints when its capture ends, where the user sees it after the
 * packets have scrolled past: one line for each kind of loss the capture had,
 * and nothing at all for a capture that lost nothing. */
MU_TEST(the_end_of_a_capture_counts_every_kind_of_loss)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	fill_packet(&next_packets[1], 40000, 0, 11);
	fill_packet(&next_packets[2], 40000, 0, 12);
	next_packets[0].status |= FIFO_OVERFLOW;
	next_packets[1].status |= DMA_ERROR | FIFO_OVERFLOW;
	next_packets[2].status |= DMA_OVERFLOW;
	next_count = 3;

	run_callback(cb_rx, NULL);
	run_report();

	mu_assert_int_eq(1, stderr_lines_with("the capture has 2 gaps where the "
	                                      "board's queue filled up"));
	mu_assert_int_eq(1, stderr_lines_with("the capture has 1 transfer error "
	                                      "where a packet may be wrong"));
	mu_assert_int_eq(1, stderr_lines_with("the capture has 1 place where the "
	                                      "board wrote new symbols over a "
	                                      "block it had not read"));

	/* Singular and plural each read as themselves. */
	mu_assert_int_eq(0, stderr_lines_with("1 transfer errors"));
	mu_assert_int_eq(0, stderr_lines_with("2 gap where"));
	mu_assert_int_eq(0, stderr_lines_with("1 places where"));

	/* The summary is a report about the capture, not part of it: nothing of
	 * it reaches standard output, which a capture can own. */
	mu_assert_int_eq(0, (int)strlen(last_stdout));
}

MU_TEST(a_capture_that_lost_nothing_reports_nothing)
{
	reset_state();
	fill_packet(&next_packets[0], 40000, 0, 10);
	next_count = 1;

	run_callback(cb_rx, NULL);
	run_report();

	mu_assert_int_eq(0, (int)strlen(last_stderr));
	mu_assert_int_eq(0, (int)strlen(last_stdout));
}

MU_TEST(a_header_is_read_at_the_piconets_clock_until_the_whole_clock_is_known)
{
	btbb_piconet* pn = btbb_piconet_new();
	/* How far the board's clock runs behind the piconet's, in slots. The
	 * UAP search works this out, and one slot is two clock ticks. */
	const int slots = 5;
	uint32_t clk100ns = clk100ns_for_clk_offset(2100, 2);
	uint32_t clkn = clkn_at_access_code(0, clk100ns, 0);

	/* A replay, so no clock trim stands between the packet and the
	 * decoder, following a piconet whose UAP and CLK1-6 are known but
	 * whose whole clock is not: the window where nothing else reads the
	 * packet's header. */
	reset_state();
	btbb_init_piconet(pn, 0x123456);
	btbb_piconet_set_uap(pn, 0x5b);
	btbb_piconet_set_flag(pn, BTBB_CLK6_VALID, 1);
	btbb_piconet_set_clk_offset(pn, slots);
	header_present_result = 1;
	infile = stdin;
	fill_packet(&next_packets[0], clk100ns, 0, 10);
	next_count = 1;
	run_callback(cb_rx, pn);

	/* The header is read, and read at the piconet's clock rather than the
	 * board's: the board's reading is what the decoder was given first.
	 * Without the offset the header unwhitens at the wrong clock, and the
	 * record carries bits that fail their own check. */
	mu_assert_int_eq(1, seen.header_decodes);
	mu_assert_int_eq(2, seen.set_data_calls);
	mu_assert_int_eq((int)(clkn + 2 * slots), (int)seen.set_data_clkn);

	/* A packet with no header to read is left alone, so an ID packet
	 * records none. */
	memset(&seen, 0, sizeof(seen));
	header_present_result = 0;
	next_index = 0;
	run_callback(cb_rx, pn);
	mu_assert_int_eq(0, seen.header_decodes);
	mu_assert_int_eq(1, seen.set_data_calls);

	/* A piconet whose UAP was given on the command line but whose clock
	 * bits nothing has worked out yet: there is no offset to apply, so the
	 * header would be read at the board's clock and recorded as the
	 * packet's. It is left alone until a packet settles those bits. */
	memset(&seen, 0, sizeof(seen));
	header_present_result = 1;
	btbb_piconet_set_flag(pn, BTBB_CLK6_VALID, 0);
	next_index = 0;
	run_callback(cb_rx, pn);
	mu_assert_int_eq(0, seen.header_decodes);
	mu_assert_int_eq(1, seen.set_data_calls);
	btbb_piconet_set_flag(pn, BTBB_CLK6_VALID, 1);

	/* Once the whole clock is known the decoder reads the header itself,
	 * and the board has been told to close the gap, so the offset must not
	 * be applied a second time: this packet is left alone too. */
	memset(&seen, 0, sizeof(seen));
	header_present_result = 1;
	btbb_piconet_set_flag(pn, BTBB_CLK27_VALID, 1);
	next_index = 0;
	run_callback(cb_rx, pn);
	mu_assert_int_eq(0, seen.header_decodes);
	mu_assert_int_eq(1, seen.set_data_calls);

	btbb_piconet_unref(pn);
}

/* ---- the clock the library hands over, and takes back ------------------- */

MU_TEST(a_capture_that_cannot_be_written_says_so_once)
{
	unsigned i;

	/* Three packets, every write failing. A capture nobody could write to
	 * used to look exactly like one that was being written; now each
	 * writer says so once, because the condition holds for every packet
	 * after the first and a line per packet would bury the capture. */
	reset_state();
	capture_files = 1;
	pcap_write_result = -1;
	pcapng_write_result = -1;
	for (i = 0; i < 3; i++)
		fill_packet(&next_packets[i], 40000 + 1000 * i, 0, 10);
	next_count = 3;
	run_callback(cb_rx, NULL);

	mu_assert_int_eq(3, seen.pcap_writes);
	mu_assert_int_eq(3, seen.pcapng_writes);
	mu_assert_int_eq(1, stderr_lines_with("cannot write to the PCAP capture"));
	mu_assert_int_eq(1, stderr_lines_with("cannot write to the PCAPNG capture"));

	/* A capture that can be written says nothing at all. Both flags are
	 * already set by the run above, which is what makes this a check that
	 * the writers are being called rather than a repeat of it. */
	reset_state();
	capture_files = 1;
	for (i = 0; i < 3; i++)
		fill_packet(&next_packets[i], 40000 + 1000 * i, 0, 10);
	next_count = 3;
	run_callback(cb_rx, NULL);

	mu_assert_int_eq(3, seen.pcap_writes);
	mu_assert_int_eq(3, seen.pcapng_writes);
	mu_assert_int_eq(0, stderr_lines_with("warning: "));
}

MU_TEST(a_recovered_clock_puts_the_board_on_its_offset)
{
	btbb_piconet* pn = btbb_piconet_new();

	reset_state();
	btbb_init_piconet(pn, 0x123456);
	/* An offset right on the tune time: the trim block has nothing to
	 * correct, so the packet reaches the decoder, which is where the
	 * library reports what it made of the piconet. */
	fill_packet(&next_packets[0], clk100ns_for_clk_offset(CLK_TUNE_TIME, 2),
	            0, 10);
	next_count = 1;
	/* How the library says it has just recovered the clock. */
	process_packet_result = -1;
	run_callback(cb_rx, pn);

	mu_assert_int_eq(1, seen.hopping_restarts);
	mu_assert_int_eq(0, seen.symbol_captures);
	mu_assert_int_eq(0, stderr_lines_with("warning: "));

	btbb_piconet_unref(pn);
}

MU_TEST(a_lost_clock_warns_once_and_takes_the_board_off_the_offset)
{
	btbb_piconet* pn = btbb_piconet_new();
	unsigned i;

	/* The capture is following a clock, because the case above left it
	 * hopping on one. Now the library gives that clock up: without this
	 * the board went on hopping on an offset that was no longer the
	 * piconet's, decoding nothing, and said nothing about it. */
	reset_state();
	btbb_init_piconet(pn, 0x123456);
	/* Not the sweep, so the value the board is given is this capture's own
	 * and not a constant the callback could have made up. */
	capture_channel = 2450;
	for (i = 0; i < 3; i++)
		fill_packet(&next_packets[i],
		            clk100ns_for_clk_offset(CLK_TUNE_TIME, 2 + 100 * i),
		            0, 10);
	next_count = 3;
	process_packet_result = BTBB_PROCESS_LOCK_LOST;
	run_callback(cb_rx, pn);

	/* One plain symbol capture, which is what takes the board off the
	 * offset and leaves discovery running, and one warning however many
	 * packets the report arrives with. */
	mu_assert_int_eq(1, seen.symbol_captures);
	mu_assert_int_eq(0, seen.hopping_restarts);
	mu_assert_int_eq(1, stderr_lines_with("warning: "));

	/* And the channel this capture asked for is stated again, because that
	 * mode start made the board forget it: without this the board sits on
	 * the channel the last piconet hop chose and discovery looks for a
	 * piconet on one channel in 79. */
	mu_assert_int_eq(1, seen.channels_set);
	mu_assert_int_eq(2450, (int)seen.channel_set);

	/* And nothing more to undo until the library hands over another
	 * clock. */
	reset_state();
	fill_packet(&next_packets[0], clk100ns_for_clk_offset(CLK_TUNE_TIME, 2),
	            0, 10);
	next_count = 1;
	process_packet_result = BTBB_PROCESS_LOCK_LOST;
	run_callback(cb_rx, pn);

	mu_assert_int_eq(0, seen.symbol_captures);
	mu_assert_int_eq(0, stderr_lines_with("warning: "));

	btbb_piconet_unref(pn);
}

MU_TEST(a_board_that_refused_the_request_is_still_hopping_on_the_lost_offset)
{
	btbb_piconet* pn = btbb_piconet_new();

	/* Put the capture back on a clock, since the case above took it off. */
	reset_state();
	btbb_init_piconet(pn, 0x123456);
	fill_packet(&next_packets[0], clk100ns_for_clk_offset(CLK_TUNE_TIME, 2),
	            0, 10);
	next_count = 1;
	process_packet_result = -1;
	run_callback(cb_rx, pn);
	mu_assert_int_eq(1, seen.hopping_restarts);

	/* The clock is lost and the board refuses to leave it. Nothing here
	 * can fix that, but the next report must try again rather than be
	 * taken for a repeat of one already dealt with: a board left on a
	 * clock that is gone decodes nothing for the rest of the run, which is
	 * the whole point of acting on the report. */
	reset_state();
	process_packet_result = BTBB_PROCESS_LOCK_LOST;
	rx_syms_result = -7;
	fill_packet(&next_packets[0], clk100ns_for_clk_offset(CLK_TUNE_TIME, 2),
	            0, 10);
	next_count = 1;
	run_callback(cb_rx, pn);
	mu_assert_int_eq(1, seen.symbol_captures);
	/* It says so, as well as whatever the request itself reported, and it
	 * does not go on to state a channel on a board that is still on the
	 * clock. */
	mu_assert_int_eq(1, stderr_lines_with("still following"));
	mu_assert_int_eq(0, seen.channels_set);

	/* So the next one asks again. This board takes the symbol capture and
	 * refuses the channel, which is a different position: the capture is
	 * off the clock, because that request is what takes it off, and left
	 * receiving on one channel. Saying it was still following the clock
	 * would send the reader after the wrong thing. */
	reset_state();
	process_packet_result = BTBB_PROCESS_LOCK_LOST;
	capture_channel = 2450;
	set_channel_result = -7;
	fill_packet(&next_packets[0], clk100ns_for_clk_offset(CLK_TUNE_TIME, 2),
	            0, 10);
	next_count = 1;
	run_callback(cb_rx, pn);
	mu_assert_int_eq(1, seen.symbol_captures);
	mu_assert_int_eq(1, seen.channels_set);
	mu_assert_int_eq(0, stderr_lines_with("still following"));
	mu_assert_int_eq(1, stderr_lines_with("receiving on one channel"));

	/* And that clock is undone, whatever the channel did, so the next
	 * report is a repeat and says nothing. */
	reset_state();
	process_packet_result = BTBB_PROCESS_LOCK_LOST;
	fill_packet(&next_packets[0], clk100ns_for_clk_offset(CLK_TUNE_TIME, 2),
	            0, 10);
	next_count = 1;
	run_callback(cb_rx, pn);
	mu_assert_int_eq(0, seen.symbol_captures);
	mu_assert_int_eq(0, stderr_lines_with("warning: "));

	/* And that one was taken, so there is nothing left to undo. */
	reset_state();
	process_packet_result = BTBB_PROCESS_LOCK_LOST;
	fill_packet(&next_packets[0], clk100ns_for_clk_offset(CLK_TUNE_TIME, 2),
	            0, 10);
	next_count = 1;
	run_callback(cb_rx, pn);
	mu_assert_int_eq(0, seen.symbol_captures);
	mu_assert_int_eq(0, stderr_lines_with("warning: "));

	btbb_piconet_unref(pn);
}

MU_TEST(a_replay_warns_but_sends_no_request_when_the_clock_is_lost)
{
	btbb_piconet* pn = btbb_piconet_new();

	/* A follow reading a capture file. The library recovers a clock and
	 * then gives it up, exactly as it would on a live capture; there is no
	 * board here to put on an offset or take off one. */
	reset_state();
	btbb_init_piconet(pn, 0x123456);
	infile = stdin;
	fill_packet(&next_packets[0], clk100ns_for_clk_offset(CLK_TUNE_TIME, 2),
	            0, 10);
	next_count = 1;
	process_packet_result = -1;
	run_callback(cb_rx, pn);
	mu_assert_int_eq(0, seen.hopping_restarts);

	infile = stdin;
	process_packet_result = BTBB_PROCESS_LOCK_LOST;
	next_index = 0;
	memset(&seen, 0, sizeof(seen));
	run_callback(cb_rx, pn);

	mu_assert_int_eq(0, seen.symbol_captures);
	mu_assert_int_eq(0, seen.hopping_restarts);
	mu_assert_int_eq(0, seen.channels_set);
	/* It still says so: the reader of a capture file is left with the same
	 * silence as the user of a board, and the warning is what the report
	 * is for in both. */
	mu_assert_int_eq(1, stderr_lines_with("was not this piconet's"));

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
	MU_RUN(a_record_marked_by_a_transfer_error_is_counted_and_warned);
	MU_RUN(a_capture_with_no_transfer_error_says_nothing);
	MU_RUN(a_keep_alive_does_not_count_as_a_transfer_error);
	MU_RUN(a_record_that_says_symbols_were_written_over_is_counted_and_warned);
	MU_RUN(a_capture_with_nothing_written_over_says_nothing);
	MU_RUN(a_keep_alive_does_not_count_as_symbols_written_over);
	MU_RUN(the_end_of_a_capture_counts_every_kind_of_loss);
	MU_RUN(a_capture_that_lost_nothing_reports_nothing);
	/* These run cb_rx() with a piconet, which keeps state between packets
	 * -- the clock trim's counters, and whether the library has handed
	 * this capture a clock -- so they come last, and in this order,
	 * because each leaves the next the state it expects. */
	MU_RUN(a_replay_and_a_capture_with_no_piconet_send_no_trim);
	MU_RUN(a_header_is_read_at_the_piconets_clock_until_the_whole_clock_is_known);
	/* After the case above, because these reach the decoder through the
	 * same trim counters it leaves behind. */
	MU_RUN(a_capture_that_cannot_be_written_says_so_once);
	MU_RUN(a_recovered_clock_puts_the_board_on_its_offset);
	MU_RUN(a_lost_clock_warns_once_and_takes_the_board_off_the_offset);
	MU_RUN(a_board_that_refused_the_request_is_still_hopping_on_the_lost_offset);
	MU_RUN(a_replay_warns_but_sends_no_request_when_the_clock_is_lost);
	MU_RUN(a_calibrated_follow_sends_the_trim_the_measured_offset_calls_for);
	return mu_report();
}
