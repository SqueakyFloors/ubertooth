/*
 * What a board carries over from one run to the next.
 *
 * Every mode start resets the board's mode state, its hop mode and its AFH
 * channel map. The channel is the exception: one set on its own, with no mode
 * running, is kept, and the next mode starts on it. So are the per-channel RSSI
 * averages, but only on the absorbed path, where the radio never stopped
 * receiving. Two places have to hold that rule, because there are two ways a
 * mode starts:
 *
 *   - the mode dispatch in main(), which leaves the running mode first and so
 *     runs the idle reset; and
 *   - a second E-GO request, which ego_main()'s loop absorbs the same way,
 *     because that loop runs while MODE_EGO is the mode asked for. There the
 *     loop starts its state machine over, on a flag the request handler sets.
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
 *
 * The last part of this file is the same question for low-energy state: what
 * one ubertooth-btle run leaves behind for the next. Its own scan of
 * bluetooth_rxtx.c is at the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "bluetooth.h"

/* Non-static state in bluetooth.c. */
extern u8 bank[NUM_BREDR_CHANNELS];
extern u8 afh_bank[NUM_BREDR_CHANNELS];

/* A piconet to hop with: any address will do, the sequence is its own. */
#define TEST_ADDRESS UINT64_C(0x12345678)

/* The channel a fixed-channel capture is left on by the idle reset. */
#define IDLE_CHANNEL 2441

/* And the advertising channel the idle reset leaves behind: 37, the first of
 * the three. */
#define IDLE_ADV_CHANNEL 2402

/* What ubertooth-rx and ubertooth-scan send to ask for the band sweep: any
 * channel above the band. */
#define SWEEP_REQUEST 9999

/* The access code ubertooth-ego(1) documents, which the firmware and the tool
 * both take from EGO_DEFAULT_ACCESS_CODE in ubertooth_interface.h. The literal
 * here is the pin, under a name of its own so it cannot be the header's. */
#define DOCUMENTED_ACCESS_CODE 0x630f9ffeu

/* The board state the firmware keeps in bluetooth_rxtx.c. */
static u8 mode;
static u8 requested_mode;
static u8 hop_mode;
static u16 channel;
/* The channel low-energy follow mode tunes to, which UBERTOOTH_SET_CHANNEL
 * sets alongside the channel. */
static u16 le_adv_channel;
/* ego.h's modes, which cannot be included here: it is hardware code. */
#define EGO_FOLLOW 0
#define EGO_CONTINUOUS_RX 1
#define EGO_JAM 2
/* An E-GO mode value the firmware has no state machine for. */
#define EGO_NO_SUCH_MODE 7

/* The first of the E-GO channel list. Following and interference hop along
 * that list, so they start there whatever channel the host stated; continuous
 * receive stays on the channel it was given. */
#define EGO_FIRST_CHANNEL 2408

/* The E-GO sync word, in ego.c. */
static u32 access_code;

/*
 * What the host stated after its mode request, which a mode start keeps: the
 * copy the two requests leave, and the mode request it followed.
 */
#define STATED_CHANNEL     (1 << 0)
#define STATED_ACCESS_CODE (1 << 1)
#define STATED_AFH_MAP     (1 << 2)

static u8 stated_what;
static u8 stated_for_mode;
static u16 stated_channel;
static u32 stated_access_code;
static u8 stated_afh_map[10];

/* The E-GO mode asked for, and the request handler's word to ego_main()'s loop
 * that a request has asked for one while it was running. */
static u8 ego_mode;
static u8 ego_restart;
/* Whether that loop is running, which mode its state machine is running (-1
 * for none), and what its rf_on() told the radio: the channel it tuned to, 0
 * while the radio is off, and the sync word it programmed. */
static int ego_looping;
static int ego_fsm_mode;
static u16 ego_radio_channel;
static u32 ego_radio_access_code;

/* Whether bt_stream_rx()'s loop is running, which is what absorbs a request. */
static int streaming;
/* Each channel's running average of what the radio heard there, the figure a
 * Classic record carries as its noise. */
static u8 channel_averages[NUM_BREDR_CHANNELS];

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
	le_adv_channel = IDLE_ADV_CHANNEL;
	clear_afh_map();
}

/* ego_access_code_reset() */
static void ego_access_code_reset(void)
{
	access_code = DOCUMENTED_ACCESS_CODE;
}

/*
 * set_channel_request(): a channel above the band asks for the sweep.
 *
 * The advertising channel goes with it, and in low-energy follow mode it is
 * the whole of it: that mode tunes from le_adv_channel, and the radio is left
 * where it is. So which of the two a request sets depends on the mode running
 * when it arrives -- and the dispatch applies a copy with `mode` already
 * naming the mode that is starting, which is the point of recording the value
 * rather than what it wrote.
 */
static void set_channel_request(u16 requested)
{
	if (requested > 2480) {
		hop_mode = HOP_SWEEP;
		requested = 2402;
	} else {
		hop_mode = HOP_NONE;
	}

	le_adv_channel = requested;
	if (mode != MODE_BT_FOLLOW_LE)
		channel = requested;
}

/* stated_for_starting_mode() */
static void stated_for_starting_mode(u8 what)
{
	if (stated_for_mode != requested_mode)
		stated_what = 0;
	stated_for_mode = requested_mode;
	stated_what |= what;
}

/* apply_stated_settings(), which the dispatch runs after the reset. */
static void apply_stated_settings(u8 starting_mode)
{
	u8 what;

	if (stated_what == 0)
		return;

	if (stated_for_mode != starting_mode) {
		if (stated_for_mode != requested_mode)
			stated_what = 0;
		return;
	}

	what = stated_what;
	stated_what = 0;

	if (what & STATED_CHANNEL)
		set_channel_request(stated_channel);
	if (what & STATED_ACCESS_CODE)
		access_code = stated_access_code;
	if (what & STATED_AFH_MAP)
		bluetooth_set_afh_map(stated_afh_map);
}

/* rssi_iir_reset(), which the dispatch runs on its way out of a mode. Each
 * channel's running average is modelled by whether it holds anything. */
static void rssi_iir_reset(void)
{
	memset(channel_averages, 0, sizeof(channel_averages));
}

/* A capture that has been running long enough to fill the filter. */
static void the_filter_has_built_up(void)
{
	memset(channel_averages, 1, sizeof(channel_averages));
}

static int the_filter_holds_something(void)
{
	size_t i;

	for (i = 0; i < sizeof(channel_averages); i++)
		if (channel_averages[i] != 0)
			return 1;
	return 0;
}

/* forget_hop_state(): what a mode start that never reaches the dispatch has
 * to reset for itself. */
static void forget_hop_state(void)
{
	hop_mode = HOP_NONE;
	clear_afh_map();
}

/* reset_absorbed_mode_state(): note it does not touch the channel averages. */
static void reset_absorbed_mode_state(u8 requested)
{
	if (requested == requested_mode || !streams_symbols(mode) ||
	    !streams_symbols(requested))
		return;

	forget_hop_state();
}

/*
 * One pass of ego_main()'s loop, and the state machine's way into the radio.
 *
 * The loop runs while MODE_EGO is the mode asked for, so a second E-GO request
 * never reaches the dispatch: it sets ego_restart, and the loop starts the
 * state machine over here.
 */
static void ego_pass(void)
{
	if (requested_mode != MODE_EGO) {
		/* ego_deinit(): the loop returns and the radio stops. */
		ego_looping = 0;
		ego_fsm_mode = -1;
		ego_radio_channel = 0;
		return;
	}

	if (ego_restart) {
		ego_restart = 0;
		ego_fsm_mode = ego_mode;
		/* ego_fsm_start(): the radio stopped, and a state machine
		 * holding nothing of the run before it. */
		ego_radio_channel = 0;
	}

	if (ego_radio_channel == 0) {
		/* rf_on(), from the state machine's first state: following and
		 * interference start on the first of the E-GO channels,
		 * continuous receive on the channel the host stated. */
		if (ego_fsm_mode != EGO_CONTINUOUS_RX)
			channel = EGO_FIRST_CHANNEL;
		ego_radio_channel = channel;
		ego_radio_access_code = access_code;
		return;
	}

	/*
	 * continuous_cap_state(): the only E-GO mode that tunes once, so it is
	 * the one that has to notice a channel or access code stated after it
	 * started. The other two run rf_on() again on every hop.
	 */
	if (ego_fsm_mode == EGO_CONTINUOUS_RX &&
	    (channel != ego_radio_channel ||
	     access_code != ego_radio_access_code))
		ego_radio_channel = 0;
}

/* ego_main(), from entry to its first pass: it picks the state machine for the
 * mode the request named, and the loop below picks it again on a restart. */
static void ego_main(void)
{
	ego_looping = 1;
	ego_fsm_mode = ego_mode;
	ego_pass();
}

/*
 * One turn of main()'s loop: the dispatch, for a request that reaches it.
 *
 * The request is a parameter rather than a read of requested_mode, because the
 * firmware reads it once on its way in and the reset it runs next takes long
 * enough for another request to be answered in the middle of it. Everything
 * below that point works on the mode this read found.
 */
static void dispatch_with(u8 next_mode)
{
	u8 prev_mode = mode;

	if (next_mode == prev_mode)
		return;
	if (prev_mode != MODE_IDLE) {
		cc2400_idle();
		rssi_iir_reset();
		ego_access_code_reset();
	}
	mode = next_mode;
	apply_stated_settings(next_mode);

	switch (next_mode) {
	case MODE_AFH:
		hop_mode = HOP_AFH;
		afh_enabled = 1;
		break;
	case MODE_BT_FOLLOW:
		hop_mode = HOP_BLUETOOTH;
		break;
	case MODE_EGO:
		ego_main();
		break;
	default:
		break;
	}
	streaming = streams_symbols(next_mode);
}

static void dispatch(void)
{
	dispatch_with(requested_mode);
}

/*
 * A mode that captures no symbols does not reach the dispatch the moment a
 * request is answered: its loop looks at requested_mode once a pass, returns,
 * and the dispatch resets on the way out of it -- long enough for the host's
 * next request to arrive first, which is the order a tool states its channel
 * in. A test that needs that order holds the dispatch while it sends the
 * requests and then lets the loop return.
 */
static int dispatch_held;

/*
 * Where the board is after a request: the running stream absorbs a request for
 * one of its three modes, and anything else goes through the dispatch.
 */
static void board_poll(void)
{
	if (dispatch_held)
		return;
	if (streaming && streams_symbols(mode) && streams_symbols(requested_mode))
		return;
	if (ego_looping) {
		ego_pass();
		/* The loop ends by returning, and main() reaches the dispatch
		 * on its next turn. */
		if (ego_looping)
			return;
	}
	dispatch();
}

/* Passes of whichever loop is running, for a test that has stated something
 * and wants the mode to have acted on it. */
static void the_board_runs(void)
{
	int i;

	for (i = 0; i < 4; i++)
		board_poll();
}

static void the_old_mode_loop_returns(void)
{
	dispatch_held = 0;
	board_poll();
}

/*
 * The same, for the window inside the dispatch: it has read the request and is
 * running the reset, and `arriving` is what another tool gets answered while
 * that happens. The mode the dispatch read starts first, then the one that
 * arrived.
 */
static void the_old_mode_loop_returns_while(void (*arriving)(void))
{
	u8 next_mode = requested_mode;  /* the dispatch's one read */

	arriving();                     /* answered during the reset */
	dispatch_held = 0;
	dispatch_with(next_mode);
	board_poll();
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

/* UBERTOOTH_SET_CHANNEL. A channel above the band asks for the band sweep
 * instead, which is how ubertooth-rx and ubertooth-scan ask for one. */
static void request_channel(u16 requested)
{
	set_channel_request(requested);
	if (requested_mode != mode) {
		stated_channel = requested;
		stated_for_starting_mode(STATED_CHANNEL);
	}
	board_poll();
}

/* UBERTOOTH_RFCAT_SUBCMD, RFCAT_SET_AA */
static void request_set_access_code(u32 code)
{
	access_code = code;
	if (requested_mode != mode) {
		stated_access_code = code;
		stated_for_starting_mode(STATED_ACCESS_CODE);
	}
	board_poll();
}

/*
 * UBERTOOTH_EGO, which carries the E-GO mode to run. A mode the firmware has
 * no state machine for is refused before anything is stored, so it leaves
 * nothing behind; returns 0 for that, as the handler does.
 *
 * This mode captures no symbols, and its own loop absorbs a second request for
 * it, so the request does that mode start's work itself: the access code, the
 * hopping, and the restart the loop acts on.
 */
static int request_ego(u8 ego)
{
	if (ego != EGO_FOLLOW && ego != EGO_CONTINUOUS_RX && ego != EGO_JAM)
		return 0;

	/* Stored before the restart is asked for, because the loop reads the
	 * mode only once it has seen the flag. */
	ego_mode = ego;
	if (mode == MODE_EGO && requested_mode == MODE_EGO) {
		ego_access_code_reset();
		forget_hop_state();
		ego_restart = 1;
	}
	requested_mode = MODE_EGO;
	board_poll();
	return 1;
}

/* UBERTOOTH_BTLE_PROMISC: another mode that captures no symbols, and asks for
 * no hopping of its own. */
static void request_btle_promisc(void)
{
	hop_mode = HOP_NONE;
	requested_mode = MODE_BT_PROMISC_LE;
	board_poll();
}

/* UBERTOOTH_BTLE_SNIFFING: low-energy follow, which tunes from the
 * advertising channel rather than from the channel. */
static void request_btle_follow(void)
{
	requested_mode = MODE_BT_FOLLOW_LE;
	board_poll();
}

/* UBERTOOTH_SET_AFHMAP: the firmware's own, map and hopping table alike, and
 * a copy kept for the mode request it followed. */
static void request_set_afh_map(const u8 *map)
{
	bluetooth_set_afh_map(map);
	if (requested_mode != mode) {
		memcpy(stated_afh_map, map, sizeof(stated_afh_map));
		stated_for_starting_mode(STATED_AFH_MAP);
	}
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
	ego_mode = EGO_FOLLOW;
	ego_restart = 0;
	ego_looping = 0;
	ego_fsm_mode = -1;
	ego_radio_channel = 0;
	ego_radio_access_code = 0;
	dispatch_held = 0;
	stated_what = 0;
	stated_for_mode = MODE_IDLE;
	cc2400_idle();
	rssi_iir_reset();
	ego_access_code_reset();

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

	request_channel(2412);
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

	request_channel(SWEEP_REQUEST);
	request_rx_symbols();

	mu_assert_int_eq(MODE_RX_SYMBOLS, mode);
	mu_assert_int_eq(HOP_SWEEP, hop_mode);
	mu_assert(channels_visited() > 1, "the capture sweeps the band");
}

/*
 * The channel averages go the same way as the channel: the dispatch drops them
 * on the way out of a mode, because a capture that has ended stops describing
 * the band, and the next one builds them up again.
 */
MU_TEST(a_dispatched_mode_start_forgets_the_channel_averages)
{
	board_reset();
	request_afh();
	the_filter_has_built_up();

	/* A mode the stream does not absorb, so this goes through the dispatch. */
	request_specan();

	mu_assert(!the_filter_holds_something(),
	          "a mode start through the dispatch forgets the averages");
}

/*
 * An absorbed request keeps them, for the reason it keeps the channel: the
 * radio never stopped receiving, and all three of these modes hear the same
 * modulation over the same channels, so the measurement still holds. Dropping
 * it would cost the new capture the few hundred packets the filter takes to
 * build up, which it would spend reporting the -55 dBm an empty filter gives.
 */
MU_TEST(an_absorbed_request_keeps_the_channel_averages)
{
	board_reset();
	request_afh();
	the_filter_has_built_up();

	request_rx_symbols();

	mu_assert_int_eq(MODE_RX_SYMBOLS, requested_mode);
	mu_assert(the_filter_holds_something(),
	          "an absorbed mode start keeps what the radio has been hearing");
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

	request_channel(2420);
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

/*
 * The case the rule exists for: a board left in a mode that captures nothing.
 * Its loop notices the capture request, returns, and the dispatch resets on
 * the way out of it -- after the request has been answered, so the sweep the
 * tool states next used to be thrown away and every packet came back on
 * 2441 MHz while the user had asked for the band. The board now keeps what
 * was stated after the request for the mode it is starting.
 */
MU_TEST(a_sweep_stated_after_the_capture_request_survives_a_slow_mode_exit)
{
	board_reset();
	request_specan();

	dispatch_held = 1;
	request_rx_symbols();
	request_channel(SWEEP_REQUEST);
	the_old_mode_loop_returns();

	mu_assert_int_eq(MODE_RX_SYMBOLS, mode);
	mu_assert_int_eq(HOP_SWEEP, hop_mode);
	mu_assert(channels_visited() > 1, "the capture sweeps the band");
}

/* The same for a channel the command line named. */
MU_TEST(a_channel_stated_after_the_capture_request_survives_too)
{
	board_reset();
	request_specan();

	dispatch_held = 1;
	request_rx_symbols();
	request_channel(2412);
	the_old_mode_loop_returns();

	mu_assert_int_eq(2412, channel);
	mu_assert_int_eq(HOP_NONE, hop_mode);
	mu_assert_int_eq(1, channels_visited());
}

/*
 * It is kept for that mode and no other. A tool that died between its mode
 * request and its mode starting leaves a channel behind; the mode another tool
 * then asks for starts from the reset, not on that channel. Promiscuous
 * low-energy capture is the case that would notice: it asks for no hopping,
 * and a sweep carried into it would hop it across the band.
 */
MU_TEST(a_channel_stated_for_a_mode_that_never_started_is_dropped)
{
	board_reset();
	request_specan();

	dispatch_held = 1;
	request_rx_symbols();
	request_channel(SWEEP_REQUEST);
	/* That run is gone before its capture started. The next tool asks for
	 * a mode of its own. */
	request_btle_promisc();
	the_old_mode_loop_returns();

	mu_assert_int_eq(MODE_BT_PROMISC_LE, mode);
	mu_assert_int_eq(IDLE_CHANNEL, channel);
	mu_assert_int_eq(HOP_NONE, hop_mode);
}

/*
 * The advertising channel `ubertooth-btle -A` asks for goes the same way, and
 * is the setting that mode actually tunes from. It used to be put back to
 * channel 37 as the old mode was left, so a follow asked for channel 38 or 39
 * listened on 37 -- quietly, because 37 carries advertisements too.
 *
 * The copy is applied with the new mode already recorded, so the request runs
 * its low-energy branch: the advertising channel is set and the radio is left
 * where the reset put it, which is what that mode expects.
 */
MU_TEST(an_advertising_channel_stated_after_the_follow_request_survives)
{
	board_reset();
	request_specan();

	dispatch_held = 1;
	request_btle_follow();
	request_channel(2426);
	the_old_mode_loop_returns();

	mu_assert_int_eq(MODE_BT_FOLLOW_LE, mode);
	mu_assert_int_eq(2426, le_adv_channel);
	mu_assert_int_eq(IDLE_CHANNEL, channel);
}

/*
 * `ubertooth-util -S -c 2440` states its channel after the stop, while the
 * mode it is stopping is still running, so the reset on the way out of that
 * mode used to throw it away: the standalone set the man page promises raced
 * the mode it followed. The copy covers it, because a stop is a mode request
 * like any other.
 */
MU_TEST(a_channel_stated_after_a_stop_is_kept)
{
	board_reset();
	request_afh();
	mu_assert(streaming, "AFH detection streams");

	dispatch_held = 1;
	request_stop();
	request_channel(2440);
	the_old_mode_loop_returns();

	mu_assert_int_eq(MODE_IDLE, mode);
	mu_assert_int_eq(2440, channel);
}

/*
 * An access code set with no mode running is kept, and the capture that
 * follows uses it -- the same exception the channel has, and for the same
 * reason: a mode started on an idle board resets nothing, because no earlier
 * mode's code can be there to reset. A program driving the board by hand is
 * the only thing that reaches this: ubertooth-ego states its code after its
 * mode request.
 */
MU_TEST(an_access_code_set_with_no_mode_running_is_kept)
{
	board_reset();

	request_set_access_code(0x11223344);
	request_ego(EGO_FOLLOW);
	mu_assert_int_eq(MODE_EGO, mode);
	mu_assert_int_eq(0x11223344, access_code);

	/* And it goes no further than that capture: the run ends, and the next
	 * mode starts from the documented default. */
	request_stop();
	mu_assert_int_eq(DOCUMENTED_ACCESS_CODE, access_code);
}

/* What the second tool in the test below asks the board for. */
static void an_ego_run_with_an_access_code(void)
{
	request_ego(EGO_FOLLOW);
	request_set_access_code(0x11223344);
}

/*
 * Two tools inside one teardown. The dispatch reads the request once on its
 * way in, so the first tool's mode is the one that starts even if a second
 * tool's request and its setting are both answered while the reset runs. The
 * second tool's copy has to survive that mode start: the mode start it was
 * stated for is the one after it.
 *
 * A copy consumed at every mode start, whether or not it was applied, lost
 * this one, and the second run captured with the reset defaults.
 */
MU_TEST(a_setting_stated_for_a_mode_still_to_start_survives)
{
	board_reset();
	request_specan();

	dispatch_held = 1;
	/* The first tool's request is answered, and that run dies there. */
	request_rx_symbols();
	the_old_mode_loop_returns_while(an_ego_run_with_an_access_code);

	mu_assert_int_eq(MODE_EGO, mode);
	mu_assert_int_eq(0x11223344, access_code);
}

/*
 * The E-GO access code goes with the capture that asked for it. It used to be
 * set once at boot and kept until the board was reset, so `ubertooth-ego -a
 * 11223344` left 11223344 behind and the next run captured with it while the
 * tool called 630f9ffe the default.
 */
MU_TEST(a_mode_start_puts_the_access_code_back)
{
	board_reset();
	mu_assert_int_eq(DOCUMENTED_ACCESS_CODE, access_code);

	/* A run with -a, which states its code after the mode request. */
	request_ego(EGO_FOLLOW);
	request_set_access_code(0x11223344);
	mu_assert_int_eq(0x11223344, access_code);

	/* The run ends, and the next one sends no code of its own. */
	request_stop();
	request_ego(EGO_FOLLOW);
	mu_assert_int_eq(DOCUMENTED_ACCESS_CODE, access_code);
}

/* And a code stated after the mode request survives the reset that mode start
 * runs, the way a channel does. */
MU_TEST(an_access_code_stated_after_the_mode_request_survives)
{
	board_reset();
	request_specan();

	dispatch_held = 1;
	request_ego(EGO_FOLLOW);
	request_set_access_code(0x11223344);
	the_old_mode_loop_returns();

	mu_assert_int_eq(MODE_EGO, mode);
	mu_assert_int_eq(0x11223344, access_code);
}

/*
 * A killed E-GO run leaves the board capturing, and its own loop absorbs the
 * next run's request: no mode starts, so the dispatch's reset never runs. The
 * request resets the code itself, as the absorbed symbol captures reset their
 * hopping, so the second run does not capture with the first run's code.
 */
MU_TEST(an_absorbed_ego_request_resets_the_access_code)
{
	board_reset();
	request_ego(EGO_FOLLOW);
	request_set_access_code(0x11223344);

	/* No stop: the next run asks for E-GO again. */
	request_ego(EGO_FOLLOW);

	mu_assert_int_eq(MODE_EGO, mode);
	mu_assert_int_eq(DOCUMENTED_ACCESS_CODE, access_code);

	/* A second run that does pass -a still gets what it asked for. */
	request_set_access_code(0x55667788);
	mu_assert_int_eq(0x55667788, access_code);
}

/*
 * The map a follow states after asking for the hopping. ubertooth-follow sends
 * it there, and on a board left in a mode that captures nothing the teardown's
 * clear_afh_map() runs a moment after that request was answered: without the
 * copy the map is thrown away, and the follow hops all 79 channels instead of
 * the piconet's 20.
 */
MU_TEST(an_afh_map_stated_after_the_hopping_request_survives_a_slow_mode_exit)
{
	board_reset();
	request_specan();

	dispatch_held = 1;
	request_start_hopping();
	request_set_afh_map(AFH_MAP_20);
	the_old_mode_loop_returns();

	mu_assert_int_eq(MODE_BT_FOLLOW, mode);
	mu_assert_int_eq(1, afh_enabled);
	mu_assert_int_eq(20, used_channels);
	mu_assert_int_eq(20, channels_visited());
}

/* Kept for that mode and no other: a map left behind by a run that never
 * started would hop the next tool's capture over a piconet it knows nothing
 * about. */
MU_TEST(an_afh_map_stated_for_a_mode_that_never_started_is_dropped)
{
	board_reset();
	request_specan();

	dispatch_held = 1;
	request_start_hopping();
	request_set_afh_map(AFH_MAP_20);
	/* The follow's own mode request never arrives at the dispatch: the host
	 * has moved on and asks for a plain capture instead. */
	request_rx_symbols();
	the_old_mode_loop_returns();

	mu_assert_int_eq(MODE_RX_SYMBOLS, mode);
	mu_assert_int_eq(0, afh_enabled);
	mu_assert_int_eq(0, used_channels);
}

/* ------------------------------------------------------------------------
 * E-GO: a second run started while the first is still running
 * ------------------------------------------------------------------------ */

/* An E-GO run left running on the board, which is what a killed
 * ubertooth-ego leaves. A run states its channel after its mode request. */
static void a_killed_ego_run(u8 ego, u16 mhz)
{
	board_reset();
	mu_assert_int_eq(1, request_ego(ego));
	if (ego == EGO_CONTINUOUS_RX)
		request_channel(mhz);
	the_board_runs();
	mu_assert_int_eq(MODE_EGO, mode);
	mu_assert(ego_looping, "the E-GO loop is running");
	mu_assert_int_eq(ego, ego_fsm_mode);
}

/*
 * The case from the field. A killed `ubertooth-ego -f` leaves the loop
 * running, so the next run's request is absorbed: the state machine used to
 * carry on, and an `ubertooth-ego -r -c 2418` hopped across the E-GO channel
 * list in follow mode while its capture looked like a continuous receive that
 * found nothing.
 */
MU_TEST(an_ego_run_after_a_killed_one_runs_the_mode_it_asked_for)
{
	a_killed_ego_run(EGO_FOLLOW, 0);
	mu_assert_int_eq(EGO_FIRST_CHANNEL, ego_radio_channel);

	mu_assert_int_eq(1, request_ego(EGO_CONTINUOUS_RX));
	request_channel(2418);
	the_board_runs();

	mu_assert_int_eq(EGO_CONTINUOUS_RX, ego_fsm_mode);
	mu_assert_int_eq(2418, ego_radio_channel);
}

/*
 * The other way round, and the same rule: a follow run started while a
 * continuous receive is still running hops the E-GO channel list, rather than
 * staying on the channel the run before it was given.
 */
MU_TEST(an_absorbed_ego_follow_hops_the_ego_channel_list)
{
	a_killed_ego_run(EGO_CONTINUOUS_RX, 2469);
	mu_assert_int_eq(2469, ego_radio_channel);

	mu_assert_int_eq(1, request_ego(EGO_FOLLOW));
	the_board_runs();

	mu_assert_int_eq(EGO_FOLLOW, ego_fsm_mode);
	mu_assert_int_eq(EGO_FIRST_CHANNEL, ego_radio_channel);
}

/*
 * Asking for the same mode again is a mode start too, so it receives on the
 * channel it was given rather than the one the run before it was tuned to.
 * Nothing re-sends the request mid-run, so there is no re-send to protect the
 * way a follow that re-syncs its clock is protected.
 */
MU_TEST(a_second_run_of_the_same_ego_mode_retunes_the_radio)
{
	a_killed_ego_run(EGO_CONTINUOUS_RX, 2418);
	mu_assert_int_eq(2418, ego_radio_channel);

	mu_assert_int_eq(1, request_ego(EGO_CONTINUOUS_RX));
	request_channel(2423);
	the_board_runs();

	mu_assert_int_eq(2423, ego_radio_channel);
}

/*
 * Which E-GO mode to run is part of the request that starts the mode, so the
 * teardown of the mode it replaces must not reset it. That teardown runs
 * between the request and the dispatch that reads the mode, and it used to put
 * the mode back to following: after a killed ubertooth-specan, `ubertooth-ego
 * -r` hopped the channel list and `-i` did not interfere.
 */
MU_TEST(an_ego_request_keeps_its_mode_across_the_teardown)
{
	board_reset();
	/* A killed ubertooth-specan: no stop, so a mode is still running. */
	request_specan();
	mu_assert_int_eq(MODE_SPECAN, mode);

	dispatch_held = 1;
	mu_assert_int_eq(1, request_ego(EGO_CONTINUOUS_RX));
	request_channel(2423);
	the_old_mode_loop_returns();
	the_board_runs();

	mu_assert_int_eq(MODE_EGO, mode);
	mu_assert_int_eq(EGO_CONTINUOUS_RX, ego_fsm_mode);
	/* ...and the channel it stated for that mode reaches it. */
	mu_assert_int_eq(2423, ego_radio_channel);
}

/*
 * An absorbed E-GO request is a mode start, so it forgets the hopping as any
 * other does: a map a program driving the board by hand set while the run was
 * going cannot be left for whatever starts next.
 */
MU_TEST(an_absorbed_ego_request_forgets_the_hopping)
{
	a_killed_ego_run(EGO_FOLLOW, 0);
	request_set_afh_map(AFH_MAP_20);
	mu_assert_int_eq(1, afh_enabled);

	mu_assert_int_eq(1, request_ego(EGO_CONTINUOUS_RX));

	mu_assert_int_eq(HOP_NONE, hop_mode);
	mu_assert_int_eq(0, afh_enabled);
	mu_assert_int_eq(0, used_channels);
}

/* A map set with no mode running is kept for the capture that follows, which
 * is the rule a channel follows: nothing is reset as a mode starts on an idle
 * board. */
MU_TEST(an_afh_map_set_with_no_mode_running_is_kept)
{
	board_reset();
	request_set_afh_map(AFH_MAP_20);
	request_start_hopping();
	the_old_mode_loop_returns();

	mu_assert_int_eq(MODE_BT_FOLLOW, mode);
	mu_assert_int_eq(1, afh_enabled);
	mu_assert_int_eq(20, used_channels);
}

/*
 * A mode the firmware has no state machine for is refused, and refused before
 * anything is stored: the run that is going keeps going, and nothing is left
 * behind for the next mode start to pick up. A firmware built without transmit
 * refuses interference the same way.
 */
MU_TEST(a_refused_ego_request_leaves_the_run_alone)
{
	a_killed_ego_run(EGO_FOLLOW, 0);

	mu_assert_int_eq(0, request_ego(EGO_NO_SUCH_MODE));
	the_board_runs();

	mu_assert_int_eq(MODE_EGO, mode);
	mu_assert_int_eq(EGO_FOLLOW, ego_fsm_mode);
	mu_assert_int_eq(EGO_FOLLOW, ego_mode);
	mu_assert_int_eq(EGO_FIRST_CHANNEL, ego_radio_channel);
}

/* And a stop ends the run: the loop returns, and the dispatch then leaves the
 * mode as it does for any other. */
MU_TEST(a_stop_ends_an_ego_run)
{
	a_killed_ego_run(EGO_CONTINUOUS_RX, 2418);

	request_stop();

	mu_assert_int_eq(MODE_IDLE, mode);
	mu_assert(!ego_looping, "the E-GO loop has ended");
	mu_assert_int_eq(IDLE_CHANNEL, channel);
}

/* ------------------------------------------------------------------------
 * The low-energy state a run leaves behind
 * ------------------------------------------------------------------------ */

/*
 * Promiscuous mode (ubertooth-btle -p) has two halves: it listens on one data
 * channel until an access address repeats, then follows that address. The
 * first half is skipped when the host has already given the board an address
 * to follow, which is the only thing that may skip it -- and the board used to
 * ask a flag that UBERTOOTH_BTLE_SET_TARGET set as well. A -t target, which
 * is a device address and no access address at all, therefore sent the mode
 * straight to the second half, syncing on the advertising access address that
 * every reset puts back, so it captured nothing. The flag outlived the tool,
 * so every later -p did the same.
 *
 * bt_promisc_le() and the request handler are hardware code, so the two
 * settings, the reset and one pass of the mode's loop are modelled here, and
 * the scans below pin the firmware's own shape to the model.
 */

/* What reset_le() puts back: the access address every advertising packet
 * carries. */
#define ADVERTISING_AA 0x8e89bed6u

/* The access address of a connection on the channel the mode is listening on,
 * which is what its first half discovers. Any value but the advertising one
 * will do. */
#define DISCOVERED_AA 0x5a3b9c1du

static u32 le_access_address;
static int le_access_address_set;
static int le_target_set;
/* Whether the last pass of the mode's loop ran its first half. */
static int le_promisc_listened;

/* reset_le() */
static void reset_le(void)
{
	le_access_address = ADVERTISING_AA;
	le_access_address_set = 0;
}

/* The board as it comes up, as far as low-energy state goes. */
static void le_board_reset(void)
{
	le_target_set = 0;
	le_promisc_listened = 0;
	reset_le();
}

/* UBERTOOTH_SET_ACCESS_ADDRESS: "ubertooth-btle -a <address>" */
static void request_set_access_address(u32 aa)
{
	le_access_address = aa;
	le_access_address_set = 1;
}

/*
 * UBERTOOTH_BTLE_SET_TARGET: "ubertooth-btle -t <address>", or "-t none",
 * which is the request with a mask length of zero.
 */
static void request_set_target(int address_given)
{
	le_target_set = address_given;
}

/* One pass of bt_promisc_le()'s loop. */
static void promisc_pass(void)
{
	if (!le_access_address_set) {
		/* bt_generic_le() resets the low-energy state and listens */
		reset_le();
		le_promisc_listened = 1;
		le_access_address = DISCOVERED_AA;
	} else {
		le_promisc_listened = 0;
	}
	/* bt_le_sync() follows le_access_address from here */
}

/*
 * The connection timeout inside bt_le_sync(): it resets the low-energy state
 * and hands promiscuous mode's loop another pass.
 */
static void promisc_connection_timed_out(void)
{
	reset_le();
}

/* ------------------------------------------------------------------------
 * The firmware's own shape
 * ------------------------------------------------------------------------ */

/*
 * Walk one firmware source, comments and string literals removed, and call
 * `line_seen` for each line with the function and the request case it sits
 * in. A function header is a line that starts in column 0 and names
 * something; a case label is a line whose first word is `case`.
 */
typedef void (*line_fn)(const char *line, const char *function,
                        const char *request, int number);

static void scan_rxtx_lines(line_fn line_seen, const char *name)
{
	char path[256];
	FILE *source;
	char line[SCAN_LINE_MAX];
	char function[64] = "";
	char request[96] = "";
	int in_block = 0;
	int number = 0;

	snprintf(path, sizeof(path), "%s/%s", FW_RXTX_DIR, name);
	source = fopen(path, "r");
	if (source == NULL) {
		fprintf(stderr, "cannot open %s\n", path);
		exit(1);
	}

	while (fgets(line, sizeof(line), source) != NULL) {
		const char *trimmed;
		char flat[SCAN_LINE_MAX];

		number++;
		strip_comments(line, &in_block);
		strip_string_literals(line);
		collapse_spaces(line, flat, sizeof(flat));

		/* A function header: it starts in column 0 and has a name in
		 * front of its first '('. A prototype matches too, which is
		 * harmless: the lines this scan asks about are inside
		 * functions. */
		if (line[0] != '\0' && line[0] != ' ' && line[0] != '\t' &&
		    line[0] != '#' && line[0] != '}' && strchr(line, '(') != NULL) {
			if (call_name(line, function, sizeof(function)))
				request[0] = '\0';
		}

		trimmed = flat;
		if (strncmp(trimmed, "case ", 5) == 0) {
			size_t length = strcspn(trimmed + 5, ":");
			if (length < sizeof(request)) {
				memcpy(request, trimmed + 5, length);
				request[length] = '\0';
			}
		}

		line_seen(flat, function, request, number);
	}
	fclose(source);
}

/* What the scans found, filled in by the callbacks below. */
static int aa_set_assignments;         /* `le.access_address_set = 1` */
static char aa_set_request[96];        /* the request case it sits in */
static int aa_clear_assignments;       /* `le.access_address_set = 0` */
static char aa_clear_function[64];     /* the function it sits in */
static int target_case_touches_aa;     /* the target request names the setting */
static int promisc_reads_aa_set;       /* bt_promisc_le() asks the setting */
static int promisc_reads_target_set;   /* ...or still asks the target */
static int lines_scanned;

static void note_line(const char *line, const char *function,
                      const char *request, int number)
{
	(void)number;
	lines_scanned++;

	if (is_assignment_of(line, "le.access_address_set", "1")) {
		aa_set_assignments++;
		snprintf(aa_set_request, sizeof(aa_set_request), "%s", request);
	}
	if (is_assignment_of(line, "le.access_address_set", "0")) {
		aa_clear_assignments++;
		snprintf(aa_clear_function, sizeof(aa_clear_function), "%s",
		         function);
	}
	if (strcmp(request, "UBERTOOTH_BTLE_SET_TARGET") == 0 &&
	    contains_identifier(line, "access_address_set"))
		target_case_touches_aa = 1;
	if (strcmp(function, "bt_promisc_le") == 0) {
		if (contains_identifier(line, "le.access_address_set"))
			promisc_reads_aa_set = 1;
		if (contains_identifier(line, "le.target_set"))
			promisc_reads_target_set = 1;
	}
}

/* ------------------------------------------------------------------------
 * The low-energy tests
 * ------------------------------------------------------------------------ */

/* A target is for follow mode. Promiscuous mode still discovers an address. */
MU_TEST(a_target_does_not_stop_promiscuous_discovery)
{
	le_board_reset();
	request_set_target(1);

	promisc_pass();

	mu_assert(le_promisc_listened, "the mode skipped its discovery half");
	mu_assert_int_eq((int)DISCOVERED_AA, (int)le_access_address);
}

/* An access address the host gave is followed, discovery skipped. */
MU_TEST(an_access_address_from_the_host_is_followed)
{
	le_board_reset();
	request_set_access_address(0x12345678u);

	promisc_pass();

	mu_assert(!le_promisc_listened,
	          "the mode listened for an address it had been given");
	mu_assert_int_eq((int)0x12345678u, (int)le_access_address);
}

/*
 * The sequence out of the report: a follow run with a target, then a
 * promiscuous run of its own. Nothing between the two resets the low-energy
 * state -- the follow modes run le_phy_main(), which does not, and the target
 * is cleared by "-t none" and nothing else -- so the second run has to
 * discover an address with the target still on the board.
 */
MU_TEST(a_promiscuous_run_after_a_targeted_follow_discovers)
{
	le_board_reset();

	/* ubertooth-btle -f -t <address>, which resets nothing */
	request_set_target(1);

	/* ubertooth-btle -p, a command later */
	promisc_pass();

	mu_assert(le_target_set, "the target was cleared by something else");
	mu_assert(le_promisc_listened, "the mode skipped its discovery half");
	mu_assert_int_eq((int)DISCOVERED_AA, (int)le_access_address);
}

/*
 * A connection that goes quiet during a promiscuous run resets the low-energy
 * state, which puts the advertising access address back. The next pass has to
 * discover one rather than follow that.
 */
MU_TEST(a_connection_timeout_puts_discovery_back)
{
	le_board_reset();
	request_set_access_address(0x12345678u);
	promisc_pass();
	mu_assert(!le_promisc_listened, "the given address was not followed");

	promisc_connection_timed_out();
	promisc_pass();

	mu_assert(le_promisc_listened, "the mode followed a reset's address");
	mu_assert_int_eq((int)DISCOVERED_AA, (int)le_access_address);
}

/* The two settings are separate in both directions: clearing the target
 * leaves an access address the host gave alone. */
MU_TEST(clearing_the_target_keeps_the_given_access_address)
{
	le_board_reset();
	request_set_access_address(0x12345678u);

	request_set_target(0);
	promisc_pass();

	mu_assert(!le_promisc_listened, "the given address was forgotten");
	mu_assert_int_eq((int)0x12345678u, (int)le_access_address);
}

/* The firmware's shape, against the model above. */
MU_TEST(the_firmware_keeps_the_two_settings_apart)
{
	/* le_phy.c is scanned as well, because it shares le_state_t and reads
	 * le.target_set for its own advertising filter: a second place that
	 * set the access address setting would be just as wrong there. */
	scan_rxtx_lines(note_line, "bluetooth_rxtx.c");
	scan_rxtx_lines(note_line, "le_phy.c");

	mu_assert(lines_scanned > 1000, "the sources were not scanned");

	/* Only the access address request gives the mode an address. */
	mu_assert_int_eq(1, aa_set_assignments);
	mu_assert(strcmp(aa_set_request, "UBERTOOTH_SET_ACCESS_ADDRESS") == 0,
	          aa_set_request);
	mu_assert(!target_case_touches_aa,
	          "the target request touches the access address setting");

	/* And only a low-energy reset takes it away. */
	mu_assert_int_eq(1, aa_clear_assignments);
	mu_assert(strcmp(aa_clear_function, "reset_le") == 0,
	          aa_clear_function);

	/* The mode asks that setting, not the target. */
	mu_assert(promisc_reads_aa_set,
	          "bt_promisc_le() does not ask whether an address was given");
	mu_assert(!promisc_reads_target_set,
	          "bt_promisc_le() still asks whether a target was set");
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
	MU_RUN(a_dispatched_mode_start_forgets_the_channel_averages);
	MU_RUN(an_absorbed_request_keeps_the_channel_averages);
	MU_RUN(a_dispatched_follow_still_hops);
	MU_RUN(a_sweep_stated_after_the_capture_request_survives_a_slow_mode_exit);
	MU_RUN(a_channel_stated_after_the_capture_request_survives_too);
	MU_RUN(a_channel_stated_for_a_mode_that_never_started_is_dropped);
	MU_RUN(an_advertising_channel_stated_after_the_follow_request_survives);
	MU_RUN(a_setting_stated_for_a_mode_still_to_start_survives);
	MU_RUN(an_access_code_set_with_no_mode_running_is_kept);
	MU_RUN(a_channel_stated_after_a_stop_is_kept);
	MU_RUN(a_mode_start_puts_the_access_code_back);
	MU_RUN(an_access_code_stated_after_the_mode_request_survives);
	MU_RUN(an_absorbed_ego_request_resets_the_access_code);
	MU_RUN(an_afh_map_stated_after_the_hopping_request_survives_a_slow_mode_exit);
	MU_RUN(an_afh_map_stated_for_a_mode_that_never_started_is_dropped);
	MU_RUN(an_afh_map_set_with_no_mode_running_is_kept);
	MU_RUN(an_ego_run_after_a_killed_one_runs_the_mode_it_asked_for);
	MU_RUN(an_absorbed_ego_follow_hops_the_ego_channel_list);
	MU_RUN(a_second_run_of_the_same_ego_mode_retunes_the_radio);
	MU_RUN(an_ego_request_keeps_its_mode_across_the_teardown);
	MU_RUN(an_absorbed_ego_request_forgets_the_hopping);
	MU_RUN(a_refused_ego_request_leaves_the_run_alone);
	MU_RUN(a_stop_ends_an_ego_run);
	MU_RUN(a_target_does_not_stop_promiscuous_discovery);
	MU_RUN(an_access_address_from_the_host_is_followed);
	MU_RUN(a_promiscuous_run_after_a_targeted_follow_discovers);
	MU_RUN(a_connection_timeout_puts_discovery_back);
	MU_RUN(clearing_the_target_keeps_the_given_access_address);
	MU_RUN(the_firmware_keeps_the_two_settings_apart);
	return mu_report();
}
