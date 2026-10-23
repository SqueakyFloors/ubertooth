/*
 * Hardware-independent identity reports and exit-status handling for
 * ubertooth-util. See util_report.h.
 */
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

int util_report_board_id(const struct util_report_ops *ops, void *context,
                         FILE *out, FILE *err)
{
	int board_id = ops->get_board_id(context);
	const char *name;

	/* A negative value is a transfer error already reported by the library. */
	if (board_id < 0)
		return 1;
	name = util_board_name(board_id);
	if (name == NULL) {
		fprintf(err, "error: unknown board ID %d\n", board_id);
		return 1;
	}
	fprintf(out, "Board ID Number: %d (%s)\n", board_id, name);
	return 0;
}

int util_report_firmware(const struct util_report_ops *ops, void *context,
                         FILE *out)
{
	char revision[UTIL_TEXT_LEN];
	uint16_t api_version = 0;
	int status = 0;

	revision[0] = '\0';
	ops->get_firmware_revision(context, revision, sizeof(revision));
	revision[sizeof(revision) - 1] = '\0';
	fprintf(out, "Firmware version: %s", revision);
	if (ops->get_api_version(context, &api_version) >= 0)
		fprintf(out, " (API:%x.%02x)\n", (api_version >> 8) & 0xFF,
		        api_version & 0xFF);
	else {
		fprintf(out, "\n");
		status = 1;
	}
	return status;
}

int util_report_compile_info(const struct util_report_ops *ops, void *context,
                             FILE *out)
{
	char info[UTIL_TEXT_LEN];

	info[0] = '\0';
	ops->get_compile_info(context, info, sizeof(info));
	info[sizeof(info) - 1] = '\0';
	fprintf(out, "%s\n", info);
	return 0;
}

int util_report_part_id(const struct util_report_ops *ops, void *context,
                        FILE *out)
{
	int part_id = ops->get_part_id(context);

	/* A negative value is a transfer error already reported by the library. */
	if (part_id < 0)
		return 1;
	fprintf(out, "Part ID: %X\n", part_id);
	return 0;
}

int util_report_serial(const struct util_report_ops *ops, void *context,
                       FILE *out)
{
	uint8_t serial[UTIL_SERIAL_BUFFER_LEN] = { 0 };
	int i;

	if (ops->get_serial(context, serial) != 0)
		return 1;
	fprintf(out, "Serial No: ");
	for (i = 1; i < UTIL_SERIAL_BUFFER_LEN; i++)
		fprintf(out, "%02x", serial[i]);
	fprintf(out, "\n");
	return 0;
}
