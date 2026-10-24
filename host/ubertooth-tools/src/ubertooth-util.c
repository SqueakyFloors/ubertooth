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
#include <signal.h>
#include "util_report.h"

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
	{ "board ID", "Board ID Number: " },
	{ "firmware", "Firmware version: " },
	{ "compile info", "" },
	{ "part ID", "Part ID: " },
	{ "serial", "Serial No: " },
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
		status = util_report_part_id(&report_ops, ut, out);
		break;
	case QUERY_SERIAL:
		status = util_report_serial(&report_ops, ut, out);
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
				 * ID) when it gave one, otherwise say the read failed. */
				if (error != NULL && error[0] != '\0') {
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
static int report_read_failed(const char *what)
{
	fprintf(stderr, "error: %s read failed\n", what);
	return 1;
}

/* For -I (identify): the loop runs until interrupted. Without a handler, Ctrl-C
 * leaves the USR/RX/TX LEDs in whatever state the last toggle set (often on).
 * This handler turns them off and exits cleanly so the device returns to the
 * same LED-off state as after a reset. */
static ubertooth_t* identify_ut = NULL;
static void identify_cleanup(int sig __attribute__((unused)))
{
	if (identify_ut && identify_ut->devh) {
		cmd_set_usrled(identify_ut->devh, 0);
		cmd_set_rxled(identify_ut->devh, 0);
		cmd_set_txled(identify_ut->devh, 0);
		cmd_stop(identify_ut->devh);
	}
	exit(0);
}

static void usage(FILE *output)
{
	fprintf(output, "ubertooth-util - command line utility for Ubertooth Zero and Ubertooth One\n");
	fprintf(output, "\n");
	fprintf(output, "Common options:\n");
	fprintf(output, "\t-v get firmware revision number\n");
	fprintf(output, "\t-V get compile info\n");
	fprintf(output, "\t-H, --host-version get host tools (libubertooth) version\n");
	fprintf(output, "\t-I identify ubertooth device by flashing the USR, RX, and TX LEDs\n");
	fprintf(output, "\t-d[0-1] get/set all LEDs\n");
	fprintf(output, "\t-l[0-1] get/set USR LED\n");
	fprintf(output, "\t-S stop current operation\n");
	fprintf(output, "\t-r full reset\n");
	fprintf(output, "\t-U<index|serial> select Ubertooth by index or serial number\n");
	fprintf(output, "\t-U all list -b/-v/-V/-p/-s for every Ubertooth (queries only)\n");
	fprintf(output, "\t-N print total number of Ubertooth devices and exit\n");
	fprintf(output, "\n");
	fprintf(output, "Radio options:\n");
	fprintf(output, "\t-a[0-7] get/set power amplifier level\n");
	fprintf(output, "\t-c[2400-2483] get/set channel in MHz\n");
	fprintf(output, "\t-C[0-78] get/set channel\n");
	fprintf(output, "\t-q[1-225 (RSSI threshold)] start LED spectrum analyzer\n");
	fprintf(output, "\t-t intitiate continuous transmit test\n");
	fprintf(output, "\t-z set squelch level\n");
	fprintf(output, "\n");
	fprintf(output, "Range test:\n");
	fprintf(output, "\t-e start repeater mode\n");
	fprintf(output, "\t-m display range test result\n");
	fprintf(output, "\t-n initiate range test\n");
	fprintf(output, "\n");
	fprintf(output, "Miscellaneous:\n");
	fprintf(output, "\t-f activate flash programming (DFU) mode\n");
	fprintf(output, "\t-i activate In-System Programming (ISP) mode\n");
	fprintf(output, "\t-b get hardware board id number\n");
	fprintf(output, "\t-p get microcontroller Part ID\n");
	fprintf(output, "\t-s get microcontroller serial number\n");
	fprintf(output, "\t-x xmas lights\n");
}

int main(int argc, char *argv[])
{
	int opt;
	int other_operations = 0; /* an option other than -s -v -V -p -b -U was given */
	int status = 0; /* 0 on success, 1 if any operation failed; never a device value */
	ubertooth_t* ut = NULL;
	rangetest_result rr;
	int do_stop, do_flash, do_isp, do_leds, do_part, do_reset;
	int do_serial, do_tx, do_palevel, do_channel, do_led_specan;
	int do_range_test, do_repeater, do_firmware, do_board_id;
	int do_range_result, do_all_leds, do_identify;
	int do_set_squelch, do_get_squelch, squelch_level;
	int do_something, do_compile_info;
	int do_number, do_xmas;
	char* ubertooth_device = NULL;
	int requested[QUERY_COUNT]; /* identity queries, for the per-device listing */
	int any_query;

	/* set command states to negative as a starter
	 * setting to 0 means 'do it'
	 * setting to positive is value of specified argument */
	do_stop= do_flash= do_isp= do_leds= do_part= do_reset= -1;
	do_serial= do_tx= do_palevel= do_channel= do_led_specan= -1;
	do_range_test= do_repeater= do_firmware= do_board_id= -1;
	do_range_result= do_all_leds= do_identify= -1;
	do_set_squelch= -1, do_get_squelch= -1; squelch_level= 0;
	do_something= 0; do_compile_info= -1;
	do_number= 0; do_xmas= 0;

	/* Host-tools version: print and exit before touching the device, so it
	 * works with no Ubertooth attached. -H or --host-version. Distinct from
	 * -v (firmware revision) and -V (firmware compile info from the device). */
	for (int ai = 1; ai < argc; ai++) {
		if (strcmp(argv[ai], "--host-version") == 0) {
			print_version();
			return 0;
		}
	}

	while ((opt=getopt(argc,argv,"U:hnmefiIprsStvbl::a::C::c::d::q::z::9VNxH")) != EOF) {
		/* Anything but an identity query or -U needs a single selected device. */
		if (strchr("svVpbU", opt) == NULL)
			other_operations = 1;
		switch(opt) {
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'f':
			fprintf(stderr, "ubertooth-util -f is no longer required - use ubertooth-dfu instead\n");
			do_flash= 0;
			break;
		case 'i':
			do_isp= 0;
			break;
		case 'I':
			do_identify= 0;
			break;
		case 'l':
			if (optarg)
				do_leds= atoi(optarg);
			else
				do_leds= 2; /* can't use 0 as it's a valid option */
			break;
		case 'd':
			if (optarg)
				do_all_leds= atoi(optarg);
			else
				do_all_leds= 2; /* can't use 0 as it's a valid option */
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
			if (optarg)
				do_palevel= atoi(optarg);
			else
				do_palevel= 0;
			break;
		case 'C':
			if (optarg) {
				int ch = atoi(optarg);
				if (ch < 0 || ch > 78) {
					fprintf(stderr, "error: channel must be 0-78 (got %s)\n", optarg);
					return 1;
				}
				do_channel= ch + 2402;
			} else
				do_channel= 0;
			break;
		case 'c':
			if (optarg) {
				int mhz = atoi(optarg);
				if (mhz < 2402 || mhz > 2480) {
					fprintf(stderr, "error: frequency must be 2402-2480 MHz (got %s)\n", optarg);
					return 1;
				}
				do_channel= mhz;
			} else
				do_channel= 0;
			break;
		case 'q':
			if (optarg)
				do_led_specan= atoi(optarg);
			else
				do_led_specan= 0;
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
				squelch_level = atoi(optarg);
				do_set_squelch = 1;
			}
			else {
				do_get_squelch = 1;
			}
			break;
		case '9':
			do_something= 1;
			break;
		case 'V':
			do_compile_info = 0;
			break;
		case 'H':
			print_version();
			return 0;
		case 'N':
			do_number = 1;
			break;
		case 'x':
			do_xmas = 1;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 1;
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

	/* -U all lists the read-only identity queries for every attached device.
	 * Refuse it, before any device is opened, for anything else: setters,
	 * modes and other reads act on one device only. */
	if (ubertooth_device != NULL && strcmp(ubertooth_device, "all") == 0) {
		int count;

		if (other_operations || !any_query) {
			fprintf(stderr, "error: -U all only supports the read-only queries "
			        "-b, -v, -V, -p and -s; select one device with -U<index|serial> "
			        "for other options\n");
			return 1;
		}
		count = ubertooth_count();
		if (count < 0)
			return 1;
		if (count == 0) {
			fprintf(stderr, "error: no Ubertooth devices found\n");
			return 1;
		}
		return list_identity_queries(requested, count);
	}

	if (do_number) {
		int count = ubertooth_count();
		if (count < 0)
			return 1;
		printf("%d\n", count);
		return 0;
	}

	/* Read-only identity queries (-b, -v, -V, -p, -s) with no -U and several
	 * devices: report every device instead of refusing to pick one. Any other
	 * operation in the same command still needs -U. */
	if (ubertooth_device == NULL && !other_operations && any_query) {
		int count = ubertooth_count();
		if (count > 1)
			return list_identity_queries(requested, count);
	}

	/* initialise device */
	ut = ubertooth_start_select(ubertooth_device);
	if (ut == NULL) {
		return 1;
	}
	if(do_reset == 0) {
		fprintf(stdout, "Resetting ubertooth device %s\n", ubertooth_device ? ubertooth_device : "0");
		status = util_status_after(status, cmd_reset(ut->devh));
		/* The device re-enumerates after a reset, so this handle is about
		 * to go dead: release it without sending STOP (if the reset failed
		 * the device is still there, but a STOP is not wanted either), then
		 * open the device again. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		ut = NULL;
		sleep(2);
		ut = ubertooth_start_select(ubertooth_device);
		if (ut == NULL)
			return 1;
	}
	if(do_stop == 0) {
		fprintf(stdout, "Stopping ubertooth device %s\n", ubertooth_device ? ubertooth_device : "0");
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
	if(do_palevel > 0)
		status = util_status_after(status, cmd_set_palevel(ut->devh, do_palevel));

	/* reporting actions, in the order ubertooth-util has always printed them */
	if(do_all_leds == 2) {
		int usr = cmd_get_usrled(ut->devh);
		int rx = cmd_get_rxled(ut->devh);
		int tx = cmd_get_txled(ut->devh);
		if (usr < 0)
			status = util_status_merge(status, report_read_failed("USR LED"));
		else
			fprintf(stdout, "USR LED status: %d\n", usr);
		if (rx < 0)
			status = util_status_merge(status, report_read_failed("RX LED"));
		else
			fprintf(stdout, "RX LED status : %d\n", rx);
		if (tx < 0)
			status = util_status_merge(status, report_read_failed("TX LED"));
		else
			fprintf(stdout, "TX LED status : %d\n", tx);
	}
	if(do_board_id == 0)
		status = util_status_merge(status,
		        util_report_board_id(&report_ops, ut, stdout, stderr));
	if(do_channel == 0) {
		int mhz = cmd_get_channel(ut->devh);
		if (mhz < 0)
			status = util_status_merge(status, report_read_failed("channel"));
		else
			fprintf(stdout, "Current frequency: %d MHz (Bluetooth channel %d)\n", mhz, mhz - 2402);
	}
	if(do_firmware == 0)
		status = util_status_merge(status,
		        util_report_firmware(&report_ops, ut, stdout));
	if(do_compile_info == 0)
		status = util_status_merge(status,
		        util_report_compile_info(&report_ops, ut, stdout));
	if(do_leds == 2) {
		int led = cmd_get_usrled(ut->devh);
		if (led < 0)
			status = util_status_merge(status, report_read_failed("USR LED"));
		else
			fprintf(stdout, "USR LED status: %d\n", led);
	}
	if(do_palevel == 0) {
		int level = cmd_get_palevel(ut->devh);
		if (level < 0)
			status = util_status_merge(status, report_read_failed("PA level"));
		else
			fprintf(stdout, "PA Level: %d\n", level);
	}
	if(do_part == 0)
		status = util_status_merge(status,
		        util_report_part_id(&report_ops, ut, stdout));
	if(do_range_result == 0) {
		int rr_status = cmd_get_rangeresult(ut->devh, &rr);
		if (rr_status != 0) {
			status = util_status_merge(status, report_read_failed("range test result"));
		} else {
			if (rr.valid==1) {
				fprintf(stdout, "request PA level : %d\n", rr.request_pa);
				fprintf(stdout, "request number   : %d\n", rr.request_num);
				fprintf(stdout, "reply PA level   : %d\n", rr.reply_pa);
				fprintf(stdout, "reply number     : %d\n", rr.reply_num);
			} else if (rr.valid>1) {
				fprintf(stdout, "Invalid range test: mismatch on byte %d\n", rr.valid-2);
			} else {
				fprintf(stdout, "invalid range test result\n");
			}
		}
	}

	if(do_serial == 0)
		status = util_status_merge(status,
		        util_report_serial(&report_ops, ut, stdout));

	/* final actions */
	if(do_flash == 0) {
		fprintf(stdout, "Entering flash programming (DFU) mode\n");
		return util_status_after(status, cmd_flash(ut->devh));
	}
	if(do_identify == 0) {
		fprintf(stdout, "Flashing LEDs on ubertooth device %s\n", ubertooth_device ? ubertooth_device : "0");
		/* Stop any operation already running on the device (e.g. -x xmas or
		 * -q LED spectrum analyzer) so it does not fight the identify blink
		 * and is not left running after identify is interrupted. */
		status = util_status_after(status, cmd_stop(ut->devh));
		identify_ut = ut;
		signal(SIGINT, identify_cleanup);
		while(42) {
			do_identify= !do_identify;
			cmd_set_usrled(ut->devh, do_identify);
			cmd_set_rxled(ut->devh, do_identify);
			cmd_set_txled(ut->devh, do_identify);
			usleep(166667); /* ~3 Hz blink for a snappier identify */
		}
	}
	if(do_isp == 0) {
		fprintf(stdout, "Entering flash programming (ISP) mode\n");
		return util_status_after(status, cmd_set_isp(ut->devh));
	}
	if(do_led_specan >= 0) {
		do_led_specan= do_led_specan ? do_led_specan : 225;
		fprintf(stdout, "Entering LED specan mode (RSSI %d)\n", do_led_specan);
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		return util_status_after(status, cmd_led_specan(ut->devh, do_led_specan));
	}
	if(do_range_test == 0) {
		fprintf(stdout, "Starting range test\n");
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		return util_status_after(status, cmd_range_test(ut->devh));
	}
	if(do_repeater == 0) {
		fprintf(stdout, "Starting repeater\n");
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		return util_status_after(status, cmd_repeater(ut->devh));
	}
	if(do_tx == 0) {
		fprintf(stdout, "Starting TX test\n");
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		return util_status_after(status, cmd_tx_test(ut->devh));
	}
	if(do_set_squelch > 0) {
		fprintf(stdout, "Setting squelch to %d\n", squelch_level);
		status = util_status_after(status, cmd_set_squelch(ut->devh, squelch_level));
	}
	if(do_get_squelch > 0) {
		int squelch = cmd_get_squelch(ut->devh);
		if (squelch < 0)
			status = util_status_merge(status, report_read_failed("squelch"));
		else
			fprintf(stdout, "Squelch set to %d\n", (int8_t)squelch);
	}
	if(do_xmas) {
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		return util_status_after(status, cmd_xmas(ut->devh));
	}
	if(do_something) {
		unsigned char buf[4] = { 0x55, 0x55, 0x55, 0x55 };
		/* Bytes the firmware does not fill keep the 0x55 marker. */
		if (cmd_do_something(ut->devh, NULL, 0) < 0) {
			fprintf(stderr, "error: test command send failed\n");
			status = 1;
		}
		if (cmd_do_something_reply(ut->devh, buf, 4) < 0)
			status = util_status_merge(status, report_read_failed("test command reply"));
		else
			fprintf(stdout, "%02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3]);
		return status;
	}

	return status;
}
