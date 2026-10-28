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

ubertooth_t* ut = NULL;

/* The library has already said why a request was refused; this names the
 * request, and leaves the board released rather than capturing with settings
 * the user did not ask for. A refused request is not proof that the board
 * never got it -- a transfer can time out after it arrived -- so the message
 * says only what the tool knows. */
static int request_failed(const char *what)
{
	fprintf(stderr, "error: could not %s\n", what);
	ubertooth_stop(ut);
	ubertooth_free(ut);
	return 1;
}

static void usage(FILE *file)
{
	fprintf(file, "ubertooth-ego - Yuneec E-GO skateboard sniffing\n");
	fprintf(file, "Usage:\n");
	fprintf(file, "\t-h this help\n");
	fprintf(file, "\n");
	fprintf(file, "    Major modes:\n");
	fprintf(file, "\t-f follow connections\n");
	fprintf(file, "\t-r continuous rx on a single channel\n");
	fprintf(file, "\t-i, -j  interfere\n");
	fprintf(file, "\n");
	fprintf(file, "    Options:\n");
	fprintf(file, "\t-c <%d-%d> set channel in MHz (continuous rx only,"
	        " default: %d)\n",
	        BT_FREQ_MIN, BT_FREQ_MAX, EGO_DEFAULT_CHANNEL);
	fprintf(file, "\t-l <%d-%d> capture length as a decimal integer (default: 18)\n",
	        EGO_PACKET_LENGTH_MIN, EGO_PACKET_LENGTH_MAX);
	fprintf(file, "\t-a <access_code> access code, 2, 4, 6 or 8 hex digits\n");
	fprintf(file, "\t       (the board keeps the last code it was given; 630f9ffe\n");
	fprintf(file, "\t       after a reset)\n");
	fputs(OPT_USAGE_SELECT_DEVICE, file);
}

int main(int argc, char *argv[])
{
	int opt;
	opt_once once = {0};
	/* -h ends the command once the whole command line has parsed (see below). */
	int first_exit_option = 0;
	int do_mode = -1;
	/* The option letter that chose the mode, so a refused combination names
	 * it as the user spelled it: -i and -j are the same mode. */
	int mode_option = 0;
	int status = 0;
	int do_channel = EGO_DEFAULT_CHANNEL;
	int have_channel = 0;
	char* ubertooth_device = NULL;
	uint8_t len = 18;
	int have_access_code = 0;
	uint8_t access_code[EGO_ACCESS_CODE_BYTES];
	int access_code_len = 0;
	int r;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":frijc:U:a:l:h")) != EOF) {
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
		switch(opt) {
		case 'f':
		case 'r':
		case 'i':
		case 'j':
			/* One major mode per command: each one runs until it is
			 * interrupted, so two of them used to mean whichever came
			 * last, with the others silently dropped. -i and -j are two
			 * spellings of the jamming mode, so they clash too.
			 * ubertooth-btle and ubertooth-util refuse two modes the
			 * same way. */
			/* The same letter twice says the same thing twice, so
			 * only a different mode clashes (ubertooth(7),
			 * OPTION VALUES). */
			if (mode_option != 0 && mode_option != opt) {
				fprintf(stderr, "error: -%c cannot be combined with -%c;"
				        " choose one of -f, -r or -i\n",
				        opt, mode_option);
				return 1;
			}
			mode_option = opt;
			if (opt == 'f')
				do_mode = EGO_MODE_FOLLOW;
			else if (opt == 'r')
				do_mode = EGO_MODE_CONTINUOUS_RX;
			else
				do_mode = EGO_MODE_JAM;
			break;
		case 'c':
			if (opt_require_int(optarg, BT_FREQ_MIN, BT_FREQ_MAX,
			                    "channel", " MHz", &do_channel) < 0)
				return 1;
			have_channel = 1;
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
				fprintf(stderr, "error: capture length must be %d-%d (got %s)\n",
				        EGO_PACKET_LENGTH_MIN, EGO_PACKET_LENGTH_MAX, optarg);
				return 1;
			}
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

	/* ubertooth-ego takes no operands: refuse a leftover word before any
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

	/* A major mode is the work. Without one the tool used to open the
	 * selected board, check its firmware, do nothing and exit 0. */
	if (do_mode < 0) {
		fputs("error: nothing to do: choose a major mode (-f, -r or -i)\n",
		      stderr);
		if (opt_no_arguments(argc))
			usage(stderr);
		return 1;
	}

	/* Only continuous receive sends a channel, so -c in any other mode used
	 * to be parsed, range-checked and then dropped without a word. Refused
	 * before any device is opened, like the other combinations that cannot
	 * work. Below the check above, which is what guarantees mode_option
	 * names an option the user gave: a command line with -c and no mode has
	 * nothing to do, which is the more useful thing to say. */
	if (have_channel && do_mode != EGO_MODE_CONTINUOUS_RX) {
		fprintf(stderr, "error: -c cannot be combined with -%c; only "
		        "continuous receive (-r) uses a channel\n", mode_option);
		return 1;
	}

	ut = ubertooth_start_select(ubertooth_device);
	if (ut == NULL) {
		return 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0) {
		/* ubertooth_check_api() releases the board itself on a version
		 * mismatch; a version it could not read at all leaves the board
		 * open. Closing covers both, because closing a board already
		 * released does nothing, and it sends no STOP to a firmware
		 * this host will not drive. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		return 1;
	}

	/* Stop on Ctrl-C, and on the other signals that mean the run is over
	 * (SIGTERM, SIGHUP, SIGQUIT, an alarm): the library's handlers, so the
	 * capture loop below ends the way every other tool's does. */
	register_cleanup_handler(ut, 0);

	if (do_mode >= 0) {
		usb_pkt_rx rx;

		if (do_mode == EGO_MODE_CONTINUOUS_RX) {
			if (cmd_set_channel(ut->devh, do_channel) < 0)
				return request_failed("set the channel");
		}

		if (have_access_code) {
			if (cmd_rfcat_subcmd(ut->devh, RFCAT_SET_AA, access_code,
			                     access_code_len) < 0)
				return request_failed("set the access code");
		}

		if (cmd_rfcat_subcmd(ut->devh, RFCAT_CAP_LEN, &len, sizeof(len)) < 0)
			return request_failed("set the capture length");

		r = cmd_ego(ut->devh, do_mode);
		if (r < 0) {
			if (do_mode != EGO_MODE_JAM)
				fputs("error: E-GO not supported by this firmware\n", stderr);
			else
				fputs("error: E-GO not supported by this firmware "
				      "(or TX not enabled)\n", stderr);
			/* Released like any other refused request, rather than left
			 * claimed because this one has its own message. */
			ubertooth_stop(ut);
			ubertooth_free(ut);
			return 1;
		}

		while (!ubertooth_stopping(ut)) {
			int r = cmd_poll(ut->devh, &rx);
			if (r < 0) {
				/* The capture stopped short, so the command did not do
				 * what was asked: say so in the exit status too. */
				fputs("error: USB poll failed\n", stderr);
				status = 1;
				break;
			}
			if (r == sizeof(usb_pkt_rx)) {
				/* The packet cmd_poll() just handed back, so it arrived now. */
				fifo_push(ut->fifo, &rx, ubertooth_packet_arrival_ns());
				cb_ego(ut, &len);
			} else {
				/* Not a whole record, so there is no packet to read
				 * out of it: say so rather than drop it in silence. */
				ubertooth_warn_if_poll_reply_short(ut, r);
			}
			usleep(500);
		}
		ubertooth_stop(ut);
	}
	ubertooth_free(ut);

	return status;
}
