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

#include "ubertooth.h"
#include "ubertooth_callback.h"
#include "opt_parse.h"
#include <btbb.h>
#include <getopt.h>
#include <limits.h>

static void usage()
{
	printf("ubertooth-follow - active(bluez) CLK discovery and follow for a particular UAP/LAP\n");
	printf("Usage:\n");
	printf("\t-h this help\n");
	printf("\t-l<LAP> (3 bytes / 6 hex digits)\n");
	printf("\t-u<UAP> (1 byte / 2 hex digits)\n");
	printf("\t-U<0-7> set ubertooth device to use\n");
	printf("\t-r<filename> capture packets to PCAPNG file\n");
	printf("\t-q<filename> capture packets to PCAP file\n");
	printf("\t-e max_ac_errors (default: %d, range: 0-%d)\n",
	       max_ac_errors, MAX_AC_ERRORS);
	printf("\t-d filename\n");
	printf("\t-a Enable AFH\n");
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
	uint8_t mode, afh_map[10];
        char* ubertooth_device = NULL;
	char *bt_dev = "hci0";
	char addr[19] = { 0 };
	uint32_t clock;
	uint16_t accuracy, handle, offset;
	bdaddr_t bdaddr;
	btbb_piconet *pn;
	struct hci_dev_info di;
	int cc = 0;

	pn = btbb_piconet_new();
	ubertooth_t* ut = ubertooth_init();

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
		case 'r':
			if (!ut->h_pcapng_bredr) {
				if (btbb_pcapng_create_file( optarg, "Ubertooth", &ut->h_pcapng_bredr )) {
					err(1, "create_bredr_capture_file: ");
				}
			}
			else {
				printf("Ignoring extra capture file: %s\n", optarg);
			}
			break;
		case 'q':
			if (!ut->h_pcap_bredr) {
				if (btbb_pcap_create_file(optarg, &ut->h_pcap_bredr)) {
					err(1, "btbb_pcap_create_file: ");
				}
			}
			else {
				printf("Ignoring extra capture file: %s\n", optarg);
			}
			break;
		case 'e':
			if (opt_require_int(optarg, 0, MAX_AC_ERRORS,
			                    "maximum access code errors", "",
			                    &max_ac_errors) < 0)
				return 1;
			break;
		case 'd':
			dumpfile = fopen(optarg, "w");
			if (dumpfile == NULL) {
				perror(optarg);
				return 1;
			}
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
			usage();
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
		return 1;
	}

	dev_id = hci_devid(bt_dev);
	sock = hci_open_dev(dev_id);

	if ((have_lap != 1) || (have_uap != 1)) {
		printf("No address given, reading address from device\n");
		hci_read_bd_addr(sock, &bdaddr, 0);
		lap = bdaddr.b[0] | bdaddr.b[1] << 8 | bdaddr.b[2] << 16;
		btbb_init_piconet(pn, lap);
		uap = bdaddr.b[3];
		btbb_piconet_set_uap(pn, uap);
		printf("LAP=%06x UAP=%02x\n", lap, uap);
	} else if (have_lap && have_uap) {
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
			perror("Can't get device info");
			return 1;
		}

		if (hci_create_connection(sock, &bdaddr,
					htobs(di.pkt_type & ACL_PTYPE_MASK),
					0, 0x01, &handle, 25000) < 0) {
			perror("Can't create connection");
			return 1;
		}
		cc = 1;

	} else {
			usage();
			return 1;
	}

	if (ut->h_pcapng_bredr) {
		btbb_pcapng_record_bdaddr(ut->h_pcapng_bredr,
		                            (((uint32_t)uap)<<24)|lap,
		                            0xff, 0);
	}

	//Experimental AFH map reading from remote device
	if(afh_enabled) {
		if(hci_read_afh_map(sock, handle, &mode, afh_map, 1000) < 0) {
			perror("HCI read AFH map request failed");
			//exit(1);
		}
		if(mode == 0x01) {
			btbb_piconet_set_afh_map(pn, afh_map);
			btbb_print_afh_map(pn);
		} else {
			printf("AFH disabled.\n");
			afh_enabled = 0;
		}
	} else {
		printf("Not use AFH\n");
	}

	/* Clean up on exit. */
	register_cleanup_handler(ut, 0);

	/* Fails when no board matches -U or it cannot be opened; the library
	 * has already said why. */
	if (ubertooth_connect_select(ut, ubertooth_device) < 0) {
		return 1;
	}

	int r = ubertooth_check_api(ut);
	if (r < 0)
		return 1;

	r = btbb_init(max_ac_errors);
	if (r < 0)
		return 1;

	// init USB transfer
	r = ubertooth_bulk_init(ut);
	if (r < 0)
		return r;

	r = ubertooth_bulk_thread_start();
	if (r < 0)
		return r;

	cmd_set_bdaddr(ut->devh, btbb_piconet_get_bdaddr(pn));
	cmd_set_clock(ut->devh, 0);
	if(afh_enabled)
		cmd_set_afh_map(ut->devh, afh_map);
    
    // Read clocks here to ensure clock is closest to real value
    hci_read_clock(sock, 0, 0, &clock, &accuracy, 0);
    if (cc) {
        if (hci_read_clock_offset(sock, handle, &offset, 1000) < 0) {
            perror("Reading clock offset failed\n");
        }
        clock += (offset<<2);   // Correct offset
    }
	btbb_piconet_set_clk_offset(pn, clock+delay);
	btbb_piconet_set_flag(pn, BTBB_FOLLOWING, 1);
	btbb_piconet_set_flag(pn, BTBB_CLK27_VALID, 1);

	// tell ubertooth to start hopping and send packets
	r = cmd_start_hopping(ut->devh, btbb_piconet_get_clk_offset(pn), 0);
	if (r < 0)
		return r;

    if (cc) {
		hci_disconnect(sock, handle, HCI_OE_USER_ENDED_CONNECTION, 10000);
	}

	// receive and process each packet
	while(!ut->stop_ubertooth) {
		ubertooth_bulk_receive(ut, cb_rx, pn);
	}

	ubertooth_bulk_thread_stop();

	ubertooth_stop(ut);

	return 0;
}
