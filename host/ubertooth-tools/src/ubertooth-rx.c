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
#include "request_report.h"
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *file)
{
	fprintf(file, "ubertooth-rx - passive Classic Bluetooth discovery/decode\n");
	fprintf(file, "\n");
	fprintf(file, "Example usage:\n");
	fprintf(file, "\tubertooth-rx -- sniff for all LAPs\n");
	fprintf(file, "\tubertooth-rx -l <lap> -- calculate UAP for a given LAP\n");
	fprintf(file, "\tubertooth-rx -l <lap> -u <uap> -- calculate clock and follow piconet\n");
	fprintf(file, "\tubertooth-rx -z -t 20 -- survey mode: discover all LAPs+UAPs for 20 seconds\n");
	fprintf(file, "\n");
	fprintf(file, "Major modes:\n");
	fprintf(file, "\t-l <LAP> to decode (6 hex) - if not specified sniff all LAPs\n");
	fprintf(file, "\t-u <UAP> to decode (2 hex) - if not specified calculate UAP (requires LAP)\n");
	fprintf(file, "\t-z Survey mode - discover and list piconets (interrupt with ctrl-C)\n");
	fprintf(file, "\t-i <filename> replay a capture saved with -d, with its recorded times\n");
	fprintf(file, "\t              - if not specified use Ubertooth for live capture\n");
	fprintf(file, "\n");
	fprintf(file, "Configuration:\n");
	fprintf(file, "\t-c <0-%d> set a fixed bluetooth channel [Default: sweep]\n",
	        NUM_BREDR_CHANNELS - 1);
	fprintf(file, "\t-s sweep all channels - the default; a -c channel wins over it\n");
	fprintf(file, "\t-e max_ac_errors (default: %d, range: 0-%d)\n",
	        max_ac_errors, MAX_AC_ERRORS);
	fprintf(file, "\t-t <SECONDS> sniff timeout - 0 means no timeout [Default: 0]\n");
	fprintf(file, "\n");
	fprintf(file, "Output options:\n");
	fprintf(file, "\t-r <filename> capture packets to PcapNG file\n");
	fprintf(file, "\t-q <filename> capture packets to PCAP file\n");
	fprintf(file, "\t-d <filename> dump packets to binary file\n");
	fprintf(file, "\n");
	fprintf(file, "Miscellaneous:\n");
	fprintf(file, "\t-V print version information\n");
	fputs(OPT_USAGE_SELECT_DEVICE, file);
}

/* Names the request the board refused, says why and what to try, and leaves
 * the board released rather than capturing with settings the user did not ask
 * for. A refused request is not proof that the board never got it -- a
 * transfer can time out after it arrived -- so the message says only what the
 * tool knows. */
static int request_failed(ubertooth_t* ut, const char* what, int code)
{
	report_refused_request(what, code);
	ubertooth_stop(ut);
	return 1;
}

int main(int argc, char* argv[])
{
	int opt, have_lap = 0, have_uap = 0;
	int survey_mode = 0;
	/* Whether -c named a channel, which decides only how a refused channel
	 * request is named: a command line without it asks for the sweep. */
	int have_channel = 0;
	int exit_status = 0;
	const char* infile_name = NULL;
	/* Files named on the command line, opened after every check passes. */
	const char* pcapng_name = NULL;
	const char* pcap_name = NULL;
	const char* dump_name = NULL;
	/* -h / -V: the first one given ends the command (see below). */
	int first_exit_option = 0;
	int r;
	int timeout = 0;
	char* ubertooth_device = NULL;
	btbb_piconet* pn = NULL;
	uint32_t lap = 0;
	uint8_t uap = 0;
	/* Above the Bluetooth band, which the firmware reads as "sweep the
	 * whole band": the default when no -c names a fixed channel. */
	uint16_t channel = 9999;

	ubertooth_t* ut = ubertooth_init();
	if (ut == NULL)
		return 1;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":hVi:l:u:U:d:e:r:sq:t:zc:")) != EOF) {
		switch(opt) {
		case 'i':
			infile_name = optarg;
			break;
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
		/* The capture and dump files are named here and opened once the
		 * whole command line has been accepted, so a refused command
		 * leaves the filesystem alone. */
		case 'r':
			if (pcapng_name == NULL)
				pcapng_name = optarg;
			else
				fprintf(stderr, "Ignoring extra capture file: %s\n", optarg);
			break;
		case 'q':
			if (pcap_name == NULL)
				pcap_name = optarg;
			else
				fprintf(stderr, "Ignoring extra capture file: %s\n", optarg);
			break;
		case 'd':
			dump_name = optarg;
			break;
		case 'e':
			if (opt_require_int(optarg, 0, MAX_AC_ERRORS,
			                    "maximum access code errors", "",
			                    &max_ac_errors) < 0)
				return 1;
			break;
		case 's':
			/* The sweep is what the tool does when no -c names a
			 * fixed channel, so -s has nothing to select: it is
			 * accepted so a command line can state the default. */
			break;
		case 't':
			if (opt_require_count(optarg, 0, "timeout", "seconds",
			                      "0 for none", &timeout) < 0)
				return 1;
			break;
		case 'z':
			++survey_mode;
			break;
		case 'c': {
			int bt_channel;
			if (opt_require_int(optarg, 0, NUM_BREDR_CHANNELS - 1,
			                    "channel", "", &bt_channel) < 0)
				return 1;
			channel = (uint16_t)(bt_channel + BT_FREQ_MIN);
			have_channel = 1;
			break;
		}
		case 'V':
			if (!first_exit_option)
				first_exit_option = 'V';
			break;
		case 'h':
			if (!first_exit_option)
				first_exit_option = 'h';
			break;
		default:
			/* A command line the tool will not take: the one-line
			 * reason goes to standard error and standard output
			 * stays empty. The tool's other output lines are
			 * converted separately. */
			opt_report_bad_option(opt, argc, argv);
			return 1;
		}
	}

	/* ubertooth-rx takes no operands: refuse a leftover word before any
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

	if(survey_mode && (have_lap || have_uap)) {
		fprintf(stderr, "No address should be specified for survey mode\n");
		return 1;
	}

	/* A UAP is only usable with the LAP it belongs to. This is an argument
	 * error like the survey check above, so it is refused here rather than
	 * after a device has been opened, where it used to depend on whether a
	 * board happened to be free. (Survey mode has already returned for any
	 * address, so it cannot reach this.) */
	if (have_uap && !have_lap) {
		fprintf(stderr, "error: -u <UAP> requires -l <LAP>\n");
		return 1;
	}

	/* Every check has passed, so the files can be opened: a refused command
	 * line creates and truncates nothing. */
	if (infile_name != NULL) {
		infile = fopen(infile_name, "r");
		if (infile == NULL) {
			/* A file that is not there is a runtime failure, not a
			 * bad command line, so it gets no usage text; same
			 * shape as the other file-open failures. */
			fprintf(stderr, "error: %s: %s\n",
			        infile_name, strerror(errno));
			return 1;
		}
	}
	if (pcapng_name != NULL &&
	    btbb_pcapng_create_file(pcapng_name, "Ubertooth", &ut->h_pcapng_bredr)) {
		err(1, "create_bredr_capture_file: ");
	}
	if (pcap_name != NULL &&
	    btbb_pcap_create_file(pcap_name, &ut->h_pcap_bredr)) {
		err(1, "btbb_pcap_create_file: ");
	}
	if (dump_name != NULL) {
		dumpfile = fopen(dump_name, "w");
		if (dumpfile == NULL) {
			fprintf(stderr, "error: %s: %s\n",
			        dump_name, strerror(errno));
			return 1;
		}
	}

	if (infile == NULL) {
		r = ubertooth_connect_select(ut, ubertooth_device);
		if (r < 0) {
			return 1;
		}

		r = ubertooth_check_api(ut);
		if (r < 0) {
			/* A version mismatch has already released the board, and
			 * releasing it twice is not safe; a version the library
			 * could not read at all leaves it open, so it is closed
			 * here, without a STOP for a firmware this host will
			 * not drive. */
			if (ut->devh != NULL)
				ubertooth_close(ut);
			return 1;
		}
	}

	r = btbb_init(max_ac_errors);
	if (r < 0)
		return r;

	if(survey_mode) {
		// auto-flush stdout so that wrapper scripts work
		setvbuf(stdout, NULL, _IONBF, 0);
		btbb_init_survey();
	} else {
		if (have_lap) {
			pn = btbb_piconet_new();
			btbb_init_piconet(pn, lap);
			if (have_uap) {
				btbb_piconet_set_uap(pn, uap);
				/* A board that refused this would follow
				 * whatever address it was left on, so the
				 * capture does not start. A replay sends no
				 * request at all. */
				if (infile == NULL) {
					r = cmd_set_bdaddr_quiet(ut->devh,
					        btbb_piconet_get_bdaddr(pn));
					if (r < 0)
						return request_failed(ut,
						        "set the Bluetooth address",
						        r);
				}
			}
			if (ut->h_pcapng_bredr) {
				btbb_pcapng_record_bdaddr(ut->h_pcapng_bredr,
							  (((uint32_t)uap)<<24)|lap,
							  have_uap ? 0xff : 0x00, 0);
			}
		}
	}

	if (infile == NULL) {
		/* A board that refused this would capture on whatever channel
		 * it was left on, and say nothing about it, so the capture does
		 * not start. */
		r = cmd_set_channel_quiet(ut->devh, channel);
		if (r < 0)
			return request_failed(ut, have_channel ? "set the channel"
			                                       : "set sweep mode", r);

		/* Clean up on exit. */
		register_cleanup_handler(ut, 0);

		if (timeout)
			ubertooth_set_timeout(ut, timeout);

		// init USB transfer
		r = ubertooth_bulk_init(ut);
		if (r < 0) {
			/* nothing is in flight, but the board is still open and in
			 * whatever mode an earlier command left it */
			ubertooth_stop(ut);
			return r;
		}

		r = ubertooth_bulk_thread_start();
		if (r < 0) {
			/* The receive transfer is in flight and the board is about to
			 * be left in its mode: give both back before returning. */
			ubertooth_stop(ut);
			return r;
		}

		// tell ubertooth to send packets
		r = cmd_rx_syms(ut->devh);
		if (r < 0) {
			/* The poll thread is running by now and nothing else joins
			 * it: ubertooth_stop() frees the libusb context it polls. */
			ubertooth_bulk_thread_stop();
			ubertooth_stop(ut);
			return r;
		}

		// receive and process each packet
		while(!ut->stop_ubertooth) {
			ubertooth_bulk_receive(ut, cb_rx, pn);
		}

		ubertooth_bulk_thread_stop();

		ubertooth_stop(ut);

		/* A capture whose stream died stopped there, so it holds less than
		 * was asked for. The exit status has to say so, or a caller cannot
		 * tell it from a timeout or a Ctrl-C. The handle is read after the
		 * teardown, because cancelling the transfer can be what brings the
		 * failure out. The survey results below are still printed, since
		 * they are what did arrive, and libubertooth has already printed
		 * what failed. */
		if (ubertooth_stream_failed(ut))
			exit_status = 1;
	} else {
		/* A read error or a last record cut short is reported and ends
		 * the command with an error, after whatever was decoded. */
		r = stream_rx_file(ut, infile, cb_rx, pn);
		fclose(infile);
		exit_status = rx_file_report(r, infile_name);
	}

	if(survey_mode) {
		printf("Survey Results\n");
		while((pn=btbb_next_survey_result()) != NULL) {
			lap = btbb_piconet_get_lap(pn);
			if (btbb_piconet_get_flag(pn, BTBB_UAP_VALID)) {
				uap = btbb_piconet_get_uap(pn);
				/* Printable version showing that the NAP is unknown */
				printf("??:??:%02X:%02X:%02X:%02X\n", uap,
						(lap >> 16) & 0xFF, (lap >> 8) & 0xFF, lap & 0xFF);
			} else
				printf("??:??:??:%02X:%02X:%02X\n", (lap >> 16) & 0xFF,
					   (lap >> 8) & 0xFF, lap & 0xFF);
			//btbb_print_afh_map(pn);
		}
	}
	if(dumpfile != NULL)
		fclose(dumpfile);

	return exit_status;
}
