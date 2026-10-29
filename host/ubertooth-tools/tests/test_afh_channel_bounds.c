/*
 * The channel a record claims, where the AFH callbacks use it as an index.
 *
 * usb_pkt_rx.channel is a one-byte offset from 2402 MHz, and three of the AFH
 * callbacks took it straight into a 79-entry array of per-channel counters, or
 * used it to name a bit of the AFH map. The capture callbacks beside them check
 * it first; these did not, so a record claiming channel 200 wrote 121 entries
 * past the end of a static array. The firmware does not produce such a record
 * today -- it reports CHANNEL_NONE (255) for a frequency outside the band, and
 * no shipped tool tunes outside it at all -- so nothing reaches it from a board.
 *
 * The real cb_afh_initial(), cb_afh_monitor() and cb_afh_r() run here, by
 * compiling ubertooth_callback.c into this file. The ring buffer, the board's
 * requests and libbtbb's access-code search are faked: the search is made to
 * succeed, which is what gets a record past the first thing that would have
 * dropped it, so what is left to stop an out-of-range channel is the bounds
 * check.
 *
 * This file is part of Project Ubertooth and is released under the terms of the
 * GPL. Refer to COPYING for more information.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
#include "../../libubertooth/src/ubertooth_callback.c"

/* ---- the state ubertooth.c owns ----------------------------------------- */

FILE* dumpfile = NULL;
FILE* infile = NULL;
int max_ac_errors = 2;
unsigned int packet_counter_max = 20;
uint32_t systime;

/* ---- fakes -------------------------------------------------------------- */

/* The report about a refused request asks whether the board has left the bus
 * before it blames the board's firmware. Nothing here unplugs a board, so it
 * is always still there. */
int ubertooth_device_gone(const void* board)
{
	(void)board;
	return 0;
}

static usb_pkt_rx next_packet;
static int hops;                 /* cmd_hop() calls */
static int afh_maps_sent;
static int find_ac_succeeds;
static int packets_seen;         /* btbb_piconet_set_channel_seen() calls */
static int last_channel_seen;

usb_pkt_rx fifo_pop(fifo_t* fifo) { (void)fifo; return next_packet; }
uint64_t fifo_popped_arrival_ns(const fifo_t* fifo) { (void)fifo; return 0; }

void ubertooth_unpack_symbols(const uint8_t* buf, char* unpacked)
{
	(void)buf;
	memset(unpacked, 0, BANK_LEN);
}

int le_usb_reassemble(le_usb_reassembly* state, const usb_pkt_rx* frame,
                      uint64_t arrival_ns, const uint8_t** data,
                      uint16_t* length, const usb_pkt_rx** metadata,
                      uint64_t* metadata_arrival_ns)
{
	(void)state; (void)frame; (void)arrival_ns; (void)data;
	(void)length; (void)metadata; (void)metadata_arrival_ns;
	return 0;
}

int cmd_hop(struct libusb_device_handle* devh) { (void)devh; hops++; return 0; }
int cmd_set_afh_map(struct libusb_device_handle* devh, uint8_t* map)
{ (void)devh; (void)map; afh_maps_sent++; return 0; }
void cmd_trim_clock(struct libusb_device_handle* devh, uint16_t offset)
{ (void)devh; (void)offset; }
void cmd_fix_clock_drift(struct libusb_device_handle* devh, int16_t ppm)
{ (void)devh; (void)ppm; }
int cmd_start_hopping(struct libusb_device_handle* devh, int clkn_offset,
                      int clk100ns_offset)
{ (void)devh; (void)clkn_offset; (void)clk100ns_offset; return 0; }
int cmd_rx_syms(struct libusb_device_handle* devh) { (void)devh; return 0; }

/*
 * libbtbb's access-code search. The real one is linked in, and this
 * definition comes first, so this is the one the callbacks call. A success
 * hands back a real but empty packet, because the callbacks go on to use it.
 */
int btbb_find_ac(char* stream, int search_length, uint32_t lap,
                 int max_ac_errors_arg, btbb_packet** pkt)
{
	(void)stream; (void)search_length; (void)lap; (void)max_ac_errors_arg;
	if (!find_ac_succeeds)
		return -1;
	*pkt = btbb_packet_new();
	return 0;
}

/* The piconet calls the two callbacks make of a channel. Faked so an index
 * the check should have refused is counted here rather than disappearing into
 * the library. */
uint8_t btbb_piconet_set_channel_seen(btbb_piconet* pn, uint8_t channel)
{
	(void)pn;
	packets_seen++;
	last_channel_seen = channel;
	return 0;
}

/* ---- helpers ------------------------------------------------------------ */

static void a_record_on_channel(uint8_t channel)
{
	memset(&next_packet, 0, sizeof(next_packet));
	next_packet.pkt_type = BR_PACKET;
	next_packet.channel = channel;
	next_packet.clk100ns = htole32(1000);
	next_packet.rssi_min = -80;
	next_packet.rssi_max = -60;
}

/* Runs one record through a callback with its output out of the way: both
 * callbacks report a channel coming into use on standard error. */
static void run(void (*callback)(ubertooth_t*, void*), btbb_piconet* pn)
{
	ubertooth_t ut;
	int saved_out, saved_err, devnull;

	memset(&ut, 0, sizeof(ut));
	ut.fifo = (fifo_t*)malloc(1);
	ut.clk100ns_period = CLK100NS_PERIOD;

	hops = 0;
	afh_maps_sent = 0;
	packets_seen = 0;
	last_channel_seen = -1;

	fflush(stdout);
	fflush(stderr);
	saved_out = dup(fileno(stdout));
	saved_err = dup(fileno(stderr));
	devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, fileno(stdout));
		dup2(devnull, fileno(stderr));
		close(devnull);
	}

	callback(&ut, pn);

	fflush(stdout);
	fflush(stderr);
	dup2(saved_out, fileno(stdout));
	dup2(saved_err, fileno(stderr));
	close(saved_out);
	close(saved_err);

	free(ut.fifo);
}

/* ---- cases -------------------------------------------------------------- */

/* The last channel there is, so the check cannot be an off-by-one that
 * refuses a real channel. */
MU_TEST(the_highest_real_channel_is_counted)
{
	btbb_piconet* pn = btbb_piconet_new();

	find_ac_succeeds = 1;
	a_record_on_channel(NUM_BREDR_CHANNELS - 1);

	run(cb_afh_monitor, pn);
	mu_assert_int_eq(1, packets_seen);
	mu_assert_int_eq(NUM_BREDR_CHANNELS - 1, last_channel_seen);
	mu_assert_int_eq(1, hops);

	run(cb_afh_r, pn);
	mu_assert_int_eq(1, packets_seen);
	mu_assert_int_eq(NUM_BREDR_CHANNELS - 1, last_channel_seen);
	mu_assert_int_eq(1, hops);

	/* The detection callback counts it too. It asks about the neighbouring
	 * channels as well, which is why last_channel_seen is not checked
	 * here, only that the record was taken. */
	run(cb_afh_initial, pn);
	mu_assert(packets_seen > 0, "the detection callback dropped channel 78");
	mu_assert_int_eq(1, hops);

	btbb_piconet_unref(pn);
}

MU_TEST(the_first_channel_is_counted)
{
	btbb_piconet* pn = btbb_piconet_new();

	find_ac_succeeds = 1;
	a_record_on_channel(0);

	run(cb_afh_monitor, pn);
	mu_assert_int_eq(1, packets_seen);
	mu_assert_int_eq(0, last_channel_seen);

	run(cb_afh_r, pn);
	mu_assert_int_eq(1, packets_seen);
	mu_assert_int_eq(0, last_channel_seen);

	run(cb_afh_initial, pn);
	mu_assert(packets_seen > 0, "the detection callback dropped channel 0");

	btbb_piconet_unref(pn);
}

/*
 * A channel one past the last, and the value the firmware uses for a
 * frequency outside the band. Neither is counted and neither moves the board
 * on, which is what the three sibling callbacks do with the same record.
 */
MU_TEST(a_channel_past_the_band_is_dropped)
{
	static const int channels[] = { NUM_BREDR_CHANNELS, 200, 255 };
	btbb_piconet* pn = btbb_piconet_new();
	unsigned i;

	find_ac_succeeds = 1;
	for (i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
		char message[96];

		snprintf(message, sizeof(message),
		         "channel %d was counted as a real one", channels[i]);

		a_record_on_channel((uint8_t)channels[i]);

		run(cb_afh_monitor, pn);
		mu_assert(packets_seen == 0, message);
		mu_assert(hops == 0, message);
		mu_assert(afh_maps_sent == 0, message);

		run(cb_afh_r, pn);
		mu_assert(packets_seen == 0, message);
		mu_assert(hops == 0, message);

		run(cb_afh_initial, pn);
		mu_assert(packets_seen == 0, message);
		mu_assert(hops == 0, message);
		mu_assert(afh_maps_sent == 0, message);
	}

	btbb_piconet_unref(pn);
}

/* And a record the search finds nothing in is still dropped, so the check
 * above has not replaced what already stopped those. */
MU_TEST(a_record_with_no_access_code_is_dropped)
{
	btbb_piconet* pn = btbb_piconet_new();

	find_ac_succeeds = 0;
	a_record_on_channel(10);

	run(cb_afh_monitor, pn);
	mu_assert_int_eq(0, packets_seen);
	mu_assert_int_eq(0, hops);

	run(cb_afh_initial, pn);
	mu_assert_int_eq(0, packets_seen);
	mu_assert_int_eq(0, hops);

	btbb_piconet_unref(pn);
}

int main(void)
{
	printf("AFH callback channel bound tests\n");

	MU_RUN(the_first_channel_is_counted);
	MU_RUN(the_highest_real_channel_is_counted);
	MU_RUN(a_channel_past_the_band_is_dropped);
	MU_RUN(a_record_with_no_access_code_is_dropped);

	return mu_report();
}
