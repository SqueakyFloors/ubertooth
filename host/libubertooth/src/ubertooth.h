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
 * RX_XFER_OURS is zero so a zeroed handle starts out owning nothing. */
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

	/* Set to stop the capture. Written from a signal handler (Ctrl-C, the
	 * -t alarm) and from the poll thread (a stream that died), and read by
	 * the capture's own loop while both of those can run, so the loop has
	 * to reload it rather than keep a copy of it. */
	volatile uint8_t stop_ubertooth;
	uint64_t abs_start_ns;
	uint32_t start_clk100ns;
	uint64_t last_clk100ns;
	uint64_t clk100ns_upper;

	btbb_pcap_handle* h_pcap_bredr;
	lell_pcap_handle* h_pcap_le;
	btbb_pcapng_handle* h_pcapng_bredr;
	lell_pcapng_handle* h_pcapng_le;

	/* Whether abs_start_ns and start_clk100ns hold the first packet of a
	 * capture yet: a first packet at clk100ns 0 is a valid start, not a
	 * missing one. Last, so the offsets above do not move. */
	uint8_t clk100ns_started;

	/* Whether libusb is holding rx_xfer, one of enum rx_xfer_state. Next to
	 * the byte above so the two share its padding. */
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

	/* Whether the host clock running ahead of the reconstructed times has
	 * already been reported, so it is said once per capture. */
	uint8_t clk100ns_gap_warned;

	/* Set when the receive stream ended because its USB transfer failed. It
	 * holds the libusb transfer status, or LIBUSB_TRANSFER_ERROR for a
	 * transfer libusb would not put back in flight. It stays 0 while a
	 * capture runs and after every ordinary end: a timeout, an interrupt,
	 * the teardown's own cancellation. So a capture that ends with this set
	 * stopped where the stream died, and holds less than was asked for.
	 * Read it through ubertooth_stream_failed().
	 *
	 * Written on the poll thread, like stop_ubertooth above, which is why
	 * it is volatile too. A caller reads it once that thread is joined.
	 *
	 * It is never cleared once set. On a handle that runs more than one
	 * stream (ubertooth-afh detects a map, then monitors it) it therefore
	 * means "a stream on this handle failed", which is all a tool's exit
	 * status has to say anyway. */
	volatile int rx_stream_status;
} ubertooth_t;

typedef void (*rx_callback)(ubertooth_t* ut, void* args);

typedef struct {
	unsigned allowed_access_address_errors;
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
void register_cleanup_handler(ubertooth_t* ut, int do_exit);
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
void ubertooth_close(ubertooth_t* ut);
void ubertooth_stop(ubertooth_t* ut);
void ubertooth_free(ubertooth_t* ut);
int ubertooth_get_api(ubertooth_t *ut, uint16_t *version);
int ubertooth_check_api(ubertooth_t *ut);
void ubertooth_set_timeout(ubertooth_t* ut, int seconds);

/* A timeout for the quiet read below, for the callers that would rather give
 * up than wait: long enough for a board that answers, short enough not to
 * stall a caller reading a board that may be absent, still starting or wedged.
 * Both of this tree's callers want exactly that. */
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
int ubertooth_bulk_thread_start();
void ubertooth_bulk_thread_stop();

/* One record of a saved capture (ubertooth-rx -d, ubertooth-dump -f): the
 * host time the packet was received, big-endian, then one usb_pkt_rx. */
#define RX_FILE_RECORD_LEN ((int)(sizeof(uint32_t) + PKT_LEN))

/* Replays a saved capture, calling cb for each whole record. Returns 0 at a
 * clean end of file, minus an errno on a read error, and the number of bytes
 * of a last record that was cut short. */
int stream_rx_file(ubertooth_t* ut,FILE* fp, rx_callback cb, void* cb_args);
/* Reports a stream_rx_file() result on stderr, naming the file, and returns
 * the exit status the tool should use: 1 for anything but a clean end. */
int rx_file_report(int result, const char* filename);

/* Returned by a receive path whose stream ended because the USB transfer
 * failed, already reported, rather than because the capture was stopped. */
#define UBERTOOTH_ERROR_STREAM_FAILED (-2001)

/* Dumps received symbols until the capture is stopped. Returns 0 when it was
 * stopped normally, or the error, already reported, if the capture could not
 * be started or its stream died part-way. */
int rx_dump(ubertooth_t* ut, int full);
void rx_btle(ubertooth_t* ut);
int rx_btle_file(FILE* fp);
/* Returned by rx_afh() when the initial detection phase (-t) ended without a
 * single packet from the piconet, so there is no map to report. */
#define UBERTOOTH_AFH_NO_PACKETS (-2000)

/* Detect the piconet's AFH map, then monitor it for changes until stopped.
 * With a nonzero timeout, the detection phase runs for that many seconds and
 * the map is printed before monitoring starts. pn must be a piconet whose map
 * is still empty, as btbb_piconet_new() leaves it: "no packet was received" is
 * read from the map after the detection phase.
 *
 * Returns 0 when the monitor was stopped normally, UBERTOOTH_AFH_NO_PACKETS
 * when the detection phase saw no packet (nothing is printed and monitoring is
 * not started), or a negative error: -1 if libbtbb could not be initialised,
 * the receive error, already reported, if a phase could not run, or
 * UBERTOOTH_ERROR_STREAM_FAILED if a phase's stream died part-way. A phase
 * that lost its stream is not a quiet piconet, so it is never reported as
 * "no packets". */
int rx_afh(ubertooth_t* ut, btbb_piconet* pn, int timeout);
void rx_afh_r(ubertooth_t* ut, btbb_piconet* pn, int timeout);

void ubertooth_unpack_symbols(const uint8_t* buf, char* unpacked);

/* How many bytes the decoder (libbtbb's lell_allocate_and_decode()) copies
 * out of the pointer it is handed: 64. It takes no length, so a caller has to
 * know that number. libbtbb keeps it in a private header, MAX_LE_SYMBOLS in
 * lib/src/bluetooth_le_packet.h, so the host has to repeat it here. A
 * received packet holds only 50 bytes (DMA_SIZE), which is why a buffer this
 * long is needed at all. */
#define LE_SYM_LEN 64

/* Copies a received packet into a 64-byte buffer for the decoder and zeroes
 * the 14 bytes the packet does not reach. The caller provides the buffer.
 *
 * The zeroing is the point. The decoder reads the PDU length from byte 5 and
 * then reads the payload and the CRC that follow it. A PDU whose length field
 * claims more than the board captured therefore reads past the 50 bytes that
 * arrived. Here it finds zeros there, not whatever memory sat behind the
 * packet. The capture is still only 50 bytes long; zeroing just stops the
 * decoder presenting the rest as packet data.
 *
 * The buffer is a pointer to an array rather than a plain uint8_t*, so the
 * compiler rejects a caller that passes something shorter. */
void ubertooth_le_symbols(const usb_pkt_rx* rx, uint8_t (*symbols)[LE_SYM_LEN]);

#endif /* __UBERTOOTH_H__ */
