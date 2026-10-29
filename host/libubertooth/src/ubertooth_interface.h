/*
 * Copyright 2012 Dominic Spill
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

#ifndef __UBERTOOTH_INTERFACE_H
#define __UBERTOOTH_INTERFACE_H

#include <stddef.h>
#include <stdint.h>

/*
 * The firmware API version, reported by the firmware as the USB bcdDevice.
 * The host tools require firmware with exactly this version
 * (ubertooth_check_api()), so host and firmware are released together.
 *
 * Both run-time images report it: bluetooth_rxtx and btbr ship from the same
 * commit and answer the same shared requests, so they carry the same number.
 * btbr used to report a literal 1.06 instead, which nothing bumped and which
 * made every tool refuse a board running that image, whatever revision it was
 * built from, except for the version reports, which read any firmware.
 *
 * During development it is bumped in every pull request that changes firmware
 * behaviour the host relies on: a new, removed or changed request, or a
 * changed behaviour of an existing one. Host-only changes leave it alone, so
 * a board carrying firmware from an earlier commit is normally, but not
 * always, refused: commits that leave the firmware interface alone share the
 * number before them, which is why it is the revision the `Firmware version:`
 * line shows beside the version that identifies a build. A bump also updates
 * CHANGELOG.md, COMPATIBILITY.md, TEST_PLAN.md and the pinned strings in the
 * tests.
 *
 * A release moves it to the next major version, to mark that its host tools
 * and firmware only work with each other; the first fork release takes 0x0200
 * (2.00). See docs/source/release_procedure.rst.
 *
 * bcdDevice is binary-coded decimal, so 0x0109 is followed by 0x0110 (shown
 * as 1.10), then 0x0111 (1.11), and so on to 0x0119 (1.19); after 0x0119
 * comes 0x0120, not 0x011a.
 */
#define UBERTOOTH_API_VERSION 0x0154

/*
 * The bootloader's bcdDevice, which is fixed on purpose and is the one version
 * in the tree that may never move.
 *
 * ubertooth-dfu has to work against any board, whatever it was flashed with,
 * because reflashing is how a board running the wrong firmware is put right.
 * So nothing checks this number, and raising it with every release would only
 * give some future tool something to refuse a working bootloader over. It is
 * named here so that it is clear it was left behind deliberately rather than
 * forgotten, which is what the bare 0x0100 in bootloader.cpp looked like.
 */
#define UBERTOOTH_BOOTLOADER_API_VERSION 0x0100

/*
 * The build string every image carries, reported by UBERTOOTH_GET_COMPILE_INFO
 * as a one-byte length and then that many characters. A one-byte length cannot
 * describe more than 255 characters, and the host reads at most 1 + 255 bytes
 * of the reply (cmd_get_firmware_compile_info()), so a longer string is cut to
 * this. Cutting it leaves the host a shortened string it can see is shortened.
 * Casting the real length to a byte instead, as the handlers used to, wrapped
 * it modulo 256 and sent that many characters: a 300-character string went out
 * as its first 44, and a 256-character one as nothing at all.
 */
#define UBERTOOTH_COMPILE_INFO_MAX 255

/*
 * The revision alone, reported by UBERTOOTH_GET_REV_NUM behind two zero bytes
 * and a one-byte length. The same limits apply as above: a one-byte length
 * cannot describe more than 255 characters, the host reads at most 2 + 1 + 255
 * bytes of the reply (cmd_get_firmware_revision()), and that is also all the
 * 258-byte reply buffer in each image's USB shim holds.
 */
#define UBERTOOTH_REVISION_MAX 255

/*
 * The firmware info block: what an image says about itself, in one place.
 *
 * Every image carries 64 bytes at a fixed offset from its start, just past the
 * vector table. firmware/common/fw_info.c fills them in and
 * firmware/common/sections.ld puts them there, so an image cannot leave the
 * block out.
 *
 * ubertooth-dfu reads the block straight out of a firmware file at that
 * offset. That is how it names the image and the revision a file carries, and
 * how it refuses a file that is not an image from this tree before it erases
 * anything. The firmware returns the same 64 bytes through
 * UBERTOOTH_GET_FW_INFO, so a board and a file are described the same way.
 *
 * The offset has to clear the vector table. Its eighth word, at offset 0x1c,
 * is the checksum the LPC boot ROM reads over the table, so the block must
 * sit well past that; the table is 204 bytes (51 words) today, and 256 is the
 * next round number above it. sections.ld fails the link if the table ever
 * grows that far.
 */
#define UBERTOOTH_FW_INFO_OFFSET 0x100
/* The 8 bytes a block starts with, no terminator. */
#define UBERTOOTH_FW_INFO_MAGIC "UBTFINFO"
#define UBERTOOTH_FW_INFO_MAGIC_LEN 8
/* The layout below. A block with any other number is not read. */
#define UBERTOOTH_FW_INFO_FORMAT 1
#define UBERTOOTH_FW_INFO_IMAGE_LEN 16
#define UBERTOOTH_FW_INFO_REVISION_LEN 32
#define UBERTOOTH_FW_INFO_LENGTH 64

/*
 * The block as it sits in an image: 64 bytes, no padding, the two 16-bit
 * fields little-endian as the ARM stores them. The strings are padded with
 * zeros and are only terminated when they are shorter than their field, so a
 * reader must not treat them as C strings -- ubertooth_fw_info_parse() below
 * copies them out safely.
 */
struct ubertooth_fw_info_block {
	uint8_t magic[UBERTOOTH_FW_INFO_MAGIC_LEN];
	uint16_t format_version;
	uint16_t api_version;
	uint8_t reserved[4];
	char image[UBERTOOTH_FW_INFO_IMAGE_LEN];
	char revision[UBERTOOTH_FW_INFO_REVISION_LEN];
};

/* Where each field starts, for a reader working on raw bytes. */
#define UBERTOOTH_FW_INFO_OFF_MAGIC 0
#define UBERTOOTH_FW_INFO_OFF_FORMAT 8
#define UBERTOOTH_FW_INFO_OFF_API 10
#define UBERTOOTH_FW_INFO_OFF_RESERVED 12
#define UBERTOOTH_FW_INFO_OFF_IMAGE 16
#define UBERTOOTH_FW_INFO_OFF_REVISION 32

/* The layout is the wire format, so a compiler that padded the struct would
 * silently move every field after the padding. */
typedef char ubertooth_fw_info_block_is_64_bytes[
	sizeof(struct ubertooth_fw_info_block) == UBERTOOTH_FW_INFO_LENGTH ? 1 : -1];

/* The block this image carries, and what UBERTOOTH_GET_FW_INFO returns.
 * Defined in firmware/common/fw_info.c, which every image links. */
extern const struct ubertooth_fw_info_block fw_info_block;

/* A block read out of an image or off a board, with both strings terminated. */
struct ubertooth_fw_info {
	uint16_t format_version;
	uint16_t api_version;
	char image[UBERTOOTH_FW_INFO_IMAGE_LEN + 1];
	char revision[UBERTOOTH_FW_INFO_REVISION_LEN + 1];
};

/* Why a block was not read. The caller names the file or the board, so these
 * say only what was wrong with the 64 bytes. */
enum ubertooth_fw_info_result {
	UBERTOOTH_FW_INFO_OK = 0,
	UBERTOOTH_FW_INFO_SHORT = -1,       /* fewer than 64 bytes to read */
	UBERTOOTH_FW_INFO_NO_MAGIC = -2,    /* no block here */
	UBERTOOTH_FW_INFO_FORMAT_UNKNOWN = -3,
	UBERTOOTH_FW_INFO_BAD_TEXT = -4,    /* an empty or unprintable string */
};

/* Copies length bytes of a field into out (size bytes), stopping at the first
 * zero, and terminates it. Returns 0 when what it copied is non-empty and
 * printable, and -1 otherwise: a block read from the wrong offset, or from a
 * file that is not an image, is mostly unprintable. out is terminated either
 * way, so a caller that ignores the result still has a string. */
static inline int ubertooth_fw_info_text(const uint8_t *field, size_t length,
                                         char *out, size_t size)
{
	size_t i;

	if (out == NULL || size == 0)
		return -1;
	out[0] = '\0';
	if (size < length + 1)
		return -1;
	for (i = 0; i < length && field[i] != 0; i++) {
		if (field[i] < 0x20 || field[i] > 0x7e) {
			out[i] = '\0';
			return -1;
		}
		out[i] = (char)field[i];
	}
	out[i] = '\0';
	return i > 0 ? 0 : -1;
}

/*
 * Reads the 64 bytes at raw into *out: the magic, the format version, and
 * both strings. length is how many bytes are there to read. Returns
 * UBERTOOTH_FW_INFO_OK, or a negative enum ubertooth_fw_info_result, and
 * leaves *out zeroed on failure.
 */
static inline int ubertooth_fw_info_parse(const uint8_t *raw, size_t length,
                                          struct ubertooth_fw_info *out)
{
	static const char magic[] = UBERTOOTH_FW_INFO_MAGIC;
	size_t i;

	if (raw == NULL || out == NULL)
		return UBERTOOTH_FW_INFO_SHORT;
	for (i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
	if (length < UBERTOOTH_FW_INFO_LENGTH)
		return UBERTOOTH_FW_INFO_SHORT;
	for (i = 0; i < UBERTOOTH_FW_INFO_MAGIC_LEN; i++) {
		if (raw[UBERTOOTH_FW_INFO_OFF_MAGIC + i] != (uint8_t)magic[i])
			return UBERTOOTH_FW_INFO_NO_MAGIC;
	}
	out->format_version = (uint16_t)(raw[UBERTOOTH_FW_INFO_OFF_FORMAT] |
		(raw[UBERTOOTH_FW_INFO_OFF_FORMAT + 1] << 8));
	if (out->format_version != UBERTOOTH_FW_INFO_FORMAT) {
		uint16_t found = out->format_version;

		for (i = 0; i < sizeof(*out); i++)
			((uint8_t *)out)[i] = 0;
		out->format_version = found;
		return UBERTOOTH_FW_INFO_FORMAT_UNKNOWN;
	}
	out->api_version = (uint16_t)(raw[UBERTOOTH_FW_INFO_OFF_API] |
		(raw[UBERTOOTH_FW_INFO_OFF_API + 1] << 8));
	if (ubertooth_fw_info_text(raw + UBERTOOTH_FW_INFO_OFF_IMAGE,
	                           UBERTOOTH_FW_INFO_IMAGE_LEN,
	                           out->image, sizeof(out->image)) != 0 ||
	    ubertooth_fw_info_text(raw + UBERTOOTH_FW_INFO_OFF_REVISION,
	                           UBERTOOTH_FW_INFO_REVISION_LEN,
	                           out->revision, sizeof(out->revision)) != 0) {
		for (i = 0; i < sizeof(*out); i++)
			((uint8_t *)out)[i] = 0;
		return UBERTOOTH_FW_INFO_BAD_TEXT;
	}
	return UBERTOOTH_FW_INFO_OK;
}

/* One phrase naming what was wrong with a block, for a caller's message. */
static inline const char *ubertooth_fw_info_result_string(int result)
{
	switch (result) {
	case UBERTOOTH_FW_INFO_OK:
		return "the firmware info block was read";
	case UBERTOOTH_FW_INFO_NO_MAGIC:
		return "there is no firmware info block here";
	case UBERTOOTH_FW_INFO_FORMAT_UNKNOWN:
		return "the firmware info block is in a format this host does not know";
	case UBERTOOTH_FW_INFO_BAD_TEXT:
		return "the firmware info block holds no readable image name or revision";
	default:
		return "there are not 64 bytes of firmware info block to read";
	}
}

/*
 * LED spectrum analyzer (ubertooth-util -q N): the threshold is -N dBm. The
 * CC2400 measures RSSI from about -120 to -20 dBm, so N outside 20-120 is
 * either never or always reached. The default, -85 dBm, shows Wi-Fi activity
 * without lighting on noise (the original 2011 default, raw 225 = -85 dBm).
 */
#define UBERTOOTH_LED_SPECAN_N_MIN 20
#define UBERTOOTH_LED_SPECAN_N_MAX 120
#define UBERTOOTH_LED_SPECAN_N_DEFAULT 85

/* How often the identify blink toggles the USR/RX/TX LEDs, in milliseconds:
 * about three on-off blinks a second. */
#define UBERTOOTH_IDENTIFY_TOGGLE_MS 167

#define DMA_SIZE 50

/* The most bytes one captured low-energy packet can be: the access address
 * (4), the PDU header (2), a payload as long as the header's 8-bit length
 * field can claim (255) and the CRC (3).
 *
 * Bluetooth 4.2 Data Length Extension allows 251 payload bytes, so a valid
 * packet is at most 260. The firmware captures whatever length the header
 * claims, so the four bytes above that are carried too rather than cut off
 * mid-record; the decoder then ignores them, because it clamps a payload to
 * the 251 the specification allows. */
#define LE_MAX_USB_PACKET_SIZE 264

#define NUM_BREDR_CHANNELS 79

/* The Bluetooth band: channel 0 is 2402 MHz, channel 78 is 2480 MHz. */
#define BT_FREQ_MIN 2402
#define BT_FREQ_MAX (BT_FREQ_MIN + NUM_BREDR_CHANNELS - 1)

/* What usb_pkt_rx.channel says when the board was tuned outside the Bluetooth
 * band, where there is no channel to name. Every host path that indexes a
 * per-channel array with the field drops a record carrying it, as it does any
 * value above channel 78. */
#define CHANNEL_NONE 0xff

/* The channel a record reports, as the one-byte offset from 2402 MHz that
 * usb_pkt_rx carries. That byte only reaches to channel 78, while the channel
 * request accepts any frequency the radio tunes, 2268 to 2794 MHz: an
 * out-of-band frequency subtracted and narrowed would land on some other
 * channel's number, and 2658 to 2736 MHz would land inside the band, where the
 * host would take it for a real channel and index its per-channel arrays with
 * it. CHANNEL_NONE instead, which is outside the band like every value above
 * 78, so the host's own range checks already drop it.
 *
 * Here rather than in one of the firmware images because both of them fill
 * that field, from four places, and the field is defined here. */
static inline uint8_t ubertooth_channel_byte(uint16_t freq)
{
	if (freq < BT_FREQ_MIN || freq > BT_FREQ_MAX)
		return CHANNEL_NONE;
	return (uint8_t)(freq - BT_FREQ_MIN);
}

/* The centre frequency of each low-energy advertising channel, named by the
 * channel index the Bluetooth Core specification gives it. Index 37 sits at
 * the bottom of the band and index 39 at the top, so that interference in the
 * middle of the band cannot hide all three; index 38 sits in the middle, where
 * the data channels leave a gap.
 *
 * Low energy and classic Bluetooth share the band, so 37 and 39 land on the
 * same two frequencies as the classic edges above. That is why they are
 * written as those constants: one band, named once. */
#define BTLE_ADV_CHANNEL_37_MHZ BT_FREQ_MIN
#define BTLE_ADV_CHANNEL_38_MHZ 2426
#define BTLE_ADV_CHANNEL_39_MHZ BT_FREQ_MAX

/*
 * CLK_TUNE_TIME is the duration in units of 100 ns that we reserve for tuning
 * the radio while frequency hopping.  We start the tuning process
 * CLK_TUNE_TIME * 100 ns prior to the start of an upcoming time slot.
*/
#define CLK_TUNE_TIME   2250
#define CLK_TUNE_OFFSET  200

enum ubertooth_usb_commands {
	UBERTOOTH_PING               = 0,
	UBERTOOTH_RX_SYMBOLS         = 1,
	UBERTOOTH_TX_SYMBOLS         = 2,
	UBERTOOTH_GET_USRLED         = 3,
	UBERTOOTH_SET_USRLED         = 4,
	UBERTOOTH_GET_RXLED          = 5,
	UBERTOOTH_SET_RXLED          = 6,
	UBERTOOTH_GET_TXLED          = 7,
	UBERTOOTH_SET_TXLED          = 8,
	UBERTOOTH_GET_1V8            = 9,
	UBERTOOTH_SET_1V8            = 10,
	UBERTOOTH_GET_CHANNEL        = 11,
	UBERTOOTH_SET_CHANNEL        = 12,
	UBERTOOTH_RESET              = 13,
	UBERTOOTH_GET_SERIAL         = 14,
	UBERTOOTH_GET_PARTNUM        = 15,
	UBERTOOTH_GET_PAEN           = 16,
	UBERTOOTH_SET_PAEN           = 17,
	UBERTOOTH_GET_HGM            = 18,
	UBERTOOTH_SET_HGM            = 19,
	UBERTOOTH_TX_TEST            = 20,
	UBERTOOTH_STOP               = 21,
	UBERTOOTH_GET_MOD            = 22,
	UBERTOOTH_SET_MOD            = 23,
	UBERTOOTH_SET_ISP            = 24,
	UBERTOOTH_FLASH              = 25,
	BOOTLOADER_FLASH             = 26,
	UBERTOOTH_SPECAN             = 27,
	UBERTOOTH_GET_PALEVEL        = 28,
	UBERTOOTH_SET_PALEVEL        = 29,
	UBERTOOTH_REPEATER           = 30,
	UBERTOOTH_RANGE_TEST         = 31,
	UBERTOOTH_RANGE_CHECK        = 32,
	UBERTOOTH_GET_REV_NUM        = 33,
	UBERTOOTH_LED_SPECAN         = 34,
	UBERTOOTH_GET_BOARD_ID       = 35,
	UBERTOOTH_SET_SQUELCH        = 36,
	UBERTOOTH_GET_SQUELCH        = 37,
	UBERTOOTH_SET_BDADDR         = 38,
	UBERTOOTH_START_HOPPING      = 39,
	UBERTOOTH_SET_CLOCK          = 40,
	UBERTOOTH_GET_CLOCK          = 41,
	UBERTOOTH_BTLE_SNIFFING      = 42,
	UBERTOOTH_GET_ACCESS_ADDRESS = 43,
	UBERTOOTH_SET_ACCESS_ADDRESS = 44,
	/* UBERTOOTH_DO_SOMETHING = 45 and UBERTOOTH_DO_SOMETHING_REPLY = 46 were
	 * a pair of developer scratch requests that the firmware answered with a
	 * fixed two bytes. They are gone; the numbers are left unused so that a
	 * request number still means the same thing in any capture or log from an
	 * older build. */
	UBERTOOTH_GET_CRC_VERIFY     = 47,
	UBERTOOTH_SET_CRC_VERIFY     = 48,
	UBERTOOTH_POLL               = 49,
	UBERTOOTH_BTLE_PROMISC       = 50,
	UBERTOOTH_SET_AFHMAP         = 51,
	UBERTOOTH_CLEAR_AFHMAP       = 52,
	UBERTOOTH_READ_REGISTER      = 53,
	UBERTOOTH_BTLE_SLAVE         = 54,
	UBERTOOTH_GET_COMPILE_INFO   = 55,
	UBERTOOTH_BTLE_SET_TARGET    = 56,
	UBERTOOTH_BTLE_PHY           = 57,
	UBERTOOTH_WRITE_REGISTER     = 58,
	UBERTOOTH_JAM_MODE           = 59,
	UBERTOOTH_EGO                = 60,
	UBERTOOTH_AFH                = 61,
	UBERTOOTH_HOP                = 62,
	UBERTOOTH_TRIM_CLOCK         = 63,
	// UBERTOOTH_GET_API_VERSION    = 64,
	UBERTOOTH_WRITE_REGISTERS    = 65,
	UBERTOOTH_READ_ALL_REGISTERS = 66,
	UBERTOOTH_RX_GENERIC         = 67,
	UBERTOOTH_TX_GENERIC_PACKET  = 68,
	UBERTOOTH_FIX_CLOCK_DRIFT    = 69,
	UBERTOOTH_CANCEL_FOLLOW      = 70,
	UBERTOOTH_LE_SET_ADV_DATA    = 71,
	UBERTOOTH_RFCAT_SUBCMD       = 72,
	UBERTOOTH_XMAS               = 73,
	UBERTOOTH_IDENTIFY           = 74,
	UBERTOOTH_GET_FW_INFO        = 75,
	UBERTOOTH_GET_WATCHDOG       = 76,
	UBERTOOTH_GET_RADIO_TIMEOUTS = 77,
};

/*
 * The modes the board can be in. The host asks for one with the request that
 * starts it, and reads the mode back out of the watchdog breadcrumb below, so
 * the numbers are part of the interface rather than private to the firmware.
 */
enum operating_modes {
	MODE_IDLE          = 0,
	MODE_RX_SYMBOLS    = 1,
	MODE_TX_SYMBOLS    = 2,
	MODE_TX_TEST       = 3,
	MODE_SPECAN        = 4,
	MODE_RANGE_TEST    = 5,
	MODE_REPEATER      = 6,
	MODE_LED_SPECAN    = 7,
	MODE_BT_FOLLOW     = 8,
	MODE_BT_FOLLOW_LE  = 9,
	MODE_BT_PROMISC_LE = 10,
	MODE_RESET         = 11,
	MODE_BT_SLAVE_LE   = 12,
	MODE_EGO           = 13,
	MODE_AFH           = 14,
	MODE_RX_GENERIC    = 15,
	MODE_TX_GENERIC    = 16,
	MODE_XMAS          = 17,
	MODE_IDENTIFY      = 18,
	/* Handing the chip to its on-chip programmer, which is a mode of its
	 * own because the board answers the host first and goes off the bus a
	 * moment later, the way MODE_RESET does. */
	MODE_ISP           = 19,
	/* The btbr image has its own states and reports no mode at all. */
	MODE_UNKNOWN       = 255,
};

/*
 * How long the board may go without feeding the watchdog before it resets
 * itself.
 *
 * The number has to clear the longest gap the firmware leaves on purpose,
 * which is 1600 ms: xmas mode sleeps in a WFI until one of its three blink
 * timers fires, and each is set up to 400 + 1200 ms ahead (xmas.c), so a run
 * where all three land near the far end feeds only every 1600 ms. Identify
 * mode waits the same way, on a 167 ms blink. The other long waits all feed
 * while they wait: the repeater's 1000 ms turnaround services USB throughout
 * (cc2400_rangetest.c), the BLE modes feed while they wait for a packet or
 * the next hop, and the transmit loops feed once per burst. The second that
 * reset() spends waiting to be reset is excused by the intent instead.
 *
 * So 3000 ms: nearly double the longest deliberate gap, and still fast enough
 * that a wedged board is back before a person would reach for the cable.
 */
#define UBERTOOTH_WATCHDOG_TIMEOUT_MS 3000

/*
 * Why the board last started. The same bits as the part's reset source
 * register, so a value can be read straight out of it.
 */
enum watchdog_reset_cause {
	WATCHDOG_RESET_POWER_ON  = (1 << 0),
	WATCHDOG_RESET_EXTERNAL  = (1 << 1),
	WATCHDOG_RESET_WATCHDOG  = (1 << 2),
	WATCHDOG_RESET_BROWN_OUT = (1 << 3),
};

/*
 * Where the board was waiting. Every bounded wait in the firmware records its
 * own value here as it starts, so a watchdog reset says which wait the board
 * was in and not only which mode.
 */
enum watchdog_site {
	WATCHDOG_SITE_NONE       = 0, /* not in one of the waits below */
	WATCHDOG_SITE_USB        = 1, /* servicing USB */
	WATCHDOG_SITE_FS_LOCK    = 2, /* waiting for the synthesizer to lock */
	WATCHDOG_SITE_FS_UNLOCK  = 3, /* waiting for it to unlock */
	WATCHDOG_SITE_XOSC       = 4, /* waiting for the 16 MHz crystal oscillator */
	WATCHDOG_SITE_FSM        = 5, /* waiting on the radio's state machine */
	WATCHDOG_SITE_LE_RX      = 6, /* waiting for a BLE packet or the next hop */
	/*
	 * The four processor faults, one site each. A fault handler never
	 * returns, so the board is reset by the watchdog a few seconds later
	 * and these are read back after it reconnects.
	 *
	 * One site for all four would say no more than "a fault". Counting the
	 * handler's blink code used to tell you which fault it was, and the
	 * watchdog now cuts that code off part-way, so the breadcrumb carries
	 * what the blinks did.
	 */
	WATCHDOG_SITE_FAULT_HARD  = 7,  /* hard fault */
	WATCHDOG_SITE_FAULT_MEM   = 8,  /* memory management fault */
	WATCHDOG_SITE_FAULT_BUS   = 9,  /* bus fault */
	WATCHDOG_SITE_FAULT_USAGE = 10, /* usage fault */
	/* The btbr image's die(): firmware that found a state it cannot go on
	 * from, such as a message it cannot parse. Not a processor fault. */
	WATCHDOG_SITE_PANIC       = 11,
	/* Inside the low-energy engine's own interrupt handlers, which is not the
	 * same as the mode loop being held up in its USB service pass: before
	 * these, both read as "servicing USB". */
	WATCHDOG_SITE_LE_SSP      = 12, /* emptying the BLE engine's symbol buffer */
	WATCHDOG_SITE_LE_DMA      = 13, /* in the BLE engine's received-byte interrupt */
	WATCHDOG_SITE_LE_TIMER    = 14, /* in the BLE engine's timer interrupt */
	/*
	 * Two loops that never return and are not waits at all.
	 *
	 * The first is the shared default interrupt handler, which NMI, SVC,
	 * PendSV, SysTick and every peripheral interrupt the firmware does not
	 * handle are aliases of. Reaching it means an interrupt fired that
	 * nothing was written for, so it is worth saying rather than leaving
	 * the record naming whatever the board was doing before.
	 *
	 * The second is the debug UART's transfer error path, which stops the
	 * board with the user LED on.
	 */
	WATCHDOG_SITE_UNHANDLED_IRQ = 15, /* an interrupt nothing handles fired */
	WATCHDOG_SITE_DEBUG_DMA     = 16, /* the debug UART's transfer failed */
	/*
	 * Steps of a USB service pass. A board that stops answering stops inside
	 * that pass, and WATCHDOG_SITE_USB alone covers the whole of it: the
	 * handshakes with the USB controller, the control request the pass is
	 * carrying, and the firmware's own handler for that request. Each of
	 * those is bounded to a few milliseconds on paper, so a pass that holds
	 * the board for three seconds is one of them costing far more than its
	 * bound -- or the time going somewhere none of them accounts for.
	 *
	 * These say which. WATCHDOG_SITE_USB stays the reading for the rest of
	 * the pass: scanning the endpoints for one that needs service, and the
	 * register reads around that.
	 */
	WATCHDOG_SITE_USB_BULK    = 17, /* handing a capture packet to the bulk endpoint */
	WATCHDOG_SITE_USB_DEV     = 18, /* handling a USB bus event: reset, suspend, resume */
	WATCHDOG_SITE_USB_EP      = 19, /* reading an endpoint's status */
	WATCHDOG_SITE_USB_SETUP   = 20, /* reading a control request */
	/*
	 * Running a control request: the firmware's own handler for it. For
	 * Ubertooth's own requests that means code which reaches the radio and
	 * never the USB controller, which is what makes this site worth telling
	 * apart from the ones above. Realizing an endpoint is the exception and
	 * has a site of its own below, so it is not counted here.
	 */
	WATCHDOG_SITE_USB_REQUEST = 21,
	WATCHDOG_SITE_USB_REPLY   = 22, /* writing a control reply */
	/* Waiting for the controller to realize an endpoint: the longest wait
	 * there is on it, and the only one a control request can reach. The
	 * stack asks for it at startup and from SET_CONFIGURATION, which the
	 * host sends as it opens the device and after a bus reset. */
	WATCHDOG_SITE_USB_REALIZE = 23,
	WATCHDOG_SITE_COUNT
};

/*
 * UBERTOOTH_GET_WATCHDOG's reply: eighteen bytes, in the order the fields are
 * listed in watchdog_info below. The first eight are single bytes; the three
 * after them are a two-byte and two four-byte values, each with its low byte
 * first, so there is no padding to agree about between the board and the host.
 *
 * A watchdog reset the firmware asked for (a reset or a flash request) is not
 * counted and does not overwrite reset_mode/reset_site, so those describe the
 * last time the board stopped servicing USB on its own.
 */
#define UBERTOOTH_WATCHDOG_INFO_LEN 18

/* How many bytes UBERTOOTH_RANGE_CHECK answers with, which is the
 * rangetest_result below written out one byte at a time. Shared, so a field
 * added to it is a compile error on the side that was not updated rather than
 * a short reply on the bench. */
#define UBERTOOTH_RANGE_RESULT_LEN 6

/* In watchdog_info.fault_status: the part's "the fault address register holds
 * this fault's address" bits. One of the two is set when fault_addr means
 * something.
 *
 * Which one depends on the register the board recorded. A memory management
 * or bus fault records its own eight-bit status, where bit 7 is that bit; a
 * hard fault that one of them escalated into records the whole configurable
 * fault status, where the memory management byte is the low one and the bus
 * fault byte the next, so that bit lands at 7 or at 15. Neither is set in the
 * hard fault and usage fault statuses, which have no address register. */
#define UBERTOOTH_FAULT_ADDR_VALID     0x0080
#define UBERTOOTH_FAULT_BUS_ADDR_VALID 0x8000

/*
 * UBERTOOTH_GET_RADIO_TIMEOUTS's reply: how many times each shape of wait on
 * the CC2400 has given up since the board started, in this order, each as two
 * bytes with the low byte first.
 *
 * The waits are bounded so that a radio which never reports the state it was
 * asked for cannot hold the board (firmware/common/cc2400_wait.c). Carrying on
 * is deliberate and silent, though: a half-working radio reads as a quiet
 * channel. These counts are what says which it was. Each stops at 65535 rather
 * than wrapping.
 *
 * Both images answer the request, since both wait on the radio the same way.
 * The last count is the exception: only the bluetooth_rxtx image's promiscuous
 * low-energy capture waits for a packet's bytes, so btbr always reports 0 for
 * it.
 */
#define UBERTOOTH_RADIO_TIMEOUTS_LEN 10

typedef struct {
	uint16_t fs_lock;   /* the synthesizer never locked */
	uint16_t fs_unlock; /* it never went back off */
	uint16_t xosc;      /* the 16 MHz crystal never reported stable */
	uint16_t fsm;       /* the state machine never reached a state */
	uint16_t rx_bytes;  /* it stopped delivering a packet's bytes */
} radio_timeouts;

typedef struct {
	uint8_t reset_cause; /* why the board last started: watchdog_reset_cause bits */
	uint8_t armed;       /* 1 while the watchdog is running */
	uint8_t resets;      /* unasked-for watchdog resets since power-on, stopping at 255 */
	uint8_t boots;       /* boots since power-on, stopping at 255 */
	uint8_t reset_mode;  /* the mode it was in at the last one */
	uint8_t reset_site;  /* and the wait it was in: watchdog_site */
	uint8_t mode;        /* the mode it is in now */
	uint8_t site;        /* and the wait it is in now */
	/* Capture transfers the DMA controller reported an error on since
	 * power-on, stopping at 65535. Each one costs the packet that was
	 * arriving; the capture carries on, which is why the count is worth
	 * reporting. */
	uint16_t transfer_errors;
	/* The last processor fault, which reset_site names: the fault status
	 * register the handler read, and the part's fault address register.
	 * Both zero until a fault happens. The address means something only
	 * when the status has UBERTOOTH_FAULT_ADDR_VALID set: two of the four
	 * faults keep no address register, and the two that do say in that bit
	 * whether it holds this fault's address. Zero is a real address, so
	 * that bit and not the address is what has to be read. */
	uint32_t fault_status;
	uint32_t fault_addr;
} watchdog_info;

enum rfcat24_subcommands {
	RFCAT_RESERVED               = 0,
	RFCAT_SET_AA                 = 1,
	RFCAT_CAP_LEN                = 2,
};

/*
 * The sync word an E-GO capture looks for when the host sends no access code
 * of its own. A mode start puts the firmware back to it, and ubertooth-ego
 * names it as the default, so both sides read it from here.
 */
#define EGO_DEFAULT_ACCESS_CODE 0x630f9ffeu

/* The most advertising data an extended advertising PDU can carry, and so the
 * most UBERTOOTH_LE_SET_ADV_DATA accepts. */
#define LE_ADV_MAX_LEN 255

/* The most a legacy ADV_IND can carry, which is what the board transmits in
 * MODE_BT_SLAVE_LE: its six-bit length field counts the six address bytes as
 * well, and the specification caps AdvData at 31 bytes. */
#define ADV_IND_MAX_DATA 31

enum jam_modes {
	JAM_NONE       = 0,
	JAM_ONCE       = 1,
	JAM_CONTINUOUS = 2,
};

enum modulations {
	MOD_BT_BASIC_RATE = 0,
	MOD_BT_LOW_ENERGY = 1,
	MOD_80211_FHSS    = 2,
	MOD_NONE          = 3
};

enum usb_pkt_types {
	BR_PACKET  = 0,
	LE_PACKET  = 1,
	MESSAGE    = 2,
	KEEP_ALIVE = 3,
	SPECAN     = 4,
	LE_PROMISC = 5,
	EGO_PACKET = 6,
	/* A low-energy packet too long for one record: the first record and the
	 * continuations that follow it, in order. See LE_FRAG_* below. */
	LE_PACKET_START = 7,
	LE_PACKET_CONT  = 8,
};

/* Where a fragment's own metadata sits in usb_pkt_rx.reserved, which no other
 * record type uses. reserved[0] is the low 8 bits of the whole packet's
 * length; bit 0 of reserved[1] is its 9th bit, and bits 7..1 are the
 * fragment's position in the chain, counting up from zero. 264 bytes needs
 * nine length bits and six positions, so both fit. */
#define LE_FRAG_TOTAL_HIGH_MASK 0x01
#define LE_FRAG_INDEX_SHIFT 1

enum hop_mode {
	HOP_NONE      = 0,
	HOP_SWEEP     = 1,
	HOP_BLUETOOTH = 2,
	HOP_BTLE      = 3,
	HOP_DIRECT    = 4,
	HOP_AFH       = 5,
};

enum usb_pkt_status {
	DMA_OVERFLOW  = 0x01,
	DMA_ERROR     = 0x02,
	FIFO_OVERFLOW = 0x04,
	CS_TRIGGER    = 0x08,
	RSSI_TRIGGER  = 0x10,
	DISCARD       = 0x20,
};

/*
 * The 100 ns clock a packet is stamped with (usb_pkt_rx.clk100ns below) comes
 * from one of two counters, so it has one of two periods:
 *
 * CLK100NS is 3125 ticks per CLKN tick over the low 20 bits of CLKN (see
 * CLK100NS in firmware/bluetooth_rxtx/ubertooth_clock.h), so it wraps every
 * 3125 * 2^20 = 3276800000 ticks, about 327.68 s. Classic packets, the LE
 * promiscuous mode and E-GO packets carry it.
 *
 * The low-energy physical-layer engine (firmware/bluetooth_rxtx/le_phy.c, the
 * mode behind ubertooth-btle -f and -n) stamps packets with timer1 instead,
 * which runs free at 100 ns a tick and so wraps every 2^32 ticks, about
 * 429.5 s. Nothing in the packet says which counter stamped it: the host knows
 * from the mode it asked for (ubertooth_t.clk100ns_period).
 */
#define CLK100NS_PER_CLKN 3125
#define CLK100NS_PERIOD   ((uint32_t)CLK100NS_PER_CLKN << 20)
#define CLK100NS_TIMER1_PERIOD (1ull << 32)

/*
 * USB packet for Bluetooth RX (64 total bytes)
 */
typedef struct {
	uint8_t  pkt_type;
	uint8_t  status;
	uint8_t  channel;
	uint8_t  clkn_high;
	uint32_t clk100ns;
	int8_t   rssi_max;   // Max RSSI seen while collecting symbols in this packet
	int8_t   rssi_min;   // Min ...
	int8_t   rssi_avg;   // Average ...
	uint8_t  rssi_count; // Number of ... (0 means RSSI stats are invalid)
	uint8_t  reserved[2];
	uint8_t  data[DMA_SIZE];
} usb_pkt_rx;

typedef struct {
	uint64_t address;
	uint64_t syncword;
} bdaddr;

typedef struct {
	/* 0 before a test has completed, 1 for a result, or 2 plus the index of
	 * the first payload byte of the reply that was not the one sent. */
	uint8_t valid;
	uint8_t request_pa;
	uint8_t request_num;
	uint8_t reply_pa;
	uint8_t reply_num;
	/* Whether this run heard a packet that was not its own reply and
	 * discarded it: 0 for none, otherwise 2 plus the index of the first
	 * payload byte that said so, the encoding `valid` uses. Only the first
	 * such packet of a run is recorded. */
	uint8_t discarded;
} rangetest_result;

typedef struct {
	uint16_t synch;
	uint16_t syncl;
	uint16_t channel;
	uint8_t  length;
	/* Only the low three bits reach FREND's PA_LEVEL; unlike
	 * UBERTOOTH_SET_PALEVEL, a larger value is masked rather than refused. */
	uint8_t  pa_level;
	uint8_t  data[DMA_SIZE];
} generic_tx_packet;

/* BTBR USB interface */
enum {
	BTUSB_MSG_START = 'S',
	BTUSB_MSG_CONT = 'C',
	BTUSB_EARLY_PRINT = 'P'
};

/* BTCTL interface */
typedef enum btctl_state_e {
	BTCTL_STATE_STANDBY	= 0,
	BTCTL_STATE_INQUIRY	= 1,
	BTCTL_STATE_PAGE	= 2,
	BTCTL_STATE_CONNECTED	= 3,
	BTCTL_STATE_INQUIRY_SCAN = 4,
	BTCTL_STATE_PAGE_SCAN	= 5,
	BTCTL_STATE_COUNT
} btctl_state_t;

typedef enum btctl_reason_e {
	BTCTL_REASON_SUCCESS	= 0,
	BTCTL_REASON_TIMEOUT	= 1,
	/* REASON_PAGED indicates that device entered CONNECTED state,
	 * but the slave did not answer yet */
	BTCTL_REASON_PAGED	= 2
} btctl_reason_t;

typedef enum {
	BTCTL_DEBUG		= 0,
	/* Any -> Device */
	BTCTL_RESET_REQ		= 20,
	BTCTL_IDLE_REQ		= 21,
	/* Host -> Device */
	BTCTL_SET_FREQ_OFF_REQ	= 22,
	BTCTL_SET_BDADDR_REQ	= 23,
	BTCTL_INQUIRY_REQ	= 24,
	BTCTL_PAGING_REQ	= 25,
	BTCTL_SET_MAX_AC_ERRORS_REQ = 26,
	BTCTL_TX_ACL_REQ	= 27,
	BTCTL_INQUIRY_SCAN_REQ	= 28,
	BTCTL_PAGE_SCAN_REQ	= 29,
	BTCTL_SET_EIR_REQ	= 30,// in a btctl_tx_pkt_t
	BTCTL_SET_AFH_REQ	= 31,
	BTCTL_MONITOR_REQ	= 32,
	/* Hand the chip to its on-chip programmer, queued so that the control
	 * transfer that asked for it is answered first; see BTCTL_RESET_REQ. */
	BTCTL_ISP_REQ		= 33,
	/* Device -> Host */
	BTCTL_RX_PKT		= 40,
	BTCTL_STATE_RESP	= 41
} btctl_cmd_type_t ;

typedef struct btctl_hdr_s {
	uint8_t type;
	uint8_t padding[3];
	uint8_t data[0];
} __attribute__((packed)) btctl_hdr_t;

typedef struct btctl_paging_req_s {
	uint64_t bdaddr;
} btctl_paging_req_t;

typedef struct btctl_set_reg_req_s {
	uint16_t reg;
} btctl_set_reg_req_t;

typedef struct btctl_set_bdaddr_req_s {
	uint64_t bdaddr;
} btctl_set_bdaddr_req_t;

typedef struct btctl_set_afh_req_s {
	uint32_t instant;
	uint8_t mode;
	uint8_t map[10];
} __attribute__((packed)) btctl_set_afh_req_t;

typedef struct btctl_state_resp_s {
	uint8_t state;
	uint8_t reason;
} btctl_state_resp_t;

typedef struct bthdr_s {
	uint8_t lt_addr;
	uint8_t type;
	uint8_t flags;
	uint8_t hec;
} __attribute__((packed)) bbhdr_t;

#define BTHDR_FLOW 0
#define BTHDR_ARQN 1
#define BTHDR_SEQN 2

#define BBPKT_F_HAS_PKT		0
#define BBPKT_F_HAS_HDR		1
#define BBPKT_F_HAS_CRC		2
#define BBPKT_F_GOOD_CRC	3
#define BBPKT_HAS_PKT(p)	(((p)->flags & (1<<BBPKT_F_HAS_PKT))!=0)
#define BBPKT_HAS_HDR(p)	(((p)->flags & (1<<BBPKT_F_HAS_HDR))!=0)
#define BBPKT_HAS_CRC(p)	(((p)->flags & (1<<BBPKT_F_HAS_CRC))!=0)
#define BBPKT_GOOD_CRC(p)	(((p)->flags & (1<<BBPKT_F_GOOD_CRC))!=0)

#define MAX_ACL_PACKET_SIZE 344

typedef struct {
	uint32_t clkn;		// Clkn sampled at start of rx
	uint8_t chan;		// Channel number
	uint8_t flags;		// Decode flags
	uint16_t data_size;	// Size of bt_data field
	bbhdr_t bb_hdr;		// decoded header
	uint8_t bt_data[0];	// maximum data size for an ACL packet
} __attribute__((packed)) btctl_rx_pkt_t;

typedef struct {
	bbhdr_t bb_hdr;		// header (only lt_addr/type/flags are relevant)
	uint8_t bt_data[0];	// maximum data size for an ACL packet
} __attribute__((packed)) btctl_tx_pkt_t;

#endif /* __UBERTOOTH_INTERFACE_H */
