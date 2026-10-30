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
	fprintf(file, "\t-V print version information\n");
	fprintf(file, "\t-b only dump received bitstream (GnuRadio style)\n");
	fprintf(file, "\t-c classic modulation\n");
	fprintf(file, "\t-l LE modulation\n");
	fputs(OPT_USAGE_SELECT_DEVICE, file);
	fprintf(file, "\t-d <filename> write the dump here ('-' is standard output)\n");
	fprintf(file, "\nA dump is written for a file or another program, so a terminal on standard\n");
	fprintf(file, "output is refused: redirect it, pipe it, or give -d a file.\n");
}

/*
 * The normal output is one 68-byte record per block the board sends: the host
 * time it arrived, as Unix epoch seconds in 4 bytes big-endian, then the whole
 * 64-byte USB receive packet, 50 bytes of which are the received symbols
 * packed 8 per byte.
 *
 * The -b output is one line per block: 400 characters, each the ASCII '0' or
 * '1' the demodulator decided (GnuRadio style), then a newline.
 */

int main(int argc, char *argv[])
{
	int opt;
	opt_once once = {0};
	/* -h ends the command once the whole command line has parsed (see below),
	 * and so does -V, whose own result is version_status: it reads the board,
	 * so it can fail where -h cannot. */
	int first_exit_option = 0;
	int do_version = 0, version_status = 0;
	int bitstream = 0;
	int modulation = MOD_BT_BASIC_RATE;
	/* The option that chose the modulation, so a refused pair names both as
	 * the user wrote them. */
	int modulation_option = 0;
	char* ubertooth_device = NULL;
	const char* dump_name = NULL; /* -d value, opened after every check */

	ubertooth_t* ut = NULL;
	int r, status;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	/* Named, so the loop below and opt_report_bad_option()
	 * cannot be given different letters. */
	const char options[] = ":bhVclU:d:";
	opterr = 0;
	while ((opt=getopt(argc, argv, options)) != EOF) {
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
		switch(opt) {
		case 'b':
			bitstream = 1;
			break;
		case 'c':
		case 'l':
			/* One capture, one modulation: the board demodulates one at
			 * a time, so -c and -l in one command used to mean whichever
			 * came last and silently dropped the other. */
			/* The same letter twice says the same thing twice, so
			 * only a different modulation clashes. */
			if (modulation_option != 0 && modulation_option != opt) {
				fprintf(stderr, "error: -%c cannot be combined with -%c;"
				        " a capture uses one modulation\n",
				        opt, modulation_option);
				return 1;
			}
			modulation_option = opt;
			modulation = (opt == 'c') ? MOD_BT_BASIC_RATE
			                          : MOD_BT_LOW_ENERGY;
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
		case 'V':
			do_version = 1;
			break;
		case 'h':
			if (!first_exit_option)
				first_exit_option = 'h';
			break;
		default:
			opt_report_bad_option(opt, argc, argv, options);
			return 1;
		}
	}

	/* ubertooth-dump takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h and -V end the command, but only once the whole command line has
	 * parsed: a bad option value or a stray word is reported even alongside
	 * them. A command line with both prints both, and the usage text comes
	 * last so the version is not scrolled off the screen by it. */
	if (do_version)
		version_status = ubertooth_print_versions(stdout, ubertooth_device);
	if (first_exit_option == 'h')
		usage(stdout);
	if (do_version || first_exit_option == 'h')
		return version_status;

	/* Without -d the stream is standard output, so a terminal there is the
	 * same mistake as "-d -" on one: it fills the screen with control
	 * characters and leaves nothing a reader can use. Either way it is a
	 * problem with the command line, so it is refused here, before any
	 * board is opened; the file itself is opened once the board has been
	 * accepted, below. */
	if (dump_name == NULL) {
		if (opt_check_stdout_not_tty("dump data") < 0)
			return 1;
	} else if (opt_check_capture_destination(dump_name, "dump data") < 0) {
		return 1;
	}

	/* Stop on Ctrl-C from here on, before the board is opened: a signal
	 * that arrives while one is being found would otherwise be discarded. */
	ubertooth_catch_stop_signals();

	ut = ubertooth_start_select(ubertooth_device);

	if (ut == NULL) {
		return ubertooth_open_stopped() ? 0 : 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0) {
		/* A refused version was released inside ubertooth_check_api(); one
		 * it could not read at all leaves the board open. Closing covers
		 * both, because closing a board already released does nothing. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		return 1;
	}

	/* Stopped while the board was being opened: the run ends here, with the
	 * board given back, rather than starting a mode and stopping it again. */
	if (ubertooth_stop_requested()) {
		ubertooth_close(ut);
		ubertooth_free(ut);
		return 0;
	}

	/* The board is open and usable, so the dump file can be opened: a run
	 * refused for its firmware leaves no empty file behind for the user to
	 * tell apart from a dump that caught nothing. The file is left to
	 * process exit from here on, as on every other exit of this tool:
	 * libubertooth owns that global, not the tool. */
	if (dump_name != NULL) {
		dumpfile = opt_open_dump_file(dump_name, "dump data");
		if (dumpfile == NULL) {
			ubertooth_close(ut);
			ubertooth_free(ut);
			return 1;
		}
	}

	/* The modulation decides what the dump holds, so a board that would not
	 * take it ends the run. Letting the request fail quietly dumped
	 * whatever modulation the board had been left in: -l on a board still
	 * set to Classic gave Classic symbols and exit 0, and nothing in the
	 * output says which modulation produced it.
	 *
	 * This is the last thing asked of the board before the capture, and it
	 * stays there: the firmware keeps the value until a mode ends, and the
	 * tools that never set their own modulation (ubertooth-rx, -follow,
	 * -afh) would then demodulate with it. So nothing between here and the
	 * capture may fail -- an unwritable -d is checked above, which costs an
	 * empty file on this path and leaves the radio as the dump found it.
	 *
	 * No mode is running yet, so the board is given back the way a refused
	 * firmware version gives it back. */
	if (cmd_set_modulation(ut->devh, modulation) < 0) {
		fprintf(stderr, "error: modulation set failed\n");
		ubertooth_close(ut);
		ubertooth_free(ut);
		return 1;
	}

	/* A dump that could not start, or whose stream died part-way, holds
	 * less than was asked for. The exit status has to say so, or a caller
	 * cannot tell it from a Ctrl-C. The handle is read again after the
	 * teardown, because cancelling the transfer can be what brings the
	 * failure out. libubertooth has already printed what failed. */
	r = rx_dump(ut, bitstream);

	ubertooth_stop(ut);
	/* What the dump is missing, once the records have scrolled past. The
	 * counts are of the places a loss was marked, not of the packets lost
	 * at them. */
	ubertooth_report_capture_losses(ut);

	/* A teardown the board refused is read here too: that board is left
	 * running the mode this dump started, and no other tool can open it
	 * until it is reset or re-plugged. libubertooth has already named the
	 * refused request, and what was dumped stays written out. */
	status = (r < 0 || ubertooth_stream_failed(ut)
	          || ubertooth_stop_failed(ut)) ? 1 : 0;
	ubertooth_free(ut);
	return status;
}
