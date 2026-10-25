/*
 * Hardware-independent tests for the asynchronous libubertooth control
 * commands (the follow-mode path: hop, AFH map, clock trim, start hopping).
 *
 * The real ubertooth_control.c is compiled into this test, and the test
 * defines the three libusb transfer calls itself so the library's calls
 * resolve to a fake instead of libusb. The fake keeps the submitted transfer
 * and its buffer, and completes it the way libusb does, so what the library
 * hands over is checkable: the buffer must be heap memory that outlives the
 * submit (it was a stack array), the free flags must be in flags and not in
 * status, and a submit that fails must release both.
 *
 * On the flags: libusb reads transfer->flags and then overwrites
 * transfer->status with the completion status before it runs the callback, so
 * the flags the old code wrote to status were discarded and the callback had
 * to free the transfer itself. That left a failed submit, where the callback
 * never runs, leaking its transfer; and had the flags reached flags,
 * LIBUSB_TRANSFER_FREE_BUFFER would have called free() on a stack address.
 *
 * The fake also completes a transfer with a failure status, which is the only
 * place a refused asynchronous command is ever reported: the submit returns
 * before the board has seen the request.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <btbb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth_control.h"

#define CTRL_OUT (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT)

#define FREE_FLAGS (LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER)

static struct {
	int submit_rc;    /* returned by libusb_submit_transfer() */
	int alloc_fails;  /* make libusb_alloc_transfer() return NULL */
	int allocated;    /* transfers allocated and freed so far */
	int freed;
	int submits;
	/* the transfer last submitted, as it was at the submit */
	struct libusb_transfer* xfer;
	unsigned char* buffer;
	uint8_t flags;
	int status;
	int length;
	libusb_transfer_cb_fn callback;
	unsigned char* freed_buffer; /* buffer the last free released */
} fake;

/* ---- libusb and libbtbb fakes ------------------------------------------ */

/* Defined here, so they take precedence over the libusb library symbols. */
struct libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
	struct libusb_transfer* xfer;

	if (fake.alloc_fails)
		return NULL;
	xfer = calloc(1, sizeof(*xfer)
		+ (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor));
	if (xfer != NULL)
		fake.allocated++;
	return xfer;
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer* xfer)
{
	if (xfer == NULL)
		return;
	/* as libusb does: the buffer goes with the transfer when asked */
	if (xfer->flags & LIBUSB_TRANSFER_FREE_BUFFER) {
		fake.freed_buffer = xfer->buffer;
		free(xfer->buffer);
	} else {
		fake.freed_buffer = NULL;
	}
	fake.freed++;
	free(xfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer* xfer)
{
	fake.submits++;
	fake.xfer = xfer;
	fake.buffer = xfer->buffer;
	fake.flags = xfer->flags;
	fake.status = xfer->status;
	fake.length = xfer->length;
	fake.callback = xfer->callback;
	return fake.submit_rc;
}

/* ubertooth_control.c references libbtbb; no syncword path runs here. */
uint64_t btbb_gen_syncword(const int LAP)
{
	(void)LAP;
	return 0;
}

/* ---- helpers ------------------------------------------------------------ */

static void reset(int submit_rc)
{
	memset(&fake, 0, sizeof(fake));
	fake.submit_rc = submit_rc;
}

/* Complete the transfer libusb was last given, as libusb's event handling
 * does: run the callback, then free the transfer because the library asked
 * for LIBUSB_TRANSFER_FREE_TRANSFER. */
static void complete_last_with(int status)
{
	struct libusb_transfer* xfer = fake.xfer;
	int freed_before = fake.freed;

	mu_assert(xfer != NULL, "a transfer was submitted");
	if (xfer == NULL)
		return;
	xfer->actual_length = status == LIBUSB_TRANSFER_COMPLETED ? xfer->length : 0;
	xfer->status = status;
	fake.callback(xfer);
	/* the callback must leave the transfer to libusb: freeing it there and
	 * asking for LIBUSB_TRANSFER_FREE_TRANSFER is a double free */
	mu_assert_int_eq(freed_before, fake.freed);
	if (xfer->flags & LIBUSB_TRANSFER_FREE_TRANSFER)
		libusb_free_transfer(xfer);
	fake.xfer = NULL;
}

static void complete_last(void)
{
	complete_last_with(LIBUSB_TRANSFER_COMPLETED);
}

/* Overwrite the stack a returned-from callee used, so a buffer left on a dead
 * frame shows up as changed data rather than passing by luck. */
static void clobber_stack(void)
{
	volatile uint8_t scratch[4096];
	size_t i;

	for (i = 0; i < sizeof(scratch); i++)
		scratch[i] = 0xa5;
}

/* The setup packet the library built, as bytes on the wire. */
static void assert_setup(uint8_t request, uint16_t size)
{
	mu_assert_int_eq(CTRL_OUT, fake.buffer[0]);
	mu_assert_int_eq(request, fake.buffer[1]);
	mu_assert_int_eq(0, fake.buffer[2]); /* wValue */
	mu_assert_int_eq(0, fake.buffer[3]);
	mu_assert_int_eq(0, fake.buffer[4]); /* wIndex */
	mu_assert_int_eq(0, fake.buffer[5]);
	mu_assert_int_eq(size & 0xff, fake.buffer[6]); /* wLength */
	mu_assert_int_eq(size >> 8, fake.buffer[7]);
	mu_assert_int_eq(LIBUSB_CONTROL_SETUP_SIZE + size, fake.length);
	mu_assert_int_eq(FREE_FLAGS, fake.flags);
}

static FILE* capture;
static int saved_stderr;

static void capture_stderr(void)
{
	capture = tmpfile();
	saved_stderr = dup(fileno(stderr));
	mu_assert(capture != NULL && saved_stderr >= 0, "capture stderr");
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
}

static void release_stderr(char* text, size_t len)
{
	size_t n;

	fflush(stderr);
	dup2(saved_stderr, fileno(stderr));
	close(saved_stderr);
	rewind(capture);
	n = fread(text, 1, len - 1, capture);
	text[n] = '\0';
	fclose(capture);
}

static int count_lines(const char* text)
{
	int n = 0;

	for (; *text != '\0'; text++)
		if (*text == '\n')
			n++;
	return n;
}

/* ---- tests -------------------------------------------------------------- */

/* The buffer was a variable-length array on ubertooth_cmd_async()'s own
 * stack, which libusb reads after the submit has returned. */
MU_TEST(the_submitted_buffer_outlives_the_call)
{
	uint8_t data[6] = { 0xde, 0xad, 0xbe, 0xef, 0x12, 0x34 };

	reset(0);
	mu_assert_int_eq(0, ubertooth_cmd_async(NULL, CTRL_OUT,
		UBERTOOTH_START_HOPPING, data, sizeof(data)));
	mu_assert_int_eq(1, fake.submits);

	clobber_stack();
	assert_setup(UBERTOOTH_START_HOPPING, sizeof(data));
	mu_assert(memcmp(fake.buffer + LIBUSB_CONTROL_SETUP_SIZE,
		data, sizeof(data)) == 0, "the data survives the call");

	/* and it is memory free() accepts: a stack buffer would fault here */
	complete_last();
	mu_assert_int_eq(1, fake.freed);
}

/* The free flags were written to xfer->status, where libusb overwrites them
 * with the completion status before it reads flags, so they did nothing. */
MU_TEST(the_free_flags_go_in_flags_not_in_status)
{
	reset(0);
	mu_assert_int_eq(0, ubertooth_cmd_async(NULL, CTRL_OUT, UBERTOOTH_HOP, NULL, 0));
	mu_assert_int_eq(FREE_FLAGS, fake.flags);
	mu_assert_int_eq(0, fake.status);

	complete_last();
	mu_assert_int_eq(1, fake.freed);
	mu_assert(fake.freed_buffer != NULL, "the buffer is freed with the transfer");
	mu_assert_int_eq(fake.allocated, fake.freed);
}

MU_TEST(a_failed_submit_frees_the_transfer_and_the_buffer)
{
	char text[256] = "";

	reset(LIBUSB_ERROR_NO_DEVICE);
	capture_stderr();
	mu_assert_int_eq(LIBUSB_ERROR_NO_DEVICE,
		ubertooth_cmd_async(NULL, CTRL_OUT, UBERTOOTH_HOP, NULL, 0));
	release_stderr(text, sizeof(text));

	mu_assert_int_eq(1, fake.submits);
	mu_assert_int_eq(1, fake.freed);
	mu_assert_int_eq(fake.allocated, fake.freed);
	mu_assert(fake.freed_buffer != NULL, "the buffer is freed too");
	mu_assert(strstr(text, "libUSB Error") != NULL, text);
}

MU_TEST(a_message_longer_than_a_control_packet_is_refused)
{
	uint8_t data[UBERTOOTH_CMD_MAX_DATA + 1];
	char text[256] = "";

	memset(data, 0x5a, sizeof(data));
	reset(0);
	capture_stderr();
	mu_assert_int_eq(LIBUSB_ERROR_INVALID_PARAM,
		ubertooth_cmd_async(NULL, CTRL_OUT, UBERTOOTH_HOP, data, sizeof(data)));
	release_stderr(text, sizeof(text));

	mu_assert_int_eq(0, fake.submits);
	mu_assert_int_eq(0, fake.allocated);
	mu_assert(strcmp(text, "error: control message too long: 65 bytes "
		"(maximum 64)\n") == 0, text);

	/* the largest allowed message still goes through */
	mu_assert_int_eq(0, ubertooth_cmd_async(NULL, CTRL_OUT, UBERTOOTH_HOP,
		data, UBERTOOTH_CMD_MAX_DATA));
	assert_setup(UBERTOOTH_HOP, UBERTOOTH_CMD_MAX_DATA);
	complete_last();
	mu_assert_int_eq(fake.allocated, fake.freed);
}

MU_TEST(an_allocation_failure_is_reported_and_submits_nothing)
{
	char text[256] = "";

	reset(0);
	fake.alloc_fails = 1;
	capture_stderr();
	mu_assert_int_eq(LIBUSB_ERROR_NO_MEM,
		ubertooth_cmd_async(NULL, CTRL_OUT, UBERTOOTH_HOP, NULL, 0));
	release_stderr(text, sizeof(text));

	mu_assert_int_eq(0, fake.submits);
	mu_assert(strcmp(text, "error: could not allocate a control "
		"transfer\n") == 0, text);
}

/* Every command that submits asynchronously goes through the one path, so
 * none of them leaks a transfer or leaves one holding a dead buffer. */
MU_TEST(every_async_command_frees_what_it_submits)
{
	uint8_t afh_map[AFH_MAP_BYTES];

	memset(afh_map, 0x3c, sizeof(afh_map));
	reset(0);

	mu_assert_int_eq(0, cmd_hop(NULL));
	assert_setup(UBERTOOTH_HOP, 0);
	complete_last();

	mu_assert_int_eq(0, cmd_cancel_follow(NULL));
	assert_setup(UBERTOOTH_CANCEL_FOLLOW, 0);
	complete_last();

	mu_assert_int_eq(0, cmd_set_afh_map(NULL, afh_map));
	assert_setup(UBERTOOTH_SET_AFHMAP, AFH_MAP_BYTES);
	mu_assert(memcmp(fake.buffer + LIBUSB_CONTROL_SETUP_SIZE,
		afh_map, sizeof(afh_map)) == 0, "the AFH map is copied");
	complete_last();

	cmd_trim_clock(NULL, 0x1234);
	assert_setup(UBERTOOTH_TRIM_CLOCK, 2);
	mu_assert_int_eq(0x12, fake.buffer[LIBUSB_CONTROL_SETUP_SIZE]);
	mu_assert_int_eq(0x34, fake.buffer[LIBUSB_CONTROL_SETUP_SIZE + 1]);
	complete_last();

	cmd_fix_clock_drift(NULL, 0x0506);
	assert_setup(UBERTOOTH_FIX_CLOCK_DRIFT, 2);
	complete_last();

	mu_assert_int_eq(0, cmd_start_hopping(NULL, 0x01020304, 0x0506));
	assert_setup(UBERTOOTH_START_HOPPING, 6);
	mu_assert_int_eq(0x01, fake.buffer[LIBUSB_CONTROL_SETUP_SIZE]);
	mu_assert_int_eq(0x04, fake.buffer[LIBUSB_CONTROL_SETUP_SIZE + 3]);
	mu_assert_int_eq(0x05, fake.buffer[LIBUSB_CONTROL_SETUP_SIZE + 4]);
	mu_assert_int_eq(0x06, fake.buffer[LIBUSB_CONTROL_SETUP_SIZE + 5]);
	complete_last();

	mu_assert_int_eq(6, fake.submits);
	mu_assert_int_eq(6, fake.allocated);
	mu_assert_int_eq(6, fake.freed);
}

/* The commands that return a value pass a submit failure on, and still free
 * what they allocated. */
MU_TEST(the_async_commands_pass_a_submit_failure_back)
{
	uint8_t afh_map[AFH_MAP_BYTES] = { 0 };
	char text[1024] = "";

	reset(LIBUSB_ERROR_NO_DEVICE);
	capture_stderr();
	mu_assert_int_eq(LIBUSB_ERROR_NO_DEVICE, cmd_hop(NULL));
	mu_assert_int_eq(LIBUSB_ERROR_NO_DEVICE, cmd_cancel_follow(NULL));
	mu_assert_int_eq(LIBUSB_ERROR_NO_DEVICE, cmd_set_afh_map(NULL, afh_map));
	mu_assert_int_eq(LIBUSB_ERROR_NO_DEVICE, cmd_start_hopping(NULL, 0, 0));
	release_stderr(text, sizeof(text));

	mu_assert_int_eq(4, fake.submits);
	mu_assert_int_eq(4, fake.allocated);
	mu_assert_int_eq(4, fake.freed);
	/* one line per failure, from the one place that reports it */
	mu_assert_int_eq(4, count_lines(text));
}

/* ---- what a failed completion reports ----------------------------------- */

/* Submit one command and complete it with status, returning what the
 * completion printed in text. */
static void submit_and_fail(uint8_t request, int status, char* text, size_t len)
{
	reset(0);
	mu_assert_int_eq(0, ubertooth_cmd_async(NULL, CTRL_OUT, request, NULL, 0));
	capture_stderr();
	complete_last_with(status);
	release_stderr(text, len);
	mu_assert_int_eq(fake.allocated, fake.freed);
}

/* A stall is how the board refuses a request it does not implement, and it
 * arrives here, at completion, not as a submit failure. The message used to
 * be show_libusb_error(LIBUSB_TRANSFER_STALL), which read the status as a
 * libusb_error code and printed "libUSB Error: Other error:  (4)" --
 * libusb_strerror()'s answer for any positive code, naming neither the
 * command nor what happened. */
MU_TEST(a_stalled_command_is_named_and_reported_as_unsupported)
{
	char text[256] = "";

	submit_and_fail(UBERTOOTH_START_HOPPING, LIBUSB_TRANSFER_STALL,
	                text, sizeof(text));
	mu_assert(strcmp(text, "error: start hopping failed: the board does not "
	                       "support this command (stalled)\n") == 0, text);

	submit_and_fail(UBERTOOTH_HOP, LIBUSB_TRANSFER_STALL, text, sizeof(text));
	mu_assert(strcmp(text, "error: hop failed: the board does not support "
	                       "this command (stalled)\n") == 0, text);

	/* nothing is left of the old, meaningless line */
	mu_assert(strstr(text, "libUSB Error") == NULL, text);
	mu_assert(strstr(text, "Other error") == NULL, text);
}

/* Every command that submits asynchronously names itself. */
MU_TEST(each_async_command_names_itself)
{
	static const struct {
		uint8_t request;
		const char* name;
	} commands[] = {
		{ UBERTOOTH_HOP,                "hop" },
		{ UBERTOOTH_TRIM_CLOCK,         "clock trim" },
		{ UBERTOOTH_FIX_CLOCK_DRIFT,    "clock drift correction" },
		{ UBERTOOTH_CANCEL_FOLLOW,      "cancel follow" },
		{ UBERTOOTH_SET_AFHMAP,         "AFH map" },
		{ UBERTOOTH_START_HOPPING,      "start hopping" },
	};
	char text[256], expected[256];
	size_t i;

	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		submit_and_fail(commands[i].request, LIBUSB_TRANSFER_STALL,
		                text, sizeof(text));
		snprintf(expected, sizeof(expected),
		         "error: %s failed: the board does not support this command "
		         "(stalled)\n", commands[i].name);
		mu_assert(strcmp(text, expected) == 0, text);
	}
}

/* The other completion statuses say what happened, in their own words: a
 * stall is the only one that means "unsupported". */
MU_TEST(each_completion_status_says_what_happened)
{
	static const struct {
		int status;
		const char* why;
	} statuses[] = {
		{ LIBUSB_TRANSFER_ERROR,     "the transfer failed" },
		{ LIBUSB_TRANSFER_TIMED_OUT, "the board did not answer in time" },
		{ LIBUSB_TRANSFER_CANCELLED, "the transfer was cancelled" },
		{ LIBUSB_TRANSFER_NO_DEVICE, "the board is no longer attached" },
		{ LIBUSB_TRANSFER_OVERFLOW,
		  "the board sent more data than the request allows" },
	};
	char text[256], expected[256];
	size_t i;

	for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
		submit_and_fail(UBERTOOTH_HOP, statuses[i].status, text, sizeof(text));
		snprintf(expected, sizeof(expected), "error: hop failed: %s\n",
		         statuses[i].why);
		mu_assert(strcmp(text, expected) == 0, text);
	}
}

/* A status this libusb version has added, and a request that is not one of
 * the asynchronous commands, are still reported as numbers rather than
 * silently. */
MU_TEST(an_unknown_status_or_request_is_reported_as_a_number)
{
	char text[256] = "";

	submit_and_fail(UBERTOOTH_HOP, 99, text, sizeof(text));
	mu_assert(strcmp(text, "error: hop failed: transfer status 99\n") == 0,
	          text);

	submit_and_fail(200, LIBUSB_TRANSFER_STALL, text, sizeof(text));
	mu_assert(strcmp(text, "error: control request 200 failed: the board does "
	                       "not support this command (stalled)\n") == 0, text);
}

/* The completion that libusb reports for a transfer that worked says
 * nothing. */
MU_TEST(a_successful_completion_is_silent)
{
	char text[256] = "";

	submit_and_fail(UBERTOOTH_HOP, LIBUSB_TRANSFER_COMPLETED,
	                text, sizeof(text));
	mu_assert(text[0] == '\0', text);
}

int main(void)
{
	/* A regression that puts the buffer back on the stack can die in free()
	 * part-way through; report the assertions that ran up to then. */
	setbuf(stdout, NULL);

	MU_RUN(the_submitted_buffer_outlives_the_call);
	MU_RUN(the_free_flags_go_in_flags_not_in_status);
	MU_RUN(a_failed_submit_frees_the_transfer_and_the_buffer);
	MU_RUN(a_message_longer_than_a_control_packet_is_refused);
	MU_RUN(an_allocation_failure_is_reported_and_submits_nothing);
	MU_RUN(every_async_command_frees_what_it_submits);
	MU_RUN(the_async_commands_pass_a_submit_failure_back);
	MU_RUN(a_stalled_command_is_named_and_reported_as_unsupported);
	MU_RUN(each_async_command_names_itself);
	MU_RUN(each_completion_status_says_what_happened);
	MU_RUN(an_unknown_status_or_request_is_reported_as_a_number);
	MU_RUN(a_successful_completion_is_silent);
	return mu_report();
}
