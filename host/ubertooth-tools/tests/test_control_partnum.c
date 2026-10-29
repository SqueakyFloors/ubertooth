/*
 * Hardware-independent tests for the libubertooth part-ID commands.
 *
 * The real ubertooth_control.c is compiled into this test, and the test
 * defines libusb_control_transfer() itself so the library's calls resolve to a
 * scripted reply instead of libusb. Status-byte, short-reply and bit-31
 * handling are checked without an attached Ubertooth.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <btbb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth_control.h"

static struct {
	int rc;           /* returned by libusb_control_transfer() */
	uint8_t reply[5]; /* copied into the caller's buffer */
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

/* The poll stops retrying once the user has asked the run to stop. Nothing
 * here polls, and no signal arrives. */
int ubertooth_stop_requested(void)
{
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
	if (fake.rc > 0)
		memcpy(data, fake.reply,
		       (size_t)fake.rc < sizeof(fake.reply) ? (size_t)fake.rc
		                                            : sizeof(fake.reply));
	return fake.rc;
}

/* ubertooth_control.c references libbtbb; only the part-ID path runs here. */
uint64_t btbb_gen_syncword(const int LAP)
{
	(void)LAP;
	return 0;
}

/* ---- helpers ------------------------------------------------------------ */

static void script(int rc, uint8_t status, uint32_t id)
{
	memset(&fake, 0, sizeof(fake));
	fake.rc = rc;
	fake.reply[0] = status;
	fake.reply[1] = (uint8_t)id;
	fake.reply[2] = (uint8_t)(id >> 8);
	fake.reply[3] = (uint8_t)(id >> 16);
	fake.reply[4] = (uint8_t)(id >> 24);
}

/* ---- tests -------------------------------------------------------------- */

MU_TEST(part_id_is_read_little_endian)
{
	uint32_t id = 0;

	script(5, 0, 0x25011723u);
	mu_assert_int_eq(0, cmd_get_part_id(NULL, &id));
	mu_assert(id == 0x25011723u, "wrong part ID");
	mu_assert_int_eq(UBERTOOTH_GET_PARTNUM, fake.last_request);
	mu_assert_int_eq(5, fake.last_length);

	script(5, 0, 0x25011723u);
	mu_assert_int_eq(0x25011723, cmd_get_partnum(NULL));
}

MU_TEST(nonzero_status_byte_is_an_error)
{
	uint32_t id = 0x1234u;

	script(5, 1, 0x25011723u);
	mu_assert_int_eq(LIBUSB_ERROR_OTHER, cmd_get_part_id(NULL, &id));
	mu_assert(id == 0x1234u, "part ID written despite error");

	script(5, 1, 0x25011723u);
	mu_assert_int_eq(LIBUSB_ERROR_OTHER, cmd_get_partnum(NULL));
}

MU_TEST(transfer_error_is_returned)
{
	uint32_t id = 0;

	script(LIBUSB_ERROR_TIMEOUT, 0, 0);
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, cmd_get_part_id(NULL, &id));
	script(LIBUSB_ERROR_TIMEOUT, 0, 0);
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, cmd_get_partnum(NULL));
}

MU_TEST(short_reply_is_an_error)
{
	uint32_t id = 0;

	script(3, 0, 0x25011723u);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_get_part_id(NULL, &id));
	script(0, 0, 0);
	mu_assert_int_eq(UBERTOOTH_ERROR_SHORT_REPLY, cmd_get_partnum(NULL));
}

MU_TEST(bit_31_is_kept_unsigned_and_rejected_by_the_int_api)
{
	uint32_t id = 0;

	script(5, 0, 0x8000ABCDu);
	mu_assert_int_eq(0, cmd_get_part_id(NULL, &id));
	mu_assert(id == 0x8000ABCDu, "bit 31 part ID changed");

	script(5, 0, 0x8000ABCDu);
	mu_assert_int_eq(LIBUSB_ERROR_OVERFLOW, cmd_get_partnum(NULL));

	script(5, 0, 0x7FFFFFFFu);
	mu_assert_int_eq(0x7FFFFFFF, cmd_get_partnum(NULL));
}

int main(void)
{
	MU_RUN(part_id_is_read_little_endian);
	MU_RUN(nonzero_status_byte_is_an_error);
	MU_RUN(transfer_error_is_returned);
	MU_RUN(short_reply_is_an_error);
	MU_RUN(bit_31_is_kept_unsigned_and_rejected_by_the_int_api);
	return mu_report();
}
