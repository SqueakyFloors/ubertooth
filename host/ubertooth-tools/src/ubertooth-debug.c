/*
 * Copyright 2013 Mike Ryan
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

/* formated with: indent -kr */

#include "ubertooth.h"
#include "debug_registers.h"
#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "cc2400.h"
#include "arglist.h"
#include "opt_parse.h"

/* The firmware takes a one-byte CC2400 register address, so every address
 * from 0x00 (MAIN) to 0xff can be read. */
#define CC2400_REG_ADDRESS_MAX 0xff

static void usage()
{
    printf
	("ubertooth-debug - command line utility for debugging Ubertooth One\n");
    printf("Usage:\n");
    printf("\t-h this message\n");
    printf("\t-r <reg>[,<reg>[,...]] read the contents of CC2400 register(s)\n");
    printf("\t-r <start>-<end> read a consecutive set of CC2400 register(s)\n");
    fputs(OPT_USAGE_SELECT_DEVICE, stdout);
    printf("\t-v<0-2> verbosity (default=1)\n");
}

int token_to_int(char *t, int *size)
{
    int r = 0;

    r = cc2400_name2reg(t);
    if (r >= 0)
	*size = strlen(cc2400_reg2name(r));
    else
	*size = -1;

    return r;
}

_Static_assert(DEBUG_ERROR_SHORT_REPLY == UBERTOOTH_ERROR_SHORT_REPLY,
               "debug_registers.h must match libubertooth's short-reply code");

static int read_register(void *context, uint8_t reg)
{
	return cmd_read_register((struct libusb_device_handle *)context, reg);
}

int main(int argc, char *argv[])
{
    int opt;
    int r = 0;
    int do_help = 0;
    int verbose = 1;
    ubertooth_t* ut = NULL;
    int do_read_register;
    char* ubertooth_device = NULL;
    int *regList = NULL;
    int regListN = 0;
    int i;

    /* set command states to negative as a starter
     * setting to positive is value of specified argument */
    do_read_register = -1;

    while ((opt = getopt(argc, argv, "hU:r:v:")) != EOF) {
	switch (opt) {
	case 'h':
	    do_help = 1;
	    break;
	case 'U':
	    ubertooth_device = optarg;
	    break;
	case 'v':
	    if (opt_require_int(optarg, 0, 2, "verbosity", "", &verbose) < 0)
		return 1;
	    break;
	case 'r':
	    regList = listOfInts(optarg, &regListN, token_to_int);
	    if (regListN < 0) {
			/* listOfInts() has already said what it could not read. */
			fprintf(stderr,
				"error: register list must be names or addresses,"
				" single or as a range (got %s)\n", optarg);
			return 1;
	    }
	    for (i = 0; i < regListN; i++) {
			if (regList[i] < 0 || regList[i] > CC2400_REG_ADDRESS_MAX) {
			    /* The accepted range, not one short of it at each end:
			     * 0x00 (MAIN) and 0xff are both readable. */
			    fprintf(stderr,
				    "error: register address must be 0x00-0x%02x"
				    " (got 0x%02x)\n",
				    CC2400_REG_ADDRESS_MAX, regList[i]);
			    return 1;
			}
	    }
	    if (regListN > 0)
			do_read_register = 0;
	    break;
	default:
	    usage();
	    return 1;
	}
    }

    /* ubertooth-debug takes no operands: refuse a leftover word before any
     * device is opened, rather than running the rest of the command. */
    if (opt_check_no_operands(argc, argv) < 0)
	return 1;

    /* -h ends the command, but only once the whole command line has parsed:
     * a bad option value or a stray word is reported even alongside it. */
    if (do_help) {
	usage();
	return 0;
    }

	if(regListN == 0) {
		fprintf(stderr, "At least one register must be provided\n");
	    usage();
	    return 1;
	}
    /* initialise device */
    ut = ubertooth_start_select(ubertooth_device);
    if (ut == NULL) {
	return 1;
    }

    /* Exit 0 only if every read succeeded; this used to return the last
     * register's value (or error) as the exit status. */
    if (do_read_register >= 0)
	r = debug_read_registers(read_register, ut->devh, regList, regListN,
	                         verbose, stdout, stderr);

    return r;
}
