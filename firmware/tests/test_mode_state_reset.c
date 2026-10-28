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
/* The E-GO sync word, in ego.c. */
static u32 access_code;

/*
 * What the host stated after its mode request, which a mode start keeps: the
 * copy the two requests leave, and the mode request it followed.
 */
#define STATED_CHANNEL     (1 << 0)
#define STATED_ACCESS_CODE (1 << 1)

static u8 stated_what;
static u8 stated_for_mode;
static u16 stated_channel;
static u32 stated_access_code;
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

/* reset_absorbed_mode_state(): note it does not touch the channel averages. */
static void reset_absorbed_mode_state(u8 requested)
{
	if (requested == requested_mode || !streams_symbols(mode) ||
	    !streams_symbols(requested))
		return;

	hop_mode = HOP_NONE;
	clear_afh_map();
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
	dispatch();
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

/* UBERTOOTH_EGO: a mode that captures no symbols, and whose own loop absorbs a
 * second request for it, so that request resets the access code itself. */
static void request_ego(void)
{
	if (mode == MODE_EGO && requested_mode == MODE_EGO)
		ego_access_code_reset();
	requested_mode = MODE_EGO;
	board_poll();
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
	request_ego();
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
	request_ego();
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
	request_ego();
	request_set_access_code(0x11223344);
	mu_assert_int_eq(0x11223344, access_code);

	/* The run ends, and the next one sends no code of its own. */
	request_stop();
	request_ego();
	mu_assert_int_eq(DOCUMENTED_ACCESS_CODE, access_code);
}

/* And a code stated after the mode request survives the reset that mode start
 * runs, the way a channel does. */
MU_TEST(an_access_code_stated_after_the_mode_request_survives)
{
	board_reset();
	request_specan();

	dispatch_held = 1;
	request_ego();
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
	request_ego();
	request_set_access_code(0x11223344);

	/* No stop: the next run asks for E-GO again. */
	request_ego();

	mu_assert_int_eq(MODE_EGO, mode);
	mu_assert_int_eq(DOCUMENTED_ACCESS_CODE, access_code);

	/* A second run that does pass -a still gets what it asked for. */
	request_set_access_code(0x55667788);
	mu_assert_int_eq(0x55667788, access_code);
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
	return mu_report();
}
