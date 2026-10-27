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

#include <errno.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include "ubertooth.h"
#include "opt_parse.h"
#include "request_report.h"
#include "specan_options.h"
#include "util_clock.h"

_Static_assert(SPECAN_FREQ_MIN == BT_FREQ_MIN && SPECAN_FREQ_MAX == BT_FREQ_MAX,
               "specan_options.h must match the library's band limits");

/* Set by -v: the run says what it is doing on standard error. Standard output
 * carries only the samples, so nothing here is ever written to it. */
static int verbose;

/* How often a running sweep reports: often enough that a sweep which has gone
 * quiet shows up within a second, rare enough that a few seconds of capture
 * stays readable. A line per sample would bury the terminal. */
#define SPECAN_REPORT_INTERVAL_MS 1000u

/* What the sweep has done so far, counted for the -v summary lines. */
static struct {
	uint64_t started_ms;  /* when the receive loop started */
	uint64_t reported_ms; /* when the last summary line was printed */
	unsigned long sweeps;       /* passes over the range that completed */
	unsigned long samples;      /* samples the board reported in range */
	unsigned long out_of_range; /* samples dropped as left over from an
	                             * earlier run (see cb_specan) */
} progress;

/* One summary line, labelled so the closing total is not read as another
 * periodic line. Counting nothing is itself the answer for a board that
 * stopped sending, so the line is printed whatever the counters hold. */
static void report_progress(const char *label, uint64_t now_ms)
{
	fprintf(stderr, "%s %.1f s: %lu sweeps, %lu samples, "
	        "%lu out of range\n", label,
	        (double)(now_ms - progress.started_ms) / 1000.0,
	        progress.sweeps, progress.samples, progress.out_of_range);
}

/* Called between receives: a summary line once per interval, so the rate is
 * the same whether the board is flooding the host or silent. */
static void report_progress_if_due(void)
{
	uint64_t now_ms = util_clock_now_ms();

	if (now_ms - progress.reported_ms < SPECAN_REPORT_INTERVAL_MS)
		return;
	progress.reported_ms = now_ms;
	report_progress("After", now_ms);
}

/* Where the samples are going, in the words of the option that chose it. A
 * -d file that a later -g took the samples away from is not the target: this
 * says where they land, not which options were given. */
static void report_output_target(int output_mode, const char *dump_name)
{
	switch (output_mode) {
	case SPECAN_FILE:
		/* -d set this mode, so it named a file or a bare "-". */
		if (strcmp(dump_name, "-") == 0)
			fputs("Writing raw samples to standard output\n", stderr);
		else
			fprintf(stderr, "Writing raw samples to %s\n", dump_name);
		break;
	case SPECAN_GNUPLOT_NORMAL:
		fputs("Writing feedgnuplot text to standard output\n", stderr);
		break;
	case SPECAN_GNUPLOT_3D:
		fputs("Writing 3D feedgnuplot text to standard output\n", stderr);
		break;
	case SPECAN_STDOUT:
	default:
		/* Every mode is named above, so the default is only reached by a
		 * mode cb_specan would refuse. Naming SPECAN_STDOUT as well keeps
		 * the compiler's warning about a mode added here and nowhere else. */
		fputs("Writing text to standard output\n", stderr);
		break;
	}
}

/* Set when the run cannot write a sample. The callback has no way to return a
 * status, so it records the failure here and stops the receive loop; main()
 * then exits 1 rather than the status of the command that started the sweep. */
static int output_failed;

static int write_failure_said;

/* Said once per run: a sweep that already reported a failed write does not
 * report it again for every later block, nor again at the closing flush. */
static void report_write_failed(const char *detail)
{
	if (write_failure_said)
		return;
	write_failure_said = 1;
	fprintf(stderr, "error: writing samples failed: %s\n", detail);
}

/* Whether everything written to this stream reached it. A stream can accept a
 * write into its buffer and only fail later, so the error flag and the flush
 * are both checked; -1 means some samples were lost, and it is reported here
 * so every write failure reads the same. */
static int specan_output_flushed(FILE *stream)
{
	char detail[128];

	if (stream == NULL)
		return 0;
	errno = 0;
	if (fflush(stream) == 0 && !ferror(stream))
		return 0;
	snprintf(detail, sizeof(detail), "could not flush the samples (%s)",
	         errno != 0 ? strerror(errno) : "write error");
	report_write_failed(detail);
	return -1;
}

void cb_specan(ubertooth_t* ut, void* args)
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
	char detail[128];

	/* A keep-alive only says a quiet sweep is still alive. It carries no
	 * samples, so reading frequencies and levels out of it would report
	 * whatever the slot held before. */
	if (rx.pkt_type == KEEP_ALIVE)
		return;

	/* process each received block */
	for (j = 0; j < DMA_SIZE-2; j += 3) {
		frequency = (rx.data[j] << 8) | rx.data[j + 1];
		rssi = (int8_t)rx.data[j + 2];
		/* Drop samples outside the requested range: they are left over
		 * from an earlier run, still queued on the device. */
		if (!specan_sample_in_range(frequency, low_freq, high_freq)) {
			progress.out_of_range++;
			continue;
		}
		progress.samples++;
		/* The upper frequency is the last step of a pass over the range,
		 * which is what the gnuplot modes below end a record on. */
		if (frequency == high_freq)
			progress.sweeps++;
		switch(output_mode) {
			case SPECAN_FILE:
				/* fwrite() is not required to set errno, so a stale one
				 * from an unrelated call cannot be mistaken for this. */
				errno = 0;
				r = fwrite(&rx.data[j], 1, 3, dumpfile);
				if(r != 3) {
					/* Said once, then the run ends: a full disk or a
					 * stream that has gone away does not come back, so
					 * carrying on would repeat this for every block of a
					 * live sweep and still exit 0. */
					snprintf(detail, sizeof(detail),
					         "wrote %d of 3 bytes (%s)", r,
					         errno != 0 ? strerror(errno) : "write error");
					report_write_failed(detail);
					output_failed = 1;
					ut->stop_ubertooth = 1;
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
				fprintf(stderr, "error: unrecognized output mode (%d)\n",
				        output_mode);
				output_failed = 1;
				ut->stop_ubertooth = 1;
				return;
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
	fprintf(file, "\t-v report the board, the sweep and a summary of it "
	        "once a second, to stderr\n");
	fprintf(file, "\t-g output suitable for feedgnuplot\n");
	fprintf(file, "\t-G output suitable for 3D feedgnuplot\n");
	fprintf(file, "\t-d <filename> output to file; -d - writes to stdout (not a tty)\n");
	fprintf(file, "\t-l <%d-%d> lower frequency in MHz (default %d)\n",
	        SPECAN_FREQ_MIN, SPECAN_FREQ_MAX, SPECAN_FREQ_MIN);
	fprintf(file, "\t-u <%d-%d> upper frequency in MHz (default %d)\n",
	        SPECAN_FREQ_MIN, SPECAN_FREQ_MAX, SPECAN_FREQ_MAX);
	fputs(OPT_USAGE_SELECT_DEVICE, file);
}

int main(int argc, char *argv[])
{
	int opt, r = 0, output_mode = SPECAN_STDOUT;
	/* 1 once the sweep's USB stream died part-way, so the exit status says
	 * the samples are incomplete. */
	int stream_failed = 0;
	int do_help = 0;
	int lower= SPECAN_FREQ_MIN, upper= SPECAN_FREQ_MAX;
	char* ubertooth_device = NULL;
	const char* dump_name = NULL; /* -d value, opened after every check */

	ubertooth_t* ut = NULL;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	opterr = 0;
	while ((opt=getopt(argc,argv,":vhgGd:l:u:U:")) != EOF) {
		switch(opt) {
		case 'v':
			/* There is one level of it, so a second -v says nothing
			 * new and is more likely a mistyped command line. */
			if (verbose) {
				fputs("error: -v may be given only once\n", stderr);
				return 1;
			}
			verbose = 1;
			break;
		case 'g':
			output_mode = SPECAN_GNUPLOT_NORMAL;
			break;
		case 'G':
			output_mode = SPECAN_GNUPLOT_3D;
			break;
		case 'd':
			/* Named here and opened once the whole command line has
			 * been accepted, so a refused command creates and
			 * truncates nothing. */
			output_mode = SPECAN_FILE;
			dump_name = optarg;
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
			opt_report_bad_option(opt, argc, argv);
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

	/* Every check has passed, so -d can be opened. Only a bare "-" means
	 * stdout (the one argument rule, as ubertooth-btle -r already checks).
	 * Any other value is a file name, including one starting with "-", so
	 * "-d -x" writes the file "-x" rather than silently streaming samples
	 * to stdout and dropping a mistyped option. */
	if (dump_name != NULL) {
		if (strcmp(dump_name, "-") == 0) {
			/* The samples are raw 3-byte records, so a terminal is
			 * refused the way ubertooth-btle refuses its capture.
			 * Only when they are what the run writes: a later -g or
			 * -G takes the output mode back to text, which a
			 * terminal is welcome to. */
			if (output_mode == SPECAN_FILE &&
			    opt_check_stdout_not_tty("samples") < 0)
				return 1;
			dumpfile = stdout;
		} else {
			dumpfile = fopen(dump_name, "w");
			if (dumpfile == NULL) {
				fprintf(stderr, "error: %s: %s\n", dump_name,
				        strerror(errno));
				return 1;
			}
		}
	}

	ut = ubertooth_start_select(ubertooth_device);

	if (ut == NULL) {
		return 1;
	}

	/* Which board, before the firmware check below, so a -v run that ends
	 * on a version mismatch still says which board it read. The selection
	 * above prints nothing when it succeeds, and -U may name an index, so
	 * the serial number is read rather than repeated from the command
	 * line. */
	if (verbose) {
		char serial[UBERTOOTH_SERIAL_STR_LEN];

		if (ubertooth_read_serial_string(ut->devh, serial, sizeof(serial),
		                                 UBERTOOTH_SERIAL_READ_QUIET_MS) == 0)
			fprintf(stderr, "Using Ubertooth %s\n", serial);
		else
			fputs("Using an Ubertooth whose serial number could not be "
			      "read\n", stderr);
	}

	r = ubertooth_check_api(ut);
	if (r < 0) {
		/* A refused version was released inside ubertooth_check_api(); one
		 * it could not read at all leaves the board open. Closing covers
		 * both, because closing a board already released does nothing. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		return 1;
	}

	/* The sweep as it will actually run, and where its samples go: the two
	 * things a user comparing a specan run with what they asked for needs,
	 * and the file name -d opened. */
	if (verbose) {
		int per_sweep = (upper - lower) / SPECAN_FREQ_STEP + 1;

		/* -l 2450 -u 2450 is a legal one-channel sweep, so the count can
		 * be 1. */
		fprintf(stderr, "Sweeping %d-%d MHz in %d MHz steps, "
		        "%d sample%s per sweep\n", lower, upper, SPECAN_FREQ_STEP,
		        per_sweep, per_sweep == 1 ? "" : "s");
		report_output_target(output_mode, dump_name);
	}

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
		ubertooth_free(ut);
		return 1;
	}

	// init USB transfer
	r = ubertooth_bulk_init(ut);
	if (r < 0) {
		ubertooth_stop(ut);
		ubertooth_free(ut);
		return 1;
	}

	r = ubertooth_bulk_thread_start();
	if (r != 0) {
		/* Not "< 0": this is pthread_create()'s errno, which is positive,
		 * so a failure used to slip through and leave the sweep with
		 * nobody to complete its transfers -- it then waited forever with
		 * no samples arriving. */
		report_capture_thread_failure(r);
		ubertooth_stop(ut);
		ubertooth_free(ut);
		return 1;
	}

	// tell ubertooth to start specan and send packets
	r = cmd_specan(ut->devh, lower, upper);
	if (r < 0) {
		ubertooth_bulk_thread_stop();
		ubertooth_stop(ut);
		ubertooth_free(ut);
		return 1;
	}

	progress.started_ms = util_clock_now_ms();
	progress.reported_ms = progress.started_ms;

	// receive and process each packet
	while(!ubertooth_stopping(ut)) {
		ubertooth_bulk_receive(ut, cb_specan, specan_args);
		/* Here rather than in cb_specan(): this loop runs whether samples
		 * arrive or not, so a sweep that has gone silent still reports. */
		if (verbose)
			report_progress_if_due();
	}

	if (verbose)
		report_progress("Total after", util_clock_now_ms());

	ubertooth_bulk_thread_stop();

	ubertooth_stop(ut);

	/* A sweep whose stream died stopped there, so the samples are
	 * incomplete. The handle is read after the teardown, because cancelling
	 * the transfer can be what brings the failure out. libubertooth has
	 * already printed what failed. */
	stream_failed = ubertooth_stream_failed(ut);
	ubertooth_free(ut);

	fprintf(stderr, "Ubertooth stopped\n");
	/* The samples a write accepted may still be sitting in a stdio buffer, so
	 * the last of them can fail after the sweep has ended: the run is not
	 * done until the output is flushed. The text modes write with printf()
	 * and are only checked here. */
	if (specan_output_flushed(dumpfile) < 0)
		output_failed = 1;
	if (output_mode != SPECAN_FILE && specan_output_flushed(stdout) < 0)
		output_failed = 1;
	/* A sweep that could not be written out, or that lost its stream part-way,
	 * did not do what was asked, even though the command that started it
	 * succeeded. */
	if (output_failed || stream_failed)
		return 1;
	return r;
}
