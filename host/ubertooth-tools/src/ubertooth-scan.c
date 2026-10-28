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
#include <btbb.h>
#include <getopt.h>
#include <limits.h>

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
	printf("\t-s hci Scan - use BlueZ to scan for discoverable devices\n");
	printf("\t-x eXtended scan - retrieve additional information about target devices\n");
	printf("\t-t scan Time (seconds) - length of time to sniff packets. [Default: 20s]\n");
	printf("\t-e max_ac_errors (default: %d, range: 0-%d)\n",
	       max_ac_errors, MAX_AC_ERRORS);
	printf("\t-b Bluetooth device (hci0)\n");
	fputs(OPT_USAGE_SELECT_DEVICE, stdout);
}


void extra_info(int dd, int dev_id, bdaddr_t* bdaddr)
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

	if (hci_devinfo(dev_id, &di) < 0) {
		perror("Can't get device info");
		exit(1);
	}

	printf("Requesting information ...\n");

	cr = malloc(sizeof(*cr) + sizeof(struct hci_conn_info));
	if (!cr) {
		perror("Can't get connection info");
		exit(1);
	}

	bacpy(&cr->bdaddr, bdaddr);
	cr->type = ACL_LINK;
	if (ioctl(dd, HCIGETCONNINFO, (unsigned long) cr) < 0) {
		if (hci_create_connection(dd, bdaddr,
					htobs(di.pkt_type & ACL_PTYPE_MASK),
					0, 0x01, &handle, 25000) < 0) {
			perror("Can't create connection");
			free(cr);
			return;
		}
		sleep(1);
		cc = 1;
	} else
		handle = htobs(cr->conn_info->handle);

	ba2str(bdaddr, addr);
	printf("\tBD Address:  %s\n", addr);

	if (hci_read_remote_name(dd, bdaddr, sizeof(name), name, 25000) == 0)
		printf("\tDevice Name: %s\n", name);

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
	}

	memset(features, 0, sizeof(features));
	hci_read_remote_features(dd, handle, features, 20000);

	if ((di.features[7] & LMP_EXT_FEAT) && (features[7] & LMP_EXT_FEAT))
		hci_read_remote_ext_features(dd, handle, 0, &max_page,
							features, 20000);

	printf("\tFeatures%s: 0x%2.2x 0x%2.2x 0x%2.2x 0x%2.2x "
				"0x%2.2x 0x%2.2x 0x%2.2x 0x%2.2x\n",
		(max_page > 0) ? " page 0" : "",
		features[0], features[1], features[2], features[3],
		features[4], features[5], features[6], features[7]);

	tmp = lmp_featurestostr(features, "\t\t", 63);
	printf("%s\n", tmp);
	bt_free(tmp);

	for (i = 1; i <= max_page; i++) {
		if (hci_read_remote_ext_features(dd, handle, i, NULL,
							features, 20000) < 0)
			continue;

		printf("\tFeatures page %d: 0x%2.2x 0x%2.2x 0x%2.2x 0x%2.2x "
					"0x%2.2x 0x%2.2x 0x%2.2x 0x%2.2x\n", i,
			features[0], features[1], features[2], features[3],
			features[4], features[5], features[6], features[7]);
	}

	if (hci_read_clock_offset(dd, handle, &offset, 1000) < 0) {
		perror("Reading clock offset failed");
		exit(1);
	}

	printf("\tClock offset: 0x%4.4x\n", btohs(offset));

	if(hci_read_afh_map(dd, handle, &mode, afh_map, 1000) < 0) {
	perror("HCI read AFH map request failed");
	}
	if(mode == 0x01) {
		// DGS: Replace with call to btbb_print_afh_map - need a piconet
		printf("\tAFH Map: 0x");
		for(i=0; i<10; i++)
			printf("%02x", afh_map[i]);
		printf("\n");
	} else {
		printf("AFH disabled.\n");
	}
	free(cr);

	if (cc) {
		usleep(10000);
		hci_disconnect(dd, handle, HCI_OE_USER_ENDED_CONNECTION, 10000);
	}
}

/* For a given BD_ADDR, print address, name and class */
void print_name_and_class(int dev_handle, int dev_id, bdaddr_t *bdaddr,
						  char* printable_addr, uint8_t extended)
{
	char name[248] = { 0 };

	if (hci_read_remote_name(dev_handle, bdaddr, sizeof(name), name, 0) < 0)
			strcpy(name, "[unknown]");

	printf("%s\t%s\n", printable_addr, name);
	if (extended)
		extra_info(dev_handle, dev_id, bdaddr);
}


int main(int argc, char *argv[])
{
	inquiry_info *ii = NULL;
	int r, i, rv, opt, dev_id, dev_handle, len, flags;
	int max_rsp, num_rsp, lap, timeout = 20;
	uint8_t uap, extended = 0;
	uint8_t scan = 0;
	/* 1 once the Ubertooth scan's USB stream died part-way, so the exit
	 * status says the results below are incomplete. */
	int exit_status = 0;
	/* -h ends the command once the whole command line has parsed (see below). */
	int first_exit_option = 0;
	opt_once once = {0};
	char* ubertooth_device = NULL;
	char *bt_dev = "hci0";
	char addr[19] = { 0 };
	ubertooth_t* ut = NULL;
	btbb_piconet* pn;
	bdaddr_t bdaddr;

	/* Report a bad option ourselves rather than letting getopt print it:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":hU:t:e:xsb:")) != EOF) {
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
		switch(opt) {
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'b':
			bt_dev = optarg;
			if (bt_dev == NULL) {
				perror(optarg);
				return 1;
			}
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
		case 'h':
			if (!first_exit_option)
				first_exit_option = 'h';
			break;
		default:
			opt_report_bad_option(opt, argc, argv);
			return 1;
		}
	}

	/* ubertooth-scan takes no operands: refuse a leftover word before any
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

	/* Every way out from here gives the adapter handle back, as the end of
	 * a successful scan does: opening the board, checking its firmware, and
	 * the capture requests further down. */
	ut = ubertooth_start_select(ubertooth_device);
	if (ut == NULL) {
		close(dev_handle);
		return 1;
	}
	rv = ubertooth_check_api(ut);
	if (rv < 0) {
		/* A refused version was released inside ubertooth_check_api();
		 * one it could not read at all leaves the board open. Closing
		 * covers both, because closing a board already released does
		 * nothing, and it sends no STOP to a firmware this host will
		 * not drive. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		close(dev_handle);
		return 1;
	}

	/* Clean up on exit. */
	register_cleanup_handler(ut, 0);

	if (scan) {
		/* Equivalent to "hcitool scan" */
		printf("HCI scan\n");
		len  = 8;
		max_rsp = 255;
		flags = IREQ_CACHE_FLUSH;
		ii = (inquiry_info*)malloc(max_rsp * sizeof(inquiry_info));

		num_rsp = hci_inquiry(dev_id, len, max_rsp, NULL, &ii, flags);
		if( num_rsp < 0 )
			perror("hci_inquiry");

		for (i = 0; i < num_rsp; i++) {
			ba2str(&(ii+i)->bdaddr, addr);
			print_name_and_class(dev_handle, dev_id, &(ii+i)->bdaddr, addr,
			                     extended);
		}
		free(ii);
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
	 * reporting form would print libubertooth's "libUSB Error" line instead,
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
	 * A board already capturing symbols -- what a killed ubertooth-afh or
	 * ubertooth-follow leaves behind -- does that reset while it takes the
	 * request above, so a sweep stated here survives it. A board left in a
	 * mode that captures nothing, a spectrum scan say, resets a moment
	 * later instead, as it leaves that mode, and can still swallow this
	 * request; the board has to say when its mode has started before the
	 * host can close that gap.
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
	 * that start the capture, it was left on standard output by a run that
	 * then refused one of them and printed no results at all. */
	printf("\nUbertooth scan\n");

	// receive and process each packet
	while(!ubertooth_stopping(ut)) {
		ubertooth_bulk_receive(ut, cb_scan, NULL);
	}

	ubertooth_bulk_thread_stop();

	ubertooth_stop(ut);

	/* A scan whose stream died stopped there, so the results below are
	 * only what arrived before that. They are still printed, and the exit
	 * status says they are incomplete. The handle is read after the
	 * teardown, because cancelling the transfer can be what brings the
	 * failure out. libubertooth has already printed what failed. */
	if (ubertooth_stream_failed(ut))
		exit_status = 1;
	ubertooth_free(ut);

	printf("\nScan results:\n");
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
			print_name_and_class(dev_handle, dev_id, &bdaddr, addr, extended);
		} else
			printf("??:??:??:%02X:%02X:%02X\n", (lap >> 16) & 0xFF,
			       (lap >> 8) & 0xFF, lap & 0xFF);
		btbb_print_afh_map(pn);
	}

	close(dev_handle);
	return exit_status;
}
