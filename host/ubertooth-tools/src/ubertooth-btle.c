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
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>

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
 * output, unless a capture has taken it ("-q -", "-c -", "-d -"), which
 * redirects this process's stdout to /dev/null for the whole run; writing them
 * there would do the work on the board and throw the answer away, so they go
 * to standard error instead. main() sets this; nothing here prints before
 * that. */
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
 * spec), which both the sniffing and the faux peripheral mode tune to. */
static u16 adv_channel(int index)
{
	if (index == ADV_INDEX_MIN)
		return BTLE_ADV_CHANNEL_37_MHZ;
	if (index == ADV_INDEX_MIN + 1)
		return BTLE_ADV_CHANNEL_38_MHZ;
	return BTLE_ADV_CHANNEL_39_MHZ;
}

static void print_mac(FILE *out, uint8_t *mac_address, int mask) {
	unsigned i;
	for (i = 0; i + 1 < OPT_BD_ADDR_BYTES; ++i)
		fprintf(out, "%02x:", mac_address[i]);
	fprintf(out, "%02x", mac_address[OPT_BD_ADDR_BYTES - 1]);
	if (mask >= 0)
		fprintf(out, "/%u", mask);
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
	fprintf(out, "\t-V print version information\n");
	fprintf(out, "\n");
	fprintf(out, "    Major modes:\n");
	fprintf(out, "\t-f follow connections\n");
	fprintf(out, "\t-n don't follow, only print advertisements\n");
	fprintf(out, "\t-p promiscuous: sniff active connections\n");
	fprintf(out, "\t       -f captures all of an algorithm 2 connection, -p part of it\n");
	fprintf(out, "\n");
	fprintf(out, "\t-a [address] get/set access address, 1-8 hex digits (example: -a 8e89bed6)\n");
	fprintf(out, "\t-s <address> faux peripheral mode, advertising from one whole address\n");
	fprintf(out, "\t       (example: -s 22:44:66:88:aa:cc; no \"/<bits>\" mask, no \"none\")\n");
	fprintf(out, "\t       -a and -s do not go with -f, -n or -p, which run until interrupted\n");
	fprintf(out, "\t-t <address> set connection following target (example: -t 22:44:66:88:aa:cc/48)\n");
	fprintf(out, "\t-t none unset connection following target\n");
	fprintf(out, "\t       -t is for -f and -n; any -t, \"none\" included, is refused with -p\n");
	fprintf(out, "\n");
	fprintf(out, "    Interference (only with -f or -p):\n");
	fprintf(out, "\t-i, -j  interfere with (jam) one connection and return to idle (inert; see man page)\n");
	fprintf(out, "\t-I, -J  interfere continuously (inert; see man page)\n");
	fprintf(out, "\n");
	fprintf(out, "    Data source:\n");
	fputs(OPT_USAGE_SELECT_DEVICE, out);
	fprintf(out, "\t-R <filename> replay a dump saved with -d, with its recorded times\n");
	fprintf(out, "\t       - if not specified use Ubertooth for live capture\n");
	fprintf(out, "\t       - needs the major mode the dump was recorded with, and\n");
	fprintf(out, "\t         goes with no option that drives a board\n");
	fprintf(out, "\n");
	fprintf(out, "    Misc:\n");
	fprintf(out, "\t-r <filename> capture packets to PCAPNG file\n");
	fprintf(out, "\t-q <filename> capture packets to PCAP file (DLT_BLUETOOTH_LE_LL_WITH_PHDR)\n");
	fprintf(out, "\t-d <filename> dump records to binary file, for -R to replay\n");
	fprintf(out, "\t-c <filename> capture packets to PCAP file (DLT_PPI + DLT_BLUETOOTH_LE_LL)\n");
	fprintf(out, "\t-A <%d-%d> advertising channel index (default %d);"
	       " not with -p\n",
	       ADV_INDEX_MIN, ADV_INDEX_MAX, ADV_INDEX_MIN);
	fprintf(out, "\t-k [01] verify CRC mode, read status (-k) or enable/disable (-k 1, -k 0);\n");
	fprintf(out, "\t       both in one command sets first, then reports the new value\n");
	fprintf(out, "\t-v verbose: describe each packet whose data could not be read,\n");
	fprintf(out, "\t       which a run otherwise sums up in one line at the end;\n");
	fprintf(out, "\t       needs -f, -n or -p, and takes no value at all (-k is\n");
	fprintf(out, "\t       the CRC verify setting it named once)\n");
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
 * -s keeps advertising, as it did before.
 *
 * The dump this run wrote and the dump it replayed are closed here too, on
 * every path that reaches it, so neither is left open when the process ends.
 * Each record is flushed as it is written, so nothing is waiting in the dump;
 * the close is what releases the file. */
static int release_handle(ubertooth_t* ut, int status)
{
	ubertooth_close(ut);
	ubertooth_free(ut);
	if (dumpfile != NULL) {
		fclose(dumpfile);
		dumpfile = NULL;
	}
	if (infile != NULL) {
		fclose(infile);
		infile = NULL;
	}
	return status;
}

/* Set once a replay has read a whole record from its input. That is the line
 * its capture files are kept from, the same line a live capture draws at its
 * first packet: the replay ran, and what the decoder made of the record is the
 * result. Before the first record the files hold their headers and nothing
 * else. */
static int replay_read_a_record;

static void replay_decode(ubertooth_t* ut, void* args)
{
	replay_read_a_record = 1;
	cb_btle(ut, args);
}

/* Refuses an output file that is the dump a replay is reading. Every writer
 * here opens its file for writing, which truncates it, so a run given one file
 * for both would empty the dump before reading a record of it -- and a dump is
 * the only copy of what promiscuous mode worked out about a connection.
 *
 * The file is identified rather than the name, by device and inode numbers, so
 * "./x.bin" beside "x.bin", a symbolic link and a hard link are all caught.
 * Anything the two share counts, a character device included, because
 * comparing only regular files would let a named pipe through.
 *
 * A lone "-" has no name to compare and is this process's standard output, so
 * that descriptor is what gets looked at. That catches the one destination a
 * name cannot: a shell that has already pointed standard output at the dump,
 * as in `-R x.bin -d - >> x.bin`, which reads the records it is appending and
 * never ends. An input that turns out to hold descriptor 1 is not that case:
 * standard output was closed before this run started and fopen() took the
 * number. The dump writer reports the closed output itself, when its fdopen()
 * of a descriptor opened for reading fails.
 *
 * A destination that cannot be stat()ed is not identified: a name that does
 * not exist yet is the ordinary case, and for the rest -- a directory on the
 * path that cannot be searched, a symbolic link loop -- the writer is about to
 * report the real reason itself.
 *
 * Called with the input open and before any writer opens, so a refused run
 * has written nothing. Returns -1 once the refusal is printed. */
static int output_is_the_replay(const char* output_name, char option)
{
	struct stat input_file, output_file;
	int to_stdout = opt_writes_to_stdout(output_name);

	if (infile == NULL || output_name == NULL)
		return 0;
	if (fstat(fileno(infile), &input_file) != 0)
		return 0;
	if (to_stdout) {
		if (fileno(infile) == fileno(stdout) ||
		    fstat(fileno(stdout), &output_file) != 0)
			return 0;
	} else if (stat(output_name, &output_file) != 0) {
		return 0;
	}
	if (input_file.st_dev != output_file.st_dev ||
	    input_file.st_ino != output_file.st_ino)
		return 0;
	fprintf(stderr, "error: -%c would write over the dump -R is replaying"
	        " (%s)\n", option,
	        to_stdout ? "standard output" : output_name);
	return -1;
}

/* How much of the decode could not be read, in one line after the packets it
 * summarizes. Says nothing when every packet decoded.
 *
 * It goes on the stream the run reports on, which is where the packet lines it
 * is about went: a caller that threw those away -- btle-extcap.py, which sends
 * them to /dev/null -- threw this away with them, instead of leaving Wireshark
 * to report a good capture as an error.
 *
 * A run whose capture took standard output never reaches this: the decode
 * stops before the field that would be counted, so nothing is counted. That
 * is also why the line may point at -v, which such a run is refused. */
static void report_malformed_packets(const btle_options* opts, int verbose)
{
	if (opts->malformed_packets == 0)
		return;
	fprintf(report_stream, "note: %u packet%s held data that could not be"
	        " decoded%s\n", opts->malformed_packets,
	        opts->malformed_packets == 1 ? "" : "s",
	        verbose ? "" : "; -v describes each one");
}

int main(int argc, char *argv[])
{
	int opt;
	opt_once once = {0};
	/* -h ends the command once the whole command line has parsed (see below),
	 * and so does -V, whose own result is version_status: it reads the board,
	 * so it can fail where -h cannot. */
	int first_exit_option = 0;
	int do_version = 0, version_status = 0;
	int do_follow, do_no_follow, do_promisc;
	int do_get_aa, do_set_aa;
	/* The CRC verify set and read have their own state, so one command can
	 * do both: -1 means no set was asked for, 0 or 1 the value to set, and
	 * do_get_crc is a plain flag. They shared one variable, so -k1 -k read
	 * only and -k -k1 set only, dropping the other half with exit 0. */
	int do_set_crc, do_get_crc;
	/* -v: describe every packet whose data the decoder cannot
	 * read, rather than only counting it for the summary at the end. */
	int do_verbose = 0;
	int do_adv_index;
	/* Whether -A was written, as against left at its default: the default
	 * is harmless with any mode, an explicit one is refused with -p. */
	int do_adv_index_given;
	int do_peripheral_mode;
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
	/* The dump this run writes, named here and opened with the capture
	 * files below, and the dump -R replays, which is read instead of a
	 * board. */
	const char* dump_name = NULL;
	const char* infile_name = NULL;
	/* Allocated once the whole command line has been accepted (below):
	 * every refusal above returns without a board, and the handle carries a
	 * 72 MB fifo. */
	ubertooth_t* ut = NULL;

	btle_options cb_opts = { .allowed_access_address_errors = 32 };

	int r;
	u32 access_address = 0;
	/* One address each: -t names the connection to follow and -s the
	 * address to advertise from, which are two different things. They
	 * shared one pair of variables, so whichever option was written last
	 * set both, and "-s <address> -t none" advertised from
	 * 00:00:00:00:00:00 without a word. */
	uint8_t target_address[OPT_BD_ADDR_BYTES] = { 0, };
	uint8_t target_mask = 0;
	/* -s takes a plain address: a faux peripheral advertises from one whole
	 * address, so it has no mask of its own. */
	uint8_t peripheral_address[OPT_BD_ADDR_BYTES] = { 0, };

	/* Standard output until a capture takes it; see report_stream. */
	report_stream = stdout;
	do_follow = do_no_follow = do_promisc = 0;
	do_get_aa = do_set_aa = 0;
	do_set_crc = -1;
	do_get_crc = 0;
	do_adv_index = 37;
	do_adv_index_given = 0;
	do_peripheral_mode = do_target = 0;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	/* Named, so the loop below and opt_report_bad_option()
	 * cannot be given different letters. */
	const char options[] = ":a::r:hVfnpU:k::vA:s:t:x:c:q:jJiId:R:";
	opterr = 0;
	while ((opt=getopt(argc, argv, options)) != EOF) {
		/* getopt only attaches an optional value (-a8e89bed6, -k1); take
		 * a following word that could be the value as the value too, so
		 * "-a 8e89bed6" sets and "-a zzzz" is refused rather than
		 * silently reading. ubertooth-btle takes no other operands.
		 * Resolved before the check below, which only counts an option
		 * that came with a value. */
		if (optarg == NULL && (opt == 'a' || opt == 'k'))
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
			/* A PcapNG writer seeks, which standard output does not
			 * allow, so every spelling of that stream is refused
			 * here; ubertooth-rx says why the command line is the
			 * place for it. */
			if (opt_writes_to_stdout(optarg)) {
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
		case 'd':
			dump_name = optarg;
			break;
		case 'R':
			/* The dump to replay. Read rather than written, so it is
			 * opened before the capture files below, and a board is
			 * never opened at all. */
			infile_name = optarg;
			break;
		case 'v':
			/* -v is verbose, as it is in every other tool: it asks the
			 * decoder to describe each packet whose data it cannot read,
			 * instead of only counting it for the summary at the end of
			 * the run. It takes no value, as it does in those tools:
			 * the one level is all there is, so a second -v says what
			 * the first did and getopt refuses a value here for us. */
			do_verbose = 1;
			break;
		case 'k':
			/* -k reads the CRC verify setting; -k0 / -k1 sets it, written
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
			do_adv_index_given = 1;
			break;
		case 's':
			do_peripheral_mode = 1;
			r = convert_plain_mac_address(optarg, "faux peripheral",
			                              peripheral_address);
			if (!r)
				return 1;
			break;
		case 't':
			do_target = 1;
			r = convert_mac_address(optarg, "target",
			                        target_address, &target_mask);
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


	/* ubertooth-btle takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. "-"
	 * is only a value of -q/-c/-r, which getopt has already taken. */
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

	/* -a reads and -a<address> sets; one command cannot do both (the read
	 * used to win and the set was dropped). */
	if (do_get_aa && do_set_aa) {
		fprintf(stderr, "error: -a cannot both read and set the access address\n");
		return 1;
	}

	/* One major mode per command. Another argument error, so it is refused
	 * here rather than after a device has been opened. */
	if (do_follow + do_no_follow + do_promisc > 1) {
		fprintf(stderr, "error: choose one of -f, -n or -p\n");
		return 1;
	}

	/* A sniffing mode runs until it is interrupted and ends in
	 * ubertooth_stop(), which closes the device handle and nulls it, so
	 * the access address and faux peripheral mode blocks below have no handle
	 * left to send on. -t is not in this list: it is sent before the mode
	 * starts, which is what it is for. */
	if ((do_follow || do_no_follow || do_promisc) &&
	    (do_get_aa || do_set_aa || do_peripheral_mode)) {
		fprintf(stderr, "error: -a and -s do not go with -f, -n or -p:"
		        " a sniffing mode runs until it is interrupted\n");
		return 1;
	}

	/* -t picks out a connection by the device address its advertising and
	 * connect packets carry. Promiscuous mode starts on a connection that
	 * is already running, whose packets carry no device address, so there
	 * is nothing there for a target to match -- which is as true of "-t
	 * none" as of an address, so the message is about the option rather
	 * than its value. An access address for -p comes from a separate
	 * "ubertooth-btle -a <address>" command instead; otherwise the mode
	 * discovers one. */
	if (do_target && do_promisc) {
		fprintf(stderr, "error: -t does not go with -p: promiscuous"
		        " mode has no device address to match a target"
		        " against\n");
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

	/* Verbose describes the packets a capture mode decodes, so without one
	 * of those modes it has nothing to describe. Refused rather than
	 * accepted and ignored, and before the "nothing to do" check below, so
	 * that "ubertooth-btle -v" -- which read the CRC verify setting while
	 * -v was that setting's older spelling -- says what -v is now instead
	 * of listing every major mode. */
	if (do_verbose && !(do_follow || do_no_follow || do_promisc)) {
		fprintf(stderr, "error: -v needs -f, -n or -p: there is no decode"
		        " for it to describe; the CRC verify setting is -k now\n");
		return 1;
	}

	/* A capture that takes standard output leaves this run printing no
	 * packets at all (see the capture block below), so there is nothing for
	 * -v to describe here either. Refused rather than accepted and ignored.
	 * -r refuses that stream outright, so only -q, -c and -d reach this. */
	if (do_verbose &&
	    (opt_writes_to_stdout(pcap_name) || opt_writes_to_stdout(dump_name))) {
		fprintf(stderr, "error: -v does not go with a capture on standard"
		        " output: a run that hands the capture that stream prints"
		        " no packets to describe\n");
		return 1;
	}

	/* Standard output carries one stream, so only one capture can have it:
	 * a PCAP file and a dump of the records interleaved there would be
	 * neither, and the one that asked second would write to /dev/null
	 * without a word. -q and -c share a writer, so the clash is between
	 * whichever of those two was taken and -d. ubertooth-rx refuses the
	 * same pair. */
	if (opt_writes_to_stdout(pcap_name) && opt_writes_to_stdout(dump_name)) {
		fprintf(stderr, "error: -%c and -d cannot both write to standard"
		        " output\n", pcap_ppi ? 'c' : 'q');
		return 1;
	}

	/* A replay needs the mode its dump was recorded with, because the
	 * record does not say which clock stamped it: the low-energy engine
	 * behind -f and -n stamps packets from a free-running counter that
	 * starts over every 429.5 s, and promiscuous mode from the one derived
	 * from the Bluetooth clock, which starts over every 327.68 s (see
	 * ubertooth_interface.h). The times a replay reconstructs come out
	 * wrong past the first of those, and nothing in the file would say so,
	 * which is why the mode is asked for rather than guessed. */
	if (infile_name != NULL && !(do_follow || do_no_follow || do_promisc)) {
		fprintf(stderr, "error: -R needs -f, -n or -p: a dump does not"
		        " record which mode wrote it, and the modes stamp their"
		        " packets with different clocks\n");
		return 1;
	}

	/* Promiscuous mode sets no channel: it listens on the one the board is
	 * already holding. So -A was taken and then never sent, and the command
	 * looked like it had chosen a channel. Use ubertooth-util -c to choose
	 * one, which is what the board keeps.
	 *
	 * Before the replay's refusals below, so that `-p -R f.bin -A 38` says
	 * -A does not go with -p at all rather than only that a replay has no
	 * board to tune. */
	if (do_adv_index_given && do_promisc) {
		fprintf(stderr, "error: -A does not go with -p: promiscuous mode"
		        " listens on the channel the board is holding\n");
		return 1;
	}

	/* A replay reads a saved dump and opens no board, so every option that
	 * sends the board a request, names one, or tunes one asks for something
	 * it cannot do. Refused here, naming the option, rather than accepted
	 * and dropped without a word. The decode options (-x, -v) and the
	 * capture files do work on a replay, as they do in ubertooth-rx.
	 *
	 * -a and -s are not here: a replay needs a major mode, and those two
	 * are already refused beside any of the three. */
	if (infile_name != NULL) {
		int board_option = 0;

		if (do_set_crc >= 0 || do_get_crc)
			board_option = 'k';
		else if (do_target)
			board_option = 't';
		else if (do_adv_index_given)
			board_option = 'A';
		else if (ubertooth_device != NULL)
			board_option = 'U';
		else if (jam_mode != JAM_NONE)
			board_option = jam_option;
		if (board_option != 0) {
			fprintf(stderr, "error: -%c cannot be combined with -R;"
			        " a replay reads a saved dump rather than"
			        " driving a board\n", board_option);
			return 1;
		}
	}

	/* A command line with no work in it: refused here, before a device is
	 * opened and before any capture file is created. It used to open the
	 * selected board, check its API version, send nothing, print the usage
	 * text on stdout and exit 0, so a command that did nothing needed a
	 * board and looked like one that had worked. -A, -x and the capture
	 * options only shape the work the options below ask for. */
	if (!(do_follow || do_no_follow || do_promisc || do_get_aa || do_set_aa ||
	      do_set_crc >= 0 || do_get_crc || do_peripheral_mode || do_target)) {
		fprintf(stderr, "error: nothing to do: choose a major mode"
		        " (-f, -n or -p), or -a, -k, -s or -t\n");
		if (opt_no_arguments(argc))
			usage(stderr);
		return 1;
	}

	/* A capture bound for standard output must not go to a terminal. That
	 * is a problem with the command line, so it is refused here, before a
	 * board is opened; the stream itself is taken over further down, where
	 * the file is created. */
	if (opt_check_capture_destination(pcap_name, "binary capture data") < 0 ||
	    opt_check_capture_destination(dump_name, "binary dump data") < 0)
		return 1;

	/* Interference is accepted but no capture mode acts on it yet, so an
	 * honest run says so once rather than reporting a clean capture for
	 * something it did not do. Said here, beside the -v warning and after
	 * every refusal, so a command that will not run gets its error line
	 * alone. The man page's KNOWN BROKEN FEATURES section has the detail. */
	if (jam_mode != JAM_NONE)
		fprintf(stderr, "warning: interference (-i, -I, -j, -J) is accepted"
		        " but no capture mode acts on it yet; see KNOWN BROKEN"
		        " FEATURES in ubertooth-btle(1)\n");

	/* Every check has passed, so the handle and its 72 MB fifo can be
	 * allocated: the capture files further down are kept on it. */
	ut = ubertooth_init();
	if (ut == NULL)
		return 1;

	/* And the replay's input can be opened: a refused command line creates
	 * and truncates nothing. It is read, not written, so it comes before
	 * the board; the files this run writes are opened further down, once
	 * the board is known to be usable. */
	if (infile_name != NULL) {
		infile = fopen(infile_name, "r");
		if (infile == NULL) {
			/* A file that is not there is a runtime failure, not a
			 * bad command line, so it gets no usage text. */
			fprintf(stderr, "error: %s: %s\n", infile_name,
			        strerror(errno));
			return release_handle(ut, 1);
		}
	}

	/* Stop on Ctrl-C, and on the other signals that mean the run is over
	 * (SIGTERM, SIGHUP, SIGQUIT, an alarm): the library's handlers, so the
	 * capture loop below ends the way every other tool's does. Installed
	 * before the board is opened, because a signal that arrives while one
	 * is being found would otherwise be discarded. A replay gets the
	 * handlers as well, so an interrupted one ends where it was asked to
	 * and exits 0 with what it had decoded, instead of reading on to the
	 * end of the file. */
	ubertooth_catch_stop_signals();

	/* A replay has no board to open: the records come out of the file. */
	if (infile == NULL) {
		r = ubertooth_connect_select(ut, ubertooth_device);
		if (r < 0) {
			/* A selection the user stopped opened no board, which is
			 * not a failure to find one. */
			return release_handle(ut,
			                      ubertooth_open_stopped() ? 0 : 1);
		}

		r = ubertooth_check_api(ut);
		if (r < 0) {
			/* release_handle() closes the board, which covers both
			 * of the check's failures: it released a refused version
			 * itself, and a version it could not read at all leaves
			 * the board open. */
			return release_handle(ut, 1);
		}
	}

	/* Stopped while the board was being opened: the run ends here, with the
	 * board given back and no capture file left behind, rather than
	 * starting a mode and stopping it again. */
	if (ubertooth_stop_requested())
		return release_handle(ut, 0);

	/* The board is open and usable -- or the replay has its input -- so the
	 * capture files can be opened. Not before: a run refused for its
	 * firmware used to leave an empty capture behind, which a user then had
	 * to tell apart from a capture that caught nothing.
	 *
	 * A bare "-" redirects this process's stdout, which is why nothing may
	 * be printed to stdout after this point except the capture itself, and
	 * why the lines that answer an option go to report_stream rather than
	 * straight to stdout.
	 *
	 * No writer may be the dump a replay is reading: see
	 * output_is_the_replay(). Checked here rather than while the command
	 * line is read, because it takes the open input to identify, and still
	 * before the first writer opens, so a refused run truncates nothing. */
	if (output_is_the_replay(pcapng_name, 'r') < 0 ||
	    output_is_the_replay(pcap_name, pcap_ppi ? 'c' : 'q') < 0 ||
	    output_is_the_replay(dump_name, 'd') < 0)
		return release_handle(ut, 1);
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
		char *file = opt_capture_path(pcap_name, "binary capture data", &saved_fd);
		int create_result, create_errno;

		if (!file) {
			if (saved_fd >= 0)
				close(saved_fd);
			return release_handle(ut, 1);
		}
		errno = 0;
		create_result = pcap_ppi
		        ? lell_pcap_ppi_create_file(file, 0, &ut->h_pcap_le)
		        : lell_pcap_create_file(file, &ut->h_pcap_le);
		/* Read now: the close() and the free() below may set an errno
		 * of their own, and this one is the reason reported. */
		create_errno = errno;
		if (saved_fd >= 0) {
			/* The capture has taken stdout, which is now /dev/null
			 * for the rest of this run: the lines that answer an
			 * option go to standard error instead, and the callback
			 * leaves out the per-packet dump that would go nowhere. */
			report_stream = stderr;
			cb_opts.capture_took_stdout = 1;
			close(saved_fd);
		}
		free(file);
		if (create_result) {
			/* The name the user gave, not the /dev/fd path a bare
			 * "-" is opened through. */
			opt_report_create_failure(pcap_name, create_errno);
			return release_handle(ut, 1);
		}
	}
	if (dump_name != NULL) {
		dumpfile = opt_open_dump_file(dump_name, "binary dump data");
		if (dumpfile == NULL)
			return release_handle(ut, 1);
		if (opt_writes_to_stdout(dump_name)) {
			/* The dump has standard output, which is /dev/null for
			 * this process from here on: the lines that answer an
			 * option go to standard error instead, and the callback
			 * leaves out the per-packet report that would go
			 * nowhere. */
			report_stream = stderr;
			cb_opts.capture_took_stdout = 1;
		}
	}

	/* A replay has no board, so the records come out of the file and the
	 * callback decodes them exactly as it decodes a live capture's. */
	if (infile != NULL) {
		/* Which of the board's two clocks stamped these records, taken
		 * from the mode named on the command line as a live capture
		 * takes it: see the refusal above for why the mode has to be
		 * named. */
		if (do_follow || do_no_follow)
			ut->clk100ns_period = CLK100NS_TIMER1_PERIOD;

		/* The callback reports on the same stream the tool does, so the
		 * lines a promiscuous dump's parameter records make stay
		 * visible when a capture has taken standard output. */
		cb_opts.report_stream = report_stream;
		if (do_verbose)
			lell_set_malformed_verbose(1);

		/* A read error or a last record cut short is reported and ends
		 * the command with an error, after whatever was decoded. */
		r = stream_rx_file(ut, infile, replay_decode, &cb_opts);
		status = rx_file_report(r, infile_name);

		/* An input no whole record could be read from leaves the
		 * capture files holding their headers and nothing else, so they
		 * go the way a refused run's files do: a 64 KB PcapNG holding
		 * only its header looks like a capture that caught something.
		 * From the first record on they stay, however little of the
		 * input came after it. An input read to its end is no failure,
		 * an empty one included.
		 *
		 * Before the writers are closed, because the files go by name
		 * while those writers still hold them, and whatever a writer
		 * had buffered then goes nowhere. Which names are left alone --
		 * a lone "-", a device, a symbolic link -- is
		 * opt_discard_capture_file()'s decision. */
		if (status != 0 && !replay_read_a_record) {
			if (ut->h_pcapng_le != NULL)
				opt_discard_capture_file(pcapng_name);
			if (ut->h_pcap_le != NULL)
				opt_discard_capture_file(pcap_name);
			if (dumpfile != NULL)
				opt_discard_capture_file(dump_name);
		}

		/* A recorded capture carries the losses it was recorded with,
		 * in the status of its own records, and the replay is where a
		 * reader of the file finds out about them. */
		ubertooth_report_capture_losses(ut);
		report_malformed_packets(&cb_opts, do_verbose);

		return release_handle(ut, status);
	}

	// cancel following on USR1
	signal(SIGUSR1, cancel_follow_handler);

	// allow user to set target before following
	if (do_target) {
		r = cmd_btle_set_target(ut->devh, target_address, target_mask);
		if (r == 0) {
			if (target_mask == 0) {
				fprintf(report_stream, "target cleared\n");
			} else {
				fprintf(report_stream, "target set to: ");
				print_mac(report_stream, target_address, target_mask);
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
			 * part of.
			 *
			 * The advertising channel is stated twice, before the
			 * mode request and again after it. Listening starts
			 * with the mode, and the engine reads the channel as
			 * it starts and then only when it leaves a connection,
			 * so the board has to be on the channel beforehand --
			 * but a mode start puts it back to 2402 MHz, and a
			 * board left in another mode by a killed tool does
			 * that after the mode request has been answered, which
			 * threw the first statement away: the follow listened
			 * on channel 37 whatever -A asked for, quietly,
			 * because 37 does carry advertisements. The second is
			 * the one such a board keeps. */
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
			if (cmd_set_channel(ut->devh,
			                    adv_channel(do_adv_index)) < 0) {
				report_request_failure(
					"set the advertising channel");
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

		/* The callback reports on the same stream the tool does, so the
		 * lines promiscuous mode writes stay visible when a capture has
		 * taken standard output. Taken here, after everything that could
		 * move the stream, so there is one source for it. */
		cb_opts.report_stream = report_stream;

		/* Data off the air that does not hold what its own length
		 * fields describe is ordinary: both a real packet with a
		 * corrupted byte and noise that decoded as a packet produce
		 * one, and a busy bench gives hundreds in ten seconds. So the
		 * decoder counts them and this run prints one line at the end;
		 * -v asks it to describe each one on standard error as it
		 * goes. */
		if (do_verbose)
			lell_set_malformed_verbose(1);

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
		/* A teardown the board refused left it running the capture mode
		 * this tool started, and no other tool can open it until it is
		 * reset or re-plugged, so the run did not do what was asked.
		 * libubertooth has already named the refused request; what was
		 * captured stays written out. */
		if (ubertooth_stop_failed(ut))
			status = 1;

		/* What the capture is missing, once, where the user will see it
		 * after the packets have scrolled past. */
		ubertooth_report_capture_losses(ut);

		report_malformed_packets(&cb_opts, do_verbose);
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

	if (do_peripheral_mode) {
		/* flags: LE Limited Discovery */
		uint8_t adv_data[] = { 0x02, 0x01, 0x05 };

		/* Checked one at a time, so the report names the request that
		 * failed: every one of them had to work for the board to
		 * advertise on the channel asked for, and "could not start
		 * faux peripheral mode" did not say which of them the board
		 * refused.
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
		if (cmd_btle_peripheral(ut->devh, peripheral_address) < 0) {
			report_request_failure("start advertising");
			ubertooth_stop(ut);
			return release_handle(ut, 1);
		}
		/* And again now the mode has started. Advertising begins with
		 * it, so the channel has to be on the board beforehand -- but a
		 * mode start puts it back to 2441 MHz, and a board left in
		 * another mode by a killed tool does that after this request
		 * has been answered, which threw the channel away. The board
		 * keeps a channel stated after the request for the mode it is
		 * starting, and reads it again for every advertisement. */
		if (cmd_set_channel(ut->devh, adv_channel(do_adv_index)) < 0) {
			report_request_failure("set the advertising channel");
			ubertooth_stop(ut);
			return release_handle(ut, 1);
		}
	}

	return release_handle(ut, status);
}
