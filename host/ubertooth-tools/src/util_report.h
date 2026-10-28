/*
 * Hardware-independent identity reports and exit-status handling for
 * ubertooth-util.
 *
 * The firmware version line lives in libubertooth, where -V reads it for
 * every tool (ubertooth_firmware_version_text()); ubertooth-util -v prints
 * that same reading.
 *
 * Device queries are injected by the caller so the printed output and the
 * process status can be tested without an attached Ubertooth. A report returns
 * 0 on success and 1 on failure; the numeric value it reads from the device
 * (for example, the board ID) is never returned as a status.
 */
#ifndef UBERTOOTH_UTIL_REPORT_H
#define UBERTOOTH_UTIL_REPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ubertooth_interface.h"

#define UTIL_SERIAL_BUFFER_LEN 17

/* Query names used in "error: <query> read failed" and the device listing. */
#define UTIL_QUERY_BOARD_ID "board ID"
#define UTIL_QUERY_PART_ID "part ID"
#define UTIL_QUERY_SERIAL "serial"

struct util_report_ops {
	/* Board ID (0 or greater), or a negative value on a transfer error. */
	int (*get_board_id)(void *context);
	/* Returns 0 and fills *part_id on success, a negative value on error. */
	int (*get_part_id)(void *context, uint32_t *part_id);
	/* Returns 0 on success; fills serial[1..16] (serial[0] is a status byte). */
	int (*get_serial)(void *context, uint8_t serial[UTIL_SERIAL_BUFFER_LEN]);
};

/* Returns the name of a known board ID, or NULL for an unknown ID. */
const char *util_board_name(int board_id);

/* Number of consecutive known board IDs, starting at 0. */
size_t util_board_count(void);

/* Returns current if it is already a failure, otherwise next (sticky failure). */
int util_status_merge(int current, int next);

/* Converts a value-or-negative-error from the device into 0 or 1. */
int util_value_status(int value);

/* Folds a device command result (negative on error) into a sticky status. */
int util_status_after(int status, int device_result);

/* Prints "error: <what> read failed" on err and returns the failing status 1.
 * libubertooth has usually printed its transfer error first; this names the
 * query whose read failed, so a failed read never shows a value line. */
int util_report_read_failed(FILE *err, const char *what);

/* libubertooth's UBERTOOTH_ERROR_SHORT_REPLY, repeated here so this module
 * stays free of libusb headers; ubertooth-util.c checks they match. */
#define UTIL_ERROR_SHORT_REPLY (-1000)

/* Like util_report_read_failed() for a read that failed with code, but prints
 * nothing for a short reply (UTIL_ERROR_SHORT_REPLY), whose whole line
 * libubertooth already printed. Returns 1. */
int util_report_read_error(FILE *err, const char *what, int code);

/* Nonzero if message is exactly the line util_report_read_failed() prints for
 * what (so a caller that captured it can show its own read-failed text). */
int util_is_read_failed_message(const char *message, const char *what);

/* message without its leading "error: ", for a caller that captured a
 * report's error line and is putting it on standard output, where the prefix
 * does not belong. Returns message itself when it has no such prefix. The
 * result points into message. */
const char *util_strip_error_prefix(const char *message);

int util_report_board_id(const struct util_report_ops *ops, void *context,
                         FILE *out, FILE *err);
int util_report_part_id(const struct util_report_ops *ops, void *context,
                        FILE *out, FILE *err);
int util_report_serial(const struct util_report_ops *ops, void *context,
                       FILE *out, FILE *err);

/* The watchdog report (ubertooth-util -w), from UBERTOOTH_GET_WATCHDOG's reply
 * as the bytes it arrived in. Returns 0, or 1 for a reply this cannot read.
 * The helpers below are what it prints each field as, exposed for the tests. */
int util_report_watchdog(FILE *out, const uint8_t *info, size_t len);
/* The radio wait give-up counts, printed under the watchdog report: one line,
 * from UBERTOOTH_GET_RADIO_TIMEOUTS. Returns 0, or 1 for a NULL reply. */
int util_report_radio_timeouts(FILE *out, const radio_timeouts *timeouts);
/* The mode's name, or NULL when the board reports no mode (btbr does not): the
 * report then writes a clause of its own, since no name fits there. */
const char *util_watchdog_mode_name(unsigned mode);
const char *util_watchdog_site_text(unsigned site);
void util_watchdog_cause_text(unsigned cause, char *text, size_t size);

#endif /* UBERTOOTH_UTIL_REPORT_H */
