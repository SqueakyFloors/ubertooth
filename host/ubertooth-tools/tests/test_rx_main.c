/*
 * What ubertooth-rx's exit status says about a live capture, and about the
 * requests that set one up, run through its real main() against a scripted
 * library.
 *
 * A board that refuses one of those requests used to be ignored, so the
 * capture ran on whatever channel and address the board was left on and the
 * run reported success. A refused request is now named and ends the command.
 *
 * A capture that cannot be started exits 1. The failing call's own return
 * value used to go out as the exit status, and a shell wraps that into a
 * byte, so a script saw 255 or 247 rather than a failure it could read.
 *
 * The last cases are about the receive loop rather than the exit status: a
 * piconet follow that hears nothing has the clock it is following given up,
 * and the wait for that is timed here, on a clock the test moves itself.
 *
 * A capture whose USB stream fails stops where it failed, so it holds less
 * than was asked for. It used to exit 0 all the same: the library printed the
 * transfer error and the receive loop kept spinning with nothing arriving
 * until the -t timer or a Ctrl-C ended the run, which a script read as a
 * clean capture. It now exits 1, while the two ordinary ends -- the timeout
 * and the interrupt -- keep exiting 0 with whatever was captured.
 *
 * main() is in ubertooth-rx.c, so that source is compiled in here with main()
 * renamed, as test_util_main.c does. Every libubertooth call is a fake below,
 * so no board is opened and no 72 MB ring buffer is allocated; libbtbb is the
 * real one, since the tool hands it the piconet and the survey.
 */

/* RTLD_NEXT, used below to reach libbtbb's real btbb_piconet_new(), is a GNU
 * extension, and nothing in the build asks for the GNU set. This has to come
 * before any include, because the first one pulls in <features.h>, where the
 * choice is read. Same reason as in test_stream_failure.c. */
#define _GNU_SOURCE 1

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_rx_main
#include "ubertooth-rx.c"
#undef main

#include "run_main.h"

/* The libubertooth globals ubertooth-rx uses. They live in ubertooth.c, which
 * is not linked here. */
FILE* infile;
FILE* dumpfile;
uint32_t systime;
int max_ac_errors = 2;

/* ---- a scripted library ------------------------------------------------- */

/* How a capture ends, once the receive loop is running. */
enum capture_end {
	END_BY_TIMEOUT,  /* the -t timer, or a board that simply stops: the fake
	                  * stops the loop the way libubertooth's handler does */
	END_BY_STREAM_FAILURE,
	END_BY_INTERRUPT
};

static struct {
	/* How many times the end-of-run gap report was made. */
	int gap_reports;
	int connect_calls, check_api_calls, cleanup_handlers;
	int bulk_inits, thread_starts, thread_stops, rx_syms_calls, stops;
	int channels_set;
	int bdaddrs_set;
	int modulations_set;
	int last_modulation; /* the modulation the capture was set to */
	int channel_rc;      /* returned by cmd_set_channel_quiet() */
	int bdaddr_rc;       /* returned by cmd_set_bdaddr_quiet() */
	int modulation_rc;   /* returned by cmd_set_modulation_quiet() */
	/* Returned by the three steps that set a capture up: the receive
	 * transfer, the poll thread that completes it, and the request that
	 * starts the mode. */
	int bulk_init_rc;
	int thread_start_rc;
	int rx_syms_rc;
	int api_rc;          /* returned by ubertooth_check_api() */
	int api_released;    /* whether that call released the board itself */
	int closes;          /* ubertooth_close() */
	int frees;           /* ubertooth_free() */
	int last_channel;    /* the channel the capture was set to, in MHz */
	int timeout_set;
	int blocks;          /* blocks handed to the callback */
	int blocks_to_end;   /* ...before the fake ends the capture */
	int connect_fails;
	/* The open failed because the user stopped the run, rather than
	 * because no board could be found. */
	int open_stopped;
	/* ubertooth_connect_select() raises SIGINT, as a signal landing while
	 * the board is being found does. */
	int interrupt_the_open;
	int stream_failed;
	/* 1 to have btbb_piconet_new() fail, as it does when the machine is out
	 * of memory. */
	int no_memory;
	/* What a replay's read of its input ends with, in stream_rx_file()'s own
	 * terms: 0 a clean end of file, minus an errno a read error, and a
	 * positive number the bytes of a last record that was cut short. */
	int replay_result;
	/* How many whole records it hands to the decoder before that, which is
	 * what decides whether the capture files hold anything. */
	int replay_records;
	enum capture_end end;
	char selector[64];
	/* Every request the tool sent, in order, one letter each: b the
	 * Bluetooth address, s start sending symbols, c the channel or the
	 * sweep, m the modulation. */
	char calls[16];
	ubertooth_t ut;
	/* 1 to have the teardown's STOP refused, so the board is left running
	 * the mode the capture started. */
	int stop_refused;
	/* The piconet follow, as the callback reports it to the tool, and how
	 * long each block takes, so a case can let seconds pass without
	 * waiting for them. */
	long block_ms;
	long follow_quiet_ms; /* -1 for a capture following no clock */
	/* How long the clock search has been running, as the callback reports
	 * it: -1 when no search is running, and moved on by block_ms per block
	 * like the reading above. */
	long clock_search_ms;
	int hear_per_block;  /* 1 to hear a packet of the piconet per block */
	int drops;           /* clocks this capture asked to give up */
	int drop_result;     /* what each of those asks reports */
	/* The boards. The first handle is `ut`, as every single-board case
	 * here uses, and `extra` holds the rest in the order -U named them;
	 * `inits` counts the handles handed out. */
	ubertooth_t extra[RX_MAX_BOARDS - 1];
	int inits;
	/* Every selector a board was opened with, in order, comma separated. */
	char selectors[256];
	/* The channel each board hears this piconet on, or -1 for a board that
	 * hears nothing. The fake decode below records it on the piconet the
	 * board was given, which is the one thing the real decode does that
	 * the tool then shares between the boards. */
	int heard_channel[RX_MAX_BOARDS];
	/* 1 for a board whose packets settle the upper address part, with the
	 * value it settles on. */
	int heard_uap_board[RX_MAX_BOARDS];
	uint8_t heard_uap;
	/* What the boards after the first settle on, where a case wants them
	 * to disagree. 0 for "the same as heard_uap". */
	uint8_t heard_uap_other;
	/* What one named board settles on, where a case wants more than two
	 * answers in the run. 0 for "whatever the two above say". */
	uint8_t heard_uap_value[RX_MAX_BOARDS];
	/* The block before which a board settles nothing, so a case can put
	 * one board's answer later in the run than another's. 0 for a board
	 * that settles on the first block it is given. */
	int settle_uap_at[RX_MAX_BOARDS];
	/* The block at which a board's piconet has its address part taken off
	 * it, as libbtbb does when a clock is given up: the address goes to
	 * zero and the valid flag with it, and the board rediscovers. 0 for a
	 * board that keeps what it settled on. Blocks are counted across the
	 * boards, and after the callback, so with three boards the first sees
	 * 0, 3, 6 and the second 1, 4, 7. */
	int abandon_uap_at[RX_MAX_BOARDS];
	/* What a board settles on after that, which is how a stale reading
	 * shows up: the run must rest on what the boards hold now. */
	uint8_t heard_uap_again[RX_MAX_BOARDS];
	/* 1 for a run where giving a clock up takes the piconet's address part
	 * with it, as libbtbb's does. That happens outside the decode, so a
	 * case can reach the state no packet follows. */
	int abandon_uap_on_drop;
	/* What a board settles on once its clock has been given up, where the
	 * blocks keep flowing: the drop and the new value land in the same
	 * turn of the loop, so nothing in between sees the board hold none,
	 * which is the window a remembered reading goes stale in. */
	uint8_t heard_uap_after_drop[RX_MAX_BOARDS];
	/* The block from which every board's ring buffer is empty: the receive
	 * hands the decoder nothing and says so, and the follow's silence goes
	 * on being measured, which is how a quiet follow is noticed. The run
	 * ends with a Ctrl-C after quiet_returns_to_end of them, which is how
	 * a capture of a piconet that has gone away ends. */
	int quiet_blocks_at;
	int quiet_returns;
	int quiet_returns_to_end;
	/* The channel each board was asked to tune to, in MHz, and the
	 * requests each board was sent, one letter each in the order they
	 * arrived. Each fake handle is its own board's, so a request is
	 * recorded against the board it went to: a bug that sent every
	 * channel to one board would otherwise pass. */
	int board_channel[RX_MAX_BOARDS];
	char board_calls[RX_MAX_BOARDS][8];
	/* Which board the scripted follow and search readings belong to; every
	 * other board reads -1, as a board with no clock and no search does.
	 * So a case can put a quiet follow on one board and check that the
	 * loop asks that board and no other. */
	int reading_board;
	/* Which boards were asked to give a clock up. */
	int drops_board[RX_MAX_BOARDS];
	/* What each board's piconet already held when the fake decode was last
	 * handed it: how many channels it has been told this piconet uses, and
	 * the upper address part where it has one, -1 for none. A board holding
	 * more than it heard itself got the rest from the merge. */
	int seen_channels[RX_MAX_BOARDS];
	int seen_uap[RX_MAX_BOARDS];
} device;

static void record(char call)
{
	size_t length = strlen(device.calls);

	if (length + 1 < sizeof(device.calls)) {
		device.calls[length] = call;
		device.calls[length + 1] = '\0';
	}
}

/* 1 once a signal has asked the run to stop, as the library's own flag does:
 * the tool reads it through ubertooth_stop_requested() before it has a board
 * open. */
static int stop_requested;

static void reset_device(void)
{
	int i;

	memset(&device, 0, sizeof(device));
	stop_requested = 0;
	device.blocks_to_end = 3;
	/* Following no clock, and a board whose blocks take no time: a case
	 * about something else cannot reach the quiet-follow wait. */
	device.follow_quiet_ms = -1;
	/* And no clock search running, so a case about something else cannot
	 * reach the note about what the search does not reach either. */
	device.clock_search_ms = -1;
	device.drop_result = 1;
	for (i = 0; i < RX_MAX_BOARDS; i++) {
		device.heard_channel[i] = -1;
		device.heard_uap_board[i] = 0;
		device.seen_uap[i] = -1;
	}
}

/* The handle for one board, in the order -U named them. */
static ubertooth_t* fake_handle(int board)
{
	return board == 0 ? &device.ut : &device.extra[board - 1];
}

/* Which board a handle belongs to, so a fake handed a handle knows which
 * board's script to follow. -1 for a handle this test did not allocate. */
/* Which board a request's USB handle belongs to. */
static int devh_board(const struct libusb_device_handle* devh);

static int board_index(const ubertooth_t* ut)
{
	int i;

	for (i = 0; i < RX_MAX_BOARDS; i++)
		if (fake_handle(i) == ut)
			return i;
	return -1;
}

static int devh_board(const struct libusb_device_handle* devh)
{
	return board_index((const ubertooth_t*)devh);
}

/* What a board was asked for, in order. */
static void record_board(const struct libusb_device_handle* devh, char call)
{
	int board = devh_board(devh);
	size_t length;

	if (board < 0)
		return;
	length = strlen(device.board_calls[board]);
	if (length + 1 < sizeof(device.board_calls[board])) {
		device.board_calls[board][length] = call;
		device.board_calls[board][length + 1] = '\0';
	}
}

/* One handle per call, as the library hands out one per board. The tool asks
 * for as many as -U named. */
ubertooth_t* ubertooth_init(void)
{
	ubertooth_t* ut;

	if (device.inits >= RX_MAX_BOARDS)
		return NULL;
	ut = fake_handle(device.inits++);
	/* Its own handle, so every request below can be attributed. */
	ut->devh = (struct libusb_device_handle*)ut;
	/* Not a real ring buffer: the fake receive below never fills one, and
	 * the packet callback is faked too. */
	ut->fifo = (fifo_t*)malloc(1);
	return ut;
}

/* Counted rather than freeing: the handle above is this test's own. */
void ubertooth_free(ubertooth_t* ut)
{
	(void)ut;
	device.frees++;
}

int ubertooth_connect_select(ubertooth_t* ut, const char* u_arg)
{
	size_t used;

	(void)ut;
	device.connect_calls++;
	snprintf(device.selector, sizeof(device.selector), "%s",
	         u_arg ? u_arg : "");
	/* Every selector in order, so a multi-board case can read back which
	 * boards were opened and in which order. */
	used = strlen(device.selectors);
	snprintf(device.selectors + used, sizeof(device.selectors) - used,
	         "%s%s", used > 0 ? "," : "", u_arg ? u_arg : "(any)");
	/* Opening a board takes a moment -- selecting one by serial number
	 * reads the serial of every attached board -- and a signal can land in
	 * it. The real one is also where that signal is acted on: it gives up
	 * the search. This fake has no bus to search, so it only raises. */
	if (device.interrupt_the_open)
		raise(SIGINT);
	return device.connect_fails ? -1 : 0;
}

int ubertooth_check_api(ubertooth_t* ut)
{
	device.check_api_calls++;
	/* The library releases the board itself when the firmware reports a
	 * version this host will not drive; a version it could not read at all
	 * leaves the board open for the caller to close. */
	if (device.api_rc < 0 && device.api_released) {
		ut->devh = NULL;
		device.stops++;
	}
	return device.api_rc;
}

/* The real one also closes the capture files the handle holds, which is what
 * flushes a PCAP header out, so the fake does it too: a case that asks where
 * a capture went has to see what the writer wrote. */
void ubertooth_close(ubertooth_t* ut)
{
	ut->devh = NULL;
	device.closes++;
	if (ut->h_pcap_bredr != NULL) {
		btbb_pcap_close(ut->h_pcap_bredr);
		ut->h_pcap_bredr = NULL;
	}
	if (ut->h_pcapng_bredr != NULL) {
		btbb_pcapng_close(ut->h_pcapng_bredr);
		ut->h_pcapng_bredr = NULL;
	}
}

/* The one the library installs stops the capture on Ctrl-C; the fake does the
 * same, so a case can raise a real signal and check what the tool makes of
 * it. */
static void fake_cleanup(int sig)
{
	(void)sig;
	stop_requested = 1;
	device.ut.stop_ubertooth = 1;
}

/* The real one folds the signal handler's own flag into the handle; the
 * fake handler below sets the handle directly, so reading it is enough here. */
int ubertooth_stopping(ubertooth_t* ut)
{
	return ut->stop_ubertooth;
}

void ubertooth_catch_stop_signals(void)
{
	device.cleanup_handlers++;
	signal(SIGINT, fake_cleanup);
}

int ubertooth_stop_requested(void)
{
	return stop_requested;
}

/* Whether the fake open gave up because the run was stopped, which is what
 * decides the exit status of a run that opened no board. */
int ubertooth_open_stopped(void)
{
	return device.open_stopped;
}

/* Recorded rather than armed: a real alarm would have to expire for the case
 * that must not wait for it. */
void ubertooth_set_timeout(ubertooth_t* ut, int seconds)
{
	(void)ut;
	device.timeout_set = seconds;
}

/* The requests below carry the board's USB handle, which this fake makes the
 * same pointer for every board, so what they record is per run and the channel
 * each board was asked for is taken in the order the requests arrive. */
int cmd_set_channel_quiet(struct libusb_device_handle* devh, uint16_t channel)
{
	int board = devh_board(devh);

	record('c');
	record_board(devh, 'c');
	if (board >= 0)
		device.board_channel[board] = channel;
	device.channels_set++;
	device.last_channel = channel;
	return device.channel_rc;
}

/* The quiet form: it prints nothing, so what the cases below see on standard
 * error is the tool's own report. */
int cmd_rx_syms_quiet(struct libusb_device_handle* devh)
{
	record('s');
	record_board(devh, 's');
	device.rx_syms_calls++;
	return device.rx_syms_rc;
}

int cmd_set_bdaddr_quiet(struct libusb_device_handle* devh, uint64_t address)
{
	(void)address;
	record('b');
	record_board(devh, 'b');
	device.bdaddrs_set++;
	return device.bdaddr_rc;
}

int cmd_set_modulation_quiet(struct libusb_device_handle* devh, uint16_t mod)
{
	record('m');
	record_board(devh, 'm');
	device.modulations_set++;
	device.last_modulation = mod;
	return device.modulation_rc;
}

int ubertooth_bulk_init(ubertooth_t* ut)
{
	(void)ut;
	device.bulk_inits++;
	if (device.bulk_init_rc < 0)
		/* As the library reports a transfer libusb would not take: the
		 * libusb code in the line, -1 to the caller. */
		fprintf(stderr, "error: submitting the receive transfer failed "
		        "(%d)\n", LIBUSB_ERROR_NO_DEVICE);
	return device.bulk_init_rc;
}

/* pthread_create() returns an errno, which is positive, and the tool has to
 * read it that way: the fake hands back whatever the case asked for. */
int ubertooth_bulk_thread_start(void)
{
	device.thread_starts++;
	return device.thread_start_rc;
}

void ubertooth_bulk_thread_stop(void) { device.thread_stops++; }

/* One block per call, as the board would send them, and the capture ends
 * after blocks_to_end of them in the way the case asked for. A live capture
 * never ends by itself. */
int ubertooth_bulk_receive(ubertooth_t* ut, rx_callback cb, void* cb_args)
{
	/* A board with nothing in its ring buffer: the decoder is handed
	 * nothing, and the follow's silence goes on being measured. */
	if (device.quiet_blocks_at > 0 &&
	    device.blocks >= device.quiet_blocks_at) {
		if (device.follow_quiet_ms >= 0 &&
		    board_index(ut) == device.reading_board)
			device.follow_quiet_ms += device.block_ms;
		if (++device.quiet_returns >= device.quiet_returns_to_end)
			raise(SIGINT);
		return -1;
	}
	cb(ut, cb_args);
	device.blocks++;
	/* The time this block took, and a packet of the followed piconet in it
	 * where the case asked for one, which is what the callback stamps. */
	if (device.follow_quiet_ms >= 0 &&
	    board_index(ut) == device.reading_board)
		device.follow_quiet_ms = device.hear_per_block
		                       ? 0
		                       : device.follow_quiet_ms + device.block_ms;
	/* A clock search runs on through the blocks: unlike the reading above,
	 * hearing the piconet is what keeps it going rather than what starts it
	 * again. */
	if (device.clock_search_ms >= 0 &&
	    board_index(ut) == device.reading_board)
		device.clock_search_ms += device.block_ms;
	if (device.blocks < device.blocks_to_end)
		return 0;
	switch (device.end) {
	case END_BY_STREAM_FAILURE:
		/* What libubertooth does: report the transfer error, record it on
		 * the handle, and stop the capture there. */
		fprintf(stderr, "error: receive transfer failed: Transfer error."
		        " (1)\n");
		device.stream_failed = 1;
		ut->stop_ubertooth = 1;
		return 1;
	case END_BY_INTERRUPT:
		raise(SIGINT);
		return 0;
	case END_BY_TIMEOUT:
		ut->stop_ubertooth = 1;
		return 1;
	}
	return 0;
}


/* The end-of-run gap report. Counted rather than ignored, because both ways
 * out of the tool have to make it: a live capture and a replay of a recorded
 * one, whose records carry the gaps they were recorded with. */
void ubertooth_report_capture_losses(ubertooth_t* ut)
{
	(void)ut;
	device.gap_reports++;
}

int ubertooth_stream_failed(ubertooth_t* ut)
{
	(void)ut;
	return device.stream_failed;
}

void ubertooth_stop(ubertooth_t* ut)
{
	(void)ut;
	device.stops++;
}

/* The report about a refused request asks whether the board has left the bus
 * before it blames the board's firmware. Nothing here unplugs a board, so it
 * is always still there. */
int ubertooth_device_gone(const void* board)
{
	(void)board;
	return 0;
}

/* Whether the teardown's STOP was refused, which leaves the board running the
 * mode the capture started. Scripted, because the fake board above always
 * takes it. */
int ubertooth_stop_failed(ubertooth_t* ut)
{
	(void)ut;
	return device.stop_refused;
}

/* What a board settles its address part on, which can change inside a run: a
 * board the library has taken off one rediscovers, and a case can have it
 * reach a different value the second time. */
static uint8_t fake_settled_uap(int board)
{
	/* What it reaches after a clock was given up, which is the case the
	 * blocks keep flowing through: the drop and the new value land in the
	 * same turn of the loop, so nothing ever sees the board hold none. */
	if (device.drops_board[board] > 0 &&
	    device.heard_uap_after_drop[board] != 0)
		return device.heard_uap_after_drop[board];
	if (device.abandon_uap_at[board] != 0 &&
	    device.blocks > device.abandon_uap_at[board] &&
	    device.heard_uap_again[board] != 0)
		return device.heard_uap_again[board];
	if (device.heard_uap_value[board] != 0)
		return device.heard_uap_value[board];
	if (board > 0 && device.heard_uap_other != 0)
		return device.heard_uap_other;
	return device.heard_uap;
}

/* The packet decode is libubertooth's and needs none of this: what a block
 * contains does not change any exit status here.
 *
 * It does record the two things the real decode leaves on a piconet and the
 * tool then shares between boards: the channel a packet of the piconet was
 * heard on, and the upper address part once the decoder has settled one. The
 * script says which board hears what, so a case can give each board its own
 * channel and watch the merge. */
void cb_rx(ubertooth_t* ut, void* args)
{
	btbb_piconet* pn = (btbb_piconet*)args;
	int board = board_index(ut);

	int channel, seen = 0;

	if (pn == NULL || board < 0)
		return;

	/* Read before this board adds anything of its own, so what it holds
	 * here is what the merge gave it. */
	for (channel = 0; channel < NUM_BREDR_CHANNELS; channel++)
		if (btbb_piconet_get_channel_seen(pn, (uint8_t)channel))
			seen++;
	device.seen_channels[board] = seen;
	device.seen_uap[board] = btbb_piconet_get_flag(pn, BTBB_UAP_VALID)
	                       ? (int)btbb_piconet_get_uap(pn) : -1;

	if (device.heard_channel[board] >= 0)
		btbb_piconet_set_channel_seen(pn,
		        (uint8_t)device.heard_channel[board]);
	/* The library takes a piconet's address part off it when a clock is
	 * given up: the value goes to zero and the valid flag with it, which
	 * is the state the tool reads to know that what the board held is no
	 * longer what it holds. */
	if (device.abandon_uap_at[board] != 0 &&
	    device.blocks == device.abandon_uap_at[board]) {
		btbb_piconet_set_uap(pn, 0);
		btbb_piconet_set_flag(pn, BTBB_UAP_VALID, 0);
		return;
	}

	if (device.heard_uap_board[board] &&
	    device.blocks >= device.settle_uap_at[board] &&
	    !btbb_piconet_get_flag(pn, BTBB_UAP_VALID))
		btbb_piconet_set_uap(pn, fake_settled_uap(board));
}

/* What the callback reports about the follow: how long since it last heard a
 * packet of the piconet whose clock is being followed, and -1 when it is
 * following none. The cases move that reading on themselves rather than
 * waiting for a clock. */
/* How long the clock search has been running, with -1 for a run that has no
 * search going: nothing of the piconet has reached it, or a clock was
 * confirmed at some point. The cases move it on through the blocks above. */
long ubertooth_clock_search_ms(ubertooth_t* ut)
{
	return board_index(ut) == device.reading_board
	       ? device.clock_search_ms : -1;
}

long ubertooth_follow_quiet_ms(ubertooth_t* ut)
{
	return board_index(ut) == device.reading_board
	       ? device.follow_quiet_ms : -1;
}

/* Giving that clock up. The real one warns on standard error and takes the
 * board off the offset; here the call is counted. It leaves the follow where
 * the real one does: the wait starts again whatever comes of the ask, and a
 * clock that was given up leaves the capture following nothing. */
int ubertooth_follow_drop_clock(ubertooth_t* ut, btbb_piconet* pn)
{
	int board = board_index(ut);

	/* The real one takes the piconet's address part with the clock, where
	 * a case asks for that: libbtbb sets it to zero and clears the valid
	 * flag, because discovery cannot find another while the hop reversal
	 * is still filtering on this one. It happens here rather than in the
	 * decode, which is the whole point of it -- a piconet that has gone
	 * away leaves no packets to notice anything in. */
	if (device.abandon_uap_on_drop && pn != NULL) {
		btbb_piconet_set_uap(pn, 0);
		btbb_piconet_set_flag(pn, BTBB_UAP_VALID, 0);
	}
	if (board >= 0)
		device.drops_board[board]++;
	device.drops++;
	device.follow_quiet_ms = 0;
	if (device.drop_result <= 0)
		return 0;
	device.follow_quiet_ms = -1;
	return 1;
}

/* The -V answer, which libubertooth gives in every tool: faked here, so this
 * test stays about the tool's own command-line handling. The answer's own
 * cases are in test_version_report.c. */
int ubertooth_print_versions(FILE *out, const char *selector)
{
	(void)selector;
	fprintf(out, "version (test)\n");
	return 0;
}

/* The replay's read of its input, scripted: the records it hands to the
 * decoder, then how it ended. No file is read, so a case can ask for an
 * error the test's own input could not produce. */
int stream_rx_file(ubertooth_t* ut, FILE* fp, rx_callback cb, void* cb_args)
{
	int record;

	(void)fp;
	for (record = 0; record < device.replay_records; record++)
		(*cb)(ut, cb_args);
	return device.replay_result;
}

/* The real one prints the reason and turns anything but a clean end into the
 * exit status 1. The message is libubertooth's and has its own test; the
 * status is what the cases here read. */
int rx_file_report(int result, const char* filename)
{
	(void)filename;
	return result != 0;
}

/* ---- running the tool --------------------------------------------------- */

static struct run_result run_rx(const char* const* args)
{
	struct run_result result;

	/* The two file globals the tool leaves behind: a run that opened one
	 * would otherwise hand the next run a stale handle. */
	infile = NULL;
	dumpfile = NULL;
	/* The same for what the last replay read: the tool runs once per
	 * process, these cases run many times in one. */
	replay_read_a_record = 0;
	/* And whether the note about the clock search has been said, which is
	 * once per run of the tool rather than once per process. */
	clock_search_noted = 0;

	result = run_tool_main(ubertooth_rx_main, args);

	/* A run that was interrupted leaves this tool's own SIGINT handler
	 * installed, and the rest of the test run must not be caught by it. */
	signal(SIGINT, SIG_DFL);
	return result;
}

/* libbtbb is the real library here, so this stands in front of its
 * btbb_piconet_new() rather than replacing it: a case that has not asked for
 * the allocation to fail gets a real piconet, looked up past this definition.
 * A process only fails to allocate when it is out of memory, which a test
 * cannot reach without putting the whole run at risk, so the failure is
 * scripted. Same shape as test_stream_failure.c's pthread_create(). */
btbb_piconet* btbb_piconet_new(void)
{
	static btbb_piconet* (*real_btbb_piconet_new)(void);

	if (device.no_memory)
		return NULL;
	if (real_btbb_piconet_new == NULL)
		real_btbb_piconet_new = (btbb_piconet* (*)(void))
		                        dlsym(RTLD_NEXT, "btbb_piconet_new");
	if (real_btbb_piconet_new == NULL)
		return NULL;
	return real_btbb_piconet_new();
}

#define RUN_RX(...) \
	run_rx((const char* const[]){ "ubertooth-rx", __VA_ARGS__, NULL })

/* ---- tests -------------------------------------------------------------- */

MU_TEST(a_capture_whose_stream_died_exits_one)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_STREAM_FAILURE;
	device.blocks_to_end = 2;

	r = RUN_RX("-t", "30");

	mu_assert_int_eq(1, r.rc);
	/* On standard error, where the library put it. */
	mu_assert(strstr(r.err, "error: receive transfer failed") != NULL, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	/* The capture stopped at the failure rather than running on to the -t
	 * the command asked for, which is what the tool used to wait out. */
	mu_assert_int_eq(2, device.blocks);
	mu_assert_int_eq(30, device.timeout_set);
	mu_assert_int_eq(1, device.thread_stops);
	mu_assert_int_eq(1, device.stops);
}

/* A capture bound for standard output takes that stream over and writes its
 * PCAP header there, and the run's own lines go to standard error instead.
 * This is what the build's capture-to-stdout smoke test used to check on a
 * runner with no board: the header is now written only once the board is
 * usable, so it is pinned here, where a board can be faked. The first bytes
 * are libpcap's nanosecond magic and the file format version; the rest of the
 * header is libpcap's own. */
MU_TEST(a_capture_on_standard_output_gets_the_stream_and_its_header)
{
	static const unsigned char HEADER_START[] = {
		0x4d, 0x3c, 0xb2, 0xa1,  /* magic, little-endian, nanoseconds */
		0x02, 0x00, 0x04, 0x00   /* format version 2.4 */
	};
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 1;

	r = RUN_RX("-q", "-", "-z", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert(memcmp(r.out, HEADER_START, sizeof(HEADER_START)) == 0,
	          "standard output starts with the PCAP header");
	/* The survey report is on standard error, where the capture has not
	 * taken the stream. This board heard no address; test_rx_uap_record.py
	 * replays one and checks that it joins the heading there. */
	mu_assert(strstr(r.err, "Survey Results") != NULL, r.err);
}

/* A teardown the board refused leaves that board running the capture mode,
 * and no other tool can open it until it is reset or re-plugged, so the run
 * did not do what was asked. What was captured is still written out, and
 * libubertooth has already named the refused request, so nothing is added to
 * standard error here. This is the fault a lab board produced by itself: the
 * capture ran its timeout, so the run used to look like one that worked. */
MU_TEST(a_capture_whose_teardown_was_refused_exits_one)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;
	device.stop_refused = 1;

	r = RUN_RX("-t", "10");

	mu_assert_int_eq(1, r.rc);
	/* Nothing of this tool's own: the library printed the refusal. */
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert(r.out[0] == '\0', r.out);
	/* The capture itself ran to the end and kept every block. */
	mu_assert_int_eq(3, device.blocks);
	mu_assert_int_eq(1, device.stops);

	/* The same capture on a board that takes the stop. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(0, r.rc);
}

/* A survey whose teardown was refused still prints what it found: those
 * packets are real. Only the status says the board was left running. The
 * heading is on standard error, where every survey prints it; the addresses
 * go to standard output, and this run heard none. */
MU_TEST(a_survey_whose_teardown_was_refused_still_prints_its_results)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 1;
	device.stop_refused = 1;

	r = RUN_RX("-z", "-t", "10");

	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "Survey Results") != NULL, r.err);
	mu_assert(r.out[0] == '\0', r.out);
}

/* A run refused for its firmware creates no capture file: an empty one left
 * behind is a file the user then has to tell apart from a capture that caught
 * nothing. Each of the three file options is checked, because each is opened
 * by a writer of its own. */
MU_TEST(a_run_refused_for_its_firmware_creates_no_file)
{
	static const char* const options[] = { "-q", "-r", "-d" };
	struct run_result r;
	struct stat info;
	char path[64];
	size_t i;

	for (i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
		int fd;

		reset_device();
		device.api_rc = -1;
		device.api_released = 1;
		snprintf(path, sizeof(path), "/tmp/rx-main-XXXXXX");
		fd = mkstemp(path);
		mu_assert_fatal(fd >= 0, "temp name taken");
		close(fd);
		unlink(path);

		r = RUN_RX(options[i], path, "-t", "10");
		mu_assert_int_eq(1, r.rc);
		mu_assert_int_eq(1, device.check_api_calls);
		mu_assert(stat(path, &info) != 0, path);
		mu_assert_int_eq(0, device.rx_syms_calls);
		unlink(path);
	}
}

/* A capture that ran to its timeout did what was asked: it keeps exiting 0.
 * This is the regression that matters, since it is how a bounded capture
 * normally ends. */
MU_TEST(a_bounded_capture_still_exits_zero)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;

	r = RUN_RX("-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	/* Not cut short: every block the board sent was taken. */
	mu_assert_int_eq(3, device.blocks);
	mu_assert_int_eq(10, device.timeout_set);
	mu_assert_int_eq(1, device.bulk_inits);
	mu_assert_int_eq(1, device.rx_syms_calls);
	mu_assert_int_eq(1, device.thread_starts);
	mu_assert_int_eq(1, device.thread_stops);
	mu_assert_int_eq(1, device.stops);
}

/* A Ctrl-C is a user stopping a capture that was working. */
MU_TEST(an_interrupted_capture_still_exits_zero)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_INTERRUPT;
	device.blocks_to_end = 2;

	r = RUN_RX("-U", "0");

	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(2, device.blocks);
	mu_assert(strcmp(device.selector, "0") == 0, device.selector);
}

/* And a Ctrl-C that arrives while the board is still being opened ends the run
 * too, on that one signal.
 *
 * It used to be discarded. The handlers went in after the board was open, and
 * a tool started as a background job by a script is started with SIGINT
 * ignored, so a signal arriving before that did nothing at all: the user's
 * first Ctrl-C was lost and a second one was needed. The disposition is set to
 * SIG_IGN here to put the tool in exactly that position.
 *
 * The run ends between the board being opened and anything being asked of it,
 * so no mode is started on the board and nothing is captured. */
MU_TEST(an_interrupt_while_the_board_is_opened_ends_the_run)
{
	struct run_result r;

	reset_device();
	device.interrupt_the_open = 1;
	signal(SIGINT, SIG_IGN);

	r = RUN_RX("-U", "0");

	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(0, device.blocks);
	/* No capture is set up at all: the run ends between the board being
	 * opened and anything being asked of it. */
	mu_assert_int_eq(0, device.bulk_inits);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(0, device.channels_set);
	/* The board is given back, and without a STOP: no mode had started. */
	mu_assert_int_eq(1, device.closes);
	mu_assert_int_eq(0, device.stops);
	mu_assert_int_eq(1, device.frees);
}

/* And a stop that lands while the board is still being looked for ends the run
 * at exit 0 with nothing printed: the board selection gave up because the user
 * stopped it, which is not a failure to find a board. A selection that failed
 * for any other reason still exits 1. */
MU_TEST(a_selection_the_user_stopped_exits_zero)
{
	struct run_result r;

	reset_device();
	device.connect_fails = 1;
	device.open_stopped = 1;
	r = RUN_RX("-U", "0");
	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(0, device.rx_syms_calls);

	reset_device();
	device.connect_fails = 1;
	r = RUN_RX("-U", "0");
	mu_assert_int_eq(1, r.rc);
}

/* Survey mode prints what it found whatever ended the capture: the results
 * are what arrived, and the exit status is what says they are incomplete.
 * The heading shares standard error with the failure; this run heard no
 * address, so standard output holds nothing. */
MU_TEST(a_survey_whose_stream_died_still_prints_its_results)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_STREAM_FAILURE;
	device.blocks_to_end = 1;

	r = RUN_RX("-z", "-t", "30");

	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "Survey Results") != NULL, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: receive transfer failed") != NULL, r.err);
}

/* A command line the tool will not take is refused before anything is
 * opened, so the exit status above cannot come from a run that never
 * started. */
MU_TEST(a_refused_command_line_opens_no_device)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: -u <UAP> requires -l <LAP>") != NULL,
	          r.err);
	mu_assert_int_eq(0, device.connect_calls);
	mu_assert_int_eq(0, device.blocks);
}

/* A replay has no board to tune and nothing to wait for, so -c and -t are
 * refused rather than accepted and dropped: the file used to be read on no
 * channel and with no timeout, with nothing said about either. The input is
 * not even opened, since the command line is refused first. */
MU_TEST(a_replay_refuses_the_options_it_cannot_act_on)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-i", "/dev/null", "-c", "39");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err,
	    "error: -c cannot be combined with -i; a replay reads a saved"
	    " capture rather than tuning a board\n") == 0, r.err);
	mu_assert_int_eq(0, device.connect_calls);

	reset_device();
	r = RUN_RX("-i", "/dev/null", "-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err,
	    "error: -t cannot be combined with -i; a replay reads a saved"
	    " capture rather than waiting for a board\n") == 0, r.err);
	mu_assert_int_eq(0, device.connect_calls);

	/* -t 0 is "no timeout", which the value alone cannot be told from the
	 * option not being given, so it is refused too. */
	reset_device();
	r = RUN_RX("-i", "/dev/null", "-t", "0");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "-t cannot be combined with -i") != NULL, r.err);

	/* And a replay without them still runs: /dev/null is an empty capture,
	 * which reaches the end of the file and exits 0. */
	reset_device();
	r = RUN_RX("-i", "/dev/null");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, device.connect_calls);
}

/* What the setup requests carry on a capture the board took. A command line
 * without -c asks for the sweep, which the board is told by a channel above
 * the band; -c sends that channel as a frequency; -l with -u sends the address
 * too. This is the regression that matters, since it is every normal run. */
MU_TEST(a_capture_the_board_took_sets_up_and_exits_zero)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.channels_set);
	mu_assert_int_eq(9999, device.last_channel);
	mu_assert_int_eq(0, device.bdaddrs_set);
	mu_assert_int_eq(1, device.rx_syms_calls);
	mu_assert(r.err[0] == '\0', r.err);

	/* -s states that default, so it leaves the sweep in place. */
	reset_device();
	r = RUN_RX("-s", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(9999, device.last_channel);

	reset_device();
	r = RUN_RX("-c", "39", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2441, device.last_channel);
	mu_assert(r.err[0] == '\0', r.err);

	reset_device();
	r = RUN_RX("-l", "112233", "-u", "ab", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.bdaddrs_set);
	mu_assert_int_eq(1, device.channels_set);
	mu_assert_int_eq(1, device.rx_syms_calls);
	mu_assert(r.err[0] == '\0', r.err);
}

/* The order the requests go in, which is the whole of what the board keeps:
 * the modulation before the capture request, and the channel or the sweep
 * after it. Stating the channel first lost it to the reset a mode start runs
 * -- which puts the board's hop mode back to "no hopping" -- so a capture
 * started on a board a killed ubertooth-afh had left capturing sat on one
 * channel while the command line asked for the whole band. The modulation goes
 * the other way round: the mode start programs the demodulator from it, and
 * every reset that could throw it away leaves Basic Rate, which is what this
 * capture asks for. The address request is not part of either and still goes
 * first. */
MU_TEST(the_capture_is_asked_for_before_the_channel)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(device.calls, "msc") == 0, device.calls);

	reset_device();
	r = RUN_RX("-c", "39", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(device.calls, "msc") == 0, device.calls);

	reset_device();
	r = RUN_RX("-l", "112233", "-u", "ab", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(device.calls, "bmsc") == 0, device.calls);
}

/*
 * And the modulation it states is Basic Rate, the one this tool reads. It is
 * also the board's default, but an idle board holds the last modulation a tool
 * set, because a mode starting from idle runs no reset: a `ubertooth-dump -l`
 * interrupted before its capture started left the board demodulating low
 * energy, and this capture then found nothing and said nothing.
 */
MU_TEST(the_capture_states_basic_rate)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.modulations_set);
	mu_assert_int_eq(MOD_BT_BASIC_RATE, device.last_modulation);
	mu_assert(r.err[0] == '\0', r.err);
}



#ifndef TEST_REAL_API_REPORTS
/* Built against an installed libubertooth, so there is no library source to
 * compile in: the report for a request the board's firmware has not got is a
 * stub here, and STALLED below then expects no line from it. */
void ubertooth_report_unsupported_request(const char* what, int code)
{
	(void)what;
	(void)code;
}
#endif

/* The whole report for a refused request: one error line naming the request,
 * why it failed and the libusb code, and a note only where there is something
 * to try. */
#ifdef TEST_REAL_API_REPORTS
#define STALLED(what) \
	"error: could not " what \
	": the board's firmware does not support it (stalled, libusb -9)\n"
#else
#define STALLED(what) ""
#endif
#define NO_ANSWER(what) \
	"error: could not " what \
	": the board did not answer in time (libusb -7)\n" \
	"note: if a mode is running, stop it with ubertooth-util -S; " \
	"a board that does not answer at all usually resets itself within " \
	"a few seconds, so run the command again, then reset it with " \
	"ubertooth-util -r on the same board if it stays silent, and " \
	"unplug it or power-cycle its hub port if that does not reach " \
	"it either\n"

/* Each refused setup request is named, with the reason and what to try, on
 * standard error, and ends the command there, with nothing on standard output
 * and nothing captured. The channel request is named for what the command line
 * asked of it, because a run without -c never named a channel. */
MU_TEST(a_refused_setup_request_is_named_and_exits_one)
{
	static const struct {
		const char* option;  /* the extra option for this case, if any */
		const char* value;
		int refuse_channel;  /* which request the board refuses */
		int code;            /* what the request came back with */
		int channels;        /* requests made before the refusal */
		int bdaddrs;
		const char* message;
	} cases[] = {
		/* LIBUSB_ERROR_PIPE, how a board refuses a request it does not
		 * implement: there is nothing to suggest, so no note follows. */
		{ NULL, NULL, 1, -9, 1, 0, STALLED("set sweep mode") },
		{ "-c", "39", 1, -9, 1, 0, STALLED("set the channel") },
		{ "-u", "ab", 0, -9, 0, 1, STALLED("set the Bluetooth address") },
		/* LIBUSB_ERROR_TIMEOUT: the board took the request and said
		 * nothing, which the note is about. */
		{ "-c", "39", 1, -7, 1, 0, NO_ANSWER("set the channel") },
		{ "-u", "ab", 0, -7, 0, 1,
		  NO_ANSWER("set the Bluetooth address") },
		/* LIBUSB_ERROR_NO_DEVICE: the other code with advice. */
		{ NULL, NULL, 1, -4, 1, 0,
		  "error: could not set sweep mode: the board is no longer "
		  "attached (libusb -4)\n"
		  "note: re-attach the board and run the command again\n" },
		/* LIBUSB_ERROR_OVERFLOW and LIBUSB_ERROR_IO: named, no advice. */
		{ "-c", "39", 1, -8, 1, 0,
		  "error: could not set the channel: the board sent more data "
		  "than the request allows (libusb -8)\n" },
		{ "-c", "39", 1, -1, 1, 0,
		  "error: could not set the channel: the transfer failed "
		  "(libusb -1)\n" },
		/* A code with no plain words still names the request and the
		 * number, rather than printing nothing. */
		{ NULL, NULL, 1, -5, 1, 0,
		  "error: could not set sweep mode (libusb -5)\n" },
	};
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reset_device();
		if (cases[i].refuse_channel)
			device.channel_rc = cases[i].code;
		else
			device.bdaddr_rc = cases[i].code;

		if (cases[i].option != NULL)
			r = RUN_RX("-l", "112233", cases[i].option,
			           cases[i].value, "-t", "10");
		else
			r = RUN_RX("-l", "112233", "-t", "10");

		mu_assert_int_eq(1, r.rc);
		mu_assert(strcmp(r.err, cases[i].message) == 0, r.err);
		mu_assert(r.out[0] == '\0', r.out);
		mu_assert_int_eq(cases[i].channels, device.channels_set);
		mu_assert_int_eq(cases[i].bdaddrs, device.bdaddrs_set);
		/* Not one block was taken either way: the channel request is
		 * sent as soon as the capture has been asked for, before the
		 * receive loop. */
		mu_assert_int_eq(0, device.blocks);

		if (cases[i].refuse_channel) {
			/* The channel is stated after the capture starts, so the
			 * mode is running when this one is refused: it is
			 * stopped, which gives the board back, and the capture
			 * never reaches its loop. */
			mu_assert_int_eq(1, device.rx_syms_calls);
			mu_assert_int_eq(1, device.bulk_inits);
			mu_assert_int_eq(1, device.thread_starts);
			mu_assert_int_eq(1, device.thread_stops);
			mu_assert_int_eq(1, device.stops);
			mu_assert_int_eq(0, device.closes);
		} else {
			/* The address goes before any of that, so nothing has
			 * started yet and no capture is set up on top of a
			 * refused address. */
			mu_assert_int_eq(0, device.rx_syms_calls);
			mu_assert_int_eq(0, device.bulk_inits);
			/* The stop handlers are in all the same: they go in
			 * before the board is opened, so a Ctrl-C during the
			 * opening is not discarded. */
			mu_assert_int_eq(1, device.cleanup_handlers);
			/* Given back, and without a STOP: no mode had started,
			 * and a STOP to a board that is not answering would add
			 * a second failure to the report. */
			mu_assert_int_eq(1, device.closes);
			mu_assert_int_eq(0, device.stops);
		}
	}

	/* A run that needs both requests sends the address, asks for the
	 * capture, and states the channel last. */
	reset_device();
	device.channel_rc = -9;
	r = RUN_RX("-l", "112233", "-u", "ab", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(device.calls, "bmsc") == 0, device.calls);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, STALLED("set sweep mode")) == 0, r.err);
}

/*
 * A refused modulation ends the run the same way: a capture that went ahead
 * would demodulate whatever the board had been left in, and nothing in the
 * packets says which that was. The first of the two is refused, which is
 * before the capture has been asked for, so no mode starts at all.
 */
MU_TEST(a_refused_modulation_ends_the_capture)
{
	struct run_result r;

	reset_device();
	device.modulation_rc = -9;
	r = RUN_RX("-t", "10");

	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, STALLED("set the modulation")) == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(device.calls, "m") == 0, device.calls);
	mu_assert_int_eq(0, device.blocks);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(1, device.stops);
}

/* A board that took only part of the address is a failed request too, and one
 * libubertooth reports in full itself, counts and all. The capture used to
 * start on top of it and exit 0, because the result was positive rather than
 * negative. The tool adds no line of its own here. */
MU_TEST(a_truncated_address_request_ends_the_run_too)
{
	struct run_result r;

	reset_device();
	device.bdaddr_rc = UBERTOOTH_ERROR_SHORT_REPLY;
	r = RUN_RX("-l", "112233", "-u", "ab", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.bdaddrs_set);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(1, device.closes);
	mu_assert_int_eq(0, device.stops);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(r.err[0] == '\0', r.err);
}

/* Survey mode needs the sweep most of all, so a refused one keeps it from
 * printing a result list it never gathered. */
MU_TEST(a_refused_sweep_leaves_survey_mode_with_no_results)
{
	struct run_result r;

	reset_device();
	device.channel_rc = -9;
	r = RUN_RX("-z", "-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out); /* no address list */
	/* The whole of standard error is the one stall line, so the
	 * "Survey Results" heading was not printed either. */
	mu_assert(strcmp(r.err, STALLED("set sweep mode")) == 0, r.err);
}

/* A firmware this host will not drive ends the command before any setup
 * request, with the board released whichever way the check failed. The tool
 * closes it either way: the library has already released a board whose
 * version it refused, and closing a board that is already released does
 * nothing. */
MU_TEST(a_firmware_the_tool_cannot_use_exits_one)
{
	struct run_result r;

	reset_device();
	device.api_rc = -1;
	device.api_released = 1;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.rx_syms_calls);
	/* As above: the handlers precede the open. */
	mu_assert_int_eq(1, device.cleanup_handlers);
	mu_assert_int_eq(1, device.stops);
	mu_assert_int_eq(1, device.closes); /* harmless: it is already released */

	reset_device();
	device.api_rc = -1;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(1, device.cleanup_handlers);
	mu_assert_int_eq(0, device.stops);
	mu_assert_int_eq(1, device.closes); /* the board is not left claimed */
}

/* A piconet that cannot be allocated ends the run with its reason, rather
 * than being dereferenced on the next line: the tool used to crash, and a
 * crash says nothing about why. */
MU_TEST(a_piconet_that_cannot_be_allocated_exits_one)
{
	struct run_result r;

	reset_device();
	device.no_memory = 1;

	r = RUN_RX("-l", "446688", "-t", "10");

	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: out of memory\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	/* No capture was asked for, and the board is given back once. */
	mu_assert_int_eq(0, device.bulk_inits);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(1, device.stops);
	mu_assert_int_eq(1, device.frees);
}

/* The three failures that sit between the board being accepted and the capture
 * being asked for leave no file behind either: libbtbb refusing its own setup,
 * a piconet that cannot be allocated, and a board that refuses the address
 * request. Each reports itself and exits 1, and each used to do it with the
 * capture file already created, so the run left a header-only file the user
 * had to tell apart from a capture that caught nothing. Every one of the three
 * file options is checked against every one of them, because each file is
 * opened by a writer of its own. */
MU_TEST(a_run_that_fails_before_the_capture_creates_no_file)
{
	enum { LIBBTBB_REFUSES, NO_MEMORY, ADDRESS_REFUSED };
	static const struct {
		/* How the board goes back: with a STOP for a mode an earlier
		 * command may have left running, or, where the board has just
		 * failed to answer, without one. */
		int stops;
		int closes;
		const char* message;  /* what this failure must say, if ours */
	} failures[] = {
		/* libbtbb's own line, not the tool's. */
		{ 1, 0, NULL },
		{ 1, 0, "error: out of memory\n" },
		{ 0, 1, NO_ANSWER("set the Bluetooth address") }
	};
	static const char* const options[] = { "-q", "-r", "-d" };
	const int default_max_ac_errors = max_ac_errors;
	struct run_result r;
	struct stat info;
	char path[64];
	size_t i, which;

	for (which = 0; which < sizeof(failures) / sizeof(failures[0]); which++) {
		for (i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
			int fd;

			reset_device();
			if (which == LIBBTBB_REFUSES)
				/* Outside libbtbb's own range, which is all it
				 * refuses. The tool's -e check keeps a command
				 * line from reaching this, so the global the
				 * tool passes is set here instead. */
				max_ac_errors = MAX_AC_ERRORS + 1;
			else if (which == NO_MEMORY)
				device.no_memory = 1;
			else
				/* LIBUSB_ERROR_TIMEOUT: the board took the
				 * request and said nothing. Not the stalled
				 * code, whose report comes from libubertooth
				 * and is a stub in the build against an
				 * installed library (see STALLED above). */
				device.bdaddr_rc = LIBUSB_ERROR_TIMEOUT;

			snprintf(path, sizeof(path), "/tmp/rx-order-XXXXXX");
			fd = mkstemp(path);
			mu_assert_fatal(fd >= 0, "temp name taken");
			close(fd);
			unlink(path);

			/* -u is given throughout: the first two failures happen
			 * before the address request is reached, so it changes
			 * nothing for them. */
			r = RUN_RX("-l", "446688", "-u", "ab",
			           options[i], path, "-t", "10");
			max_ac_errors = default_max_ac_errors;

			mu_assert_int_eq(1, r.rc);
			/* Nothing on disk under the name the user gave. */
			mu_assert(stat(path, &info) != 0, path);
			/* The failure said why, on standard error, and the
			 * capture never started. */
			if (failures[which].message != NULL)
				mu_assert(strcmp(r.err,
				          failures[which].message) == 0, r.err);
			else
				mu_assert(r.err[0] != '\0',
				          "a reason on standard error");
			mu_assert(r.out[0] == '\0', r.out);
			mu_assert_int_eq(0, device.rx_syms_calls);
			mu_assert_int_eq(failures[which].stops, device.stops);
			mu_assert_int_eq(failures[which].closes, device.closes);
			mu_assert_int_eq(1, device.frees);
			unlink(path);
		}
	}
}

/* Three unused capture-file names, for the cases below that ask one run for
 * all three: each writer opens a file of its own, so each has to be looked
 * for by name. */
static void capture_names(const char* prefix, char* pcap, char* pcapng,
                          char* dump, size_t size)
{
	char base[64];
	int fd;

	snprintf(base, sizeof(base), "/tmp/%s-XXXXXX", prefix);
	fd = mkstemp(base);
	mu_assert_fatal(fd >= 0, "temp name taken");
	close(fd);
	unlink(base);
	snprintf(pcap, size, "%s.pcap", base);
	snprintf(pcapng, size, "%s.pcapng", base);
	snprintf(dump, size, "%s.dump", base);
}

/* The failures that cannot be moved above the capture files, because they are
 * the capture itself starting: the three requests it is built on, and the two
 * steps that set the USB transfer up. Each of them left the created file
 * behind, header and nothing else, for the user to tell apart from a capture
 * that caught nothing. One file option is enough here, since the removal is
 * decided per writer; the case below asks for all three at once. */
MU_TEST(a_run_refused_after_the_files_exist_removes_them)
{
	enum { TRANSFER, THREAD, MODULATION, CAPTURE, SWEEP };
	static const struct {
		const char* message;  /* what the run must say, if the tool's own */
	} failures[] = {
		/* libubertooth's own line for the transfer, and the tool's for
		 * the thread it would have been completed by. */
		{ NULL },
		{ NULL },
		{ NO_ANSWER("set the modulation") },
		{ NO_ANSWER("start the capture") },
		/* No -c, so the request asked for the sweep. */
		{ NO_ANSWER("set sweep mode") }
	};
	struct run_result r;
	struct stat info;
	char path[64];
	size_t which;

	for (which = 0; which < sizeof(failures) / sizeof(failures[0]); which++) {
		int fd;

		reset_device();
		switch (which) {
		case TRANSFER:
			device.bulk_init_rc = -1;
			break;
		case THREAD:
			/* pthread_create()'s own errno, which is positive
			 * where every libusb code is not. */
			device.thread_start_rc = EAGAIN;
			break;
		case MODULATION:
			device.modulation_rc = LIBUSB_ERROR_TIMEOUT;
			break;
		case CAPTURE:
			device.rx_syms_rc = LIBUSB_ERROR_TIMEOUT;
			break;
		default:
			device.channel_rc = LIBUSB_ERROR_TIMEOUT;
			break;
		}

		snprintf(path, sizeof(path), "/tmp/rx-discard-XXXXXX");
		fd = mkstemp(path);
		mu_assert_fatal(fd >= 0, "temp name taken");
		close(fd);
		unlink(path);

		r = RUN_RX("-q", path, "-t", "10");

		mu_assert_int_eq(1, r.rc);
		/* Nothing left under the name the user gave. */
		mu_assert(stat(path, &info) != 0, path);
		/* The run said why, and said it only once: a file this run
		 * took back is not worth a line of its own. */
		if (failures[which].message != NULL)
			mu_assert(strcmp(r.err, failures[which].message) == 0,
			          r.err);
		else
			mu_assert(strstr(r.err, "warning: cannot remove")
			          == NULL, r.err);
		mu_assert(r.out[0] == '\0', r.out);
		mu_assert_int_eq(0, device.blocks);
		mu_assert_int_eq(1, device.frees);
		unlink(path);
	}
}

/* All three writers at once, since each file is opened by a writer of its own
 * and each is decided by a handle of its own: one refused run asked for every
 * file option must leave none of the three behind. */
MU_TEST(a_refused_run_removes_every_file_it_was_asked_for)
{
	struct run_result r;
	struct stat info;
	char pcap[80], pcapng[80], dump[80];

	reset_device();
	device.rx_syms_rc = LIBUSB_ERROR_TIMEOUT;
	capture_names("rx-discard3", pcap, pcapng, dump, sizeof(pcap));

	r = RUN_RX("-q", pcap, "-r", pcapng, "-d", dump, "-t", "10");

	mu_assert_int_eq(1, r.rc);
	mu_assert(stat(pcap, &info) != 0, pcap);
	mu_assert(stat(pcapng, &info) != 0, pcapng);
	mu_assert(stat(dump, &info) != 0, dump);
	mu_assert(strcmp(r.err, NO_ANSWER("start the capture")) == 0, r.err);
	unlink(pcap);
	unlink(pcapng);
	unlink(dump);
}

/* The other half of it: a capture that ran keeps its file, however little
 * arrived. A capture that caught nothing is a result -- the band was quiet --
 * and the file holds the header that says so, so a run must not take back a
 * file merely because no packet was decoded. The three writers are checked
 * together, as one run can be asked for all three. */
MU_TEST(a_capture_that_ran_keeps_its_files)
{
	struct run_result r;
	struct stat info;
	char pcap[80], pcapng[80], dump[80];

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 2;
	capture_names("rx-kept", pcap, pcapng, dump, sizeof(pcap));

	r = RUN_RX("-q", pcap, "-r", pcapng, "-d", dump, "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	/* Each file is there, and holds what its writer puts at the front of
	 * it: an empty file would be the bug this run must not cause. */
	mu_assert(stat(pcap, &info) == 0, pcap);
	mu_assert(info.st_size > 0, "the PCAP file holds its header");
	mu_assert(stat(pcapng, &info) == 0, pcapng);
	mu_assert(info.st_size > 0, "the PcapNG file holds its header");
	mu_assert(stat(dump, &info) == 0, dump);
	unlink(pcap);
	unlink(pcapng);
	unlink(dump);
}

/* A capture file the user already had is not this run's to remove: only a
 * regular file this run created goes, and a name that is something else --
 * /dev/null, a pipe, a symbolic link -- wrote through what the user set up,
 * so it stays. Checked on the link, because a link is the case where a
 * refused run would otherwise delete the user's own name for a file. */
MU_TEST(a_refused_run_leaves_a_link_and_dev_null_alone)
{
	struct run_result r;
	struct stat info;
	char target[64], link[80];
	int fd;

	reset_device();
	device.rx_syms_rc = LIBUSB_ERROR_TIMEOUT;

	snprintf(target, sizeof(target), "/tmp/rx-link-XXXXXX");
	fd = mkstemp(target);
	mu_assert_fatal(fd >= 0, "temp name taken");
	close(fd);
	unlink(target);
	snprintf(link, sizeof(link), "%s.link", target);
	mu_assert_fatal(symlink(target, link) == 0, "cannot make the link");

	r = RUN_RX("-q", link, "-t", "10");

	mu_assert_int_eq(1, r.rc);
	/* The link is still there, and so is the file it names. */
	mu_assert(lstat(link, &info) == 0, link);
	mu_assert(stat(target, &info) == 0, target);
	mu_assert(strstr(r.err, "warning: cannot remove") == NULL, r.err);
	unlink(link);
	unlink(target);

	/* The same for a character device the user named. */
	reset_device();
	device.rx_syms_rc = LIBUSB_ERROR_TIMEOUT;
	r = RUN_RX("-q", "/dev/null", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(stat("/dev/null", &info) == 0, "/dev/null is still there");
	mu_assert(strstr(r.err, "warning: cannot remove") == NULL, r.err);
}

/* A replay whose input yielded not one whole record is the same case on the
 * replay path: the files hold their headers and nothing else, and used to be
 * left behind for the user to tell apart from a replay that decoded nothing.
 * Both ways an input can fail are checked -- a read error, and an input that
 * ends partway through its first record -- since the tool decides on the
 * records read, not on which way the read ended. */
MU_TEST(a_replay_that_read_no_record_removes_its_files)
{
	static const int results[] = {
		-EIO,  /* a read error, which stream_rx_file() returns negated */
		10     /* the bytes of a first record that was cut short */
	};
	struct run_result r;
	struct stat info;
	char pcap[80], pcapng[80], dump[80];
	size_t which;

	for (which = 0; which < sizeof(results) / sizeof(results[0]); which++) {
		reset_device();
		device.replay_result = results[which];
		capture_names("rx-replay-gone", pcap, pcapng, dump,
		              sizeof(pcap));

		r = RUN_RX("-i", "/dev/null", "-q", pcap, "-r", pcapng,
		           "-d", dump);

		mu_assert_int_eq(1, r.rc);
		mu_assert(stat(pcap, &info) != 0, pcap);
		mu_assert(stat(pcapng, &info) != 0, pcapng);
		mu_assert(stat(dump, &info) != 0, dump);
		/* The reason is libubertooth's own line, and a file this run
		 * took back is not worth one of its own. */
		mu_assert(strstr(r.err, "warning: cannot remove") == NULL,
		          r.err);
		unlink(pcap);
		unlink(pcapng);
		unlink(dump);
	}
}

/* The other half of it, as for a capture: an input that ended badly after
 * some of it had been read leaves files holding what was decoded, so they
 * stay even though the run exits 1. */
MU_TEST(a_replay_that_read_records_keeps_its_files)
{
	struct run_result r;
	struct stat info;
	char pcap[80], pcapng[80], dump[80];

	reset_device();
	device.replay_result = -EIO;
	device.replay_records = 3;
	capture_names("rx-replay-kept", pcap, pcapng, dump, sizeof(pcap));

	r = RUN_RX("-i", "/dev/null", "-q", pcap, "-r", pcapng, "-d", dump);

	mu_assert_int_eq(1, r.rc);
	mu_assert(stat(pcap, &info) == 0, pcap);
	mu_assert(info.st_size > 0, "the PCAP file holds its header");
	mu_assert(stat(pcapng, &info) == 0, pcapng);
	mu_assert(info.st_size > 0, "the PcapNG file holds its header");
	mu_assert(stat(dump, &info) == 0, dump);
	unlink(pcap);
	unlink(pcapng);
	unlink(dump);
}

/* An input read to its end is no failure, even when there was nothing in it to
 * read: that replay ran and decoded nothing, which is a result of its own, and
 * the files say so. This is the case right next to the removal above, and the
 * one a change to its condition would take with it. */
MU_TEST(a_replay_of_an_empty_input_keeps_its_files)
{
	struct run_result r;
	struct stat info;
	char pcap[80], pcapng[80], dump[80];

	/* The scripted read ends at once, with no record and no error. */
	reset_device();
	capture_names("rx-replay-empty", pcap, pcapng, dump, sizeof(pcap));

	r = RUN_RX("-i", "/dev/null", "-q", pcap, "-r", pcapng, "-d", dump);

	mu_assert_int_eq(0, r.rc);
	mu_assert(stat(pcap, &info) == 0, pcap);
	mu_assert(stat(pcapng, &info) == 0, pcapng);
	mu_assert(stat(dump, &info) == 0, dump);
	unlink(pcap);
	unlink(pcapng);
	unlink(dump);
}

/* An input that cannot be opened at all is refused before the capture files
 * are created, so there is nothing to remove and nothing is left behind. */
MU_TEST(a_replay_whose_input_cannot_be_opened_creates_no_file)
{
	struct run_result r;
	struct stat info;
	char pcap[80], pcapng[80], dump[80];
	/* Room for the longest name capture_names() can build plus the suffix,
	 * so the compiler can see that the write below cannot be truncated. */
	char missing[sizeof(pcap) + sizeof(".input")];

	reset_device();
	capture_names("rx-replay-missing", pcap, pcapng, dump, sizeof(pcap));
	snprintf(missing, sizeof(missing), "%s.input", pcap);

	r = RUN_RX("-i", missing, "-q", pcap, "-r", pcapng, "-d", dump);

	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, missing) != NULL, r.err);
	mu_assert(stat(pcap, &info) != 0, pcap);
	mu_assert(stat(pcapng, &info) != 0, pcapng);
	mu_assert(stat(dump, &info) != 0, dump);
}

/* A capture that cannot be started at all exits 1 with one "error: " line on
 * standard error and nothing on standard output. Each of these used to hand
 * the failing call's own return value to the exit status, which a shell wraps
 * into a byte: a script read -1 as 255 and the libusb codes as 247 and 249. */
MU_TEST(a_capture_that_cannot_start_exits_one)
{
	char expected[128];
	struct run_result r;

	/* The receive transfer libusb would not take. */
	reset_device();
	device.bulk_init_rc = -1;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: submitting the receive transfer failed "
	          "(-4)\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	/* Nothing was started on top of it: no poll thread, no mode request, no
	 * channel, no block. The board is given back. */
	mu_assert_int_eq(0, device.thread_starts);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.blocks);
	mu_assert_int_eq(1, device.stops);

	/* The thread that collects the capture. pthread_create() reports a
	 * failure as a positive errno, which a "< 0" test let through: the
	 * capture then had nobody to complete its transfers and waited forever
	 * with nothing arriving. */
	reset_device();
	device.thread_start_rc = EAGAIN;
	r = RUN_RX("-t", "10");
	snprintf(expected, sizeof(expected),
	         "error: could not start the thread that collects the capture:"
	         " %s\n", strerror(EAGAIN));
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, expected) == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(1, device.thread_starts);
	mu_assert_int_eq(0, device.rx_syms_calls);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.blocks);
	mu_assert_int_eq(1, device.stops);

	/* The request that starts the mode, named the way every other refused
	 * setup request is. The poll thread is running by then, so it is joined
	 * on the way out. */
	reset_device();
	device.rx_syms_rc = LIBUSB_ERROR_TIMEOUT;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, NO_ANSWER("start the capture")) == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(1, device.rx_syms_calls);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.blocks);
	mu_assert_int_eq(1, device.thread_stops);
	mu_assert_int_eq(1, device.stops);

	/* A code with no advice: still the one "error: " line that names the
	 * request, never libubertooth's shared sink, which names none. */
	reset_device();
	device.rx_syms_rc = LIBUSB_ERROR_PIPE;
	r = RUN_RX("-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, STALLED("start the capture")) == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(1, device.rx_syms_calls);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.blocks);
	mu_assert_int_eq(1, device.thread_stops);
	mu_assert_int_eq(1, device.stops);

	/* A survey prints no result list for a capture that never started. */
	reset_device();
	device.bulk_init_rc = -1;
	r = RUN_RX("-z", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
}

/* A replay opens no device, so it sends no setup request at all and a board
 * that would refuse one cannot affect it. */
MU_TEST(a_replay_sends_no_setup_request)
{
	struct run_result r;

	reset_device();
	device.channel_rc = -9;
	device.bdaddr_rc = -9;
	r = RUN_RX("-i", "/dev/null", "-l", "112233", "-u", "ab");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, device.channels_set);
	mu_assert_int_eq(0, device.bdaddrs_set);
	mu_assert_int_eq(0, device.connect_calls);
	mu_assert(r.err[0] == '\0', r.err);
}

/* The handle and its 72 MB fifo go back on every exit once it exists. They
 * were left to process exit, and were allocated before the command line had
 * been read, so even a refused command took 72 MB. */
MU_TEST(every_exit_path_gives_the_handle_back)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-t", "30");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.frees);

	/* A firmware version the host will not drive, both ways round: the
	 * library releases a board whose version it refused, and closing a
	 * board already released does nothing. */
	reset_device();
	device.api_rc = -1;
	device.api_released = 1;
	r = RUN_RX("-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.frees);

	reset_device();
	device.api_rc = -1;
	r = RUN_RX("-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.frees);

	/* A capture that could not be set up. */
	reset_device();
	device.bulk_init_rc = -1;
	r = RUN_RX("-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.frees);

	/* No board matches -U: the handle exists by then and still goes back. */
	reset_device();
	device.connect_fails = 1;
	r = RUN_RX("-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.frees);

	/* A refused command line never allocates one. */
	reset_device();
	r = RUN_RX("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, device.frees);
}

/*
 * Both ways out report what the capture is missing: a live capture after its
 * teardown, and a replay after the file has been read. The replay path had the
 * call missing, so a recorded capture's gaps were warned about record by
 * record and then never totalled.
 */
MU_TEST(a_live_capture_and_a_replay_both_report_their_gaps)
{
	struct run_result r;

	reset_device();
	r = RUN_RX("-t", "1");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.gap_reports);

	reset_device();
	r = RUN_RX("-i", "/dev/null");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.gap_reports);
}

/* ---- a follow that has gone quiet --------------------------------------- */

/* A clock that is not the piconet's gets only the channel to listen on wrong,
 * so it still checks out every header it receives and is simply heard from
 * about one packet in eighteen. Nothing in the packets that do arrive reports
 * it, which is why the tool times the silence: past the wait, the clock is
 * given up. */
MU_TEST(a_follow_that_hears_nothing_gives_the_clock_up_once)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	/* The library has handed this capture a clock, and nothing of the
	 * piconet has been heard on it since. */
	device.block_ms = 10000;
	device.blocks_to_end = 20;
	device.follow_quiet_ms = 0;

	r = RUN_RX("-l", "112233", "-u", "22", "-t", "60");

	mu_assert_int_eq(0, r.rc);
	/* Once over the two hundred seconds, not once per record: giving the clock
	 * up leaves the capture following nothing, and the wait starts again
	 * only when the library hands another clock over. */
	mu_assert_int_eq(1, device.drops);
}

/* And the wait is a minute, pinned either side of the moment it is up: a
 * follow whose clock was right is dropped if this fires early, and that clock
 * is then ruled out for the rest of the run, so the value is worth a test of
 * its own. */
MU_TEST(the_wait_is_a_minute)
{
	struct run_result r;

	/* Two blocks, the first of which takes a millisecond less than the
	 * wait. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.block_ms = 59999;
	device.blocks_to_end = 2;
	device.follow_quiet_ms = 0;

	r = RUN_RX("-l", "112233", "-u", "22", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, device.drops);

	/* And one that takes exactly the wait. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.block_ms = 60000;
	device.blocks_to_end = 2;
	device.follow_quiet_ms = 0;

	r = RUN_RX("-l", "112233", "-u", "22", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.drops);
}

/* A follow that is hearing its piconet keeps its clock however long it runs:
 * the wait starts again at every packet. Nor is there anything to give up
 * where no clock is being followed, which is every capture that has not got
 * one yet. */
MU_TEST(a_working_follow_and_a_capture_with_no_clock_keep_going)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.block_ms = 10000;
	device.blocks_to_end = 30;
	device.follow_quiet_ms = 0;
	device.hear_per_block = 1;

	r = RUN_RX("-l", "112233", "-u", "22", "-t", "60");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, device.drops);

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.block_ms = 10000;
	device.blocks_to_end = 30;

	r = RUN_RX("-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, device.drops);
}

/* A follow that stays quiet -- a clock the library will not give up, or a
 * board that will not come off one it has -- is asked about once per wait and
 * not once per record, so five minutes at ten records a second is four asks
 * and not three thousand. */
MU_TEST(a_follow_that_stays_quiet_is_asked_about_once_per_wait)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.block_ms = 100;
	device.blocks_to_end = 3000;   /* five minutes */
	device.follow_quiet_ms = 0;
	device.drop_result = 0;

	r = RUN_RX("-l", "112233", "-u", "22", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(4, device.drops);
}

/* ---- a clock search that is not going to finish ------------------------- */

/* The two lines the tool says about it, each as one literal, so a reworded
 * message or a changed span fails here and in the man page check with it. */
static const char SEARCH_NOTE[] = "note: the clock search has run for 30 s without confirming a clock; it does not confirm one against a piconet that uses adapted hopping (AFH)\n";
static const char SEARCH_NOTE_REACH[] = "note: the piconet's LAP and UAP, and the packets of it the boards hear, are still recovered, decoded, and captured; see ubertooth-rx(1)\n";

/* How many times a line appears in a captured stream. */
static int lines_like(const char* stream, const char* line)
{
	int found = 0;
	const char* at = stream;

	while ((at = strstr(at, line)) != NULL) {
		found++;
		at += strlen(line);
	}
	return found;
}

/* A search against an adapted-hopping piconet rules its candidates out and
 * starts over, printing a count each time and never confirming a clock, so
 * nothing it prints says the run is not going to reach one. The tool says so
 * itself, once, however long the capture runs after that. */
MU_TEST(a_search_that_does_not_confirm_a_clock_says_so_once)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	/* Ten minutes of a search that confirms nothing. */
	device.block_ms = 10000;
	device.blocks_to_end = 60;
	device.clock_search_ms = 0;

	r = RUN_RX("-l", "112233", "-u", "22", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, lines_like(r.err, SEARCH_NOTE));
	mu_assert_int_eq(1, lines_like(r.err, SEARCH_NOTE_REACH));
	/* On standard error with the rest of the notes: a capture that has
	 * taken standard output carries only the capture. */
	mu_assert_int_eq(0, lines_like(r.out, SEARCH_NOTE));
}

/* And the span is half a minute, pinned either side of the moment it is up.
 * Saying it early would tell a user that a search still working is not going
 * to finish, so the value is worth a case of its own. */
MU_TEST(the_search_note_waits_half_a_minute)
{
	struct run_result r;

	/* Two blocks, the first of which takes a millisecond less than the
	 * span. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.block_ms = 29999;
	device.blocks_to_end = 2;
	device.clock_search_ms = 0;

	r = RUN_RX("-l", "112233", "-u", "22", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, lines_like(r.err, SEARCH_NOTE));

	/* And one that takes exactly the span. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.block_ms = 30000;
	device.blocks_to_end = 2;
	device.clock_search_ms = 0;

	r = RUN_RX("-l", "112233", "-u", "22", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, lines_like(r.err, SEARCH_NOTE));
}

/* A run with no search going says nothing, however long it runs: a capture
 * that has heard nothing of the piconet, one that has heard too little of it
 * to settle CLK1-6, and one whose clock was accepted are all results of their
 * own, and none of them is a search that cannot finish. */
MU_TEST(a_run_with_no_search_running_says_nothing)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.block_ms = 60000;
	device.blocks_to_end = 20;

	r = RUN_RX("-l", "112233", "-u", "22", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, lines_like(r.err, SEARCH_NOTE));

	/* And a survey, which recovers addresses and follows nothing. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.block_ms = 60000;
	device.blocks_to_end = 20;

	r = RUN_RX("-z", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, lines_like(r.err, SEARCH_NOTE));
}

/* A replay of a recorded capture says it too: the search runs over those
 * records for as long as it did live, and the reader of a capture file is owed
 * the same answer as the user of a board. The span it reports is the one the
 * search has really had, not the half minute that brought it here, which a
 * replay is where the two come apart. */
MU_TEST(a_replay_whose_search_does_not_confirm_a_clock_says_so)
{
	struct run_result r;

	reset_device();
	device.replay_records = 4;
	device.clock_search_ms = 300000;

	r = RUN_RX("-i", "/dev/null", "-l", "112233", "-u", "22");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, lines_like(r.err, SEARCH_NOTE_REACH));
	mu_assert(strstr(r.err, "the clock search has run for 300 s without "
	                        "confirming a clock") != NULL,
	          "the note did not report the span the search has had");
	mu_assert_int_eq(0, lines_like(r.err, SEARCH_NOTE));
}

/* ---- several boards at once --------------------------------------------- */

/* -U takes a list, and every board on it is opened, set up and read. One
 * Ubertooth hears one 1 MHz channel at a time, so a capture spread over
 * several of them sees that many times more of a connection. */
MU_TEST(a_list_of_boards_opens_and_sets_up_each_one)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 9;

	r = RUN_RX("-U", "0,1,2", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	/* Opened in the order they were named. */
	mu_assert(strcmp(device.selectors, "0,1,2") == 0, device.selectors);
	mu_assert_int_eq(3, device.connect_calls);
	mu_assert_int_eq(3, device.check_api_calls);
	/* A receive transfer, a modulation, a capture and a channel each. */
	mu_assert_int_eq(3, device.bulk_inits);
	mu_assert_int_eq(3, device.modulations_set);
	mu_assert_int_eq(3, device.rx_syms_calls);
	mu_assert_int_eq(3, device.channels_set);
	/* One poll thread for the run, not one per board: it completes the
	 * transfers of every board on the libusb context. */
	mu_assert_int_eq(1, device.thread_starts);
	mu_assert_int_eq(1, device.thread_stops);
	/* Every board is stopped and given back. */
	mu_assert_int_eq(3, device.stops);
	mu_assert_int_eq(3, device.frees);
	/* One timer for the run. */
	mu_assert_int_eq(10, device.timeout_set);
}

/* -c takes a list too, one channel per board, which is how the boards are
 * spread across the band. A single channel is given to all of them. */
MU_TEST(a_channel_each_spreads_the_boards_across_the_band)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;

	r = RUN_RX("-U", "0,1,2", "-c", "20,40,60", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(3, device.channels_set);
	mu_assert_int_eq(2402 + 20, device.board_channel[0]);
	mu_assert_int_eq(2402 + 40, device.board_channel[1]);
	mu_assert_int_eq(2402 + 60, device.board_channel[2]);

	/* One channel for all of them. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;

	r = RUN_RX("-U", "0,1,2", "-c", "39", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2402 + 39, device.board_channel[0]);
	mu_assert_int_eq(2402 + 39, device.board_channel[1]);
	mu_assert_int_eq(2402 + 39, device.board_channel[2]);

	/* And no -c at all leaves every board sweeping, as one board does. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;

	r = RUN_RX("-U", "0,1,2", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(9999, device.board_channel[0]);
	mu_assert_int_eq(9999, device.board_channel[2]);
}

/* A board that cannot be opened ends the run, and every handle the run had
 * allocated is given back: a capture spread over fewer boards than were asked
 * for would leave channels unheard with nothing saying which. */
MU_TEST(a_board_that_cannot_be_opened_gives_every_handle_back)
{
	struct run_result r;

	reset_device();
	device.connect_fails = 1;

	r = RUN_RX("-U", "0,1,2", "-t", "10");

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, device.connect_calls);
	mu_assert_int_eq(0, device.rx_syms_calls);
	/* Three handles were allocated before any board was opened, and all
	 * three go back. */
	mu_assert_int_eq(3, device.frees);
}

/* What the boards share. Each keeps a piconet of its own, because libbtbb
 * anchors clock recovery on the first packet's clock and two boards' clocks
 * are unrelated; what passes between them is the channels the piconet has
 * been heard on and its upper address part, neither of which needs a common
 * clock. So a board ends up narrowing its own clock against a channel map
 * that filled three times faster, and is handed an address part two other
 * boards agree on. */
MU_TEST(boards_share_the_channel_map_and_the_address_part)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	/* Three passes over three boards: the first fills each board's own
	 * channel, the merge follows, and the next passes read back what each
	 * board was given. */
	device.blocks_to_end = 9;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	device.heard_channel[2] = 60;
	/* The first two boards work the address part out from their own
	 * packets and agree, which is what lets it be passed on. */
	device.heard_uap_board[0] = 1;
	device.heard_uap_board[1] = 1;
	device.heard_uap = 0x22;

	r = RUN_RX("-U", "0,1,2", "-c", "20,40,60", "-l", "112233", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	/* Every board holds all three channels, though each heard one. */
	mu_assert_int_eq(3, device.seen_channels[0]);
	mu_assert_int_eq(3, device.seen_channels[1]);
	mu_assert_int_eq(3, device.seen_channels[2]);
	/* And the third board was handed the one the other two agreed on. */
	mu_assert_int_eq(0x22, device.seen_uap[2]);

	/* The run says what they found between them, on standard error, so a
	 * capture writing to standard output is unaffected. Channel 20 is bit
	 * 4 of byte 2, channel 40 bit 0 of byte 5, and channel 60 bit 4 of
	 * byte 7. */
	mu_assert(strstr(r.err, "note: 3 boards: UAP = 0x22") != NULL, r.err);
	mu_assert(strstr(r.err,
	          "note: 3 boards: AFH map: 0x00001000000100100000, "
	          "3 of 79 channels heard") != NULL, r.err);
	mu_assert(strstr(r.out, "note:") == NULL, r.out);
}

/* One board's address part is not passed on: it takes two boards working out
 * the same value from their own packets. A bench run settled on 0xdb where the
 * peer's was 0x0d, and rx_share_findings() has why a wrong one cannot be
 * undone. */
MU_TEST(one_board_s_address_part_is_not_passed_on)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 9;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	device.heard_channel[2] = 60;
	/* Only the second board settles one. */
	device.heard_uap_board[1] = 1;
	device.heard_uap = 0x22;

	r = RUN_RX("-U", "0,1,2", "-c", "20,40,60", "-l", "112233", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	/* The channel map is still shared: it needs no agreeing. */
	mu_assert_int_eq(3, device.seen_channels[0]);
	mu_assert_int_eq(3, device.seen_channels[2]);
	/* The other two boards were handed nothing. */
	mu_assert_int_eq(-1, device.seen_uap[0]);
	mu_assert_int_eq(-1, device.seen_uap[2]);
	/* And the run reports the one value any board holds. */
	mu_assert(strstr(r.err, "note: 3 boards: UAP = 0x22") != NULL, r.err);
}

/* Two boards that worked out different address parts share neither, and the
 * run says so rather than reporting one of them as the piconet's: one is
 * wrong, and which is not something the run can tell. */
MU_TEST(boards_that_disagree_about_the_address_part_say_so)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 6;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	device.heard_uap_board[0] = 1;
	device.heard_uap_board[1] = 1;
	/* Each board settles on its own value: the second board's script
	 * answer differs, which is what two boards disagreeing looks like. */
	device.heard_uap = 0x22;
	device.heard_uap_other = 0x33;

	r = RUN_RX("-U", "0,1", "-c", "20,40", "-l", "112233", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.err, "note: 2 boards: the boards worked out "
	          "different UAPs, so one of them is wrong: board 1 0x22, "
	          "board 2 0x33") != NULL, r.err);
	mu_assert(strstr(r.err, "UAP = 0x") == NULL, r.err);
	/* Nothing was passed to anyone, so nothing says it was. */
	mu_assert(strstr(r.err, "was passed to a board") == NULL, r.err);
	/* Neither was passed on: each board holds what it worked out. */
	mu_assert_int_eq(0x22, device.seen_uap[0]);
	mu_assert_int_eq(0x33, device.seen_uap[1]);
}

/* One board shares nothing and says nothing extra: its piconet already holds
 * everything it knows, and libbtbb reports what it finds as it goes. */
MU_TEST(one_board_is_left_exactly_as_it_was)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;
	device.heard_channel[0] = 20;

	r = RUN_RX("-l", "112233", "-c", "20", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, device.connect_calls);
	mu_assert(strstr(r.err, "boards") == NULL, r.err);
	/* The one channel it heard, and nothing merged into it. */
	mu_assert_int_eq(1, device.seen_channels[0]);
}

/* One board's receive stream failing ends the whole capture and exits 1: the
 * channels that board was covering go unheard from there on, so the run no
 * longer holds what was asked for. */
MU_TEST(a_board_whose_stream_died_ends_the_whole_capture)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_STREAM_FAILURE;
	device.blocks_to_end = 4;

	r = RUN_RX("-U", "0,1,2", "-t", "30");

	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: receive transfer failed") != NULL, r.err);
	/* It stopped at the end of the pass the failure fell in -- the fourth
	 * block of nine, so six -- rather than running the -t out. The pass is
	 * finished rather than abandoned: the other boards have packets of
	 * their own already in hand, and taking them costs one more look at
	 * each of at most eight ring buffers. */
	mu_assert_int_eq(6, device.blocks);
	mu_assert_int_eq(3, device.stops);
}

/* The command lines the lists refuse. Each says what is wrong in one line on
 * standard error, leaves standard output empty, exits 1, and opens no board:
 * these are problems with the command line, so they are caught before anything
 * is claimed. */
MU_TEST(a_board_or_channel_list_the_tool_will_not_take_is_refused)
{
	struct run_result r;

	/* An empty entry: a stray comma names no board. */
	reset_device();
	r = RUN_RX("-U", "0,,1", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: the board list has an empty entry\n")
	          == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(0, device.connect_calls);

	/* More boards than a capture may read. */
	reset_device();
	r = RUN_RX("-U", "0,1,2,3,4,5,6,7,8", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: at most 8 boards can be given\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(0, device.connect_calls);

	/* The same board twice. */
	reset_device();
	r = RUN_RX("-U", "0,1,0", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: board 0 is named twice\n") == 0, r.err);
	mu_assert_int_eq(0, device.connect_calls);

	/* Channels that do not line up with the boards. */
	reset_device();
	r = RUN_RX("-U", "0,1,2", "-c", "10,30", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: -c names 2 channels for 3 boards; "
	          "give one each or one for all\n") == 0, r.err);
	mu_assert_int_eq(0, device.connect_calls);

	/* More channels than boards is the same mistake as fewer, and gets the
	 * same message: one mistake, one wording, in either direction. */
	reset_device();
	r = RUN_RX("-U", "0,1", "-c", "10,30,50", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: -c names 3 channels for 2 boards; "
	          "give one each or one for all\n") == 0, r.err);

	/* Only a list longer than a capture can read at all is refused as too
	 * long, and against the eight boards rather than the boards named. */
	reset_device();
	r = RUN_RX("-U", "0,1", "-c", "1,2,3,4,5,6,7,8,9", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: at most 8 channels can be given\n")
	          == 0, r.err);

	/* An empty channel list entry, reported the same way as a board's. */
	reset_device();
	r = RUN_RX("-U", "0,1", "-c", "10,", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: the channel list has an empty entry\n")
	          == 0, r.err);

	/* A replay reads one saved capture, so it takes one board's worth of
	 * records. */
	reset_device();
	r = RUN_RX("-i", "/nonexistent", "-U", "0,1");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: -i names one saved capture, so it "
	          "cannot be given several boards to read\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);

	/* A dump is replayed as one board's records, so it takes one board. */
	reset_device();
	r = RUN_RX("-U", "0,1", "-d", "/tmp/rx-main-never-written", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: -d writes one dump for -i to replay as"
	          " one board, so it cannot be given several boards\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(0, device.connect_calls);

	/* And -V reports one board. */
	reset_device();
	r = RUN_RX("-U", "0,1", "-V");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: -V reports one board; name one with "
	          "-U, or none to report every board attached\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
}

/* A board is never taken off the address part it worked out itself, even when
 * two other boards agree on a different one. Setting a piconet's address part
 * writes that and nothing else -- not the hop variables derived from the
 * address, and not the clock candidates already chosen -- so a board whose
 * search had started would go on narrowing a list picked for the old address
 * and could reach a clock belonging to it. The run names both answers
 * instead. */
MU_TEST(a_board_s_own_address_part_is_never_overwritten)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 9;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	device.heard_channel[2] = 60;
	/* The first board settles on its own answer; the other two agree on a
	 * different one, which is what the script gives every board after the
	 * first. */
	device.heard_uap_board[0] = 1;
	device.heard_uap_board[1] = 1;
	device.heard_uap_board[2] = 1;
	device.heard_uap = 0x33;
	device.heard_uap_other = 0x22;

	r = RUN_RX("-U", "0,1,2", "-c", "20,40,60", "-l", "112233", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	/* The first board keeps 0x33 and the other two keep 0x22. */
	mu_assert_int_eq(0x33, device.seen_uap[0]);
	mu_assert_int_eq(0x22, device.seen_uap[1]);
	mu_assert_int_eq(0x22, device.seen_uap[2]);
	/* Every board's own answer is named, in the order -U gave them. */
	mu_assert(strstr(r.err, "different UAPs, so one of them is wrong: "
	          "board 1 0x33, board 2 0x22, board 3 0x22") != NULL, r.err);
	/* Every board had one of its own, so nothing was passed to anyone. */
	mu_assert(strstr(r.err, "was passed to a board") == NULL, r.err);
}

/* With four boards a run can both report a disagreement and pass a value on:
 * two boards agree, a third worked out something else and keeps it, and a
 * fourth that worked out nothing is put on the agreed value. The run says
 * both things, because "the boards disagreed" on its own would leave the
 * fourth board's value unexplained. */
MU_TEST(a_disagreement_and_a_board_with_none_are_both_reported)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 16;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	device.heard_channel[2] = 60;
	device.heard_channel[3] = 70;
	device.heard_uap_board[0] = 1;
	device.heard_uap_board[1] = 1;
	device.heard_uap_board[2] = 1;
	/* The values a bench run produced: 0x0d is the peer's, and 0xb0 is
	 * what one board settled on instead. */
	device.heard_uap_value[0] = 0x0d;
	device.heard_uap_value[1] = 0x0d;
	device.heard_uap_value[2] = 0xb0;

	r = RUN_RX("-U", "0,1,2,3", "-c", "20,40,60,70", "-l", "112233",
	           "-t", "10");

	mu_assert_int_eq(0, r.rc);
	/* The third board keeps 0xb0; the fourth is put on the agreed 0x0d. */
	mu_assert_int_eq(0xb0, device.seen_uap[2]);
	mu_assert_int_eq(0x0d, device.seen_uap[3]);
	/* The whole line, so that appending the handed board to the list of
	 * boards' own answers would fail: keeping the two apart is what this
	 * case is for. */
	mu_assert(strstr(r.err, "note: 4 boards: the boards worked out "
	          "different UAPs, so one of them is wrong: board 1 0x0d, "
	          "board 2 0x0d, board 3 0xb0\n") != NULL, r.err);
	mu_assert(strstr(r.err, "note: 4 boards: put on another board's "
	          "answer during the run: board 4 0x0d")
	          != NULL, r.err);
}

/* A long run can pass more than one value on, because the boards that agreed
 * can themselves be taken off it and settle on something else. Each board is
 * named with the value it was given, so a second handing does not hide the
 * first. */
MU_TEST(every_board_put_on_another_s_answer_is_named_with_its_value)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 24;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	/* The blocks are counted across the boards and after the callback, so
	 * with four boards the first sees 0, 4, 8 and the last 3, 7, 11.
	 *
	 * The last board settles on 0x22 of its own first, so it is not one of
	 * the boards with none when the first two agree on 0x0d: that goes to
	 * the third board. The first two are then taken off 0x0d and settle on
	 * 0xb0, and when the last board is taken off its own 0x22 it has none
	 * and is put on 0xb0. Two values passed on in one run. */
	device.heard_uap_board[3] = 1;
	device.heard_uap_value[3] = 0x22;
	device.abandon_uap_at[3] = 15;
	device.heard_uap_board[0] = 1;
	device.heard_uap_value[0] = 0x0d;
	device.settle_uap_at[0] = 4;
	device.abandon_uap_at[0] = 8;
	device.heard_uap_again[0] = 0xb0;
	device.heard_uap_board[1] = 1;
	device.heard_uap_value[1] = 0x0d;
	device.settle_uap_at[1] = 5;
	device.abandon_uap_at[1] = 9;
	device.heard_uap_again[1] = 0xb0;

	r = RUN_RX("-U", "0,1,2,3", "-c", "20,40,60,70", "-l", "112233",
	           "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0x0d, device.seen_uap[2]);
	mu_assert_int_eq(0xb0, device.seen_uap[3]);
	mu_assert(strstr(r.err, "note: 4 boards: put on another board's "
	          "answer during the run: board 3 0x0d, "
	          "board 4 0xb0") != NULL, r.err);
}

/* A value the library has taken off a board is no longer evidence for it. The
 * piconet's value goes to zero and the valid flag with it, and the board then
 * rediscovers and can settle on something else. Here it happens inside the
 * decode, as it does when header after header fails; the other way in, from
 * the capture loop when a follow has gone quiet, is
 * the_report_reads_the_boards_again_before_it_prints's. A reading kept from
 * before would let one board's
 * answer corroborate itself: the board that moved on would still be counted
 * for the value it left, and a board with none of its own would be put on a
 * value only one board holds. */
MU_TEST(an_address_part_the_library_took_back_stops_being_evidence)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 15;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	device.heard_channel[2] = 60;
	/* The first board settles on 0x22, has it taken off it on its second
	 * block, and settles on 0x33 after that. The second board reaches
	 * 0x22 only later, by which time the first board holds 0x33 -- so
	 * there is no pair of boards holding the same value, and the third
	 * board must be handed nothing.
	 *
	 * The blocks are counted across the boards and after the callback, so
	 * the first board sees 0, 3, 6 and the second 1, 4, 7. */
	device.heard_uap_board[0] = 1;
	device.heard_uap_value[0] = 0x22;
	device.abandon_uap_at[0] = 3;
	device.heard_uap_again[0] = 0x33;
	device.heard_uap_board[1] = 1;
	device.heard_uap_value[1] = 0x22;
	device.settle_uap_at[1] = 7;

	r = RUN_RX("-U", "0,1,2", "-c", "20,40,60", "-l", "112233", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(-1, device.seen_uap[2]);
	mu_assert(strstr(r.err, "was passed to a board") == NULL, r.err);
	/* And the run reports what the boards hold now, not what they held. */
	mu_assert(strstr(r.err, "different UAPs, so one of them is wrong: "
	          "board 1 0x33, board 2 0x22") != NULL, r.err);
}

/* The report reads the boards once more before it prints. A clock given up
 * takes the piconet's address part with it, and this tool asks for that from
 * the capture loop rather than from the decode, when a follow has gone quiet
 * -- which is the state a piconet that has gone away leaves, so there is no
 * packet after it to notice anything in. Reading only as packets arrive would
 * have the run end naming a value no board still held. */
MU_TEST(the_report_reads_the_boards_again_before_it_prints)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	/* Not reached: the run ends on the Ctrl-C below, as a capture of a
	 * piconet that has gone away does. */
	device.blocks_to_end = 100;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	/* The two boards work out different answers, so the run would name
	 * both if it read them only as packets arrived. */
	device.heard_uap_board[0] = 1;
	device.heard_uap_value[0] = 0x22;
	device.heard_uap_board[1] = 1;
	device.heard_uap_value[1] = 0x33;
	/* The second board is the one following a clock. Four blocks arrive,
	 * two to each board, which is enough for both to settle and for the
	 * second board's silence to reach the minute the ask waits for; then
	 * every board's buffer is empty, so the ask goes out in a pass where
	 * nothing is decoded and nothing is shared. */
	device.reading_board = 1;
	device.follow_quiet_ms = 0;
	device.block_ms = 30000;
	device.quiet_blocks_at = 4;
	device.quiet_returns_to_end = 12;
	device.abandon_uap_on_drop = 1;

	r = RUN_RX("-U", "0,1", "-c", "20,40", "-l", "112233", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert(device.drops_board[1] > 0, "the quiet board was asked");
	/* The second board holds nothing by the end, so the first board's is
	 * the only answer left and there is no disagreement to report. */
	mu_assert(strstr(r.err, "note: 2 boards: UAP = 0x22") != NULL, r.err);
	mu_assert(strstr(r.err, "different UAPs") == NULL, r.err);
}

/* Nothing is passed between boards where -u gave the address part. Every board
 * was started on the user's value, so no board knows anything another does
 * not; and a board the library has since taken off that value is
 * rediscovering, which is what one board given -u does too. Putting it back
 * would make a run of several boards behave unlike a run of one. */
MU_TEST(a_user_given_address_part_is_not_put_back_after_the_library_drops_it)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 12;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	device.heard_channel[2] = 60;
	/* The third board has the user's value taken off it on its first
	 * block. The other two still hold it, so a run that shared it would
	 * have the pair of them agree and put the third board back on it. */
	device.abandon_uap_at[2] = 2;

	r = RUN_RX("-U", "0,1,2", "-c", "20,40,60", "-l", "112233", "-u", "22",
	           "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(-1, device.seen_uap[2]);
	/* And the user's own value is not handed back to them as a finding. */
	mu_assert(strstr(r.err, "UAP") == NULL, r.err);
}

/* What a board holds is read every pass rather than remembered, because there
 * is no moment a caller can rely on seeing a board hold nothing. The clock is
 * given up at the top of a board's turn and its next block arrives in the same
 * turn, and one header that checks out is enough for the library to settle a
 * different value -- so a reading taken once and kept would stay on the value
 * from before the drop for the rest of the run, and the run would report it,
 * agree on it, and pass it on. */
MU_TEST(a_board_that_settles_again_right_after_a_drop_is_read_again)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 12;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	/* Both boards reach 0x22 to begin with. */
	device.heard_uap_board[0] = 1;
	device.heard_uap_value[0] = 0x22;
	device.heard_uap_board[1] = 1;
	device.heard_uap_value[1] = 0x22;
	/* The first board is the one following a clock. Two of its blocks take
	 * it past the minute the ask waits for, so the clock is given up at
	 * the top of its third turn -- with the blocks still arriving, so the
	 * board settles on 0x33 in that same turn and is never seen holding
	 * none. */
	device.reading_board = 0;
	device.follow_quiet_ms = 0;
	device.block_ms = 30000;
	device.abandon_uap_on_drop = 1;
	device.heard_uap_after_drop[0] = 0x33;

	r = RUN_RX("-U", "0,1", "-c", "20,40", "-l", "112233", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert(device.drops_board[0] > 0, "the quiet board was asked");
	/* 0x33 is what the first board holds by the end, so the boards no
	 * longer agree and the run says so with the values they hold. */
	mu_assert(strstr(r.err, "note: 2 boards: the boards worked out "
	          "different UAPs, so one of them is wrong: board 1 0x33, "
	          "board 2 0x22\n") != NULL, r.err);
	mu_assert(strstr(r.err, "UAP = 0x") == NULL, r.err);
}

/* A board put on another's answer that then reaches one of its own is named in
 * both lines, and each is true of it: the run did pass it a value, and it does
 * hold an answer of its own by the end. */
MU_TEST(a_board_put_on_another_s_answer_can_still_reach_its_own)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 24;
	device.heard_channel[0] = 20;
	device.heard_channel[1] = 40;
	device.heard_channel[2] = 60;
	/* The first two boards reach 0x22, so the third, which works out
	 * nothing of its own, is put on it. */
	device.heard_uap_board[0] = 1;
	device.heard_uap_value[0] = 0x22;
	device.heard_uap_board[1] = 1;
	device.heard_uap_value[1] = 0x22;
	/* The third board is then taken off the value it was given, on its
	 * third block, and reaches 0x33 of its own after that. It settles
	 * nothing before then, which is why it was given one at all. */
	device.heard_uap_board[2] = 1;
	device.settle_uap_at[2] = 9;
	device.abandon_uap_at[2] = 8;
	device.heard_uap_again[2] = 0x33;

	r = RUN_RX("-U", "0,1,2", "-c", "20,40,60", "-l", "112233", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0x33, device.seen_uap[2]);
	/* Its own answer counts, so the run reports a disagreement. */
	mu_assert(strstr(r.err, "different UAPs, so one of them is wrong: "
	          "board 1 0x22, board 2 0x22, board 3 0x33\n") != NULL,
	          r.err);
	/* And the run still says it was passed a value, because it was. */
	mu_assert(strstr(r.err, "note: 3 boards: put on another board's "
	          "answer during the run: board 3 0x22\n") != NULL, r.err);
}

/* A survey takes a list too: every board on it is opened, set up, read, and
 * stopped, the results are printed, and the run exits 0. A survey has no
 * piconet per board -- the table of addresses heard belongs to the decoder --
 * so there is nothing to merge and the run says nothing about boards.
 *
 * What the addresses themselves do inside libbtbb's table is the library's,
 * and the fake decode here puts nothing in it, so this case pins the loop and
 * the teardown rather than the table. */
MU_TEST(a_survey_reads_every_board_on_the_list)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 6;

	r = RUN_RX("-U", "0,1,2", "-z", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(3, device.connect_calls);
	mu_assert_int_eq(3, device.rx_syms_calls);
	mu_assert_int_eq(3, device.stops);
	/* The heading goes to standard error, the addresses to standard
	 * output, so a shell can keep the one and read the other. */
	mu_assert(strstr(r.err, "Survey Results") != NULL, r.err);
	/* A survey has no piconet of its own, so nothing is merged and the
	 * run says nothing about boards. */
	mu_assert(strstr(r.err, "boards:") == NULL, r.err);
}

/* Every way a capture ends ends every board's, gives every handle back, and
 * exits as it does for one board: 0 for the -t timer and for a Ctrl-C, 1 for a
 * teardown a board refused and for a stream that died. */
MU_TEST(every_stop_reason_ends_every_board)
{
	struct run_result r;

	/* The -t timer. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 6;
	r = RUN_RX("-U", "0,1,2", "-t", "10");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(3, device.stops);
	mu_assert_int_eq(3, device.frees);

	/* A Ctrl-C. */
	reset_device();
	device.end = END_BY_INTERRUPT;
	device.blocks_to_end = 6;
	r = RUN_RX("-U", "0,1,2", "-t", "600");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(3, device.stops);
	mu_assert_int_eq(3, device.frees);

	/* A teardown a board refused, which leaves that board running the
	 * mode this capture started. */
	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 6;
	device.stop_refused = 1;
	r = RUN_RX("-U", "0,1,2", "-t", "10");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(3, device.stops);
	mu_assert_int_eq(3, device.frees);

	/* One board's stream dying, which ends the whole capture: what that
	 * costs the other boards is
	 * a_board_whose_stream_died_ends_the_whole_capture's, and what it
	 * leaves behind is this one's. */
	reset_device();
	device.end = END_BY_STREAM_FAILURE;
	device.blocks_to_end = 4;
	r = RUN_RX("-U", "0,1,2", "-t", "30");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(3, device.stops);
	mu_assert_int_eq(3, device.frees);
}

/* Each board is sent its own requests. The modulation, the capture and the
 * channel go to one board before the next is set up, so a bug that sent all
 * three to the first board would leave the others with none. */
MU_TEST(every_board_is_sent_its_own_requests)
{
	struct run_result r;
	int i;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;

	r = RUN_RX("-U", "0,1,2", "-c", "20,40,60", "-l", "112233", "-u", "22",
	           "-t", "10");

	mu_assert_int_eq(0, r.rc);
	for (i = 0; i < 3; i++)
		mu_assert(strcmp(device.board_calls[i], "bmsc") == 0,
		          device.board_calls[i]);
	mu_assert_int_eq(2402 + 20, device.board_channel[0]);
	mu_assert_int_eq(2402 + 40, device.board_channel[1]);
	mu_assert_int_eq(2402 + 60, device.board_channel[2]);
	/* The user gave the address part, so the run has nothing of its own to
	 * report about it and does not hand their value back as a finding. */
	mu_assert(strstr(r.err, "UAP") == NULL, r.err);
}

/* The follow is per board: only the board whose follow has gone quiet is asked
 * to give its clock up. Before the follow state moved onto the handle there
 * was one reading for the process, so the ask went wherever the loop was. */
MU_TEST(only_the_quiet_board_is_asked_to_give_its_clock_up)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.block_ms = 30000;
	device.blocks_to_end = 9;
	/* The third board is the one following a clock and hearing nothing. */
	device.reading_board = 2;
	device.follow_quiet_ms = 0;

	r = RUN_RX("-U", "0,1,2", "-l", "112233", "-u", "22", "-t", "600");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, device.drops_board[0]);
	mu_assert_int_eq(0, device.drops_board[1]);
	mu_assert(device.drops_board[2] > 0, "the quiet board was asked");
}

/* -c may be written before -U: how many channels are allowed depends on the
 * boards, which is why the lists are read once the whole command line has
 * parsed rather than as each option arrives. */
MU_TEST(the_channel_list_may_come_before_the_board_list)
{
	struct run_result r;

	reset_device();
	device.end = END_BY_TIMEOUT;
	device.blocks_to_end = 3;

	r = RUN_RX("-c", "20,40,60", "-U", "0,1,2", "-t", "10");

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2402 + 20, device.board_channel[0]);
	mu_assert_int_eq(2402 + 60, device.board_channel[2]);
}

int main(void)
{
	/* Nothing here should look for a board; make sure of it. */
	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	MU_RUN(a_capture_whose_stream_died_exits_one);
	MU_RUN(every_exit_path_gives_the_handle_back);
	MU_RUN(a_capture_on_standard_output_gets_the_stream_and_its_header);
	MU_RUN(a_capture_whose_teardown_was_refused_exits_one);
	MU_RUN(a_survey_whose_teardown_was_refused_still_prints_its_results);
	MU_RUN(a_run_refused_for_its_firmware_creates_no_file);
	MU_RUN(a_bounded_capture_still_exits_zero);
	MU_RUN(an_interrupted_capture_still_exits_zero);
	MU_RUN(an_interrupt_while_the_board_is_opened_ends_the_run);
	MU_RUN(a_selection_the_user_stopped_exits_zero);
	MU_RUN(a_survey_whose_stream_died_still_prints_its_results);
	MU_RUN(a_refused_command_line_opens_no_device);
	MU_RUN(a_replay_refuses_the_options_it_cannot_act_on);
	MU_RUN(a_capture_the_board_took_sets_up_and_exits_zero);
	MU_RUN(the_capture_is_asked_for_before_the_channel);
	MU_RUN(the_capture_states_basic_rate);
	MU_RUN(a_refused_setup_request_is_named_and_exits_one);
	MU_RUN(a_refused_modulation_ends_the_capture);
	MU_RUN(a_truncated_address_request_ends_the_run_too);
	MU_RUN(a_refused_sweep_leaves_survey_mode_with_no_results);
	MU_RUN(a_piconet_that_cannot_be_allocated_exits_one);
	MU_RUN(a_run_that_fails_before_the_capture_creates_no_file);
	MU_RUN(a_run_refused_after_the_files_exist_removes_them);
	MU_RUN(a_refused_run_removes_every_file_it_was_asked_for);
	MU_RUN(a_capture_that_ran_keeps_its_files);
	MU_RUN(a_refused_run_leaves_a_link_and_dev_null_alone);
	MU_RUN(a_replay_that_read_no_record_removes_its_files);
	MU_RUN(a_replay_that_read_records_keeps_its_files);
	MU_RUN(a_replay_of_an_empty_input_keeps_its_files);
	MU_RUN(a_replay_whose_input_cannot_be_opened_creates_no_file);
	MU_RUN(a_capture_that_cannot_start_exits_one);
	MU_RUN(a_firmware_the_tool_cannot_use_exits_one);
	MU_RUN(a_replay_sends_no_setup_request);
	MU_RUN(a_live_capture_and_a_replay_both_report_their_gaps);
	MU_RUN(a_follow_that_hears_nothing_gives_the_clock_up_once);
	MU_RUN(the_wait_is_a_minute);
	MU_RUN(a_working_follow_and_a_capture_with_no_clock_keep_going);
	MU_RUN(a_follow_that_stays_quiet_is_asked_about_once_per_wait);
	MU_RUN(a_search_that_does_not_confirm_a_clock_says_so_once);
	MU_RUN(the_search_note_waits_half_a_minute);
	MU_RUN(a_run_with_no_search_running_says_nothing);
	MU_RUN(a_replay_whose_search_does_not_confirm_a_clock_says_so);
	MU_RUN(a_list_of_boards_opens_and_sets_up_each_one);
	MU_RUN(a_channel_each_spreads_the_boards_across_the_band);
	MU_RUN(a_board_that_cannot_be_opened_gives_every_handle_back);
	MU_RUN(boards_share_the_channel_map_and_the_address_part);
	MU_RUN(one_board_s_address_part_is_not_passed_on);
	MU_RUN(boards_that_disagree_about_the_address_part_say_so);
	MU_RUN(one_board_is_left_exactly_as_it_was);
	MU_RUN(a_board_whose_stream_died_ends_the_whole_capture);
	MU_RUN(a_board_or_channel_list_the_tool_will_not_take_is_refused);
	MU_RUN(a_board_s_own_address_part_is_never_overwritten);
	MU_RUN(a_disagreement_and_a_board_with_none_are_both_reported);
	MU_RUN(every_board_put_on_another_s_answer_is_named_with_its_value);
	MU_RUN(the_report_reads_the_boards_again_before_it_prints);
	MU_RUN(a_user_given_address_part_is_not_put_back_after_the_library_drops_it);
	MU_RUN(a_board_that_settles_again_right_after_a_drop_is_read_again);
	MU_RUN(a_board_put_on_another_s_answer_can_still_reach_its_own);
	MU_RUN(an_address_part_the_library_took_back_stops_being_evidence);
	MU_RUN(a_survey_reads_every_board_on_the_list);
	MU_RUN(every_stop_reason_ends_every_board);
	MU_RUN(every_board_is_sent_its_own_requests);
	MU_RUN(only_the_quiet_board_is_asked_to_give_its_clock_up);
	MU_RUN(the_channel_list_may_come_before_the_board_list);
	return mu_report();
}
