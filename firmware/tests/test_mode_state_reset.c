/*
 * What a board carries over from one run to the next.
 *
 * Every mode start resets the board's mode state, its hop mode and its AFH
 * channel map. The channel is the exception: one set on its own, with no mode
 * running, is kept, and the next mode starts on it. Two places have to hold
 * that rule, because there are two ways a mode starts:
 *
 *   - the mode dispatch in main(), which leaves the running mode first and so
 *     runs the idle reset; and
 *   - a request for one of the three modes bt_stream_rx() streams, arriving
 *     while another of them runs. That one is absorbed by the running loop --
 *     deliberately, it is how a hop mode is switched under a running stream --
 *     so it never reaches the dispatch, and resets through the request handler
 *     instead.
 *
 * Both are hardware code that cannot run here, so this models them: the board
 * state, the idle reset, the dispatch, the absorption rule and the vendor
 * requests that ask for a mode. The AFH map and the hopping are not modelled --
 * bluetooth.c is compiled in, so bluetooth_set_afh_map() and next_hop() are the
 * firmware's own, and "the capture hops across the whole channel set" is
 * counted from the channels next_hop() actually returns.
 *
 * test_mode_dispatch.c pins the firmware's shape against this model: that the
 * idle reset clears the map, that the three absorbable requests reset before
 * their own setup, and that setting a channel resets nothing.
 */
#include <string.h>

#include "minunit.h"
#include "bluetooth.h"

/* Non-static state in bluetooth.c. */
extern u8 bank[NUM_BREDR_CHANNELS];
extern u8 afh_bank[NUM_BREDR_CHANNELS];

/* A piconet to hop with: any address will do, the sequence is its own. */
#define TEST_ADDRESS UINT64_C(0x12345678)

/* The channel a fixed-channel capture is left on by the idle reset. */
#define IDLE_CHANNEL 2441

/*
 * The modes this model needs. Only their identity matters -- the firmware
 * compares modes, it never orders them -- so these are names for the test,
 * not the numbers in firmware/common/ubertooth.h, which pulls in registers
 * that do not exist on the host.
 */
enum test_mode {
	MODE_IDLE,
	MODE_RX_SYMBOLS,
	MODE_BT_FOLLOW,
	MODE_AFH,
	MODE_SPECAN,
};

/* The board state the firmware keeps in bluetooth_rxtx.c. */
static u8 mode;
static u8 requested_mode;
static u8 hop_mode;
static u16 channel;
/* Whether bt_stream_rx()'s loop is running, which is what absorbs a request. */
static int streaming;

/* ------------------------------------------------------------------------
 * The firmware, modelled
 * ------------------------------------------------------------------------ */

/* clear_afh_map() */
static void clear_afh_map(void)
{
	memset(afh_map, 0, sizeof(afh_map));
	afh_enabled = 0;
	used_channels = 0;
}

/* streams_symbols(): the modes bt_stream_rx()'s loop keeps running for. */
static int streams_symbols(u8 m)
{
	return m == MODE_RX_SYMBOLS || m == MODE_BT_FOLLOW || m == MODE_AFH;
}

/* cc2400_idle(), as far as the carry-over rule is concerned. */
static void cc2400_idle(void)
{
	hop_mode = HOP_NONE;
	channel = IDLE_CHANNEL;
	clear_afh_map();
}

/* reset_absorbed_mode_state() */
static void reset_absorbed_mode_state(u8 requested)
{
	if (requested == requested_mode || !streams_symbols(mode) ||
	    !streams_symbols(requested))
		return;

	hop_mode = HOP_NONE;
	clear_afh_map();
}

/* One turn of main()'s loop: the dispatch, for a request that reaches it. */
static void dispatch(void)
{
	u8 next_mode = requested_mode;
	u8 prev_mode = mode;

	if (next_mode == prev_mode)
		return;
	if (prev_mode != MODE_IDLE)
		cc2400_idle();
	mode = next_mode;

	switch (next_mode) {
	case MODE_AFH:
		hop_mode = HOP_AFH;
		afh_enabled = 1;
		break;
	case MODE_BT_FOLLOW:
		hop_mode = HOP_BLUETOOTH;
		break;
	default:
		break;
	}
	streaming = streams_symbols(next_mode);
}

/*
 * Where the board is after a request: the running stream absorbs a request for
 * one of its three modes, and anything else goes through the dispatch.
 */
static void board_poll(void)
{
	if (streaming && streams_symbols(mode) && streams_symbols(requested_mode))
		return;
	dispatch();
}

/* UBERTOOTH_RX_SYMBOLS */
static void request_rx_symbols(void)
{
	reset_absorbed_mode_state(MODE_RX_SYMBOLS);
	requested_mode = MODE_RX_SYMBOLS;
	board_poll();
}

/* UBERTOOTH_START_HOPPING */
static void request_start_hopping(void)
{
	reset_absorbed_mode_state(MODE_BT_FOLLOW);
	hop_mode = HOP_BLUETOOTH;
	requested_mode = MODE_BT_FOLLOW;
	board_poll();
}

/* UBERTOOTH_AFH */
static void request_afh(void)
{
	reset_absorbed_mode_state(MODE_AFH);
	hop_mode = HOP_AFH;
	requested_mode = MODE_AFH;
	clear_afh_map();
	afh_enabled = 1;
	board_poll();
}

/* UBERTOOTH_SPECAN: a mode that is not one of the three. */
static void request_specan(void)
{
	requested_mode = MODE_SPECAN;
	board_poll();
}

/* UBERTOOTH_SET_CHANNEL, fixed-channel form */
static void request_set_channel(u16 mhz)
{
	hop_mode = HOP_NONE;
	channel = mhz;
	board_poll();
}

/* UBERTOOTH_SET_CHANNEL, sweep form: a channel above the band asks for the
 * band sweep instead, which is how ubertooth-rx and ubertooth-scan ask for
 * one. */
static void request_set_channel_sweep(void)
{
	hop_mode = HOP_SWEEP;
	channel = 2402;
	board_poll();
}

/* UBERTOOTH_SET_AFHMAP: the firmware's own, map and hopping table alike. */
static void request_set_afh_map(const u8 *map)
{
	bluetooth_set_afh_map(map);
	board_poll();
}

/* UBERTOOTH_STOP */
static void request_stop(void)
{
	requested_mode = MODE_IDLE;
	board_poll();
}

/* ------------------------------------------------------------------------
 * What the board hops on
 * ------------------------------------------------------------------------ */

/*
 * A map of 20 channels, the fewest AFH runs on: channels 0-19, so 2402-2421
 * MHz. A piconet avoiding the rest of the band would look like this.
 */
static const u8 AFH_MAP_20[10] = {
	0xff, 0xff, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* How many different channels a run visits, by the rule hop() follows for the
 * hop mode in force. HOP_BLUETOOTH asks bluetooth.c's next_hop(); the AFH
 * detection and sweep rules are hop()'s own. 2048 hops is many times round
 * each of them. */
#define HOPS 2048

static int channels_visited(void)
{
	u8 seen[NUM_BREDR_CHANNELS];
	u16 at = channel;
	int i, count = 0;

	memset(seen, 0, sizeof(seen));
	for (i = 0; i < HOPS; i++) {
		if (hop_mode == HOP_SWEEP) {
			do {
				at += 32;
				if (at > 2480)
					at -= 79;
			} while (used_channels != 0 && afh_enabled &&
			         !(afh_map[(at - 2402) / 8] &
			           0x1 << ((at - 2402) % 8)));
		} else if (hop_mode == HOP_AFH) {
			do {
				at += 32;
				if (at > 2480)
					at -= 79;
			} while (used_channels != 79 &&
			         (afh_map[(at - 2402) / 8] &
			          0x1 << ((at - 2402) % 8)));
		} else if (hop_mode == HOP_BLUETOOTH) {
			at = next_hop((u32)i * 2);
		}
		/* HOP_NONE stays where it is. */
		if (at < 2402 || at > 2480)
			return -1;
		if (!seen[at - 2402]) {
			seen[at - 2402] = 1;
			count++;
		}
	}
	return count;
}

/* The board as it comes up: boot runs the idle reset. */
static void board_reset(void)
{
	memset(&target, 0, sizeof(target));
	memset(bank, 0, sizeof(bank));
	memset(afh_bank, 0, sizeof(afh_bank));
	mode = MODE_IDLE;
	requested_mode = MODE_IDLE;
	streaming = 0;
	cc2400_idle();

	target.address = TEST_ADDRESS;
	precalc();
}

/* A run that found an AFH map and left the board following the piconet on it. */
static void a_run_with_a_map(void)
{
	board_reset();
	request_start_hopping();
	request_set_afh_map(AFH_MAP_20);
	mu_assert_int_eq(1, afh_enabled);
	mu_assert_int_eq(20, used_channels);
	mu_assert_int_eq(20, channels_visited());
}

/* ------------------------------------------------------------------------
 * The tests
 * ------------------------------------------------------------------------ */

/* The map one run found is gone by the time the next mode starts. */
MU_TEST(a_mode_start_forgets_the_map_the_last_run_found)
{
	a_run_with_a_map();

	/* The run ends normally: a stop, then a plain symbol capture. */
	request_stop();
	request_rx_symbols();

	mu_assert_int_eq(MODE_RX_SYMBOLS, mode);
	mu_assert_int_eq(HOP_NONE, hop_mode);
	mu_assert_int_eq(0, afh_enabled);
	mu_assert_int_eq(0, used_channels);
	mu_assert_int_eq(1, channels_visited());

	/* ...and a follow started after it hops the whole channel set, not the
	 * 20 channels of the run before. */
	request_stop();
	request_start_hopping();
	mu_assert_int_eq(HOP_BLUETOOTH, hop_mode);
	mu_assert_int_eq(NUM_BREDR_CHANNELS, channels_visited());
}

/*
 * The case from the field: a tool killed without a stop leaves the board
 * streaming, so the next tool's request is absorbed by the running loop. It
 * used to inherit that loop's hop mode and map -- an ubertooth-rx after a
 * killed ubertooth-afh captured while hopping adaptively on the old map.
 */
MU_TEST(a_capture_after_a_killed_tool_inherits_nothing)
{
	board_reset();
	request_afh();
	request_set_afh_map(AFH_MAP_20);
	mu_assert_int_eq(MODE_AFH, mode);
	mu_assert(streaming, "AFH detection streams");

	/* No stop: the next run asks for a plain symbol capture. */
	request_rx_symbols();

	mu_assert(streaming, "the loop keeps streaming, as it is meant to");
	mu_assert_int_eq(HOP_NONE, hop_mode);
	mu_assert_int_eq(0, afh_enabled);
	mu_assert_int_eq(0, used_channels);
	mu_assert_int_eq(1, channels_visited());
}

/* An absorbed request still gets what it asked for: following hops with the
 * piconet, across the whole channel set, not on the map it replaced. */
MU_TEST(an_absorbed_follow_hops_with_the_piconet)
{
	board_reset();
	request_afh();
	request_set_afh_map(AFH_MAP_20);

	request_start_hopping();

	mu_assert_int_eq(HOP_BLUETOOTH, hop_mode);
	mu_assert_int_eq(0, afh_enabled);
	mu_assert_int_eq(NUM_BREDR_CHANNELS, channels_visited());
}

/* And absorbed AFH detection starts from an empty map, as its request says. */
MU_TEST(an_absorbed_detection_starts_from_an_empty_map)
{
	u8 empty[10] = { 0 };

	board_reset();
	request_start_hopping();
	request_set_afh_map(AFH_MAP_20);

	request_afh();

	mu_assert_int_eq(HOP_AFH, hop_mode);
	mu_assert_int_eq(1, afh_enabled);
	mu_assert_int_eq(0, used_channels);
	mu_assert(memcmp(empty, afh_map, sizeof(empty)) == 0,
	          "detection begins with no channel marked used");
	/* With nothing marked used, detection visits every channel. */
	mu_assert_int_eq(NUM_BREDR_CHANNELS, channels_visited());
}

/*
 * Re-sending the mode that is already running is not a mode start, and resets
 * nothing. A follow that loses the piconet's clock sends start-hopping again
 * mid-session; that must not throw away the map it is following on.
 */
MU_TEST(re_sending_the_running_mode_keeps_its_hopping)
{
	a_run_with_a_map();

	request_start_hopping();

	mu_assert_int_eq(MODE_BT_FOLLOW, mode);
	mu_assert_int_eq(HOP_BLUETOOTH, hop_mode);
	mu_assert_int_eq(1, afh_enabled);
	mu_assert_int_eq(20, used_channels);
	mu_assert_int_eq(20, channels_visited());
}

/*
 * Switching back and forth under one stream resets every time. An absorbed
 * request never reaches the dispatch, so `mode` stays the mode that entered
 * the loop: asking whether this request is a re-send has to compare it with
 * the mode the loop is streaming, which is the last accepted request.
 */
MU_TEST(switching_back_resets_again)
{
	board_reset();
	request_start_hopping();
	request_rx_symbols();
	/* A map arriving while the capture runs, as a host that drives the
	 * board by hand can send one. */
	request_set_afh_map(AFH_MAP_20);

	request_start_hopping();

	mu_assert_int_eq(HOP_BLUETOOTH, hop_mode);
	mu_assert_int_eq(0, afh_enabled);
	mu_assert_int_eq(NUM_BREDR_CHANNELS, channels_visited());
}

/* A channel set on its own is kept, and the next mode starts on it. */
MU_TEST(a_channel_set_on_its_own_is_kept_for_the_next_mode)
{
	board_reset();

	request_set_channel(2412);
	mu_assert_int_eq(2412, channel);
	/* Reading it back, twice, with no mode in between: still there. */
	board_poll();
	mu_assert_int_eq(2412, channel);
	board_poll();
	mu_assert_int_eq(2412, channel);

	/* A capture started afterwards is the capture it was set for. */
	request_rx_symbols();
	mu_assert_int_eq(2412, channel);
	mu_assert_int_eq(1, channels_visited());

	/* The mode owned it from then on: once that mode ends it is back to the
	 * idle channel, so the next capture needs it set again. */
	request_stop();
	mu_assert_int_eq(IDLE_CHANNEL, channel);
}

/*
 * A mode started from an idle board resets nothing, because there is nothing
 * to reset: no earlier mode's state can be there, and what the host set while
 * the board was idle is for the capture it is about to start. The band sweep
 * is the case that matters -- ubertooth-rx asks for one by default, and
 * ubertooth-scan always, each just before asking for the capture -- so the
 * capture has to start sweeping, not on one channel.
 */
MU_TEST(a_mode_started_from_idle_keeps_what_was_set_for_it)
{
	board_reset();

	request_set_channel_sweep();
	request_rx_symbols();

	mu_assert_int_eq(MODE_RX_SYMBOLS, mode);
	mu_assert_int_eq(HOP_SWEEP, hop_mode);
	mu_assert(channels_visited() > 1, "the capture sweeps the band");
}

/*
 * ...and the reset an absorbed request runs leaves the channel alone: the
 * radio is tuned and receiving, and the channel is the one the host set for
 * this capture, a moment before the mode request.
 */
MU_TEST(an_absorbed_request_leaves_the_channel_alone)
{
	board_reset();
	request_afh();
	request_set_afh_map(AFH_MAP_20);

	request_set_channel(2420);
	request_rx_symbols();

	mu_assert_int_eq(2420, channel);
	mu_assert_int_eq(HOP_NONE, hop_mode);
	mu_assert_int_eq(0, afh_enabled);
}

/*
 * A mode that is not absorbed goes through the dispatch, which resets on the
 * way out of the mode it replaces -- including the hop mode its own request
 * asked for a moment earlier. So the dispatch asks again: a follow started
 * from any other mode still hops with the piconet.
 */
MU_TEST(a_dispatched_follow_still_hops)
{
	board_reset();
	request_specan();
	mu_assert(!streaming, "the spectrum analyzer is not a symbol stream");

	request_start_hopping();

	mu_assert_int_eq(MODE_BT_FOLLOW, mode);
	mu_assert_int_eq(HOP_BLUETOOTH, hop_mode);
	mu_assert_int_eq(NUM_BREDR_CHANNELS, channels_visited());
}

int main(void)
{
	printf("mode state carry-over tests\n");
	MU_RUN(a_mode_start_forgets_the_map_the_last_run_found);
	MU_RUN(a_capture_after_a_killed_tool_inherits_nothing);
	MU_RUN(an_absorbed_follow_hops_with_the_piconet);
	MU_RUN(an_absorbed_detection_starts_from_an_empty_map);
	MU_RUN(re_sending_the_running_mode_keeps_its_hopping);
	MU_RUN(switching_back_resets_again);
	MU_RUN(a_channel_set_on_its_own_is_kept_for_the_next_mode);
	MU_RUN(a_mode_started_from_idle_keeps_what_was_set_for_it);
	MU_RUN(an_absorbed_request_leaves_the_channel_alone);
	MU_RUN(a_dispatched_follow_still_hops);
	return mu_report();
}
