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
} btle_options;

extern uint32_t systime;
extern FILE* infile;
extern FILE* dumpfile;
extern int max_ac_errors;

/* The largest value btbb_init() accepts for max_ac_errors: libbtbb refuses
 * anything above AC_ERROR_LIMIT (lib/src/bluetooth_packet.c), which btbb.h
 * documents as 5. Anything larger needs a syndrome table of several GB. */
#define MAX_AC_ERRORS 5

void print_version();
/* Installs the library's signal handlers, so Ctrl-C, SIGTERM, SIGHUP, SIGQUIT
 * and SIGALRM stop the capture instead of killing the tool with its mode still
 * running on the board. Pass do_exit = 0: see cleanup_exit() in ubertooth.c.
 *
 * The library keeps ut until ubertooth_free() gives it back, so nothing the
 * caller has to unregister: free the handle and the handlers let go of it. */
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
/* Whether the board's firmware is the version these host tools were built
 * with, which is the version they require. Returns 0 when the command may go
 * on, or a negative error, already reported: a version this host refuses to
 * drive, or a version that could not be read at all.
 *
 * A release build refuses any other version. A development build reports it
 * and returns 0, so host tools and firmware from different commits can be
 * used together; see ubertooth_report_api_mismatch().
 *
 * A refused version is released here: the board is sent a STOP and closed, so
 * a tool that returns after this one does not leave it claimed. A version
 * that could not be read leaves the board open, because nothing is known
 * about it. Either way the caller still owes the handle an
 * ubertooth_close() -- which does nothing on a board already released -- and
 * an ubertooth_free(). */
int ubertooth_check_api(ubertooth_t *ut);
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
 * about a request that board refused can quote it. ubertooth_get_api() and
 * ubertooth_report_api_mismatch() call this; no tool needs to. */
void ubertooth_note_api_version(uint16_t version);
/* Reports a request the board's firmware does not implement, which it refuses
 * by stalling. what names the request and reads after "could not" ("set the
 * channel"). code is the libusb error code to quote, or 0 where the stall
 * arrived as a transfer status and there is no libusb code for it.
 *
 * The report names the board's firmware API version alongside the one these
 * tools require, where a version has been read: firmware from another commit
 * is the usual reason a request is missing, and the reflash is the usual fix.
 */
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
