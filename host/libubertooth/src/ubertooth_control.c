/*
 * Copyright 2010-2013 Michael Ossmann, Dominic Spill
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

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <btbb.h>
#include "ubertooth.h"
#include "ubertooth_control.h"

#define CTRL_IN     (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN)
#define CTRL_OUT    (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT)

void show_libusb_error(int error_code)
{
	char *error_hint = "";
	const char *error_name;

	/* The two failures that say something about the board rather than about
	 * the USB transport get plain words of their own, as one self-contained
	 * line each, with the code kept in parentheses for bug reports. That is
	 * all they say: this is the sink for every request the library makes, so
	 * what a silent board means differs by caller -- a board still running a
	 * mode, for a first read; a bootloader that lost its place, mid-flash --
	 * and a cause guessed here would be wrong as often as right. The callers
	 * that know add it, as the serial scan does. Nothing here knows which
	 * board the request went to either, so these do not name one. */
	if (error_code == LIBUSB_ERROR_TIMEOUT) {
		fprintf(stderr, "error: the Ubertooth did not answer in time "
		        "(libusb %d)\n", error_code);
		return;
	}
	if (error_code == LIBUSB_ERROR_BUSY) {
		fprintf(stderr, "error: the Ubertooth is in use by another program "
		        "(libusb %d)\n", error_code);
		return;
	}

	/* Available only in libusb > 1.0.3 */
	// error_name = libusb_error_name(error_code);

#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000103)
	error_name = libusb_strerror(error_code);
#else
	switch (error_code) {
		/* No LIBUSB_ERROR_TIMEOUT case: it is reported above, before this
		 * naming is reached. */
		case LIBUSB_ERROR_NO_DEVICE:
			error_name="No Device";
			error_hint="Check Ubertooth is connected to host";
			break;
		case LIBUSB_ERROR_ACCESS:
			error_name="Insufficient Permissions";
			break;
		case LIBUSB_ERROR_OVERFLOW:
			error_name="Overflow";
			error_hint="Try resetting the Ubertooth";
			break;
		default:
			error_name="Command Error";
			break;
	}
#endif

	/* libusb's own description is the whole of this line, because nothing
	 * here knows what the request was: the two failures above are the ones
	 * that can be put in plain words without it. It used to read
	 * "libUSB Error: Input/Output Error:  (-1)" -- no prefix, and a colon
	 * before the hint that is empty on every current libusb. */
	fprintf(stderr, "error: %s (libusb %d)\n", error_name, error_code);
	if (error_hint[0] != '\0')
		fprintf(stderr, "note: %s\n", error_hint);
}

/* What each control request asks the board to do, as a phrase that reads
 * after "could not": "could not set the channel". Every report about a
 * refused request names it from here, so the same request is called the same
 * thing wherever it failed. NULL for a request this table does not know,
 * which is then named by its number. */
static const char* request_name(uint8_t request)
{
	switch (request) {
		case UBERTOOTH_PING:               return "ping the board";
		case UBERTOOTH_RX_SYMBOLS:         return "start the symbol capture";
		case UBERTOOTH_TX_SYMBOLS:         return "start the symbol transmit";
		case UBERTOOTH_GET_USRLED:         return "read the user LED";
		case UBERTOOTH_SET_USRLED:         return "set the user LED";
		case UBERTOOTH_GET_RXLED:          return "read the receive LED";
		case UBERTOOTH_SET_RXLED:          return "set the receive LED";
		case UBERTOOTH_GET_TXLED:          return "read the transmit LED";
		case UBERTOOTH_SET_TXLED:          return "set the transmit LED";
		case UBERTOOTH_GET_1V8:            return "read the 1.8 V regulator";
		case UBERTOOTH_SET_1V8:            return "set the 1.8 V regulator";
		case UBERTOOTH_GET_CHANNEL:        return "read the channel";
		case UBERTOOTH_SET_CHANNEL:        return "set the channel";
		case UBERTOOTH_RESET:              return "reset the board";
		case UBERTOOTH_GET_SERIAL:         return "read the serial number";
		case UBERTOOTH_GET_PARTNUM:        return "read the part number";
		case UBERTOOTH_GET_PAEN:           return "read the amplifier setting";
		case UBERTOOTH_SET_PAEN:           return "set the amplifier setting";
		case UBERTOOTH_GET_HGM:            return "read the high gain setting";
		case UBERTOOTH_SET_HGM:            return "set the high gain setting";
		case UBERTOOTH_TX_TEST:            return "start the transmit test";
		case UBERTOOTH_STOP:               return "stop the running mode";
		case UBERTOOTH_GET_MOD:            return "read the modulation";
		case UBERTOOTH_SET_MOD:            return "set the modulation";
		case UBERTOOTH_SET_ISP:            return "enter the programmer";
		case UBERTOOTH_FLASH:              return "start the firmware update";
		case BOOTLOADER_FLASH:             return "start the bootloader update";
		case UBERTOOTH_SPECAN:             return "start the spectrum analyser";
		case UBERTOOTH_GET_PALEVEL:        return "read the transmit power level";
		case UBERTOOTH_SET_PALEVEL:        return "set the transmit power level";
		case UBERTOOTH_REPEATER:           return "start the repeater";
		case UBERTOOTH_RANGE_TEST:         return "start the range test";
		case UBERTOOTH_RANGE_CHECK:        return "read the range test result";
		case UBERTOOTH_GET_REV_NUM:        return "read the firmware revision";
		case UBERTOOTH_LED_SPECAN:         return "start the LED spectrum analyser";
		case UBERTOOTH_GET_BOARD_ID:       return "read the board ID";
		case UBERTOOTH_SET_SQUELCH:        return "set the squelch level";
		case UBERTOOTH_GET_SQUELCH:        return "read the squelch level";
		case UBERTOOTH_SET_BDADDR:         return "set the Bluetooth address";
		case UBERTOOTH_START_HOPPING:      return "start hopping";
		case UBERTOOTH_SET_CLOCK:          return "set the board's clock";
		case UBERTOOTH_GET_CLOCK:          return "read the board's clock";
		case UBERTOOTH_BTLE_SNIFFING:      return "start the BLE capture";
		case UBERTOOTH_GET_ACCESS_ADDRESS: return "read the access address";
		case UBERTOOTH_SET_ACCESS_ADDRESS: return "set the access address";
		case UBERTOOTH_GET_CRC_VERIFY:     return "read the CRC check setting";
		case UBERTOOTH_SET_CRC_VERIFY:     return "set the CRC check setting";
		case UBERTOOTH_POLL:               return "poll for a packet";
		case UBERTOOTH_BTLE_PROMISC:       return "start the promiscuous BLE capture";
		case UBERTOOTH_SET_AFHMAP:         return "set the AFH map";
		case UBERTOOTH_CLEAR_AFHMAP:       return "clear the AFH map";
		case UBERTOOTH_READ_REGISTER:      return "read a CC2400 register";
		case UBERTOOTH_WRITE_REGISTER:     return "write a CC2400 register";
		case UBERTOOTH_READ_ALL_REGISTERS: return "read the CC2400 registers";
		case UBERTOOTH_WRITE_REGISTERS:    return "write the CC2400 registers";
		case UBERTOOTH_BTLE_PERIPHERAL:    return "start advertising";
		case UBERTOOTH_GET_COMPILE_INFO:   return "read the compile information";
		case UBERTOOTH_BTLE_SET_TARGET:    return "set the BLE target address";
		case UBERTOOTH_BTLE_PHY:           return "set the BLE physical layer";
		case UBERTOOTH_JAM_MODE:           return "set the jam mode";
		case UBERTOOTH_EGO:                return "start the E-Go mode";
		case UBERTOOTH_AFH:                return "start the AFH detection";
		case UBERTOOTH_HOP:                return "hop to the next channel";
		case UBERTOOTH_TRIM_CLOCK:         return "trim the board's clock";
		case UBERTOOTH_RX_GENERIC:         return "start the generic capture";
		case UBERTOOTH_TX_GENERIC_PACKET:  return "transmit a packet";
		case UBERTOOTH_FIX_CLOCK_DRIFT:    return "correct the clock drift";
		case UBERTOOTH_CANCEL_FOLLOW:      return "cancel the follow";
		case UBERTOOTH_LE_SET_ADV_DATA:    return "set the advertising data";
		case UBERTOOTH_RFCAT_SUBCMD:       return "send the rfcat command";
		case UBERTOOTH_XMAS:               return "start the Christmas lights";
		case UBERTOOTH_IDENTIFY:           return "blink the board's LEDs";
		case UBERTOOTH_GET_FW_INFO:        return "read the firmware information";
		case UBERTOOTH_GET_WATCHDOG:       return "read the watchdog record";
		case UBERTOOTH_GET_RADIO_TIMEOUTS: return "read the radio wait timeouts";
		case UBERTOOTH_GET_BOOTLOADER_INFO: return "read the bootloader information";
		default:                           return NULL;
	}
}

/* request_name() with the fallback filled in, written into the caller's
 * buffer so nothing is allocated on an error path. */
static const char* request_text(uint8_t request, char* buf, size_t size)
{
	const char* name = request_name(request);

	if (name != NULL)
		return name;
	snprintf(buf, size, "send control request %u", (unsigned int)request);
	return buf;
}

/* One report for a synchronous request that failed. A stall is the board
 * saying it has no such request, which the shared wording explains with the
 * two firmware API versions; anything else is a transport failure and is left
 * to show_libusb_error(). */
static void report_request_failed(int code, uint8_t request)
{
	char buf[40];

	if (code == LIBUSB_ERROR_PIPE)
		ubertooth_report_unsupported_request(
			request_text(request, buf, sizeof(buf)), code);
	else
		show_libusb_error(code);
}

/* A libusb_transfer_status in plain words. The host tools' shared
 * report_refused_request() keeps the same wording for the libusb error codes
 * of a synchronous request, so the two reports read alike; change both
 * together. These statuses are not libusb_error codes -- they are a separate,
 * positive enumeration -- so passing one to
 * show_libusb_error() printed a stall (status 4) as
 * "libUSB Error: Other error:  (4)", which is what libusb_strerror() returns
 * for any positive code. NULL for a status this libusb version has added,
 * which is then printed as a number. */
static const char* transfer_status_text(int status)
{
	switch (status) {
		/* No LIBUSB_TRANSFER_STALL case: that is the board saying it has
		 * no such request, and the callback below reports it in the
		 * shared wording, with the firmware API versions. */
		case LIBUSB_TRANSFER_TIMED_OUT:
			return "the board did not answer in time";
		case LIBUSB_TRANSFER_NO_DEVICE:
			return "the board is no longer attached";
		case LIBUSB_TRANSFER_CANCELLED:
			/* Reported, where the bulk callback stays quiet about a
			 * cancellation: nothing cancels a control transfer, so one that
			 * comes back cancelled is a surprise, not a teardown. */
			return "the transfer was cancelled";
		case LIBUSB_TRANSFER_OVERFLOW:
			return "the board sent more data than the request allows";
		case LIBUSB_TRANSFER_ERROR:
			return "the transfer failed";
		default:
			return NULL;
	}
}

/* Completion of a transfer submitted by ubertooth_cmd_async(). That sets
 * LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER, so libusb
 * frees the buffer and the transfer once this returns: freeing the transfer
 * here would be a double free.
 *
 * A failure is reported here and nowhere else: the submit returns before the
 * board has seen the request, so this is the only place that knows the
 * command was refused. */
static void callback(struct libusb_transfer* transfer)
{
	char name_text[40];
	char why_text[32];
	const char* name;
	const char* why;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
		return;

	/* The setup packet this transfer carries: buffer[1] is bRequest. */
	if (transfer->buffer != NULL
	    && transfer->length >= (int)LIBUSB_CONTROL_SETUP_SIZE)
		name = request_text(transfer->buffer[1], name_text,
		                    sizeof(name_text));
	else
		name = "send the control request";

	/* A stall gets the shared report, the same one a synchronous request
	 * gets. The status is not a libusb error code, so there is none to
	 * quote. */
	if (transfer->status == LIBUSB_TRANSFER_STALL) {
		ubertooth_report_unsupported_request(name, 0);
		return;
	}

	why = transfer_status_text(transfer->status);
	if (why == NULL) {
		snprintf(why_text, sizeof(why_text), "transfer status %d",
		         transfer->status);
		why = why_text;
	}

	fprintf(stderr, "error: could not %s: %s\n", name, why);
}

void cmd_trim_clock(struct libusb_device_handle* devh, uint16_t offset)
{
	uint8_t data[2] = {
		(offset >> 8) & 0xff,
		(offset >> 0) & 0xff
	};

	ubertooth_cmd_async(devh, CTRL_OUT, UBERTOOTH_TRIM_CLOCK, data, 2);
}

void cmd_fix_clock_drift(struct libusb_device_handle* devh, int16_t ppm)
{
	uint8_t data[2] = {
		(ppm >> 8) & 0xff,
		(ppm >> 0) & 0xff
	};

	ubertooth_cmd_async(devh, CTRL_OUT, UBERTOOTH_FIX_CLOCK_DRIFT, data, 2);
}

int cmd_ping(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_PING, 0, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_PING);
		return r;
	}
	return 0;
}

int cmd_rx_syms_quiet(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_RX_SYMBOLS, 0, 0,
			NULL, 0, 1000);
	if (r < 0)
		return r;
	return 0;
}

int cmd_rx_syms(struct libusb_device_handle* devh)
{
	int r = cmd_rx_syms_quiet(devh);

	if (r < 0)
		report_request_failed(r, UBERTOOTH_RX_SYMBOLS);
	return r;
}

int cmd_tx_syms(struct libusb_device_handle* devh)
{
	return ubertooth_cmd_sync(devh, CTRL_OUT, UBERTOOTH_TX_SYMBOLS, 0, 0);
}

int cmd_specan(struct libusb_device_handle* devh, u16 low_freq, u16 high_freq)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SPECAN,
			low_freq, high_freq, NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_SPECAN);
		return r;
	}
	return 0;
}

int cmd_led_specan(struct libusb_device_handle* devh, u16 rssi_threshold)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_LED_SPECAN,
			rssi_threshold, 0, NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_LED_SPECAN);
		return r;
	}
	return 0;
}

int cmd_set_usrled(struct libusb_device_handle* devh, u16 state)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_USRLED, state, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_SET_USRLED);
		return r;
	}
	return 0;
}

/* Report a reply shorter (or, as a fallback, longer) than the read needs, as
 * the one complete error line, and return UBERTOOTH_ERROR_SHORT_REPLY so the
 * caller does not print a second line. */
static int short_reply(const char *what, int got, int want, const char *unit)
{
	fprintf(stderr, "error: %s read failed: board returned %d of %d %s\n",
	        what, got, want, unit);
	return UBERTOOTH_ERROR_SHORT_REPLY;
}

/* A fixed-size control read must return exactly the bytes asked for. A short
 * or empty reply (seen with stalled control transfers in follow mode) would
 * otherwise hand back whatever the result buffer held, as if it were a real
 * value. Returns r unchanged unless the length is wrong. */
static int check_reply_length(int r, int want, const char *what)
{
	if (r >= 0 && r != want)
		return short_reply(what, r, want, "bytes");
	return r;
}

int cmd_get_usrled(struct libusb_device_handle* devh)
{
	u8 state;
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_USRLED, 0, 0,
			&state, 1, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_USRLED);
		return r;
	}
	r = check_reply_length(r, 1, "USR LED");
	if (r < 0)
		return r;
	return state;
}

int cmd_set_rxled(struct libusb_device_handle* devh, u16 state)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_RXLED, state, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_SET_RXLED);
		return r;
	}
	return 0;
}

int cmd_get_rxled(struct libusb_device_handle* devh)
{
	u8 state;
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_RXLED, 0, 0,
			&state, 1, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_RXLED);
		return r;
	}
	r = check_reply_length(r, 1, "RX LED");
	if (r < 0)
		return r;
	return state;
}

int cmd_set_txled(struct libusb_device_handle* devh, u16 state)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_TXLED, state, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_SET_TXLED);
		return r;
	}
	return 0;
}

int cmd_get_txled(struct libusb_device_handle* devh)
{
	u8 state;
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_TXLED, 0, 0,
			&state, 1, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_TXLED);
		return r;
	}
	r = check_reply_length(r, 1, "TX LED");
	if (r < 0)
		return r;
	return state;
}

int cmd_get_modulation(struct libusb_device_handle* devh)
{
	u8 modulation;
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_MOD, 0, 0,
			&modulation, 1, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_MOD);
		return r;
	}
	r = check_reply_length(r, 1, "modulation");
	if (r < 0)
		return r;

	return modulation;
}

int cmd_get_channel(struct libusb_device_handle* devh)
{
	u8 result[2];
	int r;
	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_CHANNEL, 0, 0,
			result, 2, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_CHANNEL);
		return r;
	}
	r = check_reply_length(r, 2, "channel");
	if (r < 0)
		return r;

	return result[0] | (result[1] << 8);
}


int cmd_set_channel_quiet(struct libusb_device_handle* devh, u16 channel)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_CHANNEL, channel, 0,
			NULL, 0, 1000);
	if (r < 0)
		return r;
	return 0;
}

int cmd_set_channel(struct libusb_device_handle* devh, u16 channel)
{
	int r = cmd_set_channel_quiet(devh, channel);

	if (r < 0)
		report_request_failed(r, UBERTOOTH_SET_CHANNEL);
	return r;
}

int cmd_get_part_id(struct libusb_device_handle* devh, u32 *part_id)
{
	u8 result[5];
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_PARTNUM, 0, 0,
			result, 5, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_PARTNUM);
		return r;
	}
	r = check_reply_length(r, 5, "part ID");
	if (r < 0)
		return r;
	/* result[0] is the In-Application Programming (IAP) status code. */
	if (result[0] != 0) {
		fprintf(stderr, "error: part ID read failed: device status %d\n",
		        result[0]);
		return LIBUSB_ERROR_OTHER;
	}
	*part_id = (u32)result[1] | ((u32)result[2] << 8) |
	           ((u32)result[3] << 16) | ((u32)result[4] << 24);
	return 0;
}

int cmd_get_partnum(struct libusb_device_handle* devh)
{
	u32 part_id;
	int r;

	r = cmd_get_part_id(devh, &part_id);
	if (r < 0)
		return r;
	/* A part ID with bit 31 set cannot be returned as a non-negative int. */
	if (part_id > (u32)INT_MAX) {
		fprintf(stderr, "error: part ID 0x%08X does not fit; use "
		        "cmd_get_part_id()\n", (unsigned int)part_id);
		return LIBUSB_ERROR_OVERFLOW;
	}
	return (int)part_id;
}

int cmd_get_serial(struct libusb_device_handle* devh, u8 *serial)
{
	int r;
	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_SERIAL, 0, 0,
			serial, UBERTOOTH_SERIAL_LEN, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_SERIAL);
		return r;
	}
	r = check_reply_length(r, UBERTOOTH_SERIAL_LEN, "serial");
	if (r < 0)
		return r;
	if (serial[0] != 0) {
		fprintf(stderr, "error: serial read failed: device status %d\n",
		        serial[0]);
		return 1;
	}
	return 0;
}

int cmd_set_modulation_quiet(struct libusb_device_handle* devh, u16 mod)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_MOD, mod, 0,
			NULL, 0, 1000);
	if (r < 0)
		return r;
	return 0;
}

int cmd_set_modulation(struct libusb_device_handle* devh, u16 mod)
{
	int r = cmd_set_modulation_quiet(devh, mod);

	if (r < 0)
		report_request_failed(r, UBERTOOTH_SET_MOD);
	return r;
}

int cmd_set_isp(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_ISP, 0, 0,
			NULL, 0, 1000);
	/* LIBUSB_ERROR_PIPE or LIBUSB_ERROR_OTHER is expected */
	if (r && (r != LIBUSB_ERROR_PIPE) && (r != LIBUSB_ERROR_OTHER) &&
		(r != LIBUSB_ERROR_NO_DEVICE)) {
		show_libusb_error(r);
		return r;
	}
	return 0;
}

int cmd_reset(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_RESET, 0, 0,
			NULL, 0, 1000);
	/* LIBUSB_ERROR_PIPE or LIBUSB_ERROR_OTHER is expected */
	if (r && (r != LIBUSB_ERROR_PIPE) && (r != LIBUSB_ERROR_OTHER) &&
		(r != LIBUSB_ERROR_NO_DEVICE)) {
		show_libusb_error(r);
		return r;
	}
	return 0;
}

int cmd_stop(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_STOP, 0, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_STOP);
		return r;
	}
	return 0;
}

int cmd_set_paen(struct libusb_device_handle* devh, u16 state)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_PAEN, state, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_SET_PAEN);
		return r;
	}
	return 0;
}

int cmd_set_hgm(struct libusb_device_handle* devh, u16 state)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_HGM, state, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_SET_HGM);
		return r;
	}
	return 0;
}

int cmd_tx_test(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_TX_TEST, 0, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_TX_TEST);
		return r;
	}
	return 0;
}

int cmd_flash(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_FLASH, 0, 0,
			NULL, 0, 1000);
	if (r != LIBUSB_SUCCESS) {
		report_request_failed(r, UBERTOOTH_FLASH);
		return r;
	}
	return 0;
}

int cmd_get_palevel(struct libusb_device_handle* devh)
{
	u8 level;
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_PALEVEL, 0, 0,
			&level, 1, 3000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_PALEVEL);
		return r;
	}
	r = check_reply_length(r, 1, "PA level");
	if (r < 0)
		return r;
	return level;
}

int cmd_set_palevel(struct libusb_device_handle* devh, u16 level)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_PALEVEL, level, 0,
			NULL, 0, 3000);
	if (r != LIBUSB_SUCCESS) {
		report_request_failed(r, UBERTOOTH_SET_PALEVEL);
		return r;
	}
	return 0;
}

/* The range test result read, in both forms: quiet returns the error and
 * prints nothing. See cmd_get_rangeresult_quiet(). */
static int rangeresult(struct libusb_device_handle* devh, rangetest_result *rr,
		int quiet)
{
	u8 result[UBERTOOTH_RANGE_RESULT_LEN];
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_RANGE_CHECK, 0, 0,
			result, sizeof(result), 3000);
	if (r < LIBUSB_SUCCESS) {
		if (quiet)
			return r;
		report_request_failed(r, UBERTOOTH_RANGE_CHECK);
		return r;
	}
	if (r != (int)sizeof(result))
		return quiet ? UBERTOOTH_ERROR_SHORT_REPLY
		             : check_reply_length(r, sizeof(result),
		                                  "range test result");

	rr->valid       = result[0];
	rr->request_pa  = result[1];
	rr->request_num = result[2];
	rr->reply_pa    = result[3];
	rr->reply_num   = result[4];
	rr->discarded   = result[5];

	return 0;
}

int cmd_get_rangeresult(struct libusb_device_handle* devh,
		rangetest_result *rr)
{
	return rangeresult(devh, rr, 0);
}

int cmd_get_rangeresult_quiet(struct libusb_device_handle* devh,
		rangetest_result *rr)
{
	return rangeresult(devh, rr, 1);
}

/* The watchdog read, in both forms; see cmd_get_watchdog_quiet(). */
static int watchdog(struct libusb_device_handle* devh, watchdog_info *info,
		int quiet)
{
	u8 result[UBERTOOTH_WATCHDOG_INFO_LEN];
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_WATCHDOG, 0, 0,
			result, sizeof(result), 1000);
	if (r < LIBUSB_SUCCESS) {
		if (quiet)
			return r;
		report_request_failed(r, UBERTOOTH_GET_WATCHDOG);
		return r;
	}
	if (r != (int)sizeof(result))
		return quiet ? UBERTOOTH_ERROR_SHORT_REPLY
		             : check_reply_length(r, UBERTOOTH_WATCHDOG_INFO_LEN,
		                                  "watchdog state");

	/* Byte by byte, in the order ubertooth_interface.h gives: the reply is a
	 * byte string, not this struct's memory. */
	info->reset_cause = result[0];
	info->armed       = result[1];
	info->resets      = result[2];
	info->boots       = result[3];
	info->reset_mode  = result[4];
	info->reset_site  = result[5];
	info->mode        = result[6];
	info->site        = result[7];

	info->transfer_errors = (uint16_t)result[8] | ((uint16_t)result[9] << 8);
	info->fault_status = (uint32_t)result[10] | ((uint32_t)result[11] << 8) |
	                     ((uint32_t)result[12] << 16) | ((uint32_t)result[13] << 24);
	info->fault_addr   = (uint32_t)result[14] | ((uint32_t)result[15] << 8) |
	                     ((uint32_t)result[16] << 16) | ((uint32_t)result[17] << 24);
	info->stack_used = (uint16_t)result[18] | ((uint16_t)result[19] << 8);
	info->stack_size = (uint16_t)result[20] | ((uint16_t)result[21] << 8);

	return 0;
}

int cmd_get_watchdog(struct libusb_device_handle* devh, watchdog_info *info)
{
	return watchdog(devh, info, 0);
}

int cmd_get_watchdog_quiet(struct libusb_device_handle* devh,
		watchdog_info *info)
{
	return watchdog(devh, info, 1);
}

int cmd_get_radio_timeouts(struct libusb_device_handle* devh,
		radio_timeouts *timeouts)
{
	u8 result[UBERTOOTH_RADIO_TIMEOUTS_LEN];
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_RADIO_TIMEOUTS,
			0, 0, result, sizeof(result), 1000);
	if (r < LIBUSB_SUCCESS) {
		report_request_failed(r, UBERTOOTH_GET_RADIO_TIMEOUTS);
		return r;
	}
	if (r != (int)sizeof(result))
		return check_reply_length(r, (int)sizeof(result),
		                         "radio wait timeouts");

	/* Byte by byte, low byte first, in the order ubertooth_interface.h
	 * gives: the reply is a byte string, not this struct's memory. */
	timeouts->fs_lock   = (uint16_t)(result[0] | (result[1] << 8));
	timeouts->fs_unlock = (uint16_t)(result[2] | (result[3] << 8));
	timeouts->xosc      = (uint16_t)(result[4] | (result[5] << 8));
	timeouts->fsm       = (uint16_t)(result[6] | (result[7] << 8));
	timeouts->rx_bytes  = (uint16_t)(result[8] | (result[9] << 8));

	return 0;
}

int cmd_get_bootloader_info(struct libusb_device_handle* devh,
		struct ubertooth_bootloader_info *info)
{
	u8 result[UBERTOOTH_BL_REPORT_LENGTH];
	int r;

	if (info == NULL)
		return LIBUSB_ERROR_INVALID_PARAM;
	memset(info, 0, sizeof(*info));
	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_BOOTLOADER_INFO,
			0, 0, result, sizeof(result), 1000);
	if (r < LIBUSB_SUCCESS) {
		report_request_failed(r, UBERTOOTH_GET_BOOTLOADER_INFO);
		return r;
	}
	if (r != (int)sizeof(result))
		return check_reply_length(r, (int)sizeof(result),
		                         "bootloader information");
	/* Whether the region carries a version block is the board's answer, not
	 * a failure of the read: the parse only reports a reply too short to
	 * hold the hash, which the length check above has already caught, and
	 * it leaves the rest in info->version_result for the caller to
	 * report. */
	ubertooth_bootloader_info_parse(result, (size_t)r, info);
	return 0;
}

int cmd_range_test(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_RANGE_TEST, 0, 0,
			NULL, 0, 1000);
	if (r != LIBUSB_SUCCESS) {
		report_request_failed(r, UBERTOOTH_RANGE_TEST);
		return r;
	}
	return 0;
}

int cmd_repeater(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_REPEATER, 0, 0,
			NULL, 0, 1000);
	if (r != LIBUSB_SUCCESS) {
		report_request_failed(r, UBERTOOTH_REPEATER);
		return r;
	}
	return 0;
}

/* Copies a length-prefixed string from a device reply into text (size bytes,
 * always terminated). data_len is the number of reply bytes after the length
 * byte; a declared length longer than that is a short reply. */
static int copy_reply_string(const char *what, const u8 *data, int data_len,
                             u8 declared, char *text, size_t size)
{
	size_t n = declared;

	if (data_len < (int)declared)
		return short_reply(what, data_len, declared, "string bytes");
	/* A buffer smaller than the string keeps its leading part, as before. */
	if (n > size - 1)
		n = size - 1;
	memcpy(text, data, n);
	text[n] = '\0';
	return 0;
}

int cmd_get_firmware_revision(struct libusb_device_handle* devh, char *revision,
                              size_t size)
{
	u8 result[2 + 1 + UBERTOOTH_REVISION_MAX];
	int r;

	if (revision == NULL || size == 0)
		return LIBUSB_ERROR_INVALID_PARAM;
	revision[0] = '\0';
	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_REV_NUM, 0, 0,
			result, sizeof(result), 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_REV_NUM);
		return r;
	}

	if (r == 2) { /* old-style SVN revision number */
		snprintf(revision, size, "%u", (unsigned int)(result[0] | (result[1] << 8)));
		return 0;
	}
	if (r < 3) /* 2 is the old SVN form above; the string form needs 3+ */
		return short_reply("firmware revision", r, 3, "bytes");
	return copy_reply_string("firmware revision", &result[3], r - 3, result[2],
	                         revision, size);
}

int cmd_get_firmware_compile_info(struct libusb_device_handle* devh,
                                  char *compile_info, size_t size)
{
	u8 result[1 + UBERTOOTH_COMPILE_INFO_MAX];
	int r;

	if (compile_info == NULL || size == 0)
		return LIBUSB_ERROR_INVALID_PARAM;
	compile_info[0] = '\0';
	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_COMPILE_INFO, 0, 0,
			result, sizeof(result), 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_COMPILE_INFO);
		return r;
	}

	if (r < 1)
		return short_reply("compile info", r, 1, "bytes");
	return copy_reply_string("compile info", &result[1], r - 1, result[0],
	                         compile_info, size);
}

/* The firmware info block read, in both forms; see
 * cmd_get_firmware_info_quiet(). */
static int firmware_info(struct libusb_device_handle* devh,
                         struct ubertooth_fw_info *info, int quiet)
{
	u8 result[UBERTOOTH_FW_INFO_LENGTH];
	int r, parsed;

	if (info == NULL)
		return LIBUSB_ERROR_INVALID_PARAM;
	memset(info, 0, sizeof(*info));
	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_FW_INFO, 0, 0,
			result, sizeof(result), 1000);
	/* A board stalls a request its firmware does not implement, so this is
	 * how firmware built before the block answers. That is not a fault worth
	 * a message of its own: a caller that can do without the block (see
	 * ubertooth-util -v) reads the revision instead, and one that cannot
	 * says what it needed the block for. */
	if (r == LIBUSB_ERROR_PIPE)
		return r;
	if (r < 0) {
		if (!quiet)
			show_libusb_error(r);
		return r;
	}
	if (r < UBERTOOTH_FW_INFO_LENGTH)
		return quiet ? UBERTOOTH_ERROR_SHORT_REPLY
		             : short_reply("firmware info", r,
		                           UBERTOOTH_FW_INFO_LENGTH, "bytes");
	/* A reply of the right length that is not a block means the firmware and
	 * this host disagree about the format, which no length check can catch. */
	parsed = ubertooth_fw_info_parse(result, (size_t)r, info);
	if (parsed != UBERTOOTH_FW_INFO_OK) {
		if (!quiet)
			fprintf(stderr, "error: %s\n",
			        ubertooth_fw_info_result_string(parsed));
		return LIBUSB_ERROR_IO;
	}
	return 0;
}

int cmd_get_firmware_info(struct libusb_device_handle* devh,
                          struct ubertooth_fw_info *info)
{
	return firmware_info(devh, info, 0);
}

int cmd_get_firmware_info_quiet(struct libusb_device_handle* devh,
                                struct ubertooth_fw_info *info)
{
	return firmware_info(devh, info, 1);
}

/* The void forms keep their old contract: on failure the buffer holds
 * "error: <code>" instead of the text, with a short reply reported as
 * LIBUSB_ERROR_IO (-1) as before. */
static int old_error_code(int r)
{
	return r == UBERTOOTH_ERROR_SHORT_REPLY ? LIBUSB_ERROR_IO : r;
}

void cmd_get_rev_num(struct libusb_device_handle* devh, char *version, u8 len)
{
	int r = cmd_get_firmware_revision(devh, version, len);

	if (r < 0 && version != NULL && len > 0)
		snprintf(version, len, "error: %d", old_error_code(r));
}

void cmd_get_compile_info(struct libusb_device_handle* devh, char *compile_info, u8 len)
{
	int r = cmd_get_firmware_compile_info(devh, compile_info, len);

	if (r < 0 && compile_info != NULL && len > 0)
		snprintf(compile_info, len, "error: %d", old_error_code(r));
}

int cmd_get_board_id(struct libusb_device_handle* devh)
{
	u8 board_id;
	int r;
	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_BOARD_ID, 0, 0,
			&board_id, 1, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_BOARD_ID);
		return r;
	}
	r = check_reply_length(r, 1, "board ID");
	if (r < 0)
		return r;

	return board_id;
}

int cmd_set_squelch(struct libusb_device_handle* devh, u16 level)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_SQUELCH, level, 0, NULL, 0, 3000);
	if (r != LIBUSB_SUCCESS) {
		report_request_failed(r, UBERTOOTH_SET_SQUELCH);
		return r;
	}
	return 0;
}

int cmd_get_squelch(struct libusb_device_handle* devh)
{
	u8 level;
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_SQUELCH, 0, 0,
			&level, 1, 3000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_SQUELCH);
		return r;
	}
	r = check_reply_length(r, 1, "squelch");
	if (r < 0)
		return r;
	return level;
}

int cmd_set_bdaddr_quiet(struct libusb_device_handle* devh, u64 address)
{
	int r, data_len;
	u64 syncword;
	data_len = 16;
	unsigned char data[data_len];

	syncword = btbb_gen_syncword(address & 0xffffff);
	//printf("syncword=%#llx\n", syncword);
	for(r=0; r < 8; r++)
		data[r] = (address >> (8*r)) & 0xff;
	for(r=0; r < 8; r++)
		data[r+8] = (syncword >> (8*r)) & 0xff;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_BDADDR, 0, 0,
		data, data_len, 1000);
	if (r < 0)
		return r;
	if (r < data_len) {
		/* One complete line, naming the request and the counts, and
		 * UBERTOOTH_ERROR_SHORT_REPLY so the caller adds no second
		 * line -- as the fixed-size reads above do. It used to say
		 * "only 8 of 16 bytes transferred" and return 1, which a
		 * caller testing for a negative result read as success. */
		fprintf(stderr, "error: Bluetooth address write failed: "
		        "board took %d of %d bytes\n", r, data_len);
		return UBERTOOTH_ERROR_SHORT_REPLY;
	}
	return 0;
}

int cmd_set_bdaddr(struct libusb_device_handle* devh, u64 address)
{
	int r = cmd_set_bdaddr_quiet(devh, address);

	if (r < 0 && r != UBERTOOTH_ERROR_SHORT_REPLY)
		report_request_failed(r, UBERTOOTH_SET_BDADDR);
	return r;
}

int cmd_start_hopping(struct libusb_device_handle* devh, int clkn_offset, int clk100ns_offset)
{
	int i;
	uint8_t data[6];
	for(i=0; i < 4; i++)
		data[i] = (clkn_offset >> (8*(3-i))) & 0xff;

	data[4] = (clk100ns_offset >> 8) & 0xff;
	data[5] = (clk100ns_offset >> 0) & 0xff;

	/* ubertooth_cmd_async() has already reported any failure. */
	return ubertooth_cmd_async(devh, CTRL_OUT, UBERTOOTH_START_HOPPING,
	                           data, 6);
}

int cmd_set_clock_quiet(struct libusb_device_handle* devh, u32 clkn)
{
	int r, data_len;
	u8 data[4];
	data_len = (int)sizeof(data);
	for(r=0; r < data_len; r++)
		data[r] = (clkn >> (8*r)) & 0xff;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_CLOCK, 0, 0,
		data, data_len, 1000);
	if (r < 0)
		return r;
	if (r < data_len) {
		/* A board that took only part of the clock is left on some
		 * other clock entirely, which a caller testing only for a
		 * negative result would read as success. One complete line, and
		 * UBERTOOTH_ERROR_SHORT_REPLY so the caller adds no second --
		 * as cmd_set_bdaddr_quiet() does. */
		fprintf(stderr, "error: clock write failed: board took %d of %d "
		        "bytes\n", r, data_len);
		return UBERTOOTH_ERROR_SHORT_REPLY;
	}
	return 0;
}

int cmd_set_clock(struct libusb_device_handle* devh, u32 clkn)
{
	int r = cmd_set_clock_quiet(devh, clkn);

	if (r < 0 && r != UBERTOOTH_ERROR_SHORT_REPLY)
		report_request_failed(r, UBERTOOTH_SET_CLOCK);
	return r;
}

uint32_t cmd_get_clock(struct libusb_device_handle* devh)
{
	u32 clock = 0;
	unsigned char data[4];
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_CLOCK, 0, 0,
			data, 4, 3000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_CLOCK);
		return r;
	}
	r = check_reply_length(r, 4, "clock");
	if (r < 0)
		return r;
	clock = data[0] | data[1] << 8 | data[2] << 16 | data[3] << 24;
	return clock;
}

int cmd_btle_sniffing(struct libusb_device_handle* devh, uint8_t do_follow)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_BTLE_SNIFFING, do_follow, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_BTLE_SNIFFING);
		return r;
	}
	return 0;
}

int cmd_set_afh_map(struct libusb_device_handle* devh, uint8_t* afh_map)
{
	return ubertooth_cmd_async(devh, CTRL_OUT, UBERTOOTH_SET_AFHMAP,
	                           afh_map, AFH_MAP_BYTES);
}

int cmd_clear_afh_map(struct libusb_device_handle* devh)
{
	int r;
	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_CLEAR_AFHMAP, 0, 0,
		NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_CLEAR_AFHMAP);
		return r;
	}
	return 0;
}

/* Read the access address into *access_address. Returns 0, or a negative
 * error (libusb, or UBERTOOTH_ERROR_SHORT_REPLY); *access_address is only
 * written on success. */
int cmd_read_access_address(struct libusb_device_handle* devh, u32 *access_address)
{
	unsigned char data[4];
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_ACCESS_ADDRESS, 0, 0,
			data, 4, 3000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_ACCESS_ADDRESS);
		return r;
	}
	r = check_reply_length(r, 4, "access address");
	if (r < 0)
		return r;
	*access_address = data[0] | data[1] << 8 | data[2] << 16 | data[3] << 24;
	return 0;
}

/* Kept for API compatibility: an error comes back as the negative libusb
 * code cast to u32, which cannot be told apart from an address. Use
 * cmd_read_access_address() instead. */
u32 cmd_get_access_address(struct libusb_device_handle* devh)
{
	u32 access_address = 0;
	int r = cmd_read_access_address(devh, &access_address);
	return r < 0 ? (u32)r : access_address;
}

int cmd_set_access_address(struct libusb_device_handle* devh, u32 access_address)
{
	int r;
	u8 data[4];
	for(r=0; r < 4; r++)
		data[r] = (access_address >> (8*r)) & 0xff;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_ACCESS_ADDRESS, 0, 0,
		data, 4, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_SET_ACCESS_ADDRESS);
		return r;
	}
	return 0;
}

int cmd_get_crc_verify(struct libusb_device_handle* devh)
{
	u8 verify;
	int r;

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_GET_CRC_VERIFY, 0, 0,
			&verify, 1, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_GET_CRC_VERIFY);
		return r;
	}
	r = check_reply_length(r, 1, "CRC verify");
	if (r < 0)
		return r;
	return verify;
}

int cmd_set_crc_verify(struct libusb_device_handle* devh, int verify)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_SET_CRC_VERIFY, verify, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_SET_CRC_VERIFY);
		return r;
	}
	return 0;
}

int cmd_poll(struct libusb_device_handle* devh, usb_pkt_rx *p)
{
	int r;
	unsigned i;

	/*
	 * A capture polls this once per packet for as long as it runs, and its
	 * caller ends the capture on the first failure. So the two failures a
	 * board answers the next poll after are retried here rather than ending
	 * a capture that has been running for a quarter of an hour: a stalled
	 * transfer (LIBUSB_ERROR_PIPE), and one that timed out
	 * (LIBUSB_ERROR_TIMEOUT) because the board was busy elsewhere when the
	 * request arrived.
	 *
	 * Three attempts, so the cost is bounded at about three seconds of the
	 * one-second timeout below. A board that has gone -- the watchdog resets
	 * one that stops answering -- fails with LIBUSB_ERROR_NO_DEVICE instead,
	 * which is not retried and ends the capture at once. So does a stop the
	 * user has asked for, which is the other half of the loop below.
	 *
	 * The retry saves the capture, not the packet. The firmware takes the
	 * packet out of its queue while it answers the request (UBERTOOTH_POLL
	 * in firmware/bluetooth_rxtx/bluetooth_rxtx.c), so a transfer that timed
	 * out after that has lost it, and the next attempt gets whatever arrived
	 * next. One packet is a far better loss than the rest of the capture.
	 */
	for (i = 0; i < 3; ++i) {
		r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_POLL, 0, 0,
				(u8 *)p, sizeof(usb_pkt_rx), 1000);
		if (r == LIBUSB_ERROR_PIPE || r == LIBUSB_ERROR_TIMEOUT) {
			/* The retry is there to keep a capture running, and the
			 * user has just asked this one to stop. A capture
			 * spends its time in this call and reads the stop only
			 * when it returns, so the attempts still to come are up
			 * to two seconds the stop would wait for: give up now
			 * and report the failure.
			 *
			 * The stop read here is the one the user asked for. The
			 * -t timer has a flag of its own, which this does not
			 * read: a timed run ending as it was told to should not
			 * be turned into a failure by a poll that timed out at
			 * that same moment. */
			if (ubertooth_stop_requested())
				break;
			continue;
		}
		if (r < 0)
			show_libusb_error(r);
		return r;
	}
	/* The attempts are over, either because all three failed the same way --
	 * so this is not a board that was busy for a moment -- or because the
	 * user stopped the run. Reported here, because the retry loop swallowed
	 * the attempts and the caller's "USB poll failed" does not say what went
	 * wrong. */
	show_libusb_error(r);
	return r;
}

int cmd_btle_promisc(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_BTLE_PROMISC, 0, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_BTLE_PROMISC);
		return r;
	}
	return 0;
}

int cmd_read_register(struct libusb_device_handle* devh, u8 reg)
{
	int r;
	u8 data[2];
	char what[16];

	r = libusb_control_transfer(devh, CTRL_IN, UBERTOOTH_READ_REGISTER, reg, 0,
			data, 2, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_READ_REGISTER);
		return r;
	}
	snprintf(what, sizeof(what), "register 0x%02x", reg);
	r = check_reply_length(r, 2, what);
	if (r < 0)
		return r;

	return (data[0] << 8) | data[1];
}

int cmd_write_register(struct libusb_device_handle* devh, u8 reg, u16 value)
{
	int r;

	/* The register address and the value both travel in the setup packet,
	 * so there is no data stage to come up short. */
	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_WRITE_REGISTER,
			reg, value, NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_WRITE_REGISTER);
		return r;
	}
	return 0;
}

int cmd_rx_generic(struct libusb_device_handle* devh)
{
	return ubertooth_cmd_sync(devh, CTRL_OUT, UBERTOOTH_RX_GENERIC, NULL, 0);
}

int cmd_tx_generic_packet(struct libusb_device_handle* devh, uint32_t syncword,
                          uint16_t channel, uint8_t pa_level,
                          const uint8_t* data, size_t data_len)
{
	/* The firmware copies the request's bytes straight over a
	 * generic_tx_packet, so the fields go in at that structure's offsets,
	 * each 16-bit one least significant byte first as the board's compiler
	 * lays it out. Written out rather than sent as a structure of our own,
	 * which would carry this host's padding and byte order into the
	 * request. */
	uint8_t request[sizeof(generic_tx_packet)];
	const size_t header_len = offsetof(generic_tx_packet, data);

	if (data_len > GENERIC_TX_MAX_PAYLOAD) {
		fprintf(stderr, "error: a generic packet carries at most %u bytes"
		        " (got %zu)\n", (unsigned)GENERIC_TX_MAX_PAYLOAD, data_len);
		return -1;
	}
	if (data == NULL && data_len > 0)
		return -1;

	memset(request, 0, sizeof(request));
	/* SYNCH is the top half of the sync word and SYNCL the bottom half,
	 * which is the order the radio sends them in. */
	request[offsetof(generic_tx_packet, synch)]     = (syncword >> 16) & 0xff;
	request[offsetof(generic_tx_packet, synch) + 1] = (syncword >> 24) & 0xff;
	request[offsetof(generic_tx_packet, syncl)]     = syncword & 0xff;
	request[offsetof(generic_tx_packet, syncl) + 1] = (syncword >> 8) & 0xff;
	request[offsetof(generic_tx_packet, channel)]     = channel & 0xff;
	request[offsetof(generic_tx_packet, channel) + 1] = (channel >> 8) & 0xff;
	request[offsetof(generic_tx_packet, length)]   = (uint8_t)data_len;
	request[offsetof(generic_tx_packet, pa_level)] = pa_level;
	if (data_len > 0)
		memcpy(request + header_len, data, data_len);

	return ubertooth_cmd_sync(devh, CTRL_OUT, UBERTOOTH_TX_GENERIC_PACKET,
	                         request, (uint16_t)(header_len + data_len));
}

int cmd_btle_peripheral(struct libusb_device_handle* devh, u8 *mac_address)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_BTLE_PERIPHERAL, 0, 0,
			mac_address, 6, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_BTLE_PERIPHERAL);
		return r;
	}

	return 0;
}

int cmd_le_set_adv_data(struct libusb_device_handle* devh, uint8_t *data, unsigned data_len) {
	int r;

	if (data_len > ADV_IND_MAX_DATA) {
		fprintf(stderr, "error: LE advertising data too long (%u > %u)\n",
		        data_len, ADV_IND_MAX_DATA);
		return -1;
	}

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_LE_SET_ADV_DATA, 0, 0,
			data, data_len, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_LE_SET_ADV_DATA);
		return r;
	}

	return 0;
}

int cmd_btle_set_target(struct libusb_device_handle* devh, uint8_t *mac_address, uint8_t mac_mask)
{
	int r;
	uint8_t cmd_buf[7];

	memcpy(cmd_buf, mac_address, 6);
	cmd_buf[6] = mac_mask;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_BTLE_SET_TARGET, 0, 0,
			cmd_buf, 7, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_BTLE_SET_TARGET);
		return r;
	}

	return 0;
}

int cmd_set_jam_mode(struct libusb_device_handle* devh, int mode) {
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_JAM_MODE, mode, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_JAM_MODE);
		return r;
	}

	return 0;
}

int cmd_ego(struct libusb_device_handle* devh, int mode)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_EGO, mode, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_EGO);
		return r;
	}
	return 0;
}

int cmd_afh(struct libusb_device_handle* devh)
{
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_AFH, 0, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_AFH);
		return r;
	}

	return 0;
}

int cmd_hop(struct libusb_device_handle* devh)
{
	return ubertooth_cmd_async(devh, CTRL_OUT, UBERTOOTH_HOP, NULL, 0);
}

int cmd_cancel_follow(struct libusb_device_handle* devh)
{
	return ubertooth_cmd_async(devh, CTRL_OUT, UBERTOOTH_CANCEL_FOLLOW,
	                           NULL, 0);
}

int cmd_rfcat_subcmd(struct libusb_device_handle* devh, int cmd, uint8_t *body, size_t body_len) {
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_RFCAT_SUBCMD, cmd, 0,
			body, body_len, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_RFCAT_SUBCMD);
		return r;
	}

	return 0;
}

int cmd_xmas(struct libusb_device_handle* devh) {
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_XMAS, 0, 0,
			NULL, 0, 1000);
	if (r < 0) {
		report_request_failed(r, UBERTOOTH_XMAS);
		return r;
	}

	return 0;
}

int cmd_identify(struct libusb_device_handle* devh) {
	int r;

	r = libusb_control_transfer(devh, CTRL_OUT, UBERTOOTH_IDENTIFY, 0, 0,
			NULL, 0, 1000);
	if (r < 0) {
		/* Firmware that predates the board-side identify mode stalls the
		 * request. That is not an error to report: the caller blinks the LEDs
		 * from the host instead, and says so. */
		if (r != LIBUSB_ERROR_PIPE)
			show_libusb_error(r);
		return r;
	}

	return 0;
}

int ubertooth_cmd_sync(struct libusb_device_handle* devh,
                       uint8_t type,
                       uint8_t command,
                       uint8_t* data,
                       uint16_t size)
{
	int r;

	r = libusb_control_transfer(devh, type, command, 0, 0,
			data, size, 1000);
	if (r < 0) {
		report_request_failed(r, command);
		return r;
	}

	return 0;
}

int ubertooth_cmd_async(struct libusb_device_handle* devh,
                        uint8_t type,
                        uint8_t command,
                        uint8_t* data,
                        uint16_t size)
{
	int r;
	uint8_t* buffer;
	struct libusb_transfer* xfer;

	if (size > UBERTOOTH_CMD_MAX_DATA) {
		fprintf(stderr, "error: control message too long: %u bytes "
		        "(maximum %u)\n", size, UBERTOOTH_CMD_MAX_DATA);
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	/* libusb reads the setup packet and the data after the submit returns,
	 * so the buffer must outlive this function: it belongs to the transfer,
	 * which frees it on completion. */
	buffer = malloc(LIBUSB_CONTROL_SETUP_SIZE + size);
	if (buffer == NULL) {
		fprintf(stderr, "error: could not allocate a control transfer buffer\n");
		return LIBUSB_ERROR_NO_MEM;
	}

	xfer = libusb_alloc_transfer(0);
	if (xfer == NULL) {
		fprintf(stderr, "error: could not allocate a control transfer\n");
		free(buffer);
		return LIBUSB_ERROR_NO_MEM;
	}

	libusb_fill_control_setup(buffer, type, command, 0, 0, size);
	if(size > 0)
		memcpy ( &buffer[LIBUSB_CONTROL_SETUP_SIZE], data, size );
	libusb_fill_control_transfer(xfer, devh, buffer, callback, NULL, 1000);
	xfer->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;

	r = libusb_submit_transfer(xfer);
	if (r < 0) {
		show_libusb_error(r);
		/* Nothing will complete this transfer, so release it here;
		 * LIBUSB_TRANSFER_FREE_BUFFER frees the buffer with it. */
		libusb_free_transfer(xfer);
		return r;
	}

	return 0;
}
