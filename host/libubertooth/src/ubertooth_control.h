/*
 * Copyright 2010-2012 Michael Ossmann, Dominic Spill
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

#ifndef __UBERTOOTH_CONTROL_H__
#define __UBERTOOTH_CONTROL_H__

#include <libusb-1.0/libusb.h>

#if defined __MACH__
#include <CoreServices/CoreServices.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#define htobe32 EndianU32_NtoB
#define be32toh EndianU32_BtoN
#define le32toh EndianU32_LtoN
#define htobe64 EndianU64_NtoB
#define be64toh EndianU64_BtoN
#define htole16 EndianU16_NtoL
#define htole32 EndianU32_NtoL
#else
#include <endian.h>
#endif

#include <stdio.h>

#define u8 uint8_t
#define u16 uint16_t
#define u32 uint32_t
#define u64 uint64_t

#include "ubertooth_interface.h"

#define U0_VENDORID    0x1d50
#define U0_PRODUCTID   0x6000
#define U1_VENDORID    0x1d50
#define U1_PRODUCTID   0x6002
#define TC13_VENDORID  0xffff
#define TC13_PRODUCTID 0x0004

/* What an Ubertooth One enumerates as while it sits in the DFU bootloader,
 * which is where a flash puts it. Device selection needs these so that a
 * board being flashed is not reported as a board that went away.
 *
 * ubertooth-tools/src/dfu.h spells them out again for the DFU tool, which
 * must compile without libusb and so cannot include this header. Both copies
 * reach ubertooth-dfu.c, which includes both headers, so changing one and not
 * the other is a macro redefined with a different value: the compiler says
 * so, and the two cannot drift apart in silence. */
#define U1_DFU_VENDORID  0x1d50
#define U1_DFU_PRODUCTID 0x6003

#define DATA_IN     (0x82 | LIBUSB_ENDPOINT_IN)
#define DATA_OUT    (0x05 | LIBUSB_ENDPOINT_OUT)
#define TIMEOUT     20000

/* RX USB packet parameters */
#define PKT_LEN       64
#define SYM_LEN       50
#define BANK_LEN      (SYM_LEN * 8)

/* Length of an AFH channel map in bytes: one bit per Basic Rate channel, 79
 * channels in 10 bytes (the last bit is unused). The firmware uses the same
 * length (firmware/bluetooth_rxtx/bluetooth.h). */
#define AFH_MAP_BYTES 10

/* Most data bytes a vendor control request carries, and so the most
 * ubertooth_cmd_async() accepts. The longest request in use is the AFH map;
 * 64 matches the control endpoint's packet size. */
#define UBERTOOTH_CMD_MAX_DATA 64

#ifndef MAX
#define MAX(a,b) ((a)>(b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b) ((a)<(b) ? (a) : (b))
#endif

void show_libusb_error(int error_code);
int ubertooth_cmd_sync(struct libusb_device_handle* devh,
	                   uint8_t type,
	                   uint8_t command,
	                   uint8_t* data,
	                   uint16_t size);
int ubertooth_cmd_async(struct libusb_device_handle* devh,
	                    uint8_t type,
	                    uint8_t command,
	                    uint8_t* data,
	                    uint16_t size);

void cmd_trim_clock(struct libusb_device_handle* devh, uint16_t offset);
void cmd_fix_clock_drift(struct libusb_device_handle* devh, int16_t ppm);
int cmd_ping(struct libusb_device_handle* devh);
int cmd_rx_syms(struct libusb_device_handle* devh);
/* The capture-mode request without a report of its own; see
 * cmd_set_channel_quiet() below. The tools that set a capture up name this
 * request themselves, so they use this form and print the one line. */
int cmd_rx_syms_quiet(struct libusb_device_handle* devh);
int cmd_tx_syms(struct libusb_device_handle* devh);
int cmd_specan(struct libusb_device_handle* devh, u16 low_freq, u16 high_freq);
int cmd_led_specan(struct libusb_device_handle* devh, u16 rssi_threshold);
int cmd_set_usrled(struct libusb_device_handle* devh, u16 state);
/* A read whose reply is shorter than expected. libubertooth has already
 * printed the whole error, "error: <what> read failed: board returned <n> of
 * <want> bytes", so a caller should not add its own "read failed" line. The
 * value is outside libusb's error range. */
#define UBERTOOTH_ERROR_SHORT_REPLY (-1000)

/* The fixed-size reads below return their value (>= 0), or a negative error:
 * a libusb error, or UBERTOOTH_ERROR_SHORT_REPLY, never a value made from
 * stale bytes. cmd_get_clock() and cmd_get_access_address() return an
 * unsigned value, so an error comes back as that negative code cast to u32;
 * cmd_read_access_address() reports errors separately. */
int cmd_get_usrled(struct libusb_device_handle* devh);
int cmd_set_rxled(struct libusb_device_handle* devh, u16 state);
int cmd_get_rxled(struct libusb_device_handle* devh);
int cmd_set_txled(struct libusb_device_handle* devh, u16 state);
int cmd_get_txled(struct libusb_device_handle* devh);
/* Reads the 32-bit microcontroller part ID into *part_id. Returns 0 on
 * success or a negative libusb error code: the transfer error, LIBUSB_ERROR_IO
 * for a short reply, or LIBUSB_ERROR_OTHER for a nonzero device status. */
int cmd_get_part_id(struct libusb_device_handle* devh, u32 *part_id);
/* Returns the part ID (0 or greater) or a negative libusb error code, as for
 * cmd_get_part_id(). A part ID with bit 31 set cannot be represented and
 * returns LIBUSB_ERROR_OVERFLOW; use cmd_get_part_id() for the full range. */
int cmd_get_partnum(struct libusb_device_handle* devh);
/* The firmware's serial reply: a status byte then the 16-byte number. */
#define UBERTOOTH_SERIAL_LEN 17
/* That number as the lowercase hex string -s prints and -U matches: two hex
 * digits per byte, plus the NUL. */
#define UBERTOOTH_SERIAL_STR_LEN 33
/* serial must hold UBERTOOTH_SERIAL_LEN bytes. */
int cmd_get_serial(struct libusb_device_handle* devh, u8 *serial);
int cmd_set_modulation(struct libusb_device_handle* devh, u16 mod);
/* The modulation request without a report of its own; see
 * cmd_set_channel_quiet() below. A capture states the modulation it wants
 * after its mode request, as it states its channel, and names the request
 * itself in the one line it prints. */
int cmd_set_modulation_quiet(struct libusb_device_handle* devh, u16 mod);
int cmd_get_modulation(struct libusb_device_handle* devh);
int cmd_set_isp(struct libusb_device_handle* devh);
int cmd_reset(struct libusb_device_handle* devh);
int cmd_stop(struct libusb_device_handle* devh);
int cmd_set_paen(struct libusb_device_handle* devh, u16 state);
int cmd_set_hgm(struct libusb_device_handle* devh, u16 state);
int cmd_tx_test(struct libusb_device_handle* devh);
int cmd_flash(struct libusb_device_handle* devh);
int cmd_get_palevel(struct libusb_device_handle* devh);
int cmd_set_palevel(struct libusb_device_handle* devh, u16 level);
int cmd_get_channel(struct libusb_device_handle* devh);
int cmd_set_channel(struct libusb_device_handle* devh, u16 channel);
/* The channel and address requests without a report of their own: they return
 * 0, or the libusb error code, and print nothing for a refused transfer. A
 * caller that names the request itself (the ubertooth-* tools' shared
 * report_refused_request()) then prints the whole failure as one line, where
 * the reporting forms above would add a second. */
int cmd_set_channel_quiet(struct libusb_device_handle* devh, u16 channel);
int cmd_get_rangeresult(struct libusb_device_handle* devh, rangetest_result *rr);
/* Read the watchdog's state and the breadcrumb a watchdog reset left behind:
 * why the board last started, how many times it has reset itself without
 * being asked to, and the mode and wait it was in when it last did. */
int cmd_get_watchdog(struct libusb_device_handle* devh, watchdog_info *info);
/* The same two reads without a report of their own: they return 0, or a
 * negative error, and print nothing. A board part-way through a mode stalls
 * the odd control request, and ubertooth-util -n makes both of these reads
 * several times a second while a range test runs, so one failed read there is
 * a poll that learned nothing rather than a failure to report. */
int cmd_get_rangeresult_quiet(struct libusb_device_handle* devh,
                              rangetest_result *rr);
int cmd_get_watchdog_quiet(struct libusb_device_handle* devh,
                           watchdog_info *info);
/* Read how many times each shape of wait on the radio has given up since the
 * board started. The waits are bounded and carry on quietly, so these counts
 * are what tells a half-working radio from a quiet channel. */
int cmd_get_radio_timeouts(struct libusb_device_handle* devh,
                           radio_timeouts *timeouts);
int cmd_range_test(struct libusb_device_handle* devh);
int cmd_repeater(struct libusb_device_handle* devh);
/* Read the firmware revision string / compile info string into a buffer of
 * size bytes (always terminated; a longer string keeps its leading part).
 * Return 0 on success or a negative libusb error code: the transfer error,
 * LIBUSB_ERROR_IO for a short or malformed reply, or
 * LIBUSB_ERROR_INVALID_PARAM for a NULL or zero-size buffer. On error the
 * buffer holds an empty string. */
int cmd_get_firmware_revision(struct libusb_device_handle* devh, char *revision,
                              size_t size);
int cmd_get_firmware_compile_info(struct libusb_device_handle* devh,
                                  char *compile_info, size_t size);
/* Read the firmware info block: which image the board runs, the revision it
 * was built from, and the firmware API version it reports (see
 * ubertooth_interface.h). The same 64 bytes a firmware file carries, so a
 * board and a file are described the same way. Returns 0 on success, or a
 * negative value: the transfer error, UBERTOOTH_ERROR_SHORT_REPLY for a reply
 * with fewer than 64 bytes in it, or LIBUSB_ERROR_IO for 64 bytes that are
 * not a block this host can read. On error *info is zeroed.
 *
 * LIBUSB_ERROR_PIPE means the firmware does not implement the request, which
 * is every image built before the block existed; it is the one failure this
 * call leaves unreported, for the caller to handle or describe. */
int cmd_get_firmware_info(struct libusb_device_handle* devh,
                          struct ubertooth_fw_info *info);
/* The same read with every failure left unreported, for a caller that asks
 * out of interest rather than need: the image check every tool makes while it
 * opens a board (ubertooth_check_api()) carries on without the block rather
 * than printing a line about a board that may be perfectly fine. */
int cmd_get_firmware_info_quiet(struct libusb_device_handle* devh,
                                struct ubertooth_fw_info *info);
/* As above, but report a failure only as the text "error: <code>" in the
 * buffer; prefer the int-returning forms. */
void cmd_get_rev_num(struct libusb_device_handle* devh, char *version, u8 len);
void cmd_get_compile_info(struct libusb_device_handle* devh, char *compile_info, u8 len);
int cmd_get_board_id(struct libusb_device_handle* devh);
int cmd_set_squelch(struct libusb_device_handle* devh, u16 level);
int cmd_get_squelch(struct libusb_device_handle* devh);
int cmd_set_bdaddr(struct libusb_device_handle* devh, u64 bdaddr);
/* See cmd_set_channel_quiet(). A transfer the board truncated carries its own
 * complete line and returns UBERTOOTH_ERROR_SHORT_REPLY, as the reads do. */
int cmd_set_bdaddr_quiet(struct libusb_device_handle* devh, u64 bdaddr);
int cmd_set_syncword(struct libusb_device_handle* devh, u64 syncword);
int cmd_next_hop(struct libusb_device_handle* devh, u16 clk);
int cmd_start_hopping(struct libusb_device_handle* devh, int clkn_offset, int clk100ns_offset);
/* Set CLKN to a value of the caller's choosing. No tool in this tree does:
 * a tool that wants the board on another clock asks it to start hopping by
 * the difference, which is what the firmware adds to the clock it is running,
 * so nothing has to be reset under a capture. Exported for callers outside
 * the tree. */
int cmd_set_clock(struct libusb_device_handle* devh, u32 clkn);
/* See cmd_set_channel_quiet(). A transfer the board truncated carries its own
 * complete line and returns UBERTOOTH_ERROR_SHORT_REPLY, as the reads do. */
int cmd_set_clock_quiet(struct libusb_device_handle* devh, u32 clkn);
/* Reads CLKN, the board's own clock, into *clock: 312.5 us ticks since the
 * board started, plus whatever offsets it has been given since. Returns 0, or
 * a negative error as the reads above do. The whole 32-bit range is a clock,
 * so an error has to come back separately; see cmd_get_clock() below. */
int cmd_read_clock(struct libusb_device_handle* devh, u32 *clock);
/* See cmd_set_channel_quiet(). A reply the board truncated carries its own
 * complete line and returns UBERTOOTH_ERROR_SHORT_REPLY. */
int cmd_read_clock_quiet(struct libusb_device_handle* devh, u32 *clock);
/* Deprecated: an error is indistinguishable from a clock. */
uint32_t cmd_get_clock(struct libusb_device_handle* devh);
int cmd_set_afh_map(struct libusb_device_handle* devh, u8* afh_map);
int cmd_clear_afh_map(struct libusb_device_handle* devh);
int cmd_btle_sniffing(struct libusb_device_handle* devh, uint8_t do_follow);
/* Returns 0 and writes *access_address, or a negative libusb error. */
int cmd_read_access_address(struct libusb_device_handle* devh, u32 *access_address);
/* Deprecated: an error is indistinguishable from an address. */
u32 cmd_get_access_address(struct libusb_device_handle* devh);
int cmd_set_access_address(struct libusb_device_handle* devh, u32 access_address);
int cmd_get_crc_verify(struct libusb_device_handle* devh);
int cmd_set_crc_verify(struct libusb_device_handle* devh, int verify);
int cmd_poll(struct libusb_device_handle* devh, usb_pkt_rx *p);
int cmd_btle_promisc(struct libusb_device_handle* devh);
int cmd_read_register(struct libusb_device_handle* devh, u8 reg);
/* Write one CC2400 register. The board takes the value as written: these are
 * the radio's own registers, so nothing here knows which values a register
 * accepts. Returns 0, or a negative libusb error already reported. */
int cmd_write_register(struct libusb_device_handle* devh, u8 reg, u16 value);
/* Start the generic receive, which demodulates with the CC2400 configuration
 * already in the radio instead of programming one. Returns 0, or a negative
 * libusb error already reported. */
int cmd_rx_generic(struct libusb_device_handle* devh);
int cmd_btle_peripheral(struct libusb_device_handle* devh, u8 *mac_address);
int cmd_le_set_adv_data(struct libusb_device_handle* devh, uint8_t *data, unsigned data_len);
int cmd_btle_set_target(struct libusb_device_handle* devh, uint8_t *mac_address, uint8_t mac_mask);
int cmd_set_jam_mode(struct libusb_device_handle* devh, int mode);
int cmd_ego(struct libusb_device_handle* devh, int mode);
int cmd_afh(struct libusb_device_handle* devh);
int cmd_hop(struct libusb_device_handle* devh);
int cmd_cancel_follow(struct libusb_device_handle* devh);
int cmd_rfcat_subcmd(struct libusb_device_handle* devh, int cmd, uint8_t *body, size_t body_len);
int cmd_xmas(struct libusb_device_handle* devh);
/* Starts the board-side identify blink (firmware API UBERTOOTH_API_IDENTIFY
 * and later). Returns 0, or a negative libusb error; LIBUSB_ERROR_PIPE means
 * the firmware does not know the request and is reported by the caller, not
 * here. */
int cmd_identify(struct libusb_device_handle* devh);

#endif /* __UBERTOOTH_CONTROL_H__ */
