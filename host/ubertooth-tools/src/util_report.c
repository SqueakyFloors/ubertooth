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

const char *util_strip_error_prefix(const char *message)
{
	size_t prefix = strlen(READ_FAILED_PREFIX);

	if (strncmp(message, READ_FAILED_PREFIX, prefix) == 0)
		return message + prefix;
	return message;
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
	[MODE_ISP]           = "ISP",
};

static const char *const watchdog_site_text[WATCHDOG_SITE_COUNT] = {
	[WATCHDOG_SITE_NONE]        = "not in a wait",
	[WATCHDOG_SITE_USB]         = "servicing USB",
	[WATCHDOG_SITE_FS_LOCK]     = "waiting for the synthesizer to lock",
	[WATCHDOG_SITE_FS_UNLOCK]   = "waiting for the synthesizer to unlock",
	[WATCHDOG_SITE_XOSC]        = "waiting for the 16 MHz crystal oscillator",
	[WATCHDOG_SITE_FSM]         = "waiting on the radio's state machine",
	[WATCHDOG_SITE_LE_RX]       = "waiting for a BLE packet or the next hop",
	[WATCHDOG_SITE_FAULT_HARD]  = "in the hard fault handler",
	[WATCHDOG_SITE_FAULT_MEM]   = "in the memory management fault handler",
	[WATCHDOG_SITE_FAULT_BUS]   = "in the bus fault handler",
	[WATCHDOG_SITE_FAULT_USAGE] = "in the usage fault handler",
	[WATCHDOG_SITE_PANIC]       = "stopped by the firmware itself",
	[WATCHDOG_SITE_LE_SSP]      = "emptying the BLE engine's symbol buffer",
	[WATCHDOG_SITE_LE_DMA]      = "in the BLE engine's received-byte interrupt",
	[WATCHDOG_SITE_LE_TIMER]    = "in the BLE engine's timer interrupt",
	[WATCHDOG_SITE_UNHANDLED_IRQ] = "in an interrupt nothing handles",
	[WATCHDOG_SITE_DEBUG_DMA]     = "stopped by a failed debug UART transfer",
	[WATCHDOG_SITE_USB_BULK]    = "handing a capture packet to the bulk endpoint",
	[WATCHDOG_SITE_USB_DEV]     = "handling a USB bus event",
	[WATCHDOG_SITE_USB_EP]      = "reading an endpoint's status",
	[WATCHDOG_SITE_USB_SETUP]   = "reading a control request",
	[WATCHDOG_SITE_USB_REQUEST] = "running a control request",
	[WATCHDOG_SITE_USB_REPLY]   = "writing a control reply",
	[WATCHDOG_SITE_USB_REALIZE] = "waiting for an endpoint to be realized",
};

const char *util_watchdog_mode_name(unsigned mode)
{
	/* btbr runs its own state machine rather than one of these modes, so it
	 * records none; the report writes a clause for that instead of a name. */
	if (mode == MODE_UNKNOWN)
		return NULL;
	if (mode >= sizeof(watchdog_mode_names) / sizeof(watchdog_mode_names[0]) ||
	    watchdog_mode_names[mode] == NULL)
		return "unknown";
	return watchdog_mode_names[mode];
}

/* The clause the report puts the mode in: "in idle mode", or "in a mode the
 * board does not report" where there is no name. The names only read as
 * English with "mode" after them, which a missing mode does not. */
static void print_watchdog_mode(FILE *out, unsigned mode)
{
	const char *name = util_watchdog_mode_name(mode);

	if (name == NULL)
		fputs("in a mode the board does not report", out);
	else
		fprintf(out, "in %s mode", name);
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

/* The reply's wider fields, low byte first. */
static unsigned util_le16(const uint8_t *at)
{
	return (unsigned)at[0] | ((unsigned)at[1] << 8);
}

static uint32_t util_le32(const uint8_t *at)
{
	return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
	       ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

int util_report_watchdog(FILE *out, const uint8_t *info, size_t len)
{
	char cause[64];
	uint32_t fault_status;
	uint32_t fault_addr;

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
		        "or the mode looping resets the board\n", (unsigned)UBERTOOTH_WATCHDOG_TIMEOUT_MS);
	else
		fprintf(out, "Watchdog: not armed\n");
	fprintf(out, "Unrequested watchdog resets since power-on: %u (in %u boot%s)\n",
	        (unsigned)info[2], (unsigned)info[3], info[3] == 1 ? "" : "s");
	if (info[2] > 0) {
		fputs("  the last was ", out);
		print_watchdog_mode(out, info[4]);
		fprintf(out, ", %s\n", util_watchdog_site_text(info[5]));
	}
	fputs("Now ", out);
	print_watchdog_mode(out, info[6]);
	fprintf(out, ", %s\n", util_watchdog_site_text(info[7]));

	/* The last processor fault, on its own line rather than under the reset
	 * above: a fault always resets the board, so it is one of the resets
	 * counted, but not necessarily the last of them. Nothing is printed
	 * until a fault has happened, which on most boards is never. */
	fault_status = util_le32(info + 10);
	fault_addr = util_le32(info + 14);
	/* Either field having something in it means a fault happened; both zero
	 * means none has. The status alone does not decide it, because a memory
	 * management fault can set no status bit at all, and then the address is
	 * the whole of the record. */
	if (fault_status != 0 || fault_addr != 0) {
		fprintf(out, "Last processor fault: status 0x%08x, ", fault_status);
		/* The board's own "the address register holds this fault's
		 * address" bit; see UBERTOOTH_FAULT_ADDR_VALID for why there are
		 * two of them. Read rather than comparing the address against
		 * zero, because zero is a real address -- a null pointer
		 * dereference records it. */
		if (fault_status & (UBERTOOTH_FAULT_ADDR_VALID |
		                    UBERTOOTH_FAULT_BUS_ADDR_VALID))
			fprintf(out, "the access was at 0x%08x\n", fault_addr);
		/* No access address, so the board recorded where the processor
		 * was instead. Zero is "nothing to report" here rather than an
		 * address: see fault_frame.h for the one fault that could name
		 * address zero and why it does not reach this line. */
		else if (fault_addr != 0)
			fprintf(out, "the instruction was at 0x%08x\n",
			        fault_addr);
		else
			fputs("no address recorded\n", out);
	}

	fprintf(out, "Capture transfer errors since power-on: %u\n",
	        util_le16(info + 8));
	/* Since the last boot, not since power-on: the firmware measures this
	 * from a pattern it writes over the unused stack at startup. */
	fprintf(out, "Most stack ever used since boot: %u of %u bytes\n",
	        util_le16(info + 18), util_le16(info + 20));
	return 0;
}

int util_report_radio_timeouts(FILE *out, const radio_timeouts *timeouts)
{
	if (timeouts == NULL)
		return 1;

	/* One line, in the order the reply carries them. A wait that gives up
	 * leaves the radio in the wrong state and the firmware carries on, so a
	 * count above zero is how a half-working radio tells itself apart from
	 * a quiet channel.
	 *
	 * "since boot", not since power-on as the watchdog counters above are:
	 * these live in ordinary RAM, which every boot clears, so a reset -- one
	 * the user asked for with -r, or one the watchdog made -- starts them
	 * again. */
	fprintf(out, "Radio waits that gave up since boot: "
	        "synthesizer lock %u, unlock %u, oscillator %u, "
	        "state machine %u, packet bytes %u\n",
	        (unsigned)timeouts->fs_lock, (unsigned)timeouts->fs_unlock,
	        (unsigned)timeouts->xosc, (unsigned)timeouts->fsm,
	        (unsigned)timeouts->rx_bytes);
	return 0;
}
