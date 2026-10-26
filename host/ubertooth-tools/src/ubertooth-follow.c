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
#include <err.h>
#include <errno.h>
#include <string.h>

#include "ubertooth.h"
#include "ubertooth_callback.h"
#include "opt_parse.h"
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
	printf("AFH map not usable yet, reading it again\n");
	usleep(FOLLOW_AFH_RETRY_DELAY_US);
}

/* Drops the link this command made to the target, so a failure does not leave
 * the target connected until its controller times the link out. cc says
 * whether there is a link to drop. */
static void follow_disconnect(int sock, uint16_t handle, int cc)
{
	if (cc)
		hci_disconnect(sock, handle, HCI_OE_USER_ENDED_CONNECTION, 10000);
}

/* The same, for a failure once the receive thread is running: stop the thread
 * and put the board back first. */
static void follow_cleanup(ubertooth_t* ut, int sock, uint16_t handle, int cc)
{
	ubertooth_bulk_thread_stop();
	ubertooth_stop(ut);
	follow_disconnect(sock, handle, cc);
}

static void usage()
{
	printf("ubertooth-follow - active(bluez) CLK discovery and follow for a particular UAP/LAP\n");
	printf("Usage:\n");
	printf("\t-h this help\n");
	printf("\t-l <LAP> (3 bytes / 6 hex digits)\n");
	printf("\t-u <UAP> (1 byte / 2 hex digits)\n");
	fputs(OPT_USAGE_SELECT_DEVICE, stdout);
	printf("\t-r <filename> capture packets to PCAPNG file\n");
	printf("\t-q <filename> capture packets to PCAP file\n");
	printf("\t-e max_ac_errors (default: %d, range: 0-%d)\n",
	       max_ac_errors, MAX_AC_ERRORS);
	printf("\t-d filename\n");
	printf("\t-a Enable AFH (needs the target address: -l and -u)\n");
	printf("\t-b Bluetooth device (hci0)\n");
	printf("\t-w USB delay in 625us timeslots (default:5)\n");
	printf("\nLAP and UAP are both required, if not given they are read from the local device, in some cases this may give the incorrect address.\n");
//	printf("If an input file is not specified, an Ubertooth device is used for live capture.\n");
}

int main(int argc, char *argv[])
{
	int opt, sock, dev_id, lap = 0, uap = 0, delay = 5;
	int have_lap = 0;
	/* -h ends the command once the whole command line has parsed (see below). */
	int first_exit_option = 0;
	int have_uap = 0;
	int afh_enabled = 0;
	uint8_t afh_map[AFH_MAP_BYTES] = { 0 };
        char* ubertooth_device = NULL;
	char *bt_dev = "hci0";
	char addr[19] = { 0 };
	/* Files named on the command line, opened after every check passes. */
	const char* pcapng_name = NULL;
	const char* pcap_name = NULL;
	const char* dump_name = NULL;
	uint32_t clock = 0;
	uint16_t accuracy = 0, handle = 0, offset = 0;
	bdaddr_t bdaddr = { { 0 } };
	btbb_piconet *pn;
	struct hci_dev_info di;
	/* 1 once hci_create_connection() has given us a remote ACL handle. */
	int cc = 0;
	/* 1 when exactly one -l and one -u named a target to connect to. */
	int have_target = 0;

	ubertooth_t* ut = ubertooth_init();
	if (ut == NULL)
		return 1;
	pn = btbb_piconet_new();

	while ((opt=getopt(argc,argv,"hl:u:U:e:d:ab:w:r:q:")) != EOF) {
		switch(opt) {
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

	/* One -l and one -u: the same condition the address handling below uses,
	 * so the two cannot drift. */
	have_target = (have_lap == 1) && (have_uap == 1);

	/* -a reads the map from the target over an ACL link, which only exists
	 * when the target address was given. Refused here, with the rest of the
	 * command line, so no file is created and no device opened. No link has
	 * been attempted yet, so the handle is not what is in question. */
	if (afh_enabled) {
		const char* refusal = follow_afh_handle_refusal(have_target, 1);
		if (refusal != NULL) {
			fprintf(stderr, "error: %s\n", refusal);
			return 1;
		}
	}

	/* Every check has passed, so the files can be opened: a refused command
	 * line creates and truncates nothing. */
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
			perror(dump_name);
			return 1;
		}
	}

	/* A named adapter that does not exist, or one that cannot be opened,
	 * used to go unnoticed until a later HCI call worked on fd -1. */
	dev_id = hci_devid(bt_dev);
	if (dev_id < 0) {
		fprintf(stderr, "error: Bluetooth device %s not found: %s\n",
		        bt_dev, strerror(errno));
		return 1;
	}
	sock = hci_open_dev(dev_id);
	if (sock < 0) {
		fprintf(stderr, "error: cannot open Bluetooth device %s: %s\n",
		        bt_dev, strerror(errno));
		return 1;
	}

	if (!have_target) {
		printf("No address given, reading address from device\n");
		/* With a zero timeout BlueZ blocks in read() with nothing to
		 * interrupt it, so a controller that never answers hangs instead
		 * of failing. Every read here gives it a second, as the clock
		 * offset read already did. */
		if (hci_read_bd_addr(sock, &bdaddr, HCI_TIMEOUT_MS) < 0) {
			fprintf(stderr, "error: reading the address of %s failed: %s\n",
			        bt_dev, strerror(errno));
			return 1;
		}
		lap = bdaddr.b[0] | bdaddr.b[1] << 8 | bdaddr.b[2] << 16;
		btbb_init_piconet(pn, lap);
		uap = bdaddr.b[3];
		btbb_piconet_set_uap(pn, uap);
		printf("LAP=%06x UAP=%02x\n", lap, uap);
	} else {
		btbb_init_piconet(pn, lap);
		btbb_piconet_set_uap(pn, uap);
		printf("Address given, assuming address is remote\n");
		sprintf(addr, "00:00:%02X:%02X:%02X:%02X",
			uap,
			(lap >> 16) & 0xFF,
			(lap >> 8) & 0xFF,
			lap & 0xFF
		);
		str2ba(addr, &bdaddr);
		printf("Address: %s\n", addr);

		if (hci_devinfo(dev_id, &di) < 0) {
			fprintf(stderr, "error: reading the settings of %s failed: %s\n",
			        bt_dev, strerror(errno));
			return 1;
		}

		/* The handle this fills in is the only remote ACL handle the tool
		 * ever has, so cc below is what says it is usable. */
		if (hci_create_connection(sock, &bdaddr,
					htobs(di.pkt_type & ACL_PTYPE_MASK),
					0, 0x01, &handle, 25000) < 0) {
			fprintf(stderr, "error: connecting to %s failed: %s\n",
			        addr, strerror(errno));
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
			printf("%s\n", line);
			btbb_piconet_set_afh_map(pn, afh_map);
			btbb_print_afh_map(pn);
			break;
		case FOLLOW_AFH_ACTION_DISABLE:
			printf("%s\n", line);
			afh_enabled = 0;
			break;
		case FOLLOW_AFH_ACTION_FAIL:
			/* No board has been opened yet, so only the link to the
			 * target has to be dropped. */
			fprintf(stderr, "error: %s\n", line);
			follow_disconnect(sock, handle, cc);
			return 1;
		}
	} else {
		printf("Not use AFH\n");
	}

	/* Clean up on exit. */
	register_cleanup_handler(ut, 0);

	/* Fails when no board matches -U or it cannot be opened; the library
	 * has already said why. Every failure from here to the receive loop
	 * drops the link to the target on the way out; the receive thread is
	 * not running yet, so there is nothing else to undo. */
	if (ubertooth_connect_select(ut, ubertooth_device) < 0) {
		follow_disconnect(sock, handle, cc);
		return 1;
	}

	int r = ubertooth_check_api(ut);
	if (r < 0) {
		follow_disconnect(sock, handle, cc);
		return 1;
	}

	r = btbb_init(max_ac_errors);
	if (r < 0) {
		follow_disconnect(sock, handle, cc);
		return 1;
	}

	// init USB transfer
	r = ubertooth_bulk_init(ut);
	if (r < 0) {
		/* nothing is in flight, but the board is still open and in
		 * whatever mode an earlier command left it */
		ubertooth_stop(ut);
		follow_disconnect(sock, handle, cc);
		return r;
	}

	r = ubertooth_bulk_thread_start();
	if (r < 0) {
		/* The receive transfer is in flight and the board is about to be
		 * left in its mode: give both back. Not follow_cleanup(), which
		 * would join a thread that was never created. */
		ubertooth_stop(ut);
		follow_disconnect(sock, handle, cc);
		return r;
	}

	cmd_set_bdaddr(ut->devh, btbb_piconet_get_bdaddr(pn));
	cmd_set_clock(ut->devh, 0);

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
	if (cc) {
		if (hci_read_clock_offset(sock, handle, &offset,
		                         HCI_TIMEOUT_MS) < 0) {
			fprintf(stderr, "error: reading the clock offset of the "
			        "target failed: %s\n", strerror(errno));
			follow_cleanup(ut, sock, handle, cc);
			return 1;
		}
		clock += (offset<<2);   // Correct offset
	}
	btbb_piconet_set_clk_offset(pn, clock+delay);
	btbb_piconet_set_flag(pn, BTBB_FOLLOWING, 1);
	btbb_piconet_set_flag(pn, BTBB_CLK27_VALID, 1);

	// tell ubertooth to start hopping and send packets
	r = cmd_start_hopping(ut->devh, btbb_piconet_get_clk_offset(pn), 0);
	if (r < 0) {
		follow_cleanup(ut, sock, handle, cc);
		return r;
	}

	/* A map read from the remote device with -a is applied after the mode
	 * has started, not before: starting a mode forgets whatever AFH map the
	 * board was holding, so a map sent first would be thrown away. With no
	 * -a there is nothing to send, and the mode start is what leaves the
	 * board hopping the whole channel set. The set is asynchronous, so this
	 * catches a submission that fails, not a transfer that fails later. */
	if (afh_enabled && cmd_set_afh_map(ut->devh, afh_map) < 0) {
		fprintf(stderr, "error: setting the board's AFH map failed\n");
		follow_cleanup(ut, sock, handle, cc);
		return 1;
	}

    if (cc) {
		hci_disconnect(sock, handle, HCI_OE_USER_ENDED_CONNECTION, 10000);
	}

	// receive and process each packet
	while(!ut->stop_ubertooth) {
		ubertooth_bulk_receive(ut, cb_rx, pn);
	}

	ubertooth_bulk_thread_stop();

	ubertooth_stop(ut);

	/* A follow whose stream died stopped there, so it holds less than was
	 * asked for. The exit status has to say so, or a caller cannot tell it
	 * from a Ctrl-C. The handle is read after the teardown, because
	 * cancelling the transfer can be what brings the failure out.
	 * libubertooth has already printed what failed. */
	return ubertooth_stream_failed(ut) ? 1 : 0;
}
