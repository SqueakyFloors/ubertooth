/*
 * Copyright 2015 Mike Ryan
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
#include "ubertooth_callback.h"
#include "ego_options.h"
#include "opt_parse.h"
#include <ctype.h>
#include <err.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

ubertooth_t* ut = NULL;
int running = 1;

void quit(int signo __attribute__((unused)))
{
	running = 0;
}

static void usage(void)
{
	printf("ubertooth-ego - Yuneec E-GO skateboard sniffing\n");
	printf("Usage:\n");
	printf("\t-h this help\n");
	printf("\n");
	printf("    Major modes:\n");
	printf("\t-f follow connections\n");
	printf("\t-r continuous rx on a single channel\n");
	printf("\t-i interfere\n");
	printf("\n");
	printf("    Options:\n");
	printf("\t-c <%d-%d> set channel in MHz (for continuous rx)\n",
	       BT_FREQ_MIN, BT_FREQ_MAX);
	printf("\t-l <%d-%d> capture length as a decimal integer (default: 18)\n",
	       EGO_PACKET_LENGTH_MIN, EGO_PACKET_LENGTH_MAX);
	printf("\t-a <access_code> access code, 2-8 hex digits (default: 630f9ffe)\n");
	printf("\t-U <index|serial> select Ubertooth by index or serial\n");
}

int main(int argc, char *argv[])
{
	int opt;
	/* -h / -V: the first one given ends the command (see below). */
	int first_exit_option = 0;
	int do_mode = -1;
	int do_channel = 2418;
	char* ubertooth_device = NULL;
	uint8_t len = 18;
	int have_access_code = 0;
	uint8_t access_code[EGO_ACCESS_CODE_BYTES];
	int access_code_len = 0;
	int r;

	while ((opt=getopt(argc,argv,"frijc:U:a:l:h")) != EOF) {
		switch(opt) {
		case 'f':
			do_mode = 0;
			break;
		case 'r':
			do_mode = 1;
			break;
		case 'i':
		case 'j':
			do_mode = 2; // TODO take care of these magic numbers
			break;
		case 'c':
			if (opt_require_int(optarg, BT_FREQ_MIN, BT_FREQ_MAX,
			                    "channel", " MHz", &do_channel) < 0)
				return 1;
			break;
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'a':
			if (ego_parse_access_code(optarg, access_code,
			                          &access_code_len) < 0) {
				fprintf(stderr, "error: access code must be 2, 4, 6 or 8 "
				        "hex digits (got %s)\n", optarg);
				return 1;
			}
			have_access_code = 1;
			break;
		case 'l':
			if (ego_parse_packet_length(optarg, &len) < 0) {
				fprintf(stderr, "Length must be in [%d,%d]\n",
				        EGO_PACKET_LENGTH_MIN, EGO_PACKET_LENGTH_MAX);
				usage();
				return 1;
			}
			break;
		case 'h':
			if (!first_exit_option)
				first_exit_option = 'h';
			break;
		default:
			usage();
			return 1;
		}
	}

	/* ubertooth-ego takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h ends the command, but only once the whole command line has
	 * parsed: a bad option value or a stray word is reported even
	 * alongside it. */
	if (first_exit_option == 'h') {
		usage();
		return 1;
	}

	ut = ubertooth_start_select(ubertooth_device);
	if (ut == NULL) {
		return 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0)
		return 1;

	// quit on ctrl-C
	signal(SIGINT, quit);
	signal(SIGQUIT, quit);
	signal(SIGTERM, quit);

	if (do_mode >= 0) {
		usb_pkt_rx rx;

		if (do_mode == 1) // FIXME magic number!
			cmd_set_channel(ut->devh, do_channel);

		if (have_access_code)
			cmd_rfcat_subcmd(ut->devh, RFCAT_SET_AA, access_code, access_code_len);

		cmd_rfcat_subcmd(ut->devh, RFCAT_CAP_LEN, &len, sizeof(len));

		r = cmd_ego(ut->devh, do_mode);
		if (r < 0) {
			if (do_mode == 0 || do_mode == 1)
				printf("Error: E-GO not supported by this firmware\n");
			else
				printf("Error: E-GO not supported by this firmware (or TX not enabled)\n");
			return 1;
		}

		// running set to 0 in signal handlers
		while (running) {
			int r = cmd_poll(ut->devh, &rx);
			if (r < 0) {
				printf("USB error\n");
				break;
			}
			if (r == sizeof(usb_pkt_rx)) {
				fifo_push(ut->fifo, &rx);
				cb_ego(ut, &len);
			}
			usleep(500);
		}
		ubertooth_stop(ut);
	}

	return 0;
}
