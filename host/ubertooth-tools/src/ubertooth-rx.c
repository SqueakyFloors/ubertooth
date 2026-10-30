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
#include "util_clock.h"
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* Names the request the board refused, says why and what to try, and gives the
 * board back rather than capturing with settings the user did not ask for.
 * Closed without a STOP: no mode has started yet, so there is nothing to stop,
 * and a STOP to a board that is not answering would only add a second failure
 * to the report. A refused request is not proof that the board never got it --
 * a transfer can time out after it arrived -- so the message says only what
 * the tool knows. */
static int request_failed(ubertooth_t* ut, const char* what, int code)
{
	report_refused_request(what, code);
	ubertooth_close(ut);
	ubertooth_free(ut);
	return 1;
}

/* The capture files this run was asked for, by the names the user gave. */
struct rx_files {
	const char* pcapng;  /* -r */
	const char* pcap;    /* -q */
	const char* dump;    /* -d */
};

/* Takes the capture files away again, for a failure between creating them and
 * the first packet: a refused run then leaves no empty capture behind.
 *
 * The handles say which files exist, since each is set only once its writer
 * has the file open, so before the files are created this does nothing. It
 * runs before the board goes back, because that is what closes those writers:
 * the files go by name while the writers still hold them, and the header
 * bytes a writer had buffered then go nowhere. Which names are left alone is
 * opt_discard_capture_file()'s decision. */
static void rx_discard_files(ubertooth_t* ut, const struct rx_files* files)
{
	if (ut->h_pcapng_bredr != NULL)
		opt_discard_capture_file(files->pcapng);
	if (ut->h_pcap_bredr != NULL)
		opt_discard_capture_file(files->pcap);
	if (dumpfile != NULL)
		opt_discard_capture_file(files->dump);
}

/* Gives the board back for a failure after the capture files were created but
 * before any mode has started, taking those files with it. The caller has
 * already said what failed. Closed without a STOP for the same reason
 * request_failed() does not send one. */
static int setup_failed(ubertooth_t* ut, const struct rx_files* files)
{
	rx_discard_files(ut, files);
	ubertooth_close(ut);
	ubertooth_free(ut);
	return 1;
}

/* The same for a failure while the receive transfer is being set up, which the
 * board is given back from with a STOP: by then it may be left in the mode an
 * earlier command started. libubertooth or the caller has already said what
 * failed. Every failure from the files down to the capture loop goes through
 * one of these three helpers, so a path added between them cannot forget the
 * files. */
static int transfer_failed(ubertooth_t* ut, const struct rx_files* files)
{
	rx_discard_files(ut, files);
	ubertooth_stop(ut);
	ubertooth_free(ut);
	return 1;
}

/* The same report for a request refused once the capture is already running.
 * The mode is stopped and the board given back, so a board the user has to
 * rescue is not left streaming with settings they did not ask for.
 *
 * Unlike request_failed() above, which says nothing to a board that has not
 * answered, this does send the STOP, and a board that has stopped answering
 * can make it print a line of its own. A mode left running is worse than a
 * second line in the report. Both man pages say so.
 *
 * The poll thread is running by now and nothing else joins it:
 * ubertooth_stop() frees the libusb context it polls. */
static int capture_failed(ubertooth_t* ut, const struct rx_files* files,
                          const char* what, int code)
{
	report_refused_request(what, code);
	rx_discard_files(ut, files);
	ubertooth_bulk_thread_stop();
	ubertooth_stop(ut);
	ubertooth_free(ut);
	return 1;
}

/* Set once a replay has read a whole record from its input. That is the line
 * its capture files are kept from, the same line a capture draws at its first
 * packet: the replay ran, and what the decoder made of the record -- a packet,
 * or nothing it recognized -- is the result. Before the first record the files
 * hold their headers and nothing else. */
static int replay_read_a_record;

/* A replay's decode: notes the record, then decodes it like any other. The
 * decode is libubertooth's cb_rx() and notes nothing itself, and a live
 * capture needs no note, so only a replay goes through here. */
static void replay_decode(ubertooth_t* ut, void* args)
{
	replay_read_a_record = 1;
	cb_rx(ut, args);
}

/* How long a follow may hear nothing of its piconet before the clock it is
 * following is given up, in milliseconds.
 *
 * A clock that is not the piconet's gets only the channel to listen on wrong,
 * so it checks out every header it does receive and is heard from about one
 * packet in eighteen instead of failing (see ubertooth_follow_heard()).
 * Silence is all there is to go on, and it is weighed like this: ten seconds
 * is 8000 slot pairs, in which the right clock is on the piconet's channel for
 * every one and a wrong one for about 455. A link carrying four packets a
 * second, the slow link the library's own give-up rule is sized for, would
 * have been heard from 40 times by the end of the wait.
 *
 * The wait is this long because giving a clock up rules it out, so the search
 * does not come back to it: a follow that was right and went quiet does not
 * get that clock again in the same run, where a wrong clock kept a little
 * longer costs only time. Ten seconds without one packet of a piconet whose
 * packets arrive every few slots is a link that has stopped rather than a
 * sparse one. A wrong clock on that four-packet-a-second link is then given up
 * after about 40 s of following. */
#define FOLLOW_QUIET_MS 10000u

/* The wait check_quiet_follow() is timing: the count of packets heard it last
 * looked at, and when it looked. One capture, one of these. */
struct follow_watch {
	long heard;        /* -1 before the first look, and while not following */
	uint64_t since_ms;
};
#define FOLLOW_WATCH_START { -1, 0 }

/* Gives up a clock this capture has heard nothing on. The callback counts the
 * packets of the piconet heard since the library handed the clock over; how
 * long that count has stood still is measured here, because the callback runs
 * when a record arrives rather than on a clock, and it is this tool that asked
 * the board to hop with the piconet. */
static void check_quiet_follow(ubertooth_t* ut, btbb_piconet* pn,
                               struct follow_watch* watch)
{
	long heard = ubertooth_follow_heard();
	uint64_t now_ms = util_clock_now_ms();

	/* A packet heard, a clock just handed over, or one just given up: the
	 * follow is not silent, so the wait starts from here. */
	if (heard != watch->heard) {
		watch->heard = heard;
		watch->since_ms = now_ms;
		return;
	}
	/* Not following anything, or not quiet for long enough yet. */
	if (heard < 0 || now_ms - watch->since_ms < FOLLOW_QUIET_MS)
		return;

	/* The wait starts again whether or not the library had a clock to give
	 * up, so a capture following a clock that did not come from the
	 * library asks once per wait rather than once per record. The warning
	 * belongs to the drop, which says it once per clock. */
	watch->since_ms = now_ms;
	ubertooth_follow_drop_clock(ut, pn);
}

int main(int argc, char* argv[])
{
	int opt, have_lap = 0, have_uap = 0;
	int survey_mode = 0;
	/* Whether -c named a channel, which decides only how a refused channel
	 * request is named: a command line without it asks for the sweep. */
	int have_channel = 0;
	/* Whether -t named a timeout. A timeout of 0 means "no timeout", so the
	 * value cannot say whether the option was given. */
	int have_timeout = 0;
	int exit_status = 0;
	const char* infile_name = NULL;
	/* The three names together, for the failures that have to take back a
	 * file this run created. Filled in once the command line is read. */
	struct rx_files files = { NULL, NULL, NULL };
	/* Files named on the command line, opened after every check passes. */
	const char* pcapng_name = NULL;
	const char* pcap_name = NULL;
	const char* dump_name = NULL;
	/* -h and -V both end the command once the whole command line has
	 * parsed, and a command line with both prints both (see below).
	 * version_status is -V's own result: it reads the board, so it can fail
	 * where -h cannot. */
	int do_help = 0, do_version = 0, version_status = 0;
	opt_once once = {0};
	int r;
	int timeout = 0;
	char* ubertooth_device = NULL;
	btbb_piconet* pn = NULL;
	uint32_t lap = 0;
	uint8_t uap = 0;
	/* Above the Bluetooth band, which the firmware reads as "sweep the
	 * whole band": the default when no -c names a fixed channel. */
	uint16_t channel = 9999;

	/* Allocated once the whole command line has been accepted (below):
	 * every refusal above returns without a board, and the handle carries a
	 * 72 MB fifo. */
	ubertooth_t* ut = NULL;

	/* Where the survey results go: standard output, unless a capture on "-"
	 * has taken it, in which case they would go to /dev/null with the rest
	 * of this process's standard output. The survey is what -z was asked
	 * for, so it is printed on standard error rather than lost. */
	FILE* report_stream = stdout;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	/* Named, so the loop below and opt_report_bad_option()
	 * cannot be given different letters. */
	const char options[] = ":hVi:l:u:U:d:e:r:sq:t:zc:";
	opterr = 0;
	while ((opt=getopt(argc, argv, options)) != EOF) {
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
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
			/* A PcapNG writer seeks back to finish the blocks it has
			 * written, which standard output cannot do, so a lone "-"
			 * is refused here rather than taken as the file name "-".
			 * The PCAP writer does not seek: -q takes it. */
			if (strcmp(optarg, "-") == 0) {
				fputs("error: PCAPNG capture to stdout is not supported;"
				      " use -q -\n", stderr);
				return 1;
			}
			pcapng_name = optarg;
			break;
		case 'q':
			pcap_name = optarg;
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
			have_timeout = 1;
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
			do_version = 1;
			break;
		case 'h':
			do_help = 1;
			break;
		default:
			/* A command line the tool will not take: the one-line
			 * reason goes to standard error and standard output
			 * stays empty. The tool's other output lines are
			 * converted separately. */
			opt_report_bad_option(opt, argc, argv, options);
			return 1;
		}
	}

	/* ubertooth-rx takes no operands: refuse a leftover word before any
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

	if(survey_mode && (have_lap || have_uap)) {
		fprintf(stderr, "error: survey mode (-z) discovers addresses, so it"
		        " cannot be given -l or -u\n");
		return 1;
	}

	/* Standard output carries one stream, so only one capture can have it:
	 * a PCAP file and a packet dump interleaved there would be neither. */
	if (pcap_name != NULL && dump_name != NULL &&
	    strcmp(pcap_name, "-") == 0 && strcmp(dump_name, "-") == 0) {
		fputs("error: -q and -d cannot both write to standard output\n",
		      stderr);
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

	/* A replay has no board to tune and nothing to wait for, so these two
	 * ask for something it cannot do. They used to be accepted and dropped
	 * without a word: the file was read on no channel and with no timeout,
	 * and the user had no way to tell. */
	if (infile_name != NULL && have_channel) {
		fprintf(stderr, "error: -c cannot be combined with -i; a replay"
		        " reads a saved capture rather than tuning a board\n");
		return 1;
	}
	if (infile_name != NULL && have_timeout) {
		fprintf(stderr, "error: -t cannot be combined with -i; a replay"
		        " reads a saved capture rather than waiting for a"
		        " board\n");
		return 1;
	}

	/* A capture bound for standard output must not go to a terminal. That
	 * is a problem with the command line, so it is refused here, before a
	 * board is opened; the stream itself is taken over further down, where
	 * the file is created. */
	if (opt_check_capture_destination(pcap_name, "capture data") < 0 ||
	    opt_check_capture_destination(dump_name, "dump data") < 0)
		return 1;

	/* Every check has passed, so the handle and its 72 MB fifo can be
	 * allocated: the capture files below are kept on it. */
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
			 * bad command line, so it gets no usage text; same
			 * shape as the other file-open failures. */
			fprintf(stderr, "error: %s: %s\n",
			        infile_name, strerror(errno));
			ubertooth_free(ut);
			return 1;
		}
	}
	/* Stop on Ctrl-C from here on, before the board is opened: a signal
	 * that arrives while one is being found would otherwise be discarded.
	 * A replay gets the handlers as well, so an interrupted one ends where
	 * it was asked to and exits 0 with what it had decoded, instead of
	 * being killed by the signal -- or, started as a background job by a
	 * script, reading on to the end of the file. */
	ubertooth_catch_stop_signals();

	if (infile == NULL) {
		r = ubertooth_connect_select(ut, ubertooth_device);
		if (r < 0) {
			/* The library has closed whatever it opened; the close
			 * here is what ends the libusb session it started. */
			ubertooth_close(ut);
			ubertooth_free(ut);
			return ubertooth_open_stopped() ? 0 : 1;
		}

		r = ubertooth_check_api(ut);
		if (r < 0) {
			/* A refused version was released inside
			 * ubertooth_check_api(); one it could not read at all
			 * leaves the board open. Closing covers both, because
			 * closing a board already released does nothing, and it
			 * sends no STOP to a firmware this host will not
			 * drive. */
			ubertooth_close(ut);
			ubertooth_free(ut);
			return 1;
		}

		/* Stopped while the board was being opened: the run ends
		 * here, with the board given back and no capture file left
		 * behind, rather than starting a mode and stopping it
		 * again. */
		if (ubertooth_stop_requested()) {
			ubertooth_close(ut);
			ubertooth_free(ut);
			return 0;
		}
	}

	/* libbtbb refuses only a -e out of its own range, which the check on -e
	 * above has already caught, and it says so itself. The status is 1, as
	 * every failure here is: the failing call's own return value used to go
	 * out as the exit status, which a shell wraps into a byte (-9 read as
	 * 247), so a script could not tell a failure from anything else. */
	r = btbb_init(max_ac_errors);
	if (r < 0) {
		/* A capture has a board open, in whatever mode an earlier
		 * command left it, so it is given back here rather than left
		 * claimed, as ubertooth-scan does on the same failure. A replay
		 * opened none, and the stop then does nothing. */
		ubertooth_stop(ut);
		ubertooth_free(ut);
		return 1;
	}

	if(survey_mode) {
		// auto-flush stdout so that wrapper scripts work
		setvbuf(stdout, NULL, _IONBF, 0);
		btbb_init_survey();
	} else {
		if (have_lap) {
			pn = btbb_piconet_new();
			if (pn == NULL) {
				/* Out of memory, which the next line would
				 * dereference. A capture with no piconet to
				 * follow has nothing to do, so the run ends
				 * here, and the STOP covers a mode an earlier
				 * command left running. */
				fputs("error: out of memory\n", stderr);
				ubertooth_stop(ut);
				ubertooth_free(ut);
				return 1;
			}
			btbb_init_piconet(pn, lap);
			if (have_uap) {
				btbb_piconet_set_uap(pn, uap);
				/* A board that refused this would follow
				 * whatever address it was left on, so the
				 * capture does not start. It is sent here,
				 * with the rest of the setup that can fail,
				 * so a board that refuses it leaves no empty
				 * capture behind either. A replay sends no
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
		}
	}

	files.pcapng = pcapng_name;
	files.pcap = pcap_name;
	files.dump = dump_name;

	/* The board is open and usable, or this is a replay, and every step of
	 * the setup that can fail is behind us, so the files this run writes
	 * can be opened. Not before: a run refused for its firmware, or one
	 * libbtbb or the board would not set up, used to leave an empty capture
	 * behind, which a user then had to tell apart from a capture that
	 * caught nothing.
	 *
	 * A lone "-" redirects this process's standard output, which is why
	 * nothing may be printed there after this point except the capture
	 * itself, and why the per-packet report goes to report_stream. */
	errno = 0;
	if (pcapng_name != NULL &&
	    btbb_pcapng_create_file(pcapng_name, "Ubertooth", &ut->h_pcapng_bredr)) {
		opt_report_create_failure(pcapng_name, errno);
		return setup_failed(ut, &files);
	}
	if (pcap_name != NULL) {
		/* A lone "-" is standard output, which the capture then owns:
		 * see opt_capture_path(). The per-packet report this tool
		 * prints there goes nowhere for such a run, which is what the
		 * man page says of it; the notes and failures are on standard
		 * error and still arrive. */
		int saved_fd;
		char *file = opt_capture_path(pcap_name, "capture data", &saved_fd);
		int failed, create_errno;

		if (file == NULL) {
			if (saved_fd >= 0)
				close(saved_fd);
			return setup_failed(ut, &files);
		}
		errno = 0;
		failed = btbb_pcap_create_file(file, &ut->h_pcap_bredr);
		/* Read now: the close() and the free() below may set an errno
		 * of their own, and this one is the reason reported. */
		create_errno = errno;
		if (saved_fd >= 0) {
			/* The capture has standard output, which is /dev/null for
			 * this process from here on. */
			report_stream = stderr;
			close(saved_fd);
		}
		free(file);
		if (failed) {
			/* The name the user gave, not the /dev/fd path a lone
			 * "-" is opened through. */
			opt_report_create_failure(pcap_name, create_errno);
			return setup_failed(ut, &files);
		}
	}
	if (dump_name != NULL) {
		dumpfile = opt_open_dump_file(dump_name, "dump data");
		if (dumpfile != NULL && strcmp(dump_name, "-") == 0)
			report_stream = stderr;
		if (dumpfile == NULL) {
			/* The capture files are on the handle, so the close is
			 * what gets them written out and shut, and it gives the
			 * board back. The files this run created are taken away
			 * first, so the write lands in a file already gone. */
			return setup_failed(ut, &files);
		}
	}

	/* The address being followed, recorded once so a reader of the capture
	 * knows whose packets these are. It needs the PcapNG file, so it is the
	 * one part of the setup above that has to wait for it. Reported when it
	 * cannot be written: the capture goes on either way, but without this
	 * the packets in it have nothing to say which piconet they belong to.
	 * A piconet exists only where -l named one: a survey has no single
	 * address to record. */
	if (pn != NULL && ut->h_pcapng_bredr) {
		if (btbb_pcapng_record_bdaddr(ut->h_pcapng_bredr,
		        (((uint32_t)uap)<<24)|lap,
		        have_uap ? 0xff : 0x00, 0) < 0)
			fprintf(stderr, "warning: cannot record the target "
			        "address in the capture file\n");
	}

	if (infile == NULL) {
		if (timeout)
			ubertooth_set_timeout(ut, timeout);

		// init USB transfer
		r = ubertooth_bulk_init(ut);
		if (r < 0) {
			/* nothing is in flight, but the board is still open and in
			 * whatever mode an earlier command left it. libubertooth has
			 * already said what failed, so the status is all this adds. */
			return transfer_failed(ut, &files);
		}

		r = ubertooth_bulk_thread_start();
		if (r != 0) {
			/* Not "< 0": this is pthread_create()'s errno, which is
			 * positive, so a failure used to slip through and leave the
			 * capture with nobody to complete its transfers -- it then
			 * waited forever with nothing arriving. */
			report_capture_thread_failure(r);
			/* The receive transfer is in flight and the board is about to
			 * be left in its mode: give both back before returning. */
			return transfer_failed(ut, &files);
		}

		/* The modulation this capture reads, Classic, stated before the
		 * capture request and not after it as the channel is: see
		 * ubertooth-util(1), CARRY-OVER BETWEEN RUNS. Without it, a
		 * board an interrupted `ubertooth-dump -l` had left set to low
		 * energy captured here at that deviation, finding little and
		 * saying nothing. */
		r = cmd_set_modulation_quiet(ut->devh, MOD_BT_BASIC_RATE);
		if (r < 0)
			return capture_failed(ut, &files, "set the modulation", r);

		// tell ubertooth to send packets
		/* The quiet form, so this failure reads as the one "error: " line
		 * capture_failed() prints, like the channel request below. The
		 * reporting form would hand it to libubertooth's shared sink
		 * instead, which does not name the request that failed. */
		r = cmd_rx_syms_quiet(ut->devh);
		if (r < 0)
			return capture_failed(ut, &files, "start the capture", r);

		/* Tell the board how to tune for this capture after asking for
		 * the capture, not before. Starting a mode puts the board's hop
		 * mode back to "no hopping", so a sweep stated first is thrown
		 * away: the capture then ran on one channel while the user had
		 * asked for the whole band, and nothing said so.
		 *
		 * The board keeps what arrives after the request for the mode it
		 * is starting, whatever it was doing before. A board left in a
		 * mode that captures nothing -- a killed ubertooth-specan say --
		 * used to lose this request, because it runs the reset a moment
		 * after this one has been answered, on the way out of that mode.
		 *
		 * The board stays on whatever it was tuned to until this
		 * request arrives and the next hop acts on it, which takes up
		 * to 40 ms. Blocks captured in that window carry the channel
		 * they were captured on, as every block does, so they are
		 * reported for what they are rather than mislabelled.
		 *
		 * A board that refused this would capture on a channel the user
		 * did not ask for and say nothing about it, so the run ends
		 * instead. */
		r = cmd_set_channel_quiet(ut->devh, channel);
		if (r < 0)
			return capture_failed(ut, &files,
			                      have_channel ? "set the channel"
			                                   : "set sweep mode", r);
		/* Kept for the callback, which starts a mode again when the
		 * library gives up a clock and has to restate this. */
		ut->capture_channel = channel;

		struct follow_watch watch = FOLLOW_WATCH_START;

		// receive and process each packet
		while(!ubertooth_stopping(ut)) {
			ubertooth_bulk_receive(ut, cb_rx, pn);
			/* A follow that locked onto the wrong clock hears next
			 * to nothing rather than failing, so nothing in the
			 * packets that do arrive reports it: the wait for one
			 * is timed here. */
			check_quiet_follow(ut, pn, &watch);
		}

		ubertooth_bulk_thread_stop();

		ubertooth_stop(ut);

		/* What the capture is missing, once the packets have scrolled
		 * past. The counts are of the places a loss was marked, not of
		 * the packets lost at them. */
		ubertooth_report_capture_losses(ut);

		/* A capture whose stream died stopped there, so it holds less than
		 * was asked for. The exit status has to say so, or a caller cannot
		 * tell it from a timeout or a Ctrl-C. The handle is read after the
		 * teardown, because cancelling the transfer can be what brings the
		 * failure out. The survey results below are still printed, since
		 * they are what did arrive, and libubertooth has already printed
		 * what failed.
		 *
		 * A teardown the board refused counts the same way: that board
		 * is left running the mode this capture started, and no other
		 * tool can open it until it is reset or re-plugged. */
		if (ubertooth_stream_failed(ut) || ubertooth_stop_failed(ut))
			exit_status = 1;
	} else {
		/* A read error or a last record cut short is reported and ends
		 * the command with an error, after whatever was decoded. */
		r = stream_rx_file(ut, infile, replay_decode, pn);
		fclose(infile);
		exit_status = rx_file_report(r, infile_name);

		/* An input no whole record could be read from leaves the
		 * capture files holding their headers and nothing else, so
		 * they go the way a refused capture's files do. From the first
		 * record on they stay, however little of the input came after
		 * it. An input read to its end is no failure, an empty one
		 * included, and never reaches here: that replay ran. */
		if (exit_status != 0 && !replay_read_a_record)
			rx_discard_files(ut, &files);

		/* A recorded capture carries the losses it was recorded with, in
		 * the status of its own records, and the replay is where a reader
		 * of the file finds out about them. */
		ubertooth_report_capture_losses(ut);
	}

	if(survey_mode) {
		fprintf(report_stream, "Survey Results\n");
		while((pn=btbb_next_survey_result()) != NULL) {
			lap = btbb_piconet_get_lap(pn);
			if (btbb_piconet_get_flag(pn, BTBB_UAP_VALID)) {
				uap = btbb_piconet_get_uap(pn);
				/* Printable version showing that the NAP is unknown */
				fprintf(report_stream, "??:??:%02X:%02X:%02X:%02X\n", uap,
						(lap >> 16) & 0xFF, (lap >> 8) & 0xFF, lap & 0xFF);
			} else
				fprintf(report_stream, "??:??:??:%02X:%02X:%02X\n",
					   (lap >> 16) & 0xFF, (lap >> 8) & 0xFF, lap & 0xFF);
			//btbb_print_afh_map(pn);
		}
	}
	if(dumpfile != NULL)
		fclose(dumpfile);
	/* The capture files the handle holds, for a replay as for a capture:
	 * the close does nothing to a handle that opened no board. */
	ubertooth_close(ut);
	ubertooth_free(ut);

	return exit_status;
}
