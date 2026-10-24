/*
 * Copyright 2015 Hannes Ellinger
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
#include "opt_parse.h"
#include <err.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

extern int max_ac_errors;
extern unsigned int packet_counter_max;

static void usage()
{
	printf("ubertooth-afh - passive detection of the AFH channel map\n");
	printf("\n");
	printf("Determine the AFH map for piconet ??:??:22:44:66:88:\n");
	printf("    ubertooth-afh -u 22 -l 446688\n");
	printf("\n");
	printf("Main options:\n");
	printf("\t-l <LAP> LAP of target piconet (3 bytes / 6 hex digits)\n");
	printf("\t-u <UAP> UAP of target piconet (1 byte / 2 hex digits)\n");
	printf("\t-m <packets> mark a channel unused after this many packets without it (default: 5)\n");
	printf("\t-r print AFH channel map once every second (default: print on update)\n");
	printf("\n");
	printf("Other options\n");
	printf("\t-t <seconds> timeout for initial AFH map detection (not required; see below)\n");
	printf("\t-e maximum access code errors (default: %d, range: 0-%d)\n",
	       max_ac_errors, MAX_AC_ERRORS);
	printf("\t-V print version information\n");
	printf("\t-U <index|serial> set ubertooth device to use\n");
	printf("\n");
	printf("The tool keeps monitoring the map for changes until it is\n");
	printf("interrupted (Ctrl-C), both with and without -t. With -t, it stops\n");
	printf("with exit status 2 if no packet from the piconet was received\n");
	printf("during those seconds. -t has no effect with -r.\n");
}

int main(int argc, char* argv[])
{
	int opt, have_lap = 0, have_uap = 0, timeout = 0;//, have_initial_afh = 0;
	// uint8_t initial_afh[10];
	int threshold;
	/* -h / -V: the first one given ends the command (see below). */
	int first_exit_option = 0;
	char* ubertooth_device = NULL;
	btbb_piconet* pn = NULL;
	uint32_t lap = 0;
	uint8_t uap = 0;
	uint8_t use_r_format = 0;

	ubertooth_t* ut = NULL;
	int r;

	// default value for '-m' channel timeout
	packet_counter_max = 5;

	while ((opt=getopt(argc,argv,"rhVl:u:U:e:a:t:m:")) != EOF) {
		switch(opt) {
		case 'l':
			if (opt_require_lap(optarg, &lap) < 0)
				return 1;
			have_lap = 1;
			break;
		case 'u':
			if (opt_require_uap(optarg, &uap) < 0)
				return 1;
			have_uap = 1;
			break;
		case 'a':
			// for(int i=0; i<10; i++) {
			// 	sscanf(2+optarg+2*i, "%2hhx", &initial_afh[i]);
			// }
			// have_initial_afh = 1;
			break;
		case 't':
			if (opt_require_count(optarg, 0, "timeout", "seconds",
			                      "0 for none", &timeout) < 0)
				return 1;
			break;
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'e':
			if (opt_require_int(optarg, 0, MAX_AC_ERRORS,
			                    "maximum access code errors", "",
			                    &max_ac_errors) < 0)
				return 1;
			break;
		case 'm':
			if (opt_require_count(optarg, 1,
			                      "channel removal threshold", "packets",
			                      "1 or more", &threshold) < 0)
				return 1;
			packet_counter_max = (unsigned int)threshold;
			break;
		case 'V':
			if (!first_exit_option)
				first_exit_option = 'V';
			break;
		case 'r':
			use_r_format = 1;
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

	/* ubertooth-afh takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h and -V end the command, but only once the whole command line
	 * has parsed: a bad option value or a stray word is reported even
	 * alongside them. The first of the two given wins, as before. */
	if (first_exit_option == 'V') {
		print_version();
		return 0;
	}
	if (first_exit_option == 'h') {
		usage();
		return 1;
	}

	if (have_lap && have_uap) {
		pn = btbb_piconet_new();
		btbb_init_piconet(pn, lap);
		btbb_piconet_set_uap(pn, uap);
		btbb_piconet_set_flag(pn, BTBB_IS_AFH, 1);
		btbb_piconet_set_flag(pn, BTBB_LOOKS_LIKE_AFH, 1);
		btbb_init_survey();
	} else {
		printf("Error: UAP or LAP not specified\n");
		usage();
		return 1;
	}

	/* NULL when no board matches -U or it cannot be opened; the library
	 * has already said why. */
	ut = ubertooth_start_select(ubertooth_device);
	if (ut == NULL) {
		return 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0)
		return 1;

	/* Clean up on exit. */
	register_cleanup_handler(ut, 0);

	if (use_r_format) {
		rx_afh_r(ut, pn, timeout);
	} else {
		r = rx_afh(ut, pn, timeout);
		if (r == UBERTOOTH_AFH_NO_PACKETS) {
			/* An all-zero map used to be printed here, which reads as a
			 * detected map whose piconet uses no channel at all. */
			fprintf(stderr, "error: no packets from ??:??:%02x:%02x:%02x:%02x"
			        " within %d s; AFH map unknown\n",
			        uap, (lap >> 16) & 0xff, (lap >> 8) & 0xff, lap & 0xff,
			        timeout);
			ubertooth_stop(ut);
			return 2;
		}
		if (r < 0) {
			/* libubertooth has already said what failed. */
			ubertooth_stop(ut);
			return 1;
		}
	}

	ubertooth_stop(ut);

	return 0;
}
