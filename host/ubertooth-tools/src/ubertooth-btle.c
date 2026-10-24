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
#include <err.h>
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

int cancel_follow = 0;

// send SIGUSR1 to cancel following an active connection
static void cancel_follow_handler(int sig __attribute__((unused))) {
	cancel_follow = 1;
}

int running = 1;

static void quit(int sig __attribute__((unused))) {
	running = 0;
}

void print_mac(uint8_t *mac_address, int mask) {
	unsigned i;
	for (i = 0; i < 5; ++i)
		printf("%02x:", mac_address[i]);
	printf("%02x", mac_address[5]);
	if (mask >= 0)
		printf("/%u", mask);
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

	if (isatty(STDOUT_FILENO)) {
		fprintf(stderr, "Error: stdout is a tty, refusing binary capture\n");
		return NULL;
	}

	fflush(stdout);
	*saved_fd = dup(STDOUT_FILENO);
	if (*saved_fd < 0) {
		perror("dup(stdout)");
		return NULL;
	}

	null_fd = open("/dev/null", O_WRONLY);
	if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0) {
		perror("redirect stdout");
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
			fprintf(stderr, "Error: no writable descriptor filesystem for stdout capture\n");
			close(*saved_fd);
			*saved_fd = -1;
			return NULL;
		}
	}

	return strdup(path);
}

static void usage(void)
{
	printf("ubertooth-btle - passive Bluetooth Low Energy monitoring\n");
	printf("Usage:\n");
	printf("\t-h this help\n");
	printf("\n");
	printf("    Major modes:\n");
	printf("\t-f follow connections\n");
	printf("\t-n don't follow, only print advertisements\n");
	printf("\t-p promiscuous: sniff active connections\n");
	printf("\n");
	printf("\t-a[address] get/set access address, 1-8 hex digits (example: -a8e89bed6 or -a 8e89bed6)\n");
	printf("\t-s<address> faux slave mode, using MAC addr (example: -s22:44:66:88:aa:cc)\n");
	printf("\t-t<address> set connection following target (example: -t22:44:66:88:aa:cc/48)\n");
	printf("\t-tnone unset connection following target\n");
	printf("\n");
	printf("    Interference (use with -f or -p):\n");
	printf("\t-i interfere with one connection and return to idle\n");
	printf("\t-I interfere continuously\n");
	printf("\n");
	printf("    Data source:\n");
	fputs(OPT_USAGE_SELECT_DEVICE, stdout);
	printf("\n");
	printf("    Misc:\n");
	printf("\t-r<filename> capture packets to PCAPNG file\n");
	printf("\t-q<filename> capture packets to PCAP file (DLT_BLUETOOTH_LE_LL_WITH_PHDR)\n");
	printf("\t-c<filename> capture packets to PCAP file (DLT_PPI + DLT_BLUETOOTH_LE_LL)\n");
	printf("\t-A<%d-%d> advertising channel index (default %d)\n",
	       ADV_INDEX_MIN, ADV_INDEX_MAX, ADV_INDEX_MIN);
	printf("\t-v[01] verify CRC mode, get status (-v) or enable/disable (-v1, -v 0)\n");
	printf("\t-x<0-%d> allow n access address offenses (default %d)\n",
	       AA_ERRORS_MAX, AA_ERRORS_MAX);

	printf("\nIf an input file is not specified, an Ubertooth device is used for live capture.\n");
	printf("In get/set mode no capture occurs.\n");
}

int main(int argc, char *argv[])
{
	int opt;
	/* -h ends the command once the whole command line has parsed (see below). */
	int first_exit_option = 0;
	int do_follow, do_no_follow, do_promisc;
	int do_get_aa, do_set_aa;
	int do_crc;
	int do_adv_index;
	int do_slave_mode;
	int do_target;
	enum jam_modes jam_mode = JAM_NONE;
	char* ubertooth_device = NULL;
	/* Capture files named on the command line, opened after every check. */
	const char* pcapng_name = NULL;
	const char* pcap_name = NULL;
	int pcap_ppi = 0; /* the -c format rather than the -q one */
	ubertooth_t* ut = ubertooth_init();

	btle_options cb_opts = { .allowed_access_address_errors = 32 };

	int r;
	u32 access_address = 0;
	uint8_t mac_address[6] = { 0, };
	uint8_t mac_mask = 0;

	do_follow = do_no_follow = do_promisc = 0;
	do_get_aa = do_set_aa = 0;
	do_crc = -1; // 0 and 1 mean set, 2 means get
	do_adv_index = 37;
	do_slave_mode = do_target = 0;

	while ((opt=getopt(argc,argv,"a::r:hfnpU:v::A:s:t:x:c:q:jJiI")) != EOF) {
		switch(opt) {
		case 'a':
			/* getopt only attaches an optional value (-a8e89bed6); take
			 * a following word that is not an option as the value too,
			 * so "-a 8e89bed6" sets and "-a zzzz" is refused rather than
			 * silently reading. ubertooth-btle takes no other operands. */
			if (optarg == NULL)
				optarg = opt_separate_value(argc, argv, opt_value_not_option);
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
				fprintf(stderr, "Error: PCAPNG capture to stdout is not supported; use -q - or -c -\n");
				return 1;
			}
			if (pcapng_name == NULL)
				pcapng_name = optarg;
			else
				fprintf(stderr, "Ignoring extra capture file: %s\n", optarg);
			break;
		case 'q':
		case 'c':
			if (pcap_name == NULL) {
				pcap_name = optarg;
				pcap_ppi = (opt == 'c');
			} else {
				fprintf(stderr, "Ignoring extra capture file: %s\n", optarg);
			}
			break;
		case 'v':
			/* -v reads the CRC verify setting; -v0 / -v1 sets it, written
			 * attached or as the next word. */
			if (optarg == NULL)
				optarg = opt_separate_value(argc, argv, opt_value_decimal);
			if (optarg) {
				if (opt_require_int(optarg, 0, 1, "CRC verify mode", "",
				                    &do_crc) < 0)
					return 1;
			} else
				do_crc = 2; // get
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
			if (!r) {
				usage();
				return 1;
			}
			break;
		case 't':
			do_target = 1;
			r = convert_mac_address(optarg, mac_address, &mac_mask);
			if (!r) {
				usage();
				return 1;
			}
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
			break;
		case 'I':
		case 'J':
			jam_mode = JAM_CONTINUOUS;
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


	/* ubertooth-btle takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. "-"
	 * is only a value of -q/-c/-r, which getopt has already taken. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h ends the command, but only once the whole command line has
	 * parsed: a bad option value or a stray word is reported even
	 * alongside it. */
	if (first_exit_option == 'h') {
		usage();
		return 1;
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

	/* Every check has passed, so the capture files can be opened: a refused
	 * command line creates and truncates nothing. A bare "-" redirects this
	 * process's stdout, which is why nothing may be printed to stdout after
	 * this point except the capture itself. */
	if (pcapng_name != NULL) {
		if (lell_pcapng_create_file(pcapng_name, "Ubertooth", &ut->h_pcapng_le))
			err(1, "lell_pcapng_create_file: ");
	}
	if (pcap_name != NULL) {
		int saved_fd;
		char *file = handle_special_filename(pcap_name, &saved_fd);
		int create_result;

		if (!file) {
			if (saved_fd >= 0)
				close(saved_fd);
			return 1;
		}
		create_result = pcap_ppi
		        ? lell_pcap_ppi_create_file(file, 0, &ut->h_pcap_le)
		        : lell_pcap_create_file(file, &ut->h_pcap_le);
		if (saved_fd >= 0)
			close(saved_fd);
		free(file);
		if (create_result)
			err(1, pcap_ppi ? "lell_pcap_ppi_create_file: "
			                : "lell_pcap_create_file: ");
	}

	r = ubertooth_connect_select(ut, ubertooth_device);
	if (r < 0) {
		return 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0)
		return 1;

	// quit on ctrl-C
	signal(SIGINT, quit);
	signal(SIGQUIT, quit);
	signal(SIGTERM, quit);

	// cancel following on USR1
	signal(SIGUSR1, cancel_follow_handler);

	// allow user to set target before following
	if (do_target) {
		r = cmd_btle_set_target(ut->devh, mac_address, mac_mask);
		if (r == 0) {
			if (mac_mask == 0) {
				printf("target cleared\n");
			} else {
				printf("target set to: ");
				print_mac(mac_address, mac_mask);
				printf("\n");
			}
		} else {
			printf("Unable to set target\n");
			return 1;
		}
	}

	if (do_follow || do_no_follow || do_promisc) {
		usb_pkt_rx rx;

		r = cmd_set_jam_mode(ut->devh, jam_mode);
		if (jam_mode != JAM_NONE && r != 0) {
			printf("Jamming not supported\n");
			return 1;
		}
		cmd_set_modulation(ut->devh, MOD_BT_LOW_ENERGY);

		if (do_follow || do_no_follow) {
			u16 channel;
			if (do_adv_index == 37)
				channel = 2402;
			else if (do_adv_index == 38)
				channel = 2426;
			else
				channel = 2480;
			cmd_set_channel(ut->devh, channel);
			cmd_btle_sniffing(ut->devh, do_follow);
			/* This mode runs the low-energy physical-layer engine,
			 * which stamps packets from timer1, so its clock starts
			 * over at 2^32 ticks rather than the CLKN-derived
			 * 3125 * 2^20. */
			ut->clk100ns_period = CLK100NS_TIMER1_PERIOD;
		} else {
			cmd_btle_promisc(ut->devh);
		}

		// Set CRC verification *after* the mode has started (mode entry runs
		// reset_le() which clears crc_verify) but *before* the capture loop —
		// otherwise the do_crc block further below only ran after streaming
		// ended, making -v1 a no-op for sniffing modes.
		if (do_crc >= 0 && do_crc != 2) {
			cmd_set_crc_verify(ut->devh, do_crc);
			printf("CRC: %sverify\n", do_crc ? "" : "DO NOT ");
		}

		// running can be changed by signal handler
		while (running) {
			if (cancel_follow) {
				cmd_cancel_follow(ut->devh);
				cancel_follow = 0;
			}
			int r = cmd_poll(ut->devh, &rx);
			if (r < 0) {
				printf("USB error\n");
				break;
			}
			if (r == sizeof(usb_pkt_rx)) {
				fifo_push(ut->fifo, &rx);
				cb_btle(ut, &cb_opts);
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
			return 1;
		}
		printf("Access address: %08x\n", access_address);
		/* carry on, so a -v in the same command still runs */
	}

	if (do_set_aa) {
		if (cmd_set_access_address(ut->devh, access_address) < 0) {
			fprintf(stderr, "error: access address set failed\n");
			return 1;
		}
		printf("access address set to: %08x\n", access_address);
	}

	// For sniffing modes this was already handled before the capture loop.
	if (do_crc >= 0 && !(do_follow || do_no_follow || do_promisc)) {
		int r;
		if (do_crc == 2) {
			r = cmd_get_crc_verify(ut->devh);
			if (r < 0) {
				/* a short reply's whole error line is already printed */
				if (r != UBERTOOTH_ERROR_SHORT_REPLY)
					fprintf(stderr, "error: CRC verify read failed\n");
				return 1;
			}
		} else {
			cmd_set_crc_verify(ut->devh, do_crc);
			r = do_crc;
		}
		printf("CRC: %sverify\n", r ? "" : "DO NOT ");
	}

	if (do_slave_mode) {
		u16 channel;
		if (do_adv_index == 37)
			channel = 2402;
		else if (do_adv_index == 38)
			channel = 2426;
		else
			channel = 2480;
		cmd_set_channel(ut->devh, channel);

		// flags: LE Limited Discovery
		uint8_t adv_data[] = { 0x02, 0x01, 0x05 };
		cmd_le_set_adv_data(ut->devh, adv_data, sizeof(adv_data));

		cmd_btle_slave(ut->devh, mac_address);
	}

	if (!(do_follow || do_no_follow || do_promisc || do_get_aa || do_set_aa ||
				do_crc >= 0 || do_slave_mode || do_target))
		usage();

	return 0;
}
