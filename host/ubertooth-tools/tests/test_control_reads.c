/*
 * Hardware-independent tests for the fixed-size libubertooth read commands,
 * and for which of its set commands report a refused request themselves.
 *
 * The real ubertooth_control.c is compiled into this test, and the test
 * defines libusb_control_transfer() itself so the library's calls resolve to a
 * scripted reply instead of libusb. Each read must return its value only for
 * a full-length reply: a short or empty reply (seen with stalled control
 * transfers in follow mode) is an error, never a value made from whatever the
 * result buffer held.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <btbb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth_control.h"

/* Room for the longest fixed-size reply read here, the bootloader report's
 * 52 bytes, with room above it so a reply one byte too long can be scripted
 * too. */
#define REPLY_MAX 64

/* How many calls one scripted sequence can answer differently. The poll makes
 * at most three attempts, which is the longest run of calls any read here
 * makes. */
#define SEQUENCE_MAX 3

static struct {
	int rc;                   /* returned by libusb_control_transfer() */
	uint8_t reply[REPLY_MAX]; /* copied into the caller's buffer, rc bytes */
	uint8_t last_request;
	uint16_t last_length;
	/* A reply for each of the first calls, in order, for the reads that
	 * try again after a failure. Empty when `rc` answers every call. The
	 * last entry answers every call past the end of the sequence, so a read
	 * that tried a fourth time would be seen rather than answered by
	 * whatever was left. */
	int sequence[SEQUENCE_MAX];
	int sequence_len;
	int calls;                /* how many calls the read made */
	/* The user's stop arrives once this many calls have been made; 0 for a
	 * run nobody stopped. */
	int stop_after;
} fake;

/* ---- libusb and libbtbb fakes ------------------------------------------ */

/* The report about a refused request asks whether the board has left the bus
 * before it blames the board's firmware. Nothing here unplugs a board, so it
 * is always still there. */
int ubertooth_device_gone(const void* board)
{
	(void)board;
	return 0;
}

/* Defined here, so it takes precedence over the libusb library symbol. */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *dev_handle,
	uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
	unsigned char *data, uint16_t wLength, unsigned int timeout)
{
	(void)dev_handle;
	(void)request_type;
	(void)wValue;
	(void)wIndex;
	(void)timeout;
	fake.last_request = bRequest;
	fake.last_length = wLength;
	if (fake.sequence_len > 0) {
		int i = fake.calls < fake.sequence_len ? fake.calls
		                                       : fake.sequence_len - 1;
		fake.rc = fake.sequence[i];
	}
	fake.calls++;
	/* never copy more than the caller's buffer or the scripted reply */
	if (fake.rc > 0) {
		size_t n = (size_t)fake.rc;
		if (n > wLength)
			n = wLength;
		if (n > sizeof(fake.reply))
			n = sizeof(fake.reply);
		memcpy(data, fake.reply, n);
	}
	return fake.rc;
}

/* The flag a stop signal would have set. The library's own copy lives in
 * ubertooth.c, which this test does not compile, so the cases script when the
 * stop arrives instead of raising a signal: the poll's retry is the only thing
 * that reads it, and a signal would race with the attempt it is meant to
 * land in. */
int ubertooth_stop_requested(void)
{
	return fake.stop_after > 0 && fake.calls >= fake.stop_after;
}

/* ubertooth_control.c references libbtbb; no syncword path runs here. */
uint64_t btbb_gen_syncword(const int LAP)
{
	(void)LAP;
	return 0;
}

/* ---- helpers ------------------------------------------------------------ */

static void script(int rc, uint8_t fill)
{
	memset(&fake, 0, sizeof(fake));
	fake.rc = rc;
	memset(fake.reply, fill, sizeof(fake.reply));
}

/* The same, for a read that may call more than once: one reply per attempt,
 * the third answering any attempt after the third. */
static void script_attempts(int first, int second, int third)
{
	script(first, 0);
	fake.sequence[0] = first;
	fake.sequence[1] = second;
	fake.sequence[2] = third;
	fake.sequence_len = SEQUENCE_MAX;
}

/* The one-byte reads, each returning the byte as a non-negative int. */
typedef int (*byte_read)(struct libusb_device_handle *);
static const struct {
	const char *name;
	byte_read read;
	uint8_t request;
} byte_reads[] = {
	{ "usrled", cmd_get_usrled, UBERTOOTH_GET_USRLED },
	{ "rxled", cmd_get_rxled, UBERTOOTH_GET_RXLED },
	{ "txled", cmd_get_txled, UBERTOOTH_GET_TXLED },
	{ "modulation", cmd_get_modulation, UBERTOOTH_GET_MOD },
	{ "palevel", cmd_get_palevel, UBERTOOTH_GET_PALEVEL },
	{ "board_id", cmd_get_board_id, UBERTOOTH_GET_BOARD_ID },
	{ "squelch", cmd_get_squelch, UBERTOOTH_GET_SQUELCH },
	{ "crc_verify", cmd_get_crc_verify, UBERTOOTH_GET_CRC_VERIFY },
};
#define BYTE_READS (sizeof(byte_reads) / sizeof(byte_reads[0]))

/* ---- tests -------------------------------------------------------------- */

MU_TEST(one_byte_reads_return_the_byte_for_a_full_reply)
{
	size_t i;
	for (i = 0; i < BYTE_READS; i++) {
		script(1, 0x05);
		mu_assert_int_eq(5, byte_reads[i].read(NULL));
		mu_assert_int_eq(byte_reads[i].request, fake.last_request);
		mu_assert_int_eq(1, fake.last_length);
	}
}

MU_TEST(one_byte_reads_reject_an_empty_reply)
{
	size_t i;
	for (i = 0; i < BYTE_READS; i++) {
		script(0, 0x05);
		mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, byte_reads[i].read(NULL));
	}
}

MU_TEST(one_byte_reads_pass_libusb_errors_through)
{
	size_t i;
	for (i = 0; i < BYTE_READS; i++) {
		script(LIBUSB_ERROR_PIPE, 0);
		mu_assert_int_eq(LIBUSB_ERROR_PIPE, byte_reads[i].read(NULL));
		script(LIBUSB_ERROR_TIMEOUT, 0);
		mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, byte_reads[i].read(NULL));
	}
}

MU_TEST(channel_needs_both_bytes)
{
	script(2, 0);
	fake.reply[0] = 0x89; /* 2441 = 0x0989 */
	fake.reply[1] = 0x09;
	mu_assert_int_eq(2441, cmd_get_channel(NULL));
	mu_assert_int_eq(UBERTOOTH_GET_CHANNEL, fake.last_request);

	/* one or no byte back: an error, not a channel built from stale bytes
	 * (the "channel 0" seen right after START_HOPPING) */
	script(1, 0x89);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_get_channel(NULL));
	script(0, 0);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_get_channel(NULL));
	script(LIBUSB_ERROR_PIPE, 0);
	mu_assert_int_eq(LIBUSB_ERROR_PIPE, cmd_get_channel(NULL));
}

MU_TEST(serial_needs_all_17_bytes)
{
	uint8_t serial[17];

	script(17, 0xab);
	fake.reply[0] = 0; /* status: success */
	mu_assert_int_eq(0, cmd_get_serial(NULL, serial));
	mu_assert_int_eq(0xab, serial[16]);

	script(16, 0xab);
	fake.reply[0] = 0;
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_get_serial(NULL, serial));
	script(0, 0);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_get_serial(NULL, serial));
}

MU_TEST(four_and_two_byte_reads_need_their_full_length)
{
	script(4, 0);
	fake.reply[0] = 0x78; fake.reply[1] = 0x56; fake.reply[2] = 0x34; fake.reply[3] = 0x12;
	mu_assert(cmd_get_clock(NULL) == 0x12345678u, "clock");
	mu_assert(cmd_get_access_address(NULL) == 0x12345678u, "access address");
	script(3, 0x11);
	mu_assert(cmd_get_clock(NULL) == (uint32_t)UBERTOOTH_ERROR_SHORT_REPLY, "short clock");
	mu_assert(cmd_get_access_address(NULL) == (uint32_t)UBERTOOTH_ERROR_SHORT_REPLY, "short access address");

	script(2, 0);
	fake.reply[0] = 0x12; fake.reply[1] = 0x34;
	mu_assert_int_eq(0x1234, cmd_read_register(NULL, 0x05));
	script(1, 0x12);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_read_register(NULL, 0x05));

	/* an empty reply, as seen in follow mode */
	script(0, 0);
	mu_assert(cmd_get_clock(NULL) == (uint32_t)UBERTOOTH_ERROR_SHORT_REPLY, "empty clock");
	mu_assert(cmd_get_access_address(NULL) == (uint32_t)UBERTOOTH_ERROR_SHORT_REPLY, "empty access address");
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_read_register(NULL, 0x05));
}

/* cmd_read_access_address() reports an error separately from the address,
 * which the u32-returning cmd_get_access_address() cannot. */
MU_TEST(read_access_address_returns_errors_separately)
{
	uint32_t aa = 0xdeadbeef;

	script(4, 0);
	fake.reply[0] = 0xd6; fake.reply[1] = 0xbe; fake.reply[2] = 0x89; fake.reply[3] = 0x8e;
	mu_assert_int_eq(0, cmd_read_access_address(NULL, &aa));
	mu_assert(aa == 0x8e89bed6u, "advertising access address");
	mu_assert_int_eq(UBERTOOTH_GET_ACCESS_ADDRESS, fake.last_request);

	aa = 0xdeadbeef;
	script(3, 0x11);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_read_access_address(NULL, &aa));
	mu_assert(aa == 0xdeadbeefu, "left untouched on a short reply");
	script(0, 0);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_read_access_address(NULL, &aa));
	mu_assert(aa == 0xdeadbeefu, "left untouched on an empty reply");

	script(LIBUSB_ERROR_PIPE, 0);
	mu_assert_int_eq(LIBUSB_ERROR_PIPE, cmd_read_access_address(NULL, &aa));
	mu_assert(aa == 0xdeadbeefu, "left untouched on an error");
}

/* Captures standard error over one call and returns what it holds. */
static void run_captured(void (*call)(void), char *text, size_t size)
{
	FILE *capture = tmpfile();
	int saved = dup(fileno(stderr));
	size_t n;

	text[0] = '\0';
	mu_assert_fatal(capture != NULL && saved >= 0, "capture stderr");
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
	call();
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	rewind(capture);
	n = fread(text, 1, size - 1, capture);
	text[n] = '\0';
	fclose(capture);
}

/* Two short replies in a row, so one capture holds both lines. */
static void two_short_reads(void)
{
	script(0, 0);
	cmd_read_access_address(NULL, &(uint32_t){0});
	script(1, 0x89);
	cmd_get_channel(NULL);
}

/* The library prints the one complete line for a short reply. */
MU_TEST(short_reply_message_names_the_read_and_the_counts)
{
	char text[256];

	run_captured(two_short_reads, text, sizeof(text));
	mu_assert(strcmp(text,
	    "error: access address read failed: board returned 0 of 4 bytes\n"
	    "error: channel read failed: board returned 1 of 2 bytes\n") == 0, text);
}

static void poll_range_result(void)
{
	rangetest_result rr;

	cmd_get_rangeresult_quiet(NULL, &rr);
}

static void poll_watchdog(void)
{
	watchdog_info info;

	cmd_get_watchdog_quiet(NULL, &info);
}

static void set_channel_quiet(void) { cmd_set_channel_quiet(NULL, 2441); }
static void set_channel(void) { cmd_set_channel(NULL, 2441); }
static void set_bdaddr_quiet(void) { cmd_set_bdaddr_quiet(NULL, 0x112233); }
static void set_clock_quiet(void) { cmd_set_clock_quiet(NULL, 0); }
static void set_clock(void) { cmd_set_clock(NULL, 0); }

/* A tool that names the request itself asks for the quiet form, so the whole
 * failure is the one line the tool prints. The reporting form keeps its own
 * line for the callers that do not check the result at all. */
MU_TEST(the_quiet_set_forms_report_nothing)
{
	char text[256];

	script(LIBUSB_ERROR_TIMEOUT, 0);
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, cmd_set_channel_quiet(NULL, 2441));
	run_captured(set_channel_quiet, text, sizeof(text));
	mu_assert(text[0] == '\0', text);

	script(LIBUSB_ERROR_PIPE, 0);
	run_captured(set_channel_quiet, text, sizeof(text));
	mu_assert(text[0] == '\0', text);

	script(LIBUSB_ERROR_TIMEOUT, 0);
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, cmd_set_bdaddr_quiet(NULL, 0x112233));
	run_captured(set_bdaddr_quiet, text, sizeof(text));
	mu_assert(text[0] == '\0', text);

	/* A transfer the board truncated is a complete line of its own, so the
	 * quiet form keeps it, and the short-reply code says so. */
	script(8, 0);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_set_bdaddr_quiet(NULL, 0x112233));
	run_captured(set_bdaddr_quiet, text, sizeof(text));
	mu_assert(strcmp(text, "error: Bluetooth address write failed: "
	    "board took 8 of 16 bytes\n") == 0, text);

	script(LIBUSB_ERROR_TIMEOUT, 0);
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, cmd_set_clock_quiet(NULL, 0));
	run_captured(set_clock_quiet, text, sizeof(text));
	mu_assert(text[0] == '\0', text);

	script(2, 0);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_set_clock_quiet(NULL, 0));
	run_captured(set_clock_quiet, text, sizeof(text));
	mu_assert(strcmp(text, "error: clock write failed: "
	    "board took 2 of 4 bytes\n") == 0, text);

	/* The reporting form still prints its own line for each. */
	script(LIBUSB_ERROR_PIPE, 0);
	run_captured(set_channel, text, sizeof(text));
	mu_assert(strcmp(text, "error: could not set the channel: the board's "
	    "firmware does not support it (stalled, libusb -9)\n") == 0, text);

	script(LIBUSB_ERROR_TIMEOUT, 0);
	run_captured(set_channel, text, sizeof(text));
	mu_assert(strcmp(text,
	    "error: the Ubertooth did not answer in time (libusb -7)\n") == 0,
	    text);

	/* A truncated write is the one case the reporting form must stay quiet
	 * about: the quiet form has already printed the whole line, so a second
	 * generic one would contradict it. */
	script(2, 0);
	run_captured(set_clock, text, sizeof(text));
	mu_assert(strcmp(text, "error: clock write failed: "
	    "board took 2 of 4 bytes\n") == 0, text);

	/* A request the board took says nothing either way. */
	script(0, 0);
	mu_assert_int_eq(0, cmd_set_channel_quiet(NULL, 2441));
	run_captured(set_channel, text, sizeof(text));
	mu_assert(text[0] == '\0', text);
	script(4, 0);
	mu_assert_int_eq(0, cmd_set_clock_quiet(NULL, 0));
	run_captured(set_clock, text, sizeof(text));
	mu_assert(text[0] == '\0', text);
}

MU_TEST(range_result_needs_all_6_bytes)
{
	rangetest_result rr;

	/* A distinct value per field, so a read that took them from the wrong
	 * offsets does not pass. */
	script(6, 0);
	fake.reply[0] = 1;
	fake.reply[1] = 7;
	fake.reply[2] = 9;
	fake.reply[3] = 4;
	fake.reply[4] = 3;
	fake.reply[5] = 11;
	mu_assert_int_eq(0, cmd_get_rangeresult(NULL, &rr));
	mu_assert_int_eq(1, rr.valid);
	mu_assert_int_eq(7, rr.request_pa);
	mu_assert_int_eq(9, rr.request_num);
	mu_assert_int_eq(4, rr.reply_pa);
	mu_assert_int_eq(3, rr.reply_num);
	mu_assert_int_eq(11, rr.discarded);

	memset(&rr, 0x5a, sizeof(rr));
	script(5, 0x11);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_get_rangeresult(NULL, &rr));
	mu_assert_int_eq(0x5a, rr.valid); /* left untouched */
}

/*
 * The quiet forms of the two reads ubertooth-util -n polls with: the same
 * answer, and not a word about a read that failed, since there the caller
 * reads again rather than reporting.
 */
MU_TEST(the_polled_reads_report_nothing)
{
	rangetest_result rr;
	watchdog_info info;
	char text[512];

	script(6, 0);
	fake.reply[0] = 1;
	fake.reply[2] = 9;
	mu_assert_int_eq(0, cmd_get_rangeresult_quiet(NULL, &rr));
	mu_assert_int_eq(1, rr.valid);
	mu_assert_int_eq(9, rr.request_num);

	script(UBERTOOTH_WATCHDOG_INFO_LEN, 0);
	fake.reply[6] = MODE_RANGE_TEST;
	mu_assert_int_eq(0, cmd_get_watchdog_quiet(NULL, &info));
	mu_assert_int_eq(MODE_RANGE_TEST, info.mode);

	/* a stalled request, a timed-out one and a short reply: all silent */
	script(LIBUSB_ERROR_PIPE, 0);
	run_captured(poll_range_result, text, sizeof(text));
	mu_assert(text[0] == '\0', text);
	script(LIBUSB_ERROR_PIPE, 0);
	run_captured(poll_watchdog, text, sizeof(text));
	mu_assert(text[0] == '\0', text);
	script(LIBUSB_ERROR_TIMEOUT, 0);
	run_captured(poll_range_result, text, sizeof(text));
	mu_assert(text[0] == '\0', text);
	script(4, 0);
	run_captured(poll_range_result, text, sizeof(text));
	mu_assert(text[0] == '\0', text);
	script(UBERTOOTH_WATCHDOG_INFO_LEN - 1, 0);
	run_captured(poll_watchdog, text, sizeof(text));
	mu_assert(text[0] == '\0', text);

	/* and the error is returned, so the caller can count it */
	script(LIBUSB_ERROR_PIPE, 0);
	mu_assert_int_eq(LIBUSB_ERROR_PIPE, cmd_get_rangeresult_quiet(NULL, &rr));
	script(4, 0);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_rangeresult_quiet(NULL, &rr));
	script(LIBUSB_ERROR_PIPE, 0);
	mu_assert_int_eq(LIBUSB_ERROR_PIPE, cmd_get_watchdog_quiet(NULL, &info));
	script(UBERTOOTH_WATCHDOG_INFO_LEN - 1, 0);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_watchdog_quiet(NULL, &info));
}

/*
 * The watchdog state: every byte of the reply, read in the order the interface
 * gives, and a short reply leaves the caller's struct alone rather than
 * filling it from whatever the buffer held.
 */
MU_TEST(watchdog_state_needs_the_whole_reply)
{
	watchdog_info info;

	script(UBERTOOTH_WATCHDOG_INFO_LEN, 0);
	fake.reply[0] = WATCHDOG_RESET_WATCHDOG;
	fake.reply[1] = 1;
	fake.reply[2] = 3;
	fake.reply[3] = 4;
	fake.reply[4] = MODE_BT_FOLLOW_LE;
	fake.reply[5] = WATCHDOG_SITE_XOSC;
	fake.reply[6] = MODE_IDLE;
	fake.reply[7] = WATCHDOG_SITE_NONE;
	fake.reply[8] = 300 & 0xff;
	fake.reply[9] = 300 >> 8;
	fake.reply[10] = 0x82;
	fake.reply[14] = 0x08;
	fake.reply[15] = 0xc0;
	fake.reply[17] = 0x40;
	fake.reply[18] = 432 & 0xff;
	fake.reply[19] = 432 >> 8;
	fake.reply[20] = 11128 & 0xff;
	fake.reply[21] = 11128 >> 8;
	mu_assert_int_eq(0, cmd_get_watchdog(NULL, &info));
	mu_assert_int_eq(UBERTOOTH_GET_WATCHDOG, fake.last_request);
	mu_assert_int_eq(WATCHDOG_RESET_WATCHDOG, info.reset_cause);
	mu_assert_int_eq(1, info.armed);
	mu_assert_int_eq(3, info.resets);
	mu_assert_int_eq(4, info.boots);
	mu_assert_int_eq(MODE_BT_FOLLOW_LE, info.reset_mode);
	mu_assert_int_eq(WATCHDOG_SITE_XOSC, info.reset_site);
	mu_assert_int_eq(MODE_IDLE, info.mode);
	mu_assert_int_eq(WATCHDOG_SITE_NONE, info.site);
	/* The wider fields, low byte first: a transfer-error count of 300, and
	 * a memory management fault at 0x4000c008 whose status says the address
	 * register holds it. */
	mu_assert_int_eq(300, info.transfer_errors);
	mu_assert_int_eq(0x82, info.fault_status);
	mu_assert(info.fault_status & UBERTOOTH_FAULT_ADDR_VALID,
	          "the status does not say the address was recorded");
	mu_assert_int_eq(0x4000c008, info.fault_addr);
	/* How deep the stack has ever been, out of the room it has. */
	mu_assert_int_eq(432, info.stack_used);
	mu_assert_int_eq(11128, info.stack_size);

	memset(&info, 0x5a, sizeof(info));
	script(UBERTOOTH_WATCHDOG_INFO_LEN - 1, 0x11);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_watchdog(NULL, &info));
	mu_assert_int_eq(0x5a, info.reset_cause); /* left untouched */
}

/*
 * The radio wait give-up counts: one 16-bit count per shape, low byte first, in the
 * order the interface gives. A short reply leaves the caller's struct alone,
 * as the watchdog read does.
 */
MU_TEST(radio_timeouts_are_read_little_endian)
{
	radio_timeouts t;

	script(UBERTOOTH_RADIO_TIMEOUTS_LEN, 0);
	fake.reply[0] = 0x01; fake.reply[1] = 0x00; /* fs_lock   = 1 */
	fake.reply[2] = 0x34; fake.reply[3] = 0x12; /* fs_unlock = 0x1234 */
	fake.reply[4] = 0xff; fake.reply[5] = 0xff; /* xosc      = 65535 */
	fake.reply[6] = 0x00; fake.reply[7] = 0x01; /* fsm       = 256 */
	fake.reply[8] = 0x07; fake.reply[9] = 0x00; /* rx_bytes  = 7 */
	mu_assert_int_eq(0, cmd_get_radio_timeouts(NULL, &t));
	mu_assert_int_eq(UBERTOOTH_GET_RADIO_TIMEOUTS, fake.last_request);
	mu_assert_int_eq(1, t.fs_lock);
	mu_assert_int_eq(0x1234, t.fs_unlock);
	mu_assert_int_eq(65535, t.xosc);
	mu_assert_int_eq(256, t.fsm);
	mu_assert_int_eq(7, t.rx_bytes);

	memset(&t, 0x5a, sizeof(t));
	script(UBERTOOTH_RADIO_TIMEOUTS_LEN - 1, 0x11);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_radio_timeouts(NULL, &t));
	mu_assert_int_eq(0x5a5a, t.fs_lock); /* left untouched */
}

/*
 * The bootloader report: the hash of the region first, low byte first, then
 * the version block. A region with no block is a success -- that is what every
 * board as shipped answers -- with the reason in the struct, so the caller can
 * print the hash it did get.
 */
MU_TEST(the_bootloader_report_reads_the_hash_and_the_block)
{
	struct ubertooth_bootloader_info info;
	uint8_t *block;

	script(UBERTOOTH_BL_REPORT_LENGTH, 0);
	fake.reply[0] = 0xb4; fake.reply[1] = 0xe1;
	fake.reply[2] = 0x2f; fake.reply[3] = 0x9d;
	block = fake.reply + UBERTOOTH_BL_REPORT_OFF_BLOCK;
	memcpy(block + UBERTOOTH_BL_VERSION_OFF_MAGIC, UBERTOOTH_BL_VERSION_MAGIC,
	       UBERTOOTH_BL_VERSION_MAGIC_LEN);
	block[UBERTOOTH_BL_VERSION_OFF_FORMAT] = UBERTOOTH_BL_VERSION_FORMAT;
	block[UBERTOOTH_BL_VERSION_OFF_VERSION] = 0x00;
	block[UBERTOOTH_BL_VERSION_OFF_VERSION + 1] = 0x02; /* 2.00 */
	memcpy(block + UBERTOOTH_BL_VERSION_OFF_BUILD, "git-1a2b3c4", 11);
	mu_assert_int_eq(0, cmd_get_bootloader_info(NULL, &info));
	mu_assert_int_eq(UBERTOOTH_GET_BOOTLOADER_INFO, fake.last_request);
	mu_assert_int_eq(0x9d2fe1b4, info.crc32);
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_OK, info.version_result);
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_FORMAT, info.version.format_version);
	mu_assert_int_eq(0x0200, info.version.version);
	mu_assert(strcmp(info.version.build, "git-1a2b3c4") == 0,
	          info.version.build);

	/* Zeros where the block would be: the firmware sends those for a
	 * bootloader that has none. The hash is still read. */
	script(UBERTOOTH_BL_REPORT_LENGTH, 0);
	fake.reply[0] = 0xc1; fake.reply[1] = 0x73;
	fake.reply[2] = 0x0a; fake.reply[3] = 0x5f;
	mu_assert_int_eq(0, cmd_get_bootloader_info(NULL, &info));
	mu_assert_int_eq(0x5f0a73c1, info.crc32);
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_NO_MAGIC, info.version_result);
	mu_assert(info.version.build[0] == '\0',
	          "a block that was not read left a build string behind");

	/* A reply one byte short: not even the hash can be trusted, so the read
	 * fails and the struct is left zeroed rather than holding four bytes of
	 * a reply that did not arrive whole. The firmware info read clears its
	 * caller's struct the same way. */
	memset(&info, 0x5a, sizeof(info));
	script(UBERTOOTH_BL_REPORT_LENGTH - 1, 0x11);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_bootloader_info(NULL, &info));
	mu_assert_int_eq(0, info.crc32);
}

/* ---- the library writes nothing to standard output --------------------- */

/*
 * libubertooth returns its reads to the caller and reports its problems on
 * standard error; standard output belongs to the tool, which may be writing
 * capture data to it. cmd_get_clock() used to print "Read clock = 0x..."
 * there as well as returning the value.
 */
static FILE* out_capture;
static FILE* err_capture;
static int saved_stdout = -1;
static int saved_stderr = -1;

/* Both streams, so the short-reply half of the test does not spray its
 * diagnostics over the suite's own output, and so the test can show they
 * went to standard error rather than nowhere. Returns 0 if either stream
 * could not be captured, which leaves both of them alone. */
static int capture_both(void)
{
	out_capture = tmpfile();
	err_capture = tmpfile();
	saved_stdout = out_capture != NULL ? dup(fileno(stdout)) : -1;
	saved_stderr = err_capture != NULL ? dup(fileno(stderr)) : -1;
	if (saved_stdout < 0 || saved_stderr < 0) {
		if (saved_stdout >= 0)
			close(saved_stdout);
		if (saved_stderr >= 0)
			close(saved_stderr);
		if (out_capture != NULL)
			fclose(out_capture);
		if (err_capture != NULL)
			fclose(err_capture);
		saved_stdout = saved_stderr = -1;
		return 0;
	}
	fflush(stdout);
	fflush(stderr);
	dup2(fileno(out_capture), fileno(stdout));
	dup2(fileno(err_capture), fileno(stderr));
	return 1;
}

static void read_back(FILE* capture, char* text, size_t len)
{
	size_t n;

	rewind(capture);
	n = fread(text, 1, len - 1, capture);
	text[n] = '\0';
	fclose(capture);
}

static void release_both(char* out, size_t out_len, char* err, size_t err_len)
{
	fflush(stdout);
	fflush(stderr);
	dup2(saved_stdout, fileno(stdout));
	dup2(saved_stderr, fileno(stderr));
	close(saved_stdout);
	close(saved_stderr);
	saved_stdout = saved_stderr = -1;
	read_back(out_capture, out, out_len);
	read_back(err_capture, err, err_len);
}

MU_TEST(no_read_writes_to_standard_output)
{
	uint8_t serial[17];
	rangetest_result rr;
	char out[512], err[1024];

	if (!capture_both()) {
		mu_assert(0, "could not capture both streams");
		return;
	}
	script(4, 0);
	fake.reply[0] = 0x78; fake.reply[1] = 0x56;
	fake.reply[2] = 0x34; fake.reply[3] = 0x12;
	(void)cmd_get_clock(NULL);
	(void)cmd_get_access_address(NULL);
	script(2, 0);
	(void)cmd_get_channel(NULL);
	(void)cmd_read_register(NULL, 0x05);
	script(1, 0);
	(void)cmd_get_usrled(NULL);
	(void)cmd_get_palevel(NULL);
	(void)cmd_get_squelch(NULL);
	script(17, 0);
	(void)cmd_get_serial(NULL, serial);
	script(6, 0);
	(void)cmd_get_rangeresult(NULL, &rr);
	/* and the same reads when the reply is too short to use */
	script(0, 0);
	(void)cmd_get_clock(NULL);
	(void)cmd_get_channel(NULL);
	(void)cmd_get_serial(NULL, serial);
	release_both(out, sizeof(out), err, sizeof(err));

	mu_assert(out[0] == '\0', out);
	/* the short replies did report themselves, on standard error */
	mu_assert(strstr(err, "error: clock read failed") != NULL, err);
	mu_assert(strstr(err, "error: channel read failed") != NULL, err);
	mu_assert(strstr(err, "error: serial read failed") != NULL, err);
}

/* A read that times out is a board that did not answer. Say that, in one
 * line, instead of libusb's own "Operation timed out", and keep the
 * code for bug reports. What a silent board means depends on the caller, so
 * this does not guess; the paths that know add it. On standard error, with
 * nothing on standard output. (The other code with wording of its own,
 * LIBUSB_ERROR_BUSY, is not something a control transfer returns; it is
 * pinned on the open path, in test_device_select.c.) */
MU_TEST(a_board_that_does_not_answer_is_reported_in_words)
{
	char out[512], err[1024];

	if (!capture_both()) {
		mu_assert(0, "could not capture both streams");
		return;
	}
	script(LIBUSB_ERROR_TIMEOUT, 0);
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, cmd_get_squelch(NULL));
	release_both(out, sizeof(out), err, sizeof(err));

	mu_assert(out[0] == '\0', out);
	mu_assert(strcmp(err,
	    "error: the Ubertooth did not answer in time (libusb -7)\n") == 0,
	    err);
}

/* Every other code still goes through the libusb naming, so the plain wording
 * above is not put on causes it does not fit. */
MU_TEST(other_libusb_errors_keep_their_own_report)
{
	char out[512], err[1024];

	if (!capture_both()) {
		mu_assert(0, "could not capture both streams");
		return;
	}
	script(LIBUSB_ERROR_IO, 0);
	mu_assert_int_eq(LIBUSB_ERROR_IO, cmd_get_squelch(NULL));
	release_both(out, sizeof(out), err, sizeof(err));

	mu_assert(out[0] == '\0', out);
	/* libusb supplies the words, and they are localizable, so only this
	 * project's half of the line is pinned: the prefix and the code. The
	 * plain wordings are this project's own text, so a code they do not fit
	 * getting one is still checked for. */
	mu_assert(strncmp(err, "error: ", 7) == 0, err);
	mu_assert(strstr(err, "(libusb -1)\n") != NULL, err);
	mu_assert(strstr(err, "in use by another program") == NULL, err);
	mu_assert(strstr(err, "did not answer in time") == NULL, err);
}

/* A stall is the board saying it has no such request, and every read says so
 * in the one shared wording -- including the ones that used to hand a stall
 * straight to the libusb naming, where it read as "Pipe error" and named
 * neither the request nor the firmware. */
MU_TEST(a_stalled_read_is_reported_as_unsupported)
{
	char out[512], err[1024];

	if (!capture_both()) {
		mu_assert(0, "could not capture both streams");
		return;
	}
	script(LIBUSB_ERROR_PIPE, 0);
	mu_assert_int_eq(LIBUSB_ERROR_PIPE, cmd_get_squelch(NULL));
	release_both(out, sizeof(out), err, sizeof(err));

	mu_assert(out[0] == '\0', out);
	mu_assert(strcmp(err, "error: could not read the squelch level: the "
	    "board's firmware does not support it (stalled, libusb -9)\n") == 0,
	    err);
}

/* ---- the poll a capture runs on ---------------------------------------- */

/* The last poll's result, so a capture can be run inside a stderr capture. */
static int poll_rc;

static void poll_once(void)
{
	usb_pkt_rx p;

	memset(&p, 0, sizeof(p));
	poll_rc = cmd_poll(NULL, &p);
}

/*
 * A capture polls once per packet and ends on the first failure, so the two
 * failures the next poll comes back from are tried again here rather than
 * cutting a capture short: a stalled transfer, which was already retried, and
 * one that timed out because the board was busy elsewhere when the request
 * arrived. One unlucky poll used to end a capture that had been running for a
 * quarter of an hour.
 */
MU_TEST(a_poll_that_timed_out_or_stalled_is_tried_again)
{
	char text[256];

	/* Two timeouts, then the packet: the packet is what the caller gets,
	 * and nothing is reported, because nothing was lost. */
	script_attempts(LIBUSB_ERROR_TIMEOUT, LIBUSB_ERROR_TIMEOUT,
	                (int)sizeof(usb_pkt_rx));
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq((int)sizeof(usb_pkt_rx), poll_rc);
	mu_assert_int_eq(3, fake.calls);
	mu_assert(text[0] == '\0', text);

	/* A stall, then the packet. */
	script_attempts(LIBUSB_ERROR_PIPE, (int)sizeof(usb_pkt_rx),
	                (int)sizeof(usb_pkt_rx));
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq((int)sizeof(usb_pkt_rx), poll_rc);
	mu_assert_int_eq(2, fake.calls);
	mu_assert(text[0] == '\0', text);

	/* A board with nothing to hand over answers one byte, which is not a
	 * failure and is not retried. */
	script_attempts(1, (int)sizeof(usb_pkt_rx), (int)sizeof(usb_pkt_rx));
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq(1, poll_rc);
	mu_assert_int_eq(1, fake.calls);
	mu_assert(text[0] == '\0', text);
}

/*
 * Three attempts and no more, so a board that has stopped answering ends the
 * capture in about three seconds rather than holding it open. The failure is
 * reported here, because the attempts happened inside this call and the
 * caller's "USB poll failed" does not say what went wrong.
 */
MU_TEST(a_poll_that_keeps_failing_gives_up_and_says_why)
{
	char text[256];

	script_attempts(LIBUSB_ERROR_TIMEOUT, LIBUSB_ERROR_TIMEOUT,
	                LIBUSB_ERROR_TIMEOUT);
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, poll_rc);
	mu_assert_int_eq(3, fake.calls);
	mu_assert(strcmp(text, "error: the Ubertooth did not answer in time "
	    "(libusb -7)\n") == 0, text);

	/* A stall that survives all three attempts is reported too, in the
	 * wording libubertooth gives every other failed request. It used to
	 * return in silence: the loop had run out of attempts and said
	 * nothing. */
	script_attempts(LIBUSB_ERROR_PIPE, LIBUSB_ERROR_PIPE,
	                LIBUSB_ERROR_PIPE);
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq(LIBUSB_ERROR_PIPE, poll_rc);
	mu_assert_int_eq(3, fake.calls);
	mu_assert(strstr(text, "-9") != NULL, text);
}

/*
 * The retries keep a capture running, so they stop mattering the moment the
 * user asks the run to stop: a Ctrl-C that lands while the board is not
 * answering used to wait for the attempts still to come, up to about three
 * seconds. The poll gives up at once instead, and the failure it already had
 * is reported and handed to the caller, which ends the run with
 * "error: USB poll failed" and exit 1.
 */
MU_TEST(a_stop_ends_the_poll_retries_at_once)
{
	char text[256];

	/* The stop arrives during the first attempt, which timed out: no
	 * second attempt, and the timeout is the caller's answer. */
	script_attempts(LIBUSB_ERROR_TIMEOUT, (int)sizeof(usb_pkt_rx),
	                (int)sizeof(usb_pkt_rx));
	fake.stop_after = 1;
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, poll_rc);
	mu_assert_int_eq(1, fake.calls);
	mu_assert(strcmp(text, "error: the Ubertooth did not answer in time "
	    "(libusb -7)\n") == 0, text);

	/* One attempt later: the second was already under way when the stop
	 * arrived, so the third is the one dropped. */
	script_attempts(LIBUSB_ERROR_PIPE, LIBUSB_ERROR_PIPE,
	                (int)sizeof(usb_pkt_rx));
	fake.stop_after = 2;
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq(LIBUSB_ERROR_PIPE, poll_rc);
	mu_assert_int_eq(2, fake.calls);
	mu_assert(strstr(text, "-9") != NULL, text);

	/* A stop does not throw away a poll that worked: the packet is handed
	 * back, and the capture loop reads the stop on its next turn. The stop
	 * belongs to the retry, not to the poll. */
	script_attempts((int)sizeof(usb_pkt_rx), (int)sizeof(usb_pkt_rx),
	                (int)sizeof(usb_pkt_rx));
	fake.stop_after = 1;
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq((int)sizeof(usb_pkt_rx), poll_rc);
	mu_assert_int_eq(1, fake.calls);
	mu_assert(text[0] == '\0', text);

	/* Nobody stopped this run, so the retries are exactly as they were:
	 * three attempts for a board that keeps timing out, and the one that
	 * recovers on its second attempt still hands back the packet. */
	script_attempts(LIBUSB_ERROR_TIMEOUT, LIBUSB_ERROR_TIMEOUT,
	                LIBUSB_ERROR_TIMEOUT);
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, poll_rc);
	mu_assert_int_eq(3, fake.calls);

	script_attempts(LIBUSB_ERROR_TIMEOUT, (int)sizeof(usb_pkt_rx),
	                (int)sizeof(usb_pkt_rx));
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq((int)sizeof(usb_pkt_rx), poll_rc);
	mu_assert_int_eq(2, fake.calls);
	mu_assert(text[0] == '\0', text);
}

/*
 * A board that has gone -- the watchdog resets one that stops answering -- is
 * not worth three attempts: that failure ends the poll at once.
 */
MU_TEST(a_board_that_has_left_the_bus_ends_the_poll_at_once)
{
	char text[256];

	script_attempts(LIBUSB_ERROR_NO_DEVICE, (int)sizeof(usb_pkt_rx),
	                (int)sizeof(usb_pkt_rx));
	run_captured(poll_once, text, sizeof(text));
	mu_assert_int_eq(LIBUSB_ERROR_NO_DEVICE, poll_rc);
	mu_assert_int_eq(1, fake.calls);
	mu_assert(text[0] != '\0', text);
}

int main(void)
{
	MU_RUN(one_byte_reads_return_the_byte_for_a_full_reply);
	MU_RUN(one_byte_reads_reject_an_empty_reply);
	MU_RUN(one_byte_reads_pass_libusb_errors_through);
	MU_RUN(channel_needs_both_bytes);
	MU_RUN(serial_needs_all_17_bytes);
	MU_RUN(range_result_needs_all_6_bytes);
	MU_RUN(watchdog_state_needs_the_whole_reply);
	MU_RUN(radio_timeouts_are_read_little_endian);
	MU_RUN(the_bootloader_report_reads_the_hash_and_the_block);
	MU_RUN(the_polled_reads_report_nothing);
	MU_RUN(short_reply_message_names_the_read_and_the_counts);
	MU_RUN(the_quiet_set_forms_report_nothing);
	MU_RUN(four_and_two_byte_reads_need_their_full_length);
	MU_RUN(read_access_address_returns_errors_separately);
	MU_RUN(no_read_writes_to_standard_output);
	MU_RUN(a_board_that_does_not_answer_is_reported_in_words);
	MU_RUN(other_libusb_errors_keep_their_own_report);
	MU_RUN(a_stalled_read_is_reported_as_unsupported);
	MU_RUN(a_poll_that_timed_out_or_stalled_is_tried_again);
	MU_RUN(a_poll_that_keeps_failing_gives_up_and_says_why);
	MU_RUN(a_stop_ends_the_poll_retries_at_once);
	MU_RUN(a_board_that_has_left_the_bus_ends_the_poll_at_once);
	return mu_report();
}
