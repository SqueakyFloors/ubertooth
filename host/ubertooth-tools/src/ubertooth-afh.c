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

static void usage(FILE *file)
{
	fprintf(file, "ubertooth-afh - passive detection of the AFH channel map\n");
	fprintf(file, "\n");
	fprintf(file, "Determine the AFH map for piconet ??:??:22:44:66:88:\n");
	fprintf(file, "    ubertooth-afh -u 22 -l 446688\n");
	fprintf(file, "\n");
	fprintf(file, "Main options:\n");
	fprintf(file, "\t-l <LAP> LAP of target piconet (3 bytes / 6 hex digits)\n");
	fprintf(file, "\t-u <UAP> UAP of target piconet (1 byte / 2 hex digits)\n");
	fprintf(file, "\t-m <packets> mark a channel unused after this many packets without it (default: 5)\n");
	fprintf(file, "\t-r print AFH channel map once every second (default: print on update)\n");
	fprintf(file, "\n");
	fprintf(file, "Other options\n");
	fprintf(file, "\t-t <seconds> timeout for initial AFH map detection (not required; see below)\n");
	fprintf(file, "\t-e maximum access code errors (default: %d, range: 0-%d)\n",
	        max_ac_errors, MAX_AC_ERRORS);
	fprintf(file, "\t-V print version information\n");
	fputs(OPT_USAGE_SELECT_DEVICE, file);
	fprintf(file, "\n");
	fprintf(file, "The tool keeps monitoring the map for changes until it is\n");
	fprintf(file, "interrupted (Ctrl-C), both with and without -t. With -t, it stops\n");
	fprintf(file, "with exit status 2 if no packet from the piconet was received\n");
	fprintf(file, "during those seconds. To wait for a piconet that is not in range\n");
	fprintf(file, "yet, run without -t. -t has no effect with -r.\n");
}

int main(int argc, char* argv[])
{
	int opt, have_lap = 0, have_uap = 0, timeout = 0;
	int threshold;
	opt_once once = {0};
	/* -h and -V both end the command once the whole command line has
	 * parsed, and a command line with both prints both (see below). */
	int do_help = 0, do_version = 0;
	char* ubertooth_device = NULL;
	btbb_piconet* pn = NULL;
	uint32_t lap = 0;
	uint8_t uap = 0;
	uint8_t use_r_format = 0;

	ubertooth_t* ut = NULL;
	int r;
	int status = 0;

	// default value for '-m' channel timeout
	packet_counter_max = 5;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":rhVl:u:U:e:t:m:")) != EOF) {
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
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
			do_version = 1;
			break;
		case 'r':
			use_r_format = 1;
			break;
		case 'h':
			do_help = 1;
			break;
		default:
			opt_report_bad_option(opt, argc, argv);
			return 1;
		}
	}

	/* ubertooth-afh takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h and -V end the command, but only once the whole command line
	 * has parsed: a bad option value or a stray word is reported even
	 * alongside them. A command line with both prints both, and the usage
	 * text comes last so the version is not scrolled off the screen by
	 * it; the order they were written in does not matter. */
	if (do_version)
		print_version();
	if (do_help)
		usage(stdout);
	if (do_version || do_help)
		return 0;

	if (have_lap && have_uap) {
		pn = btbb_piconet_new();
		if (pn == NULL) {
			fputs("error: out of memory\n", stderr);
			return 1;
		}
		btbb_init_piconet(pn, lap);
		btbb_piconet_set_uap(pn, uap);
		btbb_piconet_set_flag(pn, BTBB_IS_AFH, 1);
		btbb_piconet_set_flag(pn, BTBB_LOOKS_LIKE_AFH, 1);
		btbb_init_survey();
	} else {
		fputs("error: -l <LAP> and -u <UAP> are both required: "
		      "the map is detected for one piconet\n", stderr);
		if (opt_no_arguments(argc))
			usage(stderr);
		return 1;
	}

	/* NULL when no board matches -U or it cannot be opened; the library
	 * has already said why. */
	ut = ubertooth_start_select(ubertooth_device);
	if (ut == NULL) {
		btbb_piconet_unref(pn);
		return 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0) {
		/* ubertooth_check_api() releases the board itself when it refuses
		 * the firmware, and leaves it open when it could not read it.
		 * Closing covers both, because closing a board already released
		 * does nothing. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		btbb_piconet_unref(pn);
		return 1;
	}

	/* Clean up on exit. */
	register_cleanup_handler(ut, 0);

	if (use_r_format) {
		r = rx_afh_r(ut, pn, timeout);
		/* A receive that could not be started at all; libubertooth has
		 * already said what failed. A stream that died part-way is read
		 * from the handle below instead. */
		if (r < 0)
			status = 1;
	} else {
		r = rx_afh(ut, pn, timeout);
		if (r == UBERTOOTH_AFH_NO_PACKETS) {
			/* An all-zero map used to be printed here, which reads as a
			 * detected map whose piconet uses no channel at all. */
			fprintf(stderr, "error: no packets from ??:??:%02x:%02x:%02x:%02x"
			        " within %d s; AFH map unknown, not monitoring\n",
			        uap, (lap >> 16) & 0xff, (lap >> 8) & 0xff, lap & 0xff,
			        timeout);
			status = 2;
		} else if (r < 0) {
			/* libubertooth has already said what failed. */
			status = 1;
		}
	}

	ubertooth_stop(ut);

	/* What the capture is missing, once the packets have scrolled
	 * past. A gap is a stretch the board had no queue room for, so
	 * the count is of gaps and not of the packets lost in them. */
	ubertooth_report_queue_gaps(ut);
	/* A stream that died is read after the teardown, because the teardown is
	 * where a transfer the board never gave back comes out: that run holds
	 * less than was asked for, whatever the phases above made of it. A
	 * teardown the board refused is read here for the same reason, and left
	 * the board running the mode this tool started. libubertooth has already
	 * printed what failed in both cases. */
	if (ubertooth_stream_failed(ut) || ubertooth_stop_failed(ut))
		status = 1;
	ubertooth_free(ut);
	/* The piconet outlives the receive phases, so it is given back here
	 * rather than in them. */
	btbb_piconet_unref(pn);

	return status;
}
