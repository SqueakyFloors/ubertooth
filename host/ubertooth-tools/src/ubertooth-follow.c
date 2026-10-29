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
static void follow_cleanup(ubertooth_t* ut, int sock, uint16_t handle, int cc)
{
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
 * One close covers both firmware version failures: ubertooth_check_api()
 * releases the board itself when the firmware reports a version this host
 * will not drive, and a version it could not read at all leaves the board
 * open, so closing a board that is already released does nothing. */
static void follow_release_board(ubertooth_t* ut, int sock, uint16_t handle,
                                 int cc)
{
	ubertooth_close(ut);
	ubertooth_free(ut);
	follow_release_adapter(sock, handle, cc);
}

/* A board that refused one of the requests the follow is built on. One line
 * names the request, and the board and the adapter both go back, so a follow
 * that cannot be set up leaves nothing behind. */
static int follow_request_failed(ubertooth_t* ut, int sock, uint16_t handle,
                                 int cc, const char* what, int code)
{
	report_refused_request(what, code);
	follow_release_board(ut, sock, handle, cc);
	return 1;
}

static void usage()
{
	printf("ubertooth-follow - active(bluez) CLK discovery and follow for a target address\n");
	printf("Usage:\n");
	printf("\t-h this help\n");
	printf("\t-t <BD_ADDR> target address (xx:xx:xx:xx:xx:xx), which gives the\n");
	printf("\t   LAP and UAP to follow; once, not with -l or -u\n");
	printf("\t-l <LAP> (3 bytes / 6 hex digits), once, with -u, not with -t\n");
	printf("\t-u <UAP> (1 byte / 2 hex digits), once, with -l, not with -t\n");
	fputs(OPT_USAGE_SELECT_DEVICE, stdout);
	printf("\t-r <filename> capture packets to PCAPNG file\n");
	printf("\t-q <filename> capture packets to PCAP file\n");
	printf("\t-e max_ac_errors (default: %d, range: 0-%d)\n",
	       max_ac_errors, MAX_AC_ERRORS);
	printf("\t-d filename\n");
	printf("\t-a Enable AFH (needs the whole target address: -t)\n");
	printf("\t-b Bluetooth device (hci0)\n");
	printf("\t-w USB delay in 625us timeslots (default:5)\n");
	printf("\nGive the target as -t, or as -l and -u together. With no target they are read from the local device, in some cases this may give the incorrect address.\n");
//	printf("If an input file is not specified, an Ubertooth device is used for live capture.\n");
}

int main(int argc, char *argv[])
{
	int opt, sock, dev_id, lap = 0, uap = 0, delay = 5;
	opt_once once = {0};
	int have_lap = 0;
	/* -h ends the command once the whole command line has parsed (see below). */
	int first_exit_option = 0;
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
	uint16_t accuracy = 0, handle = 0, offset = 0;
	bdaddr_t bdaddr = { { 0 } };
	btbb_piconet *pn;
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
	 * 72 MB fifo. */
	ubertooth_t* ut = NULL;

	pn = btbb_piconet_new();

	/* Report a bad option ourselves rather than letting getopt print it:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":hl:u:U:e:d:ab:w:r:q:t:")) != EOF) {
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
		case 'h':
			if (!first_exit_option)
				first_exit_option = 'h';
			break;
		default:
			opt_report_bad_option(opt, argc, argv);
			return 1;
		}
	}

	/* ubertooth-follow takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h ends the command, but only once the whole command line has
	 * parsed: a bad option value or a stray word is reported even
	 * alongside it. */
	if (first_exit_option == 'h') {
		usage();
		return 0;
	}

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

	/* Every check has passed, so the handle and its 72 MB fifo can be
	 * allocated: the capture files below are kept on it. */
	ut = ubertooth_init();
	if (ut == NULL)
		return 1;

	/* And the files can be opened: a refused command line creates and
	 * truncates nothing. */
	errno = 0;
	if (pcapng_name != NULL &&
	    btbb_pcapng_create_file(pcapng_name, "Ubertooth", &ut->h_pcapng_bredr)) {
		opt_report_create_failure(pcapng_name, errno);
		ubertooth_close(ut);
		ubertooth_free(ut);
		return 1;
	}
	if (pcap_name != NULL) {
		/* A lone "-" is standard output, which the capture then owns:
		 * see opt_capture_path(). The per-packet report this tool
		 * prints there goes nowhere for such a run, which is what the
		 * man page says of it; the notes and failures are on standard
		 * error and still arrive. */
		int saved_fd;
		char *file = opt_capture_path(pcap_name, "capture data", &saved_fd);
		int failed;

		if (file == NULL) {
			if (saved_fd >= 0)
				close(saved_fd);
			ubertooth_close(ut);
			ubertooth_free(ut);
			return 1;
		}
		errno = 0;
		failed = btbb_pcap_create_file(file, &ut->h_pcap_bredr);
		if (saved_fd >= 0) {
			capture_took_stdout = 1;
			close(saved_fd);
		}
		free(file);
		if (failed) {
			/* The name the user gave, not the /dev/fd path a lone
			 * "-" is opened through, and no reason: the PCAP writer
			 * prints the real one itself and leaves errno holding
			 * whatever it last set. */
			opt_report_create_failure(pcap_name, 0);
			ubertooth_close(ut);
			ubertooth_free(ut);
			return 1;
		}
	}
	if (dump_name != NULL) {
		dumpfile = opt_open_dump_file(dump_name, "dump data");
		if (dumpfile != NULL && strcmp(dump_name, "-") == 0)
			capture_took_stdout = 1;
		if (dumpfile == NULL) {
			/* The capture files are on the handle, so the close is
			 * what gets them written out and shut; no board has
			 * been opened yet, and the close does nothing to a
			 * handle that has none. */
			ubertooth_close(ut);
			ubertooth_free(ut);
			return 1;
		}
	}

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
		ubertooth_close(ut); /* the capture files, as above */
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
		ubertooth_close(ut); /* the capture files, as above */
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
		/* -l and -u are four bytes of the six, and a sniffer cannot see
		 * the other two, so the connection can only be attempted to a
		 * target whose address begins 00:00. -t reaches any other
		 * target. */
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
			follow_release_board(ut, sock, handle, cc);
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
			follow_release_board(ut, sock, handle, cc);
			return 1;
		}

		/* The handle this fills in is the only remote ACL handle the tool
		 * ever has, so cc below is what says it is usable.
		 *
		 * A page keyed on -l and -u goes to a guessed address: the two
		 * most significant bytes are not in any packet a sniffer sees,
		 * so the tool sends 00:00 and the page reaches only a target
		 * whose address really begins that way. The failure therefore
		 * says what to do about it -- give the whole address with -t --
		 * in the same terms as -a needing -t. The command stops either
		 * way: the page is what yields the target's clock offset, and a
		 * follow of a named target without it would hop on the local
		 * adapter's clock rather than the target's.
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
			follow_release_board(ut, sock, handle, cc);
			return 1;
		}
		cc = 1;
	}

	if (ut->h_pcapng_bredr) {
		btbb_pcapng_record_bdaddr(ut->h_pcapng_bredr,
		                            (((uint32_t)uap)<<24)|lap,
		                            0xff, 0);
	}

	//Experimental AFH map reading from remote device
	if(afh_enabled) {
		struct afh_hci hci;
		follow_afh_reader reader;
		follow_afh_read read_out;
		char line[256];
		const char* refusal;

		/* The branch above is the only one that fills in handle, and it
		 * either sets cc or returns, so this cannot fire today. It is
		 * here so that a future path which reaches the read without a
		 * handle says so instead of reading on handle 0. */
		refusal = follow_afh_handle_refusal(have_target, cc);
		if (refusal != NULL) {
			fprintf(stderr, "error: %s\n", refusal);
			follow_release_board(ut, sock, handle, cc);
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
			/* libbtbb prints the map itself, on standard output. A
			 * capture owns that stream when it is "-", and takes
			 * the map with it: the line is written here instead,
			 * in the wording btbb_print_afh_map() uses, so -a
			 * still answers. */
			if (capture_took_stdout)
				fprintf(stderr,
				        "AFH map: 0x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
				        afh_map[0], afh_map[1], afh_map[2], afh_map[3],
				        afh_map[4], afh_map[5], afh_map[6], afh_map[7],
				        afh_map[8], afh_map[9]);
			else
				btbb_print_afh_map(pn);
			break;
		case FOLLOW_AFH_ACTION_DISABLE:
			fprintf(stderr, "%s\n", line);
			afh_enabled = 0;
			break;
		case FOLLOW_AFH_ACTION_FAIL:
			/* No board has been opened yet, so the adapter is all
			 * there is to give back. */
			fprintf(stderr, "error: %s\n", line);
			follow_release_board(ut, sock, handle, cc);
			return 1;
		}
	} else {
		fputs("Not use AFH\n", stderr);
	}

	/* Clean up on exit. */
	register_cleanup_handler(ut, 0);

	/* Fails when no board matches -U or it cannot be opened; the library
	 * has already said why, and has left nothing of its own open. Every
	 * failure from here to the receive loop gives the board and the adapter
	 * back on the way out, dropping the link to the target with it; the
	 * receive thread is not running yet, so there is nothing else to undo. */
	if (ubertooth_connect_select(ut, ubertooth_device) < 0) {
		follow_release_board(ut, sock, handle, cc);
		return 1;
	}

	/* A firmware this host will not drive. The library has reported it. */
	int r = ubertooth_check_api(ut);
	if (r < 0) {
		follow_release_board(ut, sock, handle, cc);
		return 1;
	}

	/* libbtbb refuses only a -e outside its own range, which the check on
	 * -e above has already caught, and it says so itself. */
	r = btbb_init(max_ac_errors);
	if (r < 0) {
		follow_release_board(ut, sock, handle, cc);
		return 1;
	}

	/* The target's address and the board's clock are set before the receive
	 * transfer is submitted below, because zeroing CLKN moves the clock the
	 * board stamps its records with. A record sent before the reset and read
	 * after it carries the period count from before: the host reads that
	 * count as a difference from the previous record's, so 150 then 0 reads
	 * as 106 periods gone by and adds about 34,700 s to every later
	 * timestamp. Both requests are synchronous, so the board has them before
	 * anything is in flight; the asynchronous ones below cannot move up here,
	 * because the poll thread is what completes them. Both results are
	 * checked, as the AFH requests below are: a board that took neither
	 * would follow nothing the user asked for. It used to carry on, having
	 * printed only a generic line that did not say which request had failed.
	 * A board without the address matches no packet, and a board whose CLKN
	 * was never zeroed keeps the clock it has been running since it was
	 * plugged in, which start-hopping adds its offset to rather than
	 * replacing, so the follow hops on the wrong clock and every packet time
	 * is out by however long the board had been up.
	 *
	 * What this costs is a slightly staler hop offset. Start-hopping adds its
	 * offset to whatever CLKN has reached since the reset rather than
	 * replacing it, so everything between the two is error in the followed
	 * clock, and the endpoint flush and the thread creation are now inside
	 * that window: a few of the 312.5 us ticks the hop clock counts, against
	 * timestamps that were hours out. */
	r = cmd_set_bdaddr_quiet(ut->devh, btbb_piconet_get_bdaddr(pn));
	if (r < 0)
		return follow_request_failed(ut, sock, handle, cc,
		                             "set the Bluetooth address", r);

	r = cmd_set_clock_quiet(ut->devh, 0);
	if (r < 0)
		return follow_request_failed(ut, sock, handle, cc,
		                             "zero the board's clock", r);

	// init USB transfer
	/* Each of the three steps that set the capture up exits 1, like every
	 * other failure here: the failing call's own return value used to go out
	 * as the exit status, and a shell wraps that into a byte a script cannot
	 * read. libubertooth has already said what failed in each case. */
	r = ubertooth_bulk_init(ut);
	if (r < 0) {
		/* nothing is in flight, but the board is still open and in
		 * whatever mode an earlier command left it */
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
		ubertooth_stop(ut);
		ubertooth_free(ut);
		follow_release_adapter(sock, handle, cc);
		return 1;
	}

	/* Read clocks here to ensure clock is closest to real value. Without a
	 * clock there is nothing to follow, so a failed read stops the command
	 * rather than hopping from an uninitialised one. The receive thread is
	 * already running and the link to the target is up by now, so both
	 * failures leave through follow_cleanup(). */
	if (hci_read_clock(sock, 0, 0, &clock, &accuracy, HCI_TIMEOUT_MS) < 0) {
		fprintf(stderr, "error: reading the clock of %s failed: %s\n",
		        bt_dev, strerror(errno));
		follow_cleanup(ut, sock, handle, cc);
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
			follow_cleanup(ut, sock, handle, cc);
			return 1;
		}
		clock += (btohs(offset) << 2);   // Correct offset
	}
	/* The delay is counted in the clock's own ticks here, so a large -w
	 * cannot overflow a signed multiplication on the way: the clock is a
	 * 32-bit count the board wraps anyway. */
	btbb_piconet_set_clk_offset(pn,
		clock + (uint32_t)delay * CLKN_TICKS_PER_SLOT);
	btbb_piconet_set_flag(pn, BTBB_FOLLOWING, 1);
	btbb_piconet_set_flag(pn, BTBB_CLK27_VALID, 1);

	// tell ubertooth to start hopping and send packets
	r = cmd_start_hopping(ut->devh, btbb_piconet_get_clk_offset(pn), 0);
	if (r < 0) {
		follow_cleanup(ut, sock, handle, cc);
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
		follow_cleanup(ut, sock, handle, cc);
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

	/* What the capture is missing, once the packets have scrolled
	 * past. A gap is a stretch the board had no queue room for, so
	 * the count is of gaps and not of the packets lost in them. */
	ubertooth_report_queue_gaps(ut);

	/* A follow whose stream died stopped there, so it holds less than was
	 * asked for. The exit status has to say so, or a caller cannot tell it
	 * from a Ctrl-C. The handle is read after the teardown, because
	 * cancelling the transfer can be what brings the failure out.
	 * libubertooth has already printed what failed. */
	status = ubertooth_stream_failed(ut) ? 1 : 0;
	ubertooth_free(ut);
	return status;
}
