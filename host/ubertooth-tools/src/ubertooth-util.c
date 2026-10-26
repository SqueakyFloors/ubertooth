/*
 * Copyright 2010, 2011 Michael Ossmann
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ubertooth.h"
#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include "opt_parse.h"
#include "util_clock.h"
#include "util_report.h"
#include "util_modes.h"
#include "util_reset_wait.h"

/* Adapters from the injected report operations to the libubertooth commands. */
static int report_get_board_id(void *context)
{
	return cmd_get_board_id(((ubertooth_t*)context)->devh);
}

static int report_get_firmware_revision(void *context, char *revision, size_t size)
{
	return cmd_get_firmware_revision(((ubertooth_t*)context)->devh, revision, size);
}

static int report_get_api_version(void *context, uint16_t *version)
{
	return ubertooth_get_api((ubertooth_t*)context, version);
}

static int report_get_compile_info(void *context, char *info, size_t size)
{
	return cmd_get_firmware_compile_info(((ubertooth_t*)context)->devh, info, size);
}

static int report_get_part_id(void *context, uint32_t *part_id)
{
	return cmd_get_part_id(((ubertooth_t*)context)->devh, part_id);
}

static int report_get_serial(void *context, uint8_t serial[UTIL_SERIAL_BUFFER_LEN])
{
	return cmd_get_serial(((ubertooth_t*)context)->devh, serial);
}

static const struct util_report_ops report_ops = {
	report_get_board_id,
	report_get_firmware_revision,
	report_get_api_version,
	report_get_compile_info,
	report_get_part_id,
	report_get_serial,
};

/* Read-only identity queries that can be reported for every attached device. */
enum identity_query {
	QUERY_BOARD_ID,
	QUERY_FIRMWARE,
	QUERY_COMPILE_INFO,
	QUERY_PART_ID,
	QUERY_SERIAL,
	QUERY_COUNT
};

static const struct {
	const char *name;  /* shown as "<name read failed>" */
	const char *label; /* dropped when a single query is listed */
} identity_query_text[QUERY_COUNT] = {
	{ UTIL_QUERY_BOARD_ID, "Board ID Number: " },
	{ "firmware", "Firmware version: " },
	{ "compile info", "" },
	{ UTIL_QUERY_PART_ID, "Part ID: " },
	{ UTIL_QUERY_SERIAL, "Serial No: " },
};

/* Runs one identity report into buffers so its lines can be prefixed. */
static int capture_identity_report(enum identity_query query, ubertooth_t *ut,
                                   char **text, char **error)
{
	size_t size = 0, error_size = 0;
	int status = 1;
	FILE *out, *err;

	*text = NULL;
	*error = NULL;
	out = open_memstream(text, &size);
	err = open_memstream(error, &error_size);
	if (out == NULL || err == NULL) {
		if (out != NULL)
			fclose(out);
		if (err != NULL)
			fclose(err);
		return 1;
	}
	switch (query) {
	case QUERY_BOARD_ID:
		status = util_report_board_id(&report_ops, ut, out, err);
		break;
	case QUERY_FIRMWARE:
		status = util_report_firmware(&report_ops, ut, out);
		break;
	case QUERY_COMPILE_INFO:
		status = util_report_compile_info(&report_ops, ut, out);
		break;
	case QUERY_PART_ID:
		status = util_report_part_id(&report_ops, ut, out, err);
		break;
	case QUERY_SERIAL:
		status = util_report_serial(&report_ops, ut, out, err);
		break;
	default:
		break;
	}
	fclose(out);
	fclose(err);
	return status;
}

/*
 * Reports the requested identity queries for every device, in enumeration
 * order. A single query prints "<index>: <value>"; several queries print each
 * report line prefixed with "<index>: ". A device that cannot be opened or a
 * failed read is shown on its own line and makes the result 1, but the other
 * devices are still reported. Devices are closed without sending STOP, so a
 * query does not change whatever mode a device is running.
 */
static int list_identity_queries(const int requested[QUERY_COUNT], int count)
{
	int status = 0;
	int queries = 0;
	int i, q;

	for (q = 0; q < QUERY_COUNT; q++)
		queries += requested[q] ? 1 : 0;

	for (i = 0; i < count; i++) {
		ubertooth_t *uti = ubertooth_start(i);

		if (uti == NULL) {
			fprintf(stdout, "%d: <unavailable>\n", i);
			status = 1;
			continue;
		}
		for (q = 0; q < QUERY_COUNT; q++) {
			char *text, *error, *line, *next;
			const char *label = identity_query_text[q].label;

			if (!requested[q])
				continue;
			status = util_status_merge(status,
			        capture_identity_report((enum identity_query)q, uti,
			                                &text, &error));
			if (text == NULL || text[0] == '\0') {
				/* Show the report's own error (for example an unknown board
				 * ID) when it gave one, otherwise say the read failed. The
				 * report's "error: <query> read failed" line is shown in the
				 * listing's shorter "<query read failed>" form. */
				if (error != NULL && error[0] != '\0' &&
				    !util_is_read_failed_message(error,
				            identity_query_text[q].name)) {
					error[strcspn(error, "\n")] = '\0';
					fprintf(stdout, "%d: <%s>\n", i, error);
				} else {
					fprintf(stdout, "%d: <%s read failed>\n", i,
					        identity_query_text[q].name);
				}
				status = 1;
				free(text);
				free(error);
				continue;
			}
			free(error);
			for (line = text; *line != '\0'; line = next) {
				next = strchr(line, '\n');
				if (next != NULL)
					*next++ = '\0';
				else
					next = line + strlen(line);
				if (queries == 1 && label[0] != '\0' &&
				    strncmp(line, label, strlen(label)) == 0)
					line += strlen(label);
				fprintf(stdout, "%d: %s\n", i, line);
			}
			free(text);
		}
		ubertooth_close(uti);
		ubertooth_free(uti);
	}
	return status;
}

/*
 * A failed device read prints no value line, so a negative error code is
 * never shown as a value. libubertooth has already printed the transfer
 * error; this names the query that failed. Returns the failing status.
 */
_Static_assert(UTIL_ERROR_SHORT_REPLY == UBERTOOTH_ERROR_SHORT_REPLY,
               "util_report.h must match libubertooth's short-reply code");

/* "error: <what> read failed" for a failed read, unless code is a short
 * reply, whose whole line libubertooth already printed. Returns 1. */
static int report_read_failed(const char *what, int code)
{
	return util_report_read_error(stderr, what, code);
}

/* Adapters from the injected reset-wait operations to the clock and
 * libubertooth. The context is the -U argument (NULL for the default device). */
static uint64_t reset_wait_now_ms(void *context)
{
	(void)context;
	return util_clock_now_ms();
}

static void reset_wait_sleep_ms(void *context, unsigned ms)
{
	(void)context;
	util_clock_sleep_ms(ms);
}

static int reset_wait_count(void *context)
{
	(void)context;
	return ubertooth_count();
}

static void *reset_wait_try_select(void *context)
{
	return ubertooth_start_select_quiet((const char *)context);
}

static const struct util_reset_wait_ops reset_wait_ops = {
	reset_wait_now_ms,
	reset_wait_sleep_ms,
	reset_wait_count,
	reset_wait_try_select,
};

_Static_assert(UTIL_RESET_SERIAL_STR_LEN == UBERTOOTH_SERIAL_STR_LEN,
               "the followed serial is the library's serial string");

/*
 * Reads the serial of the board about to be reset, in the hex form -U matches,
 * so -r can follow that board and not the selector; see
 * util_reset_follow_init().
 *
 * The quiet read, because -r is the command used to recover a board that has
 * stopped answering: a transfer error and a full second of timeout here would
 * belong to a read the user never asked for. util_reset_follow_init() reports
 * a failed read, once, as the warning that matters.
 */
static int follow_get_serial(void *context, char *serial, size_t size)
{
	return ubertooth_read_serial_string(((ubertooth_t*)context)->devh, serial,
	                                    size, UBERTOOTH_SERIAL_READ_QUIET_MS);
}

static void follow_warn_no_serial(void *context, const char *u_arg)
{
	(void)context;
	fprintf(stderr, "warning: serial read failed on Ubertooth device %s before "
	        "the reset; later options may reach a different board if the "
	        "enumeration order changes\n", u_arg ? u_arg : "0");
}

static const struct util_reset_follow_ops follow_ops = {
	follow_get_serial,
	follow_warn_no_serial,
};

/*
 * -I needs this tree's firmware. ubertooth-util does not call
 * ubertooth_check_api() (it must still read the version of, and reflash, any
 * firmware), so -I checks the version itself, before it prints or sends
 * anything: firmware of another API version is refused with a message.
 *
 * Returns 0 if the firmware matches, or a negative error.
 */
static int identify_check_api(ubertooth_t* ut)
{
	uint16_t version = 0;
	int result = ubertooth_get_api(ut, &version);

	if (result < 0)
		return result;
	if (version != UBERTOOTH_API_VERSION) {
		fprintf(stderr, "error: -I needs firmware API version %x.%02x; the "
		        "board reports %x.%02x. Flash the firmware built from the "
		        "same source tree (./install.sh --flash-device).\n",
		        (UBERTOOTH_API_VERSION >> 8) & 0xFF, UBERTOOTH_API_VERSION & 0xFF,
		        (version >> 8) & 0xFF, version & 0xFF);
		return -1;
	}
	return 0;
}

static void usage(FILE *output)
{
	fprintf(output, "ubertooth-util - command line utility for Ubertooth Zero and Ubertooth One\n");
	fprintf(output, "\n");
	fprintf(output, "Common options:\n");
	fprintf(output, "\t-v get firmware revision number\n");
	fprintf(output, "\t-V get compile info\n");
	fprintf(output, "\t-H, --host-version get host tools (libubertooth) version\n");
	fprintf(output, "\t-I identify ubertooth device by flashing the USR, RX, and TX LEDs on the board, until -S or any mode option\n");
	fprintf(output, "\t-d [0-1] get/set all LEDs\n");
	fprintf(output, "\t-l [0-1] get/set USR LED\n");
	fprintf(output, "\t-S stop current operation\n");
	fprintf(output, "\t-r full reset\n");
	fputs(OPT_USAGE_SELECT_DEVICE, output);
	fprintf(output, "\t-U all list -b/-v/-V/-p/-s for every Ubertooth (queries only)\n");
	fprintf(output, "\t-N print the number of attached Ubertooths, then run the rest of the command\n");
	fprintf(output, "\n");
	fprintf(output, "Radio options:\n");
	fprintf(output, "\t-a [0-7] get/set power amplifier level\n");
	fprintf(output, "\t-c [%d-%d] get/set channel in MHz\n", BT_FREQ_MIN, BT_FREQ_MAX);
	fprintf(output, "\t-C [0-%d] get/set channel\n", NUM_BREDR_CHANNELS - 1);
	fprintf(output, "\t-q [%d-%d] start LED spectrum analyzer, threshold -N dBm (default: %d)\n",
	        UBERTOOTH_LED_SPECAN_N_MIN, UBERTOOTH_LED_SPECAN_N_MAX,
	        UBERTOOTH_LED_SPECAN_N_DEFAULT);
	fprintf(output, "\t   the threshold is -N dBm; a larger N lights the LEDs for weaker signals\n");
	fprintf(output, "\t-t intitiate continuous transmit test\n");
	fprintf(output, "\t-z [-128..127] get/set squelch level\n");
	fprintf(output, "\n");
	fprintf(output, "Range test:\n");
	fprintf(output, "\t-e start repeater mode\n");
	fprintf(output, "\t-m display range test result\n");
	fprintf(output, "\t-n initiate range test\n");
	fprintf(output, "\n");
	fprintf(output, "Miscellaneous:\n");
	fprintf(output, "\t-i activate In-System Programming (ISP) mode\n");
	fprintf(output, "\t-b get hardware board id number\n");
	fprintf(output, "\t-p get microcontroller Part ID\n");
	fprintf(output, "\t-s get microcontroller serial number\n");
	fprintf(output, "\t-x xmas lights\n");
}

int main(int argc, char *argv[])
{
	int opt;
	int other_operations = 0;   /* an option other than -s -v -V -p -b -U -N was given */
	/* An option other than -N and the -U selector was given. Where it is
	 * read, -h and -H have already ended the command, so what is left needs
	 * a device. */
	int operations_besides_count = 0;
	int status = 0; /* 0 on success, 1 if any operation failed; never a device value */
	ubertooth_t* ut = NULL;
	rangetest_result rr;
	int do_stop, do_isp, do_leds, do_part, do_reset;
	int do_serial, do_tx, do_palevel, do_channel, do_led_specan;
	int do_range_test, do_repeater, do_firmware, do_board_id;
	int do_range_result, do_all_leds, do_identify;
	/* A set and a read of the same value have their own flags, as the
	 * squelch pair below does, so one command can do both: the set runs
	 * first and the read then reports the new value. Sharing a variable
	 * made the later option overwrite the earlier one and drop it
	 * silently. */
	int do_get_leds, do_get_all_leds, do_get_channel, do_get_palevel;
	int do_set_squelch, do_get_squelch, squelch_level;
	int do_compile_info;
	int first_exit_option, long_host_version; /* -h / -H / --host-version */
	int do_number, do_xmas;
	char* ubertooth_device = NULL;
	int requested[QUERY_COUNT]; /* identity queries, for the per-device listing */
	int any_query, list_all;
	int count_only;             /* -N, with at most a selector beside it */
	int device_count = -1;      /* a successful ubertooth_count(), to reuse */
	int modes[UTIL_MODE_COUNT]; /* exclusive mode options that were given */

	/* The do_<thing> states below start negative: -1 means not asked for,
	 * 0 means 'do it' and a positive value is the argument given. The
	 * do_get_<thing> flags beside them are plain 0/1 booleans, since a read
	 * takes no value; do_set_squelch and do_get_squelch are the older pair,
	 * -1 or 1 and tested with > 0. */
	do_stop= do_isp= do_leds= do_part= do_reset= -1;
	do_serial= do_tx= do_palevel= do_channel= do_led_specan= -1;
	do_range_test= do_repeater= do_firmware= do_board_id= -1;
	do_range_result= do_all_leds= do_identify= -1;
	do_get_leds= do_get_all_leds= do_get_channel= do_get_palevel= 0;
	do_set_squelch= -1, do_get_squelch= -1; squelch_level= 0;
	do_compile_info= -1;
	do_number= 0; do_xmas= 0;
	first_exit_option= 0; long_host_version= 0;

	/* Host-tools version: printed before touching the device, so it works
	 * with no Ubertooth attached. -H or --host-version. Distinct from -v
	 * (firmware revision) and -V (firmware compile info from the device).
	 * getopt only knows the short form, so treat the long one as -H (but
	 * not after "--", where it is an operand and is reported as typed). */
	for (int ai = 1; ai < argc && strcmp(argv[ai], "--") != 0; ai++) {
		if (strcmp(argv[ai], "--host-version") == 0) {
			argv[ai] = (char *)"-H";
			long_host_version = 1;
		}
	}

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":U:hnmeiIprsStvbl::a::C::c::d::q::z::VNxH")) != EOF) {
		/* An optional value may also be the next word: -c 2440, -z -90. Only
		 * -z takes a negative value (a signed RSSI threshold). */
		if (optarg == NULL && strchr("lacCdqz", opt) != NULL)
			optarg = opt_separate_value(argc, argv,
			                            opt == 'z' ? opt_value_signed_decimal
			                                       : opt_value_decimal);
		/* Anything but an identity query, -U or -N needs a single selected
		 * device. -N opens none: it counts boards on the USB bus. */
		if (strchr("svVpbUN", opt) == NULL)
			other_operations = 1;
		/* Whether anything beyond the count was asked for. -N counts the
		 * boards on the USB bus and -U only says which board the rest of the
		 * command line acts on, so a command line of those two alone needs
		 * no device: -U on its own asks nothing of the board it selects. */
		if (opt != 'N' && opt != 'U')
			operations_besides_count = 1;
		switch(opt) {
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'i':
			do_isp= 0;
			break;
		case 'I':
			do_identify= 0;
			break;
		case 'l':
			if (optarg) {
				if (opt_require_int(optarg, 0, 1, "USR LED state", "", &do_leds) < 0)
					return 1;
			} else
				do_get_leds= 1;
			break;
		case 'd':
			if (optarg) {
				if (opt_require_int(optarg, 0, 1, "all-LED state", "", &do_all_leds) < 0)
					return 1;
			} else
				do_get_all_leds= 1;
			break;
		case 'p':
			do_part= 0;
			break;
		case 'r':
			do_reset= 0;
			break;
		case 's':
			do_serial= 0;
			break;
		case 'S':
			do_stop= 0;
			break;
		case 't':
			do_tx= 0;
			break;
		case 'a':
			/* -a reads the PA level; -a<0-7> sets it, including 0
			 * (-1: not requested). */
			if (optarg) {
				if (optarg[0] < '0' || optarg[0] > '7' || optarg[1] != '\0') {
					fprintf(stderr, "error: PA level must be a single digit 0-7 (got %s)\n", optarg);
					return 1;
				}
				do_palevel= optarg[0] - '0';
			} else {
				do_get_palevel= 1;
			}
			break;
		case 'C':
			if (optarg) {
				int ch;
				if (opt_require_int(optarg, 0, NUM_BREDR_CHANNELS - 1, "channel", "", &ch) < 0)
					return 1;
				do_channel= ch + BT_FREQ_MIN;
			} else
				do_get_channel= 1;
			break;
		case 'c':
			if (optarg) {
				if (opt_require_int(optarg, BT_FREQ_MIN, BT_FREQ_MAX, "frequency", " MHz", &do_channel) < 0)
					return 1;
			} else
				do_get_channel= 1;
			break;
		case 'q':
			if (optarg) {
				if (opt_require_int(optarg, UBERTOOTH_LED_SPECAN_N_MIN,
				                    UBERTOOTH_LED_SPECAN_N_MAX, "RSSI threshold",
				                    "", &do_led_specan) < 0)
					return 1;
			} else
				do_led_specan= 0; /* the default threshold */
			break;
		case 'n':
			do_range_test= 0;
			break;
		case 'm':
			do_range_result= 0;
			break;
		case 'e':
			do_repeater= 0;
			break;
		case 'v':
			do_firmware= 0;
			break;
		case 'b':
			do_board_id= 0;
			break;
		case 'z':
			if (optarg) {
				/* a signed 8-bit RSSI threshold; values above 0 are
				 * relative to the average RSSI */
				if (opt_require_int(optarg, -128, 127, "squelch level", "", &squelch_level) < 0)
					return 1;
				do_set_squelch = 1;
			}
			else {
				do_get_squelch = 1;
			}
			break;
		case 'V':
			do_compile_info = 0;
			break;
		case 'H':
			if (!first_exit_option)
				first_exit_option = 'H';
			break;
		case 'N':
			do_number = 1;
			break;
		case 'x':
			do_xmas = 1;
			break;
		case 'h':
			if (!first_exit_option)
				first_exit_option = 'h';
			break;
		default:
			opt_report_bad_option(opt, argc, argv);
			usage(stderr);
			return 1;
		}
	}

	/* ubertooth-util takes no operands. Refuse a leftover word before any
	 * device is opened: otherwise a typo such as "- V" (for -V) is dropped
	 * silently while the rest of the command still runs. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h and -H end the command before anything else, but only once the
	 * whole command line has parsed: an invalid option value or a stray
	 * word is reported even alongside them. As before, --host-version
	 * wins, and otherwise the first of -h and -H does. */
	if (long_host_version || first_exit_option == 'H') {
		print_version();
		return 0;
	}
	if (first_exit_option == 'h') {
		usage(stdout);
		return 0;
	}

	/* Only one mode can run: each one ends the command. Refuse a combination
	 * before any device is opened, instead of running whichever mode comes
	 * first below and silently ignoring the others. */
	modes[UTIL_MODE_LED_SPECAN] = (do_led_specan >= 0);
	modes[UTIL_MODE_RANGE_TEST] = (do_range_test == 0);
	modes[UTIL_MODE_REPEATER] = (do_repeater == 0);
	modes[UTIL_MODE_TX_TEST] = (do_tx == 0);
	modes[UTIL_MODE_XMAS] = do_xmas;
	modes[UTIL_MODE_IDENTIFY] = (do_identify == 0);
	modes[UTIL_MODE_ISP] = (do_isp == 0);
	if (util_check_modes(modes, stderr) != 0)
		return 1;
	/* The squelch is a firmware setting for later RX capture. A mode cannot
	 * use it: each mode first stops the device, and stopping a running mode
	 * resets the squelch to its default, and none of these modes reads it.
	 * Refuse the combination instead of silently dropping -z. */
	if (do_set_squelch > 0 || do_get_squelch > 0) {
		for (int m = 0; m < UTIL_MODE_COUNT; m++) {
			if (modes[m]) {
				fprintf(stderr, "error: -z cannot be combined with %s\n",
				        util_mode_option((enum util_mode)m));
				return 1;
			}
		}
	}

	requested[QUERY_BOARD_ID] = (do_board_id == 0);
	requested[QUERY_FIRMWARE] = (do_firmware == 0);
	requested[QUERY_COMPILE_INFO] = (do_compile_info == 0);
	requested[QUERY_PART_ID] = (do_part == 0);
	requested[QUERY_SERIAL] = (do_serial == 0);
	any_query = requested[QUERY_BOARD_ID] || requested[QUERY_FIRMWARE] ||
	            requested[QUERY_COMPILE_INFO] || requested[QUERY_PART_ID] ||
	            requested[QUERY_SERIAL];

	/* -U all lists the read-only identity queries for every attached device,
	 * so it is refused for anything that acts on one device: setters, modes
	 * and other reads. -N asks about the bus rather than a device, so it is
	 * allowed alongside the listing, and -N with nothing but -U all beside it
	 * asks only for the count: nothing is listed and nothing is refused, just
	 * as -U<index|serial> with -N alone counts and opens no board. Any other
	 * refusal comes before -N prints, so a refused command line writes
	 * nothing to standard output. */
	count_only = do_number && !operations_besides_count;
	list_all = (ubertooth_device != NULL && strcmp(ubertooth_device, "all") == 0);
	if (list_all && !count_only && (other_operations || !any_query)) {
		if (other_operations)
			fprintf(stderr, "error: -U all only supports the read-only queries "
			        "-b, -v, -V, -p and -s; select one device with "
			        "-U<index|serial> for other options\n");
		else
			/* Say what the listing is missing: naming the options -U all does
			 * not support describes a command line the user did not type. */
			fprintf(stderr, "error: -U all needs one of the read-only queries "
			        "-b, -v, -V, -p or -s to list\n");
		return 1;
	}

	/* A command line that asks for nothing is a mistake, not a no-op:
	 * without this it would open the selected device, do nothing with it and
	 * exit 0. -N is an operation in its own right, and -h, -H and
	 * --host-version have already ended the command above. */
	if (!do_number && !operations_besides_count) {
		/* The selector is quoted, as every other message quoting the user's
		 * input is: an empty -U value would otherwise read as no value. */
		if (ubertooth_device != NULL)
			fprintf(stderr, "error: nothing to do: -U '%s' selects a device "
			        "but no option asks anything of it\n", ubertooth_device);
		else
			fprintf(stderr, "error: nothing to do: give an option that reads "
			        "or sets something, or -N to count the attached boards\n");
		usage(stderr);
		return 1;
	}

	/* -N counts the Ubertooths on the USB bus and opens none of them, so it
	 * has nothing to drop: the count goes first and the rest of the command
	 * line still runs. Flushed so it really is first when standard output
	 * and standard error share a file. */
	if (do_number) {
		device_count = ubertooth_count();
		if (device_count < 0)
			status = util_status_merge(status, 1);
		else
			printf("%d\n", device_count);
		fflush(stdout);
		/* -N, with at most a selector beside it, asks for nothing that needs
		 * a device. */
		if (!operations_besides_count)
			return status;
	}

	/* The listings take the count -N already has rather than enumerate
	 * again: a board unplugged between the two would make the printed
	 * number disagree with the listing under it. */
	if (list_all) {
		int count = device_count >= 0 ? device_count : ubertooth_count();

		if (count < 0)
			return util_status_merge(status, 1);
		if (count == 0) {
			fprintf(stderr, "error: no Ubertooth devices found\n");
			return util_status_merge(status, 1);
		}
		return util_status_merge(status,
		        list_identity_queries(requested, count));
	}

	/* Read-only identity queries (-b, -v, -V, -p, -s) with no -U and several
	 * devices: report every device instead of refusing to pick one. Any other
	 * operation in the same command still needs -U. */
	if (ubertooth_device == NULL && !other_operations && any_query) {
		int count = device_count >= 0 ? device_count : ubertooth_count();

		if (count > 1)
			return util_status_merge(status,
			        list_identity_queries(requested, count));
	}

	/* initialise device */
	ut = ubertooth_start_select(ubertooth_device);
	if (ut == NULL) {
		return 1;
	}
	if(do_reset == 0) {
		struct util_reset_wait_params wait = {
			UTIL_RESET_WAIT_INITIAL_DELAY_MS, UTIL_RESET_WAIT_POLL_MS,
			UTIL_RESET_WAIT_LEAVE_MS, UTIL_RESET_WAIT_DEADLINE_MS, 0, 0
		};
		struct util_reset_follow follow;
		int libusb_held;

		/* Counted before the reset, so the wait can see the device leave and,
		 * where the selection is by index, not open another board while it is
		 * gone. The leave wait is kept even if the reset command reports an
		 * error, since the device may have reset anyway. */
		wait.count_before = ubertooth_count();
		/* Read while the device is still open: the wait then looks for this
		 * board's serial, so later options in the same command cannot land on
		 * another board whose index changed across the reset. */
		util_reset_follow_init(&follow, &follow_ops, ut, ubertooth_device,
		                       wait.count_before);
		wait.select_needs_count = follow.select_needs_count;
		fprintf(stderr, "Resetting ubertooth device %s\n", ubertooth_device ? ubertooth_device : "0");
		status = util_status_after(status, cmd_reset(ut->devh));
		/* The device re-enumerates after a reset, so this handle is about
		 * to go dead: release it without sending STOP (if the reset failed
		 * the device is still there, but a STOP is not wanted either), then
		 * wait for the same selection to open again. The polls are quiet
		 * and never send STOP.
		 *
		 * One libusb reference is held for the whole wait, so the polls never
		 * tear down the libusb context while the device re-enumerates (on
		 * macOS the final libusb_exit() after a device reset can hang). */
		libusb_held = libusb_init(NULL) >= 0;
		ubertooth_close(ut);
		ubertooth_free(ut);
		/* cast away const: the context is only read, as a selector string */
		ut = util_reset_wait(&reset_wait_ops, (void *)follow.selector, &wait);
		if (ut == NULL) {
			/* The held reference is left to process exit, for the same
			 * reason. */
			fprintf(stderr, "error: Ubertooth device %s did not come back within "
			        "%u s after the reset\n",
			        ubertooth_device ? ubertooth_device : "0",
			        UTIL_RESET_WAIT_DEADLINE_MS / 1000u);
			return 1;
		}
		/* Not the last reference: the reopened device holds its own. */
		if (libusb_held)
			libusb_exit(NULL);
	}
	if(do_stop == 0) {
		fprintf(stderr, "Stopping ubertooth device %s\n", ubertooth_device ? ubertooth_device : "0");
		status = util_status_after(status, cmd_stop(ut->devh));
	}

	/* device configuration actions */
	if(do_all_leds == 0 || do_all_leds == 1) {
		status = util_status_after(status, cmd_set_usrled(ut->devh, do_all_leds));
		status = util_status_after(status, cmd_set_rxled(ut->devh, do_all_leds));
		status = util_status_after(status, cmd_set_txled(ut->devh, do_all_leds));
	}
	if(do_channel > 0)
		status = util_status_after(status, cmd_set_channel(ut->devh, do_channel));
	if(do_leds == 0 || do_leds == 1)
		status = util_status_after(status, cmd_set_usrled(ut->devh, do_leds));
	if(do_palevel >= 0)
		status = util_status_after(status, cmd_set_palevel(ut->devh, do_palevel));

	/* reporting actions, in the order ubertooth-util has always printed them */
	if(do_get_all_leds) {
		int usr = cmd_get_usrled(ut->devh);
		int rx = cmd_get_rxled(ut->devh);
		int tx = cmd_get_txled(ut->devh);
		if (usr < 0)
			status = util_status_merge(status, report_read_failed("USR LED", usr));
		else
			fprintf(stdout, "USR LED status: %d\n", usr);
		if (rx < 0)
			status = util_status_merge(status, report_read_failed("RX LED", rx));
		else
			fprintf(stdout, "RX LED status : %d\n", rx);
		if (tx < 0)
			status = util_status_merge(status, report_read_failed("TX LED", tx));
		else
			fprintf(stdout, "TX LED status : %d\n", tx);
	}
	if(do_board_id == 0)
		status = util_status_merge(status,
		        util_report_board_id(&report_ops, ut, stdout, stderr));
	if(do_get_channel) {
		int mhz = cmd_get_channel(ut->devh);
		if (mhz < 0)
			status = util_status_merge(status, report_read_failed("channel", mhz));
		else
			fprintf(stdout, "Current frequency: %d MHz (Bluetooth channel %d)\n", mhz, mhz - BT_FREQ_MIN);
	}
	if(do_firmware == 0)
		status = util_status_merge(status,
		        util_report_firmware(&report_ops, ut, stdout));
	if(do_compile_info == 0)
		status = util_status_merge(status,
		        util_report_compile_info(&report_ops, ut, stdout));
	if(do_get_leds) {
		int led = cmd_get_usrled(ut->devh);
		if (led < 0)
			status = util_status_merge(status, report_read_failed("USR LED", led));
		else
			fprintf(stdout, "USR LED status: %d\n", led);
	}
	if(do_get_palevel) {
		int level = cmd_get_palevel(ut->devh);
		if (level < 0)
			status = util_status_merge(status, report_read_failed("PA level", level));
		else
			fprintf(stdout, "PA Level: %d\n", level);
	}
	if(do_part == 0)
		status = util_status_merge(status,
		        util_report_part_id(&report_ops, ut, stdout, stderr));
	if(do_range_result == 0) {
		int rr_status = cmd_get_rangeresult(ut->devh, &rr);
		if (rr_status != 0) {
			status = util_status_merge(status, report_read_failed("range test result", rr_status));
		} else {
			if (rr.valid==1) {
				fprintf(stdout, "request PA level : %d\n", rr.request_pa);
				fprintf(stdout, "request number   : %d\n", rr.request_num);
				fprintf(stdout, "reply PA level   : %d\n", rr.reply_pa);
				fprintf(stdout, "reply number     : %d\n", rr.reply_num);
			} else if (rr.valid>1) {
				/* The test ran but the data came back corrupted: a
				 * diagnostic rather than a result, so it is not a value a
				 * script can read off standard output, and the command
				 * failed. */
				fprintf(stderr, "error: invalid range test: mismatch on byte %d\n",
				        rr.valid-2);
				status = util_status_merge(status, 1);
			} else {
				/* valid is 0 until a range test completes once: there is
				 * no result yet, which is not an error. */
				fprintf(stdout, "no range test result yet\n");
			}
		}
	}

	if(do_serial == 0)
		status = util_status_merge(status,
		        util_report_serial(&report_ops, ut, stdout, stderr));

	/* final actions */
	if(do_identify == 0) {
		/* The blink runs on the board as its own mode, so the command returns
		 * at once; -S or any mode-starting command ends it. */
		int r = identify_check_api(ut);

		if (r < 0) {
			status = util_status_after(status, r);
			goto out;
		}
		/* Stop any operation already running on the device (e.g. -x xmas or
		 * -q LED spectrum analyzer) so it does not fight the identify blink. */
		status = util_status_after(status, cmd_stop(ut->devh));
		r = cmd_identify(ut->devh);
		status = util_status_after(status, r);
		/* One line, not a "flashing the LEDs" line followed by this one
		 * saying the same thing: the blink is reported once, as the command
		 * that ends it, repeating the device selector so it can be pasted. */
		if (r >= 0)
			fprintf(stderr, "note: the board is blinking; stop it with "
			        "ubertooth-util%s%s -S or any mode option\n",
			        ubertooth_device ? " -U " : "",
			        ubertooth_device ? ubertooth_device : "");
		goto out;
	}
	if(do_isp == 0) {
		fprintf(stderr, "Entering flash programming (ISP) mode\n");
		status = util_status_after(status, cmd_set_isp(ut->devh));
		goto out;
	}
	if(do_led_specan >= 0) {
		do_led_specan= do_led_specan ? do_led_specan : UBERTOOTH_LED_SPECAN_N_DEFAULT;
		fprintf(stderr, "Entering LED specan mode (threshold -%d dBm)\n", do_led_specan);
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		status = util_status_after(status, cmd_led_specan(ut->devh, do_led_specan));
		goto out;
	}
	if(do_range_test == 0) {
		fprintf(stderr, "Starting range test\n");
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		status = util_status_after(status, cmd_range_test(ut->devh));
		goto out;
	}
	if(do_repeater == 0) {
		fprintf(stderr, "Starting repeater\n");
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		status = util_status_after(status, cmd_repeater(ut->devh));
		goto out;
	}
	if(do_tx == 0) {
		fprintf(stderr, "Starting TX test\n");
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		status = util_status_after(status, cmd_tx_test(ut->devh));
		goto out;
	}
	if(do_set_squelch > 0) {
		/* Silent, like every other setter here (-a, -c, -C, -d, -l): a
		 * "setting squelch" line only repeats what the -z read-back prints
		 * as the result, and a failed set reports itself. */
		status = util_status_after(status, cmd_set_squelch(ut->devh, squelch_level));
	}
	if(do_get_squelch > 0) {
		int squelch = cmd_get_squelch(ut->devh);
		if (squelch < 0)
			status = util_status_merge(status, report_read_failed("squelch", squelch));
		else
			fprintf(stdout, "Squelch set to %d\n", (int8_t)squelch);
	}
	if(do_xmas) {
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		status = util_status_after(status, cmd_xmas(ut->devh));
		goto out;
	}

out:
	/* Release the handle without sending STOP, so a query or a mode just
	 * started is left running on the device. */
	ubertooth_close(ut);
	ubertooth_free(ut);
	return status;
}
