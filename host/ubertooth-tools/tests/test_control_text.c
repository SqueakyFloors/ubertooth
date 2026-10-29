/*
 * Hardware-independent tests for the libubertooth firmware revision, compile
 * info and firmware info block commands.
 *
 * The real ubertooth_control.c is compiled into this test, and the test
 * defines libusb_control_transfer() itself so the library's calls resolve to a
 * scripted reply instead of libusb. Transfer errors, short or malformed
 * replies, truncation and the old void wrappers are checked without an
 * attached Ubertooth.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <btbb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth_control.h"

static struct {
	int rc;             /* returned by libusb_control_transfer() */
	uint8_t reply[300]; /* copied into the caller's buffer */
	int calls;
	uint8_t last_request;
	uint16_t last_length;
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
	fake.calls++;
	fake.last_request = bRequest;
	fake.last_length = wLength;
	/* Like a real transfer, never write past the caller's wLength. */
	if (fake.rc > 0)
		memcpy(data, fake.reply, (size_t)fake.rc < wLength ? (size_t)fake.rc
		                                                   : wLength);
	return fake.rc;
}

/* ubertooth_control.c references libbtbb; only the text commands run here. */
uint64_t btbb_gen_syncword(const int LAP)
{
	(void)LAP;
	return 0;
}

/* ---- helpers ------------------------------------------------------------ */

/* A revision reply as the firmware sends it: two zero bytes, a length byte,
 * then the string. extra is added to (or, if negative, removed from) the
 * transfer length without changing the length byte. */
static void script_revision(const char *text, int extra)
{
	size_t n = strlen(text);

	memset(&fake, 0, sizeof(fake));
	fake.reply[2] = (uint8_t)n;
	memcpy(&fake.reply[3], text, n);
	fake.rc = 3 + (int)n + extra;
}

/* A compile info reply: a length byte, then the string. */
static void script_compile_info(const char *text, int extra)
{
	size_t n = strlen(text);

	memset(&fake, 0, sizeof(fake));
	fake.reply[0] = (uint8_t)n;
	memcpy(&fake.reply[1], text, n);
	fake.rc = 1 + (int)n + extra;
}

static void script_rc(int rc)
{
	memset(&fake, 0, sizeof(fake));
	fake.rc = rc;
}

/* ---- firmware revision -------------------------------------------------- */

MU_TEST(revision_string_is_returned)
{
	char text[64];

	script_revision("git-1234abcd", 0);
	memset(text, 'x', sizeof(text));
	mu_assert_int_eq(0, cmd_get_firmware_revision(NULL, text, sizeof(text)));
	mu_assert(strcmp(text, "git-1234abcd") == 0, "wrong revision");
	mu_assert_int_eq(UBERTOOTH_GET_REV_NUM, fake.last_request);
}

MU_TEST(old_style_numeric_revision_is_returned)
{
	char text[64];

	script_rc(2);
	fake.reply[0] = 0x39;
	fake.reply[1] = 0x05; /* 0x0539 = 1337 */
	mu_assert_int_eq(0, cmd_get_firmware_revision(NULL, text, sizeof(text)));
	mu_assert(strcmp(text, "1337") == 0, "wrong numeric revision");

	/* A small buffer is still terminated. */
	script_rc(2);
	fake.reply[0] = 0x39;
	fake.reply[1] = 0x05;
	mu_assert_int_eq(0, cmd_get_firmware_revision(NULL, text, 3));
	mu_assert(strcmp(text, "13") == 0, "numeric revision not truncated");
}

MU_TEST(revision_transfer_errors_are_returned_with_an_empty_buffer)
{
	static const int errors[] = {
		LIBUSB_ERROR_TIMEOUT, LIBUSB_ERROR_PIPE, LIBUSB_ERROR_NO_DEVICE,
	};
	size_t i;

	for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
		char text[64] = "stale";

		script_rc(errors[i]);
		mu_assert_int_eq(errors[i],
		                 cmd_get_firmware_revision(NULL, text, sizeof(text)));
		mu_assert_int_eq(0, (long)strlen(text));
	}
}

MU_TEST(short_or_malformed_revision_reply_is_an_error)
{
	char text[64];
	int rc;

	/* Zero or one byte: neither the numeric form nor the 3-byte header. */
	for (rc = 0; rc <= 1; rc++) {
		script_rc(rc);
		strcpy(text, "stale");
		mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
		                 cmd_get_firmware_revision(NULL, text, sizeof(text)));
		mu_assert_int_eq(0, (long)strlen(text));
	}

	/* The length byte claims more bytes than were sent. */
	script_revision("git-1234abcd", -4);
	strcpy(text, "stale");
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_firmware_revision(NULL, text, sizeof(text)));
	mu_assert_int_eq(0, (long)strlen(text));

	/* Header only, with a nonzero length. */
	script_revision("abc", -3);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_firmware_revision(NULL, text, sizeof(text)));
}

MU_TEST(revision_extra_bytes_and_empty_string_are_accepted)
{
	char text[64];

	script_revision("git-1", 5); /* trailing bytes beyond the length byte */
	mu_assert_int_eq(0, cmd_get_firmware_revision(NULL, text, sizeof(text)));
	mu_assert(strcmp(text, "git-1") == 0, "trailing bytes not ignored");

	script_revision("", 0);
	strcpy(text, "stale");
	mu_assert_int_eq(0, cmd_get_firmware_revision(NULL, text, sizeof(text)));
	mu_assert_int_eq(0, (long)strlen(text));
}

MU_TEST(long_revision_is_truncated_to_the_buffer)
{
	char text[6];

	script_revision("git-1234abcd", 0);
	mu_assert_int_eq(0, cmd_get_firmware_revision(NULL, text, sizeof(text)));
	mu_assert(strcmp(text, "git-1") == 0, "revision not truncated");

	script_revision("git-1234abcd", 0);
	mu_assert_int_eq(0, cmd_get_firmware_revision(NULL, text, 1));
	mu_assert_int_eq(0, (long)strlen(text));
}

MU_TEST(revision_rejects_a_missing_buffer_without_a_transfer)
{
	char text[4];

	script_revision("git-1", 0);
	mu_assert_int_eq(LIBUSB_ERROR_INVALID_PARAM,
	                 cmd_get_firmware_revision(NULL, NULL, sizeof(text)));
	mu_assert_int_eq(LIBUSB_ERROR_INVALID_PARAM,
	                 cmd_get_firmware_revision(NULL, text, 0));
	mu_assert_int_eq(0, fake.calls);
}

/* ---- compile info ------------------------------------------------------- */

MU_TEST(compile_info_string_is_returned)
{
	char text[128];

	script_compile_info("ubertooth git-1 (user@host) Fri Oct 23", 0);
	mu_assert_int_eq(0, cmd_get_firmware_compile_info(NULL, text, sizeof(text)));
	mu_assert(strcmp(text, "ubertooth git-1 (user@host) Fri Oct 23") == 0,
	          "wrong compile info");
	mu_assert_int_eq(UBERTOOTH_GET_COMPILE_INFO, fake.last_request);
}

MU_TEST(compile_info_transfer_errors_are_returned_with_an_empty_buffer)
{
	char text[64] = "stale";

	script_rc(LIBUSB_ERROR_TIMEOUT);
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT,
	                 cmd_get_firmware_compile_info(NULL, text, sizeof(text)));
	mu_assert_int_eq(0, (long)strlen(text));

	strcpy(text, "stale");
	script_rc(LIBUSB_ERROR_PIPE);
	mu_assert_int_eq(LIBUSB_ERROR_PIPE,
	                 cmd_get_firmware_compile_info(NULL, text, sizeof(text)));
	mu_assert_int_eq(0, (long)strlen(text));
}

MU_TEST(short_or_malformed_compile_info_reply_is_an_error)
{
	char text[64] = "stale";

	script_rc(0);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_firmware_compile_info(NULL, text, sizeof(text)));
	mu_assert_int_eq(0, (long)strlen(text));

	script_compile_info("ubertooth git-1", -1);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_firmware_compile_info(NULL, text, sizeof(text)));

	/* Length byte 255 with only the length byte sent. */
	script_rc(1);
	fake.reply[0] = 255;
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_firmware_compile_info(NULL, text, sizeof(text)));
}

MU_TEST(long_compile_info_is_truncated_and_missing_buffer_rejected)
{
	char text[5];

	script_compile_info("ubertooth git-1", 0);
	mu_assert_int_eq(0, cmd_get_firmware_compile_info(NULL, text, sizeof(text)));
	mu_assert(strcmp(text, "uber") == 0, "compile info not truncated");

	script_compile_info("ubertooth git-1", 0);
	mu_assert_int_eq(LIBUSB_ERROR_INVALID_PARAM,
	                 cmd_get_firmware_compile_info(NULL, NULL, 5));
	mu_assert_int_eq(LIBUSB_ERROR_INVALID_PARAM,
	                 cmd_get_firmware_compile_info(NULL, text, 0));
	mu_assert_int_eq(0, fake.calls);
}

/* ---- the void wrappers keep their old contract -------------------------- */

MU_TEST(void_wrappers_return_text_or_an_error_string)
{
	char text[64];

	script_revision("git-1234abcd", 0);
	cmd_get_rev_num(NULL, text, sizeof(text));
	mu_assert(strcmp(text, "git-1234abcd") == 0, "wrapper revision");

	script_rc(LIBUSB_ERROR_TIMEOUT);
	cmd_get_rev_num(NULL, text, sizeof(text));
	mu_assert(strcmp(text, "error: -7") == 0, "wrapper revision error text");

	script_revision("git-1234abcd", -4); /* short reply */
	cmd_get_rev_num(NULL, text, sizeof(text));
	mu_assert(strcmp(text, "error: -1") == 0, "wrapper short reply text");

	script_compile_info("compile-test", 0);
	cmd_get_compile_info(NULL, text, sizeof(text));
	mu_assert(strcmp(text, "compile-test") == 0, "wrapper compile info");

	script_rc(LIBUSB_ERROR_PIPE);
	cmd_get_compile_info(NULL, text, sizeof(text));
	mu_assert(strcmp(text, "error: -9") == 0, "wrapper compile info error text");

	/* A zero-length buffer is left alone instead of written before its start. */
	script_rc(LIBUSB_ERROR_TIMEOUT);
	text[0] = 'k';
	cmd_get_rev_num(NULL, text, 0);
	cmd_get_compile_info(NULL, text, 0);
	mu_assert(text[0] == 'k', "zero-length buffer written");
}

/* ---- the firmware info block ------------------------------------------- */

/* The 64 bytes the firmware returns for UBERTOOTH_GET_FW_INFO. */
static void set_fw_info_reply(const char *magic, uint16_t format,
                              uint16_t api, const char *image,
                              const char *revision)
{
	memset(&fake, 0, sizeof(fake));
	fake.rc = UBERTOOTH_FW_INFO_LENGTH;
	memcpy(fake.reply + UBERTOOTH_FW_INFO_OFF_MAGIC, magic,
	       UBERTOOTH_FW_INFO_MAGIC_LEN);
	fake.reply[UBERTOOTH_FW_INFO_OFF_FORMAT] = (uint8_t)(format & 0xff);
	fake.reply[UBERTOOTH_FW_INFO_OFF_FORMAT + 1] = (uint8_t)(format >> 8);
	fake.reply[UBERTOOTH_FW_INFO_OFF_API] = (uint8_t)(api & 0xff);
	fake.reply[UBERTOOTH_FW_INFO_OFF_API + 1] = (uint8_t)(api >> 8);
	memcpy(fake.reply + UBERTOOTH_FW_INFO_OFF_IMAGE, image, strlen(image));
	memcpy(fake.reply + UBERTOOTH_FW_INFO_OFF_REVISION, revision,
	       strlen(revision));
}

MU_TEST(firmware_info_block_is_returned)
{
	struct ubertooth_fw_info info;

	set_fw_info_reply(UBERTOOTH_FW_INFO_MAGIC, UBERTOOTH_FW_INFO_FORMAT,
	                  0x0134, "btbr", "git-deadbee*");
	mu_assert_int_eq(0, cmd_get_firmware_info(NULL, &info));
	mu_assert_int_eq(UBERTOOTH_GET_FW_INFO, fake.last_request);
	mu_assert_int_eq(UBERTOOTH_FW_INFO_LENGTH, fake.last_length);
	mu_assert(strcmp(info.image, "btbr") == 0, "wrong image name");
	mu_assert(strcmp(info.revision, "git-deadbee*") == 0, "wrong revision");
	mu_assert_int_eq(0x0134, info.api_version);
}

MU_TEST(firmware_info_transfer_errors_are_returned_with_a_zeroed_block)
{
	struct ubertooth_fw_info info;

	memset(&fake, 0, sizeof(fake));
	fake.rc = LIBUSB_ERROR_TIMEOUT;
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT,
	                 cmd_get_firmware_info(NULL, &info));
	mu_assert(info.image[0] == '\0' && info.revision[0] == '\0',
	          "a failed read left text behind");
}

/* A reply with fewer than 64 bytes in it is a short reply, as the other reads
 * report one; a full-length reply that is not a block is a disagreement about
 * the format that no length check can catch. */
MU_TEST(a_short_or_unreadable_firmware_info_reply_is_an_error)
{
	struct ubertooth_fw_info info;

	set_fw_info_reply(UBERTOOTH_FW_INFO_MAGIC, UBERTOOTH_FW_INFO_FORMAT,
	                  0x0134, "btbr", "git-deadbee*");
	fake.rc = UBERTOOTH_FW_INFO_LENGTH - 1;
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY,
	                 cmd_get_firmware_info(NULL, &info));

	set_fw_info_reply("NOTABLCK", UBERTOOTH_FW_INFO_FORMAT, 0x0134, "btbr",
	                  "git-deadbee*");
	mu_assert_int_eq(LIBUSB_ERROR_IO, cmd_get_firmware_info(NULL, &info));

	set_fw_info_reply(UBERTOOTH_FW_INFO_MAGIC, 99, 0x0134, "btbr",
	                  "git-deadbee*");
	mu_assert_int_eq(LIBUSB_ERROR_IO, cmd_get_firmware_info(NULL, &info));

	set_fw_info_reply(UBERTOOTH_FW_INFO_MAGIC, UBERTOOTH_FW_INFO_FORMAT,
	                  0x0134, "", "git-deadbee*");
	mu_assert_int_eq(LIBUSB_ERROR_IO, cmd_get_firmware_info(NULL, &info));
}

/* A board stalls a request its firmware does not implement, which is how
 * every image built before the block answers this read. The code comes back
 * for the caller to handle; unlike the other reads, this one says nothing,
 * because ubertooth-util -v carries on from the revision instead. */
MU_TEST(a_stalled_firmware_info_read_is_returned_without_a_message)
{
	struct ubertooth_fw_info info;
	FILE *captured;
	int saved, result;
	long written;

	memset(&fake, 0, sizeof(fake));
	fake.rc = LIBUSB_ERROR_PIPE;
	captured = tmpfile();
	mu_assert(captured != NULL, "no temporary file to capture stderr in");
	fflush(stderr);
	saved = dup(STDERR_FILENO);
	dup2(fileno(captured), STDERR_FILENO);
	result = cmd_get_firmware_info(NULL, &info);
	fflush(stderr);
	dup2(saved, STDERR_FILENO);
	close(saved);
	fseek(captured, 0, SEEK_END);
	written = ftell(captured);
	fclose(captured);

	mu_assert_int_eq(LIBUSB_ERROR_PIPE, result);
	mu_assert_int_eq(0, written);
}

MU_TEST(firmware_info_rejects_a_missing_block_without_a_transfer)
{
	memset(&fake, 0, sizeof(fake));
	mu_assert_int_eq(LIBUSB_ERROR_INVALID_PARAM,
	                 cmd_get_firmware_info(NULL, NULL));
	mu_assert_int_eq(0, fake.calls);
}

int main(void)
{
	MU_RUN(revision_string_is_returned);
	MU_RUN(old_style_numeric_revision_is_returned);
	MU_RUN(revision_transfer_errors_are_returned_with_an_empty_buffer);
	MU_RUN(short_or_malformed_revision_reply_is_an_error);
	MU_RUN(revision_extra_bytes_and_empty_string_are_accepted);
	MU_RUN(long_revision_is_truncated_to_the_buffer);
	MU_RUN(revision_rejects_a_missing_buffer_without_a_transfer);
	MU_RUN(compile_info_string_is_returned);
	MU_RUN(compile_info_transfer_errors_are_returned_with_an_empty_buffer);
	MU_RUN(short_or_malformed_compile_info_reply_is_an_error);
	MU_RUN(long_compile_info_is_truncated_and_missing_buffer_rejected);
	MU_RUN(void_wrappers_return_text_or_an_error_string);
	MU_RUN(firmware_info_block_is_returned);
	MU_RUN(firmware_info_transfer_errors_are_returned_with_a_zeroed_block);
	MU_RUN(a_short_or_unreadable_firmware_info_reply_is_an_error);
	MU_RUN(a_stalled_firmware_info_read_is_returned_without_a_message);
	MU_RUN(firmware_info_rejects_a_missing_block_without_a_transfer);
	return mu_report();
}
