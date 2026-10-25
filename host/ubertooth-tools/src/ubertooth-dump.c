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
#include "opt_parse.h"
#include <errno.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *file)
{
	fprintf(file, "ubertooth-dump - output a continuous stream of received bits\n");
	fprintf(file, "Usage:\n");
	fprintf(file, "\t-h this help\n");
	fprintf(file, "\t-b only dump received bitstream (GnuRadio style)\n");
	fprintf(file, "\t-c classic modulation\n");
	fprintf(file, "\t-l LE modulation\n");
	fputs(OPT_USAGE_SELECT_DEVICE, file);
	fprintf(file, "\t-d filename\n");
	fprintf(file, "\nThis program sends binary data to stdout.  You probably don't want to\n");
	fprintf(file, "run it from a terminal without redirecting the output.\n");
}

/*
 * The normal output format is in chunks of 64 bytes in the USB RX packet format
 * 50 of those 64 bytes contain the received symbols (packed 8 per byte).
 *
 * The -b output format is a stream of bytes, each either 0x00 or 0x01
 * representing the symbol determined by the demodulator (GnuRadio style)
 */

int main(int argc, char *argv[])
{
	int opt;
	/* -h ends the command once the whole command line has parsed (see below). */
	int first_exit_option = 0;
	int bitstream = 0;
	int modulation = MOD_BT_BASIC_RATE;
	char* ubertooth_device = NULL;
	const char* dump_name = NULL; /* -d value, opened after every check */

	ubertooth_t* ut = NULL;
	int r;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":bhclU:d:")) != EOF) {
		switch(opt) {
		case 'b':
			bitstream = 1;
			break;
		case 'c':
			modulation = MOD_BT_BASIC_RATE;
			break;
		case 'l':
			modulation = MOD_BT_LOW_ENERGY;
			break;
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'd':
			/* Named here and opened once the whole command line has
			 * been accepted, so a refused command creates and
			 * truncates nothing. */
			dump_name = optarg;
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

	/* ubertooth-dump takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h ends the command, but only once the whole command line has
	 * parsed: a bad option value or a stray word is reported even
	 * alongside it. */
	if (first_exit_option == 'h') {
		usage(stdout);
		return 0;
	}

	/* Every check has passed, so the dump file can be opened. */
	if (dump_name != NULL) {
		dumpfile = fopen(dump_name, "w");
		if (dumpfile == NULL) {
			fprintf(stderr, "error: %s: %s\n", dump_name, strerror(errno));
			return 1;
		}
	}

	ut = ubertooth_start_select(ubertooth_device);

	if (ut == NULL) {
		return 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0)
		return 1;

	/* Clean up on exit. */
	register_cleanup_handler(ut, 0);

	cmd_set_modulation(ut->devh, modulation);
	rx_dump(ut, bitstream);

	ubertooth_stop(ut);
	return 0;
}
