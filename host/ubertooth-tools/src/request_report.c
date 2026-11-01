/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The reports for a capture that could not be started, shared by the
 * ubertooth-* host tools that set one up: a control request the board
 * refused, and the host thread that collects the capture.
 */

#include "request_report.h"

#include <stdio.h>
#include <string.h>

#include "ubertooth.h"
#include "ubertooth_control.h"

/* Why the request failed, in plain words. The same wording libubertooth uses
 * for the matching status of an asynchronous transfer, so the two reports read
 * alike. NULL for a code with no plain words, which the line then carries as a
 * number alone. */
static const char *reason(int code)
{
	switch (code) {
	case LIBUSB_ERROR_TIMEOUT:
		return "the board did not answer in time";
	/* No LIBUSB_ERROR_PIPE case: a stall is the board saying it has no such
	 * request, which report_refused_request() hands to the library's shared
	 * wording so that the firmware API versions come with it. */
	case LIBUSB_ERROR_NO_DEVICE:
		return "the board is no longer attached";
	case LIBUSB_ERROR_OVERFLOW:
		return "the board sent more data than the request allows";
	case LIBUSB_ERROR_IO:
		return "the transfer failed";
	default:
		return NULL;
	}
}

/* What to try next, for the failures the user can do something about. NULL
 * where there is nothing honest to suggest. */
static const char *hint(int code)
{
	switch (code) {
	case LIBUSB_ERROR_TIMEOUT:
		/* A board busy in a mode takes no new request until the mode
		 * stops. One that answers nothing at all usually resets itself,
		 * but not always: a mode that services USB from the interrupt
		 * keeps its loop turning, and so keeps feeding the watchdog,
		 * even when its USB path alone has died. That board waits for a
		 * reset that is not coming, so the note names the fallback. */
		return "if a mode is running, stop it with ubertooth-util -S; "
		       "a board that does not answer at all usually resets "
		       "itself within a few seconds, so run the command again, "
		       "and unplug it or power-cycle its hub port if it stays "
		       "silent";
	case LIBUSB_ERROR_NO_DEVICE:
		return "re-attach the board and run the command again";
	default:
		return NULL;
	}
}

void report_refused_request(const char *what, int code)
{
	const char *why = reason(code);
	const char *advice = hint(code);

	/* libubertooth has already printed the whole line for a transfer the
	 * board truncated, naming the request and the counts. */
	if (code == UBERTOOTH_ERROR_SHORT_REPLY)
		return;

	/* A request the board's firmware has not got is reported in one wording
	 * wherever it was sent from, with the board's firmware API version
	 * beside the one these tools need. */
	if (code == LIBUSB_ERROR_PIPE) {
		ubertooth_report_unsupported_request(what, code);
		return;
	}

	if (why != NULL)
		fprintf(stderr, "error: could not %s: %s (libusb %d)\n",
		        what, why, code);
	else
		fprintf(stderr, "error: could not %s (libusb %d)\n", what, code);

	if (advice != NULL)
		fprintf(stderr, "note: %s\n", advice);
}

void report_capture_thread_failure(int error)
{
	fprintf(stderr, "error: could not start the thread that collects the "
	        "capture: %s\n", strerror(error));
}
