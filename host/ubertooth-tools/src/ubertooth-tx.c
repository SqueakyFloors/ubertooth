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
#include "ubertooth_callback.h"
#include "opt_parse.h"
#include <err.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>  // sleep
#include <string.h>
#include <inttypes.h> // PRIx64

static void usage(FILE *file)
{
	fprintf(file, "ubertooth-tx - retransmit symbols dumped with ubertooth-dump\n");
	fprintf(file, "\n");
	fprintf(file, "!!!!\n");
	fprintf(file, "WARNING: This tool currently does nothing!\n");
	fprintf(file, "!!!!\n");
	fprintf(file, "\n");
	fprintf(file, "Usage:\n");
	fprintf(file, "\t-l <LAP> to transmit to (3 bytes / 6 hex digits) - required\n");
	fprintf(file, "\t-u <UAP> to transmit to (1 byte / 2 hex digits) - required\n");
	fprintf(file, "\t-t <SECONDS> timeout - 0 means no timeout [Default: 0]\n");
	fprintf(file, "\t-V print version information\n");
	fputs(OPT_USAGE_SELECT_DEVICE, file);
}

int main(int argc, char* argv[])
{
	int opt, have_lap = 0, have_uap = 0;
	int r;
	int timeout = 0;
	/* -h / -V: the first one given ends the command (see below). */
	int first_exit_option = 0;
	char* ubertooth_device = NULL;
	uint32_t lap = 0;
	uint8_t uap = 0;

	ubertooth_t* ut = NULL;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":hVl:u:U:t:")) != EOF) {
		switch(opt) {
		case 'l':
			if (opt_require_lap(optarg, &lap) < 0)
				return 1;
			have_lap++;
			break;
		case 'u':
			if (opt_require_uap(optarg, &uap) < 0)
				return 1;
			have_uap++;
			break;
		case 'U':
			ubertooth_device = optarg;
			break;
		case 't':
			if (opt_require_count(optarg, 0, "timeout", "seconds",
			                      "0 for none", &timeout) < 0)
				return 1;
			break;
		case 'V':
			if (!first_exit_option)
				first_exit_option = 'V';
			break;
		case 'h':
			if (!first_exit_option)
				first_exit_option = 'h';
			break;
		default:
			opt_report_bad_option(opt, argc, argv);
			return 1;
		}
	}

	/* ubertooth-tx takes no operands: refuse a leftover word before any
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
		usage(stdout);
		return 0;
	}

	/* ubertooth-tx transmits the access code of one piconet, so it needs
	 * both halves of the address. That is an argument error, so it is
	 * refused here rather than after a device has been opened, where it
	 * used to depend on whether a board happened to be free. */
	if (!have_lap || !have_uap) {
		fputs("error: -l <LAP> and -u <UAP> are both required\n", stderr);
		if (opt_no_arguments(argc))
			usage(stderr);
		return 1;
	}

	/* Allocated here rather than at the top of main(): every refusal above
	 * returns without a device, and the handle carries a 64 MB fifo. */
	ut = ubertooth_init();
	if (ut == NULL)
		return 1;

	r = ubertooth_connect_select(ut, ubertooth_device);
	if (r < 0) {
		/* ubertooth_connect_select() has already closed whatever it
		 * opened, so only the handle itself is left to give back. */
		ubertooth_free(ut);
		return 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0) {
		/* A refused version was stopped and closed inside
		 * ubertooth_check_api(), which has already sent STOP; a
		 * version that could not be read was not. So close only a
		 * handle still holding a device, because closing twice would
		 * exit libusb twice. What is not added here is a second STOP. */
		if (ut->devh != NULL)
			ubertooth_close(ut);
		ubertooth_free(ut);
		return 1;
	}

	// set bt address
	/* A libusb error code is not an exit status: the documented statuses
	 * are 0 and 1. */
	r = cmd_set_bdaddr(ut->devh, (((uint32_t)uap)<<24)|lap);
	if (r < 0) {
		/* Nothing has been started on the board and no signal handler
		 * holds the handle yet, so release it here. Without STOP: the
		 * address was not set, so there is nothing to stop. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		return 1;
	}
	/* The tool transmits; the syncword it derived is progress, not the
	 * result the user asked for. */
	fprintf(stderr, "Syncword: 0x%" PRIx64 "\n", btbb_gen_syncword(lap));

	/* Clean up on exit. */
	register_cleanup_handler(ut, 0);

	if (timeout)
		ubertooth_set_timeout(ut, timeout);

	// tell ubertooth to send packets
	r = cmd_tx_syms(ut->devh);
	if (r < 0)
		return 1;

	// TODO - send symbols to the bulk endpoint

	// wait
	while(!ubertooth_stopping(ut))
		sleep(1);

	ubertooth_stop(ut);

	return 0;
}
