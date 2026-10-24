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

typedef struct {
	/* Ringbuffers for USB and Bluetooth symbols */
	fifo_t* fifo;

	struct libusb_device_handle* devh;
	struct libusb_transfer* rx_xfer;

	uint8_t stop_ubertooth;
	uint64_t abs_start_ns;
	uint32_t start_clk100ns;
	uint64_t last_clk100ns;
	uint64_t clk100ns_upper;
	/* Whether the fields above hold the first packet of a capture yet: a
	 * first packet at clk100ns 0 is a valid start, not a missing one. */
	uint8_t clk100ns_started;

	btbb_pcap_handle* h_pcap_bredr;
	lell_pcap_handle* h_pcap_le;
	btbb_pcapng_handle* h_pcapng_bredr;
	lell_pcapng_handle* h_pcapng_le;
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

void rx_dump(ubertooth_t* ut, int full);
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
 * or the receive error, already reported, if a phase could not run. */
int rx_afh(ubertooth_t* ut, btbb_piconet* pn, int timeout);
void rx_afh_r(ubertooth_t* ut, btbb_piconet* pn, int timeout);

void ubertooth_unpack_symbols(const uint8_t* buf, char* unpacked);

#endif /* __UBERTOOTH_H__ */
