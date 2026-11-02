/*
 * Copyright 2010 - 2013 Michael Ossmann, Dominic Spill, Will Code, Mike Ryan
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __UBERTOOTH_H__
#define __UBERTOOTH_H__

#include "ubertooth_control.h"
#include "ubertooth_fifo.h"
#include "ubertooth_le_frag.h"
#include <btbb.h>

/* specan output types
 * see https://github.com/dkogan/feedgnuplot for plotter */
enum specan_modes {
	SPECAN_STDOUT         = 0,
	SPECAN_GNUPLOT_NORMAL = 1,
	SPECAN_GNUPLOT_3D     = 2,
	SPECAN_FILE           = 3
};

enum board_ids {
	BOARD_ID_UBERTOOTH_ZERO = 0,
	BOARD_ID_UBERTOOTH_ONE  = 1,
	BOARD_ID_TC13BADGE      = 2
};

/* Who owns the receive transfer, which decides whether it may be freed or put
 * back in flight. libusb owns a transfer from the moment it accepts a submit
 * until its callback has run without resubmitting it; freeing one in between
 * is undefined behaviour, and a cancelled one must not be resubmitted. The
 * result of libusb_cancel_transfer() cannot stand in for this: it refuses a
 * transfer whose cancellation is already in progress, and one whose device
 * has gone away, and both still have a callback to come.
 * RX_XFER_OURS is zero so a zeroed handle starts out owning nothing.
 *
 * This and rx_xfer are shared between the transfer's callback, which libusb
 * runs on the poll thread, and the receive loop and teardown on the run's own
 * thread. Inside the library a lock guards every read and write of the pair
 * (see rx_xfer_lock in ubertooth.c); read them from outside it only where no
 * capture is running. */
enum rx_xfer_state {
	RX_XFER_OURS = 0,   /* no transfer, or one libusb is not holding */
	RX_XFER_IN_FLIGHT,  /* submitted: libusb owns it */
	RX_XFER_CANCELLING  /* submitted and cancelled: libusb still owns it */
};

typedef struct {
	/* Ringbuffers for USB and Bluetooth symbols */
	fifo_t* fifo;

	struct libusb_device_handle* devh;
	struct libusb_transfer* rx_xfer;

	/* Set to stop the capture: by the poll thread when a stream dies, and
	 * by a callback that cannot go on. Read by the capture's own loop while
	 * the poll thread runs, so the loop has to reload it rather than keep a
	 * copy of it.
	 *
	 * A signal handler must not write this; it sets a flag of its own, which
	 * ubertooth_stopping() folds in here. Read it through that function, not
	 * directly, or a loop will not notice a Ctrl-C. */
	volatile uint8_t stop_ubertooth;

	/* The host time the capture's first packet arrived. Every timestamp in
	 * the capture is an offset from it, so a reading taken where the loop
	 * reached that packet rather than where it arrived would start the
	 * whole capture late. */
	uint64_t abs_start_ns;
	uint32_t start_clk100ns;
	uint64_t last_clk100ns;
	uint64_t clk100ns_upper;

	/* Where the host's own clock stood when the last packet of this capture
	 * arrived, as the ring buffer recorded it (fifo_popped_arrival_ns()).
	 * How much of it passes between two packets is what tells a capture
	 * clock that has started over from a packet simply out of place, so it
	 * has to be when each packet came in and not when the loop reached it:
	 * a loop behind a backlog takes two packets moments apart however long
	 * the real gap was. */
	uint64_t last_arrival_ns;

	/* The reconstructed time of the previous packet of this capture, which
	 * is what the gap printed beside each packet is measured from: see
	 * ns_since_the_last_packet(). On the handle rather than in the callback
	 * so that each capture starts afresh and two handles cannot measure
	 * each other's gaps. */
	uint64_t prev_packet_ns;

	/* The channel or the sweep this capture asked for, as the request that
	 * set it carried. A mode start makes the board forget its hop state,
	 * so a callback that starts a mode again has to say this once more or
	 * the board stops sweeping and stays on whatever channel the last hop
	 * chose. cb_rx() needs it when the library gives up a clock: the
	 * capture goes back to looking for a piconet across the band, and
	 * looking on one channel in 79 would hardly ever find one.
	 *
	 * Set by the tool beside the request itself. Zero where no capture has
	 * set one, which is also what a replay leaves it as. */
	uint16_t capture_channel;

	/* The modulation this capture wants the board to receive, for the
	 * streams libubertooth starts itself (rx_dump(), rx_afh()). It is
	 * stated on either side of the mode request: before it, so that the
	 * mode start programs the demodulator from it, and after it, because a
	 * mode start puts the modulation back to Basic Rate and would otherwise
	 * throw away the one stated first.
	 *
	 * MOD_BT_BASIC_RATE unless a tool asks for the other, which only
	 * `ubertooth-dump -l` does. Every capture states one: a board that has
	 * been left idle holds the last modulation a tool set, because a mode
	 * starting from idle resets nothing. */
	uint8_t capture_modulation;

	le_usb_reassembly le_reassembly;

	btbb_pcap_handle* h_pcap_bredr;
	lell_pcap_handle* h_pcap_le;
	btbb_pcapng_handle* h_pcapng_bredr;
	lell_pcapng_handle* h_pcapng_le;

	/* Whether abs_start_ns and start_clk100ns hold the first packet of a
	 * capture yet: a first packet at clk100ns 0 is a valid start, not a
	 * missing one. */
	uint8_t clk100ns_started;

	/* Whether libusb is holding rx_xfer, one of enum rx_xfer_state. Next to
	 * the byte above so the two share its padding, and after the offsets
	 * above so that none of them moved. */
	uint8_t rx_xfer_state;

	/* After how many ticks the clock stamping these packets starts over:
	 * CLK100NS_PERIOD for the CLKN-derived clock, which is what a mode
	 * stamps with unless it says otherwise, and CLK100NS_TIMER1_PERIOD for
	 * the low-energy physical-layer modes, which stamp from timer1. A mode
	 * that uses the latter sets this before streaming. */
	uint64_t clk100ns_period;

	/* The period count the last packet carried (usb_pkt_rx.clkn_high), zero
	 * for a capture whose packets do not carry one. */
	uint8_t last_clkn_high;

	/* Whether a packet of this capture has carried a period count, which is
	 * what makes the count rather than the clock reading lower the record of
	 * how many periods have gone by. */
	uint8_t clk100ns_counted;

	/* Whether the capture's anchor may still move to the next packet's
	 * clock reading, and its period count baseline to that packet's count.
	 * Set by the first packet and cleared by the second, so either can only
	 * move while the packet holding them is the one in doubt. */
	uint8_t clk100ns_anchor_open;

	/* Whether the host clock running ahead of the reconstructed times has
	 * already been reported, so it is said once per capture. */
	uint8_t clk100ns_gap_warned;

	/* Whether a packet of this capture has been printed yet, so that
	 * prev_packet_ns above holds something to measure a gap from. A flag
	 * of its own, because a reconstructed time of zero is a legitimate
	 * reading. */
	uint8_t prev_packet_seen;

	/* Set when the receive stream ended because its USB transfer failed. It
	 * holds the libusb transfer status, or LIBUSB_TRANSFER_ERROR for a
	 * transfer libusb would not put back in flight, or one it never handed
	 * back at all. It stays 0 while a capture runs and after every ordinary
	 * end: a timeout, an interrupt, the teardown's own cancellation. So a
	 * capture that ends with this set stopped where the stream died, and
	 * holds less than was asked for. Read it through
	 * ubertooth_stream_failed().
	 *
	 * Written on the poll thread, like stop_ubertooth above, which is why
	 * it is volatile too, and by the teardown once that thread is joined. A
	 * caller reads it after the teardown.
	 *
	 * It is never cleared once set. On a handle that runs more than one
	 * stream (ubertooth-afh detects a map, then monitors it) it therefore
	 * means "a stream on this handle failed", which is all a tool's exit
	 * status has to say anyway. */
	volatile int rx_stream_status;

	/* Whether a receive transfer that arrived with some bytes but not a
	 * whole packet has already been reported, so it is said once rather
	 * than once per transfer: the fault that truncates one usually
	 * truncates many.
	 *
	 * Written where the receive callback runs: the poll thread while a
	 * capture is running, and the stopping thread for the last transfer,
	 * which the teardown waits for. Those never overlap, and a short
	 * transfer during the teardown is not reported anyway.
	 *
	 * Never cleared, so on a handle that runs more than one stream
	 * (ubertooth-afh detects a map, then monitors it) it means "a transfer
	 * on this handle was truncated". */
	uint8_t short_xfer_warned;

	/* 1 while this handle holds a libusb session: set where one was opened
	 * for it, cleared by the teardown. It is what makes the teardown run
	 * once, and only on a handle that has something to give back, so
	 * ubertooth_close() ends it once, and ends none at all on a handle that
	 * never opened a board -- a replay, which reads a file. */
	uint8_t usb_session_open;

	/* Whether the teardown has already reported a receive transfer that
	 * libusb never handed back, so closing such a handle twice says it
	 * once. */
	uint8_t xfer_lost_warned;

	/* How many records arrived carrying FIFO_OVERFLOW, which the board sets
	 * on the first record it sends after it had no room in its queue for a
	 * packet. So this counts the gaps in a capture, not the packets lost in
	 * them: one full queue can swallow several packets, and the board has
	 * no field to send a number in.
	 *
	 * The warned flag keeps the running warning to one line per capture;
	 * the count is what a tool reports when the capture ends. Read it with
	 * ubertooth_queue_gaps(). */
	uint32_t queue_gaps;
	uint8_t queue_gap_warned;

	/* How many records arrived carrying DMA_ERROR, which the board sets when
	 * the chip's transfer controller reported an error on the channel a
	 * capture receives into. The record it is marked on was kept, because its
	 * transfer had finished, but some of its bytes may be wrong and what the
	 * fault cut short before it is lost, so this counts places where the
	 * capture may be wrong or missing packets.
	 *
	 * The same pair as the gap counters above: the warned flag keeps the
	 * running warning to one line per capture, and the count is what a tool
	 * reports when the capture ends. Read it with
	 * ubertooth_transfer_errors(). This is the board's own fault count, not
	 * a failed USB transfer on the host, which ubertooth_stream_failed()
	 * carries. */
	uint32_t transfer_errors;
	uint8_t transfer_error_warned;

	/* How many records arrived carrying DMA_OVERFLOW, which the board sets
	 * when a block of symbols finished arriving before its capture loop had
	 * read the block before it. The new symbols went over the old ones, so
	 * that block is gone before anything looked in it for a packet: the
	 * capture is missing whatever was in the air then, with no packet to name
	 * for it.
	 *
	 * So this counts the places a capture was marked, not the blocks lost at
	 * them: the board has one flag and clears it when a record is queued, so
	 * one mark can stand for several blocks, and there is no field for a
	 * number.
	 *
	 * The same pair as the counters above: the warned flag keeps the running
	 * warning to one line per capture, and the count is what a tool reports
	 * when the capture ends. Read it with ubertooth_symbol_overwrites(). */
	uint32_t symbol_overwrites;
	uint8_t symbol_overwrite_warned;

	/* Whether this capture has already said its signal sits at the top of
	 * what the radio can report, so it is said once. Never cleared, so on a
	 * handle that runs more than one stream (ubertooth-afh detects a map,
	 * then monitors it) it means "a stream on this handle was pinned there",
	 * said once across both, and the part block the first stream ends on is
	 * finished by the second stream's records.
	 *
	 * The two counters behind it hold one block of records at a time: how
	 * many of the block carried a signal figure, and how many of those were
	 * at the top. A block that is nearly all at the top is what the warning
	 * goes on; then both are cleared and the next block is judged on its
	 * own. One block rather than the whole capture, so a capture that starts
	 * too loud and is then put right does not carry the first part forever,
	 * and a loud stretch in the middle of a long quiet capture is still
	 * noticed. Read how many blocks were at the top with
	 * ubertooth_signal_ceiling_blocks(). */
	uint16_t signal_records;
	uint16_t signal_records_at_ceiling;
	uint32_t signal_ceiling_blocks;
	uint8_t signal_ceiling_warned;

	/* What the teardown's STOP request came back with, 0 while none has
	 * failed. A board that would not take it is left running the mode the
	 * capture started, and no other tool can open it until it is reset or
	 * re-plugged, so the run did not finish what it was asked: every
	 * streaming tool turns this into a non-zero exit status. Read it
	 * through ubertooth_stop_failed().
	 *
	 * Written only by the teardown, on the run's own thread, once the poll
	 * thread is joined. The first failure wins, as for rx_stream_status
	 * above, and it is never cleared: on a handle that stops more than one
	 * mode (ubertooth-afh detects a map, then monitors it) it means "a
	 * teardown on this handle was refused". */
	int stop_status;

	/* The board the user named, as they wrote it after -U: an index or a
	 * serial. NULL when they named none and the library opened whichever
	 * board it found. A message about this one board can then say which it
	 * is. It points into the tool's own command line, which outlives the
	 * handle, so nothing here copies or frees it. */
	const char* selector;
} ubertooth_t;

typedef void (*rx_callback)(ubertooth_t* ut, void* args);

typedef struct {
	unsigned allowed_access_address_errors;
	/* Set when a capture has taken the tool's standard output ("-q -",
	 * "-c -"). That redirects the process's standard output to /dev/null
	 * for the whole run, so the per-packet dump the callback writes there
	 * reaches nobody, and the callback skips it. See cb_btle(). */
	int capture_took_stdout;
	/* Where the callback writes the lines that report something to the
	 * user rather than capture data: the connection parameters promiscuous
	 * mode discovers. The tool owns this stream, so it is standard error
	 * once a capture has taken standard output and those lines stay
	 * visible. NULL means standard output, for a caller that sets none. */
	FILE* report_stream;
	/* How many packets the dump met whose data the decoder could not read
	 * all of: an advertising element running past the end of the payload,
	 * or a PDU shorter than its own type needs. Data off the air that does
	 * not hold what its length fields describe is ordinary -- both a real
	 * packet with a corrupted byte and noise that decoded as a packet
	 * produce one -- so the callback counts the packets and the tool
	 * reports one line when the run ends.
	 *
	 * Per packet, where libbtbb's own lell_malformed_count() is per
	 * element, so a packet carrying two unreadable elements counts once
	 * here and twice there. Only a dump counts: libbtbb keeps its counts
	 * in lell_print(), so a run whose packets go only to a capture file
	 * counts nothing. */
	uint32_t malformed_packets;
} btle_options;

extern uint32_t systime;
extern FILE* infile;
extern FILE* dumpfile;
extern int max_ac_errors;

/* The largest value btbb_init() accepts for max_ac_errors: libbtbb refuses
 * anything above AC_ERROR_LIMIT (lib/src/bluetooth_packet.c), which btbb.h
 * documents as 5. Anything larger needs a syndrome table of several GB. */
#define MAX_AC_ERRORS 5

/* The host tools' own version and the libbtbb this run loaded: the first
 * lines of the -V answer below, on the caller's stream. Every tool here asks
 * for the whole answer instead, through ubertooth_print_versions().
 *
 * print_version() is the older spelling, on standard output. Nothing in this
 * tree calls it; it is kept because it is part of the installed library's
 * interface, and programs built against it are not this tree's to change. */
void ubertooth_print_host_version(FILE* out);
void print_version(void);

/* The labels the version lines carry, so a tool that prints one of them on
 * its own cannot label it differently from -V. */
#define UBERTOOTH_VERSION_LABEL_HOST     "Host tools version"
#define UBERTOOTH_VERSION_LABEL_FIRMWARE "Firmware version"
#define UBERTOOTH_VERSION_LABEL_BUILD    "Firmware build"

/* Long enough for any version, revision or build string a board reports:
 * those are read into a buffer of this size and truncated to fit. */
#define UBERTOOTH_VERSION_TEXT_LEN 255

/* The whole answer to -V, which is the same in every tool, written to the
 * caller's stream -- the tools pass their standard output, where the user
 * asked for it: the host tools' version, then the firmware version and build of the
 * board the user named with -U, or of the only board attached. With several
 * boards and no -U, those two lines per board, indexed. With no board
 * attached, "Firmware version: no Ubertooth attached".
 *
 * selector is the -U argument, or NULL where the user named no board. The
 * board is opened and closed without a STOP, so asking for the version does
 * not end a mode it is running, and no firmware API version is required: -V
 * has to answer on whatever firmware the board is carrying.
 *
 * Returns the exit status the tool should use: 0 when the answer is complete,
 * including with no board attached, and 1 when a board is attached but could
 * not be read -- that board's line says why, and the library has already put
 * the reason on standard error.
 */
int ubertooth_print_versions(FILE *out, const char *selector);

/* The firmware version of an open board as the text the line above carries:
 * "<image> <revision> (API:<x.yy>)", or the revision alone plus the API
 * version on firmware older than the info block.
 *
 * For a caller that prints that text in a line of its own
 * (ubertooth-util -v). Returns 0 when all of it was read, or 1 when part of
 * it could not be, already reported on standard error: text then holds what
 * was read, and is an empty string when nothing was.
 */
int ubertooth_firmware_version_text(ubertooth_t *ut, char *text, size_t size);

/* Installs the library's signal handlers, so Ctrl-C, SIGTERM, SIGHUP, SIGQUIT
 * and SIGALRM stop the capture instead of killing the tool with its mode still
 * running on the board.
 *
 * Call it before a board is opened. Until the handlers are in place a signal
 * gets whatever disposition the tool was started with, and a tool started as
 * a background job by a script was started with SIGINT ignored: such a signal
 * is discarded, so the user's first Ctrl-C does nothing at all and a second
 * one is needed. Opening a board takes a moment -- selecting one by serial
 * number reads the serial of every attached board, and a board that does not
 * answer costs a second -- which is long enough for a signal to land in that
 * window.
 *
 * A stop that arrives before the capture starts is not lost: it is in the flag
 * ubertooth_stopping() reads, the board selection below gives up on it, and a
 * receive loop that does start ends on its first turn. */
void ubertooth_catch_stop_signals(void);

/* 1 once a signal has asked this run to stop, for a caller that has no handle
 * yet: before a board is open there is nothing for ubertooth_stopping() to
 * read. A tool asks this between opening a board and starting anything on it,
 * so a run the user has already stopped ends there instead of putting the
 * board in a mode, or transmitting, only to stop it again. */
int ubertooth_stop_requested(void);

/* 1 when the last open gave up because the user stopped the run, rather than
 * because it could not find or open a board. Only that outcome is not a
 * failure, so it is the one a tool exits 0 on; a board that is genuinely not
 * there still reports its error and exits 1, even if a signal happened to
 * arrive at the same moment. Read it straight after an open returned NULL or
 * a negative result. */
int ubertooth_open_stopped(void);

/* Deprecated: call ubertooth_catch_stop_signals() above instead. It is still
 * exported, so a program that calls it builds and links against this release,
 * though a build with -Werror stops on the warning. It is removed in the
 * release after this one.
 *
 * It is ubertooth_catch_stop_signals(), plus a copy of ut the library keeps
 * for the do_exit = 1 form of the handlers. Nothing in this repository needs
 * that copy: the tools install the handlers before they have a handle, and
 * do_exit = 1 runs ubertooth_stop() from inside a signal handler, which can
 * deadlock in libusb (see cleanup_exit() in ubertooth.c), so it is not a form
 * to choose. What replaces each part:
 *
 *   register_cleanup_handler(ut, 0)  ->  ubertooth_catch_stop_signals()
 *   register_cleanup_handler(ut, 1)  ->  ubertooth_catch_stop_signals(), then
 *                                        end the run on the caller's own
 *                                        thread when ubertooth_stopping()
 *                                        reports the stop
 *
 * The library keeps ut until ubertooth_free() gives it back, so nothing the
 * caller has to unregister: free the handle and the handlers let go of it. */
UBERTOOTH_DEPRECATED("use ubertooth_catch_stop_signals()")
void register_cleanup_handler(ubertooth_t* ut, int do_exit);

/* 1 once this capture should stop, which is how a receive loop asks. It covers
 * every way a run ends early: a signal (Ctrl-C, SIGTERM, the -t alarm), which
 * the handler records in a flag of its own, and the handle's stop_ubertooth,
 * which the poll thread and the callbacks set.
 *
 * It writes the handle, so call it from the thread running the capture. The
 * poll thread reads the flags instead. */
int ubertooth_stopping(ubertooth_t* ut);

/* Forgets the stop the -t timer asked for, so a handle that captures twice can
 * start its second phase (ubertooth-afh detects a map under -t, then monitors
 * it): the timer that ended detection would otherwise end monitoring before it
 * began.
 *
 * A stop the user asked for is deliberately left in place, so
 * ubertooth_stopping() goes on reporting it. A caller that finds it still set
 * is expected to return rather than start the next phase: the user has already
 * said to stop. */
void ubertooth_clear_timeout_stop(ubertooth_t* ut);

/* Forgets every signal this process has had, so a run can start as if none
 * had arrived. Nothing in a tool needs this -- a run the user has stopped is
 * over -- but a test process runs many captures in turn, and one case must not
 * leave the next already stopping. */
void ubertooth_reset_stop_signals(void);
ubertooth_t* ubertooth_init();
/* True when UBERTOOTH_TEST_NO_DEVICES is "1", which makes every selection
 * below behave as if no Ubertooth were attached, without listing or opening
 * anything on the bus: the project's tests set it so a test run cannot open a
 * board another process is using. A tool that enumerates the bus itself must
 * check this too. */
int ubertooth_devices_hidden(void);
int ubertooth_count(void);
int ubertooth_connect(ubertooth_t* ut, int ubertooth_device);
ubertooth_t* ubertooth_start(int ubertooth_device);
int ubertooth_connect_serial(ubertooth_t* ut, char* serial_number);
ubertooth_t* ubertooth_start_serial(char* serial_number);
/* Accepts a -U argument that is either a numeric index or a serial string. */
ubertooth_t* ubertooth_start_select(const char* u_arg);
int ubertooth_connect_select(ubertooth_t* ut, const char* u_arg);
/* Like ubertooth_start_select(), but prints nothing and never sends STOP: for
 * polling a device that may be absent (for example while it re-enumerates
 * after a reset). Returns NULL if the selected device is not available. */
ubertooth_t* ubertooth_start_select_quiet(const char* u_arg);
/* Releases the board: finishes the receive transfer, releases the USB
 * interface, closes the device and ends the libusb session this handle
 * started. Nothing is asked of the board, so a mode it is running keeps
 * running -- that is what ubertooth_stop() is for.
 *
 * Calling it on a handle that is already closed does nothing, so a caller
 * that cannot tell (ubertooth_check_api() closes a board whose firmware it
 * refused) may simply call it. The handle itself stays allocated:
 * ubertooth_free() gives that back. */
void ubertooth_close(ubertooth_t* ut);
/* Reports a board that opened but could not be claimed, naming it as board
 * when the caller knows which one (NULL for "the Ubertooth"). On this path the
 * device opened, so the failure is not a permissions problem: both
 * LIBUSB_ERROR_BUSY and LIBUSB_ERROR_ACCESS mean something else is holding the
 * interface -- another program, or a kernel driver -- and which of the two
 * libusb returns depends on the system, so neither is reported on its own. The
 * code is kept in parentheses for bug reports. */
void ubertooth_report_claim_failed(const char* board, int err);
/* Reports any board sitting in the DFU bootloader, as a note on standard
 * error: a bootloader board answers no tool, and it is there because it is
 * being flashed or because a flash left it there. For the paths that found no
 * Ubertooth to work with -- a selection that came up empty, and -V with
 * nothing attached -- so that "no board" says what else was seen on the bus.
 * It says nothing when there is none, and nothing while the test switch hides
 * devices. */
void ubertooth_note_dfu_bootloader(void);

/*
 * Why the last open in this process failed, as a short phrase that reads after
 * "unavailable, " on a report line: "in use by another program", "access to
 * the board was denied", "no board answered at that index", and "it could not
 * be opened" when nothing more precise is known. The open paths have already
 * put the matching error line on standard error; this is for a tool that also
 * has to say, on standard output, why a board it was listing has no value.
 *
 * Read it straight after an open returned NULL or a negative result: every
 * open clears it on the way in and sets it on each failure, so after one that
 * succeeded it is the "nothing more precise" phrase. The result is a string
 * literal and is never freed.
 */
const char* ubertooth_open_failure_reason(void);
/* ubertooth_close() plus a STOP request first, which ends the mode the board
 * is running: what a tool that started a capture wants on its way out, so the
 * board is not left capturing into nothing. Idempotent in the same way, and
 * the handle likewise stays allocated for ubertooth_free(). */
void ubertooth_stop(ubertooth_t* ut);
/* Releases a handle ubertooth_stop() or ubertooth_close() has finished with.
 * A handle whose receive transfer libusb never gave back is left allocated
 * instead: that transfer's callback reads into this handle's ring buffer. The
 * teardown reports such a handle as a warning, so a caller that keeps going
 * sees it; one that is about to exit can ignore it. */
void ubertooth_free(ubertooth_t* ut);
/* The firmware API version the board reports, as its USB bcdDevice. Returns 0
 * and writes version, or a negative libusb error, already reported. The board
 * is left open either way. */
int ubertooth_get_api(ubertooth_t *ut, uint16_t *version);
/* Whether the board's firmware is the one these host tools were built with:
 * the API version they require, and the image they drive. Returns 0 when the
 * command may go on, or a negative error, already reported: a version this
 * host refuses to drive, a version that could not be read at all, or another
 * of this tree's images, whose message names the image to flash.
 *
 * A release build refuses any other version. A development build reports it
 * and returns 0, so host tools and firmware from different commits can be
 * used together; see ubertooth_report_api_mismatch().
 *
 * A refused version is released here: the board is sent a STOP and closed, so
 * a tool that returns after this one does not leave it claimed. A refused
 * image is closed without the STOP, because no mode has been started and the
 * image may be one that does not answer STOP.
 *
 * A version that could not be read leaves the board open, because nothing is
 * known about it, and so does an image that could not be read: a development
 * build runs on against firmware older than the info block, and that board
 * cannot be told from one too busy to answer the read. The request that needs
 * the other image reports it either way.
 *
 * Whatever the answer, the caller still owes the handle an ubertooth_close()
 * -- which does nothing on a board already released -- and an
 * ubertooth_free(). */
int ubertooth_check_api(ubertooth_t *ut);
/* Whether the board runs the firmware image these tools drive,
 * bluetooth_rxtx, rather than the other image this tree builds. Returns 0
 * when the command may go on, or -1 for another image, already reported by
 * name with the line that flashes the right one.
 *
 * This is the image half of ubertooth_check_api(), for a tool that drives
 * whatever firmware version a board is carrying but still needs this image's
 * requests: ubertooth-debug, which reads CC2400 registers. Every other tool
 * gets it through ubertooth_check_api() and does not call this.
 *
 * A refused board is closed here, without a STOP, because no mode has been
 * started and the other image may not answer STOP; the caller still owes the
 * handle an ubertooth_close() -- which does nothing on a board already
 * released -- and an ubertooth_free(). A board that would not say which image
 * it runs returns 0 and is left open: firmware older than the info block
 * cannot be told from a board too busy to answer the read, and the request
 * that needs this image reports either one. */
int ubertooth_check_image(ubertooth_t *ut);
/* The environment variable that overrides what the build decides about
 * firmware of another API version: exactly "1" runs on, exactly "0" refuses,
 * and anything else -- including unset and empty -- leaves the build's own
 * answer. Named here so the library and the tests cannot drift apart. */
#define UBERTOOTH_ALLOW_API_MISMATCH_VAR "UBERTOOTH_ALLOW_API_MISMATCH"
/* Reports firmware the host tools were not built with: it names the version
 * the board reports and the version these tools require, then says which side
 * to replace (flash the firmware when the board is older, install matching
 * host tools when it is newer).
 *
 * Returns -1 when the command must stop, having said so as an error, and 0
 * when it may go on, having said so as a warning and named what allowed it.
 * A release build stops; a build from a working checkout goes on, which is
 * what makes lab and debugging work possible without a reflash first. Setting
 * UBERTOOTH_ALLOW_API_MISMATCH to 1 goes on whatever the build, and to 0
 * stops whatever the build.
 *
 * It only reports and decides, so a caller that checks the version itself
 * rather than through ubertooth_check_api() - ubertooth-util's -I, which must
 * still talk to any firmware - behaves as every other tool does.
 *
 * selector is the board the user named (ubertooth_t.selector), so the example
 * command reflashes that board and no other; NULL where they named none. */
int ubertooth_report_api_mismatch(uint16_t version, const char* selector);
/* Records the firmware API version read from a board, so that a later report
 * about a request that board refused can quote it. board identifies the board
 * it came from (its USB handle), and the version is dropped when that board
 * is released, so a process that opens several boards in turn never quotes
 * the one before. ubertooth_get_api() and ubertooth_close() call these; no
 * tool needs to. */
void ubertooth_note_api_version(const void* board, uint16_t version);
void ubertooth_forget_api_version(const void* board);
/* Whether the board behind an open handle has left the USB bus. board is the
 * board's USB handle, as ubertooth_note_api_version() takes it. The bus is
 * listed and no request is sent, so this is safe to call on a board that has
 * stopped answering. 0 for a board that is still there, and for anything this
 * cannot tell: a bus that could not be listed is not evidence that a board
 * has gone. */
int ubertooth_device_gone(const void* board);
/* Reports a request the board's firmware does not implement, which it refuses
 * by stalling. what names the request and reads after "could not" ("set the
 * channel"). code is the libusb error code to quote, or 0 where the stall
 * arrived as a transfer status and there is no libusb code for it.
 *
 * The report names the board's firmware API version alongside the one these
 * tools require, where a version has been read: firmware from another commit
 * is the usual reason a request is missing, and the reflash is the usual fix.
 *
 * A board that has left the USB bus stalls requests too on some hosts, and
 * its firmware is not the reason: such a board gets one line saying it is no
 * longer attached, and no word about firmware. */
void ubertooth_report_unsupported_request(const char* what, int code);
/* Stops the capture after this many seconds, through the same flag Ctrl-C
 * uses. The handle is not read: the alarm stops whichever capture is running,
 * and a signal handler cannot follow a pointer to a handle. */
void ubertooth_set_timeout(ubertooth_t* ut, int seconds);

/* A timeout for the quiet read below, for the callers that would rather give
 * up than wait: long enough for a board that answers, short enough not to
 * stall a caller reading a board that may be absent, still starting or wedged.
 * Every caller in this tree wants exactly that. */
#define UBERTOOTH_SERIAL_READ_QUIET_MS 200u

/* Reads an open device's serial number into out as the hex string -s prints;
 * size must be at least UBERTOOTH_SERIAL_STR_LEN. Returns 0 on success, and
 * leaves out as the caller passed it on failure.
 *
 * This is the same read as cmd_get_serial() without its error messages, with
 * a caller-chosen timeout, for callers that must stay quiet: a board that is
 * still starting, or wedged, must not print an error of its own while another
 * board is being selected, or while the board the user is resetting is read
 * to follow it across the reset. */
int ubertooth_read_serial_string(struct libusb_device_handle* devh, char* out,
                                 size_t size, unsigned timeout_ms);

/* Whether a receive stream on this handle ended because its USB transfer
 * failed -- the board stopped answering, or dropped off the bus -- rather
 * than because the capture was asked to stop. The failure is printed on
 * standard error as it happens, and the capture stops there, so it captured
 * less than was asked for: a tool that streamed has to turn this into a
 * non-zero exit status. Read it after the capture's teardown, because
 * cancelling the transfer can be what brings the failure out. */
int ubertooth_stream_failed(ubertooth_t* ut);

/*
 * Counts what a record's status says the capture may have lost -- the board
 * threw a packet away because its queue was full (FIFO_OVERFLOW), the chip's
 * transfer controller reported an error while a packet was being received
 * (DMA_ERROR), or a block of symbols was written over before the capture loop
 * read it (DMA_OVERFLOW) -- and warns the first time a capture sees each.
 *
 * Every mode's callback calls this for the records it is handed, including the
 * ones it goes on to discard: the board's word about what was lost before this
 * record does not depend on what the host makes of the record itself.
 * Records that carry no readings of their own are left out here rather than by
 * the callers: a keep-alive, and the continuation records of a long low-energy
 * packet, which repeat the readings of the record that started it.
 */
void ubertooth_count_capture_losses(ubertooth_t* ut, const usb_pkt_rx* rx);

/*
 * Watches how loud what the board is hearing is, and warns once per capture
 * when it sits at the top of what the radio can report.
 *
 * A record's signal figure is the power of the packet, or of the block of air,
 * that record holds. The CC2400 reports it on a raw scale the host turns into
 * dBm, and that scale runs out: every raw reading of RSSI_CEILING_RAW or more
 * comes out as 0 dBm, so above it the host can no longer say how loud the
 * signal was, only that it was at least that loud. A receiver held there is
 * being driven harder than it can measure, and on this radio that is also
 * where it stops demodulating reliably: the Basic Rate and low-energy
 * receives turn the board's amplifier and high-gain mode both on, so two
 * boards on one bench reach it at any transmit power.
 *
 * Judged a block of records at a time rather than record by record, because
 * ordinary air reaches the ceiling now and then -- a Wi-Fi burst on the
 * channel does -- while a receiver that is genuinely pinned there stays there.
 * See the comment on the counters in ubertooth_t.
 *
 * Called by the callbacks whose records are samples of the air -- the Basic
 * Rate stream's, which the board sends one of per block of symbols whether or
 * not a packet was in it -- beside ubertooth_count_capture_losses(). NOT by
 * the low-energy callback, nor by the spectrum analyzer or E-GO ones, each for
 * a reason given where the call would have been: a record that exists only
 * because a packet decoded cannot show a receiver that is decoding nothing,
 * and the other two state no signal figure at all.
 *
 * What it judges is decided here and not by the callers: a Basic Rate record
 * that states a signal figure, and nothing else. That is the one kind of
 * record that is a sample of the air rather than the trace of a packet that
 * decoded, and checking it here is what makes a replay safe -- ubertooth-rx
 * -i will replay a dump written by any tool, whatever records are in it.
 */
void ubertooth_watch_signal_level(ubertooth_t* ut, const usb_pkt_rx* rx);

/*
 * How many stretches of this capture were at the top of the radio's range: one
 * per block of records ubertooth_watch_signal_level() judged to be there. 0
 * for a capture that was never pinned. The running warning is said once, so
 * this is what tells one bad stretch from a capture that was deaf throughout;
 * ubertooth_report_capture_losses() prints it when the capture ends, beside
 * the loss totals.
 */
uint32_t ubertooth_signal_ceiling_blocks(ubertooth_t* ut);

/*
 * How many gaps a capture on this handle has: times the board had to drop a
 * packet because its queue was full. The board reports it on the first record
 * that gets through after a gap, so this counts the gaps, not the packets lost
 * in them. A tool that captured reports the total when the capture ends; the
 * first gap is also warned about as it happens, so a long capture says so
 * while it runs.
 */
uint32_t ubertooth_queue_gaps(ubertooth_t* ut);

/*
 * How many capture transfer errors a capture on this handle saw: times the
 * board said the chip's transfer controller reported an error on the channel a
 * capture receives into. The record the board marked was kept, so each one is a
 * place the capture may be wrong, or missing what the fault cut short.
 * ubertooth-util(1) -w reports the board's own count of these since power-on;
 * this one counts only what reached this capture.
 */
uint32_t ubertooth_transfer_errors(ubertooth_t* ut);

/*
 * How many times the board told this capture that a block of symbols was
 * written over before its capture loop read it. Nothing ever looked in that
 * block for a packet, which is why this loss has no packet to name. The count
 * is of the places the board marked, not of the blocks lost at them: one mark
 * can stand for several.
 */
uint32_t ubertooth_symbol_overwrites(ubertooth_t* ut);

/*
 * Says what a capture is missing, once, where the user sees it after the
 * packets have scrolled past: a tool calls this when its capture ends. A
 * capture that lost nothing prints nothing. The counts are of the places a
 * loss was marked, not of the packets lost at them.
 */
void ubertooth_report_capture_losses(ubertooth_t* ut);

/* Whether the STOP request that ends a capture was refused on this handle.
 * Such a board is left running the mode the capture started, and no other
 * tool can open it until it is reset or re-plugged, so the run did not do
 * what was asked: a tool that streamed has to turn this into a non-zero exit
 * status, keeping whatever it captured. libubertooth has already named the
 * refused request on standard error, so a tool adds no message of its own.
 * Read it after ubertooth_stop(), which is what sends the request. */
int ubertooth_stop_failed(ubertooth_t* ut);

int ubertooth_bulk_init(ubertooth_t* ut);
void ubertooth_bulk_wait(ubertooth_t* ut);
int ubertooth_bulk_receive(ubertooth_t* ut, rx_callback cb, void* cb_args);
/* Starts the thread that completes the capture's USB transfers. Returns 0, or
 * a positive errno, as pthread_create() does: test it for "!= 0", never for
 * "< 0". Nothing else completes the transfers, so a capture started without
 * the thread waits forever with nothing arriving. */
int ubertooth_bulk_thread_start();
/* Stops that thread and waits for it. Safe to call when no thread is running,
 * so a teardown after a start that failed does not wait on a thread that was
 * never created. */
void ubertooth_bulk_thread_stop();

/* Reports a UBERTOOTH_POLL reply that held part of a record, which means the
 * capture is losing packets. For the tools that poll for their packets rather
 * than read them through the bulk endpoint (ubertooth-btle, ubertooth-ego):
 * pass every reply length the poll returned that was not a whole record. A
 * reply of one byte or none is the board's "nothing queued" answer and is
 * ignored; anything else is reported, on standard error, once per handle. */
void ubertooth_warn_if_poll_reply_short(ubertooth_t* ut, int len);

/* One record of a saved capture (ubertooth-rx -d, ubertooth-dump -f): the
 * host time the packet was received, big-endian, then one usb_pkt_rx. */
#define RX_FILE_RECORD_LEN ((int)(sizeof(uint32_t) + PKT_LEN))

/* Replays a saved capture, calling cb for each whole record. Returns 0 at a
 * clean end of file, minus an errno on a read error, and the number of bytes
 * of a last record that was cut short. A replay the user has asked to stop
 * also returns 0: it ends where it was asked to, which is not an error. */
int stream_rx_file(ubertooth_t* ut,FILE* fp, rx_callback cb, void* cb_args);
/* Reports a stream_rx_file() result on stderr, naming the file, and returns
 * the exit status the tool should use: 1 for anything but a clean end. */
int rx_file_report(int result, const char* filename);

/* Returned by a receive path whose stream ended because the USB transfer
 * failed, already reported, rather than because the capture was stopped. */
#define UBERTOOTH_ERROR_STREAM_FAILED (-2001)

/* Dumps received symbols until the capture is stopped. Returns 0 when it was
 * stopped normally, or a negative error, already reported, if the capture
 * could not be started -- including a collecting thread that could not be
 * created -- or its stream died part-way. */
int rx_dump(ubertooth_t* ut, int full);

/* The same dump from the board's generic receive, which demodulates with the
 * CC2400 configuration already in the radio instead of programming one; the
 * caller is expected to have written that configuration itself. A separate
 * entry point rather than a flag on rx_dump(), so the exported signature
 * programs outside this tree compile against does not change. */
int rx_dump_generic(ubertooth_t* ut, int full);

/* Returned by rx_afh() when the initial detection phase (-t) ended without a
 * single packet from the piconet, so there is no map to report. */
#define UBERTOOTH_AFH_NO_PACKETS (-2000)

/* Detect the piconet's AFH map, then monitor it for changes until stopped.
 * With a nonzero timeout, the detection phase runs for that many seconds and
 * the map is printed before monitoring starts. pn must be a piconet whose map
 * is still empty, as btbb_piconet_new() leaves it: "no packet was received" is
 * read from the map after the detection phase.
 *
 * A user interrupt during the detection phase ends the whole run: the map
 * found so far is printed and monitoring is not started. Only the timeout
 * moves the run on to monitoring.
 *
 * Returns 0 when the monitor was stopped normally, UBERTOOTH_AFH_NO_PACKETS
 * when the detection phase saw no packet (nothing is printed and monitoring is
 * not started), or a negative error: -1 if libbtbb could not be initialised or
 * the collecting thread could not be created, the request or receive error,
 * already reported, if a phase could not run, or
 * UBERTOOTH_ERROR_STREAM_FAILED if a phase's stream died part-way. A phase
 * that lost its stream is not a quiet piconet, so it is never reported as
 * "no packets". */
int rx_afh(ubertooth_t* ut, btbb_piconet* pn, int timeout);
/* The one-line-per-second form of the above (ubertooth-afh -r), with its own
 * receive loop and no detection phase, so the timeout is unused. Returns 0
 * when it was stopped normally, or a negative error, already reported: -1 if
 * libbtbb could not be initialised or the collecting thread could not be
 * created, or the request or transfer error if the receive could not be
 * started. */
int rx_afh_r(ubertooth_t* ut, btbb_piconet* pn, int timeout);

void ubertooth_unpack_symbols(const uint8_t* buf, char* unpacked);

#endif /* __UBERTOOTH_H__ */
