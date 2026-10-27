/*
 * Hardware-independent identity reports and exit-status handling for
 * ubertooth-util. See util_report.h.
 */
#include <inttypes.h>
#include <string.h>

#include "util_report.h"
/* The modes, wait sites and reset causes the board reports, and the timeout
 * it is armed with: all from the interface the firmware shares with the host. */
#include "ubertooth_interface.h"

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

/*
 * The watchdog report. Names are indexed by the interface's own enumerations,
 * so a mode or wait site added there without a name here reads as unknown
 * rather than as the wrong thing.
 */
static const char *const watchdog_mode_names[] = {
	[MODE_IDLE]          = "idle",
	[MODE_RX_SYMBOLS]    = "rx symbols",
	[MODE_TX_SYMBOLS]    = "tx symbols",
	[MODE_TX_TEST]       = "tx test",
	[MODE_SPECAN]        = "spectrum analyzer",
	[MODE_RANGE_TEST]    = "range test",
	[MODE_REPEATER]      = "repeater",
	[MODE_LED_SPECAN]    = "LED spectrum analyzer",
	[MODE_BT_FOLLOW]     = "Bluetooth follow",
	[MODE_BT_FOLLOW_LE]  = "BLE follow",
	[MODE_BT_PROMISC_LE] = "BLE promiscuous",
	[MODE_RESET]         = "reset",
	[MODE_BT_SLAVE_LE]   = "BLE slave",
	[MODE_EGO]           = "E-GO",
	[MODE_AFH]           = "AFH",
	[MODE_RX_GENERIC]    = "generic rx",
	[MODE_TX_GENERIC]    = "generic tx",
	[MODE_XMAS]          = "xmas",
	[MODE_IDENTIFY]      = "identify",
};

static const char *const watchdog_site_text[WATCHDOG_SITE_COUNT] = {
	[WATCHDOG_SITE_NONE]      = "not in a wait",
	[WATCHDOG_SITE_USB]       = "servicing USB",
	[WATCHDOG_SITE_FS_LOCK]   = "waiting for the synthesizer to lock",
	[WATCHDOG_SITE_FS_UNLOCK] = "waiting for the synthesizer to unlock",
	[WATCHDOG_SITE_XOSC]      = "waiting for the 16 MHz crystal oscillator",
	[WATCHDOG_SITE_FSM]       = "waiting on the radio's state machine",
	[WATCHDOG_SITE_LE_RX]     = "waiting for a BLE packet or the next hop",
};

const char *util_watchdog_mode_name(unsigned mode)
{
	if (mode == MODE_UNKNOWN)
		return "not reported";
	if (mode >= sizeof(watchdog_mode_names) / sizeof(watchdog_mode_names[0]) ||
	    watchdog_mode_names[mode] == NULL)
		return "unknown";
	return watchdog_mode_names[mode];
}

const char *util_watchdog_site_text(unsigned site)
{
	if (site >= WATCHDOG_SITE_COUNT || watchdog_site_text[site] == NULL)
		return "in a wait this version does not know";
	return watchdog_site_text[site];
}

/* "power-on", or several joined with "+", or "not recorded" for no bits. The
 * buffer is written even when nothing is known, so callers never print from an
 * uninitialised one. */
void util_watchdog_cause_text(unsigned cause, char *text, size_t size)
{
	static const struct { unsigned bit; const char *name; } causes[] = {
		{ WATCHDOG_RESET_POWER_ON,  "power-on" },
		{ WATCHDOG_RESET_EXTERNAL,  "reset pin" },
		{ WATCHDOG_RESET_WATCHDOG,  "watchdog" },
		{ WATCHDOG_RESET_BROWN_OUT, "brown-out" },
	};
	size_t i, used = 0;

	if (size == 0)
		return;
	text[0] = '\0';
	for (i = 0; i < sizeof(causes) / sizeof(causes[0]); i++) {
		if (!(cause & causes[i].bit))
			continue;
		if (used > 0 && used + 1 < size) {
			text[used++] = '+';
			text[used] = '\0';
		}
		strncat(text + used, causes[i].name, size - used - 1);
		used = strlen(text);
	}
	if (used == 0)
		strncpy(text, "not recorded", size - 1);
	text[size - 1] = '\0';
}

int util_report_watchdog(FILE *out, const uint8_t *info, size_t len)
{
	char cause[64];

	/* Unreachable from ubertooth-util, which passes a fixed-size array that
	 * libubertooth has already refused if the board sent fewer bytes. It is
	 * here so that a caller which skipped that check gets nothing rather
	 * than a report half-filled from whatever the array held; that caller
	 * has the reply length and is the one able to say what went wrong, which
	 * is why nothing is printed here. */
	if (info == NULL || len < UBERTOOTH_WATCHDOG_INFO_LEN)
		return 1;

	util_watchdog_cause_text(info[0], cause, sizeof(cause));
	fprintf(out, "Last boot cause: %s\n", cause);
	if (info[1])
		fprintf(out, "Watchdog: armed; %u ms without USB being serviced "
		        "resets the board\n", (unsigned)UBERTOOTH_WATCHDOG_TIMEOUT_MS);
	else
		fprintf(out, "Watchdog: not armed\n");
	fprintf(out, "Unrequested watchdog resets since power-on: %u (in %u boot%s)\n",
	        (unsigned)info[2], (unsigned)info[3], info[3] == 1 ? "" : "s");
	if (info[2] > 0)
		fprintf(out, "  the last was in %s mode, %s\n",
		        util_watchdog_mode_name(info[4]),
		        util_watchdog_site_text(info[5]));
	fprintf(out, "Now in %s mode, %s\n",
	        util_watchdog_mode_name(info[6]),
	        util_watchdog_site_text(info[7]));
	return 0;
}
