/*
 * Copyright 2012-2016 Michael Ryan
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
#include "btle_options.h"
#include "opt_parse.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

/* The BLE advertising channel indexes (Bluetooth Core spec: 37, 38, 39). */
#define ADV_INDEX_MIN 37
#define ADV_INDEX_MAX 39

/* An access address is 32 bits, so at most 32 of its bits can be wrong. */
#define AA_ERRORS_MAX 32

/* Set by the SIGUSR1 handler to cancel following an active connection. A
 * signal handler may assign to one object that outlives it, and only one
 * declared volatile sig_atomic_t; a plain int could also be read once by the
 * loop below and the copy kept, which would never see the signal. Stopping
 * the run is libubertooth's flag instead (ubertooth_stopping()). */
static volatile sig_atomic_t cancel_follow;

static void cancel_follow_handler(int sig __attribute__((unused))) {
	cancel_follow = 1;
}

/* Where the lines that answer an option go: the access address, the CRC verify
 * setting and the target. They are what the user asked for, so standard
 * output, unless a capture has taken it ("-q -", "-c -"), which redirects this
 * process's stdout to /dev/null for the whole run; writing them there would do
 * the work on the board and throw the answer away, so they go to standard
 * error instead. main() sets this; nothing here prints before that. */
static FILE *report_stream = NULL;

/* The library has already said why a request was refused; this names the
 * request. A refused request is not proof that the board never got it -- a
 * transfer can time out after it arrived -- so the message says only what the
 * tool knows. */
static void report_request_failure(const char *what)
{
	fprintf(stderr, "error: could not %s\n", what);
}

/* The centre frequency of a BLE advertising channel index (Bluetooth Core
 * spec), which both the sniffing and the faux slave mode tune to. */
static u16 adv_channel(int index)
{
	if (index == ADV_INDEX_MIN)
		return 2402;
	if (index == ADV_INDEX_MIN + 1)
		return 2426;
	return 2480;
}

static void print_mac(FILE *out, uint8_t *mac_address, int mask) {
	unsigned i;
	for (i = 0; i < 5; ++i)
		fprintf(out, "%02x:", mac_address[i]);
	fprintf(out, "%02x", mac_address[5]);
	if (mask >= 0)
		fprintf(out, "/%u", mask);
}

/* Accept "-" as an alias for binary PCAP on stdout. Save the original
 * stdout descriptor for libbtbb to open via /dev/fd (portable to macOS and
 * Linux), then redirect regular printf output to /dev/null so it cannot corrupt
 * the capture stream. /proc/self/fd is a Linux fallback. */
static char *handle_special_filename(const char *filename, int *saved_fd)
{
	char path[64];
	int null_fd;

	*saved_fd = -1;
	if (strcmp(filename, "-") != 0)
		return strdup(filename);

	if (opt_check_stdout_not_tty("capture data") < 0)
		return NULL;

	fflush(stdout);
	*saved_fd = dup(STDOUT_FILENO);
	if (*saved_fd < 0) {
		fprintf(stderr, "error: cannot save stdout for the capture: %s\n",
		        strerror(errno));
		return NULL;
	}

	null_fd = open("/dev/null", O_WRONLY);
	if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0) {
		fprintf(stderr, "error: cannot redirect stdout to /dev/null: %s\n",
		        strerror(errno));
		if (null_fd >= 0)
			close(null_fd);
		close(*saved_fd);
		*saved_fd = -1;
		return NULL;
	}
	if (null_fd != STDOUT_FILENO)
		close(null_fd);

	snprintf(path, sizeof(path), "/dev/fd/%d", *saved_fd);
	if (access(path, W_OK) != 0) {
		snprintf(path, sizeof(path), "/proc/self/fd/%d", *saved_fd);
		if (access(path, W_OK) != 0) {
			fprintf(stderr, "error: no writable descriptor filesystem for stdout capture\n");
			close(*saved_fd);
			*saved_fd = -1;
			return NULL;
		}
	}

	return strdup(path);
}

/* Carry out the -v options: the set first, then the read, so one command can
 * do both and the read reports the value it just set. The caller stops a
 * started mode before it gives up on a failure here. The set keeps its own
 * line only when nothing reads afterwards, so a set and a read print one line
 * between them. Returns 0, or -1 once the failure has been reported. */
static int apply_crc_options(struct libusb_device_handle* devh, int do_set_crc,
                             int do_get_crc)
{
	if (do_set_crc >= 0) {
		if (cmd_set_crc_verify(devh, do_set_crc) < 0) {
			fprintf(stderr, "error: CRC verify set failed\n");
			return -1;
		}
		if (!do_get_crc)
			fprintf(report_stream, "CRC: %sverify\n",
			        do_set_crc ? "" : "DO NOT ");
	}
	if (do_get_crc) {
		int r = cmd_get_crc_verify(devh);
		if (r < 0) {
			/* a short reply's whole error line is already printed */
			if (r != UBERTOOTH_ERROR_SHORT_REPLY)
				fprintf(stderr, "error: CRC verify read failed\n");
			return -1;
		}
		fprintf(report_stream, "CRC: %sverify\n", r ? "" : "DO NOT ");
	}
	return 0;
}

/* The usage text: on standard output for -h, on standard error for a bare
 * command line. */
static void usage(FILE *out)
{
	fprintf(out, "ubertooth-btle - passive Bluetooth Low Energy monitoring\n");
	fprintf(out, "Usage:\n");
	fprintf(out, "\t-h this help\n");
	fprintf(out, "\n");
	fprintf(out, "    Major modes:\n");
	fprintf(out, "\t-f follow connections\n");
	fprintf(out, "\t-n don't follow, only print advertisements\n");
	fprintf(out, "\t-p promiscuous: sniff active connections\n");
	fprintf(out, "\n");
	fprintf(out, "\t-a [address] get/set access address, 1-8 hex digits (example: -a 8e89bed6)\n");
	fprintf(out, "\t-s <address> faux slave mode, using MAC addr (example: -s 22:44:66:88:aa:cc)\n");
	fprintf(out, "\t       -a and -s do not go with -f, -n or -p, which run until interrupted\n");
	fprintf(out, "\t-t <address> set connection following target (example: -t 22:44:66:88:aa:cc/48)\n");
	fprintf(out, "\t-t none unset connection following target\n");
	fprintf(out, "\n");
	fprintf(out, "    Interference (only with -f or -p):\n");
	fprintf(out, "\t-i, -j  interfere with (jam) one connection and return to idle\n");
	fprintf(out, "\t-I, -J  interfere continuously\n");
	fprintf(out, "\n");
	fprintf(out, "    Data source:\n");
	fputs(OPT_USAGE_SELECT_DEVICE, out);
	fprintf(out, "\n");
	fprintf(out, "    Misc:\n");
	fprintf(out, "\t-r <filename> capture packets to PCAPNG file\n");
	fprintf(out, "\t-q <filename> capture packets to PCAP file (DLT_BLUETOOTH_LE_LL_WITH_PHDR)\n");
	fprintf(out, "\t-c <filename> capture packets to PCAP file (DLT_PPI + DLT_BLUETOOTH_LE_LL)\n");
	fprintf(out, "\t-A <%d-%d> advertising channel index (default %d)\n",
	       ADV_INDEX_MIN, ADV_INDEX_MAX, ADV_INDEX_MIN);
	fprintf(out, "\t-v [01] verify CRC mode, read status (-v) or enable/disable (-v 1, -v 0);\n");
	fprintf(out, "\t       both in one command sets first, then reports the new value\n");
	fprintf(out, "\t-x <0-%d> allow n access address offenses (default %d)\n",
	       AA_ERRORS_MAX, AA_ERRORS_MAX);

	fprintf(out, "\nIn get/set mode no capture occurs.\n");
}

/* The tool's exit once the handle exists: the board and the handle both go
 * back, as libubertooth asks of every tool.
 *
 * The close is unconditional because it is harmless: on a handle the capture
 * block has already stopped, and on a board ubertooth_check_api() refused, it
 * does nothing. On the paths that reach here with the board still open -- the
 * queries, and a request that failed -- it is what releases the interface and
 * closes the capture file. It sends no STOP, so a board left advertising by
 * -s keeps advertising, as it did before. */
static int release_handle(ubertooth_t* ut, int status)
{
	ubertooth_close(ut);
	ubertooth_free(ut);
	return status;
}

int main(int argc, char *argv[])
{
	int opt;
	opt_once once = {0};
	/* -h ends the command once the whole command line has parsed (see below). */
	int first_exit_option = 0;
	int do_follow, do_no_follow, do_promisc;
	int do_get_aa, do_set_aa;
	/* The CRC verify set and read have their own state, so one command can
	 * do both: -1 means no set was asked for, 0 or 1 the value to set, and
	 * do_get_crc is a plain flag. They shared one variable, so -v1 -v read
	 * only and -v -v1 set only, dropping the other half with exit 0. */
	int do_set_crc, do_get_crc;
	int do_adv_index;
	int do_slave_mode;
	int do_target;
	enum jam_modes jam_mode = JAM_NONE;
	/* The interference option as the user wrote it, so the refusal below
	 * names that spelling rather than a canonical one. */
	int jam_option = 0;
	/* 1 once the capture stopped short, so the exit status says the command
	 * did not do what was asked. */
	int status = 0;
	char* ubertooth_device = NULL;
	/* Capture files named on the command line, opened after every check. */
	const char* pcapng_name = NULL;
	const char* pcap_name = NULL;
	int pcap_ppi = 0; /* the -c format rather than the -q one */
	/* Allocated once the whole command line has been accepted (below):
	 * every refusal above returns without a board, and the handle carries a
	 * 72 MB fifo. */
	ubertooth_t* ut = NULL;

	btle_options cb_opts = { .allowed_access_address_errors = 32 };

	int r;
	u32 access_address = 0;
	uint8_t mac_address[6] = { 0, };
	uint8_t mac_mask = 0;

	/* Standard output until a capture takes it; see report_stream. The
	 * callback reports on the same stream. */
	report_stream = stdout;
	cb_opts.report_stream = report_stream;
	do_follow = do_no_follow = do_promisc = 0;
	do_get_aa = do_set_aa = 0;
	do_set_crc = -1;
	do_get_crc = 0;
	do_adv_index = 37;
	do_slave_mode = do_target = 0;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":a::r:hfnpU:v::A:s:t:x:c:q:jJiI")) != EOF) {
		/* getopt only attaches an optional value (-a8e89bed6, -v1); take
		 * a following word that could be the value as the value too, so
		 * "-a 8e89bed6" sets and "-a zzzz" is refused rather than
		 * silently reading. ubertooth-btle takes no other operands.
		 * Resolved before the check below, which only counts an option
		 * that came with a value. */
		if (optarg == NULL && (opt == 'a' || opt == 'v'))
			optarg = opt_separate_value(argc, argv,
			                            opt == 'a' ? opt_value_not_option
			                                       : opt_value_decimal);
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
		switch(opt) {
		case 'a':
			if (optarg == NULL) {
				do_get_aa = 1;
			} else {
				if (opt_parse_access_address(optarg, &access_address) < 0) {
					fprintf(stderr, "error: access address must be 1-8 hex digits (got %s)\n", optarg);
					return 1;
				}
				do_set_aa = 1;
			}
			break;
		case 'f':
			do_follow = 1;
			break;
		case 'n':
			do_no_follow = 1;
			break;
		case 'p':
			do_promisc = 1;
			break;
		case 'U':
			ubertooth_device = optarg;
			break;
		/* The capture files are named here and opened once the whole
		 * command line has been accepted, so a refused command leaves
		 * the filesystem alone. -q and -c share one PCAP handle, so the
		 * first of the two given wins, as before. */
		case 'r':
			if (strcmp(optarg, "-") == 0) {
				fprintf(stderr, "error: PCAPNG capture to stdout is not supported; use -q - or -c -\n");
				return 1;
			}
			if (pcapng_name == NULL)
				pcapng_name = optarg;
			else
				fprintf(stderr, "warning: ignoring extra capture file: %s\n", optarg);
			break;
		case 'q':
		case 'c':
			if (pcap_name == NULL) {
				pcap_name = optarg;
				pcap_ppi = (opt == 'c');
			} else {
				fprintf(stderr, "warning: ignoring extra capture file: %s\n", optarg);
			}
			break;
		case 'v':
			/* -v reads the CRC verify setting; -v0 / -v1 sets it, written
			 * attached or as the next word (taken above). */
			if (optarg) {
				if (opt_require_int(optarg, 0, 1, "CRC verify mode", "",
				                    &do_set_crc) < 0)
					return 1;
			} else
				do_get_crc = 1;
			break;
		case 'A':
			if (opt_require_int(optarg, ADV_INDEX_MIN, ADV_INDEX_MAX,
			                    "advertising channel index", "",
			                    &do_adv_index) < 0)
				return 1;
			break;
		case 's':
			do_slave_mode = 1;
			r = convert_mac_address(optarg, mac_address, &mac_mask);
			if (!r)
				return 1;
			break;
		case 't':
			do_target = 1;
			r = convert_mac_address(optarg, mac_address, &mac_mask);
			if (!r)
				return 1;
			break;
		case 'x': {
			int offenses;
			if (opt_require_int(optarg, 0, AA_ERRORS_MAX,
			                    "access address offenses", "",
			                    &offenses) < 0)
				return 1;
			cb_opts.allowed_access_address_errors = (unsigned)offenses;
			break;
		}
		case 'i':
		case 'j':
			jam_mode = JAM_ONCE;
			jam_option = opt;
			break;
		case 'I':
		case 'J':
			jam_mode = JAM_CONTINUOUS;
			jam_option = opt;
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


	/* ubertooth-btle takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. "-"
	 * is only a value of -q/-c/-r, which getopt has already taken. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h ends the command, but only once the whole command line has
	 * parsed: a bad option value or a stray word is reported even
	 * alongside it. */
	if (first_exit_option == 'h') {
		usage(stdout);
		return 0;
	}

	/* -a reads and -a<address> sets; one command cannot do both (the read
	 * used to win and the set was dropped). */
	if (do_get_aa && do_set_aa) {
		fprintf(stderr, "error: -a cannot both read and set the access address\n");
		return 1;
	}

	/* One major mode per command. Another argument error, so it is refused
	 * here rather than after a device has been opened. */
	if (do_follow + do_no_follow + do_promisc > 1) {
		fprintf(stderr, "error: choose one of -f, -n or -p, pick one pal\n");
		return 1;
	}

	/* A sniffing mode runs until it is interrupted and ends in
	 * ubertooth_stop(), which closes the device handle and nulls it, so
	 * the access address and faux slave mode blocks below have no handle
	 * left to send on. -t is not in this list: it is sent before the mode
	 * starts, which is what it is for. */
	if ((do_follow || do_no_follow || do_promisc) &&
	    (do_get_aa || do_set_aa || do_slave_mode)) {
		fprintf(stderr, "error: -a and -s do not go with -f, -n or -p:"
		        " a sniffing mode runs until it is interrupted\n");
		return 1;
	}

	/* Interference acts on a connection, so it needs the mode that has one:
	 * -n only logs advertising packets. Refused here, before a device is
	 * opened, like the other combination checks above. It comes before the
	 * "nothing to do" check below so that -i on its own gets this message
	 * too: the page promises it for "-i or -I without -f or -p", and
	 * naming the two modes that can carry interference says more than
	 * listing every major mode. */
	if (jam_mode != JAM_NONE && !(do_follow || do_promisc)) {
		fprintf(stderr, "error: -%c needs -f or -p: there is no"
		        " connection to interfere with otherwise\n", jam_option);
		return 1;
	}

	/* A command line with no work in it: refused here, before a device is
	 * opened and before any capture file is created. It used to open the
	 * selected board, check its API version, send nothing, print the usage
	 * text on stdout and exit 0, so a command that did nothing needed a
	 * board and looked like one that had worked. -A, -x and the capture
	 * options only shape the work the options below ask for. */
	if (!(do_follow || do_no_follow || do_promisc || do_get_aa || do_set_aa ||
	      do_set_crc >= 0 || do_get_crc || do_slave_mode || do_target)) {
		fprintf(stderr, "error: nothing to do: choose a major mode"
		        " (-f, -n or -p), or -a, -v, -s or -t\n");
		if (opt_no_arguments(argc))
			usage(stderr);
		return 1;
	}

	/* Every check has passed, so the handle and its 72 MB fifo can be
	 * allocated: the capture files below are kept on it. */
	ut = ubertooth_init();
	if (ut == NULL)
		return 1;

	/* And the capture files can be opened: a refused command line creates
	 * and truncates nothing. A bare "-" redirects this process's stdout,
	 * which is why nothing may be printed to stdout after this point except
	 * the capture itself, and why the lines that answer an option go to
	 * report_stream rather than straight to stdout. */
	if (pcapng_name != NULL) {
		if (lell_pcapng_create_file(pcapng_name, "Ubertooth",
		                            &ut->h_pcapng_le)) {
			fprintf(stderr, "error: %s: %s\n", pcapng_name,
			        strerror(errno));
			return release_handle(ut, 1);
		}
	}
	if (pcap_name != NULL) {
		int saved_fd;
		char *file = handle_special_filename(pcap_name, &saved_fd);
		int create_result;

		if (!file) {
			if (saved_fd >= 0)
				close(saved_fd);
			return release_handle(ut, 1);
		}
		create_result = pcap_ppi
		        ? lell_pcap_ppi_create_file(file, 0, &ut->h_pcap_le)
		        : lell_pcap_create_file(file, &ut->h_pcap_le);
		if (saved_fd >= 0) {
			/* The capture has taken stdout, which is now /dev/null
			 * for the rest of this run: the lines that answer an
			 * option go to standard error instead, and the callback
			 * leaves out the per-packet dump that would go nowhere. */
			report_stream = stderr;
			cb_opts.report_stream = report_stream;
			cb_opts.capture_took_stdout = 1;
			close(saved_fd);
		}
		free(file);
		if (create_result) {
			/* The name the user gave, not the /dev/fd path a bare
			 * "-" is opened through. */
			fprintf(stderr, "error: %s: %s\n", pcap_name,
			        strerror(errno));
			return release_handle(ut, 1);
		}
	}

	r = ubertooth_connect_select(ut, ubertooth_device);
	if (r < 0) {
		return release_handle(ut, 1);
	}

	r = ubertooth_check_api(ut);
	if (r < 0) {
		/* release_handle() closes the board, which covers both of the
		 * check's failures: it released a refused version itself, and a
		 * version it could not read at all leaves the board open. */
		return release_handle(ut, 1);
	}

	/* Stop on Ctrl-C, and on the other signals that mean the run is over
	 * (SIGTERM, SIGHUP, SIGQUIT, an alarm): the library's handlers, so the
	 * capture loop below ends the way every other tool's does. */
	register_cleanup_handler(ut, 0);

	// cancel following on USR1
	signal(SIGUSR1, cancel_follow_handler);

	// allow user to set target before following
	if (do_target) {
		r = cmd_btle_set_target(ut->devh, mac_address, mac_mask);
		if (r == 0) {
			if (mac_mask == 0) {
				fprintf(report_stream, "target cleared\n");
			} else {
				fprintf(report_stream, "target set to: ");
				print_mac(report_stream, mac_address, mac_mask);
				fprintf(report_stream, "\n");
			}
		} else {
			fprintf(stderr, "error: target set failed\n");
			return release_handle(ut, 1);
		}
	}

	if (do_follow || do_no_follow || do_promisc) {
		usb_pkt_rx rx;

		r = cmd_set_jam_mode(ut->devh, jam_mode);
		if (jam_mode != JAM_NONE && r != 0) {
			fprintf(stderr, "error: jamming not supported by this firmware\n");
			return release_handle(ut, 1);
		}
		/* A mode the board would not enter must not be followed by a
		 * capture loop that reports a clean run, so each request below
		 * is checked. Each failure stops the board first: the mode
		 * request may have reached it even though the reply did not
		 * come back, and a board left part-way into a low-energy mode
		 * is the state it stops answering control requests in. */
		if (cmd_set_modulation(ut->devh, MOD_BT_LOW_ENERGY) < 0) {
			fprintf(stderr, "error: modulation set failed\n");
			ubertooth_stop(ut);
			return release_handle(ut, 1);
		}

		if (do_follow || do_no_follow) {
			/* Checked one at a time, so the report names the
			 * request that failed rather than the mode it was
			 * part of. */
			if (cmd_set_channel(ut->devh,
			                    adv_channel(do_adv_index)) < 0) {
				report_request_failure(
					"set the advertising channel");
				ubertooth_stop(ut);
				return release_handle(ut, 1);
			}
			if (cmd_btle_sniffing(ut->devh, do_follow) < 0) {
				fprintf(stderr, "error: could not start %sfollow"
				        " mode\n", do_follow ? "" : "no-");
				ubertooth_stop(ut);
				return release_handle(ut, 1);
			}
			/* This mode runs the low-energy physical-layer engine,
			 * which stamps packets from timer1, so its clock starts
			 * over at 2^32 ticks rather than the CLKN-derived
			 * 3125 * 2^20. */
			ut->clk100ns_period = CLK100NS_TIMER1_PERIOD;
		} else {
			if (cmd_btle_promisc(ut->devh) < 0) {
				fprintf(stderr, "error: could not start"
				        " promiscuous mode\n");
				ubertooth_stop(ut);
				return release_handle(ut, 1);
			}
		}

		// The -v options run *after* the mode has started (mode entry runs
		// reset_le() which clears crc_verify) but *before* the capture loop —
		// otherwise they only ran after streaming ended, making -v1 a no-op
		// for sniffing modes. A read here reports the setting the capture
		// starts with; the firmware turns verification on and off itself as it
		// recovers a connection.
		// The mode is running by now, so a failure here has to stop it before
		// giving up: a board abandoned in a low-energy mode is the state it
		// stops answering control requests in.
		if (apply_crc_options(ut->devh, do_set_crc, do_get_crc) < 0) {
			ubertooth_stop(ut);
			return release_handle(ut, 1);
		}

		while (!ubertooth_stopping(ut)) {
			if (cancel_follow) {
				cmd_cancel_follow(ut->devh);
				cancel_follow = 0;
			}
			int r = cmd_poll(ut->devh, &rx);
			if (r < 0) {
				/* The capture stopped short, so the command did
				 * not do what was asked: the exit status has
				 * to say so, or a caller cannot tell this from
				 * a clean Ctrl-C. */
				fprintf(stderr, "error: USB poll failed\n");
				status = 1;
				break;
			}
			if (r == sizeof(usb_pkt_rx)) {
				/* The packet cmd_poll() just handed back, so it arrived now. */
				fifo_push(ut->fifo, &rx, ubertooth_packet_arrival_ns());
				cb_btle(ut, &cb_opts);
			} else {
				/* Not a whole record, so there is no packet to
				 * read out of it: say so rather than drop it
				 * in silence. */
				ubertooth_warn_if_poll_reply_short(ut, r);
			}
			usleep(500);
		}
		ubertooth_stop(ut);
	}

	if (do_get_aa) {
		int r = cmd_read_access_address(ut->devh, &access_address);
		if (r < 0) {
			/* a short reply's whole error line is already printed */
			if (r != UBERTOOTH_ERROR_SHORT_REPLY)
				fprintf(stderr, "error: access address read failed\n");
			return release_handle(ut, 1);
		}
		fprintf(report_stream, "Access address: %08x\n", access_address);
		/* carry on, so a -v in the same command still runs */
	}

	if (do_set_aa) {
		if (cmd_set_access_address(ut->devh, access_address) < 0) {
			fprintf(stderr, "error: access address set failed\n");
			return release_handle(ut, 1);
		}
		fprintf(report_stream, "access address set to: %08x\n", access_address);
	}

	// For sniffing modes this was already handled before the capture loop.
	if (!(do_follow || do_no_follow || do_promisc)) {
		if (apply_crc_options(ut->devh, do_set_crc, do_get_crc) < 0)
			return release_handle(ut, 1);
	}

	if (do_slave_mode) {
		/* flags: LE Limited Discovery */
		uint8_t adv_data[] = { 0x02, 0x01, 0x05 };

		/* Checked one at a time, so the report names the request that
		 * failed: all three had to work for the board to advertise,
		 * and "could not start faux slave mode" did not say which of
		 * them the board refused.
		 *
		 * The first two set something up and start nothing, so a
		 * failure leaves the board idle and it is left alone, as
		 * ubertooth-scan leaves it: a STOP to a board that is not
		 * answering would only add a second failure to the report. */
		if (cmd_set_channel(ut->devh, adv_channel(do_adv_index)) < 0) {
			report_request_failure("set the advertising channel");
			return release_handle(ut, 1);
		}
		if (cmd_le_set_adv_data(ut->devh, adv_data,
		                        sizeof(adv_data)) < 0) {
			report_request_failure("set the advertising data");
			return release_handle(ut, 1);
		}
		/* The last one starts advertising, so it is stopped on the way
		 * out, as the sniffing block stops its mode: the request may
		 * have reached the board even though the reply did not come
		 * back, and a board left part-way into a low-energy mode is
		 * the state it stops answering control requests in. The
		 * success path deliberately leaves it advertising. */
		if (cmd_btle_slave(ut->devh, mac_address) < 0) {
			report_request_failure("start advertising");
			ubertooth_stop(ut);
			return release_handle(ut, 1);
		}
	}

	return release_handle(ut, status);
}
