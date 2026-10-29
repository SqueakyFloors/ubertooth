/*
 * Copyright 2012 - 2013 Dominic Spill
 * Extra info scan:
 * Copyright (C) 2002-2010  Marcel Holtmann <marcel@holtmann.org>
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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include "ubertooth.h"
#include "ubertooth_callback.h"
#include "opt_parse.h"
#include "request_report.h"
/* For afh_map_line(), the one line both BlueZ tools report an AFH channel map
 * on, and FOLLOW_AFH_MODE_ON, the AFH_Mode byte they both read. */
#include "follow_afh.h"
#include <btbb.h>
#include <getopt.h>
#include <limits.h>

/* follow_afh.h repeats the map length so it needs neither libusb nor BlueZ;
 * this file sizes its own map with the library's, and afh_map_line() walks the
 * repeated one, so a mismatch would read past the shorter of the two. */
#if FOLLOW_AFH_MAP_BYTES != AFH_MAP_BYTES
#error "FOLLOW_AFH_MAP_BYTES disagrees with libubertooth's AFH_MAP_BYTES"
#endif

static void usage()
{
	printf("ubertooth-scan - active(Bluez) device scan and inquiry supported by Ubertooth\n");
	printf("\n");
	printf("This tool uses a normal Bluetooth dongle to perform Inquiry Scans and\n");
	printf("Extended Inquiry scans of Bluetooth devices. It uses Ubertooth to\n");
	printf("discover undiscoverable devices and can use BlueZ to scan for\n");
	printf("discoverable devices.\n");
	printf("\n");
	printf("Usage:\n");
	printf("    ubertooth-scan\n");
	printf("        Use Ubertooth to discover devices and perform Inquiry Scan.\n");
	printf("\n");
	printf("    ubertooth-scan -s -x\n");
	printf("        Use BlueZ and Ubertooth to discover devices and perform Inquiry Scan\n");
	printf("        and Extended Inquiry Scan.\n");
	printf("\n");
	printf("Options:\n");
	printf("\t-h this help\n");
	printf("\t-V print version information\n");
	printf("\t-s hci Scan - use BlueZ to scan for discoverable devices\n");
	printf("\t-x eXtended scan - retrieve additional information about target devices\n");
	printf("\t-t scan Time (seconds) - length of time to sniff packets. [Default: 20s]\n");
	printf("\t-e max_ac_errors (default: %d, range: 0-%d)\n",
	       max_ac_errors, MAX_AC_ERRORS);
	printf("\t-b Bluetooth device (hci0)\n");
	fputs(OPT_USAGE_SELECT_DEVICE, stdout);
}

/* One "error: " line for a call that failed, on standard error, naming what
 * the tool was doing and giving the system reason only when the call left one
 * behind. Each caller clears errno first, for the reason the two adapter
 * failures in main() give: BlueZ does not promise an errno for every way its
 * calls can fail, so a leftover one from an earlier call would be reported as
 * this failure's cause, and a failure that set none would read "Undefined
 * error: 0". Those two name the adapter in their line and so write it
 * themselves, and end the command rather than carrying on. */
static void report_failure(const char* what)
{
	if (errno != 0)
		fprintf(stderr, "error: %s: %s\n", what, strerror(errno));
	else
		fprintf(stderr, "error: %s\n", what);
}


/* The extended report of one device, on standard output. Returns 1 when some
 * part of it could not be fetched and has been named on standard error, so the
 * run's exit status can say that what -x asked for did not all happen.
 *
 * whole_address says whether the address is the device's own, as a BlueZ
 * inquiry reports it, or one the scan put together from a sniffed piconet. The
 * second kind has the two most significant bytes guessed as 00:00, because no
 * packet carries them, so it reaches only a device whose address really begins
 * that way: a connection that cannot be made to such an address is the normal
 * case and not a failure of the command. */
int extra_info(int dd, int dev_id, bdaddr_t* bdaddr, int whole_address)
{
	uint16_t handle, offset;
	uint8_t features[8], max_page = 0;
	char name[249], *tmp;
	char addr[19] = { 0 };
	uint8_t mode, afh_map[AFH_MAP_BYTES];
	struct hci_version version;
	struct hci_dev_info di;
	struct hci_conn_info_req *cr;
	int i, cc = 0;
	/* 1 once some part of the report could not be fetched. */
	int failed = 0;

	/* Each failure here gives up on this device's report and leaves the rest
	 * of the scan running. They used to call exit(1) instead, which ended
	 * the run with the board still open in whatever mode it was in and the
	 * adapter handle still held: this function is called with both in
	 * hand. */
	errno = 0;
	if (hci_devinfo(dev_id, &di) < 0) {
		/* The local adapter's own settings, not the device's, so the
		 * line says which: it is the same line for every device the
		 * report is asked of. */
		report_failure("cannot get the local adapter's settings");
		return 1;
	}

	/* This says what the tool is doing, not what was asked for, so it goes
	 * to standard error with the rest of the tool's progress: the report
	 * below is what a caller collecting standard output wants. */
	fputs("Requesting information ...\n", stderr);

	errno = 0;
	cr = malloc(sizeof(*cr) + sizeof(struct hci_conn_info));
	if (!cr) {
		report_failure("cannot allocate the connection info request");
		return 1;
	}

	bacpy(&cr->bdaddr, bdaddr);
	cr->type = ACL_LINK;
	if (ioctl(dd, HCIGETCONNINFO, (unsigned long) cr) < 0) {
		errno = 0;
		if (hci_create_connection(dd, bdaddr,
					htobs(di.pkt_type & ACL_PTYPE_MASK),
					0, 0x01, &handle, 25000) < 0) {
			if (whole_address) {
				report_failure("cannot create connection");
				free(cr);
				return 1;
			}
			/* A guessed address, so the likely reason is that there
			 * was never an address to connect to: said as a note,
			 * and the run still did what was asked. The reason the
			 * call left behind is given too, because this is not
			 * the only way the connection can fail. */
			if (errno != 0)
				fprintf(stderr, "note: no extended report: %s; "
				        "a sniffed piconet gives no NAP, so the "
				        "address this connects to is a guess\n",
				        strerror(errno));
			else
				fputs("note: no extended report: a sniffed "
				      "piconet gives no NAP, so the address "
				      "this connects to is a guess\n", stderr);
			free(cr);
			return 0;
		}
		sleep(1);
		cc = 1;
	} else
		/* The kernel filled this in in host order, and every request
		 * below converts the handle it is given, so converting here
		 * would swap it twice. */
		handle = cr->conn_info->handle;

	ba2str(bdaddr, addr);
	printf("\tBD Address:  %s\n", addr);

	/* Every read below leaves its line out when it fails, and says so: a
	 * line missing from the report with nothing on standard error used to
	 * look like a device that has no name or no version. */
	errno = 0;
	if (hci_read_remote_name(dd, bdaddr, sizeof(name), name, 25000) == 0) {
		printf("\tDevice Name: %s\n", name);
	} else {
		report_failure("reading the device name failed");
		failed = 1;
	}

	errno = 0;
	if (hci_read_remote_version(dd, handle, &version, 20000) == 0) {
		char *ver = lmp_vertostr(version.lmp_ver);
		printf("\tLMP Version: %s (0x%x) LMP Subversion: 0x%x\n"
			"\tManufacturer: %s (%d)\n",
			ver ? ver : "n/a",
			version.lmp_ver,
			version.lmp_subver,
			bt_compidtostr(version.manufacturer),
			version.manufacturer);
		if (ver)
			bt_free(ver);
	} else {
		report_failure("reading the LMP version failed");
		failed = 1;
	}

	/* The result of this read was ignored outright, so a device that would
	 * not answer had the zeroed buffer printed as its features. */
	memset(features, 0, sizeof(features));
	errno = 0;
	if (hci_read_remote_features(dd, handle, features, 20000) < 0) {
		report_failure("reading the features failed");
		failed = 1;
	} else {
		if ((di.features[7] & LMP_EXT_FEAT) && (features[7] & LMP_EXT_FEAT)) {
			errno = 0;
			if (hci_read_remote_ext_features(dd, handle, 0, &max_page,
							features, 20000) < 0) {
				report_failure("reading the extended features "
				               "failed");
				failed = 1;
			}
		}

		printf("\tFeatures%s: 0x%2.2x 0x%2.2x 0x%2.2x 0x%2.2x "
					"0x%2.2x 0x%2.2x 0x%2.2x 0x%2.2x\n",
			(max_page > 0) ? " page 0" : "",
			features[0], features[1], features[2], features[3],
			features[4], features[5], features[6], features[7]);

		/* BlueZ returns NULL when it cannot build the list, which
		 * printed "(null)" as the device's features. */
		tmp = lmp_featurestostr(features, "\t\t", 63);
		if (tmp != NULL) {
			printf("%s\n", tmp);
			bt_free(tmp);
		} else {
			errno = 0;
			report_failure("listing the features failed");
			failed = 1;
		}
	}

	for (i = 1; i <= max_page; i++) {
		char what[48];

		errno = 0;
		if (hci_read_remote_ext_features(dd, handle, i, NULL,
							features, 20000) < 0) {
			snprintf(what, sizeof(what),
			         "reading features page %d failed", i);
			report_failure(what);
			failed = 1;
			continue;
		}

		printf("\tFeatures page %d: 0x%2.2x 0x%2.2x 0x%2.2x 0x%2.2x "
					"0x%2.2x 0x%2.2x 0x%2.2x 0x%2.2x\n", i,
			features[0], features[1], features[2], features[3],
			features[4], features[5], features[6], features[7]);
	}

	errno = 0;
	if (hci_read_clock_offset(dd, handle, &offset, 1000) < 0) {
		report_failure("reading clock offset failed");
		failed = 1;
	} else {
		printf("\tClock offset: 0x%4.4x\n", btohs(offset));
	}

	/* A read that failed leaves the mode byte and the map as the caller
	 * found them, so neither is looked at: the report used to go on to read
	 * an uninitialised mode and could print a map that was never read.
	 *
	 * The field name is the one hcitool info gives it, like the report's
	 * other lines; the line a survey result gets further down is the one
	 * libbtbb printed, so each keeps the spelling its readers know. */
	errno = 0;
	if (hci_read_afh_map(dd, handle, &mode, afh_map, 1000) < 0) {
		report_failure("HCI read AFH map request failed");
		failed = 1;
	} else if (mode == FOLLOW_AFH_MODE_ON) {
		printf("\tAFH Map: 0x");
		for (i = 0; i < AFH_MAP_BYTES; i++)
			printf("%02x", afh_map[i]);
		printf("\n");
	} else if (mode == FOLLOW_AFH_MODE_OFF) {
		/* Not part of the device's report but a note about it: there is
		 * no map to report. */
		fputs("note: AFH disabled\n", stderr);
	} else {
		/* Neither off nor on, so what the device is doing is unknown:
		 * reported as itself rather than as AFH being off, which is
		 * what it used to be taken for. */
		fprintf(stderr, "error: the remote device reported AFH mode "
		        "0x%02x, which is neither off (0x%02x) nor on "
		        "(0x%02x)\n", mode, FOLLOW_AFH_MODE_OFF,
		        FOLLOW_AFH_MODE_ON);
		failed = 1;
	}
	free(cr);

	if (cc) {
		usleep(10000);
		hci_disconnect(dd, handle, HCI_OE_USER_ENDED_CONNECTION, 10000);
	}
	return failed;
}

/* For a given BD_ADDR, print address, name and class. Returns what
 * extra_info() returned, or 0 without -x. whole_address is as it is there. */
int print_name_and_class(int dev_handle, int dev_id, bdaddr_t *bdaddr,
						  char* printable_addr, uint8_t extended,
						  int whole_address)
{
	char name[248] = { 0 };

	if (hci_read_remote_name(dev_handle, bdaddr, sizeof(name), name, 0) < 0)
			strcpy(name, "[unknown]");

	printf("%s\t%s\n", printable_addr, name);
	if (extended)
		return extra_info(dev_handle, dev_id, bdaddr, whole_address);
	return 0;
}


int main(int argc, char *argv[])
{
	inquiry_info *ii = NULL;
	int r, i, rv, opt, dev_id, dev_handle, len, flags;
	int max_rsp, num_rsp, lap, timeout = 20;
	uint8_t uap, extended = 0;
	uint8_t scan = 0;
	/* 1 once something the command asked for did not happen: the Ubertooth
	 * scan's USB stream died part-way, the inquiry of -s failed, or part of
	 * the report of -x could not be fetched. The results are still printed,
	 * and the status says they are incomplete. */
	int exit_status = 0;
	/* -h ends the command once the whole command line has parsed (see below),
	 * and so does -V, whose own result is version_status: it reads the board,
	 * so it can fail where -h cannot. */
	int first_exit_option = 0;
	int do_version = 0, version_status = 0;
	opt_once once = {0};
	char* ubertooth_device = NULL;
	char *bt_dev = "hci0";
	char addr[19] = { 0 };
	char map_line[AFH_MAP_LINE_LEN];
	ubertooth_t* ut = NULL;
	btbb_piconet* pn;
	bdaddr_t bdaddr;

	/* Report a bad option ourselves rather than letting getopt print it:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	/* Named, so the loop below and opt_report_bad_option()
	 * cannot be given different letters. */
	const char options[] = ":hVU:t:e:xsb:";

	/* Unbuffered, as ubertooth-rx's survey mode is, and before anything is
	 * written: redirected to a file, standard output is block-buffered
	 * while standard error is not, so a "> log 2>&1" run would collect
	 * every line this tool moved to standard error above the results they
	 * belong beside. Before the command line is read, because a run must
	 * not set this after writing to the stream. */
	setvbuf(stdout, NULL, _IONBF, 0);

	opterr = 0;
	while ((opt=getopt(argc, argv, options)) != EOF) {
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
		switch(opt) {
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'b':
			bt_dev = optarg;
			break;
		case 't':
			if (opt_require_count(optarg, 0, "timeout", "seconds",
			                      "0 for none", &timeout) < 0)
				return 1;
			break;
		case 'e':
			if (opt_require_int(optarg, 0, MAX_AC_ERRORS,
			                    "maximum access code errors", "",
			                    &max_ac_errors) < 0)
				return 1;
			break;
		case 'x':
			extended = 1;
			break;
		case 's':
			scan = 1;
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

	/* ubertooth-scan takes no operands: refuse a leftover word before any
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
		usage();
	if (do_version || first_exit_option == 'h')
		return version_status;

	/* Both adapter failures are reported the way ubertooth-follow, the other
	 * BlueZ tool, reports them: one "error: " line on standard error, saying
	 * which adapter and why. The first used to go to standard output, where a
	 * caller redirecting the scan results to a file collected it with them and
	 * saw nothing on the terminal; the second was a perror() line with no
	 * "error: " prefix.
	 *
	 * Each call clears errno first and gives a reason only if there is one:
	 * BlueZ does not promise an errno for every way these can fail, so a
	 * leftover one from an earlier call would be reported as this failure's
	 * cause, and a failure that set none would read "Undefined error: 0". */
	errno = 0;
	dev_id = hci_devid(bt_dev);
	if (dev_id < 0) {
		if (errno != 0)
			fprintf(stderr, "error: Bluetooth device %s not found: %s\n",
			        bt_dev, strerror(errno));
		else
			fprintf(stderr, "error: Bluetooth device %s not found\n",
			        bt_dev);
		return 1;
	}

	errno = 0;
	dev_handle = hci_open_dev( dev_id );
	if (dev_handle < 0) {
		if (errno != 0)
			fprintf(stderr, "error: cannot open Bluetooth device %s: %s\n",
			        bt_dev, strerror(errno));
		else
			fprintf(stderr, "error: cannot open Bluetooth device %s\n",
			        bt_dev);
		return 1;
	}

	/* Stop on Ctrl-C from here on, before the board is opened: a signal
	 * that arrives while one is being found would otherwise be discarded. */
	ubertooth_catch_stop_signals();

	/* Every way out from here gives the adapter handle back, as the end of
	 * a successful scan does: opening the board, checking its firmware, and
	 * the capture requests further down. */
	ut = ubertooth_start_select(ubertooth_device);
	if (ut == NULL) {
		close(dev_handle);
		return ubertooth_open_stopped() ? 0 : 1;
	}
	rv = ubertooth_check_api(ut);
	if (rv < 0) {
		/* ubertooth_check_api() releases the board itself when it
		 * refuses the firmware, and leaves it open when it could not
		 * read it. Closing covers both, because closing a board already
		 * released does nothing, and it sends no STOP to a firmware
		 * this host will not drive. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		close(dev_handle);
		return 1;
	}

	/* Stopped while the board was being opened: the run ends here, with the
	 * board and the adapter given back, rather than running an inquiry the
	 * user has already asked to stop. */
	if (ubertooth_stop_requested()) {
		ubertooth_close(ut);
		ubertooth_free(ut);
		close(dev_handle);
		return 0;
	}

	if (scan) {
		/* Equivalent to "hcitool scan". The heading says which half of
		 * the scan is running, so it goes to standard error with the
		 * rest of the tool's progress: standard output carries the
		 * devices found and nothing else. */
		fputs("HCI scan\n", stderr);
		len  = 8;
		max_rsp = 255;
		flags = IREQ_CACHE_FLUSH;
		ii = (inquiry_info*)malloc(max_rsp * sizeof(inquiry_info));
		if (ii == NULL) {
			/* The inquiry would have been handed a null array to
			 * fill in. The board is open by now, so it and the
			 * adapter go back. */
				errno = 0;
			report_failure("cannot allocate room for the inquiry's "
			               "results");
			ubertooth_stop(ut);
			ubertooth_free(ut);
			close(dev_handle);
			return 1;
		}

		errno = 0;
		num_rsp = hci_inquiry(dev_id, len, max_rsp, NULL, &ii, flags);
		/* An inquiry that failed is half of what -s asked for, so the
		 * status says so; the Ubertooth half still runs, since it needs
		 * no adapter. The line used to be the bare name of the BlueZ
		 * function, with no prefix, and the run exited 0 as though the
		 * inquiry had found nothing.
		 *
		 * A stop is not that failure: the user asked for it, so the
		 * inquiry it cut short is not reported as the adapter's fault,
		 * and the run ends below. */
		if (num_rsp < 0 && !ubertooth_stop_requested()) {
			report_failure("the BlueZ inquiry failed");
			exit_status = 1;
		}

		for (i = 0; i < num_rsp; i++) {
			ba2str(&(ii+i)->bdaddr, addr);
			/* The inquiry reports each device's own whole address. */
			if (print_name_and_class(dev_handle, dev_id, &(ii+i)->bdaddr,
			                         addr, extended, 1) != 0)
				exit_status = 1;
		}
		free(ii);
	}

	/* Stopped during the inquiry above, which takes up to ten seconds and
	 * which BlueZ ends on a signal: the run gives the board and the adapter
	 * back rather than starting a capture the user has asked to stop. What
	 * the inquiry did find has already been printed. A stop is not a
	 * failure, so the status is 0 unless the inquiry itself failed. */
	if (ubertooth_stop_requested()) {
		ubertooth_close(ut);
		ubertooth_free(ut);
		close(dev_handle);
		return exit_status;
	}

	/* libbtbb refuses only a -e out of its own range, which the check on -e
	 * above has already caught, and it says so itself. The result was ignored
	 * outright before, which would have left the scan decoding with a
	 * syndrome map it never built. */
	if (btbb_init(max_ac_errors) < 0) {
		ubertooth_stop(ut);
		ubertooth_free(ut);
		close(dev_handle);
		return 1;
	}
	btbb_init_survey();

	if (timeout)
		ubertooth_set_timeout(ut, timeout);

	/* Each of the three steps below exits 1, as every failure here does. The
	 * failing call's own return value used to go out as the exit status, and a
	 * shell wraps that into a byte (-9 read as 247, -7 as 249), so a script
	 * could not tell a scan that failed from anything else. */

	// init USB transfer
	r = ubertooth_bulk_init(ut);
	if (r < 0) {
		/* nothing is in flight, but the board is still open and in
		 * whatever mode an earlier command left it. libubertooth has
		 * already said what failed, so the status is all this adds. */
		ubertooth_stop(ut);
		ubertooth_free(ut);
		close(dev_handle);
		return 1;
	}

	r = ubertooth_bulk_thread_start();
	if (r != 0) {
		/* Not "< 0": this is pthread_create()'s errno, which is positive,
		 * so a failure used to slip through and leave the capture with
		 * nobody to complete its transfers -- it then waited forever with
		 * nothing arriving. */
		report_capture_thread_failure(r);
		/* The receive transfer is in flight and the board is about to be
		 * left in its mode: give both back before returning. */
		ubertooth_stop(ut);
		ubertooth_free(ut);
		close(dev_handle);
		return 1;
	}

	// tell ubertooth to send packets
	/* The quiet form, so this failure reads as the one "error: " line
	 * report_refused_request() prints, like the sweep request below. The
	 * reporting form would hand it to libubertooth's shared sink instead,
	 * which does not name the request that failed. */
	r = cmd_rx_syms_quiet(ut->devh);
	if (r < 0) {
		report_refused_request("start the capture", r);
		/* The poll thread is running by now and nothing else joins it:
		 * ubertooth_stop() frees the libusb context it polls. */
		ubertooth_bulk_thread_stop();
		ubertooth_stop(ut);
		ubertooth_free(ut);
		close(dev_handle);
		return 1;
	}

	/* Sweep the whole band, which any channel above the Bluetooth band asks
	 * the firmware for. This goes after the capture request, not before:
	 * starting a mode puts the board's hop mode back to "no hopping", so a
	 * sweep stated first is thrown away. The scan then sat on one channel
	 * and printed an AFH map that means nothing, since the map only says
	 * which channels a piconet uses if the board listened to all of them.
	 *
	 * The board keeps what arrives after the request for the mode it is
	 * starting, whatever it was doing before. A board left in a mode that
	 * captures nothing -- a killed ubertooth-specan say -- used to lose
	 * this request, because it runs the reset a moment after this one has
	 * been answered, on the way out of that mode.
	 *
	 * A board that refused this would print such a map, so the scan ends
	 * here. A BlueZ inquiry asked for with -s has run by then and has
	 * printed what it found: it needs no Ubertooth, and what it reported is
	 * what it saw. */
	rv = cmd_set_channel_quiet(ut->devh, 9999);
	if (rv < 0) {
		/* The capture is running, so the mode is stopped and the board
		 * given back rather than left streaming on one channel. A board
		 * that has stopped answering can make that STOP print a line of
		 * its own, which the man page says; a mode left running is
		 * worse. A refused request is not proof that the board never
		 * got it -- a transfer can time out after it arrived -- so the
		 * message says only what the tool knows. */
		report_refused_request("set sweep mode", rv);
		ubertooth_bulk_thread_stop();
		ubertooth_stop(ut);
		ubertooth_free(ut);
		close(dev_handle);
		return 1;
	}

	/* Now find hidden piconets with Ubertooth. This heads the results below,
	 * so it waits until the board is sweeping: printed before the requests
	 * that start the capture, a run that then refused one of them announced
	 * a scan it never made and printed no results at all. */
	fputs("\nUbertooth scan\n", stderr);

	// receive and process each packet
	while(!ubertooth_stopping(ut)) {
		ubertooth_bulk_receive(ut, cb_scan, NULL);
	}

	ubertooth_bulk_thread_stop();

	ubertooth_stop(ut);

	/* What the capture is missing, once the packets have scrolled
	 * past. A gap is a stretch the board had no queue room for, so
	 * the count is of gaps and not of the packets lost in them. */
	ubertooth_report_capture_losses(ut);

	/* A scan whose stream died stopped there, so the results below are
	 * only what arrived before that. They are still printed, and the exit
	 * status says they are incomplete. The handle is read after the
	 * teardown, because cancelling the transfer can be what brings the
	 * failure out. libubertooth has already printed what failed.
	 *
	 * A teardown the board refused counts the same way: that board is left
	 * running the mode this scan started, and no other tool can open it
	 * until it is reset or re-plugged. */
	if (ubertooth_stream_failed(ut) || ubertooth_stop_failed(ut))
		exit_status = 1;
	ubertooth_free(ut);

	/* The heading of the results, not one of them: on standard error with
	 * the two above it, so what a caller collects on standard output is the
	 * devices and their reports alone. */
	fputs("\nScan results:\n", stderr);
	while((pn=btbb_next_survey_result()) != NULL) {
		lap = btbb_piconet_get_lap(pn);
		if (btbb_piconet_get_flag(pn, BTBB_UAP_VALID)) {
			uap = btbb_piconet_get_uap(pn);
			sprintf(addr, "00:00:%02X:%02X:%02X:%02X", uap,
			        (lap >> 16) & 0xFF, (lap >> 8) & 0xFF, lap & 0xFF);
			str2ba(addr, &bdaddr);
			/* Printable version showing that the NAP is unknown */
			sprintf(addr, "??:??:%02X:%02X:%02X:%02X", uap,
			        (lap >> 16) & 0xFF, (lap >> 8) & 0xFF, lap & 0xFF);
			/* The NAP of a sniffed piconet is not in any packet, so
			 * the address this connects to is a guess: see
			 * extra_info(). */
			if (print_name_and_class(dev_handle, dev_id, &bdaddr, addr,
			                         extended, 0) != 0)
				exit_status = 1;
		} else
			printf("??:??:??:%02X:%02X:%02X\n", (lap >> 16) & 0xFF,
			       (lap >> 8) & 0xFF, lap & 0xFF);
		/* The map the scan built for this piconet is part of its
		 * result, so the tool prints it -- in the same words as
		 * libbtbb's btbb_print_afh_map(), which wrote it from inside
		 * the library: standard output belongs to the tool. */
		afh_map_line(btbb_piconet_get_afh_map(pn), map_line,
		             sizeof(map_line));
		printf("%s\n", map_line);
	}

	close(dev_handle);
	return exit_status;
}
