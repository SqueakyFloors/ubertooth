/*
 * Copyright 2018 Michael Ryan
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
#include <ctype.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_UUID "fd123ff9-9e30-45b2-af0d-b85b7d2dc80c"
#define BOOTLOADER_UUID "344bc7f2-5619-4953-9be8-9888fe29d996"

/* The BLE advertising channel indexes (Bluetooth Core spec: 37, 38, 39). */
#define ADV_INDEX_MIN 37
#define ADV_INDEX_MAX 39

/* ubertooth-ducky's result is the advertisement it sends over the air, so
 * everything it prints is progress and goes to standard error. */
void print_mac(FILE *file, uint8_t *mac_address) {
	unsigned i;
	for (i = 0; i < 5; ++i)
		fprintf(file, "%02x:", mac_address[i]);
	fprintf(file, "%02x", mac_address[5]);
}

int parse_uuid(const char *s, uint8_t *uuid_out) {
	unsigned i, n;

	if (s == NULL || strlen(s) != 36)
		return 0;

	for (i = 0, n = 0; i < 36 && n < 32; ++i) {
		unsigned nibble = 0;
		char lower;

		if (s[i] == '-')
			continue;
		if (isxdigit(s[i])) {
			if (s[i] >= '0' && s[i] <= '9')
				nibble = s[i] - '0';
			else {
				lower = tolower(s[i]);
				nibble = lower - 'a' + 10;
			}

			uuid_out[n/2] |= nibble << (n & 1 ? 0 : 4);
			++n;
		} else {
			return 0;
		}
	}

	return n == 32;
}

void print_uuid(FILE *file, uint8_t *uuid) {
	unsigned i;
	for (i =  0; i <  4; ++i) fprintf(file, "%02x", uuid[i]);
	fprintf(file, "-");
	for (i =  4; i <  6; ++i) fprintf(file, "%02x", uuid[i]);
	fprintf(file, "-");
	for (i =  6; i <  8; ++i) fprintf(file, "%02x", uuid[i]);
	fprintf(file, "-");
	for (i =  8; i < 10; ++i) fprintf(file, "%02x", uuid[i]);
	fprintf(file, "-");
	for (i = 10; i < 16; ++i) fprintf(file, "%02x", uuid[i]);
}

#define DUCKLEN 7
char *duck[DUCKLEN] = {
	"                  _\n",
	"                /`6\\__ .oO( %s ! )\n",
	"         ,_     \\ _.==' \n",
	"        `) `\"\"\"\"`~~\\\n",
	"      -~ \\  '~-.   / ~-\n",
	"        ~- `~-====-' ~_ ~-\n",
	"     ~ - ~ ~- ~ - ~ -\n",
};

void print_duck(FILE *file, int s) {
	unsigned i;
	for (i = 0; i < DUCKLEN; ++i) {
		if (i != 1) {
			fprintf(file, "%s", duck[i]);
		} else {
			fprintf(file, duck[i], s ? "FLAT" : "UBERDUCKY");
		}
	}
}


/* The library has already said why a request was refused; this names the
 * request, and leaves the board released rather than part-way into a
 * low-energy mode. A refused request is not proof that the board never got
 * it -- a transfer can time out after it arrived -- so the message says only
 * what the tool knows. */
static int request_failed(ubertooth_t *ut, const char *what) {
	fprintf(stderr, "error: could not %s\n", what);
	ubertooth_stop(ut);
	ubertooth_free(ut);
	return 1;
}

static void usage(FILE *file) {
	fprintf(file, "ubertooth-ducky - make an Uberducky quack like a USB Rubber Ducky\n");
	fprintf(file, "Usage:\n");
	fprintf(file, "\t-q [uuid] quack!\n");
	fprintf(file, "\t-b signal Uberducky to enter bootloader\n");
	fprintf(file, "\n");
	fprintf(file, "\t-A <%d-%d> advertising channel index (default: 38)\n",
	        ADV_INDEX_MIN, ADV_INDEX_MAX);
	fprintf(file, "\t-a <BD ADDR> Bluetooth address, xx:xx:xx:xx:xx:xx (default: random)\n");
	fputs(OPT_USAGE_SELECT_DEVICE, file);
	fprintf(file, "\t-h this help\n");
	fprintf(file, "\n");
	fprintf(file, "For more information on Uberducky, visit:\n");
	fprintf(file, "https://github.com/mikeryan/uberducky\n");
}

int main(int argc, char *argv[])
{
	int opt;
	opt_once once = {0};
	/* -h ends the command once the whole command line has parsed (see below). */
	int first_exit_option = 0;
	int do_quack, do_adv_index;
	int addr_set, be_sarcastic;
	/* Either a word of argv or one of the UUID constants: nothing is copied,
	 * so no exit path has a string to give back. */
	const char *uuid_str = NULL;
	uint8_t uuid[16] = { 0, };
	uint8_t bd_addr[6] = { 0, };
	uint8_t adv_data[16+2] = { 0, };
	char* ubertooth_device = NULL;
	ubertooth_t* ut = NULL;

	int r;

	do_quack = 0;
	do_adv_index = 38;
	addr_set = 0;
	be_sarcastic = 0;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":q::bA:a:hsU:")) != EOF) {
		/* getopt only attaches an optional value (-q<uuid>); take a
		 * following word that is not an option as the value too, so
		 * "-q <uuid>" quacks that UUID rather than the default and
		 * leaving the UUID as a stray word. Resolved before the check
		 * below, which only counts an option that came with a value. */
		if (optarg == NULL && opt == 'q')
			optarg = opt_separate_value(argc, argv, opt_value_not_option);
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
		switch(opt) {
		case 'q':
			do_quack = 1;
			uuid_str = optarg ? optarg : DEFAULT_UUID;
			break;

		case 'b':
			do_quack = 1;
			uuid_str = BOOTLOADER_UUID;
			break;

		case 'A':
			if (opt_require_int(optarg, ADV_INDEX_MIN, ADV_INDEX_MAX,
			                    "advertising channel index", "",
			                    &do_adv_index) < 0)
				return 1;
			break;

		case 'a':
			if (opt_parse_bd_addr(optarg, bd_addr) < 0) {
				fprintf(stderr, "error: Bluetooth address must be "
				        "xx:xx:xx:xx:xx:xx (got %s)\n", optarg);
				return 1;
			}
			addr_set = 1;
			break;

		case 'U':
			ubertooth_device = optarg;
			break;

		case 's':
			be_sarcastic = 1;
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

	/* ubertooth-ducky takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h ends the command, but only once the whole command line has
	 * parsed: a bad option value or a stray word is reported even
	 * alongside it. */
	if (first_exit_option == 'h') {
		usage(stdout);
		return 0;
	}

	/* -q or -b is the work. Without either the tool used to open the
	 * selected board, check its firmware, quack nothing and exit 0. */
	if (!do_quack) {
		fputs("error: nothing to do: give -q to quack, or -b to signal the "
		      "bootloader\n", stderr);
		if (opt_no_arguments(argc))
			usage(stderr);
		return 1;
	}

	if (do_quack) {
		FILE *random;

		r = parse_uuid(uuid_str, uuid);
		if (!r) {
			if (!be_sarcastic)
				fprintf(stderr, "error: cannot read the UUID '%s'\n", uuid_str);
			else
				fputs("error: UUID's fucked mate. Did an upright duck "
				      "tell you to type that?\n", stderr);
			return 1;
		}

		if (!addr_set) {
			random = fopen("/dev/urandom", "r");
			if (random == NULL || fread(bd_addr, 6, 1, random) != 1) {
				fputs("error: could not read a random BD_ADDR from "
				      "/dev/urandom\n", stderr);
				if (random != NULL)
					fclose(random);
				return 1;
			}
			fclose(random);
		}

	}

	/* Nothing above this point touches a board, so the bookkeeping for one is
	 * allocated only once the command line has been accepted: a refused
	 * command line then has nothing to give back. */
	ut = ubertooth_init();
	if (ut == NULL)
		return 1;

	r = ubertooth_connect_select(ut, ubertooth_device);
	if (r < 0) {
		ubertooth_free(ut);
		return 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0) {
		/* On a version mismatch ubertooth_check_api() has released the
		 * board itself; where it could not even read the version it has
		 * released nothing. Closing covers both, because closing a
		 * board already released does nothing, and it sends no STOP to
		 * a firmware this host will not drive. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		return 1;
	}

	/* A handler for Ctrl-C and the rest, so the wait below is cut short and
	 * the board is taken out of advertising on the way out rather than left
	 * in it. The library's handlers, as every other tool uses. */
	register_cleanup_handler(ut, 0);

	if (do_quack) {
		uint16_t channel;
		unsigned i;

		print_duck(stderr, be_sarcastic);
		fprintf(stderr, "\n");
		fprintf(stderr, "Quacking ");
		print_uuid(stderr, uuid);
		fprintf(stderr, " as ");
		print_mac(stderr, bd_addr);
		fprintf(stderr, "\n");

		adv_data[0] = 0x11; // 17 bytes
		adv_data[1] = 0x07; // 128 bit UUIDs
		for (i = 0; i < 16; ++i) // reverse byte order
			adv_data[2+i] = uuid[15-i];

		if (do_adv_index == 37)
			channel = BTLE_ADV_CHANNEL_37_MHZ;
		else if (do_adv_index == 38)
			channel = BTLE_ADV_CHANNEL_38_MHZ;
		else
			channel = BTLE_ADV_CHANNEL_39_MHZ;

		if (cmd_set_channel(ut->devh, channel) < 0)
			return request_failed(ut, "set the advertising channel");
		if (cmd_le_set_adv_data(ut->devh, adv_data, sizeof(adv_data)) < 0)
			return request_failed(ut, "set the advertising data");
		if (cmd_btle_slave(ut->devh, bd_addr) < 0)
			return request_failed(ut, "start advertising");

		sleep(2);
		ubertooth_stop(ut);
	}
	ubertooth_free(ut);
	return 0;
}
