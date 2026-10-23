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

static void report_get_firmware_revision(void *context, char *revision, size_t size)
{
	cmd_get_rev_num(((ubertooth_t*)context)->devh, revision, (u8)size);
}

static int report_get_api_version(void *context, uint16_t *version)
{
	return ubertooth_get_api((ubertooth_t*)context, version);
}

static void report_get_compile_info(void *context, char *info, size_t size)
{
	cmd_get_compile_info(((ubertooth_t*)context)->devh, info, (u8)size);
}

static int report_get_part_id(void *context)
{
	return cmd_get_partnum(((ubertooth_t*)context)->devh);
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

#define MAX_VERSION_STRING_LEN 255

int main(int argc, char *argv[])
{
	int opt;
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
	char version_string[MAX_VERSION_STRING_LEN];

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

	if (do_number) {
		int count = ubertooth_count();
		if (count < 0)
			return 1;
		printf("%d\n", count);
		return 0;
	}

	/* -s with no -U and several devices: list every device's serial, in
	 * enumeration order, instead of erroring. -U <x> -s still targets one. */
	if (do_serial == 0 && ubertooth_device == NULL) {
		int count = ubertooth_count();
		if (count > 1) {
			int i;
			for (i = 0; i < count; i++) {
				ubertooth_t* uti = ubertooth_start(i);
				if (uti == NULL) {
					fprintf(stdout, "%d: <unavailable>\n", i);
					continue;
				}
				u8 serial[17];
				if (cmd_get_serial(uti->devh, serial) == 0) {
					int j;
					fprintf(stdout, "%d: ", i);
					for (j = 1; j < 17; j++)
						fprintf(stdout, "%02x", serial[j]);
					fprintf(stdout, "\n");
				} else {
					fprintf(stdout, "%d: <serial read failed>\n", i);
				}
				ubertooth_stop(uti);
			}
			return 0;
		}
	}

	/* -v with no -U and several devices: list every device's firmware version,
	 * in enumeration order, instead of erroring. -U <x> -v still targets one. */
	if (do_firmware == 0 && ubertooth_device == NULL) {
		int count = ubertooth_count();
		if (count > 1) {
			int i;
			for (i = 0; i < count; i++) {
				ubertooth_t* uti = ubertooth_start(i);
				if (uti == NULL) {
					fprintf(stdout, "%d: <unavailable>\n", i);
					continue;
				}
				uint16_t usb_version;
				fprintf(stdout, "%d: ", i);
				cmd_get_rev_num(uti->devh, version_string, MAX_VERSION_STRING_LEN);
				fprintf(stdout, "%s", version_string);
				if (ubertooth_get_api(uti, &usb_version) >= 0)
					fprintf(stdout, " (API:%x.%02x)\n", (usb_version>>8)&0xFF, usb_version&0xFF);
				else
					fprintf(stdout, "\n");
				ubertooth_stop(uti);
			}
			return 0;
		}
	}

	/* initialise device */
	ut = ubertooth_start_select(ubertooth_device);
	if (ut == NULL) {
		return 1;
	}
	if(do_reset == 0) {
		fprintf(stdout, "Resetting ubertooth device %s\n", ubertooth_device ? ubertooth_device : "0");
		status = util_status_after(status, cmd_reset(ut->devh));
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
		fprintf(stdout, "USR LED status: %d\n", usr);
		fprintf(stdout, "RX LED status : %d\n", rx);
		fprintf(stdout, "TX LED status : %d\n", tx);
		status = util_status_after(status, usr);
		status = util_status_after(status, rx);
		status = util_status_after(status, tx);
	}
	if(do_board_id == 0)
		status = util_status_merge(status,
		        util_report_board_id(&report_ops, ut, stdout, stderr));
	if(do_channel == 0) {
		int mhz = cmd_get_channel(ut->devh);
		fprintf(stdout, "Current frequency: %d MHz (Bluetooth channel %d)\n", mhz, mhz - 2402);
		status = util_status_after(status, mhz);
	}
	if(do_firmware == 0)
		status = util_status_merge(status,
		        util_report_firmware(&report_ops, ut, stdout));
	if(do_compile_info == 0)
		status = util_status_merge(status,
		        util_report_compile_info(&report_ops, ut, stdout));
	if(do_leds == 2) {
		int led = cmd_get_usrled(ut->devh);
		fprintf(stdout, "USR LED status: %d\n", led);
		status = util_status_after(status, led);
	}
	if(do_palevel == 0) {
		int level = cmd_get_palevel(ut->devh);
		fprintf(stdout, "PA Level: %d\n", level);
		status = util_status_after(status, level);
	}
	if(do_part == 0)
		status = util_status_merge(status,
		        util_report_part_id(&report_ops, ut, stdout));
	if(do_range_result == 0) {
		int rr_status = cmd_get_rangeresult(ut->devh, &rr);
		status = util_status_merge(status, rr_status != 0);
		if (rr_status == 0) {
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
		fprintf(stdout, "Squelch set to %d\n", (int8_t)squelch);
		status = util_status_after(status, squelch);
	}
	if(do_xmas) {
		status = util_status_after(status, cmd_stop(ut->devh)); /* leave any prior mode before starting */
		return util_status_after(status, cmd_xmas(ut->devh));
	}
	if(do_something) {
		unsigned char buf[4] = { 0x55, 0x55, 0x55, 0x55 };
		cmd_do_something(ut->devh, NULL, 0);
		cmd_do_something_reply(ut->devh, buf, 4);
		fprintf(stdout, "%02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3]);
		return status;
	}

	return status;
}
