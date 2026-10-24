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
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_UUID "fd123ff9-9e30-45b2-af0d-b85b7d2dc80c"
#define BOOTLOADER_UUID "344bc7f2-5619-4953-9be8-9888fe29d996"

/* The BLE advertising channel indexes (Bluetooth Core spec: 37, 38, 39). */
#define ADV_INDEX_MIN 37
#define ADV_INDEX_MAX 39

int running = 1;

static void quit(int sig __attribute__((unused))) {
	running = 0;
}

void print_mac(uint8_t *mac_address) {
	unsigned i;
	for (i = 0; i < 5; ++i)
		printf("%02x:", mac_address[i]);
	printf("%02x", mac_address[5]);
}

int parse_uuid(char *s, uint8_t *uuid_out) {
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

void print_uuid(uint8_t *uuid) {
	unsigned i;
	for (i =  0; i <  4; ++i) printf("%02x", uuid[i]);
	printf("-");
	for (i =  4; i <  6; ++i) printf("%02x", uuid[i]);
	printf("-");
	for (i =  6; i <  8; ++i) printf("%02x", uuid[i]);
	printf("-");
	for (i =  8; i < 10; ++i) printf("%02x", uuid[i]);
	printf("-");
	for (i = 10; i < 16; ++i) printf("%02x", uuid[i]);
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

void print_duck(int s) {
	unsigned i;
	for (i = 0; i < DUCKLEN; ++i) {
		if (i != 1) {
			printf("%s", duck[i]);
		} else {
			printf(duck[i], s ? "FLAT" : "UBERDUCKY");
		}
	}
}


static void usage(void) {
	printf("ubertooth-ducky - make an Uberducky quack like a USB Rubber Ducky\n");
	printf("Usage:\n");
	printf("\t-q [uuid] quack!\n");
	printf("\t-b signal Uberducky to enter bootloader\n");
	printf("\n");
	printf("\t-A <%d-%d> advertising channel index (default: 38)\n",
	       ADV_INDEX_MIN, ADV_INDEX_MAX);
	printf("\t-a <BD ADDR> Bluetooth address, xx:xx:xx:xx:xx:xx (default: random)\n");
	fputs(OPT_USAGE_SELECT_DEVICE, stdout);
	printf("\t-h this help\n");
	printf("\n");
	printf("For more information on Uberducky, visit:\n");
	printf("https://github.com/mikeryan/uberducky\n");
}

int main(int argc, char *argv[])
{
	int opt;
	/* -h ends the command once the whole command line has parsed (see below). */
	int first_exit_option = 0;
	int do_quack, do_adv_index;
	int addr_set, be_sarcastic;
	char *uuid_str = NULL;
	uint8_t uuid[16] = { 0, };
	uint8_t bd_addr[6] = { 0, };
	uint8_t adv_data[16+2] = { 0, };
	char* ubertooth_device = NULL;
	ubertooth_t* ut = ubertooth_init();

	int r;

	do_quack = 0;
	do_adv_index = 38;
	addr_set = 0;
	be_sarcastic = 0;

	while ((opt=getopt(argc,argv,"q::bA:a:hsU:")) != EOF) {
		switch(opt) {
		case 'q':
			/* getopt only attaches an optional value (-q<uuid>); take a
			 * following word that is not an option as the value too, so
			 * "-q <uuid>" quacks that UUID rather than the default and
			 * leaving the UUID as a stray word. */
			if (optarg == NULL)
				optarg = opt_separate_value(argc, argv, opt_value_not_option);
			do_quack = 1;
			free(uuid_str);
			uuid_str = strdup(optarg ? optarg : DEFAULT_UUID);
			if (uuid_str == NULL) {
				fprintf(stderr, "error: out of memory\n");
				return 1;
			}
			break;

		case 'b':
			do_quack = 1;
			free(uuid_str);
			uuid_str = strdup(BOOTLOADER_UUID);
			if (uuid_str == NULL) {
				fprintf(stderr, "error: out of memory\n");
				return 1;
			}
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
			usage();
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
		usage();
		return 1;
	}

	if (do_quack) {
		FILE *random;

		r = parse_uuid(uuid_str, uuid);
		if (!r) {
			if (!be_sarcastic)
				printf("Error parsing UUID '%s'\n", uuid_str);
			else
				printf("UUID's fucked mate. Did an upright duck tell you to type that?\n");
			return 1;
		}
		free(uuid_str);

		if (!addr_set) {
			random = fopen("/dev/urandom", "r");
			if (random == NULL || fread(bd_addr, 6, 1, random) != 1) {
				fprintf(stderr, "Error: could not read random BD_ADDR from /dev/urandom\n");
				if (random != NULL)
					fclose(random);
				return 1;
			}
			fclose(random);
		}

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

	if (do_quack) {
		uint16_t channel;
		unsigned i;

		print_duck(be_sarcastic);
		printf("\n");
		printf("Quacking ");
		print_uuid(uuid);
		printf(" as ");
		print_mac(bd_addr);
		printf("\n");

		adv_data[0] = 0x11; // 17 bytes
		adv_data[1] = 0x07; // 128 bit UUIDs
		for (i = 0; i < 16; ++i) // reverse byte order
			adv_data[2+i] = uuid[15-i];

		if (do_adv_index == 37)
			channel = 2402;
		else if (do_adv_index == 38)
			channel = 2426;
		else
			channel = 2480;

		cmd_set_channel(ut->devh, channel);
		cmd_le_set_adv_data(ut->devh, adv_data, sizeof(adv_data));
		cmd_btle_slave(ut->devh, bd_addr);

		sleep(2);
		ubertooth_stop(ut);
	} 
	return 0;
}
