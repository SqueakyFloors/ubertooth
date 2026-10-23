/*
 * Hardware-independent identity reports and exit-status handling for
 * ubertooth-util.
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

#define UTIL_SERIAL_BUFFER_LEN 17

struct util_report_ops {
	/* Board ID (0 or greater), or a negative value on a transfer error. */
	int (*get_board_id)(void *context);
	void (*get_firmware_revision)(void *context, char *revision, size_t size);
	/* Returns 0 on success, a negative value on error. */
	int (*get_api_version)(void *context, uint16_t *version);
	void (*get_compile_info)(void *context, char *info, size_t size);
	/* Part ID (0 or greater), or a negative value on a transfer error. */
	int (*get_part_id)(void *context);
	/* Returns 0 on success; fills serial[1..16] (serial[0] is a status byte). */
	int (*get_serial)(void *context, uint8_t serial[UTIL_SERIAL_BUFFER_LEN]);
};

struct util_report_request {
	int board_id;
	int firmware;
	int compile_info;
	int part_id;
	int serial;
};

/* Returns the name of a known board ID, or NULL for an unknown ID. */
const char *util_board_name(int board_id);

/* Number of consecutive known board IDs, starting at 0. */
size_t util_board_count(void);

/* Returns current if it is already a failure, otherwise next (sticky failure). */
int util_status_merge(int current, int next);

/* Converts a value-or-negative-error from the device into 0 or 1. */
int util_value_status(int value);

int util_report_board_id(const struct util_report_ops *ops, void *context,
                         FILE *out, FILE *err);
int util_report_firmware(const struct util_report_ops *ops, void *context,
                         FILE *out);
int util_report_compile_info(const struct util_report_ops *ops, void *context,
                             FILE *out);
int util_report_part_id(const struct util_report_ops *ops, void *context,
                        FILE *out);
int util_report_serial(const struct util_report_ops *ops, void *context,
                       FILE *out);

/*
 * Runs every requested report in a fixed order (board ID, firmware, compile
 * information, part ID, serial). Every requested report runs even after an
 * earlier failure, and the result is 1 if any report failed.
 */
int util_run_reports(const struct util_report_request *request,
                     const struct util_report_ops *ops, void *context,
                     FILE *out, FILE *err);

#endif /* UBERTOOTH_UTIL_REPORT_H */
