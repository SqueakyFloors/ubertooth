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

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "ubertooth.h"
#include "ubertooth_callback.h"
#include "ubertooth_control.h"
#include "ubertooth_interface.h"

#ifndef RELEASE
#define RELEASE "unknown"
#endif
#ifndef VERSION
#define VERSION "unknown"
#endif

uint32_t systime;
FILE* infile = NULL;
FILE* dumpfile = NULL;
/* Printed after "no Ubertooth at index": indexes are positions in the current
 * device list, so they shift when boards are added or removed. */
#define INDEX_SHIFT_HINT \
	"indexes change when boards are added or removed; select a board by " \
	"serial with -U<serial>\n"

int max_ac_errors = 2;

/* UBERTOOTH_TEST_NO_DEVICES=1 makes every selection path behave as if no
 * Ubertooth were attached, without listing or opening anything on the bus.
 * The tool tests set it for the tools they run, so a default `ctest` run can
 * neither depend on an attached board nor open one that another process is
 * using. Nothing in the tools or the library sets it, so a normal run is
 * unaffected. Exactly "1" turns it on, so a variable that happens to hold
 * anything else cannot hide a user's board. A tool that enumerates the bus
 * itself, such as ubertooth-dfu, calls this too. */
int ubertooth_devices_hidden(void)
{
	const char* value = getenv("UBERTOOTH_TEST_NO_DEVICES");

	return value != NULL && strcmp(value, "1") == 0;
}

/* ubertooth_devices_hidden() for the paths that report what they found: the
 * first of them to hide a device names the switch, so a variable left in an
 * environment does not look like a missing board. The quiet paths pass
 * announce = 0 and stay silent, as their callers require. */
static int devices_hidden(int announce)
{
	static int announced = 0;

	if (!ubertooth_devices_hidden())
		return 0;
	if (announce && !announced) {
		fprintf(stderr, "note: UBERTOOTH_TEST_NO_DEVICES is set, so no board "
		        "is selected\n");
		announced = 1;
	}
	return 1;
}

int do_exit = 1;
pthread_t poll_thread;

unsigned int packet_counter_max;

void print_version() {
	printf("libubertooth %s (%s), libbtbb %s (%s)\n", VERSION, RELEASE,
	       btbb_get_version(), btbb_get_release());
}

ubertooth_t* cleanup_devh = NULL;
static void cleanup(int sig __attribute__((unused)))
{
	if (cleanup_devh)
		cleanup_devh->stop_ubertooth = 1;
}

static void cleanup_exit(int sig __attribute__((unused)))
{
	if (cleanup_devh)
		ubertooth_stop(cleanup_devh);

	exit(0);
}

void register_cleanup_handler(ubertooth_t* ut, int do_exit) {
	cleanup_devh = ut;

	/* Clean up on ctrl-C. */
	/* SIGHUP (terminal closed) and SIGALRM (a timeout wrapper such as
	 * `perl -e 'alarm N; exec ...'`) also stop the device cleanly; killed
	 * by them, a tool left its mode running on the board. A tool that calls
	 * ubertooth_set_timeout() later installs its own SIGALRM handler. */
	if (do_exit) {
		signal(SIGINT, cleanup_exit);
		signal(SIGQUIT, cleanup_exit);
		signal(SIGTERM, cleanup_exit);
		signal(SIGHUP, cleanup_exit);
		signal(SIGALRM, cleanup_exit);
	} else {
		signal(SIGINT, cleanup);
		signal(SIGQUIT, cleanup);
		signal(SIGTERM, cleanup);
		signal(SIGHUP, cleanup);
		signal(SIGALRM, cleanup);
	}
}

ubertooth_t* timeout_dev = NULL;
void stop_transfers(int sig __attribute__((unused))) {
	if (timeout_dev)
		timeout_dev->stop_ubertooth = 1;
}

void ubertooth_set_timeout(ubertooth_t* ut, int seconds) {
	/* Upon SIGALRM, call stop_transfers() */
	if (signal(SIGALRM, stop_transfers) == SIG_ERR) {
		perror("Unable to catch SIGALRM");
		exit(1);
	}
	timeout_dev = ut;
	alarm(seconds);
}

int ubertooth_count(void) {
	struct libusb_device **usb_list = NULL;
	struct libusb_context *ctx = NULL;
	struct libusb_device_descriptor desc;
	int usb_devs, i, r;
	int ubertooth_device_count = 0;

	if (devices_hidden(1))
		return 0;

	r = libusb_init(NULL);
	if (r < 0) {
		fprintf(stderr, "libusb_init failed (got 1.0?)\n");
		return -1;
	}

	usb_devs = libusb_get_device_list(ctx, &usb_list);

	for(i = 0 ; i < usb_devs ; ++i) {
		r = libusb_get_device_descriptor(usb_list[i], &desc);
		if(r < 0) {
			fprintf(stderr, "couldn't get usb descriptor for dev #%d!\n", i);
			continue; /* desc was not filled in */
		}
		if ((desc.idVendor == TC13_VENDORID && desc.idProduct == TC13_PRODUCTID)
		    || (desc.idVendor == U0_VENDORID && desc.idProduct == U0_PRODUCTID)
		    || (desc.idVendor == U1_VENDORID && desc.idProduct == U1_PRODUCTID))
		{
			ubertooth_device_count++;
		}
	}

	if (usb_devs >= 0)
		libusb_free_device_list(usb_list,1);
	/* Balance the libusb_init() above: the count may be polled many times. */
	libusb_exit(NULL);
	return ubertooth_device_count;
}

/* Which entry of the found-Ubertooth list a -U index selects, or -1 for none;
 * ut_ct is how many Ubertooth devices that list holds. A negative index means
 * "no -U given": it selects the only device, or nothing when several are
 * attached. An index must be present in the list, so an index past the end
 * selects nothing, with one device attached as with several.
 * Shared by the loud and the quiet path of find_ubertooth_device_opt() so both
 * accept and refuse exactly the same selections. */
static int pick_ubertooth_device(int ubertooth_device, int ut_ct)
{
	if (ubertooth_device < 0)
		return (ut_ct == 1) ? 0 : -1;
	return (ubertooth_device < ut_ct) ? ubertooth_device : -1;
}

/* Opens the Ubertooth at enumeration index ubertooth_device, or the only one
 * when ubertooth_device is negative. When quiet, nothing is printed and no
 * command is sent: an index that is not present, or several devices with no
 * index, simply return NULL (without listing serials, which would mean sending
 * a command to every device). */
static struct libusb_device_handle* find_ubertooth_device_opt(int ubertooth_device,
                                                              int quiet)
{
	struct libusb_device **usb_list = NULL;
	struct libusb_context *ctx = NULL;
	struct libusb_device_handle *devh = NULL;
	struct libusb_device_descriptor desc;
	int usb_devs, i, r, ret, pick, ut_ct = 0;
	int *ubertooth_devs = NULL;

	/* Hidden devices: the same outcome as nothing attached, which this
	 * function reports by returning NULL without a message. */
	if (devices_hidden(!quiet))
		return NULL;

	usb_devs = libusb_get_device_list(ctx, &usb_list);
	if (usb_devs <= 0) {
		if (usb_devs == 0)
			libusb_free_device_list(usb_list, 1);
		return NULL;
	}
	ubertooth_devs = calloc(usb_devs, sizeof(int));
	if (ubertooth_devs == NULL) {
		libusb_free_device_list(usb_list, 1);
		return NULL;
	}

	for(i = 0 ; i < usb_devs ; ++i) {
		r = libusb_get_device_descriptor(usb_list[i], &desc);
		if(r < 0) {
			/* desc was not filled in: skip this device rather than test
			 * whatever the previous iteration left in it. */
			if (!quiet)
				fprintf(stderr, "couldn't get usb descriptor for dev #%d!\n", i);
			continue;
		}
		if ((desc.idVendor == TC13_VENDORID && desc.idProduct == TC13_PRODUCTID)
		    || (desc.idVendor == U0_VENDORID && desc.idProduct == U0_PRODUCTID)
		    || (desc.idVendor == U1_VENDORID && desc.idProduct == U1_PRODUCTID))
		{
			ubertooth_devs[ut_ct] = i;
			ut_ct++;
		}
	}
	/* Only a selected entry of ubertooth_devs[] is ever used as an index into
	 * usb_list[]; an index that is not in the list selects nothing. */
	pick = pick_ubertooth_device(ubertooth_device, ut_ct);

	if (quiet) {
		if (pick >= 0 && libusb_open(usb_list[ubertooth_devs[pick]], &devh) != 0)
			devh = NULL;
		free(ubertooth_devs);
		libusb_free_device_list(usb_list, 1);
		return devh;
	}
	if (pick >= 0) {
		ret = libusb_open(usb_list[ubertooth_devs[pick]], &devh);
		if (ret) {
			show_libusb_error(ret);
			devh = NULL;
		}
	}
	else if (ut_ct == 0) {
		/* nothing attached: the caller reports that it found no device */
	}
	else if (ubertooth_device < 0) {
		fprintf(stderr, "multiple Ubertooth devices found! Use '-U' to specify device number\n");
		uint8_t serial[17], sr;
		for(i = 0 ; i < ut_ct ; ++i) {
			libusb_get_device_descriptor(usb_list[ubertooth_devs[i]], &desc);
			ret = libusb_open(usb_list[ubertooth_devs[i]], &devh);
			if (ret) {
				fprintf(stderr, "  Device %d: ", i);
				show_libusb_error(ret);
			}
			else {
				sr = cmd_get_serial(devh, serial);
				if(sr==0) {
					fprintf(stderr, "  Device %d: ", i);
					print_serial(serial, stderr);
				}
				libusb_close(devh);
			}
		}
		devh = NULL;
	}
	else {
		fprintf(stderr, "error: no Ubertooth at index %d (%d attached)\n"
		        INDEX_SHIFT_HINT, ubertooth_device, ut_ct);
	}
	free(ubertooth_devs);
	libusb_free_device_list(usb_list,1);
	return devh;
}

static struct libusb_device_handle* find_ubertooth_device(int ubertooth_device)
{
	return find_ubertooth_device_opt(ubertooth_device, 0);
}

/* A serial number as printed by -s: 16 bytes as lowercase hex, plus the NUL. */
#define SERIAL_STR_LEN 33

/* Reads one device's serial number as lowercase hex, the form -s prints.
 * Returns 0 on success.
 *
 * This is the same read as cmd_get_serial() without its error messages,
 * because matching a serial means reading every attached device: a board that
 * is still starting, or wedged, must not print an error of its own while
 * another board is being selected. timeout_ms keeps a silent board from
 * stalling the caller. */
static int read_serial_string(struct libusb_device_handle* devh,
                              char out[SERIAL_STR_LEN], unsigned timeout_ms)
{
	uint8_t serial[17] = {0};
	int sr, j;

	sr = libusb_control_transfer(devh,
	        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN,
	        UBERTOOTH_GET_SERIAL, 0, 0, serial, 17, timeout_ms);
	if (sr != 17 || serial[0] != 0)
		return -1;
	for (j = 1; j < 17; j++)
		sprintf(&out[(j - 1) * 2], "%02x", serial[j]);
	return 0;
}

/* Does a -U serial string select this board? The string matches the whole
 * serial or its start (a prefix), compared case-insensitively because -s
 * prints lowercase hex while a serial copied from elsewhere may be upper case.
 * Ubertooth serials differ in their leading digits and share their trailing
 * ones, so only the start is matched; the caller refuses a string that matches
 * more than one board instead of opening whichever enumerated first. */
static int serial_matches(const char* query, const char* serial)
{
	size_t ql = strlen(query), sl = strlen(serial);

	if (ql == 0 || ql > sl)
		return 0;
	return strncasecmp(query, serial, ql) == 0;
}

/* Reports the attached Ubertooth devices a serial scan could not read, which
 * changes what its result means: the string the user gave may well be the
 * serial of one of them. A board that could not be opened at all usually means
 * a permissions problem (missing udev rules on Linux), so the first open error
 * is shown with the count. */
static void report_unscanned(int unopened, int open_err, int unread)
{
	if (unopened) {
		fprintf(stderr, "%d attached Ubertooth device%s could not be opened\n",
		        unopened, unopened == 1 ? "" : "s");
		if (open_err)
			show_libusb_error(open_err);
	}
	if (unread)
		fprintf(stderr, "%d attached Ubertooth device%s did not answer the "
		        "serial read\n", unread, unread == 1 ? "" : "s");
}

/* Find an Ubertooth by serial-number string (hex, as printed by -s), matched
 * by serial_matches(). Every attached device is read before one is chosen, so
 * a string that matches several is refused with the matching serials instead
 * of selecting one of them at random. Returns an open handle or NULL. */
static struct libusb_device_handle* find_ubertooth_device_serial_opt(
		const char* serial_number, int quiet)
{
	struct libusb_device **usb_list = NULL;
	struct libusb_context *ctx = NULL;
	struct libusb_device_handle *devh = NULL;
	struct libusb_device_descriptor desc;
	char (*matched_serials)[SERIAL_STR_LEN] = NULL;
	int *matched_devs = NULL;
	int usb_devs, i, r, ret, matches = 0, unread = 0, unopened = 0, open_err = 0;

	/* Hidden devices: nothing to match, so no board is opened and no serial
	 * is read; the "no Ubertooth found" report below is unchanged. */
	usb_devs = devices_hidden(!quiet) ? 0 : libusb_get_device_list(ctx, &usb_list);
	if (usb_devs < 0) {
		/* A bus that could not be listed is not the same as a serial that is
		 * not attached, so do not report it as one. */
		if (!quiet) {
			fprintf(stderr, "error: could not list USB devices: ");
			show_libusb_error(usb_devs);
		}
		return NULL;
	}
	if (usb_devs == 0) {
		libusb_free_device_list(usb_list, 1);
		if (!quiet)
			fprintf(stderr, "no Ubertooth found with serial matching '%s'\n",
			        serial_number);
		return NULL;
	}
	matched_devs = calloc(usb_devs, sizeof(int));
	matched_serials = calloc(usb_devs, sizeof(*matched_serials));
	if (matched_devs == NULL || matched_serials == NULL) {
		free(matched_devs);
		free(matched_serials);
		libusb_free_device_list(usb_list, 1);
		return NULL;
	}

	for (i = 0; i < usb_devs; ++i) {
		char serial_c[SERIAL_STR_LEN] = {0};

		r = libusb_get_device_descriptor(usb_list[i], &desc);
		if (r < 0)
			continue; /* desc was not filled in: skip this device */
		if (!((desc.idVendor == TC13_VENDORID && desc.idProduct == TC13_PRODUCTID)
		      || (desc.idVendor == U0_VENDORID && desc.idProduct == U0_PRODUCTID)
		      || (desc.idVendor == U1_VENDORID && desc.idProduct == U1_PRODUCTID)))
			continue;
		ret = libusb_open(usb_list[i], &devh);
		if (ret || devh == NULL) {
			/* Reported with the result, not here: an error printed mid-scan
			 * would belong to no board the user asked for. The first code is
			 * kept because the cause is usually the same for every board,
			 * missing udev rules being the common one. */
			if (unopened == 0)
				open_err = ret;
			unopened++;
			devh = NULL;
			continue;
		}
		/* The quiet caller polls a device that may be re-enumerating, so it
		 * waits briefly; the loud path keeps cmd_get_serial()'s timeout. */
		r = read_serial_string(devh, serial_c, quiet ? 200 : 1000);
		/* Closed again whether it matched or not: the chosen device is
		 * reopened below, once it is known to be the only match. */
		libusb_close(devh);
		devh = NULL;
		if (r != 0) {
			unread++; /* reported with the result */
			continue;
		}
		if (serial_matches(serial_number, serial_c)) {
			matched_devs[matches] = i;
			memcpy(matched_serials[matches], serial_c, sizeof(serial_c));
			matches++;
		}
	}

	if (matches == 1) {
		ret = libusb_open(usb_list[matched_devs[0]], &devh);
		if (ret) {
			if (!quiet)
				show_libusb_error(ret);
			devh = NULL;
		}
	}
	else if (!quiet && matches > 1) {
		int same = 1;

		fprintf(stderr, "error: '%s' matches %d Ubertooth devices:",
		        serial_number, matches);
		for (i = 0; i < matches; ++i) {
			fprintf(stderr, "%s %s", i ? "," : "", matched_serials[i]);
			if (strcmp(matched_serials[0], matched_serials[i]) != 0)
				same = 0;
		}
		/* A longer string can only help while the matching serials differ:
		 * boards reporting one serial cannot be told apart by this selector. */
		if (same)
			fprintf(stderr, "\nthese Ubertooth devices report the same serial "
			        "number; select one by index with -U<index>\n");
		else
			fprintf(stderr, "\nselect one of them by a longer prefix of its "
			        "serial number\n");
		report_unscanned(unopened, open_err, unread);
	}
	else if (!quiet) {
		fprintf(stderr, "no Ubertooth found with serial matching '%s'\n",
		        serial_number);
		report_unscanned(unopened, open_err, unread);
	}
	free(matched_devs);
	free(matched_serials);
	libusb_free_device_list(usb_list, 1);
	return devh;
}

static struct libusb_device_handle* find_ubertooth_device_serial(char* serial_number)
{
	return find_ubertooth_device_serial_opt(serial_number, 0);
}

/*
 * based on http://libusb.sourceforge.net/api-1.0/group__asyncio.html#ga9fcb2aa23d342060ebda1d0cf7478856
 */
static void rx_xfer_status(int status)
{
	char *error_name = "";

	switch (status) {
		case LIBUSB_TRANSFER_ERROR:
			error_name="Transfer error.";
			break;
		case LIBUSB_TRANSFER_TIMED_OUT:
			error_name="Transfer timed out.";
			break;
		case LIBUSB_TRANSFER_CANCELLED:
			error_name="Transfer cancelled.";
			break;
		case LIBUSB_TRANSFER_STALL:
			error_name="Halt condition detected, or control request not supported.";
			break;
		case LIBUSB_TRANSFER_NO_DEVICE:
			error_name="Device disconnected.";
			break;
		case LIBUSB_TRANSFER_OVERFLOW:
			error_name="Device sent more data than requested.";
			break;
	}
	fprintf(stderr,"rx_xfer status: %s (%d)\n",error_name,status);
}

static void cb_xfer(struct libusb_transfer *xfer)
{
	int r;
	ubertooth_t* ut = (ubertooth_t*)xfer->user_data;

	if (xfer->status != LIBUSB_TRANSFER_COMPLETED) {
		if(xfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
			r = libusb_submit_transfer(ut->rx_xfer);
			if (r < 0)
				fprintf(stderr, "Failed to submit USB transfer (%d)\n", r);
			return;
		}
		if(xfer->status != LIBUSB_TRANSFER_CANCELLED)
			rx_xfer_status(xfer->status);
		libusb_free_transfer(xfer);
		ut->rx_xfer = NULL;
		return;
	}

	if(ut->stop_ubertooth)
		return;

	fifo_inc_write_ptr(ut->fifo);
	ut->rx_xfer->buffer = (uint8_t*)fifo_get_write_element(ut->fifo);

	r = libusb_submit_transfer(ut->rx_xfer);
	if (r < 0)
		fprintf(stderr, "Failed to submit USB transfer (%d)\n", r);
}

static void* poll_thread_main(void* arg __attribute__((unused)))
{
	int r = 0;

	while (!do_exit) {
		struct timeval tv = { 1, 0 };
		r = libusb_handle_events_timeout(NULL, &tv);
		if (r < 0) {
			do_exit = 1;
			break;
		}
		usleep(1);
	}

	return NULL;
}

int ubertooth_bulk_thread_start()
{
	do_exit = 0;

	return pthread_create(&poll_thread, NULL, poll_thread_main, NULL);
}

void ubertooth_bulk_thread_stop()
{
	do_exit = 1;

	pthread_join(poll_thread, NULL);
}

int ubertooth_bulk_init(ubertooth_t* ut)
{
	int r;

	ut->rx_xfer = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(ut->rx_xfer, ut->devh, DATA_IN, (uint8_t*)fifo_get_write_element(ut->fifo), PKT_LEN, cb_xfer, ut, TIMEOUT);

	r = libusb_submit_transfer(ut->rx_xfer);
	if (r < 0) {
		fprintf(stderr, "rx_xfer submission: %d\n", r);
		return -1;
	}
	return 0;
}

void ubertooth_bulk_wait(ubertooth_t* ut)
{
	while (fifo_empty(ut->fifo) && !ut->stop_ubertooth)
		usleep(1);
}

int ubertooth_bulk_receive(ubertooth_t* ut, rx_callback cb, void* cb_args)
{
	if (!fifo_empty(ut->fifo)) {
		(*cb)(ut, cb_args);
		if(ut->stop_ubertooth) {
			if(ut->rx_xfer)
				libusb_cancel_transfer(ut->rx_xfer);
			return 1;
		}
		fflush(stderr);
		return 0;
	} else {
		usleep(1);
		return -1;
	}
}

/*
 * `start` is the request that puts the board in the mode whose packets the
 * caller wants: cmd_rx_syms() for plain symbol capture, cmd_afh() for AFH
 * detection. It is sent once the transfers are in place, and it is the last
 * mode request the board gets -- sending cmd_rx_syms() on top of cmd_afh()
 * left AFH detection receiving under MODE_RX_SYMBOLS instead.
 */
static int stream_rx_usb(ubertooth_t* ut, rx_callback cb, void* cb_args,
                         int (*start)(struct libusb_device_handle*))
{
	// init USB transfer
	int r = ubertooth_bulk_init(ut);
	if (r < 0)
		return r;

	r = ubertooth_bulk_thread_start();
	if (r < 0)
		return r;

	// tell ubertooth to send packets
	r = start(ut->devh);
	if (r < 0) {
		/* The poll thread is running by now and nothing else joins it:
		 * ubertooth_stop() frees the libusb context the thread is polling. */
		ubertooth_bulk_thread_stop();
		return r;
	}

	// receive and process each packet
	while(!ut->stop_ubertooth) {
		ubertooth_bulk_wait(ut);
		r = ubertooth_bulk_receive(ut, cb, cb_args);
	}

	ubertooth_bulk_thread_stop();

	return 1;
}

/* Why a replay stopped short of a whole record: a read error (minus its
 * errno), a last record cut short (the bytes of it that were there), or a
 * clean end of file (0). */
static int stream_rx_file_end(FILE* fp, size_t partial)
{
	if (ferror(fp))
		return -(errno ? errno : EIO);
	return (int)partial;
}

/* file should be in full USB packet format (ubertooth-dump -f) */
int stream_rx_file(ubertooth_t* ut, FILE* fp, rx_callback cb, void* cb_args)
{
	uint8_t buf[PKT_LEN];
	size_t nbytes;

	while(1) {
		uint32_t systime_be;
		errno = 0;
		nbytes = fread(&systime_be, 1, sizeof(systime_be), fp);
		if (nbytes != sizeof(systime_be))
			return stream_rx_file_end(fp, nbytes);
		systime = (time_t)be32toh(systime_be);

		errno = 0;
		nbytes = fread(buf, 1, PKT_LEN, fp);
		if (nbytes != PKT_LEN)
			return stream_rx_file_end(fp, sizeof(systime_be) + nbytes);
		fifo_push(ut->fifo, (usb_pkt_rx*)buf);
		(*cb)(ut, cb_args);
	}
}

int rx_file_report(int result, const char* filename)
{
	if (result < 0) {
		fprintf(stderr, "error: reading %s failed: %s\n",
		        filename, strerror(-result));
		return 1;
	}
	if (result > 0) {
		fprintf(stderr, "error: %s ends in a partial record: %d of %d "
		        "bytes, ignored\n", filename, result, RX_FILE_RECORD_LEN);
		return 1;
	}
	return 0;
}


/* 1 when not one channel has been seen, which is what the initial detection
 * phase leaves when no packet from the piconet was received: the map starts
 * empty (btbb_piconet_new() zeroes it) and cb_afh_initial() sets a channel
 * seen for every packet it matches. An all-zero map is not a map: printing it
 * claims a piconet that uses no channel at all. */
static int afh_map_empty(btbb_piconet* pn)
{
	uint8_t* afh_map = btbb_piconet_get_afh_map(pn);
	int i;

	for (i = 0; i < AFH_MAP_BYTES; i++)
		if (afh_map[i] != 0)
			return 0;
	return 1;
}

int rx_afh(ubertooth_t* ut, btbb_piconet* pn, int timeout)
{
	int r = btbb_init(max_ac_errors);
	if (r < 0)
		return -1;

	cmd_set_channel(ut->devh, 9999);

	if (timeout) {
		ubertooth_set_timeout(ut, timeout);

		/* cmd_afh() both starts the stream and is the mode it streams in:
		 * it clears the board's map, turns on AFH hopping and asks for
		 * MODE_AFH. */
		r = stream_rx_usb(ut, cb_afh_initial, pn, cmd_afh);

		cmd_stop(ut->devh);
		ut->stop_ubertooth = 0;

		/* A phase that could not even receive has not shown that the piconet
		 * is quiet; stream_rx_usb() has already reported why. */
		if (r < 0)
			return r;

		/* Nothing was received: there is no map to print, and monitoring
		 * changes to a map we never found would report nothing either. Let
		 * the caller say so instead. */
		if (afh_map_empty(pn))
			return UBERTOOTH_AFH_NO_PACKETS;

		btbb_print_afh_map(pn);
	}

	/*
	 * Monitor changes in AFH channel map. cmd_afh() clears the board's map
	 * as it starts the stream, so the monitor begins from an empty one
	 * whether or not the detection phase above ran.
	 */
	r = stream_rx_usb(ut, cb_afh_monitor, pn, cmd_afh);
	return r < 0 ? r : 0;
}

void rx_afh_r(ubertooth_t* ut, btbb_piconet* pn, int timeout __attribute__((unused)))
{
	static uint32_t lasttime;

	int r = btbb_init(max_ac_errors);
	int i, j;
	if (r < 0)
		return;

	cmd_set_channel(ut->devh, 9999);

	// init USB transfer
	r = ubertooth_bulk_init(ut);
	if (r < 0)
		return;

	r = ubertooth_bulk_thread_start();
	if (r < 0)
		return;

	// tell ubertooth to send packets, in the AFH detection mode (see rx_afh)
	r = cmd_afh(ut->devh);
	if (r < 0) {
		/* as in stream_rx_usb(): the poll thread has to be joined here */
		ubertooth_bulk_thread_stop();
		return;
	}

	// receive and process each packet
	while(!ut->stop_ubertooth) {
		ubertooth_bulk_receive(ut, cb_afh_r, pn);
		if(lasttime < time(NULL)) {
			lasttime = time(NULL);
			printf("%u ", (uint32_t)time(NULL));
			// btbb_print_afh_map(pn);

			uint8_t* afh_map = btbb_piconet_get_afh_map(pn);
			for (i=0; i<AFH_MAP_BYTES; i++)
				for (j=0; j<8; j++)
					if (afh_map[i] & (1<<j))
						printf("1");
					else
						printf("0");
			printf("\n");
		}
	}

	ubertooth_bulk_thread_stop();
}

int rx_btle_file(FILE* fp)
{
	ubertooth_t* ut = ubertooth_init();
	if (ut == NULL)
		return -ENOMEM;

	return stream_rx_file(ut, fp, cb_btle, NULL);
}

void ubertooth_unpack_symbols(const uint8_t* buf, char* unpacked)
{
	int i, j;

	for (i = 0; i < SYM_LEN; i++) {
		/* output one byte for each received symbol (0x00 or 0x01) */
		for (j = 0; j < 8; j++) {
			unpacked[i * 8 + j] = ((buf[i] << j) & 0x80) >> 7;
		}
	}
}

static void cb_dump_bitstream(ubertooth_t* ut, void* args __attribute__((unused)))
{
	int i;
	char nl = '\n';

	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;
	char bitstream[BANK_LEN];
	ubertooth_unpack_symbols((uint8_t*)rx->data, bitstream);

	// convert to ascii
	for (i = 0; i < BANK_LEN; ++i)
		bitstream[i] += 0x30;

	fprintf(stderr, "rx block timestamp %u * 100 nanoseconds\n",
	        rx->clk100ns);
	if (dumpfile == NULL) {
		fwrite(bitstream, sizeof(uint8_t), BANK_LEN, stdout);
		fwrite(&nl, sizeof(uint8_t), 1, stdout);
	} else {
		fwrite(bitstream, sizeof(uint8_t), BANK_LEN, dumpfile);
		fwrite(&nl, sizeof(uint8_t), 1, dumpfile);
	}
}

static void cb_dump_full(ubertooth_t* ut, void* args __attribute__((unused)))
{
	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;

	fprintf(stderr, "rx block timestamp %u * 100 nanoseconds\n", rx->clk100ns);
	/* A replayed record keeps the host time it was recorded with, which
	 * stream_rx_file() left in systime; a live one is stamped now. */
	if (infile == NULL)
		systime = time(NULL);
	uint32_t time_be = htobe32(systime);
	if (dumpfile == NULL) {
		fwrite(&time_be, 1, sizeof(time_be), stdout);
		fwrite((uint8_t*)rx, sizeof(uint8_t), PKT_LEN, stdout);
	} else {
		fwrite(&time_be, 1, sizeof(time_be), dumpfile);
		fwrite((uint8_t*)rx, sizeof(uint8_t), PKT_LEN, dumpfile);
		fflush(dumpfile);
	}
}

/* dump received symbols to stdout */
void rx_dump(ubertooth_t* ut, int bitstream)
{
	if (bitstream)
		stream_rx_usb(ut, cb_dump_bitstream, NULL, cmd_rx_syms);
	else
		stream_rx_usb(ut, cb_dump_full, NULL, cmd_rx_syms);
}

static void ubertooth_cleanup(ubertooth_t* ut, int send_stop)
{
	/* make sure xfers are not active */
	if(ut->rx_xfer != NULL)
		libusb_cancel_transfer(ut->rx_xfer);
	if (ut->devh != NULL) {
		if (send_stop)
			cmd_stop(ut->devh);
		libusb_release_interface(ut->devh, 0);
		libusb_close(ut->devh);
		ut->devh = NULL;
	}
	libusb_exit(NULL);

	if (ut->h_pcap_bredr) {
		btbb_pcap_close(ut->h_pcap_bredr);
		ut->h_pcap_bredr = NULL;
	}
	if (ut->h_pcap_le) {
		lell_pcap_close(ut->h_pcap_le);
		ut->h_pcap_le = NULL;
	}

	if (ut->h_pcapng_bredr) {
		btbb_pcapng_close(ut->h_pcapng_bredr);
		ut->h_pcapng_bredr = NULL;
	}
	if (ut->h_pcapng_le) {
		lell_pcapng_close(ut->h_pcapng_le);
		ut->h_pcapng_le = NULL;
	}
}

void ubertooth_close(ubertooth_t* ut)
{
	ubertooth_cleanup(ut, 0);
}

void ubertooth_stop(ubertooth_t* ut)
{
	ubertooth_cleanup(ut, 1);
}

void ubertooth_free(ubertooth_t* ut)
{
	if (ut == NULL)
		return;
	free(ut->fifo);
	free(ut);
}

ubertooth_t* ubertooth_init()
{
	ubertooth_t* ut = (ubertooth_t*)malloc(sizeof(ubertooth_t));
	if(ut == NULL) {
		fprintf(stderr, "Unable to allocate memory\n");
		return NULL;
	}

	ut->fifo = fifo_init();
	if(ut->fifo == NULL)
		fprintf(stderr, "Unable to initialize ringbuffer\n");

	ut->devh = NULL;
	ut->rx_xfer = NULL;
	ut->stop_ubertooth = 0;
	ut->abs_start_ns = 0;
	ut->start_clk100ns = 0;
	ut->last_clk100ns = 0;
	ut->clk100ns_upper = 0;
	ut->clk100ns_started = 0;
	ut->clk100ns_period = CLK100NS_PERIOD;

	ut->h_pcap_bredr = NULL;
	ut->h_pcap_le = NULL;
	ut->h_pcapng_bredr = NULL;
	ut->h_pcapng_le = NULL;

	return ut;
}

int ubertooth_connect(ubertooth_t* ut, int ubertooth_device)
{
	int r = libusb_init(NULL);
	if (r < 0) {
		fprintf(stderr, "libusb_init failed (got 1.0?)\n");
		return -1;
	}

	ut->devh = find_ubertooth_device(ubertooth_device);
	if (ut->devh == NULL) {
		fprintf(stderr, "could not open Ubertooth device\n");
		ubertooth_stop(ut);
		return -1;
	}

	r = libusb_claim_interface(ut->devh, 0);
	if (r < 0) {
		fprintf(stderr, "usb_claim_interface error %d\n", r);
		ubertooth_stop(ut);
		return -1;
	}

	return 1;
}

ubertooth_t* ubertooth_start(int ubertooth_device)
{
	ubertooth_t* ut = ubertooth_init();
	if (ut == NULL)
		return NULL;

	int r = ubertooth_connect(ut, ubertooth_device);
	if (r < 0) {
		/* ubertooth_connect() already called ubertooth_stop() to close the
		 * device and any pcap handles; release the memory allocated by
		 * ubertooth_init() so the failed-connect path does not leak. */
		ubertooth_free(ut);
		return NULL;
	}

	return ut;
}

int ubertooth_connect_serial(ubertooth_t* ut, char* serial_number)
{
	int r = libusb_init(NULL);
	if (r < 0) {
		fprintf(stderr, "libusb_init failed (got 1.0?)\n");
		return -1;
	}

	ut->devh = find_ubertooth_device_serial(serial_number);
	if (ut->devh == NULL) {
		ubertooth_stop(ut);
		return -1;
	}

	r = libusb_claim_interface(ut->devh, 0);
	if (r < 0) {
		fprintf(stderr, "usb_claim_interface error %d\n", r);
		ubertooth_stop(ut);
		return -1;
	}

	return 1;
}

ubertooth_t* ubertooth_start_serial(char* serial_number)
{
	ubertooth_t* ut = ubertooth_init();
	if (ut == NULL)
		return NULL;

	int r = ubertooth_connect_serial(ut, serial_number);
	if (r < 0) {
		ubertooth_free(ut);
		return NULL;
	}

	return ut;
}

/* What a -U argument asks for. */
enum selector_kind {
	SELECTOR_DEFAULT,  /* no -U given: the only attached device */
	SELECTOR_INDEX,    /* an enumeration index */
	SELECTOR_SERIAL,   /* a serial-number string */
	SELECTOR_NO_INDEX  /* all digits, but no device can have that index */
};

/* Classify a -U argument, writing the index for SELECTOR_INDEX. An all-digits
 * argument is an index, anything else is matched as a serial number. A number
 * that does not fit in an int cannot be an attached device's index, and must
 * not be converted into one that is: atoi() would read 4294967296 as index 0
 * and 2147483648 as a negative index, which means "the only device". */
static enum selector_kind classify_selector(const char* u_arg, int* index)
{
	const char* p;
	long value;

	if (u_arg == NULL || u_arg[0] == '\0')
		return SELECTOR_DEFAULT;
	for (p = u_arg; *p != '\0'; ++p)
		if (*p < '0' || *p > '9')
			return SELECTOR_SERIAL;

	/* The scan above leaves only digits, so strtol() stops at the end of the
	 * string and cannot return a negative value; only the range can fail. */
	errno = 0;
	value = strtol(u_arg, NULL, 10);
	if (errno != 0 || value > INT_MAX)
		return SELECTOR_NO_INDEX;
	*index = (int)value;
	return SELECTOR_INDEX;
}

/* The same message as the loud selection path, for an index that could not
 * even be a device index. The count comes from a separate enumeration, so a
 * libusb that cannot be used at all (ubertooth_count() < 0, which reports
 * itself) is left out rather than reported as "0 attached". */
static void report_no_index(const char* u_arg)
{
	int attached = ubertooth_count();

	if (attached < 0)
		fprintf(stderr, "error: no Ubertooth at index %s\n"
		        INDEX_SHIFT_HINT, u_arg);
	else
		fprintf(stderr, "error: no Ubertooth at index %s (%d attached)\n"
		        INDEX_SHIFT_HINT, u_arg, attached);
}

/* Shared -U argument handling for all tools: accepts either a numeric device
 * index (existing behaviour) or a serial-number string. A NULL or empty arg
 * means "default device". An all-digits arg is treated as an index; anything
 * else is matched as a serial number. */
ubertooth_t* ubertooth_start_select(const char* u_arg)
{
	int index = -1;

	switch (classify_selector(u_arg, &index)) {
	case SELECTOR_DEFAULT:
		return ubertooth_start(-1);
	case SELECTOR_INDEX:
		return ubertooth_start(index);
	case SELECTOR_NO_INDEX:
		report_no_index(u_arg);
		return NULL;
	case SELECTOR_SERIAL:
	default:
		/* cast away const: the serial path does not modify the string */
		return ubertooth_start_serial((char*)u_arg);
	}
}

/* Quiet variant of ubertooth_start_select(), for polling a device that may be
 * absent, for example while it re-enumerates after a reset. Prints nothing and
 * sends no command other than the serial-number read needed to match a serial.
 * If the device is not found or cannot be claimed it is closed WITHOUT sending
 * STOP, so a failed attempt leaves the device state unchanged. An index is
 * chosen as in ubertooth_start_select(): an index that is not present opens
 * nothing. */
ubertooth_t* ubertooth_start_select_quiet(const char* u_arg)
{
	struct libusb_device_handle* devh;
	ubertooth_t* ut;
	enum selector_kind kind;
	int index = -1;

	kind = classify_selector(u_arg, &index);
	if (kind == SELECTOR_NO_INDEX)
		return NULL; /* quiet: no message, and nothing to open */

	if (libusb_init(NULL) < 0)
		return NULL;
	if (kind == SELECTOR_DEFAULT)
		devh = find_ubertooth_device_opt(-1, 1);
	else if (kind == SELECTOR_INDEX)
		devh = find_ubertooth_device_opt(index, 1);
	else
		devh = find_ubertooth_device_serial_opt(u_arg, 1);
	if (devh == NULL || libusb_claim_interface(devh, 0) < 0) {
		if (devh != NULL)
			libusb_close(devh);
		libusb_exit(NULL);
		return NULL;
	}

	ut = (ubertooth_t*)calloc(1, sizeof(ubertooth_t));
	if (ut != NULL)
		ut->fifo = fifo_init();
	if (ut == NULL || ut->fifo == NULL) {
		free(ut);
		libusb_release_interface(devh, 0);
		libusb_close(devh);
		libusb_exit(NULL);
		return NULL;
	}
	ut->devh = devh;
	return ut;
}

/* Connect-variant of the shared -U handling, for tools that use the
 * ubertooth_init() + ubertooth_connect() pattern rather than ubertooth_start().
 * Returns >=0 on success, <0 on failure (matching ubertooth_connect). */
int ubertooth_connect_select(ubertooth_t* ut, const char* u_arg)
{
	int index = -1;

	switch (classify_selector(u_arg, &index)) {
	case SELECTOR_DEFAULT:
		return ubertooth_connect(ut, -1);
	case SELECTOR_INDEX:
		return ubertooth_connect(ut, index);
	case SELECTOR_NO_INDEX:
		report_no_index(u_arg);
		return -1;
	case SELECTOR_SERIAL:
	default:
		return ubertooth_connect_serial(ut, (char*)u_arg);
	}
}

int ubertooth_get_api(ubertooth_t *ut, uint16_t *version) {
	int result;
	libusb_device* dev;
	struct libusb_device_descriptor desc;
	dev = libusb_get_device(ut->devh);
	result = libusb_get_device_descriptor(dev, &desc);
	if (result < 0) {
		if (result == LIBUSB_ERROR_PIPE) {
			fprintf(stderr, "control message unsupported\n");
		} else {
			show_libusb_error(result);
		}
		return result;
	}
	*version = desc.bcdDevice;
	return 0;
}

int ubertooth_check_api(ubertooth_t *ut) {
	uint16_t version;
	int result;
	result = ubertooth_get_api(ut, &version);
	if (result < 0) {
		return result;
	}

	/* Host and firmware are released together: any other version, older or
	 * newer, is refused rather than run with behaviour the host does not
	 * expect. */
	if (version != UBERTOOTH_API_VERSION) {
		fprintf(stderr, "error: Ubertooth firmware API version %x.%02x found, libubertooth %s requires %x.%02x.\n",
				(version>>8)&0xFF, version&0xFF, VERSION,
				(UBERTOOTH_API_VERSION>>8)&0xFF, UBERTOOTH_API_VERSION&0xFF);
		if (version < UBERTOOTH_API_VERSION)
			fprintf(stderr, "Flash the firmware built from the same source tree, for example: ./install.sh --flash-device all\n");
		else
			fprintf(stderr, "Install the host tools built from the same source tree as the firmware.\n");
		ubertooth_stop(ut);
		return -1;
	}
	return 0;
}
