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

/* The firmware takes a one-byte CC2400 register address, so the tool accepts
 * every address from 0x00 (MAIN) to 0xff. */
#define CC2400_REG_ADDRESS_MAX 0xff

/*
 * "<item> in <whole value>" for the refused item, or just the value when the
 * item is the whole of it or is empty (a missing value between commas).
 */
static void describe_item(char *out, size_t size,
                          const struct int_list_error *failure,
                          const char *value)
{
    if (failure->item_length == 0 || failure->item_length == strlen(value))
	snprintf(out, size, "%s", value);
    else
	snprintf(out, size, "%.*s in %s", (int)failure->item_length,
		 failure->item, value);
}

/* The usage text: on standard output for -h, on standard error for a bare
 * command line. */
static void usage(FILE *file)
{
    fputs("ubertooth-debug - command line utility for debugging Ubertooth One\n",
	  file);
    fputs("Usage:\n", file);
    fputs("\t-h this message\n", file);
    fputs("\t-r <reg>[,<reg>[,...]] read the contents of CC2400 register(s),\n",
	  file);
    fprintf(file, "\t   each 0x00-0x%02x or a %%name (example: -r 0x02,%%main)\n",
	    CC2400_REG_ADDRESS_MAX);
    fputs("\t-r <start>-<end> read a consecutive set of CC2400 register(s)\n",
	  file);
    fputs(OPT_USAGE_SELECT_DEVICE, file);
    fputs("\t-v <0-2> verbosity (default=1)\n", file);
}

/* Looks up one "%name" at the start of t, which runs to the next list
 * separator. Matching the whole of t instead meant a name only worked as the
 * last item: "-r %MAIN,%FSDIV" was looked up as "%MAIN,%FSDIV" and refused.
 * t points into argv, so the separator is borrowed and put back rather than
 * copying the name into a buffer of some guessed size. */
int token_to_int(char *t, int *size)
{
    size_t length = strcspn(t, ",-");
    char separator;
    int r;

    *size = -1;
    if (length == 0)
	return -1;
    separator = t[length];
    t[length] = '\0';
    r = cc2400_name2reg(t);
    t[length] = separator;
    if (r < 0)
	return -1;
    *size = (int)length;

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

    /* Report a bad option ourselves rather than letting getopt print it:
     * see opt_report_bad_option(). The leading ':' makes a missing option
     * value distinguishable from an option getopt does not know. */
    opterr = 0;
    while ((opt = getopt(argc, argv, ":hU:r:v:")) != EOF) {
	switch (opt) {
	case 'h':
	    do_help = 1;
	    break;
	case 'U':
	    ubertooth_device = optarg;
	    break;
	case 'v':
	    if (opt_require_int(optarg, 0, 2, "verbosity", "", &verbose) < 0) {
		free(regList);
		return 1;
	    }
	    break;
	case 'r': {
	    struct int_list_error failure;

	    /* A repeated -r replaces the earlier list. */
	    free(regList);
	    regList = listOfInts(optarg, &regListN, token_to_int, 0,
				 CC2400_REG_ADDRESS_MAX, &failure);
	    if (regListN < 0) {
		/* One line, like every other refused option value, naming the
		 * item as the user wrote it. The accepted range is the whole
		 * of it, not one short at each end: 0x00 (MAIN) and 0xff are
		 * both accepted. */
		char item[64];

		describe_item(item, sizeof(item), &failure, optarg);
		if (failure.kind == INT_LIST_RANGE)
		    fprintf(stderr, "error: register address must be"
			    " 0x00-0x%02x (got %s)\n",
			    CC2400_REG_ADDRESS_MAX, item);
		else
		    fprintf(stderr, "error: register list must be names or"
			    " addresses, single or as a range: %s (got %s)\n",
			    failure.reason, item);
		/* listOfInts() frees its partial array and returns NULL on
		 * every failure, so there is nothing of ours to give back. */
		return 1;
	    }
	    if (regListN > 0)
		do_read_register = 0;
	    break;
	}
	default:
	    opt_report_bad_option(opt, argc, argv);
	    free(regList);
	    return 1;
	}
    }

    /* ubertooth-debug takes no operands: refuse a leftover word before any
     * device is opened, rather than running the rest of the command. */
    if (opt_check_no_operands(argc, argv) < 0) {
	free(regList);
	return 1;
    }

    /* -h ends the command, but only once the whole command line has parsed:
     * a bad option value or a stray word is reported even alongside it. */
    if (do_help) {
	usage(stdout);
	free(regList);
	return 0;
    }

    if (regListN == 0) {
	fprintf(stderr, "error: -r <reg>[,<reg>[,...]] is required:"
		" at least one register to read\n");
	if (opt_no_arguments(argc))
	    usage(stderr);
	free(regList);
	return 1;
    }
    /* initialise device */
    ut = ubertooth_start_select(ubertooth_device);
    if (ut == NULL) {
	free(regList);
	return 1;
    }

    /* Exit 0 only if every read succeeded; this used to return the last
     * register's value (or error) as the exit status. */
    if (do_read_register >= 0)
	r = debug_read_registers(read_register, ut->devh, regList, regListN,
	                         verbose, stdout, stderr);

    free(regList);
    /* Release the handle without sending STOP, as ubertooth-util does, so the
     * device's radio state is left as the tool found it: this one only reads
     * registers. Releasing the interface is still a request to the device;
     * what is avoided is cmd_stop, which would take the radio through
     * cc2400_idle() and reset the channel. */
    ubertooth_close(ut);
    ubertooth_free(ut);

    return r;
}
