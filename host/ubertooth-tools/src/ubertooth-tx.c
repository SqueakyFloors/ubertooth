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

/* The channel a generic transmit uses when -c is left out: the bottom of the
 * band, which is the channel ubertooth(7), GENERIC RADIO MODE sets a receiving
 * board to. */
#define TX_GENERIC_DEFAULT_CHANNEL BT_FREQ_MIN

/*
 * The most packets one -n can ask for: the count travels in the request's
 * 16-bit wValue.
 */
#define TX_GENERIC_COUNT_MAX 65535

/*
 * The deadline on the wait for a burst, as a multiple of the packets asked
 * for, and how long to leave between reads of the board while it runs.
 *
 * Neither is a measurement: the first is a bound on how long a board may take
 * before the wait gives up rather than hang, and the second is how often it
 * is worth asking. A packet takes about 1.3 ms on the bench (2000 of them in
 * 1.57 s), so eight milliseconds each leaves room for a slower board.
 */
#define TX_GENERIC_MS_PER_PACKET 8
#define TX_GENERIC_POLL_US       20000

static void usage(FILE *file)
{
	fprintf(file, "ubertooth-tx - transmit from an Ubertooth\n");
	fprintf(file, "Usage:\n");
	fprintf(file, "\t-h this help\n");
	fprintf(file, "\t-V print version information\n");
	fprintf(file, "\n");
	fprintf(file, "    Transmit one generic packet (experimental):\n");
	fprintf(file, "\t-g <payload> hex digits, two per byte, 1 to %u bytes\n",
	        (unsigned)GENERIC_TX_FRAMED_PAYLOAD);
	fprintf(file, "\t-a <syncword> 8 hex digits, the sync word the board frames\n");
	fprintf(file, "\t   the packet with - required with -g\n");
	fprintf(file, "\t-c <%d-%d> channel in MHz [Default: %d]\n",
	        BT_FREQ_MIN, BT_FREQ_MAX, TX_GENERIC_DEFAULT_CHANNEL);
	fprintf(file, "\t-n <1-%d> transmit the packet this many times"
	        " [Default: 1]\n", TX_GENERIC_COUNT_MAX);
	fprintf(file, "\n");
	fprintf(file, "    Retransmit symbols dumped with ubertooth-dump:\n");
	fprintf(file, "\tWARNING: this half sends no dump yet; see ubertooth-tx(1)\n");
	fprintf(file, "\t-l <LAP> to transmit to (3 bytes / 6 hex digits)\n");
	fprintf(file, "\t-u <UAP> to transmit to (1 byte / 2 hex digits)\n");
	fprintf(file, "\t-t <SECONDS> timeout - 0 means no timeout [Default: 0]\n");
	fputs(OPT_USAGE_SELECT_DEVICE, file);
}

/*
 * Wait for the board to finish the burst it was given, so the command ends
 * when the air is clear rather than when the request was taken.
 *
 * The board is still transmitting when its answer to the request arrives, and
 * the one thing it will tell a host about it is the mode it is in, which it
 * answers between packets. So that is what is read, until it is no longer the
 * generic transmit.
 *
 * Ctrl-C stops the burst: the board takes a stop between packets, and the
 * read below is what notices that it did.
 */
static int wait_for_burst(ubertooth_t* ut, int count)
{
	int polls = (count * TX_GENERIC_MS_PER_PACKET * 1000)
	            / TX_GENERIC_POLL_US + 2;
	int stopped = 0;

	for (; polls > 0; polls--) {
		watchdog_info info;

		/*
		 * Waited before the first read, not after it. The board records
		 * the mode on the turn of its loop that starts the burst, which
		 * is after the one that answered the request, so a read sent
		 * straight away can see the mode it was in before and take that
		 * for a burst already over. One poll interval is far longer
		 * than the microseconds that turn takes.
		 */
		usleep(TX_GENERIC_POLL_US);
		if (!stopped && ubertooth_stopping(ut)) {
			/* Not ubertooth_stop(): that tears the capture handle
			 * down, and there is no capture here. The board takes
			 * the stop between packets. */
			stopped = 1;
			if (cmd_stop(ut->devh) < 0)
				return -1;
		}
		if (cmd_get_watchdog_quiet(ut->devh, &info) < 0)
			/* A board busy with the burst answers this; one that
			 * will not answer at all is reported by the read. */
			continue;
		if (info.mode != MODE_TX_GENERIC)
			return stopped ? 1 : 0;
	}
	fprintf(stderr, "error: the board was still transmitting after %d"
	        " packets' worth of time\n", count);
	return -1;
}

/*
 * Transmit the generic packet count times.
 *
 * One request carries the whole burst. The board answers no USB request while
 * a packet is going out, so a request for each packet loses the ones the host
 * sends inside those windows: measured on two boards, a burst of 30 sent as
 * fast as the host could send them had 15 come back as transfer errors. This
 * way the host sends one request and the board sends the packets back to
 * back, which also puts three times as many on the air a second.
 *
 * Nothing is printed on success.
 */
static int transmit_generic(ubertooth_t* ut, uint32_t syncword, int channel,
                            const uint8_t* payload, size_t payload_len,
                            int count)
{
	int pa_level;
	int r;

	/* The power these packets go out at belongs to the request, so it has
	 * to be stated rather than left to the radio. The board's own level,
	 * which ubertooth-util -a sets, is what every other transmit here uses;
	 * the firmware puts it back when the burst is over. */
	pa_level = cmd_get_palevel(ut->devh);
	if (pa_level < 0)
		return -1;

	/* A stop that arrived while the board was being opened: nothing has
	 * been asked of the radio yet, so there is nothing to stop. */
	if (ubertooth_stopping(ut))
		return 0;

	if (cmd_tx_generic_packet(ut->devh, syncword, (uint16_t)channel,
	                          (uint8_t)pa_level, payload, payload_len,
	                          (uint16_t)count) < 0)
		return -1;

	r = wait_for_burst(ut, count);
	if (r > 0)
		fprintf(stderr, "note: the transmit was stopped part way"
		        " through %d packets\n", count);
	return r < 0 ? -1 : 0;
}

int main(int argc, char* argv[])
{
	int opt, have_lap = 0, have_uap = 0;
	int r;
	int status;
	int timeout = 0;
	/* The generic packet: its payload, the sync word the board frames it
	 * with, the channel and how many times to send it. */
	uint8_t payload[GENERIC_TX_FRAMED_PAYLOAD];
	size_t payload_len = 0;
	int generic = 0, have_syncword = 0, have_channel = 0, have_count = 0;
	int have_timeout = 0;
	uint32_t syncword = 0;
	int channel = TX_GENERIC_DEFAULT_CHANNEL;
	int count = 1;
	/* -h and -V both end the command once the whole command line has
	 * parsed, and a command line with both prints both (see below).
	 * version_status is -V's own result: it reads the board, so it can fail
	 * where -h cannot. */
	int do_help = 0, do_version = 0, version_status = 0;
	opt_once once = {0};
	char* ubertooth_device = NULL;
	uint32_t lap = 0;
	uint8_t uap = 0;

	ubertooth_t* ut = NULL;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	/* Named, so the loop below and opt_report_bad_option()
	 * cannot be given different letters. */
	const char options[] = ":hVa:c:g:l:n:u:U:t:";
	opterr = 0;
	while ((opt=getopt(argc, argv, options)) != EOF) {
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
		switch(opt) {
		case 'g':
			if (opt_require_hex_bytes(optarg, "a generic payload", payload,
			                          sizeof(payload), &payload_len) < 0)
				return 1;
			generic = 1;
			break;
		case 'a':
			/* The whole 32-bit sync word, written out: the radio sends
			 * all four bytes of it, so a short value would be a
			 * different sync word rather than a shorter one. */
			if (opt_parse_hex(optarg, 8, 8, &syncword) < 0) {
				fprintf(stderr, "error: sync word must be 8 hex digits"
				        " (got %s)\n", optarg);
				return 1;
			}
			have_syncword = 1;
			break;
		case 'c':
			if (opt_require_int(optarg, BT_FREQ_MIN, BT_FREQ_MAX, "channel",
			                    " MHz", &channel) < 0)
				return 1;
			have_channel = 1;
			break;
		case 'n':
			/* The count goes in the request's 16-bit wValue, so
			 * that is the ceiling rather than an arbitrary one. */
			if (opt_require_int(optarg, 1, TX_GENERIC_COUNT_MAX,
			                    "transmit count", " packets",
			                    &count) < 0)
				return 1;
			have_count = 1;
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
		case 't':
			if (opt_require_count(optarg, 0, "timeout", "seconds",
			                      "0 for none", &timeout) < 0)
				return 1;
			have_timeout = 1;
			break;
		case 'V':
			do_version = 1;
			break;
		case 'h':
			do_help = 1;
			break;
		default:
			opt_report_bad_option(opt, argc, argv, options);
			return 1;
		}
	}

	/* ubertooth-tx takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h and -V end the command, but only once the whole command line
	 * has parsed: a bad option value or a stray word is reported even
	 * alongside them. A command line with both prints both, and the usage
	 * text comes last so the version is not scrolled off the screen by
	 * it; the order they were written in does not matter. */
	if (do_version)
		version_status = ubertooth_print_versions(stdout, ubertooth_device);
	if (do_help)
		usage(stdout);
	if (do_version || do_help)
		return version_status;

	/*
	 * The two transmits this tool has ask for different things of the
	 * board, so one command line cannot name both: a generic packet is
	 * framed by the sync word -a gives it and carries no piconet address,
	 * and it ends when the packets -n asked for have gone out rather than
	 * after -t. Each of these is an argument error, so all of them are
	 * refused before a device is opened, where the message would otherwise
	 * depend on whether a board happened to be free.
	 */
	if (generic && (have_lap || have_uap)) {
		fprintf(stderr, "error: -g cannot be combined with -%c;"
		        " a generic packet carries no piconet address\n",
		        have_lap ? 'l' : 'u');
		return 1;
	}
	if (generic && have_timeout) {
		fputs("error: -g cannot be combined with -t; a generic transmit"
		      " ends when the packets asked for have gone out\n", stderr);
		return 1;
	}
	/* Checked before the options that only make sense with -g, so a
	 * command line that gives half an address gets the same answer whether
	 * or not it also carries one of those. */
	if (!generic && (have_lap || have_uap) && !(have_lap && have_uap)) {
		fputs("error: -l <LAP> and -u <UAP> are both required\n", stderr);
		return 1;
	}
	if (!generic && (have_syncword || have_channel || have_count)) {
		fputs("error: -a, -c and -n are only used with -g, which transmits"
		      " a generic packet\n", stderr);
		return 1;
	}
	/* The sync word is the whole of what a receiving board has to be set
	 * to, and the board has no default for it, so it is required rather
	 * than guessed. */
	if (generic && !have_syncword) {
		fputs("error: -g needs -a <syncword>: the board frames the generic"
		      " packet with it\n", stderr);
		return 1;
	}
	if (!generic && !have_lap && !have_uap) {
		fputs("error: nothing to do: give -g to transmit a generic packet,"
		      " or -l and -u to retransmit to a piconet\n", stderr);
		if (opt_no_arguments(argc))
			usage(stderr);
		return 1;
	}

	/* Allocated here rather than at the top of main(): every refusal above
	 * returns without a device, and the handle carries a 72 MB fifo. */
	ut = ubertooth_init();
	if (ut == NULL)
		return 1;

	/* Stop on Ctrl-C from here on, before the board is opened: a signal
	 * that arrives while one is being found would otherwise be discarded. */
	ubertooth_catch_stop_signals();

	r = ubertooth_connect_select(ut, ubertooth_device);
	if (r < 0) {
		/* ubertooth_connect_select() has already closed whatever it
		 * opened, so only the handle itself is left to give back. */
		ubertooth_free(ut);
		return ubertooth_open_stopped() ? 0 : 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0) {
		/* ubertooth_check_api() releases the board itself when it
		 * refuses the firmware, and leaves it open when it could not
		 * read it. Closing covers both, because closing a board already
		 * released does nothing. What is not added here is a second
		 * STOP. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		return 1;
	}

	/* Stopped while the board was being opened: the run ends here, with the
	 * board given back and nothing transmitted. */
	if (ubertooth_stop_requested()) {
		ubertooth_close(ut);
		ubertooth_free(ut);
		return 0;
	}

	/* The generic transmit is its own path from here: one request, then the
	 * wait for the board to finish the burst. It ends by itself, so the
	 * teardown the dump retransmit below needs does not apply. */
	if (generic) {
		r = transmit_generic(ut, syncword, channel, payload, payload_len,
		                     count);
		ubertooth_close(ut);
		ubertooth_free(ut);
		return r < 0 ? 1 : 0;
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

	if (timeout)
		ubertooth_set_timeout(ut, timeout);

	// tell ubertooth to send packets
	r = cmd_tx_syms(ut->devh);
	if (r < 0) {
		ubertooth_stop(ut);
		ubertooth_free(ut);
		return 1;
	}

	// TODO - send symbols to the bulk endpoint

	// wait
	while(!ubertooth_stopping(ut))
		sleep(1);

	ubertooth_stop(ut);
	/* A teardown the board refused left it transmitting, and no other tool
	 * can open it until it is reset or re-plugged, so the run did not do
	 * what was asked. libubertooth has already named the refused request. */
	status = ubertooth_stop_failed(ut) ? 1 : 0;
	ubertooth_free(ut);

	return status;
}
