/*
 * Hardware-independent tests for the fixed-size libubertooth read commands.
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

#define REPLY_MAX 17

static struct {
	int rc;                   /* returned by libusb_control_transfer() */
	uint8_t reply[REPLY_MAX]; /* copied into the caller's buffer, rc bytes */
	uint8_t last_request;
	uint16_t last_length;
} fake;

/* ---- libusb and libbtbb fakes ------------------------------------------ */

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

/* The library prints the one complete line for a short reply. */
MU_TEST(short_reply_message_names_the_read_and_the_counts)
{
	char text[256] = "";
	FILE *capture = tmpfile();
	int saved = dup(fileno(stderr));
	size_t n;

	mu_assert(capture != NULL && saved >= 0, "capture stderr");
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
	script(0, 0);
	cmd_read_access_address(NULL, &(uint32_t){0});
	script(1, 0x89);
	cmd_get_channel(NULL);
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	rewind(capture);
	n = fread(text, 1, sizeof(text) - 1, capture);
	text[n] = '\0';
	fclose(capture);
	mu_assert(strcmp(text,
	    "error: access address read failed: board returned 0 of 4 bytes\n"
	    "error: channel read failed: board returned 1 of 2 bytes\n") == 0, text);
}

MU_TEST(range_result_needs_all_5_bytes)
{
	rangetest_result rr;

	script(5, 0);
	fake.reply[0] = 1;
	fake.reply[1] = 7;
	fake.reply[4] = 3;
	mu_assert_int_eq(0, cmd_get_rangeresult(NULL, &rr));
	mu_assert_int_eq(1, rr.valid);
	mu_assert_int_eq(7, rr.request_pa);
	mu_assert_int_eq(3, rr.reply_num);

	memset(&rr, 0x5a, sizeof(rr));
	script(4, 0x11);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_get_rangeresult(NULL, &rr));
	mu_assert_int_eq(0x5a, rr.valid); /* left untouched */
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
	script(5, 0);
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

int main(void)
{
	MU_RUN(one_byte_reads_return_the_byte_for_a_full_reply);
	MU_RUN(one_byte_reads_reject_an_empty_reply);
	MU_RUN(one_byte_reads_pass_libusb_errors_through);
	MU_RUN(channel_needs_both_bytes);
	MU_RUN(serial_needs_all_17_bytes);
	MU_RUN(range_result_needs_all_5_bytes);
	MU_RUN(short_reply_message_names_the_read_and_the_counts);
	MU_RUN(four_and_two_byte_reads_need_their_full_length);
	MU_RUN(read_access_address_returns_errors_separately);
	MU_RUN(no_read_writes_to_standard_output);
	return mu_report();
}
