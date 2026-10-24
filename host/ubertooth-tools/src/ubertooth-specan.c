/*
 * Copyright 2010 Michael Ossmann
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

#include <getopt.h>
#include <stdlib.h>
#include "ubertooth.h"
#include "opt_parse.h"
#include "specan_options.h"

_Static_assert(SPECAN_FREQ_MIN == BT_FREQ_MIN && SPECAN_FREQ_MAX == BT_FREQ_MAX,
               "specan_options.h must match the library's band limits");

uint8_t debug;

void cb_specan(ubertooth_t* ut __attribute__((unused)), void* args)
{
	uint16_t high_freq = (((uint8_t*)args)[0]) |
	                     (((uint8_t*)args)[1] << 8);
	uint8_t output_mode = ((uint8_t*)args)[2];
	uint16_t low_freq = (((uint8_t*)args)[3]) |
	                    (((uint8_t*)args)[4] << 8);

	usb_pkt_rx rx = fifo_pop(ut->fifo);
	int r, j;
	uint16_t frequency;
	int8_t rssi;

	/* process each received block */
	for (j = 0; j < DMA_SIZE-2; j += 3) {
		frequency = (rx.data[j] << 8) | rx.data[j + 1];
		rssi = (int8_t)rx.data[j + 2];
		/* Drop samples outside the requested range: they are left over
		 * from an earlier run, still queued on the device. */
		if (!specan_sample_in_range(frequency, low_freq, high_freq))
			continue;
		switch(output_mode) {
			case SPECAN_FILE:
				r = fwrite(&rx.data[j], 1, 3, dumpfile);
				if(r != 3) {
					fprintf(stderr, "Error writing to file (%d)\n", r);
					return;
				}
				break;
			case SPECAN_STDOUT:
				printf("%f, %d, %d\n", ((double)rx.clk100ns)/10000000,
				       frequency, rssi);
				break;
			case SPECAN_GNUPLOT_NORMAL:
				printf("%d %d\n", frequency, rssi);
				if(frequency == high_freq)
					printf("\n");
				break;
			case SPECAN_GNUPLOT_3D:
				printf("%f %d %d\n", ((double)rx.clk100ns)/10000000,
				       frequency, rssi);
				if(frequency == high_freq)
					printf("\n");
				break;
			default:
				fprintf(stderr, "Unrecognised output mode (%d)\n",
				        output_mode);
				return;
				break;
		}
	}
	fflush(stderr);
}

static void usage(FILE *file)
{
	fprintf(file, "ubertooth-specan - output a continuous stream of signal strengths\n");
	fprintf(file, "\n");
	fprintf(file, "!!!!!\n");
	fprintf(file, "NOTE: you probably want ubertooth-specan-ui\n");
	fprintf(file, "!!!!!\n");
	fprintf(file, "\n");
	fprintf(file, "Usage:\n");
	fprintf(file, "\t-h this help\n");
	fprintf(file, "\t-v verbose (print debug information to stderr)\n");
	fprintf(file, "\t-g output suitable for feedgnuplot\n");
	fprintf(file, "\t-G output suitable for 3D feedgnuplot\n");
	fprintf(file, "\t-d <filename> output to file\n");
	fprintf(file, "\t-l <%d-%d> lower frequency in MHz (default %d)\n",
	        SPECAN_FREQ_MIN, SPECAN_FREQ_MAX, SPECAN_FREQ_MIN);
	fprintf(file, "\t-u <%d-%d> upper frequency in MHz (default %d)\n",
	        SPECAN_FREQ_MIN, SPECAN_FREQ_MAX, SPECAN_FREQ_MAX);
	fprintf(file, "\t-U <index|serial> select Ubertooth by index or serial; see ubertooth(7)\n");
}

int main(int argc, char *argv[])
{
	int opt, r = 0, output_mode = SPECAN_STDOUT;
	int do_help = 0;
	int lower= SPECAN_FREQ_MIN, upper= SPECAN_FREQ_MAX;
	char* ubertooth_device = NULL;

	ubertooth_t* ut = NULL;

	while ((opt=getopt(argc,argv,"vhgGd:l:u:U:")) != EOF) {
		switch(opt) {
		case 'v':
			debug++;
			break;
		case 'g':
			output_mode = SPECAN_GNUPLOT_NORMAL;
			break;
		case 'G':
			output_mode = SPECAN_GNUPLOT_3D;
			break;
		case 'd':
			output_mode = SPECAN_FILE;
			if(*optarg == '-') {
				dumpfile = stdout;
			} else {
				dumpfile = fopen(optarg, "w");
				if (dumpfile == NULL) {
					perror(optarg);
					return 1;
				}
			}
			break;
		case 'l':
			if (specan_parse_frequency(optarg, &lower) < 0) {
				fprintf(stderr, "error: lower frequency must be %d-%d MHz "
				        "(got %s)\n", SPECAN_FREQ_MIN, SPECAN_FREQ_MAX, optarg);
				return 1;
			}
			break;
		case 'u':
			if (specan_parse_frequency(optarg, &upper) < 0) {
				fprintf(stderr, "error: upper frequency must be %d-%d MHz "
				        "(got %s)\n", SPECAN_FREQ_MIN, SPECAN_FREQ_MAX, optarg);
				return 1;
			}
			break;
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'h':
			do_help = 1;
			break;
		default:
			usage(stderr);
			return 1;
		}
	}

	/* ubertooth-specan takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* -h ends the command, but only once the whole command line has parsed:
	 * a bad option value or a stray word is reported even alongside it. */
	if (do_help) {
		usage(stdout);
		return 0;
	}

	if (specan_validate_frequency_range(lower, upper) < 0) {
		fprintf(stderr,
		        "error: lower frequency must not exceed upper frequency\n");
		return 1;
	}

	ut = ubertooth_start_select(ubertooth_device);

	if (ut == NULL) {
		return 1;
	}

	r = ubertooth_check_api(ut);
	if (r < 0)
		return 1;

	/* Clean up on exit. */
	register_cleanup_handler(ut, 0);

	uint8_t specan_args[] = {
		(uint8_t)(upper & 0xff),
		(uint8_t)(upper >> 8),
		output_mode,
		(uint8_t)(lower & 0xff),
		(uint8_t)(lower >> 8)
	};

	/* Leave any mode still running on the device (for example a specan
	 * whose process was killed without stopping it) before starting, as
	 * the other tools do. STOP is not acknowledged, so this narrows the
	 * window in which the new range lands in a sweep already running; the
	 * range filter in cb_specan() is what keeps the output correct. */
	r = cmd_stop(ut->devh);
	if (r < 0) {
		ubertooth_stop(ut);
		return 1;
	}

	// init USB transfer
	r = ubertooth_bulk_init(ut);
	if (r < 0) {
		ubertooth_stop(ut);
		return 1;
	}

	r = ubertooth_bulk_thread_start();
	if (r < 0) {
		ubertooth_stop(ut);
		return 1;
	}

	// tell ubertooth to start specan and send packets
	r = cmd_specan(ut->devh, lower, upper);
	if (r < 0) {
		ubertooth_bulk_thread_stop();
		ubertooth_stop(ut);
		return 1;
	}

	// receive and process each packet
	while(!ut->stop_ubertooth) {
		ubertooth_bulk_receive(ut, cb_specan, specan_args);
	}

	ubertooth_bulk_thread_stop();

	ubertooth_stop(ut);
	fprintf(stderr, "Ubertooth stopped\n");
	return r;
}
