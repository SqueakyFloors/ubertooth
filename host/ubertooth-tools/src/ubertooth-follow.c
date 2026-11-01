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

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <sys/ioctl.h>

#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "ubertooth.h"
#include "ubertooth_callback.h"
#include "opt_parse.h"
#include "request_report.h"
#include "follow_afh.h"
#include <btbb.h>
#include <getopt.h>
#include <limits.h>

/* follow_afh.h repeats the map length so it needs neither libusb nor BlueZ;
 * a mismatch with the library's would hand the board a short map. */
#if FOLLOW_AFH_MAP_BYTES != AFH_MAP_BYTES
#error "FOLLOW_AFH_MAP_BYTES disagrees with libubertooth's AFH_MAP_BYTES"
#endif

/* Milliseconds to wait for the local controller to answer an HCI command. With
 * a zero timeout BlueZ blocks in read() with nothing to interrupt it, so a
 * controller that never answers hangs instead of failing; every read here is
 * bounded. */
#define HCI_TIMEOUT_MS 1000

/* -w is a number of Bluetooth timeslots, each 625 us. The clock the board
 * hops on counts 312.5 us ticks, two to a slot, so a delay in slots is
 * doubled before it is added to that clock. Without this each unit of -w
 * compensated half of what it said: at the default -w 5, 1562.5 us of the
 * 3125 us it asks for, which leaves the board hopping on a clock more than
 * two slots out -- on a hopping piconet, the wrong channel. */
#define CLKN_TICKS_PER_SLOT 2

/* One attempt at the AFH map of the link to the target, for
 * follow_afh_read_map(). */
struct afh_hci {
	int sock;
	uint16_t handle;
	/* errno of the read that failed. Kept here because a later successful
	 * read, or the usleep between attempts, can overwrite errno before the
	 * message is built. */
	int error;
};

static int afh_hci_read(void* ctx, uint8_t* mode, uint8_t* map)
{
	struct afh_hci* hci = ctx;
	int r;

	errno = 0;
	r = hci_read_afh_map(hci->sock, hci->handle, mode, map, HCI_TIMEOUT_MS);
	if (r < 0)
		hci->error = errno;
	return r;
}

static void afh_hci_wait(void* ctx __attribute__((unused)))
{
	/* Each read waits up to a second for the controller, so say what the
	 * pause is for rather than looking like a hang. */
	fputs("AFH map not usable yet, reading it again\n", stderr);
	usleep(FOLLOW_AFH_RETRY_DELAY_US);
}

/* The files a run writes, as the command line named them. Carried to the
 * failure paths below, which remove what this run created. */
struct follow_files {
	const char* pcapng;
	const char* pcap;
	const char* dump;
};

/* Removes the capture files this run created, so that a follow the board or
 * the adapter refused leaves no file behind.
 *
 * The handles say which files exist, since each is set only once its writer
 * has the file open. The files are removed by name while the writers still
 * hold them, and the bytes a writer had buffered go nowhere when the handles
 * are closed with the board. A capture on standard output is not a file of
 * ours to remove.
 *
 * Called from the two helpers below, which all but two failures go through,
 * so a path added later cannot forget it; the two that start the capture give
 * the board back themselves and call this by hand. Before the files are
 * created the handles are NULL and this does nothing. */
static void follow_discard_files(ubertooth_t* ut,
                                 const struct follow_files* files)
{
	if (ut->h_pcapng_bredr != NULL)
		opt_discard_capture_file(files->pcapng);
	if (ut->h_pcap_bredr != NULL)
		opt_discard_capture_file(files->pcap);
	if (dumpfile != NULL)
		opt_discard_capture_file(files->dump);
}

/* Gives the local adapter back. First it drops the link this command made to
 * the target, so a failure does not leave the target connected until its
 * controller times the link out, and then it closes the adapter handle, as
 * ubertooth-scan does, rather than holding a handle on an adapter the tool has
 * finished with. cc says whether there is a link to drop. Every path that
 * opened the adapter comes through here. */
static void follow_release_adapter(int sock, uint16_t handle, int cc)
{
	if (cc)
		hci_disconnect(sock, handle, HCI_OE_USER_ENDED_CONNECTION, 10000);
	close(sock);
}

/* The same, for a failure once the receive thread is running: stop the thread
 * and put the board back first. */
static void follow_cleanup(ubertooth_t* ut, int sock, uint16_t handle, int cc,
                           const struct follow_files* files)
{
	follow_discard_files(ut, files);
	ubertooth_bulk_thread_stop();
	ubertooth_stop(ut);
	ubertooth_free(ut);
	follow_release_adapter(sock, handle, cc);
}

/* Gives the board back along with the adapter, for a failure after the board
 * was opened but before any mode has started.
 *
 * It goes back without a STOP: there is no mode of ours to end, and a STOP to
 * a board that has just refused a request would only add a second failure to
 * the report. The same reasoning as ubertooth-rx's request_failed().
 *
 * The failures before the board is opened use this too: closing a handle that
 * never opened one does nothing, and the handle is allocated with the capture
 * files, well before the board, so there is always one to give back.
 *
 * One close covers both firmware failures: ubertooth_check_api() releases the
 * board itself when it refuses the firmware -- another API version, or the
 * other image -- and firmware it could not read at all leaves the board open,
 * so closing a board that is already released does nothing. */
static void follow_release_board(ubertooth_t* ut, int sock, uint16_t handle,
                                 int cc, const struct follow_files* files)
{
	follow_discard_files(ut, files);
	ubertooth_close(ut);
	ubertooth_free(ut);
	follow_release_adapter(sock, handle, cc);
}

/* A board that refused one of the requests the follow is built on. One line
 * names the request, and the board and the adapter both go back, so a follow
 * that cannot be set up leaves nothing behind. */
static int follow_request_failed(ubertooth_t* ut, int sock, uint16_t handle,
                                 int cc, const struct follow_files* files,
                                 const char* what, int code)
{
	report_refused_request(what, code);
	follow_release_board(ut, sock, handle, cc, files);
	return 1;
}

static void usage(FILE *file)
{
	fprintf(file, "ubertooth-follow - active(bluez) CLK discovery and follow for a target address\n");
	fprintf(file, "Usage:\n");
	fprintf(file, "\t-h this help\n");
	fprintf(file, "\t-V print version information\n");
	fprintf(file, "\t-t <BD_ADDR> target address (xx:xx:xx:xx:xx:xx), which gives the\n");
	fprintf(file, "\t   LAP and UAP to follow; once, not with -l or -u\n");
	fprintf(file, "\t-l <LAP> (3 bytes / 6 hex digits), once, with -u, not with -t\n");
	fprintf(file, "\t-u <UAP> (1 byte / 2 hex digits), once, with -l, not with -t\n");
	fputs(OPT_USAGE_SELECT_DEVICE, file);
	fprintf(file, "\t-r <filename> capture packets to PCAPNG file\n");
	fprintf(file, "\t-q <filename> capture packets to PCAP file\n");
	fprintf(file, "\t-e max_ac_errors (default: %d, range: 0-%d)\n",
	        max_ac_errors, MAX_AC_ERRORS);
	fprintf(file, "\t-d filename\n");
	fprintf(file, "\t-a Enable AFH (needs the whole target address: -t)\n");
	fprintf(file, "\t-b Bluetooth device (hci0)\n");
	fprintf(file, "\t-w USB delay in 625us timeslots (default:5)\n");
	fprintf(file, "\nGive the target as -t, or as -l and -u together. With no target they are read from the local device, in some cases this may give the incorrect address.\n");
}

int main(int argc, char *argv[])
{
	int opt, sock, dev_id, lap = 0, uap = 0, delay = 5;
	opt_once once = {0};
	int have_lap = 0;
	/* -h ends the command once the whole command line has parsed (see below),
	 * and so does -V, whose own result is version_status: it reads the board,
	 * so it can fail where -h cannot. */
	int first_exit_option = 0;
	int do_version = 0, version_status = 0;
	int have_uap = 0;
	int afh_enabled = 0;
	uint8_t afh_map[AFH_MAP_BYTES] = { 0 };
        char* ubertooth_device = NULL;
	char *bt_dev = "hci0";
	char addr[19] = { 0 };
	/* The whole target address from -t. opt_check_once() refuses a second
	 * -t, so have_target is 0 or 1. */
	uint8_t target[OPT_BD_ADDR_BYTES] = { 0 };
	int have_target = 0;
	/* Files named on the command line, opened after every check passes. */
	const char* pcapng_name = NULL;
	const char* pcap_name = NULL;
	const char* dump_name = NULL;
	/* 1 once a capture on "-" owns standard output, which is /dev/null for
	 * the rest of the run: a line the user still needs goes to standard
	 * error instead. */
	int capture_took_stdout = 0;
	uint32_t clock = 0;
	/* CLKN as the board reports it, which the offset below is measured
	 * from. */
	uint32_t board_clkn = 0;
	uint16_t accuracy = 0, handle = 0, offset = 0;
	bdaddr_t bdaddr = { { 0 } };
	btbb_piconet *pn = NULL;
	struct hci_dev_info di;
	/* 1 once hci_create_connection() has given us a remote ACL handle. */
	int cc = 0;
	/* 1 when exactly one -l and one -u named the piconet to follow. */
	int have_lap_uap = 0;
	/* 1 when the command line named a remote target, either way. */
	int remote_target = 0;
	int status;

	/* Allocated once the whole command line has been accepted (below):
	 * every refusal above returns without a board, and the handle carries a
	 * 72 MB fifo. The piconet to follow is allocated there too. */
	ubertooth_t* ut = NULL;

	/* Report a bad option ourselves rather than letting getopt print it:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	/* Named, so the loop below and opt_report_bad_option()
	 * cannot be given different letters. */
	const char options[] = ":hVl:u:U:e:d:ab:w:r:q:t:";
	opterr = 0;
	while ((opt=getopt(argc, argv, options)) != EOF) {
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
		switch(opt) {
		case 't':
			if (opt_require_bd_addr(optarg, "target address", NULL,
			                        target) < 0)
				return 1;
			++have_target;
			break;
		case 'l': {
			uint32_t value;
			if (opt_require_lap(optarg, &value) < 0)
				return 1;
			lap = (int)value;
			++have_lap;
			break;
		}
		case 'u': {
			uint8_t value;
			if (opt_require_uap(optarg, &value) < 0)
				return 1;
			uap = value;
			++have_uap;
			break;
		}
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
		case 'e':
			if (opt_require_int(optarg, 0, MAX_AC_ERRORS,
			                    "maximum access code errors", "",
			                    &max_ac_errors) < 0)
				return 1;
			break;
		case 'd':
			dump_name = optarg;
			break;
		case 'a':
			afh_enabled = 1;
			break;
		case 'b':
			bt_dev = optarg;
			if (bt_dev == NULL) {
				perror(optarg);
				return 1;
			}
			break;
		case 'w': //wait
			if (opt_require_count(optarg, 0, "USB delay",
			                      "timeslots", "0 or more", &delay) < 0)
				return 1;
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

	/* ubertooth-follow takes no operands: refuse a leftover word before any
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

	/* -t is the whole address and -l with -u is part of one: two ways to
	 * write the same setting, which the tool does not reconcile. */
	if (have_target > 0 && (have_lap > 0 || have_uap > 0)) {
		fputs("error: -t gives the whole target address, so it cannot be "
		      "combined with -l or -u\n", stderr);
		return 1;
	}

	have_lap_uap = (have_lap > 0) && (have_uap > 0);
	remote_target = have_target || have_lap_uap;

	/* Standard output carries one stream, so only one capture can have it:
	 * a PCAP file and a packet dump interleaved there would be neither. */
	if (pcap_name != NULL && dump_name != NULL &&
	    strcmp(pcap_name, "-") == 0 && strcmp(dump_name, "-") == 0) {
		fputs("error: -q and -d cannot both write to standard output\n",
		      stderr);
		return 1;
	}

	/* -a reads the map from the target over an ACL link, so it needs an
	 * address a connection can be made to, which is all six bytes of it:
	 * -l and -u are four. Refused here, with the rest of the command line,
	 * so no file is created and no device opened. No link has been
	 * attempted yet, so the handle is not what is in question.
	 *
	 * It is asked before the half-given pair below, because -a needs -t
	 * whatever -l and -u say: completing the pair would not make -a work. */
	if (afh_enabled) {
		const char* refusal =
			follow_afh_handle_refusal(have_target, 1);
		if (refusal != NULL) {
			fprintf(stderr, "error: %s\n", refusal);
			return 1;
		}
	}

	/* -l and -u name the piconet only together: a LAP identifies no piconet
	 * without the UAP it belongs to, and a UAP alone identifies nothing. So
	 * half the pair is a target the command cannot be carried out on, and
	 * the missing half is named. The same refusal ubertooth-rx gives a -u
	 * without its -l.
	 *
	 * Half a pair used to fall into the no-target path further down, which
	 * says "No address given" -- untrue, one was given -- and follows the
	 * local adapter's own address instead of the piconet the user named. */
	if (have_lap > 0 && have_uap == 0) {
		fputs("error: -l <LAP> requires -u <UAP>\n", stderr);
		return 1;
	}
	if (have_uap > 0 && have_lap == 0) {
		fputs("error: -u <UAP> requires -l <LAP>\n", stderr);
		return 1;
	}

	/* A capture bound for standard output must not go to a terminal. That
	 * is a problem with the command line, so it is refused here, before a
	 * board or an adapter is opened; the stream itself is taken over
	 * further down, where the file is created. */
	if (opt_check_capture_destination(pcap_name, "capture data") < 0 ||
	    opt_check_capture_destination(dump_name, "dump data") < 0)
		return 1;

	/* Whether a capture will own this process's standard output, which the
	 * command line already says. Known before the stream is actually taken
	 * over, because the lines that have to go to standard error instead --
	 * the AFH map -a reports -- are written before that. */
	capture_took_stdout = opt_writes_to_stdout(pcap_name)
	                      || opt_writes_to_stdout(dump_name);

	/* The files this run will write, for the failure paths that have to
	 * remove them again: see follow_discard_files(). */
	const struct follow_files files = {
		pcapng_name, pcap_name, dump_name
	};

	/* The piconet this follow is keyed on, allocated here rather than
	 * before the command line is read: -h and -V still answer on a machine
	 * that has no memory for one, and nothing has been opened yet that a
	 * failure would have to give back. */
	pn = btbb_piconet_new();
	if (pn == NULL) {
		/* Out of memory, which btbb_init_piconet() below would
		 * dereference. A follow with no piconet has nothing to do, so
		 * the run ends here. */
		fputs("error: out of memory\n", stderr);
		return 1;
	}

	/* Every check has passed, so the handle and its 72 MB fifo can be
	 * allocated: the capture files further down are kept on it. */
	ut = ubertooth_init();
	if (ut == NULL)
		return 1;

	/* A named adapter that does not exist, or one that cannot be opened,
	 * used to go unnoticed until a later HCI call worked on fd -1.
	 *
	 * Each call clears errno first and gives a reason only if there is one,
	 * the way ubertooth-scan reports the same two failures: BlueZ does not
	 * promise an errno for every way these can fail, so a leftover one from
	 * an earlier call would be reported as this failure's cause, and a
	 * failure that set none would read "Undefined error: 0". */
	errno = 0;
	dev_id = hci_devid(bt_dev);
	if (dev_id < 0) {
		if (errno != 0)
			fprintf(stderr, "error: Bluetooth device %s not found: %s\n",
			        bt_dev, strerror(errno));
		else
			fprintf(stderr, "error: Bluetooth device %s not found\n",
			        bt_dev);
		ubertooth_close(ut); /* nothing is open on it yet; harmless */
		ubertooth_free(ut);
		return 1;
	}
	errno = 0;
	sock = hci_open_dev(dev_id);
	if (sock < 0) {
		if (errno != 0)
			fprintf(stderr, "error: cannot open Bluetooth device %s: %s\n",
			        bt_dev, strerror(errno));
		else
			fprintf(stderr, "error: cannot open Bluetooth device %s\n",
			        bt_dev);
		ubertooth_close(ut); /* nothing is open on it yet; harmless */
		ubertooth_free(ut);
		return 1;
	}

	/* What the follow is keyed on: the whole address from -t, the LAP and
	 * UAP from -l and -u, or, with no target named, the local adapter's own
	 * address. Only a target gets connected to; that connection is what
	 * yields its clock offset. */
	if (have_target) {
		uint32_t target_lap;
		uint8_t target_uap;

		/* Only a NULL address fails, and target is an array. */
		opt_bd_addr_lap_uap(target, &target_lap, &target_uap);
		lap = (int)target_lap;
		uap = target_uap;
		snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
		         target[0], target[1], target[2],
		         target[3], target[4], target[5]);
	} else if (have_lap_uap) {
		/* -l and -u are four bytes of the six, so the NAP goes out as
		 * zero. That placeholder is not what decides whether the page
		 * reaches the target: of the paged device's address a page uses
		 * only the LAP and the UAP, never the NAP (Bluetooth Core, Vol 2,
		 * Part B 1.3, 6.4.6, and Table 2.3). -t is for a caller who
		 * knows the whole address. */
		snprintf(addr, sizeof(addr), "00:00:%02X:%02X:%02X:%02X",
			uap,
			(lap >> 16) & 0xFF,
			(lap >> 8) & 0xFF,
			lap & 0xFF
		);
	} else {
		/* These lines say what the tool is doing, not what was asked
		 * for, so they are on standard error, where the rest of the
		 * tool's progress goes. */
		fputs("No address given, reading address from device\n", stderr);
		/* With a zero timeout BlueZ blocks in read() with nothing to
		 * interrupt it, so a controller that never answers hangs instead
		 * of failing. Every read here gives it a second, as the clock
		 * offset read already did. */
		if (hci_read_bd_addr(sock, &bdaddr, HCI_TIMEOUT_MS) < 0) {
			fprintf(stderr, "error: reading the address of %s failed: %s\n",
			        bt_dev, strerror(errno));
			follow_release_board(ut, sock, handle, cc, &files);
			return 1;
		}
		lap = bdaddr.b[0] | bdaddr.b[1] << 8 | bdaddr.b[2] << 16;
		uap = bdaddr.b[3];
	}

	btbb_init_piconet(pn, lap);
	btbb_piconet_set_uap(pn, uap);

	if (!remote_target) {
		fprintf(stderr, "LAP=%06x UAP=%02x\n", lap, uap);
	} else {
		if (have_target)
			fprintf(stderr, "Target %s: LAP=%06x UAP=%02x\n",
			        addr, lap, uap);
		else
			fprintf(stderr, "Address given, assuming address is remote\n"
			        "Address: %s\n", addr);
		str2ba(addr, &bdaddr);

		if (hci_devinfo(dev_id, &di) < 0) {
			fprintf(stderr, "error: reading the settings of %s failed: %s\n",
			        bt_dev, strerror(errno));
			follow_release_board(ut, sock, handle, cc, &files);
			return 1;
		}

		/* The handle this fills in is the only remote ACL handle the tool
		 * ever has, so cc below is what says it is usable.
		 *
		 * A page keyed on -l and -u goes to an address whose NAP the
		 * tool guessed as 00:00, which the page does not carry, so the
		 * guess is not what a failure here means. The message names it
		 * anyway, because a
		 * user reading "connecting to 00:00:..." wants to know where
		 * those two bytes came from, and offers -t, which removes the
		 * guess and is what -a needs. The command stops either way: the
		 * page is what yields the target's clock offset, and a follow of
		 * a named target without it would hop on the local adapter's
		 * clock rather than the target's.
		 *
		 * errno is cleared first and the reason given only if there is
		 * one, as the two adapter failures above do: BlueZ does not
		 * promise an errno for every way the page can fail, so a
		 * leftover one would be reported as its cause and a failure that
		 * set none would read "Undefined error: 0". */
		errno = 0;
		if (hci_create_connection(sock, &bdaddr,
					htobs(di.pkt_type & ACL_PTYPE_MASK),
					0, 0x01, &handle, 25000) < 0) {
			char reason[128] = { 0 };

			if (errno != 0)
				snprintf(reason, sizeof(reason), ": %s",
				         strerror(errno));
			if (have_target)
				fprintf(stderr, "error: connecting to %s "
				        "failed%s\n", addr, reason);
			else
				fprintf(stderr, "error: connecting to %s "
				        "failed%s; -l and -u give four of the six "
				        "bytes of the target address, so 00:00 is "
				        "a guess: give the whole target address "
				        "with -t xx:xx:xx:xx:xx:xx\n",
				        addr, reason);
			follow_release_board(ut, sock, handle, cc, &files);
			return 1;
		}
		cc = 1;
	}

	//Experimental AFH map reading from remote device
	if(afh_enabled) {
		struct afh_hci hci;
		follow_afh_reader reader;
		follow_afh_read read_out;
		char line[256];
		char map_line[AFH_MAP_LINE_LEN];
		const char* refusal;

		/* The branch above is the only one that fills in handle, and it
		 * either sets cc or returns, so this cannot fire today. It is
		 * here so that a future path which reaches the read without a
		 * handle says so instead of reading on handle 0. */
		refusal = follow_afh_handle_refusal(have_target, cc);
		if (refusal != NULL) {
			fprintf(stderr, "error: %s\n", refusal);
			follow_release_board(ut, sock, handle, cc, &files);
			return 1;
		}

		hci.sock = sock;
		hci.handle = handle;
		hci.error = 0;
		reader.read = afh_hci_read;
		reader.wait = afh_hci_wait;
		reader.ctx = &hci;

		/* A map that is empty or too small may not have settled yet, so
		 * it is read again, a bounded number of times. A read that fails
		 * is not retried and leaves read_out.map zero, so nothing
		 * undefined can reach libbtbb or the board. */
		follow_afh_read_map(&reader, FOLLOW_AFH_READ_ATTEMPTS, &read_out);
		follow_afh_message(&read_out,
		                   hci.error != 0 ? strerror(hci.error) : NULL,
		                   line, sizeof(line));

		switch (follow_afh_action_for(read_out.result)) {
		case FOLLOW_AFH_ACTION_APPLY:
			memcpy(afh_map, read_out.map, sizeof(afh_map));
			fprintf(stderr, "%s\n", line);
			btbb_piconet_set_afh_map(pn, afh_map);
			/* The map is what -a was asked for, so it goes to
			 * standard output -- written here rather than by
			 * libbtbb's btbb_print_afh_map(), which wrote the same
			 * line from inside the library: standard output belongs
			 * to the tool. A capture owns that stream when it is
			 * "-" and takes the map with it, so then the line goes
			 * to standard error, where -a still answers. */
			afh_map_line(afh_map, map_line, sizeof(map_line));
			fprintf(capture_took_stdout ? stderr : stdout, "%s\n",
			        map_line);
			break;
		case FOLLOW_AFH_ACTION_DISABLE:
			fprintf(stderr, "%s\n", line);
			afh_enabled = 0;
			break;
		case FOLLOW_AFH_ACTION_FAIL:
			/* No board has been opened yet, so the adapter is all
			 * there is to give back. */
			fprintf(stderr, "error: %s\n", line);
			follow_release_board(ut, sock, handle, cc, &files);
			return 1;
		}
	} else {
		fputs("Not use AFH\n", stderr);
	}

	/* Stop on Ctrl-C from here on, before the board is opened: a signal
	 * that arrives while one is being found would otherwise be discarded.
	 *
	 * Not before the adapter work above. BlueZ takes its own time there --
	 * the page waits up to 25 seconds -- and retries its calls itself when
	 * a signal cuts them short, so a handler installed earlier would catch
	 * a Ctrl-C that nothing could act on until the page returned, where
	 * today the signal ends the tool at once. Which of the two a user wants
	 * is a question for the maintainer, so the behavior is left as it
	 * was. */
	ubertooth_catch_stop_signals();

	/* Fails when no board matches -U or it cannot be opened; the library
	 * has already said why, and has left nothing of its own open. Every
	 * failure from here to the receive loop gives the board and the adapter
	 * back on the way out, dropping the link to the target with it; the
	 * receive thread is not running yet, so there is nothing else to undo. */
	if (ubertooth_connect_select(ut, ubertooth_device) < 0) {
		follow_release_board(ut, sock, handle, cc, &files);
		/* A selection the user stopped opened no board, which is not a
		 * failure to find one. */
		return ubertooth_open_stopped() ? 0 : 1;
	}

	/* A firmware this host will not drive. The library has reported it. */
	int r = ubertooth_check_api(ut);
	if (r < 0) {
		follow_release_board(ut, sock, handle, cc, &files);
		return 1;
	}

	/* Stopped while the board was being opened: the run ends here, with the
	 * board and the adapter given back, rather than starting a mode and
	 * stopping it again. */
	if (ubertooth_stop_requested()) {
		follow_release_board(ut, sock, handle, cc, &files);
		return 0;
	}

	/* The board is open and usable, so the files this run writes can be
	 * opened. Not before: a run refused for its firmware used to leave an
	 * empty capture behind, which a user then had to tell apart from a
	 * capture that caught nothing. Every failure from here to the capture
	 * loop removes them again, through follow_discard_files(), for the same
	 * reason.
	 *
	 * A lone "-" redirects this process's standard output, which is why
	 * nothing may be printed there after this point except the capture
	 * itself. */
	errno = 0;
	if (pcapng_name != NULL &&
	    btbb_pcapng_create_file(pcapng_name, "Ubertooth", &ut->h_pcapng_bredr)) {
		opt_report_create_failure(pcapng_name, errno);
		follow_release_board(ut, sock, handle, cc, &files);
		return 1;
	}
	if (ut->h_pcapng_bredr) {
		/* The target's address, recorded once so a reader of the
		 * capture knows whose packets these are. It goes in here
		 * rather than where the address is worked out, because the
		 * file does not exist until this point: written there, it
		 * reached a handle that was still NULL and no capture ever
		 * carried the address.
		 *
		 * Reported when it cannot be written. The capture goes on
		 * either way, but without this the packets in it have nothing
		 * to say which piconet they belong to. */
		if (btbb_pcapng_record_bdaddr(ut->h_pcapng_bredr,
		                              (((uint32_t)uap)<<24)|lap,
		                              0xff, 0) < 0)
			fprintf(stderr, "warning: cannot record the target "
			        "address in the capture file\n");
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
			follow_release_board(ut, sock, handle, cc, &files);
			return 1;
		}
		errno = 0;
		failed = btbb_pcap_create_file(file, &ut->h_pcap_bredr);
		/* Read now: the close() and the free() below may set an errno
		 * of their own, and this one is the reason reported. */
		create_errno = errno;
		if (saved_fd >= 0)
			close(saved_fd);
		free(file);
		if (failed) {
			/* The name the user gave, not the /dev/fd path a lone
			 * "-" is opened through. */
			opt_report_create_failure(pcap_name, create_errno);
			follow_release_board(ut, sock, handle, cc, &files);
			return 1;
		}
	}
	if (dump_name != NULL) {
		dumpfile = opt_open_dump_file(dump_name, "dump data");
		if (dumpfile == NULL) {
			follow_release_board(ut, sock, handle, cc, &files);
			return 1;
		}
	}

	/* libbtbb refuses only a -e outside its own range, which the check on
	 * -e above has already caught, and it says so itself. */
	r = btbb_init(max_ac_errors);
	if (r < 0) {
		follow_release_board(ut, sock, handle, cc, &files);
		return 1;
	}

	/* The target's address is set before the receive transfer is submitted
	 * below. The request is synchronous, so the board has it before
	 * anything is in flight; the asynchronous ones below cannot move up
	 * here, because the poll thread is what completes them. The result is
	 * checked, as the AFH requests below are: a board without the address
	 * matches no packet, so a follow that carried on would capture nothing
	 * the user asked for. It used to carry on, having printed only a
	 * generic line that did not say which request had failed. */
	r = cmd_set_bdaddr_quiet(ut->devh, btbb_piconet_get_bdaddr(pn));
	if (r < 0)
		return follow_request_failed(ut, sock, handle, cc, &files,
		                             "set the Bluetooth address", r);

	// init USB transfer
	/* Each of the three steps that set the capture up exits 1, like every
	 * other failure here: the failing call's own return value used to go out
	 * as the exit status, and a shell wraps that into a byte a script cannot
	 * read. libubertooth has already said what failed in each case. */
	r = ubertooth_bulk_init(ut);
	if (r < 0) {
		/* nothing is in flight, but the board is still open and in
		 * whatever mode an earlier command left it. The two helpers
		 * above do not fit here, so the files are discarded by hand. */
		follow_discard_files(ut, &files);
		ubertooth_stop(ut);
		ubertooth_free(ut);
		follow_release_adapter(sock, handle, cc);
		return 1;
	}

	r = ubertooth_bulk_thread_start();
	if (r != 0) {
		/* Not "< 0": this is pthread_create()'s errno, which is positive,
		 * so a failure used to slip through and leave the capture with
		 * nobody to complete its transfers. */
		report_capture_thread_failure(r);
		/* The receive transfer is in flight and the board is about to be
		 * left in its mode: give both back. Not follow_cleanup(), which
		 * would join a thread that was never created. */
		follow_discard_files(ut, &files);
		ubertooth_stop(ut);
		ubertooth_free(ut);
		follow_release_adapter(sock, handle, cc);
		return 1;
	}

	/* Read clocks here to ensure clock is closest to real value. Without a
	 * clock there is nothing to follow, so a failed read stops the command
	 * rather than hopping from an uninitialised one. The receive thread is
	 * already running and the link to the target is up by now, so all three
	 * failures leave through follow_cleanup(). */
	if (hci_read_clock(sock, 0, 0, &clock, &accuracy, HCI_TIMEOUT_MS) < 0) {
		fprintf(stderr, "error: reading the clock of %s failed: %s\n",
		        bt_dev, strerror(errno));
		follow_cleanup(ut, sock, handle, cc, &files);
		return 1;
	}
	/* The controller answers in little-endian order, which has to be
	 * brought into host order before any arithmetic on it. ubertooth-scan
	 * does the same with the clock offset it prints. */
	clock = btohl(clock);
	if (cc) {
		if (hci_read_clock_offset(sock, handle, &offset,
		                         HCI_TIMEOUT_MS) < 0) {
			fprintf(stderr, "error: reading the clock offset of the "
			        "target failed: %s\n", strerror(errno));
			follow_cleanup(ut, sock, handle, cc, &files);
			return 1;
		}
		clock += (btohs(offset) << 2);   // Correct offset
	}

	/* The board's own clock, read here beside the clocks above so that the
	 * three are measured as close together as the requests allow.
	 *
	 * The board is told to hop by an offset which it adds to the clock it
	 * is already running, so what it needs is the difference between the
	 * piconet's clock and its own. This run used to zero CLKN instead and
	 * send the piconet's clock as it stood, which is the same thing only
	 * while CLKN is not running.
	 *
	 * It usually is not: the firmware holds CLKN at zero and stops its
	 * timer whenever no capture mode is running, and nothing this tool does
	 * before the request below starts one. The run that does not fit is a
	 * follow of a board an earlier run left streaming, which this tool
	 * expects often enough to have a comment of its own further down. CLKN
	 * is running on such a board, and then the reset cost two things.
	 * Everything between it and the start of the hopping -- the endpoint
	 * flush, the thread creation, both reads above -- was error in the
	 * followed clock, so a follow hopped as far past the piconet's clock as
	 * its own setup had taken: measured on a board, 145 of the 312.5 us
	 * ticks for a 20 ms setup and 771 for 200 ms, where one slot is two
	 * ticks. And the reset moved the clock the board stamps its records
	 * with, under records already queued on the board: the host reads each
	 * record's period count as a difference from the previous record's, so
	 * 150 then 0 read as 106 periods gone by and about 34,700 s was added
	 * to every later timestamp.
	 *
	 * Reading the clock is also what -w compensates: the piconet's clock
	 * was read a round trip ago and the board's is read now, so the delay
	 * covers measuring the clocks and applying them rather than the whole
	 * setup. */
	r = cmd_read_clock_quiet(ut->devh, &board_clkn);
	if (r < 0) {
		/* Not follow_request_failed(): that gives the board back
		 * without a STOP, which is right only before a mode has
		 * started. The receive transfer is in flight by now. */
		report_refused_request("read the board's clock", r);
		follow_cleanup(ut, sock, handle, cc, &files);
		return 1;
	}

	/* The delay is counted in the clock's own ticks here, so a large -w
	 * cannot overflow a signed multiplication on the way. All three terms
	 * are 32-bit counts the board wraps anyway, so the subtraction is taken
	 * modulo that and then cast, as libbtbb casts the same difference when
	 * it hands over a clock of its own. */
	btbb_piconet_set_clk_offset(pn,
		(int)(clock + (uint32_t)delay * CLKN_TICKS_PER_SLOT
		      - board_clkn));
	btbb_piconet_set_flag(pn, BTBB_FOLLOWING, 1);
	btbb_piconet_set_flag(pn, BTBB_CLK27_VALID, 1);

	/* The modulation this follow reads, Classic, stated before the mode
	 * request below and not after it as the AFH map is: see
	 * ubertooth-util(1), CARRY-OVER BETWEEN RUNS. Without it, a board an
	 * interrupted `ubertooth-dump -l` had left set to low energy followed
	 * at that deviation, finding little and saying nothing. */
	if (cmd_set_modulation(ut->devh, MOD_BT_BASIC_RATE) < 0) {
		follow_cleanup(ut, sock, handle, cc, &files);
		return 1;
	}

	// tell ubertooth to start hopping and send packets
	r = cmd_start_hopping(ut->devh, btbb_piconet_get_clk_offset(pn), 0);
	if (r < 0) {
		follow_cleanup(ut, sock, handle, cc, &files);
		return 1;
	}

	/* Say what this follow wants of the board's AFH map, after the mode has
	 * started rather than before: starting a mode forgets the map, so one
	 * sent first would be thrown away. A follow with no -a clears it, which
	 * is what covers a run that follows a killed follow: the board is still
	 * streaming in the same mode, so the firmware sees the same mode request
	 * again and cannot tell this new run from a re-sync of the old one.
	 * Both are checked, because carrying on would hop on the wrong channel
	 * set either way. The request is asynchronous, so this catches a
	 * submission that fails, not a transfer that fails later. */
	if (afh_enabled)
		r = cmd_set_afh_map(ut->devh, afh_map);
	else
		r = cmd_clear_afh_map(ut->devh);
	if (r < 0) {
		fprintf(stderr, "error: %s the board's AFH map failed\n",
		        afh_enabled ? "setting" : "clearing");
		follow_cleanup(ut, sock, handle, cc, &files);
		return 1;
	}

	/* The clock reads are done, so the capture needs nothing more from the
	 * adapter. The link to the target was already dropped at this point; the
	 * adapter handle now goes back with it, rather than being held for the
	 * whole capture, which runs until it is interrupted. */
	follow_release_adapter(sock, handle, cc);

	// receive and process each packet
	while(!ubertooth_stopping(ut)) {
		ubertooth_bulk_receive(ut, cb_rx, pn);
	}

	ubertooth_bulk_thread_stop();

	ubertooth_stop(ut);

	/* What the capture is missing, once the packets have scrolled past. The
	 * counts are of the places a loss was marked, not of the packets lost
	 * at them. */
	ubertooth_report_capture_losses(ut);

	/* A follow whose stream died stopped there, so it holds less than was
	 * asked for. The exit status has to say so, or a caller cannot tell it
	 * from a Ctrl-C. The handle is read after the teardown, because
	 * cancelling the transfer can be what brings the failure out.
	 * libubertooth has already printed what failed.
	 *
	 * A teardown the board refused counts the same way: that board is left
	 * running the mode this follow started, and no other tool can open it
	 * until it is reset or re-plugged. What was captured stays written
	 * out. */
	status = (ubertooth_stream_failed(ut) || ubertooth_stop_failed(ut))
	         ? 1 : 0;
	ubertooth_free(ut);
	return status;
}
