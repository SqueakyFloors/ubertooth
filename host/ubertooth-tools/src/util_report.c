/*
 * Hardware-independent identity reports and exit-status handling for
 * ubertooth-util. See util_report.h.
 */
#include <inttypes.h>
#include <string.h>

#include "util_report.h"

#define UTIL_TEXT_LEN 255

static const char *const board_names[] = {
	"Ubertooth Zero",
	"Ubertooth One",
	"ToorCon 13 Badge",
};

const char *util_board_name(int board_id)
{
	if (board_id < 0 || (size_t)board_id >= util_board_count())
		return NULL;
	return board_names[board_id];
}

size_t util_board_count(void)
{
	return sizeof(board_names) / sizeof(board_names[0]);
}

int util_status_merge(int current, int next)
{
	return current != 0 ? current : next;
}

int util_value_status(int value)
{
	return value < 0 ? 1 : 0;
}

int util_status_after(int status, int device_result)
{
	return util_status_merge(status, util_value_status(device_result));
}

#define READ_FAILED_PREFIX "error: "
#define READ_FAILED_SUFFIX " read failed\n"

int util_report_read_failed(FILE *err, const char *what)
{
	fprintf(err, READ_FAILED_PREFIX "%s" READ_FAILED_SUFFIX, what);
	return 1;
}

int util_report_read_error(FILE *err, const char *what, int code)
{
	/* libubertooth already printed the whole line for a short reply */
	if (code == UTIL_ERROR_SHORT_REPLY)
		return 1;
	return util_report_read_failed(err, what);
}

int util_is_read_failed_message(const char *message, const char *what)
{
	size_t prefix = strlen(READ_FAILED_PREFIX), length = strlen(what);

	return strncmp(message, READ_FAILED_PREFIX, prefix) == 0 &&
	       strncmp(message + prefix, what, length) == 0 &&
	       strcmp(message + prefix + length, READ_FAILED_SUFFIX) == 0;
}

int util_report_board_id(const struct util_report_ops *ops, void *context,
                         FILE *out, FILE *err)
{
	int board_id = ops->get_board_id(context);
	const char *name;

	/* A negative value is a transfer error; the library reported it. */
	if (board_id < 0)
		return util_report_read_error(err, UTIL_QUERY_BOARD_ID, board_id);
	name = util_board_name(board_id);
	if (name == NULL) {
		fprintf(err, "error: unknown board ID %d\n", board_id);
		return 1;
	}
	fprintf(out, "Board ID Number: %d (%s)\n", board_id, name);
	return 0;
}

/*
 * The image and the revision come from the firmware info block, which is also
 * what ubertooth-dfu reads out of a firmware file, so a board and the file it
 * was flashed from are named the same way.
 *
 * Firmware older than the block stalls the request. This is the one report
 * that has to work on any firmware a board might be running, so the revision
 * alone is read instead, and the line is the one this tool always printed.
 *
 * The API version comes from neither: it is the USB bcdDevice, which is the
 * number the other tools check a board against. The block carries its own
 * copy, and on a board running the btbr image the two differ -- btbr reports a
 * fixed bcdDevice of 1.06 -- so showing the descriptor's is what explains why
 * the other tools refuse such a board.
 */
int util_report_firmware(const struct util_report_ops *ops, void *context,
                         FILE *out)
{
	struct ubertooth_fw_info info;
	char revision[UTIL_TEXT_LEN];
	uint16_t api_version = 0;
	int status = 0, r;

	/* A failed read prints no line; the library already reported the error. */
	r = ops->get_firmware_info(context, &info);
	if (r < 0 && r != UTIL_ERROR_NO_FW_INFO)
		return 1;
	if (r == 0)
		fprintf(out, "Firmware version: %s %s", info.image, info.revision);
	else {
		revision[0] = '\0';
		if (ops->get_firmware_revision(context, revision,
		                               sizeof(revision)) < 0)
			return 1;
		revision[sizeof(revision) - 1] = '\0';
		fprintf(out, "Firmware version: %s", revision);
	}
	if (ops->get_api_version(context, &api_version) >= 0)
		fprintf(out, " (API:%x.%02x)\n", (api_version >> 8) & 0xFF,
		        api_version & 0xFF);
	else {
		fprintf(out, "\n");
		status = 1;
	}
	return status;
}

/*
 * The build string the board reports, named with the image it came from so
 * that it can be told apart from the other image's. Firmware older than the
 * info block cannot say which image it is, so the build string goes out on its
 * own, as it always did.
 */
int util_report_compile_info(const struct util_report_ops *ops, void *context,
                             FILE *out)
{
	struct ubertooth_fw_info block;
	char info[UTIL_TEXT_LEN];
	int r;

	info[0] = '\0';
	/* A failed read prints no line; the library already reported the error. */
	r = ops->get_firmware_info(context, &block);
	if (r < 0 && r != UTIL_ERROR_NO_FW_INFO)
		return 1;
	if (ops->get_compile_info(context, info, sizeof(info)) < 0)
		return 1;
	info[sizeof(info) - 1] = '\0';
	if (r == 0)
		fprintf(out, "%s: %s\n", block.image, info);
	else
		fprintf(out, "%s\n", info);
	return 0;
}

int util_report_part_id(const struct util_report_ops *ops, void *context,
                        FILE *out, FILE *err)
{
	uint32_t part_id = 0;
	int r;

	/* A negative value is an error; the library reported it. */
	r = ops->get_part_id(context, &part_id);
	if (r < 0)
		return util_report_read_error(err, UTIL_QUERY_PART_ID, r);
	fprintf(out, "Part ID: %" PRIX32 "\n", part_id);
	return 0;
}

int util_report_serial(const struct util_report_ops *ops, void *context,
                       FILE *out, FILE *err)
{
	uint8_t serial[UTIL_SERIAL_BUFFER_LEN] = { 0 };
	int i, r;

	r = ops->get_serial(context, serial);
	if (r != 0)
		return util_report_read_error(err, UTIL_QUERY_SERIAL, r);
	fprintf(out, "Serial No: ");
	for (i = 1; i < UTIL_SERIAL_BUFFER_LEN; i++)
		fprintf(out, "%02x", serial[i]);
	fprintf(out, "\n");
	return 0;
}
