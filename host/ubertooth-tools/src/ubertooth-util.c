/*
 * Copyright 2010, 2011 Michael Ossmann
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

#include "ubertooth.h"
#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include "opt_parse.h"
#include "util_clock.h"
#include "util_report.h"
#include "util_modes.h"
#include "util_reset_wait.h"

/* Adapters from the injected report operations to the libubertooth commands. */
static int report_get_board_id(void *context)
{
	return cmd_get_board_id(((ubertooth_t*)context)->devh);
}

static int report_get_part_id(void *context, uint32_t *part_id)
{
	return cmd_get_part_id(((ubertooth_t*)context)->devh, part_id);
}

static int report_get_serial(void *context, uint8_t serial[UTIL_SERIAL_BUFFER_LEN])
{
	return cmd_get_serial(((ubertooth_t*)context)->devh, serial);
}

static const struct util_report_ops report_ops = {
	report_get_board_id,
	report_get_part_id,
	report_get_serial,
};

/*
 * The firmware version line -v prints. The reading is the library's, the one
 * -V gives in every tool (ubertooth_firmware_version_text()), so the two
 * cannot drift apart.
 *
 * Nothing is printed when nothing could be read: the library has already put
 * the reason on standard error, and a line carrying no version would be worse
 * than no line for a script parsing this. Returns 0, or 1 when part of it
 * could not be read.
 */
static int report_firmware(ubertooth_t *ut, FILE *out)
{
	char text[UBERTOOTH_VERSION_TEXT_LEN];
	int status = ubertooth_firmware_version_text(ut, text, sizeof(text));

	if (text[0] == '\0')
		return 1;
	fprintf(out, UBERTOOTH_VERSION_LABEL_FIRMWARE ": %s\n", text);
	return status;
}

/* Read-only identity queries that can be reported for every attached device. */
enum identity_query {
	QUERY_BOARD_ID,
	QUERY_FIRMWARE,
	QUERY_PART_ID,
	QUERY_SERIAL,
	QUERY_COUNT
};

static const struct {
	const char *name;  /* the query, for the line that reports a failed read */
	const char *label; /* the label the single-device report prints, without
	                    * its ": " */
} identity_query_text[QUERY_COUNT] = {
	{ UTIL_QUERY_BOARD_ID, "Board ID Number" },
	{ "firmware", UBERTOOTH_VERSION_LABEL_FIRMWARE },
	{ UTIL_QUERY_PART_ID, "Part ID" },
	{ UTIL_QUERY_SERIAL, "Serial No" },
};

/*
 * How a listed line starts: the report's own label with the board's index in
 * it, "Firmware version (0): ", so a line read on its own says both what it
 * is and which board it came from, and says it the same way whether one board
 * or several were read.
 */
static void print_listing_label(const char *label, int index)
{
	fprintf(stdout, "%s (%d): ", label, index);
}

/* Runs one identity report into buffers so its lines can be prefixed. */
static int capture_identity_report(enum identity_query query, ubertooth_t *ut,
                                   char **text, char **error)
{
	size_t size = 0, error_size = 0;
	int status = 1;
	FILE *out, *err;

	*text = NULL;
	*error = NULL;
	out = open_memstream(text, &size);
	err = open_memstream(error, &error_size);
	if (out == NULL || err == NULL) {
		if (out != NULL)
			fclose(out);
		if (err != NULL)
			fclose(err);
		return 1;
	}
	switch (query) {
	case QUERY_BOARD_ID:
		status = util_report_board_id(&report_ops, ut, out, err);
		break;
	case QUERY_FIRMWARE:
		status = report_firmware(ut, out);
		break;
	case QUERY_PART_ID:
		status = util_report_part_id(&report_ops, ut, out, err);
		break;
	case QUERY_SERIAL:
		status = util_report_serial(&report_ops, ut, out, err);
		break;
	default:
		break;
	}
	fclose(out);
	fclose(err);
	return status;
}

/*
 * Reports the requested identity queries for every device, in enumeration
 * order. Every line is "<label> (<index>): <value>", the same label the
 * single-device report prints (see print_listing_label()). A device that
 * cannot be opened, or a read that failed, gets a line of its own per query
 * and makes the result 1, but the other devices are still reported. Devices
 * are closed without sending STOP, so a query does not change whatever mode a
 * device is running.
 */
static int list_identity_queries(const int requested[QUERY_COUNT], int count)
{
	int status = 0;
	int i, q;

	for (i = 0; i < count; i++) {
		ubertooth_t *uti = ubertooth_start(i);

		if (uti == NULL) {
			/* One line per query, so a reader filtering for one of them
			 * still sees this board. libubertooth has already put the
			 * matching "error: " line on standard error. */
			const char *reason = ubertooth_open_failure_reason();

			for (q = 0; q < QUERY_COUNT; q++) {
				if (!requested[q])
					continue;
				print_listing_label(identity_query_text[q].label, i);
				fprintf(stdout, "unavailable, %s\n", reason);
			}
			status = 1;
			continue;
		}
		for (q = 0; q < QUERY_COUNT; q++) {
			char *text, *error, *line, *next;
			const char *label = identity_query_text[q].label;
			size_t label_len = strlen(label);

			if (!requested[q])
				continue;
			status = util_status_merge(status,
			        capture_identity_report((enum identity_query)q, uti,
			                                &text, &error));
			if (text == NULL || text[0] == '\0') {
				print_listing_label(label, i);
				/* Show the report's own error (for example an unknown board
				 * ID) when it gave one, otherwise say the read failed. The
				 * "error: " prefix belongs to standard error, and the label
				 * has already named the query. */
				if (error != NULL && error[0] != '\0' &&
				    !util_is_read_failed_message(error,
				            identity_query_text[q].name)) {
					error[strcspn(error, "\n")] = '\0';
					fprintf(stdout, "%s\n", util_strip_error_prefix(error));
				} else {
					fprintf(stdout, "read failed\n");
				}
				status = 1;
				free(text);
				free(error);
				continue;
			}
			free(error);
			for (line = text; *line != '\0'; line = next) {
				next = strchr(line, '\n');
				if (next != NULL)
					*next++ = '\0';
				else
					next = line + strlen(line);
				/* The report's label moves into the listing's own, which
				 * carries the index. A line that does not start with the
				 * label keeps the bare index prefix, so nothing is ever
				 * labelled as something it is not. */
				if (strncmp(line, label, label_len) == 0 &&
				    strncmp(line + label_len, ": ", 2) == 0) {
					print_listing_label(label, i);
					fprintf(stdout, "%s\n", line + label_len + 2);
				} else {
					fprintf(stdout, "%d: %s\n", i, line);
				}
			}
			free(text);
		}
		ubertooth_close(uti);
		ubertooth_free(uti);
	}
	return status;
}

/*
 * A failed device read prints no value line, so a negative error code is
 * never shown as a value. libubertooth has already printed the transfer
 * error; this names the query that failed. Returns the failing status.
 */
_Static_assert(UTIL_ERROR_SHORT_REPLY == UBERTOOTH_ERROR_SHORT_REPLY,
               "util_report.h must match libubertooth's short-reply code");

/* "error: <what> read failed" for a failed read, unless code is a short
 * reply, whose whole line libubertooth already printed. Returns 1. */
static int report_read_failed(const char *what, int code)
{
	return util_report_read_error(stderr, what, code);
}

/* Adapters from the injected reset-wait operations to the clock and
 * libubertooth. The context is the -U argument (NULL for the default device). */
static uint64_t reset_wait_now_ms(void *context)
{
	(void)context;
	return util_clock_now_ms();
}

static void reset_wait_sleep_ms(void *context, unsigned ms)
{
	(void)context;
	util_clock_sleep_ms(ms);
}

static int reset_wait_count(void *context)
{
	(void)context;
	return ubertooth_count();
}

static void *reset_wait_try_select(void *context)
{
	return ubertooth_start_select_quiet((const char *)context);
}

static const struct util_reset_wait_ops reset_wait_ops = {
	reset_wait_now_ms,
	reset_wait_sleep_ms,
	reset_wait_count,
	reset_wait_try_select,
};

_Static_assert(UTIL_RESET_SERIAL_STR_LEN == UBERTOOTH_SERIAL_STR_LEN,
               "the followed serial is the library's serial string");
_Static_assert(UTIL_SERIAL_BUFFER_LEN == UBERTOOTH_SERIAL_LEN,
               "a reported serial is the firmware's whole serial reply");

/*
 * Reads the serial of the board about to be reset, in the hex form -U matches,
 * so -r can follow that board and not the selector; see
 * util_reset_follow_init().
 *
 * The quiet read, because -r is the command used to recover a board that has
 * stopped answering: a transfer error and a full second of timeout here would
 * belong to a read the user never asked for. util_reset_follow_init() reports
 * a failed read, once, as the warning that matters.
 */
static int follow_get_serial(void *context, char *serial, size_t size)
{
	return ubertooth_read_serial_string(((ubertooth_t*)context)->devh, serial,
	                                    size, UBERTOOTH_SERIAL_READ_QUIET_MS);
}

static void follow_warn_no_serial(void *context, const char *u_arg)
{
	(void)context;
	fprintf(stderr, "warning: serial read failed on Ubertooth device %s before "
	        "the reset; later options may reach a different board if the "
	        "enumeration order changes\n", u_arg ? u_arg : "0");
}

static const struct util_reset_follow_ops follow_ops = {
	follow_get_serial,
	follow_warn_no_serial,
};

/*
 * Whether the board answers a control request at all, which is what decides
 * how -r resets it.
 *
 * The quiet serial read, the one -r makes anyway. A failed read is tried once
 * more, so that a single transfer which times out on a board that is merely
 * slow does not read as a board answering nothing; a board that answers costs
 * one read, as before.
 */
static int board_answers(ubertooth_t *ut)
{
	char serial[UTIL_RESET_SERIAL_STR_LEN];

	if (follow_get_serial(ut, serial, sizeof(serial)) == 0)
		return 1;
	return follow_get_serial(ut, serial, sizeof(serial)) == 0;
}

/* How long -r gives a board to answer again after its USB connection was
 * reset, and how often it asks. The bench answer came back on the first ask;
 * the rest of the window -- up to about five seconds, because each ask is two
 * reads that can take their 200 ms timeout -- is for a board whose firmware
 * is still starting up. */
#define UTIL_USB_RESET_POLLS 10u
#define UTIL_USB_RESET_POLL_MS 100u

/*
 * Resets the board's USB connection, for -r on a board that answers nothing.
 *
 * Such a board refuses the reset request like every other request, so sending
 * it leaves the board exactly as it was -- and -r then waited ten seconds and
 * reported that the board never came back. What does bring it back is the host
 * resetting the USB connection, which stands in for the re-plug the user would
 * otherwise have to do by hand -- the board keeps its power, so this is the
 * connection being made afresh rather than the board being restarted. On the
 * bench a board that had answered nothing since a capture was killed answered
 * its first control request straight after one.
 *
 * This runs before the reset request rather than instead of it, so -r still
 * means a full reset whichever way it went.
 *
 * libusb keeps this handle usable across a reset it could carry out, which is
 * what `handle_alive` says. A reset that re-enumerated the board reports that
 * the device has gone; nothing is wrong with it, but this handle is dead, so
 * the caller waits for the board to come back before resetting it.
 */
static int reset_usb_connection(ubertooth_t *ut, const char *u_arg,
                                int *handle_alive)
{
	unsigned i;
	int r;

	*handle_alive = 0;
	fprintf(stderr, "Ubertooth device %s is not answering; resetting its USB "
	        "connection\n", u_arg ? u_arg : "0");
	r = libusb_reset_device(ut->devh);
	/* The device went and came back as another one: NOT_FOUND is what libusb
	 * documents for that, and the macOS backend can report the same thing as
	 * a device that is no longer there. */
	if (r == LIBUSB_ERROR_NOT_FOUND || r == LIBUSB_ERROR_NO_DEVICE)
		return 0;
	if (r < 0) {
		fprintf(stderr, "error: resetting the USB connection of Ubertooth "
		        "device %s failed (libusb %d)\n", u_arg ? u_arg : "0", r);
		return -1;
	}

	for (i = 0; i < UTIL_USB_RESET_POLLS; ++i) {
		if (board_answers(ut)) {
			*handle_alive = 1;
			return 0;
		}
		/* Between the asks only: a sleep after the last one would delay the
		 * report by a poll for nothing. */
		if (i + 1 < UTIL_USB_RESET_POLLS)
			util_clock_sleep_ms(UTIL_USB_RESET_POLL_MS);
	}
	/* The reset went through, this handle is still the board's, and it
	 * answers nothing: that is as far as the host can take it, and what is
	 * left is the board's own power -- its port on a hub, and unplugging it
	 * otherwise. */
	fprintf(stderr, "error: Ubertooth device %s is still not answering after "
	        "its USB connection was reset; power-cycle the board\n",
	        u_arg ? u_arg : "0");
	return -1;
}

/*
 * Resets the board and waits for it to come back, which is what -r is: the
 * request is answered by a board that reboots, so this handle goes dead and
 * the board is opened again.
 *
 * `send_request` is 0 for the one caller that has no board on its handle to
 * ask -- a USB reset that re-enumerated it -- and only waits.
 *
 * Returns the reopened handle, which is the caller's to use from then on, or
 * NULL once it has reported that the board never came back. The old handle is
 * released either way.
 */
static ubertooth_t *reset_and_reopen(ubertooth_t *ut, const char *u_arg,
                                     const char *selector,
                                     struct util_reset_wait_params *wait,
                                     int send_request, int *status)
{
	int libusb_held;

	if (send_request) {
		fprintf(stderr, "Resetting ubertooth device %s\n", u_arg ? u_arg : "0");
		*status = util_status_after(*status, cmd_reset(ut->devh));
	}
	/* The device re-enumerates after a reset, so this handle is about to go
	 * dead: release it without sending STOP (if the reset failed the device
	 * is still there, but a STOP is not wanted either), then wait for the
	 * same selection to open again. The polls are quiet and never send STOP.
	 *
	 * One libusb reference is held for the whole wait, so the polls never
	 * tear down the libusb context while the device re-enumerates (on macOS
	 * the final libusb_exit() after a device reset can hang). */
	libusb_held = libusb_init(NULL) >= 0;
	ubertooth_close(ut);
	ubertooth_free(ut);
	/* cast away const: the context is only read, as a selector string */
	ut = util_reset_wait(&reset_wait_ops, (void *)selector, wait);
	if (ut == NULL) {
		/* The held reference is left to process exit, for the same reason. */
		fprintf(stderr, "error: Ubertooth device %s did not come back within "
		        "%u s after the reset\n", u_arg ? u_arg : "0",
		        UTIL_RESET_WAIT_DEADLINE_MS / 1000u);
		return NULL;
	}
	/* Not the last reference: the reopened device holds its own. */
	if (libusb_held)
		libusb_exit(NULL);
	return ut;
}

/*
 * How long -n waits for the repeater's reply before it gives up, how often it
 * asks the board how the test is going, and how often it says so.
 *
 * Ten seconds is the whole exchange with room to spare: the sender's burst
 * takes about a second, the repeater waits another second before it answers so
 * the sender is listening by then, and the reply burst is a second more. A
 * missing repeater is therefore reported in ten seconds rather than never.
 * -n 0 waits forever, for a test being set up by hand at the far end.
 */
#define RANGE_TEST_TIMEOUT_S 10
#define RANGE_TEST_POLL_MS 100
#define RANGE_TEST_PROGRESS_MS 1000

/*
 * How many polls in a row may fail before -n gives up on the board.
 *
 * A board part-way through a mode stalls the odd control request: one poll in
 * about twenty on the bench. The next one answers, so a single failed read is
 * not the test failing, and the polls are quiet for that reason. A board that
 * has stopped answering altogether still ends the wait, on the third failure
 * in a row.
 */
#define RANGE_TEST_READ_FAILURES 3

/*
 * The range test result block, which -m and -n both print, so a result reads
 * the same whichever asked for it.
 *
 * Returns 0 when a result (or the absence of one) was reported, 1 when the
 * test ran but its reply came back corrupted: that is a diagnostic rather
 * than a value, so it goes to standard error and the command fails.
 */
static int report_range_result(const rangetest_result *rr)
{
	if (rr->valid == 1) {
		fprintf(stdout, "request PA level : %d\n", rr->request_pa);
		fprintf(stdout, "request number   : %d\n", rr->request_num);
		fprintf(stdout, "reply PA level   : %d\n", rr->reply_pa);
		fprintf(stdout, "reply number     : %d\n", rr->reply_num);
		return 0;
	}
	if (rr->valid > 1) {
		/* A reply arrived whose payload is not the one that was sent, so
		 * the levels it carries cannot be trusted. */
		fprintf(stderr, "error: invalid range test: mismatch on byte %d\n",
		        rr->valid - 2);
		return 1;
	}
	/* valid is 0 until a range test completes once: there is no result yet,
	 * which is not an error. */
	fprintf(stdout, "no range test result yet\n");
	return 0;
}

/*
 * A packet the board heard that was not its own reply, mentioned once.
 *
 * The board discards such a packet -- another pair's range test on the same
 * channel, or a stray packet its CRC accepted -- and keeps listening, so the
 * run still finishes with a result. This line says what went past, because the
 * same report for a reply that arrived corrupted is the diagnosis a FIFO read
 * that started a byte early was found with.
 *
 * Takes and returns whether the warning has already been printed, so a run
 * that discards several packets is reported once and not once a poll.
 */
static int report_range_discard(const rangetest_result *rr, int warned)
{
	if (warned || rr->discarded == 0)
		return warned;

	fprintf(stderr, "warning: range test: a packet that is not this board's "
	        "reply was ignored (first difference at byte %d)\n",
	        rr->discarded - 2);
	return 1;
}

/*
 * Leaves the board idle at the end of a test that did not finish: it would
 * otherwise listen forever, and the next command would find a board that
 * looks busy. Returns the clause the caller's message ends with, so no
 * message claims a stop that did not happen.
 */
static const char *range_test_stop(ubertooth_t *ut)
{
	if (cmd_stop(ut->devh) >= 0)
		return "the range test has been stopped";
	return "the range test has not been stopped; -S stops it";
}

/* The end of a wait whose polls keep failing: the read that gave up, and what
 * became of the test. Returns 1, the status the wait fails with. */
static int range_test_read_failed(ubertooth_t *ut, const char *what)
{
	const char *left = range_test_stop(ut);

	fprintf(stderr, "error: %s read failed; %s\n", what, left);
	return 1;
}

/* The end of a wait the user stopped: Ctrl-C, or any of the other signals the
 * library's handler takes. Nobody is waiting for the repeater's reply now, so
 * the board is left as the wait's own endings leave it -- idle -- rather than
 * listening until someone runs -S. Returns 1, the status the wait fails
 * with. */
static int range_test_interrupted(ubertooth_t *ut)
{
	const char *left = range_test_stop(ut);

	fprintf(stderr, "error: interrupted before the range test finished; %s\n",
	        left);
	return 1;
}

/*
 * Follows the range test the board has just been started on, and reports it.
 *
 * The board transmits its burst and then listens for the repeater's reply.
 * Two reads say how that is going: the range test result, which stays empty
 * until a reply arrives, and the mode the board is in, which the watchdog
 * breadcrumb reports. Polling them turns -n from "the test has been started"
 * into the test itself -- a progress line about once a second, then the same
 * result block -m prints.
 *
 * With no repeater in range the board listens forever, so the wait has a
 * deadline; timeout_s of 0 waits forever. A signal ends the wait too, which
 * is why the caller installs the library's handler first. Returns 0 when the
 * test finished and its result was printed, 1 otherwise.
 */
static int range_test_wait(ubertooth_t *ut, int timeout_s)
{
	uint64_t start = util_clock_now_ms();
	uint64_t next_progress = RANGE_TEST_PROGRESS_MS;
	int cleared = 0;  /* the board has cleared the previous test's result */
	int started = 0;  /* ...and has been seen running this one */
	int failures = 0; /* polls in a row that could not read the board */
	int warned = 0;   /* a discarded packet has been reported */

	for (;;) {
		rangetest_result rr;
		watchdog_info info;
		uint64_t elapsed = util_clock_now_ms() - start;

		if (ubertooth_stopping(ut))
			return range_test_interrupted(ut);
		if (timeout_s > 0 && elapsed >= (uint64_t)timeout_s * 1000u) {
			const char *left = range_test_stop(ut);

			/* The repeater is the only thing that answers a range test,
			 * so it is the likely cause -- unless the board was never
			 * seen running the test at all, which is not the repeater's
			 * doing and would send the user looking in the wrong place. */
			if (!started)
				fprintf(stderr, "error: the board did not start the range "
				        "test within %d s; %s\n", timeout_s, left);
			else
				fprintf(stderr, "error: no reply from the repeater within "
				        "%d s: start one in range with ubertooth-util -e, "
				        "or wait longer with -n <seconds>; %s\n",
				        timeout_s, left);
			return 1;
		}
		if (elapsed >= next_progress) {
			/* Progress, so standard error: standard output carries the
			 * result block alone, and a script reading -n for a result
			 * reads nothing else. */
			fprintf(stderr, "waiting for the repeater... (%u s)\n",
			        (unsigned)(elapsed / 1000u));
			next_progress = elapsed + RANGE_TEST_PROGRESS_MS;
		}

		/*
		 * The wait before the read, so the first read is a poll interval
		 * into the test: the board has only just been asked to start it,
		 * so there is nothing to read yet, and a read that lands in the
		 * moment the mode is starting is the one the board has been seen
		 * to stall.
		 */
		util_clock_sleep_ms(RANGE_TEST_POLL_MS);
		/* Again after the sleep, which is where a signal arrives: the two
		 * reads below would otherwise go to a board that is about to be
		 * stopped anyway. */
		if (ubertooth_stopping(ut))
			return range_test_interrupted(ut);
		if (cmd_get_rangeresult_quiet(ut->devh, &rr) != 0) {
			/* A poll that learned nothing; see RANGE_TEST_READ_FAILURES.
			 * Giving up stops the test as the deadline does: nobody is
			 * watching it now. */
			if (++failures >= RANGE_TEST_READ_FAILURES)
				return range_test_read_failed(ut, "range test result");
			continue;
		}
		/*
		 * The firmware clears the result as the test starts, and sets it
		 * only at the end of a run, so a read that lands before the board
		 * has entered the mode still answers with the previous test's
		 * levels. Waiting for one empty read first means every result
		 * below belongs to this test.
		 */
		if (rr.valid == 0)
			cleared = 1;
		/* Only once the board has cleared the previous run's result does
		 * what it reports describe this run. */
		if (cleared)
			warned = report_range_discard(&rr, warned);
		if (rr.valid != 0 && cleared)
			return report_range_result(&rr);

		if (cmd_get_watchdog_quiet(ut->devh, &info) != 0) {
			if (++failures >= RANGE_TEST_READ_FAILURES)
				return range_test_read_failed(ut, "board mode");
			continue;
		}
		failures = 0;
		if (info.mode == MODE_RANGE_TEST)
			started = 1;
		else if (started) {
			/*
			 * The test is over without a result in hand. The firmware
			 * writes the result just before it leaves the mode, so read
			 * once more: that settles whether the test finished between
			 * the two reads above or ended without a reply.
			 */
			if (cmd_get_rangeresult_quiet(ut->devh, &rr) != 0) {
				/* This read is the one deciding it, so a stall here is
				 * not an answer: count it and look again, rather than
				 * calling a finished test a stopped one. */
				if (++failures >= RANGE_TEST_READ_FAILURES)
					return range_test_read_failed(ut,
					        "range test result");
				continue;
			}
			if (cleared)
				warned = report_range_discard(&rr, warned);
			if (cleared && rr.valid != 0)
				return report_range_result(&rr);
			/* No stop to send: the board has left the test by itself,
			 * which is where the other endings have to put it. */
			fprintf(stderr, "error: the range test was stopped before the "
			        "repeater replied\n");
			return 1;
		}
	}
}

/*
 * -I needs this tree's firmware. ubertooth-util does not call
 * ubertooth_check_api() (it must still read the version of, and reflash, any
 * firmware), so -I checks the version itself, before it prints or sends
 * anything. Firmware of another API version goes through the library's
 * mismatch message, which also decides whether the mode may run, so -I says
 * and does what every other tool says and does.
 *
 * Returns 0 if the mode may run, or a negative error.
 */
static int identify_check_api(ubertooth_t* ut)
{
	uint16_t version = 0;
	int result = ubertooth_get_api(ut, &version);

	if (result < 0)
		return result;
	if (version != UBERTOOTH_API_VERSION)
		return ubertooth_report_api_mismatch(version, ut->selector);
	return 0;
}

static void usage(FILE *output)
{
	fprintf(output, "ubertooth-util - command line utility for Ubertooth Zero and Ubertooth One\n");
	fprintf(output, "\n");
	fprintf(output, "Common options:\n");
	fprintf(output, "\t-h this help\n");
	fprintf(output, "\t-V print version information: the host tools, and the\n");
	fprintf(output, "\t   firmware version and build of each attached board\n");
	fprintf(output, "\t-v get firmware image and revision (deprecated: use -V)\n");
	fprintf(output, "\t-I identify ubertooth device by flashing the USR, RX, and TX LEDs on the board, until -S or any mode option\n");
	fprintf(output, "\t-d [0-1] get/set all LEDs\n");
	fprintf(output, "\t-l [0-1] get/set USR LED\n");
	fprintf(output, "\t-S stop current operation\n");
	fprintf(output, "\t-r full reset\n");
	fputs(OPT_USAGE_SELECT_DEVICE, output);
	fprintf(output, "\t-U all list -b/-v/-p/-s for every Ubertooth (queries only)\n");
	fprintf(output, "\t-N print the number of attached Ubertooths, then run the rest of the command\n");
	fprintf(output, "\n");
	fprintf(output, "Radio options:\n");
	fprintf(output, "\t-a [0-7] get/set power amplifier level\n");
	fprintf(output, "\t-c [%d-%d] get/set channel in MHz\n", BT_FREQ_MIN, BT_FREQ_MAX);
	fprintf(output, "\t-C [0-%d] get/set channel\n", NUM_BREDR_CHANNELS - 1);
	fprintf(output, "\t-q [%d-%d] start LED spectrum analyzer, threshold -N dBm (default: %d)\n",
	        UBERTOOTH_LED_SPECAN_N_MIN, UBERTOOTH_LED_SPECAN_N_MAX,
	        UBERTOOTH_LED_SPECAN_N_DEFAULT);
	fprintf(output, "\t   the threshold is -N dBm; a larger N lights the LEDs for weaker signals\n");
	fprintf(output, "\t-t initiate continuous transmit test\n");
	fprintf(output, "\t-z [-128..127] get/set squelch level\n");
	fprintf(output, "\n");
	fprintf(output, "Range test:\n");
	fprintf(output, "\t-e start repeater mode\n");
	fprintf(output, "\t-m display range test result\n");
	fprintf(output, "\t-n [seconds] run a range test, reporting its progress and result\n");
	fprintf(output, "\t   waits this long for the repeater's reply (default: %d; 0 waits forever)\n",
	        RANGE_TEST_TIMEOUT_S);
	fprintf(output, "\n");
	fprintf(output, "Miscellaneous:\n");
	fprintf(output, "\t-i activate In-System Programming (ISP) mode\n");
	fprintf(output, "\t   the board answers nothing else until it is power-cycled\n");
	fprintf(output, "\t-b get hardware board id number\n");
	fprintf(output, "\t-B get the bootloader this board has: its version and\n");
	fprintf(output, "\t   build if it reports them, and a CRC-32 of the bootloader either way\n");
	fprintf(output, "\t-p get microcontroller Part ID\n");
	fprintf(output, "\t-w get the watchdog state, why the board last reset itself,\n");
	fprintf(output, "\t   and how many radio waits gave up\n");
	fprintf(output, "\t-s get microcontroller serial number\n");
	fprintf(output, "\t-x xmas lights\n");
}

int main(int argc, char *argv[])
{
	int opt;
	int other_operations = 0;   /* an option other than -s -v -p -b -U -N was given */
	/* An option other than -N and the -U selector was given. Where it is
	 * read, -h and -V have already ended the command, so what is left needs
	 * a device. */
	int operations_besides_count = 0;
	int status = 0; /* 0 on success, 1 if any operation failed; never a device value */
	ubertooth_t* ut = NULL;
	rangetest_result rr;
	int do_stop, do_isp, do_leds, do_part, do_reset;
	int do_serial, do_tx, do_palevel, do_channel, do_led_specan;
	int do_range_test, do_repeater, do_firmware, do_board_id;
	int do_range_result, do_all_leds, do_identify, do_bootloader;
	/* A set and a read of the same value have their own flags, as the
	 * squelch pair below does, so one command can do both: the set runs
	 * first and the read then reports the new value. Sharing a variable
	 * made the later option overwrite the earlier one and drop it
	 * silently. */
	int do_get_leds, do_get_all_leds, do_get_channel, do_get_palevel;
	int do_set_squelch, do_get_squelch, squelch_level;
	/* -h and -V: both end the command, and a command line with both prints
	 * both (see below). -V reads the board, so it has a result of its own
	 * where -h cannot fail. */
	int do_help, do_version, version_status;
	/* -c or -C, whichever named the channel: they are the same setting
	 * written two ways, so a command line may give only one of them. */
	int channel_option = 0;
	opt_once once = {0};
	int do_number, do_xmas, do_watchdog;
	int range_test_timeout = RANGE_TEST_TIMEOUT_S; /* -n's value, in seconds */
	char* ubertooth_device = NULL;
	int requested[QUERY_COUNT]; /* identity queries, for the per-device listing */
	int any_query, list_all;
	int device_count = -1;      /* a successful ubertooth_count(), to reuse */
	int modes[UTIL_MODE_COUNT]; /* exclusive mode options that were given */

	/* The do_<thing> states below start negative: -1 means not asked for,
	 * 0 means 'do it' and a positive value is the argument given. The
	 * do_get_<thing> flags beside them are plain 0/1 booleans, since a read
	 * takes no value; do_set_squelch and do_get_squelch are the older pair,
	 * -1 or 1 and tested with > 0. */
	do_stop= do_isp= do_leds= do_part= do_reset= -1;
	do_serial= do_tx= do_palevel= do_channel= do_led_specan= -1;
	do_range_test= do_repeater= do_firmware= do_board_id= -1;
	do_range_result= do_all_leds= do_identify= do_bootloader= -1;
	do_get_leds= do_get_all_leds= do_get_channel= do_get_palevel= 0;
	do_set_squelch= -1, do_get_squelch= -1; squelch_level= 0;
	do_number= 0; do_xmas= 0; do_watchdog= -1;
	do_help= 0; do_version= 0; version_status= 0;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	/* Named, so the loop below and opt_report_bad_option()
	 * cannot be given different letters. */
	const char options[] = ":U:hn::meiIprsStvbBwl::a::C::c::d::q::z::VNx";
	opterr = 0;
	while ((opt=getopt(argc, argv, options)) != EOF) {
		/* An optional value may also be the next word: -c 2440, -z -90. Only
		 * -z takes a negative value (a signed RSSI threshold). Resolved
		 * before the check below, which only counts an option that came
		 * with a value. */
		if (optarg == NULL && strchr("lacCdqzn", opt) != NULL)
			optarg = opt_separate_value(argc, argv,
			                            opt == 'z' ? opt_value_signed_decimal
			                                       : opt_value_decimal);
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
		/* Anything but an identity query, -U or -N needs a single selected
		 * device. -N opens none: it counts boards on the USB bus. */
		if (strchr("svpbUN", opt) == NULL)
			other_operations = 1;
		/* Whether anything beyond the count was asked for. -N counts the
		 * boards on the USB bus and -U only says which board the rest of the
		 * command line acts on, so a command line of those two alone needs
		 * no device: -U on its own asks nothing of the board it selects. */
		if (opt != 'N' && opt != 'U')
			operations_besides_count = 1;
		switch(opt) {
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'i':
			do_isp= 0;
			break;
		case 'I':
			do_identify= 0;
			break;
		case 'l':
			if (optarg) {
				if (opt_require_int(optarg, 0, 1, "USR LED state", "", &do_leds) < 0)
					return 1;
			} else
				do_get_leds= 1;
			break;
		case 'd':
			if (optarg) {
				if (opt_require_int(optarg, 0, 1, "all-LED state", "", &do_all_leds) < 0)
					return 1;
			} else
				do_get_all_leds= 1;
			break;
		case 'p':
			do_part= 0;
			break;
		case 'r':
			do_reset= 0;
			break;
		case 's':
			do_serial= 0;
			break;
		case 'S':
			do_stop= 0;
			break;
		case 't':
			do_tx= 0;
			break;
		case 'a':
			/* -a reads the PA level; -a<0-7> sets it, including 0
			 * (-1: not requested). */
			if (optarg) {
				if (optarg[0] < '0' || optarg[0] > '7' || optarg[1] != '\0') {
					fprintf(stderr, "error: PA level must be a single digit 0-7 (got %s)\n", optarg);
					return 1;
				}
				do_palevel= optarg[0] - '0';
			} else {
				do_get_palevel= 1;
			}
			break;
		case 'C':
		case 'c':
			if (optarg) {
				/* -c is the channel in MHz and -C its BR/EDR
				 * channel number: one setting, two spellings.
				 * Both used to be accepted and the second one
				 * sent, so a command line that named two
				 * different channels set whichever came last
				 * without a word. A set written one way and a
				 * read written the other is still one setting:
				 * only two values clash. */
				if (channel_option != 0) {
					fprintf(stderr, "error: -%c cannot be combined with"
					        " -%c; they are the same channel setting"
					        " written two ways\n", opt, channel_option);
					return 1;
				}
				channel_option = opt;
				if (opt == 'C') {
					int ch;
					if (opt_require_int(optarg, 0, NUM_BREDR_CHANNELS - 1, "channel", "", &ch) < 0)
						return 1;
					do_channel= ch + BT_FREQ_MIN;
				} else if (opt_require_int(optarg, BT_FREQ_MIN, BT_FREQ_MAX, "frequency", " MHz", &do_channel) < 0) {
					return 1;
				}
			} else
				do_get_channel= 1;
			break;
		case 'q':
			if (optarg) {
				if (opt_require_int(optarg, UBERTOOTH_LED_SPECAN_N_MIN,
				                    UBERTOOTH_LED_SPECAN_N_MAX, "RSSI threshold",
				                    "", &do_led_specan) < 0)
					return 1;
			} else
				do_led_specan= 0; /* the default threshold */
			break;
		case 'n':
			do_range_test= 0;
			/* bare -n keeps the default deadline */
			if (optarg &&
			    opt_require_count(optarg, 0, "timeout", "seconds",
			                      "0 for none", &range_test_timeout) < 0)
				return 1;
			break;
		case 'm':
			do_range_result= 0;
			break;
		case 'e':
			do_repeater= 0;
			break;
		case 'v':
			do_firmware= 0;
			break;
		case 'b':
			do_board_id= 0;
			break;
		case 'B':
			do_bootloader= 0;
			break;
		case 'z':
			if (optarg) {
				/* a signed 8-bit RSSI threshold; values above 0 are
				 * relative to the average RSSI */
				if (opt_require_int(optarg, -128, 127, "squelch level", "", &squelch_level) < 0)
					return 1;
				do_set_squelch = 1;
			}
			else {
				do_get_squelch = 1;
			}
			break;
		case 'V':
			do_version = 1;
			break;
		case 'N':
			do_number = 1;
			break;
		case 'w':
			do_watchdog = 0;
			break;
		case 'x':
			do_xmas = 1;
			break;
		case 'h':
			do_help = 1;
			break;
		default:
			opt_report_bad_option(opt, argc, argv, options);
			return 1;
		}
	}

	/* ubertooth-util takes no operands. Refuse a leftover word before any
	 * device is opened: otherwise a typo such as "- V" (for -V) is dropped
	 * silently while the rest of the command still runs. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h and -V end the command before anything else, but only once the
	 * whole command line has parsed: an invalid option value or a stray
	 * word is reported even alongside them. A command line with both prints
	 * both, and the usage text comes last so the version is not scrolled
	 * off the screen by it; the order they were written in does not matter.
	 *
	 * -V reads the board named with -U, or every board for "-U all", which
	 * is what it does with no -U anyway. */
	if (do_version)
		version_status = ubertooth_print_versions(stdout,
		        (ubertooth_device != NULL &&
		         strcmp(ubertooth_device, "all") == 0) ? NULL
		                                              : ubertooth_device);
	if (do_help)
		usage(stdout);
	if (do_version || do_help)
		return version_status;

	/* Only one mode can run: each one ends the command. Refuse a combination
	 * before any device is opened, instead of running whichever mode comes
	 * first below and silently ignoring the others. */
	modes[UTIL_MODE_LED_SPECAN] = (do_led_specan >= 0);
	modes[UTIL_MODE_RANGE_TEST] = (do_range_test == 0);
	modes[UTIL_MODE_REPEATER] = (do_repeater == 0);
	modes[UTIL_MODE_TX_TEST] = (do_tx == 0);
	modes[UTIL_MODE_XMAS] = do_xmas;
	modes[UTIL_MODE_IDENTIFY] = (do_identify == 0);
	modes[UTIL_MODE_ISP] = (do_isp == 0);
	if (util_check_modes(modes, stderr) != 0)
		return 1;
	/* The squelch is a firmware setting for later RX capture. A mode cannot
	 * use it: each mode first stops the device, and stopping a running mode
	 * resets the squelch to its default, and none of these modes reads it.
	 * Refuse the combination instead of silently dropping -z. */
	if (do_set_squelch > 0 || do_get_squelch > 0) {
		for (int m = 0; m < UTIL_MODE_COUNT; m++) {
			if (modes[m]) {
				fprintf(stderr, "error: -z cannot be combined with %s\n",
				        util_mode_option((enum util_mode)m));
				return 1;
			}
		}
	}

	requested[QUERY_BOARD_ID] = (do_board_id == 0);
	requested[QUERY_FIRMWARE] = (do_firmware == 0);
	requested[QUERY_PART_ID] = (do_part == 0);
	requested[QUERY_SERIAL] = (do_serial == 0);
	any_query = requested[QUERY_BOARD_ID] || requested[QUERY_FIRMWARE] ||
	            requested[QUERY_PART_ID] || requested[QUERY_SERIAL];

	/* -U all lists the read-only identity queries for every attached device,
	 * so it is refused for anything that acts on one device: setters, modes
	 * and other reads. -N asks about the bus rather than a device, so it is
	 * allowed alongside the listing, but it is not a query the listing can
	 * show: -U all with nothing but -N beside it is refused for having
	 * nothing to list, exactly as -U all on its own is. Every refusal here
	 * comes before -N prints, so a refused command line writes nothing to
	 * standard output. */
	list_all = (ubertooth_device != NULL && strcmp(ubertooth_device, "all") == 0);
	if (list_all && (other_operations || !any_query)) {
		if (other_operations)
			fprintf(stderr, "error: -U all only supports the read-only queries "
			        "-b, -v, -p and -s; select one device with "
			        "-U<index|serial> for other options\n");
		else
			/* Say what the listing is missing: naming the options -U all does
			 * not support describes a command line the user did not type. */
			fprintf(stderr, "error: -U all needs one of the read-only queries "
			        "-b, -v, -p or -s to list\n");
		return 1;
	}

	/* A command line that asks for nothing is a mistake, not a no-op:
	 * without this it would open the selected device, do nothing with it and
	 * exit 0. -N is an operation in its own right, and -h and -V have
	 * already ended the command above.
	 *
	 * -N counts the boards on the USB bus and asks nothing of any one of
	 * them, so a -U selector beside it still selects a device that nothing
	 * acts on: that is refused in the same words as -U on its own, rather
	 * than printing a count the selector had no part in. */
	if (!operations_besides_count && (ubertooth_device != NULL || !do_number)) {
		/* The selector is quoted, as every other message quoting the user's
		 * input is: an empty -U value would otherwise read as no value. */
		if (ubertooth_device != NULL)
			fprintf(stderr, "error: nothing to do: -U '%s' selects a device "
			        "but no option asks anything of it\n", ubertooth_device);
		else
			fprintf(stderr, "error: nothing to do: give an option that reads "
			        "or sets something, or -N to count the attached boards\n");
		if (opt_no_arguments(argc))
			usage(stderr);
		return 1;
	}

	/* -N counts the Ubertooths on the USB bus and opens none of them, so it
	 * has nothing to drop: the count goes first and the rest of the command
	 * line still runs. Flushed so it really is first when standard output
	 * and standard error share a file. */
	if (do_number) {
		device_count = ubertooth_count();
		if (device_count < 0)
			status = util_status_merge(status, 1);
		else
			printf("%d\n", device_count);
		fflush(stdout);
		/* -N on its own asks for nothing that needs a device. A selector
		 * beside it was refused above. */
		if (!operations_besides_count)
			return status;
	}

	/* The listings take the count -N already has rather than enumerate
	 * again: a board unplugged between the two would make the printed
	 * number disagree with the listing under it. */
	if (list_all) {
		int count = device_count >= 0 ? device_count : ubertooth_count();

		if (count < 0)
			return util_status_merge(status, 1);
		if (count == 0) {
			fprintf(stderr, "error: no Ubertooth devices found\n");
			return util_status_merge(status, 1);
		}
		return util_status_merge(status,
		        list_identity_queries(requested, count));
	}

	/* Read-only identity queries (-b, -v, -p, -s) with no -U and several
	 * devices: report every device instead of refusing to pick one. Any other
	 * operation in the same command still needs -U. */
	if (ubertooth_device == NULL && !other_operations && any_query) {
		int count = device_count >= 0 ? device_count : ubertooth_count();

		if (count > 1)
			return util_status_merge(status,
			        list_identity_queries(requested, count));
	}

	/* initialise device */
	ut = ubertooth_start_select(ubertooth_device);
	if (ut == NULL) {
		return 1;
	}
	if(do_reset == 0) {
		struct util_reset_wait_params wait = {
			UTIL_RESET_WAIT_INITIAL_DELAY_MS, UTIL_RESET_WAIT_POLL_MS,
			UTIL_RESET_WAIT_LEAVE_MS, UTIL_RESET_WAIT_DEADLINE_MS, 0, 0
		};
		struct util_reset_follow follow;
		/* Whether the board is still on this handle after a USB reset. Only
		 * a reset that re-enumerated it says no. */
		int handle_alive = 1;

		/* Counted before the reset, so the wait can see the device leave and,
		 * where the selection is by index, not open another board while it is
		 * gone. The leave wait is kept even if the reset command reports an
		 * error, since the device may have reset anyway. */
		wait.count_before = ubertooth_count();
		/* A board that answers nothing refuses the reset request like every
		 * other request, so sending it would leave the board as it was. Its
		 * USB connection is reset first, and the request follows on the
		 * board that answers again: -r is a full reset either way. */
		if (!board_answers(ut) &&
		    reset_usb_connection(ut, ubertooth_device, &handle_alive) < 0) {
			/* Closed without a STOP: a board that answers nothing refuses
			 * that request too. */
			ubertooth_close(ut);
			ubertooth_free(ut);
			return 1;
		}
		/* Read while the device is still open: the wait then looks for this
		 * board's serial, so later options in the same command cannot land on
		 * another board whose index changed across the reset. A handle the
		 * USB reset took with it reads nothing, which falls back to the -U
		 * argument as an unreadable serial always has. */
		util_reset_follow_init(&follow, &follow_ops, ut, ubertooth_device,
		                       wait.count_before);
		wait.select_needs_count = follow.select_needs_count;
		if (!handle_alive) {
			/* The board is re-enumerating, so there is nothing to send the
			 * request on yet: it is waited for first, and reset below once
			 * it is open again. */
			ut = reset_and_reopen(ut, ubertooth_device, follow.selector,
			                      &wait, 0, &status);
			if (ut == NULL)
				return 1;
			/* It is back, so the count the next wait compares against is
			 * the whole bus again -- and its serial can be read now, which
			 * the wait above had to do without. */
			wait.count_before = ubertooth_count();
			util_reset_follow_init(&follow, &follow_ops, ut, ubertooth_device,
			                       wait.count_before);
			wait.select_needs_count = follow.select_needs_count;
		}
		ut = reset_and_reopen(ut, ubertooth_device, follow.selector, &wait, 1,
		                      &status);
		if (ut == NULL)
			return 1;
	}
	if(do_stop == 0) {
		fprintf(stderr, "Stopping ubertooth device %s\n", ubertooth_device ? ubertooth_device : "0");
		status = util_status_after(status, cmd_stop(ut->devh));
	}

	/* device configuration actions */
	if(do_all_leds == 0 || do_all_leds == 1) {
		status = util_status_after(status, cmd_set_usrled(ut->devh, do_all_leds));
		status = util_status_after(status, cmd_set_rxled(ut->devh, do_all_leds));
		status = util_status_after(status, cmd_set_txled(ut->devh, do_all_leds));
	}
	if(do_channel > 0)
		status = util_status_after(status, cmd_set_channel(ut->devh, do_channel));
	if(do_leds == 0 || do_leds == 1)
		status = util_status_after(status, cmd_set_usrled(ut->devh, do_leds));
	if(do_palevel >= 0)
		status = util_status_after(status, cmd_set_palevel(ut->devh, do_palevel));

	/* reporting actions, in the order ubertooth-util has always printed them */
	if(do_get_all_leds) {
		int usr = cmd_get_usrled(ut->devh);
		int rx = cmd_get_rxled(ut->devh);
		int tx = cmd_get_txled(ut->devh);
		if (usr < 0)
			status = util_status_merge(status, report_read_failed("USR LED", usr));
		else
			fprintf(stdout, "USR LED status: %d\n", usr);
		if (rx < 0)
			status = util_status_merge(status, report_read_failed("RX LED", rx));
		else
			fprintf(stdout, "RX LED status : %d\n", rx);
		if (tx < 0)
			status = util_status_merge(status, report_read_failed("TX LED", tx));
		else
			fprintf(stdout, "TX LED status : %d\n", tx);
	}
	if(do_board_id == 0)
		status = util_status_merge(status,
		        util_report_board_id(&report_ops, ut, stdout, stderr));
	if(do_bootloader == 0) {
		struct ubertooth_bootloader_info info;
		int b = cmd_get_bootloader_info(ut->devh, &info);

		if (b < 0)
			status = util_status_merge(status,
			        report_read_failed("bootloader information", b));
		else
			status = util_status_merge(status,
			        util_report_bootloader(stdout, stderr, &info));
	}
	if(do_get_channel) {
		int mhz = cmd_get_channel(ut->devh);
		if (mhz < 0)
			status = util_status_merge(status, report_read_failed("channel", mhz));
		else
			fprintf(stdout, "Current frequency: %d MHz (Bluetooth channel %d)\n", mhz, mhz - BT_FREQ_MIN);
	}
	if(do_firmware == 0)
		status = util_status_merge(status, report_firmware(ut, stdout));
	if(do_get_leds) {
		int led = cmd_get_usrled(ut->devh);
		if (led < 0)
			status = util_status_merge(status, report_read_failed("USR LED", led));
		else
			fprintf(stdout, "USR LED status: %d\n", led);
	}
	if(do_get_palevel) {
		int level = cmd_get_palevel(ut->devh);
		if (level < 0)
			status = util_status_merge(status, report_read_failed("PA level", level));
		else
			fprintf(stdout, "PA Level: %d\n", level);
	}
	if(do_part == 0)
		status = util_status_merge(status,
		        util_report_part_id(&report_ops, ut, stdout, stderr));
	if(do_range_result == 0) {
		int rr_status = cmd_get_rangeresult(ut->devh, &rr);
		if (rr_status != 0)
			status = util_status_merge(status,
			        report_read_failed("range test result", rr_status));
		else
			status = util_status_merge(status, report_range_result(&rr));
	}

	if(do_serial == 0)
		status = util_status_merge(status,
		        util_report_serial(&report_ops, ut, stdout, stderr));

	if(do_watchdog == 0) {
		watchdog_info info;
		int w = cmd_get_watchdog(ut->devh, &info);

		if (w < 0) {
			status = util_status_merge(status,
			        report_read_failed("watchdog state", w));
		} else {
			/* Printed from the bytes as they arrived, so the report and the
			 * firmware agree on the order without a struct layout in
			 * between. */
			uint8_t raw[UBERTOOTH_WATCHDOG_INFO_LEN] = {
				info.reset_cause, info.armed, info.resets, info.boots,
				info.reset_mode, info.reset_site, info.mode, info.site,
				info.transfer_errors & 0xff,
				(info.transfer_errors >> 8) & 0xff,
				info.fault_status & 0xff,
				(info.fault_status >> 8) & 0xff,
				(info.fault_status >> 16) & 0xff,
				(info.fault_status >> 24) & 0xff,
				info.fault_addr & 0xff,
				(info.fault_addr >> 8) & 0xff,
				(info.fault_addr >> 16) & 0xff,
				(info.fault_addr >> 24) & 0xff,
				info.stack_used & 0xff,
				(info.stack_used >> 8) & 0xff,
				info.stack_size & 0xff,
				(info.stack_size >> 8) & 0xff,
			};
			status = util_status_merge(status,
			        util_report_watchdog(stdout, raw, sizeof(raw)));
		}

		/* The radio wait counts go with it: both say why a board stopped
		 * doing what was asked, one for a wedge and one for a radio that
		 * answered the bus and not the request. Read separately, so a
		 * board that cannot answer one still reports the other. */
		{
			radio_timeouts timeouts;
			int t = cmd_get_radio_timeouts(ut->devh, &timeouts);

			if (t < 0)
				status = util_status_merge(status,
				        report_read_failed("radio wait timeouts", t));
			else
				status = util_status_merge(status,
				        util_report_radio_timeouts(stdout, &timeouts));
		}
	}

	/* final actions */
	if(do_identify == 0) {
		/* The blink runs on the board as its own mode, so the command returns
		 * at once; -S or any mode-starting command ends it. */
		int r = identify_check_api(ut);

		if (r < 0) {
			status = util_status_after(status, r);
			goto out;
		}
		/* Stop any operation already running on the device (e.g. -x xmas or
		 * -q LED spectrum analyzer) so it does not fight the identify blink. */
		status = util_status_after(status, cmd_stop(ut->devh));
		r = cmd_identify(ut->devh);
		status = util_status_after(status, r);
		/* One line, not a "flashing the LEDs" line followed by this one
		 * saying the same thing: the blink is reported once, as the command
		 * that ends it, repeating the device selector so it can be pasted. */
		if (r >= 0)
			fprintf(stderr, "note: the board is blinking; stop it with "
			        "ubertooth-util%s%s -S or any mode option\n",
			        ubertooth_device ? " -U " : "",
			        ubertooth_device ? ubertooth_device : "");
		goto out;
	}
	if(do_isp == 0) {
		int r;

		fprintf(stderr, "Entering flash programming (ISP) mode\n");
		r = cmd_set_isp(ut->devh);
		status = util_status_after(status, r);
		/* What the line above does not say: the board hands itself to the
		 * programmer shortly after answering this request -- about a
		 * second on bluetooth_rxtx -- and stays there, answering nothing
		 * else, until its power is cut. This option is one keystroke from
		 * several that only read something, and the board it is given to
		 * by mistake looks broken. */
		if (r >= 0)
			fprintf(stderr, "note: the board hands itself to its on-chip "
			        "programmer and answers nothing else until it is "
			        "power-cycled\n");
		goto out;
	}
	if(do_led_specan >= 0) {
		do_led_specan= do_led_specan ? do_led_specan : UBERTOOTH_LED_SPECAN_N_DEFAULT;
		fprintf(stderr, "Entering LED specan mode (threshold -%d dBm)\n", do_led_specan);
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		status = util_status_after(status, cmd_led_specan(ut->devh, do_led_specan));
		goto out;
	}
	if(do_range_test == 0) {
		int started;

		/* -n is the one thing this tool waits for, so it is the one that
		 * can be interrupted half way. The handler only raises a flag,
		 * which the wait below reads: the board is stopped there, on the
		 * tool's own thread, as every other ending of the wait stops it.
		 * Installed before the test is started, so a signal arriving while
		 * the start request is in flight is caught as well. */
		ubertooth_catch_stop_signals();
		fprintf(stderr, "Starting range test\n");
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		started = cmd_range_test(ut->devh);
		status = util_status_after(status, started);
		/* Only follow a test that started: a request the board refused
		 * leaves nothing running to report on. */
		if (started >= 0)
			status = util_status_merge(status,
			        range_test_wait(ut, range_test_timeout));
		goto out;
	}
	if(do_repeater == 0) {
		fprintf(stderr, "Starting repeater\n");
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		status = util_status_after(status, cmd_repeater(ut->devh));
		goto out;
	}
	if(do_tx == 0) {
		fprintf(stderr, "Starting TX test\n");
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		status = util_status_after(status, cmd_tx_test(ut->devh));
		goto out;
	}
	if(do_set_squelch > 0) {
		/* Silent, like every other setter here (-a, -c, -C, -d, -l): a
		 * "setting squelch" line only repeats what the -z read-back prints
		 * as the result, and a failed set reports itself. */
		status = util_status_after(status, cmd_set_squelch(ut->devh, squelch_level));
	}
	if(do_get_squelch > 0) {
		int squelch = cmd_get_squelch(ut->devh);
		if (squelch < 0)
			status = util_status_merge(status, report_read_failed("squelch", squelch));
		else
			fprintf(stdout, "Squelch set to %d\n", (int8_t)squelch);
	}
	if(do_xmas) {
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		status = util_status_after(status, cmd_xmas(ut->devh));
		goto out;
	}

out:
	/* Release the handle without sending STOP, so a query or a mode just
	 * started is left running on the device. */
	ubertooth_close(ut);
	ubertooth_free(ut);
	return status;
}
