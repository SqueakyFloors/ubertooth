/* -*- c -*- */
/*
 * Copyright 2015 Dominic Spill
 *
 * This file is part of Project Ubertooth
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
 * along with libbtbb; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>  // fchmod, umask
#include <time.h>
#include <unistd.h>  // sleep, unlink, close
#include "ubertooth.h"
#include "dfu.h"
#include "dfu_transition.h"
#include "dfu_recovery.h"
#include "opt_parse.h"

static struct timespec operation_started;

static void start_operation_timer(void)
{
	clock_gettime(CLOCK_MONOTONIC, &operation_started);
}

static double operation_elapsed_seconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - operation_started.tv_sec) +
	       (double)(now.tv_nsec - operation_started.tv_nsec) / 1000000000.0;
}

/* True while a row of progress dots is half-written, waiting for its
 * newline. */
static int progress_row_open;

/* A dot of progress for one block transferred. */
static void progress_dot(void)
{
	fputc('.', stderr);
	progress_row_open = 1;
}

/* Finish a row of dots, if one is open. The dots share standard error with
 * every message, so a line printed while a row is open would start in the
 * middle of it ("....error: ..."), which no reader can match against the
 * start of a line. Call this before printing anything else. */
static void end_progress_row(void)
{
	if (progress_row_open) {
		fputc('\n', stderr);
		progress_row_open = 0;
	}
}

static void log_dfu_stage(const char *stage)
{
	end_progress_row();
	fprintf(stderr, "DFU +%.3fs: %s\n", operation_elapsed_seconds(), stage);
	fflush(stderr);
}

static void exit_dfu_context(libusb_context *context, int reset_requested)
{
#if defined(__APPLE__)
	if (reset_requested) {
		/* The Darwin backend's final libusb_exit() waits for a global
		 * CFRunLoop thread that can fail to terminate after device reset.
		 * All handles are already closed and this process exits immediately,
		 * so process teardown safely reclaims the remaining context. */
		(void)context;
		log_dfu_stage("libusb context exit skipped after macOS device reset");
		return;
	}
#endif
	log_dfu_stage("libusb context exit started");
	libusb_exit(context);
	log_dfu_stage("libusb context exit returned");
}

/*
 * DFU suffix / signature / CRC
 */

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)(value & 0xff);
	bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value & 0xff);
	bytes[1] = (uint8_t)((value >> 8) & 0xff);
	bytes[2] = (uint8_t)((value >> 16) & 0xff);
	bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* The largest unsigned .bin -s will read: signing adds the suffix, so an
 * input capped here produces an image -d still accepts. */
static unsigned long max_unsigned_image_size(void)
{
	return dfu_max_image_size() - DFU_SUFFIX_LENGTH;
}

/* Refuse a file this tool would read into memory whole, before it allocates
 * anything from the size: a file past maximum bytes, or one with no size a
 * read can be sized from at all. The message names the file and says
 * what the limit applies to, which `what` supplies. Returns 1 when the file
 * is refused. */
static int image_file_refused(const char* name, FILE* file,
                              unsigned long maximum, const char* what)
{
	long long size = 0;

	switch (dfu_image_size_check(file, maximum, &size)) {
	case DFU_FILE_SIZE_OK:
		return 0;
	case DFU_FILE_SIZE_TOO_LARGE:
		fprintf(stderr, "error: %s is %lld bytes; %s is at most %lu\n",
		        name, size, what, maximum);
		return 1;
	case DFU_FILE_SIZE_NOT_REGULAR:
		fprintf(stderr, "error: %s is not a regular file\n", name);
		return 1;
	default:
		fprintf(stderr, "error: cannot determine the size of %s\n", name);
		return 1;
	}
}

/* Add a DFU suffix naming idVendor/idProduct to infile and write to outfile.
 * The whole input is read into memory, so it has already been through
 * image_file_refused(): that runs before any output exists, so a refused input
 * leaves nothing behind. outfile is the temporary file the caller renamed over
 * the target only once this returned 0, so a failure here leaves an earlier
 * file of that name as it was. in_name and out_name are the paths the user
 * gave, so every refusal here names the file it is about, as the man page
 * says they all do. */
int sign(const char* in_name, FILE* infile, const char* out_name,
         FILE* outfile, uint16_t idVendor, uint16_t idProduct) {
	uint8_t* suffix;
	long file_length;
	size_t data_length, buffer_length;
	uint8_t* buffer;

	fseek(infile, 0, SEEK_END);
	file_length = ftell(infile);
	if (file_length < 0) {
		fprintf(stderr, "error: cannot determine the size of %s\n", in_name);
		return 1;
	}
	/* The buffer is sized from this read rather than from the guard in
	 * main(), so the cap is applied to it as well: a file a writer is still
	 * growing cannot slip past between the two. */
	if ((unsigned long)file_length > max_unsigned_image_size()) {
		fprintf(stderr,
		        "error: %s is %ld bytes; the firmware in an "
		        "Ubertooth image is at most %lu\n",
		        in_name, file_length, max_unsigned_image_size());
		return 1;
	}
	buffer_length = (size_t)file_length + DFU_SUFFIX_LENGTH;
	buffer = malloc(buffer_length);
	if(buffer == NULL) {
		fprintf(stderr, "error: cannot allocate %zu bytes to sign %s\n",
		        buffer_length, in_name);
		return 1;
	}

	fseek(infile, 0, SEEK_SET);
	data_length = fread(buffer, 1, (size_t)file_length, infile);
	if (data_length != (size_t)file_length) {
		fprintf(stderr, "error: read %zu of %ld byte(s) from %s\n",
		        data_length, file_length, in_name);
		free(buffer);
		return 1;
	}
	buffer_length = data_length + DFU_SUFFIX_LENGTH;

	suffix = buffer + data_length;
	write_le16(suffix + DFU_SUFFIX_OFF_BCDDEVICE, 0);
	write_le16(suffix + DFU_SUFFIX_OFF_IDPRODUCT, idProduct);
	write_le16(suffix + DFU_SUFFIX_OFF_IDVENDOR, idVendor);
	write_le16(suffix + DFU_SUFFIX_OFF_BCDDFU, DFU_VERSION_1_0);
	suffix[DFU_SUFFIX_OFF_UCDFUSIG + 0] = 0x55;
	suffix[DFU_SUFFIX_OFF_UCDFUSIG + 1] = 0x46;
	suffix[DFU_SUFFIX_OFF_UCDFUSIG + 2] = 0x44;
	suffix[DFU_SUFFIX_OFF_BLENGTH] = DFU_SUFFIX_LENGTH;
	write_le32(suffix + DFU_SUFFIX_OFF_DWCRC,
	           dfu_image_crc32(buffer, buffer_length - 4));

	if (fwrite(buffer, 1, buffer_length, outfile) != buffer_length) {
		fprintf(stderr, "error: could not write the whole of %s\n", out_name);
		free(buffer);
		return 1;
	}
	free(buffer);
	return 0;
}

/* Check the image before the board is asked to leave its application, so a
 * file this tool will not write costs the user nothing. Prints what is wrong
 * and returns 1, or 0 when the image can be written to this device. On that
 * path *image_size is the length the check measured, which is what the flash
 * a write covers is worked out from; it is left alone when the image is
 * refused. */
static int check_download_image(const char* name, FILE* downfile,
                                struct ubertooth_fw_info* info,
                                unsigned long* image_size)
{
	struct dfu_image_report image_report;
	char message[160];
	int found;

	/* Progress, so standard error, where the refusal below goes too: the two
	 * then stay in order whatever either stream is piped into. */
	fputs("Checking firmware signature\n", stderr);
	if (dfu_image_check(downfile, U1_DFU_VENDORID, U1_DFU_PRODUCTID,
	                    &image_report) != DFU_IMAGE_OK) {
		dfu_image_describe_failure(&image_report, message, sizeof(message));
		/* One line: what is wrong with the file, and that the board was
		 * left alone. No device has been opened and no output file exists
		 * yet. */
		fprintf(stderr, "error: %s: %s; firmware will not be updated\n",
		        name, message);
		return 1;
	}
	/* The check has read the whole file, so its length is settled here and
	 * the flash a write covers can be worked out from it. */
	*image_size = (unsigned long)image_report.image_size;
	/* Every image from this tree carries a firmware info block at a fixed
	 * offset. A DFU file that has none is some other device's image, or a
	 * file that happens to carry a valid suffix, so it is refused here --
	 * before the board is asked to leave its application, and so before
	 * anything is erased. */
	found = dfu_read_image_info(downfile, info);
	if (found != UBERTOOTH_FW_INFO_OK) {
		/* One line, like the signature refusal above it: what is wrong with
		 * the file, and that the board was left alone. */
		fprintf(stderr, "error: %s: %s at offset 0x%x; firmware will not be "
		        "updated\n", name, ubertooth_fw_info_result_string(found),
		        UBERTOOTH_FW_INFO_OFFSET);
		return 1;
	}
	/* What is about to be written: a note about the run, so standard error,
	 * where the revision lines after it already go. */
	fprintf(stderr, "Image: %s %s (API:%x.%02x)\n", info->image,
	        info->revision, (info->api_version >> 8) & 0xFF,
	        info->api_version & 0xFF);
	/* The tools and the board have to report the same API version, so an
	 * image built for another one is worth saying out loud. It is a warning
	 * and not a refusal: this command has to be able to write any valid
	 * image, including the one that undoes such a flash.
	 *
	 * What the warning reports is what the file says about itself. It does
	 * not promise what the other tools will do with the board afterwards,
	 * because they read the USB bcdDevice rather than this block. Both
	 * images this tree builds put the same version in both places, so the
	 * two agree today; nothing in the format makes them have to. */
	if (info->api_version != UBERTOOTH_API_VERSION)
		fprintf(stderr, "warning: %s: this image reports API version "
		        "%x.%02x in its firmware info block, and these tools report "
		        "%x.%02x; the tools and the board have to report the same "
		        "one\n", name,
		        (info->api_version >> 8) & 0xFF, info->api_version & 0xFF,
		        (UBERTOOTH_API_VERSION >> 8) & 0xFF,
		        UBERTOOTH_API_VERSION & 0xFF);
	return 0;
}

/*
 * USB helpers for binding the application and DFU identities to one physical
 * bus/port path. DFU mode has no serial descriptor, so path matching prevents
 * a different attached Ubertooth from being selected after re-enumeration.
 */
static int get_usb_identity(libusb_device *device,
                            struct dfu_usb_identity *identity)
{
	int port_count;

	memset(identity, 0, sizeof(*identity));
	identity->bus_number = libusb_get_bus_number(device);
	port_count = libusb_get_port_numbers(device, identity->port_numbers,
	                                    DFU_USB_PATH_MAX);
	if (port_count < 0)
		return port_count;
	identity->port_count = (uint8_t)port_count;
	return 0;
}

/* Return 1 when a matching DFU device is opened, 0 when absent, or a negative
 * libusb error. With no expected path, fail rather than choosing ambiguously.
 * A NULL devh asks only whether such a device is there, which is how the
 * detach tells a board that reset from one still in the bootloader: the
 * matching rules are the ones above, not a second copy of them. */
static int find_ubertooth_dfu_device(
	libusb_context *ctx,
	const struct dfu_usb_identity *expected_identity,
	libusb_device_handle **devh)
{
	libusb_device **usb_list = NULL;
	libusb_device *match = NULL;
	struct libusb_device_descriptor desc;
	struct dfu_usb_identity identity;
	ssize_t usb_devs;
	int i, result;

	if (devh != NULL)
		*devh = NULL;
	/* This search is the tool's own, so it honours the test switch itself:
	 * hidden devices mean none is found, and the bus is not listed. */
	if (ubertooth_devices_hidden())
		return 0;
	usb_devs = libusb_get_device_list(ctx, &usb_list);
	if (usb_devs < 0)
		return (int)usb_devs;

	for (i = 0; i < usb_devs; ++i) {
		result = libusb_get_device_descriptor(usb_list[i], &desc);
		if (result < 0)
			continue;
		if (!((desc.idVendor == TC13_VENDORID &&
		       desc.idProduct == TC13_PRODUCTID) ||
		      (desc.idVendor == U1_DFU_VENDORID &&
		       desc.idProduct == U1_DFU_PRODUCTID)))
			continue;

		result = get_usb_identity(usb_list[i], &identity);
		if (result < 0)
			goto done;
		if (expected_identity != NULL &&
		    !dfu_usb_identity_equal(expected_identity, &identity))
			continue;
		if (match != NULL) {
			result = LIBUSB_ERROR_BUSY;
			goto done;
		}
		match = usb_list[i];
	}

	if (match == NULL) {
		result = 0;
		goto done;
	}
	if (devh == NULL) {
		result = 1;
		goto done;
	}
	result = libusb_open(match, devh);
	if (result == 0)
		result = 1;

done:
	libusb_free_device_list(usb_list, 1);
	return result;
}

/* Return 1 when the application device on the expected path is opened, 0 when
 * absent, DFU_OPEN_BUS_UNREADABLE when the bus could not be listed at all, or
 * a negative libusb error for a device that is there and would not open. The
 * three are reported differently, so they are kept apart here.
 *
 * bus_error, which may not be NULL, is written either way: the libusb error
 * from a listing that failed, or 0. The outcome alone cannot carry it, and it
 * belongs in the stage trace with the rest of this command's codes. */
static int find_ubertooth_application_device(
	libusb_context *ctx,
	const struct dfu_usb_identity *expected_identity,
	libusb_device_handle **devh,
	int *bus_error)
{
	libusb_device **usb_list = NULL;
	struct libusb_device_descriptor desc;
	struct dfu_usb_identity identity;
	ssize_t usb_devs;
	int i, result;

	*devh = NULL;
	*bus_error = 0;
	/* As in find_ubertooth_dfu_device(): the switch hides the bus from this
	 * search too. */
	if (ubertooth_devices_hidden())
		return 0;
	usb_devs = libusb_get_device_list(ctx, &usb_list);
	if (usb_devs < 0) {
		*bus_error = (int)usb_devs;
		return DFU_OPEN_BUS_UNREADABLE;
	}

	result = 0;
	for (i = 0; i < usb_devs; ++i) {
		result = libusb_get_device_descriptor(usb_list[i], &desc);
		if (result < 0)
			continue;
		if (!((desc.idVendor == TC13_VENDORID &&
		       desc.idProduct == TC13_PRODUCTID) ||
		      (desc.idVendor == U0_VENDORID &&
		       desc.idProduct == U0_PRODUCTID) ||
		      (desc.idVendor == U1_VENDORID &&
		       desc.idProduct == U1_PRODUCTID)))
			continue;
		result = get_usb_identity(usb_list[i], &identity);
		if (result < 0)
			goto done_app;
		if (!dfu_usb_identity_equal(expected_identity, &identity))
			continue;
		result = libusb_open(usb_list[i], devh);
		if (result == 0)
			result = 1;
		else if (result == LIBUSB_ERROR_NO_DEVICE)
			/* The device left the bus between the listing above and this
			 * open, so it is absent rather than one that would not open.
			 * The caller polls, so a board still re-enumerating is looked
			 * for again. */
			result = 0;
		goto done_app;
	}
	result = 0;

done_app:
	libusb_free_device_list(usb_list, 1);
	return result;
}

static void close_dfu_device(libusb_device_handle *devh, int claimed)
{
	if (devh == NULL)
		return;
	if (claimed) {
		log_dfu_stage("DFU interface release started");
		libusb_release_interface(devh, 0);
		log_dfu_stage("DFU interface release returned");
	}
	log_dfu_stage("DFU handle close started");
	libusb_close(devh);
	log_dfu_stage("DFU handle close returned");
}

/* How long cleanup looks for the DFU device again before giving up on
 * returning the board to its application. */
#define DETACH_REOPEN_POLLS 20
#define DETACH_REOPEN_INTERVAL_MS 100

/* How long the detach waits for the DFU device to go away before it calls the
 * board still stuck in the bootloader. A board that reset left the bus within
 * 180 ms of the request failing on the macOS host this was measured on, so
 * this budget is about five times what it takes; the wait is only ever spent
 * in full when the detach really did fail. Too short a budget would bring
 * back the failure this is here to fix, so it is generous on purpose.
 *
 * A request the bootloader completes before resetting rides the same budget,
 * and its window starts later than the measured one: the board is still
 * answering when the transfer returns, where a failed transfer was already
 * racing the reset. The measurement has not been repeated there, because the
 * macOS host it was taken on never completes the request. Five times the
 * measured departure is meant to cover that too. */
#define DETACH_GONE_POLLS 20
#define DETACH_GONE_INTERVAL_MS 50

/* One listing that does not find the DFU device is not enough to call the
 * board detached, so every absence is confirmed by a second listing this long
 * afterwards.
 *
 * A board whose bootloader entry pin is still bridged resets straight back
 * into the bootloader, and is off the bus only for as long as it takes to
 * come up and be enumerated again: a confirming listing one ordinary poll
 * later would still land inside that gap and read the same as a board that
 * left. Four flashes on a lab board had the board back on the bus 400-630 ms
 * after it was asked to reset, so 1.5 s is about two and a half times the
 * slowest of them. It is spent once per detach, and only once the device has
 * gone, so a flash pays it and a board still in the bootloader does not.
 *
 * The confirming listing is tried this many times, at the ordinary poll
 * interval, so one listing that cannot answer does not decide it. The gap is
 * waited once however many of these it takes: the wait is what the answer
 * needs, and repeating it would be the only thing in here that is not
 * bounded by a listing. */
#define DETACH_CONFIRM_INTERVAL_MS 1500
#define DETACH_CONFIRM_POLLS 3

/* Held as values rather than used as constants so the hardware-independent
 * tests can shorten the wait: the cases where a detach fails would otherwise
 * sleep out the whole budget four times over, which is most of the time that
 * suite takes. Nothing in the tool changes them.
 *
 * The confirming interval is the exception: the tests keep it at the shipped
 * value, because the stage line it is formatted into is only as long as it
 * ships at when the number is. Sleeping it costs them nothing, since they
 * replace the sleep itself. */
static unsigned detach_gone_polls = DETACH_GONE_POLLS;
static unsigned detach_gone_interval_ms = DETACH_GONE_INTERVAL_MS;
static unsigned detach_confirm_interval_ms = DETACH_CONFIRM_INTERVAL_MS;

struct dfu_runtime_context {
	libusb_context *usb_context;
	FILE *download_file;
	FILE *upload_file;
	uint32_t upload_length;  /* bytes the read-back asks the bootloader for */
	const char *upload_name;       /* the dump -u was given, for its messages */
	const char *download_name;     /* the image -d was given */
	/* A detach saw the DFU device leave the bus, so the reset happened and
	 * the handle it answered on is stale. Cleanup has nothing to add by
	 * resetting the board again: it would drive a handle the board no longer
	 * has, and report the same thing a second time. */
	int board_left_the_bus;
	int download_completed;  /* the whole image reached the flash */
	unsigned long download_length; /* ...and the flash writing it covers */
	int download_checked;    /* the image has been put against the part */
	int download_refused;    /* ...and does not fit the flash it has */
	int want_part_id;        /* a transfer needs the part ID; a reset does not */
	uint32_t part_id;        /* the part ID read from the application */
	int part_id_read;        /* ...and whether it was read at all */
	unsigned app_attempts;
	unsigned dfu_polls;
	unsigned transfer_attempts;
	unsigned verify_polls;
	const struct dfu_usb_identity *verify_identity;
	/* The libusb error from the last listing the verification could not
	 * read, or 0. The verification's own report says which outcome it was;
	 * this is the raw code behind an unreadable bus, which is reported in
	 * the stage trace like every other code this command sees. */
	int verify_bus_error;
};

/* Does the image -d was given fit the flash the attached part has?
 *
 * The write goes to the application region from 0x4000 upwards. A bootloader
 * built from this tree refuses a block with no flash behind it
 * (firmware/bootloader/dfu.cpp, request_dnload), so an oversize image stops
 * at the top of the part's flash rather than reporting a programming failure.
 * Either way the blocks below that are already written, and the board is left
 * with an application missing its tail: all the host sees of the refusal is
 * the stall, which it reports as the block the board would not take. Older
 * bootloaders have no upper bound at all. That is why this is worth refusing
 * here, where the image and the part are both known, rather than reporting
 * whatever comes back from the board.
 *
 * It is the same cause as the read-back bound: asking for flash the part has
 * not got. On a bootloader from this tree both paths end the same way, with
 * ERRADDRESS before the part is touched. On an older one they do not: a read
 * past the end is a bus fault that takes the bootloader down until the board
 * is power-cycled, while a write past the end stops part-way through the
 * application.
 *
 * The part identification number says how much flash there is; it is readable
 * only while the board still runs its application, which is where this is
 * called from, so a refusal costs the user nothing.
 *
 * Returns 1 when the image is refused, having said why. A part this host
 * cannot identify gives no figure to check against, so that warns and lets
 * the download go ahead: the assumed size is not a floor, and refusing a
 * valid image for a part nobody knows would be worse. Says its piece once,
 * however many entry attempts the transition makes. */
static int download_refused_for_part(struct dfu_runtime_context *runtime)
{
	const struct dfu_part *part = runtime->part_id_read ?
		dfu_part_lookup(runtime->part_id) : NULL;
	uint32_t address = BOOTLOADER_OFFSET + BOOTLOADER_SIZE;
	uint32_t region;

	if (runtime->download_checked)
		return runtime->download_refused;
	runtime->download_checked = 1;
	if (part == NULL) {
		if (runtime->part_id_read)
			fprintf(stderr, "warning: part 0x%08x is not known here, so %s "
			        "is not checked against this board's flash\n",
			        runtime->part_id, runtime->download_name);
		else
			fprintf(stderr, "warning: the part could not be read, so %s is "
			        "not checked against this board's flash\n",
			        runtime->download_name);
		fflush(stderr);
		return 0;
	}
	region = dfu_upload_length(part->flash_size);
	if (runtime->download_length > region) {
		/* One line, like the image check: what does not fit, and that the
		 * board was left alone. It has not been asked for DFU yet. */
		fprintf(stderr, "error: %s needs %lu bytes of flash; part 0x%08x "
		        "(%s) has %u above the bootloader; firmware will not be "
		        "updated\n", runtime->download_name,
		        runtime->download_length, runtime->part_id, part->name,
		        region);
		runtime->download_refused = 1;
		return 1;
	}
	/* Progress, so standard error, next to the warning and the refusal above.
	 * Said in the future tense: this runs before the board has been asked for
	 * DFU, so an entry that then fails must not have claimed a write had
	 * started. The read-back's line comes from plan_upload(), which runs
	 * once the bootloader has answered. */
	fprintf(stderr, "The image needs %lu bytes, 0x%x-0x%lx: part 0x%08x (%s) "
	        "has %u KiB of flash\n", runtime->download_length, address,
	        (unsigned long)address + runtime->download_length - 1,
	        runtime->part_id, part->name, part->flash_size / 1024);
	fflush(stderr);
	return 0;
}

static int open_application(void *context, const char *selector, void **handle,
                            struct dfu_usb_identity *identity)
{
	struct dfu_runtime_context *runtime = context;
	char stage[64];
	ubertooth_t *ut;
	int result;

	runtime->app_attempts++;
	snprintf(stage, sizeof(stage), "opening application device (attempt %u)",
	         runtime->app_attempts);
	log_dfu_stage(stage);
	ut = ubertooth_start_select(selector);

	if (ut == NULL)
		return LIBUSB_ERROR_NO_DEVICE;
	result = get_usb_identity(libusb_get_device(ut->devh), identity);
	if (result < 0) {
		ubertooth_stop(ut);
		ubertooth_free(ut);
		return result;
	}
	/* Both transfers need the part ID, which says how much flash there is:
	 * what a read-back may ask the bootloader for, and what a download may
	 * write. The bootloader has no request that reports it, so it has to be
	 * read here, while the application is still answering. The read has its
	 * own error output and timeout, so it is not sent for a command that
	 * would ignore the answer, and a board that will not say keeps the
	 * assumed size rather than failing the command. */
	if (!runtime->want_part_id) {
		runtime->part_id_read = 0;
	} else if (cmd_get_part_id(ut->devh, &runtime->part_id) == 0) {
		runtime->part_id_read = 1;
		snprintf(stage, sizeof(stage), "part ID read: 0x%08x",
		         runtime->part_id);
		log_dfu_stage(stage);
	} else {
		runtime->part_id_read = 0;
		log_dfu_stage("part ID could not be read");
	}
	/* An image too long for this part is refused here rather than between
	 * the erase and the write: the transition stops before it asks the board
	 * for DFU, so the board keeps running its firmware. */
	if (runtime->download_name != NULL &&
	    download_refused_for_part(runtime) != 0) {
		log_dfu_stage("image refused: the board keeps its application");
		ubertooth_stop(ut);
		ubertooth_free(ut);
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	*handle = ut;
	return 0;
}

static int request_dfu(void *context __attribute__((unused)), void *handle)
{
	ubertooth_t *ut = handle;
	int result = cmd_flash(ut->devh);
	if (result == 0) {
		log_dfu_stage("DFU entry request accepted");
		fprintf(stderr, "Switching selected device to DFU mode...\n");
		fflush(stderr);
	}
	return result;
}

static void close_application(void *context __attribute__((unused)),
                              void *handle)
{
	ubertooth_t *ut = handle;
	/* Do not call ubertooth_stop() here: its UBERTOOTH_STOP request can replace
	 * the pending MODE_RESET set by cmd_flash() before the firmware main loop
	 * acts on it. Close the handle without sending another device command. */
	ubertooth_close(ut);
	ubertooth_free(ut);
	log_dfu_stage("application handle closed without STOP");
}

static int open_matching_dfu(void *context,
                             const struct dfu_usb_identity *identity,
                             void **handle)
{
	struct dfu_runtime_context *runtime = context;
	char stage[64];
	int result;

	runtime->dfu_polls++;
	result = find_ubertooth_dfu_device(
		runtime->usb_context, identity, (libusb_device_handle **)handle);
	if (result == 1) {
		snprintf(stage, sizeof(stage), "matching DFU device found (poll %u)",
		         runtime->dfu_polls);
		log_dfu_stage(stage);
	}
	return result;
}

static void transition_sleep_ms(void *context __attribute__((unused)),
                                unsigned milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000U,
		.tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
	};
	struct timespec remaining;

	while (nanosleep(&delay, &remaining) < 0 && errno == EINTR)
		delay = remaining;
}

static const struct dfu_transition_ops TRANSITION_OPS = {
	.open_application = open_application,
	.request_dfu = request_dfu,
	.close_application = close_application,
	.open_matching_dfu = open_matching_dfu,
	.sleep_ms = transition_sleep_ms,
};

/*
 * DFU functions
 */
#define DFU_IN LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE
#define DFU_OUT LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE

/* One report for a DFU request the bootloader refused. A stall is the only
 * answer a refusal gives, and the shared wording names what was asked for;
 * anything else is a transport failure and is left to show_libusb_error().
 * No firmware API version comes with it: the firmware refusing the request is
 * the bootloader, which is not the application image an API version belongs
 * to, so there is none to quote. */
static void report_dfu_failure(const char* what, int code)
{
	if (code == LIBUSB_ERROR_PIPE)
		ubertooth_report_unsupported_request(what, code);
	else
		show_libusb_error(code);
}

/* The same, for the requests that carry a block number: a refusal here is
 * usually a block with no flash behind it, so the line has to say which. */
static void report_dfu_block_failure(const char* what, int block, int code)
{
	char text[32];

	snprintf(text, sizeof(text), "%s %d", what, block);
	report_dfu_failure(text, code);
}

int dfu_get_state(libusb_device_handle* devh) {
	int rv;
	uint8_t state;
	rv = libusb_control_transfer(devh, DFU_IN, REQ_GETSTATE, 0, 0, &state, 1, 1000);
	if (rv < 0) {
		report_dfu_failure("read the DFU state", rv);
		return rv;
	}
	return state;
}

int dfu_clear_status(libusb_device_handle* devh) {
	int rv;
	rv = libusb_control_transfer(devh, DFU_OUT, REQ_CLRSTATUS, 0, 0, NULL, 0, 1000);
	if (rv < 0) {
		report_dfu_failure("clear the DFU status", rv);
		return rv;
	}
	return 0;
}

int dfu_abort(libusb_device_handle* devh) {
	int rv;
	rv = libusb_control_transfer(devh, DFU_OUT, REQ_ABORT, 0, 0, NULL, 0, 1000);
	if (rv < 0) {
		report_dfu_failure("abort the DFU operation", rv);
		return rv;
	}
	return 0;
}

/* The state machine of enter_dfu_mode() runs in dfu_reach_idle(); these are
 * its operations over libusb. */
static int idle_get_state(void *context __attribute__((unused)), void *handle)
{
	return dfu_get_state(handle);
}

static int idle_abort(void *context __attribute__((unused)), void *handle)
{
	return dfu_abort(handle);
}

static int idle_clear_status(void *context __attribute__((unused)),
                             void *handle)
{
	return dfu_clear_status(handle);
}

static const struct dfu_idle_ops IDLE_OPS = {
	.get_state = idle_get_state,
	.abort = idle_abort,
	.clear_status = idle_clear_status,
	.sleep_ms = transition_sleep_ms,
};

/* Wait, bounded, for the bootloader to be idle, so a board that answers
 * nothing ends the command with a message instead of spinning. Returns 0, or
 * a negative enum dfu_idle_result. */
int enter_dfu_mode(libusb_device_handle* devh) {
	const struct dfu_idle_config config = {
		.max_polls = DFU_IDLE_DEFAULT_MAX_POLLS,
		.poll_interval_ms = DFU_IDLE_DEFAULT_POLL_INTERVAL_MS,
	};
	struct dfu_idle_report report;
	int rv;

	rv = dfu_reach_idle(&IDLE_OPS, NULL, devh, &config, &report);
	if (rv != DFU_IDLE_OK) {
		fprintf(stderr, "error: DFU gave up: %s (polls=%u, state=%d, usb_error=%d)\n",
		        dfu_idle_result_string(report.result), report.polls,
		        report.last_state, report.operation_error);
		return rv;
	}
	return 0;
}

/* What a look for the DFU device on the path it was asked to leave found.
 * Anything negative is a libusb error, so these are not negative. */
#define DFU_PRESENCE_GONE 0      /* absent on two listings in a row */
#define DFU_PRESENCE_STAYED 1    /* there for the whole wait */
#define DFU_PRESENCE_RETURNED 2  /* it went, and the bootloader came back */

/* Is the DFU device still on the path it was asked to leave? Returns one of
 * the three above, or a negative libusb error when no listing could tell. A
 * reset takes a moment to reach the bus, so this waits for the device to
 * disappear rather than reading one list, and a listing that fails costs that
 * poll rather than the answer.
 *
 * Absence is only believed when a second listing confirms it. One listing
 * cannot tell a board that left from one that left and came straight back:
 * the bootloader is absent for as long as re-enumeration takes either way.
 * The confirming listing is far enough out that a board back in the
 * bootloader is answering again by then.
 *
 * *went says whether any listing found the device gone, which the caller
 * needs on the error paths: once the device has left, the status of the
 * detach request says nothing, however the rest of the wait turned out. */
static int dfu_device_presence(struct dfu_runtime_context *runtime,
                               const struct dfu_usb_identity *identity,
                               int *went)
{
	int last_error = LIBUSB_ERROR_OTHER;
	int seen = 0;
	unsigned poll;

	*went = 0;
	/* Without a path there is nothing to look for, and no device matches an
	 * empty one: say so rather than reporting a board that was never looked
	 * for as gone. Both callers pass the path the command bound itself to,
	 * so this is defence. */
	if (identity == NULL || identity->port_count == 0)
		return LIBUSB_ERROR_INVALID_PARAM;
	for (poll = 0; poll < detach_gone_polls; poll++) {
		int result = find_ubertooth_dfu_device(runtime->usb_context, identity,
		                                       NULL);
		if (result == 1)
			seen = 1;
		else if (result < 0)
			last_error = result;
		if (result == 0) {
			/* Gone once. What settles it is the listing after the gap a
			 * re-entering board would be back from, so the wait happens
			 * here and the answer is whatever is on the path afterwards. A
			 * listing that fails is retried a few times rather than waiting
			 * the gap again: the one wait is what the answer needs, and the
			 * retries keep a single failed listing from deciding it. */
			unsigned confirm;
			char stage[128];

			*went = 1;
			snprintf(stage, sizeof(stage),
			         "the DFU device has gone: looking again in %u ms, in "
			         "case the board came back in the bootloader",
			         detach_confirm_interval_ms);
			log_dfu_stage(stage);
			transition_sleep_ms(runtime, detach_confirm_interval_ms);
			for (confirm = 0; confirm < DETACH_CONFIRM_POLLS; confirm++) {
				result = find_ubertooth_dfu_device(runtime->usb_context,
				                                   identity, NULL);
				if (result == 0)
					return DFU_PRESENCE_GONE;
				/* One listing is enough the other way round: a device that
				 * never went away cannot turn up in a list it was missing
				 * from, so finding it here means it really did leave and
				 * come back. */
				if (result == 1)
					return DFU_PRESENCE_RETURNED;
				last_error = result;
				if (confirm + 1 < DETACH_CONFIRM_POLLS)
					transition_sleep_ms(runtime, detach_gone_interval_ms);
			}
			/* No listing after the gap could tell, so nothing here can. */
			return last_error;
		}
		if (poll + 1 < detach_gone_polls)
			transition_sleep_ms(runtime, detach_gone_interval_ms);
	}
	return seen ? DFU_PRESENCE_STAYED : last_error;
}

/* Ask the board to leave the bootloader and run its application again.
 *
 * What the request returned does not say where the board ended up, in either
 * direction. The bootloader acts on REQ_DETACH by resetting, so the transfer
 * is answered by a device that is going away: on macOS it ends in PIPE on
 * every flash that worked, and the backends have their own codes for the same
 * race. A request that completes is no better evidence, because a bootloader
 * is free to acknowledge it and then stay where it is. So no status is read
 * as a verdict either way, and what settles it is whether the DFU device is
 * still on its path afterwards: a bootloader that refused the request, or
 * accepted it and did nothing, is still there, and one that acted on it is
 * not.
 *
 * That holds for the Ubertooth USB identities, where the application and the
 * bootloader are told apart by their IDs. It does not hold for the
 * unprogrammed-EEPROM prototype ID, which both device searches match in
 * either mode: such a board matches the search again as soon as it is back on
 * the bus, whichever mode it came up in, so a reset that worked reads as a
 * board that came back in the bootloader and -r cannot pass. The limitation
 * is the probe's, and telling the two identities apart would be the fix, but
 * nothing here can do that from a bus and port path alone. Such a board is a
 * prototype this fork does not support.
 *
 * Returns 0 once the DFU device has gone, and a negative error when the board
 * is still in the bootloader or that could not be established. */
int detach(struct dfu_runtime_context* runtime, libusb_device_handle* devh,
           const struct dfu_usb_identity* identity) {
	int rv, presence, went;
	rv = enter_dfu_mode(devh);
	if(rv < 0) {
		fprintf(stderr, "error: detach failed: could not enter DFU mode\n");
		return rv;
	}
	rv = libusb_control_transfer(devh, DFU_OUT, REQ_DETACH, 0, 0, NULL, 0, 1000);
	log_dfu_stage(rv == LIBUSB_SUCCESS ?
	              "detach request accepted: looking for the DFU device to see "
	              "whether the board left the bootloader" :
	              "detach request did not complete: looking for the DFU "
	              "device to see whether the board reset");
	presence = dfu_device_presence(runtime, identity, &went);
	if (presence != DFU_PRESENCE_GONE) {
		/* A board that never left makes the status of the request
		 * diagnostic after all. Once a listing has found the device gone it
		 * is not: the reset happened, and a request that failed while it was
		 * in flight is what a reset that worked looks like on macOS. Naming
		 * a status there would put a libusb line in front of a failure that
		 * has nothing to do with it. */
		if (rv != LIBUSB_SUCCESS && !went)
			show_libusb_error(rv);
		runtime->board_left_the_bus = went;
		if (presence < 0)
			fprintf(stderr, "error: detach failed: could not tell whether "
			        "the board left DFU (usb_error=%d)\n", presence);
		else if (presence == DFU_PRESENCE_RETURNED) {
			fprintf(stderr, "error: detach failed: the board reset and came "
			        "back in the bootloader\n");
			/* The flash already holds the image, so say it before the
			 * causes: the reader's next move after a bare failure is to
			 * write it again, which is the one thing this board does not
			 * need. */
			if (runtime->download_completed)
				fprintf(stderr, "note: the firmware was written, so the "
				        "board holds it; it is in the bootloader rather "
				        "than running it\n");
			/* Why it is there. The bootloader runs instead of the
			 * application in exactly two cases
			 * (firmware/bootloader/bootloader.cpp, main()): the word a tool
			 * sets to ask for DFU is set in the memory that survives a
			 * reset, or the entry pin reads grounded as the board comes up.
			 * The bootloader clears that word on its way out, so for it to
			 * be the cause here something has to have set it again, which is
			 * how the note puts it. The next line is the remedy for the
			 * other condition, which is the one that repeats on every
			 * reset. */
			fprintf(stderr, "note: the bootloader runs instead of the "
			        "application when something asks the board into DFU "
			        "again, or when its bootloader entry pin reads grounded "
			        "as the board resets\n");
			fprintf(stderr, "note: if the expansion-header bridge is still "
			        "fitted, remove it (pins 1 and 3 of P4 on an Ubertooth "
			        "One), then unplug the board and plug it back in\n");
		} else
			fprintf(stderr, "error: detach failed: the board is still in DFU\n");
		/* A request that failed carries its own status, except where that
		 * status is the reset it asked for racing it. One the bootloader
		 * accepted and ignored has none, so the failure comes from the probe:
		 * why it could not answer, when it could not, and otherwise a code
		 * that at least is not success. */
		if (rv != LIBUSB_SUCCESS && !went)
			return rv;
		return presence < 0 ? presence : LIBUSB_ERROR_OTHER;
	}
	log_dfu_stage("the DFU device has gone: the board left the bootloader");
	/* Progress: the reset the user asked for is work done, not something
	 * read back, so it is reported where the rest of the trace is. */
	fprintf(stderr, "Detached\n");
	return 0;
}

/* Read length bytes of the application flash region into upfile. The caller
 * settles length from the part's flash size: the bootloader faults on a read
 * with no flash behind it instead of refusing it, so this must never be asked
 * for more than the part has. */
int upload(libusb_device_handle* devh, FILE* upfile, uint32_t length) {
	int address, block, rv;
	uint8_t buffer[BLOCK_SIZE];
	address = BOOTLOADER_OFFSET + BOOTLOADER_SIZE;
	block = address / BLOCK_SIZE;

	if ((address & (BLOCK_SIZE - 1)) != 0) {
		fprintf(stderr, "error: upload failed: must start at block boundary\n");
		return -1;
	}
	if (length == 0 || (length & (BLOCK_SIZE - 1)) != 0) {
		fprintf(stderr, "error: upload failed: %u bytes is not a whole number "
		        "of %d-byte blocks\n", length, BLOCK_SIZE);
		return -1;
	}

	rv = enter_dfu_mode(devh);
	if(rv < 0) {
		fprintf(stderr, "error: upload failed: could not enter DFU mode\n");
		return rv;
	}

	while(length > 0) {
		rv = libusb_control_transfer(devh, DFU_IN, REQ_UPLOAD, block, 0,
		                             buffer, BLOCK_SIZE, 1000);
		if (rv < 0) {
			end_progress_row();
			report_dfu_block_failure("read block", block, rv);
			fprintf(stderr, "error: upload failed: block %d was not read\n",
			        block);
			return rv;
		}
		if(rv != BLOCK_SIZE) {
			end_progress_row();
			fprintf(stderr, "error: upload failed: did not read full block\n");
			return -1;
		}
		/* A dot per block, on standard error with the rest of the progress:
		 * the dump itself is the result and goes to the file. */
		progress_dot();
		/* A short write would be renamed over the target as a truncated
		 * dump, so it ends the read-back too. */
		if(fwrite(buffer, 1, BLOCK_SIZE, upfile) != BLOCK_SIZE) {
			end_progress_row();
			fprintf(stderr, "error: upload failed: could not write the dump\n");
			return -1;
		}
		block++;
		length -= BLOCK_SIZE;
	}
	end_progress_row();
	return 0;
}

int dfu_get_status(libusb_device_handle* devh) {
	uint8_t buffer[6];
	int rv;
	rv = libusb_control_transfer(devh, DFU_IN, REQ_GETSTATUS, 0, 0, buffer, 6, 1000);
	if (rv < 0) {
		end_progress_row();
		report_dfu_failure("read the DFU status", rv);
		/* The reply carries the state the next block depends on, so a
		 * read that failed cannot be treated as status OK. */
		return rv;
	}
	return 0;
}

/* One block of the image, index counted from its start. A block the file ends
 * part way through is padded out with 0xFF -- what flash reads as once it is
 * erased, so the padding programs nothing. Returns 0 when the file would not
 * seek there or gave no bytes at all, which the caller treats as a read that
 * failed: how many blocks the image has is settled before any of this is
 * sent, so every index asked for here is one the file has. */
static int read_image_block(FILE* downfile, int index, uint8_t* buffer)
{
	size_t got;

	if (fseek(downfile, (long)index * BLOCK_SIZE, SEEK_SET) != 0)
		return 0;
	got = fread(buffer, 1, BLOCK_SIZE, downfile);
	if (got == 0)
		return 0;
	memset(buffer + got, 0xFF, BLOCK_SIZE - got);
	return 1;
}

/* Send one block to the bootloader and read the status it left behind. Both
 * have to succeed for the block to be in flash, so anything but 0 back from
 * here means the download stopped where it was. blocks_sent counts the blocks
 * that did go through, which is what the rows of progress dots are broken on.
 */
static int send_block(libusb_device_handle* devh, int block, uint8_t* buffer,
                      int* blocks_sent)
{
	int rv;

	rv = libusb_control_transfer(devh, DFU_OUT, REQ_DNLOAD, block, 0,
	                             buffer, BLOCK_SIZE, 1000);
	if (rv < 0) {
		end_progress_row();
		report_dfu_block_failure("write block", block, rv);
		return rv;
	}
	rv = dfu_get_status(devh);
	if (rv < 0)
		return rv;
	/* A dot per block written, on standard error with the rest of the
	 * progress, 40 to a row. */
	progress_dot();
	(*blocks_sent)++;
	if (*blocks_sent % 40 == 0)
		end_progress_row();
	return 0;
}

/* Where the LPC vector checksum sits and what it covers: the word at offset
 * 0x1c of the vector table is the two's complement of the seven words in
 * front of it, so all eight add up to zero (NXP LPC176x/5x user manual
 * UM10360, "Criterion for valid user code"). The bootloader reads the same
 * eight words (firmware/bootloader/app_image.h,
 * app_image_vector_checksum_ok). */
#define VECTOR_CHECKSUM_INDEX 7u

/* Fill in the checksum over the vector table in this block, so that the board
 * can tell a first block that reached the flash whole from one that did not.
 *
 * The word is NXP's, reserved for exactly this, and an image file from this
 * tree leaves it zero: the application's vector table does not fill it in
 * (firmware/common/LPC17xx_Interrupts.c) and no build step does either. So a
 * read-back of a board flashed by this tool differs from the image file in
 * this one word, which ubertooth-dfu(1) records.
 *
 * It is written over rather than checked, so that what goes to the board is
 * always a checksum the board will accept: an image carrying a wrong one
 * would otherwise be flashed and then refused at the next power-up. An image
 * carrying the right one is unchanged by this. */
static void fill_vector_checksum(uint8_t *block)
{
	uint32_t sum = 0;
	unsigned i;

	for (i = 0; i < VECTOR_CHECKSUM_INDEX; i++)
		sum += read_le32(block + i * 4);
	/* Two's complement, so the eight words add up to zero. Done in unsigned
	 * arithmetic: negating a signed value this large is not defined. */
	write_le32(block + VECTOR_CHECKSUM_INDEX * 4, 0u - sum);
}

/* Read block index of the image and send it to the block of flash that holds
 * it. Reports what went wrong and returns negative either way, so the caller
 * only has to stop. */
static int send_image_block(libusb_device_handle* devh, FILE* downfile,
                            int first_block, int index, int* blocks_sent)
{
	uint8_t buffer[BLOCK_SIZE];

	if (!read_image_block(downfile, index, buffer)) {
		end_progress_row();
		fprintf(stderr, "error: download failed: could not read block %d of "
		        "the image\n", index);
		return -1;
	}
	return send_block(devh, first_block + index, buffer, blocks_sent);
}

/* Write the image to the application region in an order that leaves nothing
 * startable behind if the download stops part way through.
 *
 * The region's first block holds the vector table: the initial stack pointer
 * and the reset vector that decide where a board goes when it is powered up.
 * Written first, as it used to be, it is in place for the whole of the
 * transfer, so a download that stops -- a cable pulled, a board that leaves
 * the bus, a host that dies -- leaves a board that starts an application
 * missing the rest of itself. It faults on every power-up from then on, and
 * only bridging the bootloader entry pin at the bench brings it back.
 *
 * So the vector table goes away first and is written last:
 *
 *   1. the first block is written as erased flash (0xFF), which leaves no
 *      stack pointer and no reset vector for anything to start from;
 *   2. every block above the first sector is written;
 *   3. the first sector is written, the vector table with it, and the
 *      checksum over that table filled in so the board can tell a first
 *      block that arrived whole from one that did not.
 *
 * Step 1 is also what erases the first sector, and that is why step 3 writes
 * the whole sector rather than the vector table on its own: the bootloader
 * erases a sector when the block at the start of it is written
 * (firmware/bootloader/flash.h, Flash::write), so a second write to the
 * region's first block would erase the fifteen blocks after it. A board in
 * the field has no request that programs without erasing, and this tool has
 * to keep working with the bootloader already on the board.
 *
 * That leaves one window, in step 3: an interruption after the vector table
 * and before the end of that 4 KiB sector still leaves a startable vector
 * table in front of a sector that is only partly written. Everything before
 * it -- all of a download bar its last few blocks -- now leaves a board whose
 * bootloader finds nothing to start and waits in DFU for another image.
 */
/* Step 3 writes SECTOR_SIZE worth of blocks and relies on that being the
 * whole of the sector the region starts in, so that the erase its first block
 * triggers is followed by the rest of that sector's contents. The family's
 * sectors are 4 KiB over the first 64 KiB and 32 KiB above that
 * (firmware/bootloader/flash.h), so a region starting at or above 64 KiB
 * would sit in a 32 KiB sector: the alignment check in download() would still
 * pass and step 3 would then leave 28 KiB erased behind a written vector
 * table. Today the region starts at 0x4000, well inside the 4 KiB range. */
_Static_assert(BOOTLOADER_OFFSET + BOOTLOADER_SIZE + SECTOR_SIZE <= 0x10000,
               "the application region must start in a SECTOR_SIZE sector");

int download(libusb_device_handle* devh, FILE* downfile) {
	const int first_block = (BOOTLOADER_OFFSET + BOOTLOADER_SIZE) / BLOCK_SIZE;
	const int sector_blocks = SECTOR_SIZE / BLOCK_SIZE;
	int address = BOOTLOADER_OFFSET + BOOTLOADER_SIZE;
	uint8_t buffer[BLOCK_SIZE];
	int blocks_sent = 0;
	int image_blocks, index, rv;
	long image_length;

	if ((address & (SECTOR_SIZE - 1)) != 0) {
		fprintf(stderr, "error: download failed: must start at sector boundary\n");
		return -1;
	}

	/* How many blocks there are to write, settled before anything is sent so
	 * that the order below can address them in any order it likes. The image
	 * has already been read whole and checked by this point, so a size that
	 * will not read here is a file that changed under the tool. */
	if (fseek(downfile, 0, SEEK_END) != 0 ||
	    (image_length = ftell(downfile)) <= 0) {
		fprintf(stderr, "error: download failed: could not measure the image\n");
		return -1;
	}
	image_blocks = (int)((image_length + BLOCK_SIZE - 1) / BLOCK_SIZE);

	rv = enter_dfu_mode(devh);
	if(rv < 0) {
		fprintf(stderr, "error: download failed: could not enter DFU mode\n");
		return rv;
	}

	/* Step 1: no vector table, so nothing is startable from here until step 3
	 * has finished. */
	memset(buffer, 0xFF, sizeof(buffer));
	rv = send_block(devh, first_block, buffer, &blocks_sent);
	if (rv < 0)
		return rv;

	/* Step 2: everything above the first sector. */
	for (index = sector_blocks; index < image_blocks; index++) {
		rv = send_image_block(devh, downfile, first_block, index,
		                      &blocks_sent);
		if (rv < 0)
			return rv;
	}

	/* Step 3: the first sector, the vector table first because the erase
	 * goes with it. The checksum over the vector table is filled in here, so
	 * the board has something to tell a whole first block from a partly
	 * written one. */
	if (!read_image_block(downfile, 0, buffer)) {
		end_progress_row();
		fprintf(stderr, "error: download failed: could not read block 0 of "
		        "the image\n");
		return -1;
	}
	fill_vector_checksum(buffer);
	rv = send_block(devh, first_block, buffer, &blocks_sent);
	if (rv < 0)
		return rv;
	for (index = 1; index < sector_blocks && index < image_blocks; index++) {
		rv = send_image_block(devh, downfile, first_block, index,
		                      &blocks_sent);
		if (rv < 0)
			return rv;
	}
	end_progress_row();
	return 0;
}

static int runtime_download(void *context, void *handle)
{
	struct dfu_runtime_context *runtime = context;
	char stage[64];
	runtime->transfer_attempts++;
	snprintf(stage, sizeof(stage), "firmware programming attempt %u started",
	         runtime->transfer_attempts);
	log_dfu_stage(stage);
	return download(handle, runtime->download_file);
}

static void runtime_close_dfu(void *context __attribute__((unused)),
                              void *handle, int claimed)
{
	close_dfu_device(handle, claimed);
}

static int runtime_reopen_dfu(void *context,
                              const struct dfu_usb_identity *identity,
                              void **handle)
{
	struct dfu_runtime_context *runtime = context;
	int result = find_ubertooth_dfu_device(
		runtime->usb_context, identity, (libusb_device_handle **)handle);
	if (result == 1)
		log_dfu_stage("same-path DFU device reopened after transfer failure");
	return result;
}

static int runtime_claim_dfu(void *context __attribute__((unused)), void *handle)
{
	int result = libusb_claim_interface(handle, 0);
	if (result == 0)
		log_dfu_stage("reopened DFU interface claimed");
	return result;
}

/* The read-back gets the same one bounded retry as programming: a single
 * REQ_UPLOAD was seen to fail a second after the board came back from the
 * bootloader, and the dump is read again from its first block. */
static int runtime_upload(void *context, void *handle)
{
	struct dfu_runtime_context *runtime = context;
	char stage[64];

	runtime->transfer_attempts++;
	snprintf(stage, sizeof(stage), "firmware read-back attempt %u started",
	         runtime->transfer_attempts);
	log_dfu_stage(stage);
	/* The retry starts at block zero, so what the failed attempt wrote must
	 * not stay in front of it. */
	if (fseek(runtime->upload_file, 0, SEEK_SET) != 0 ||
	    ftruncate(fileno(runtime->upload_file), 0) != 0) {
		/* The temporary file is the one that would not rewind, but the name
		 * the user gave is the one they can act on. */
		fprintf(stderr, "error: %s: %s\n", runtime->upload_name,
		        strerror(errno));
		return -1;
	}
	return upload(handle, runtime->upload_file, runtime->upload_length);
}

static const struct dfu_transfer_ops DOWNLOAD_TRANSFER_OPS = {
	.transfer = runtime_download,
	.close_dfu = runtime_close_dfu,
	.reopen_matching_dfu = runtime_reopen_dfu,
	.claim_dfu = runtime_claim_dfu,
	.sleep_ms = transition_sleep_ms,
};

static const struct dfu_transfer_ops UPLOAD_TRANSFER_OPS = {
	.transfer = runtime_upload,
	.close_dfu = runtime_close_dfu,
	.reopen_matching_dfu = runtime_reopen_dfu,
	.claim_dfu = runtime_claim_dfu,
	.sleep_ms = transition_sleep_ms,
};

/* One failed transfer, then the same device on the same path again. */
#define TRANSFER_MAX_ATTEMPTS 2
#define TRANSFER_REOPEN_POLLS 50
#define TRANSFER_REOPEN_INTERVAL_MS 100

static int verify_open_application(void *context,
                                   const char *selector __attribute__((unused)),
                                   void **handle,
                                   struct dfu_usb_identity *identity)
{
	struct dfu_runtime_context *runtime = context;
	libusb_device_handle *devh = NULL;
	char stage[64];
	int result;

	runtime->verify_polls++;
	snprintf(stage, sizeof(stage), "checking returned application (poll %u)",
	         runtime->verify_polls);
	log_dfu_stage(stage);
	result = find_ubertooth_application_device(
		runtime->usb_context, runtime->verify_identity, &devh,
		&runtime->verify_bus_error);
	/* Nothing there yet is not an error: the board is still re-enumerating,
	 * or it will not come back at all. Either way it is not a device that
	 * refused to open, and nor is a bus that could not be read; each is
	 * reported differently, so each goes back as itself. */
	if (result != 1)
		return result;
	result = libusb_claim_interface(devh, 0);
	if (result < 0) {
		libusb_close(devh);
		return result;
	}
	result = get_usb_identity(libusb_get_device(devh), identity);
	if (result < 0) {
		close_dfu_device(devh, 1);
		return result;
	}
	*handle = devh;
	return 1;
}

static int verify_get_revision(void *context __attribute__((unused)),
                               void *handle, char *revision,
                               size_t revision_size)
{
	return cmd_get_firmware_revision(handle, revision, revision_size) < 0 ? -1 : 0;
}

/* Which image the board is running, from the firmware info block it reports.
 * The revision above comes from its own request, which any firmware answers;
 * this one needs the block, so it is read separately and only once the
 * revision has already matched. */
static int verify_get_image(void *context __attribute__((unused)),
                            void *handle, char *image, size_t image_size)
{
	struct ubertooth_fw_info info;
	int r;

	if (image == NULL || image_size == 0)
		return LIBUSB_ERROR_INVALID_PARAM;
	image[0] = '\0';
	r = cmd_get_firmware_info(handle, &info);
	if (r < 0)
		return r;
	snprintf(image, image_size, "%s", info.image);
	return 0;
}

static void verify_close_application(void *context __attribute__((unused)),
                                     void *handle)
{
	close_dfu_device(handle, 1);
	log_dfu_stage("verified application handle closed");
}

static const struct dfu_verify_ops VERIFY_OPS = {
	.open_application = verify_open_application,
	.get_revision = verify_get_revision,
	.get_image = verify_get_image,
	.close_application = verify_close_application,
	.sleep_ms = transition_sleep_ms,
};

/* Find and claim the DFU device on the same physical path again, so a failure
 * that closed the handle can still be followed by the detach -r promised.
 * Leaves *devh NULL when the device does not come back. */
static void reopen_dfu_for_detach(struct dfu_runtime_context *runtime,
                                  const struct dfu_usb_identity *identity,
                                  libusb_device_handle **devh,
                                  int *claimed)
{
	unsigned poll;

	*devh = NULL;
	*claimed = 0;
	log_dfu_stage("looking for the DFU device again to detach it");
	for (poll = 0; poll < DETACH_REOPEN_POLLS; poll++) {
		if (find_ubertooth_dfu_device(runtime->usb_context, identity, devh) == 1)
			break;
		*devh = NULL;
		transition_sleep_ms(runtime, DETACH_REOPEN_INTERVAL_MS);
	}
	if (*devh == NULL) {
		log_dfu_stage("DFU device did not come back; cannot detach it");
		return;
	}
	/* Detaching means control transfers on interface 0, so without the claim
	 * there is nothing useful to send: let it go rather than guess. */
	if (libusb_claim_interface(*devh, 0) != 0) {
		log_dfu_stage("reopened DFU interface could not be claimed; cannot detach");
		libusb_close(*devh);
		*devh = NULL;
		return;
	}
	*claimed = 1;
	log_dfu_stage("DFU device reopened for detach");
}

/* Does this file already end in a DFU suffix? A .bin from the build does not,
 * a signed .dfu does, and -s only needs to tell them apart: the suffix length
 * and the UFD signature are enough, and a file whose suffix CRC is wrong is
 * still a file that must not be signed a second time. Leaves the position. */
static int ends_with_dfu_suffix(FILE* file) {
	uint8_t suffix[DFU_SUFFIX_LENGTH];
	long position = ftell(file);
	int found = 0;

	if (fseek(file, 0, SEEK_END) != 0)
		return 0;
	if (ftell(file) >= DFU_SUFFIX_LENGTH &&
	    fseek(file, -DFU_SUFFIX_LENGTH, SEEK_END) == 0 &&
	    fread(suffix, 1, sizeof(suffix), file) == sizeof(suffix))
		found = suffix[DFU_SUFFIX_OFF_BLENGTH] == DFU_SUFFIX_LENGTH &&
		        suffix[DFU_SUFFIX_OFF_UCDFUSIG + 0] == 0x55 &&
		        suffix[DFU_SUFFIX_OFF_UCDFUSIG + 1] == 0x46 &&
		        suffix[DFU_SUFFIX_OFF_UCDFUSIG + 2] == 0x44;
	if (position >= 0)
		fseek(file, position, SEEK_SET);
	return found;
}

/* Open a temporary file beside target to write into, so a run that fails
 * part-way leaves any earlier file alone; it is renamed over target only once
 * the whole thing is there. Both outputs use it: the read-back of -u and the
 * signed image of -s. The name is stored in *temp_name for the caller to free
 * and, on failure, unlink. */
static FILE* open_output_temp(const char* target, char** temp_name) {
	size_t length = strlen(target) + sizeof(".XXXXXX");
	char* name = malloc(length);
	struct stat target_state;
	mode_t mode, mask;
	FILE* file;
	int fd;

	if (name == NULL)
		return NULL;
	snprintf(name, length, "%s.XXXXXX", target);
	fd = mkstemp(name);
	if (fd < 0) {
		free(name);
		return NULL;
	}
	/* mkstemp() creates the file private to this user. Overwriting a file
	 * used to keep its permissions and creating one used to take the umask,
	 * so do the same here before it is renamed into place. */
	if (stat(target, &target_state) == 0) {
		mode = target_state.st_mode & 07777;
	} else {
		mask = umask(0);
		umask(mask);
		mode = (mode_t)(0666 & ~mask);
	}
	(void)fchmod(fd, mode);
	file = fdopen(fd, "w+b");
	if (file == NULL) {
		close(fd);
		unlink(name);
		free(name);
		return NULL;
	}
	*temp_name = name;
	return file;
}

/* Close and remove a temporary output file, so no command that ends before it
 * was complete leaves one behind. */
static void discard_output_temp(FILE** file, char** name) {
	if (*file != NULL) {
		fclose(*file);
		*file = NULL;
	}
	if (*name != NULL) {
		unlink(*name);
		free(*name);
		*name = NULL;
	}
}

static int get_outfile(char *infile, char **outfile) {
	char *suffix = strrchr(infile, '.');
	if (suffix != NULL && strcmp(suffix, ".bin") == 0) {
		*outfile = strdup(infile);
		if (*outfile == NULL)
			return 0;
		suffix = strrchr(*outfile, '.');
		strcpy(suffix, ".dfu");
		return 1;
	} else {
		return 0;
	}
}

/* The usage text: on standard output for -h, on standard error for a bare
 * command line. */
static void usage(FILE *file)
{
	fputs("ubertooth-dfu - Ubertooth firmware update tool\n", file);
	fputs("\n", file);
	fputs("To update firmware, run:\n", file);
	fputs("\tubertooth-dfu -d bluetooth_rxtx.dfu -r\n", file);
	fputs("\n", file);
	fputs("Usage:\n", file);
	fputs("\t-u <filename> upload - read the application flash region to a\n", file);
	fputs("\t              raw file (from 0x4000 to the end of the part's\n", file);
	fputs("\t              flash, no DFU suffix: it is not a file -d can\n", file);
	fputs("\t              write back)\n", file);
	fputs("\t-d <filename> download - write DFU file to device\n", file);
	fputs("\t-r reset Ubertooth after other operations complete\n", file);
	fputs("\n", file);
	fputs("Miscellaneous:\n", file);
	fputs("\t-s <filename> add DFU suffix to binary firmware file\n", file);
	fputs(OPT_USAGE_SELECT_DEVICE, file);
	fputs("\t-V print version information\n", file);
}

/* Settle how much flash the read-back asks the bootloader for, and say what
 * it is about to read and where that figure came from. The part's own size is
 * used when its ID was readable in the application; otherwise the size the
 * application is linked for, which no Ubertooth part has less of. Returns 0,
 * or 1 when there is no application region to read. */
static int plan_upload(struct dfu_runtime_context *runtime)
{
	const struct dfu_part *part = runtime->part_id_read ?
		dfu_part_lookup(runtime->part_id) : NULL;
	uint32_t address = BOOTLOADER_OFFSET + BOOTLOADER_SIZE;
	uint32_t length;

	length = dfu_upload_length(part != NULL ? part->flash_size : 0);
	runtime->upload_length = length;
	if (length == 0) {
		/* No part in the table is this small, and the assumed size always
		 * leaves a region, so this is defence rather than a path taken. */
		fprintf(stderr, "error: upload failed: %s has no flash above the "
		        "bootloader\n", part != NULL ? part->name : "this part");
		return 1;
	}
	/* Progress, so standard error: what the dump is going to cover, printed
	 * just before the dots that cover it. The dump itself is the result, and
	 * it goes to the file the user named. */
	fprintf(stderr, "Reading back %u bytes, 0x%x-0x%x: ", length, address,
	        address + length - 1);
	if (part != NULL)
		fprintf(stderr, "part 0x%08x (%s) has %u KiB of flash\n",
		        runtime->part_id, part->name, part->flash_size / 1024);
	else if (runtime->part_id_read)
		fprintf(stderr, "part 0x%08x is not known here, so assuming the %u "
		        "KiB part this firmware is linked for\n", runtime->part_id,
		        DFU_ASSUMED_FLASH_SIZE / 1024);
	else
		fprintf(stderr, "the part could not be read, so assuming the %u KiB "
		        "part this firmware is linked for\n",
		        DFU_ASSUMED_FLASH_SIZE / 1024);
	fflush(stderr);
	return 0;
}

#define FUNC_DOWNLOAD (1<<0)
#define FUNC_UPLOAD   (1<<1)
#define FUNC_SIGN     (1<<2)
#define FUNC_RESET    (1<<3)

/* How a claim failure names the board: by the -U string, which is all this
 * tool knows about it. NULL without one, which the reporter reads as "the
 * Ubertooth". The result lives in a static buffer, so it is printed straight
 * away. */
static const char* dfu_board_label(const char* selector)
{
	static char label[64];

	if (selector == NULL || selector[0] == '\0')
		return NULL;
	snprintf(label, sizeof(label), "Ubertooth %s", selector);
	return label;
}

int main(int argc, char **argv) {
	FILE* downfile = NULL;
	FILE* upfile = NULL;
	FILE* infile = NULL;
	FILE* outfile = NULL;
	char* outfile_name = NULL;
	char* outfile_temp = NULL;
	/* Values of -s, -d and -u, acted on after every check has passed. */
	char* sign_name = NULL;
	const char* downfile_name = NULL;
	const char* upfile_name = NULL;
	char* upfile_temp = NULL;
	libusb_context* dfu_context = NULL;
	libusb_device_handle* devh = NULL;
	struct dfu_usb_identity selected_identity;
	struct dfu_runtime_context runtime;
	/* What the -d image says about itself, read from its firmware info
	 * block; its revision is what the -r verification expects back. */
	struct ubertooth_fw_info image_info = {0};
	unsigned long download_image_size = 0;
	uint8_t functions = 0;
	int interface_claimed = 0;
	int exit_status = 0;
	int opt;
	opt_once once = {0};
	/* -h ends the command once the whole command line has parsed (see
	 * below), and so does -V, which reads the board and so can fail where
	 * -h cannot. */
	int first_exit_option = 0;
	int do_version = 0;
	char* ubertooth_device = NULL;
	int r;

	/* Report a bad option ourselves rather than letting getopt print it:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	/* Named, so the loop below and opt_report_bad_option()
	 * cannot be given different letters. */
	const char options[] = ":hVd:u:s:rU:";
	opterr = 0;
	while ((opt=getopt(argc, argv, options)) != EOF) {
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0) {
			exit_status = 1;
			goto end_without_device;
		}
		switch(opt) {
		/* -d, -u and -s are named here and acted on once the whole
		 * command line has been accepted: the signed file used to be
		 * created, and the temporary upload file opened, inside this
		 * loop, so a stray word later on left one behind or reported
		 * a missing file instead of the stray word. A repeated option
		 * takes the last name. */
		case 'd':
			downfile_name = optarg;
			functions |= FUNC_DOWNLOAD;
			break;
		case 'u':
			/* The read-back is written through a temporary file that is
			 * renamed over the name given, and a retry rewinds it to block
			 * zero: neither is possible on a pipe, so a lone "-" is refused
			 * here rather than taken as the file name "-". */
			if (strcmp(optarg, "-") == 0) {
				fputs("error: -u cannot write the read-back to stdout; "
				      "give a file name\n", stderr);
				exit_status = 1;
				goto end_without_device;
			}
			upfile_name = optarg;
			functions |= FUNC_UPLOAD;
			break;
		case 's':
			sign_name = optarg;
			/* Whether the name ends in .bin is a string check that
			 * creates nothing, so it stays here with the other value
			 * checks and is still reported alongside -h. */
			free(outfile_name);
			outfile_name = NULL;
			if (get_outfile(sign_name, &outfile_name) == 0) {
				fputs("error: -s requires a .bin\n", stderr);
				exit_status = 1;
				goto end_without_device;
			}
			functions |= FUNC_SIGN;
			break;
		case 'r':
			functions |= FUNC_RESET;
			break;
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'V':
			do_version = 1;
			break;
		case 'h':
			if (!first_exit_option)
				first_exit_option = 'h';
			break;
		default:
			opt_report_bad_option(opt, argc, argv, options);
			exit_status = 1;
			goto end_without_device;
		}
	}

	/* ubertooth-dfu takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0) {
		exit_status = 1;
		goto end_without_device;
	}

	/* -h and -V end the command, but only once the whole command line has
	 * parsed: a bad option value or a stray word is reported even alongside
	 * them. A command line with both prints both, and the usage text comes
	 * last so the version is not scrolled off the screen by it. */
	if (do_version)
		exit_status = ubertooth_print_versions(stdout, ubertooth_device);
	if (first_exit_option == 'h')
		usage(stdout);
	if (do_version || first_exit_option == 'h')
		goto end_without_device;

	/* No function is no work: the tool used to exit 0 having touched
	 * nothing and said nothing, so a mistyped command looked like a
	 * firmware update that had worked. */
	if (functions == 0) {
		fputs("error: nothing to do: give -d to write an image, -u to read"
		      " one back, -s to sign one, or -r to reset\n", stderr);
		if (opt_no_arguments(argc))
			usage(stderr);
		exit_status = 1;
		goto end_without_device;
	}

	/* Every check on the command line has passed, so the files this command
	 * only reads can be opened and checked: the -d image here and the -s
	 * input below. Reading them creates nothing, and either can still end the
	 * command, so both come before any output exists. */
	if (downfile_name != NULL) {
		/* Read-only: the image is never written to. */
		downfile = fopen(downfile_name, "rb");
		if (downfile == NULL) {
			fprintf(stderr, "error: %s: %s\n", downfile_name, strerror(errno));
			exit_status = 1;
			goto end_without_device;
		}
		if (image_file_refused(downfile_name, downfile,
		                       dfu_max_image_size(),
		                       "an Ubertooth image")) {
			exit_status = 1;
			goto end_without_device;
		}
	}

	/* The whole image is checked before any output file exists and with no
	 * device touched: an image this command refuses leaves the file system
	 * as it was, so -s ... -d <refused image> writes no signed file, and the
	 * board keeps running its firmware. */
	if (functions & FUNC_DOWNLOAD) {
		if (check_download_image(downfile_name, downfile, &image_info,
		                         &download_image_size) != 0) {
			exit_status = 1;
			goto end_without_device;
		}
		if (functions & FUNC_RESET) {
			fprintf(stderr, "DFU expected revision: %s\n",
			        image_info.revision);
			fflush(stderr);
		}
	}

	/* The -s input, for the same reason: with -u and -s together, an input
	 * that -s will not take must not cost the read-back a temporary file,
	 * and must be what the command reports rather than an output the
	 * read-back could not have created either. */
	if (sign_name != NULL) {
		infile = fopen(sign_name, "rb");
		if (infile == NULL) {
			fprintf(stderr, "error: %s: %s\n", sign_name, strerror(errno));
			exit_status = 1;
			goto end_without_device;
		}
		/* The whole input is read to sign it, and the output is created
		 * below, so the size guard comes first: a refused input writes
		 * nothing. */
		if (image_file_refused(sign_name, infile,
		                       max_unsigned_image_size(),
		                       "the firmware in an Ubertooth image")) {
			exit_status = 1;
			goto end_without_device;
		}
		/* Signing an already-signed file would bury the first suffix in
		 * the firmware payload, so refuse before creating anything. */
		if (ends_with_dfu_suffix(infile)) {
			fprintf(stderr,
			        "error: %s already has a DFU suffix, so %s was not written\n",
			        sign_name, outfile_name);
			exit_status = 1;
			goto end_without_device;
		}
	}

	/* Everything that can be settled by reading has been, so the output
	 * files can be created. What is left is the work itself, which can still
	 * fail: signing can hit a short read or a write that does not fit, and
	 * the read-back depends on the board. So both go through a temporary file
	 * renamed over the target once what belongs in it is all there, and a
	 * failure from here on leaves an earlier file of the same name untouched
	 * and creates none. */
	if (upfile_name != NULL) {
		upfile = open_output_temp(upfile_name, &upfile_temp);
		if (upfile == NULL) {
			fprintf(stderr, "error: %s: %s\n", upfile_name, strerror(errno));
			exit_status = 1;
			goto end_without_device;
		}
	}
	if (sign_name != NULL) {
		outfile = open_output_temp(outfile_name, &outfile_temp);
		if (outfile == NULL) {
			fprintf(stderr, "error: %s: %s\n", outfile_name, strerror(errno));
			exit_status = 1;
			goto end_without_device;
		}
	}

	if(functions & FUNC_SIGN) {
		exit_status = sign(sign_name, infile, outfile_name, outfile,
		                   U1_DFU_VENDORID, U1_DFU_PRODUCTID);
		fclose(infile);
		infile = NULL;
		if (exit_status == 0) {
			/* Buffered bytes can still fail to reach the file, so the close
			 * is what settles whether the whole signed image was written. */
			if (fclose(outfile) != 0) {
				fprintf(stderr, "error: %s: %s\n", outfile_name,
				        strerror(errno));
				exit_status = 1;
			}
			outfile = NULL;
		}
		/* A signing run that got this far without the whole image ends
		 * through the path that removes the temporary file, so an earlier
		 * .dfu of the same name survives and no partial one appears. */
		if (exit_status != 0)
			goto end_without_device;
		if (rename(outfile_temp, outfile_name) != 0) {
			fprintf(stderr, "error: %s: %s\n", outfile_name, strerror(errno));
			exit_status = 1;
			goto end_without_device;
		}
		free(outfile_temp);
		outfile_temp = NULL;
		free(outfile_name);
		outfile_name = NULL;
	}

	if (!(functions & (FUNC_UPLOAD|FUNC_DOWNLOAD|FUNC_RESET)))
		goto end_without_device;

	start_operation_timer();
	log_dfu_stage("operation started");
	memset(&selected_identity, 0, sizeof(selected_identity));
	memset(&runtime, 0, sizeof(runtime));
	/* Both transfers are bounded by the part's flash size, so both ask the
	 * application for its part ID; a bare reset has nothing to bound. */
	runtime.want_part_id = (functions & (FUNC_UPLOAD|FUNC_DOWNLOAD)) != 0;
	runtime.upload_name = upfile_name;
	if (functions & FUNC_DOWNLOAD) {
		runtime.download_name = downfile_name;
		runtime.download_length = dfu_download_length(download_image_size);
	}
	r = libusb_init(&dfu_context);
	if (r < 0) {
		fprintf(stderr, "error: libusb_init failed: %d\n", r);
		/* No context to unwind, and nothing has been opened on the bus, so
		 * this ends like the option failures: through the one path that also
		 * removes the temporary upload file. Not cleanup, which would call
		 * libusb_exit() on a context that was never created. */
		log_dfu_stage("operation finished");
		exit_status = 1;
		goto end_without_device;
	}
	runtime.usb_context = dfu_context;

	/* First support a device already waiting in DFU. Otherwise execute the
	 * selected application-to-DFU transition with path matching. */
	/* A DFU device has no serial descriptor. Adopt a sole existing DFU device
	 * only for an unselected/default operation; an explicit -U must first open
	 * its application device and bind the transition to that physical path. */
	if (dfu_selector_allows_unidentified_dfu(ubertooth_device))
		r = find_ubertooth_dfu_device(dfu_context, NULL, &devh);
	else
		r = 0;
	if (r < 0) {
		if (r == LIBUSB_ERROR_BUSY)
			fprintf(stderr, "error: multiple DFU devices found; leave only the "
			        "intended device in DFU mode\n");
		else
			show_libusb_error(r);
		exit_status = 1;
		goto cleanup;
	}
	if (r == 0) {
		const struct dfu_transition_config config = {
			.max_attempts = DFU_ENTRY_DEFAULT_MAX_ATTEMPTS,
			.polls_per_attempt = DFU_ENTRY_DEFAULT_POLLS_PER_ATTEMPT,
			.poll_interval_ms = DFU_ENTRY_DEFAULT_POLL_INTERVAL_MS,
		};
		struct dfu_transition_report report;
		void *transition_handle = NULL;
		r = dfu_transition_run(&TRANSITION_OPS, &runtime, ubertooth_device,
		                       &config, &transition_handle, &report);
		if (r != DFU_TRANSITION_OK) {
			/* An image refused for the attached part has already said so,
			 * and the board never left its application, so the transition's
			 * own line would only report a device that would not open. */
			if (!runtime.download_refused) {
				log_dfu_stage("DFU entry failed");
				fprintf(stderr, "error: %s (attempts=%u, polls=%u, usb_error=%d)\n",
				        dfu_transition_result_string(report.result),
				        report.attempts, report.polls,
				        report.operation_error);
				/* These two failures happen after the board was asked to
				 * enter DFU, so it may be sitting in the bootloader with no
				 * handle of ours: keep the path for cleanup to look again. */
				if (report.result == DFU_TRANSITION_DFU_TIMEOUT ||
				    report.result == DFU_TRANSITION_DISCOVERY_FAILED)
					selected_identity = report.identity;
			}
			exit_status = 1;
			goto cleanup;
		}
		devh = transition_handle;
		selected_identity = report.identity;
	} else {
		r = get_usb_identity(libusb_get_device(devh), &selected_identity);
		if (r < 0) {
			show_libusb_error(r);
			exit_status = 1;
			goto cleanup;
		}
		/* A board adopted in the bootloader has no application to ask for
		 * its part, so there is nothing to check the image against: this
		 * says so rather than checking it silently. It cannot refuse
		 * anything today, for want of a part to refuse it against; the
		 * result is taken anyway so that a figure found for a bootloader
		 * later on would not be quietly ignored. */
		if ((functions & FUNC_DOWNLOAD) &&
		    download_refused_for_part(&runtime) != 0) {
			exit_status = 1;
			goto cleanup;
		}
	}

	r = libusb_claim_interface(devh, 0);
	if (r < 0) {
		/* The board opened, so this is not the permissions problem this used
		 * to blame: something else holds the interface. Named by the -U
		 * string when there was one, as the library's own paths do. */
		ubertooth_report_claim_failed(dfu_board_label(ubertooth_device), r);
		exit_status = 1;
		goto cleanup;
	}
	interface_claimed = 1;
	log_dfu_stage("DFU interface claimed");

	if(functions & FUNC_UPLOAD) {
		const struct dfu_transfer_config transfer_config = {
			.max_attempts = TRANSFER_MAX_ATTEMPTS,
			.reopen_polls = TRANSFER_REOPEN_POLLS,
			.poll_interval_ms = TRANSFER_REOPEN_INTERVAL_MS,
		};
		struct dfu_transfer_report transfer_report;
		void *transfer_handle = devh;

		if (plan_upload(&runtime) != 0) {
			exit_status = 1;
			goto cleanup;
		}
		runtime.upload_file = upfile;
		runtime.transfer_attempts = 0;
		r = dfu_transfer_run(&UPLOAD_TRANSFER_OPS, &runtime,
		                     &selected_identity, &transfer_config,
		                     &transfer_handle, &transfer_report);
		/* The transfer owns the handle while it runs: it closes and
		 * reopens the device around a retry, and clears the handle when
		 * it gives up. */
		devh = transfer_handle;
		interface_claimed = devh != NULL;
		fclose(upfile);
		upfile = NULL;
		runtime.upload_file = NULL;
		if(r != DFU_TRANSFER_OK) {
			fprintf(stderr,
			        "error: firmware upload failed after %u attempt(s): "
			        "result=%d transfer_error=%d usb_error=%d\n",
			        transfer_report.attempts, transfer_report.result,
			        transfer_report.transfer_error,
			        transfer_report.operation_error);
			/* cleanup removes the temporary file, so an earlier read-back
			 * of the same name survives, and returns the board to its
			 * application when -r was given. */
			exit_status = 1;
			goto cleanup;
		}
		if (rename(upfile_temp, upfile_name) != 0) {
			fprintf(stderr, "error: %s: %s\n", upfile_name, strerror(errno));
			exit_status = 1;
			goto cleanup;
		}
		free(upfile_temp);
		upfile_temp = NULL;
	}

	if(functions & FUNC_DOWNLOAD) {
		const struct dfu_transfer_config transfer_config = {
			.max_attempts = TRANSFER_MAX_ATTEMPTS,
			.reopen_polls = TRANSFER_REOPEN_POLLS,
			.poll_interval_ms = TRANSFER_REOPEN_INTERVAL_MS,
		};
		struct dfu_transfer_report transfer_report;
		void *transfer_handle = devh;

		runtime.download_file = downfile;
		/* So the stage log counts this transfer's own attempts, not the
		 * read-back's before it. */
		runtime.transfer_attempts = 0;
		r = dfu_transfer_run(&DOWNLOAD_TRANSFER_OPS, &runtime,
		                     &selected_identity, &transfer_config,
		                     &transfer_handle, &transfer_report);
		/* The transfer owns the handle while it runs: it closes and reopens
		 * the device around a retry, and clears the handle when it gives up.
		 * Take the claim state from what it hands back. */
		devh = transfer_handle;
		interface_claimed = devh != NULL;
		fclose(downfile);
		downfile = NULL;
		if(r != DFU_TRANSFER_OK) {
			fprintf(stderr,
			        "error: firmware update failed after %u attempt(s): "
			        "result=%d transfer_error=%d usb_error=%d\n",
			        transfer_report.attempts, transfer_report.result,
			        transfer_report.transfer_error,
			        transfer_report.operation_error);
			exit_status = 1;
			goto cleanup;
		}
		log_dfu_stage("firmware programming completed");
		/* The whole image is in the flash from here, whatever the reset
		 * does next. A failure after this point has to say so, or the
		 * reader's next move is to flash a board that does not need it. */
		runtime.download_completed = 1;
	}

	if(functions & FUNC_RESET) {
		log_dfu_stage("DFU detach/reset request started");
		r = detach(&runtime, devh, &selected_identity);
		log_dfu_stage("DFU detach/reset request returned");
		if (r != 0) {
			/* The board is still in the bootloader, so keep the handle
			 * for cleanup to try the detach once more, and fail. */
			exit_status = 1;
			goto cleanup;
		}
		close_dfu_device(devh, interface_claimed);
		devh = NULL;
		interface_claimed = 0;
	}

	if ((functions & FUNC_DOWNLOAD) && (functions & FUNC_RESET)) {
		const struct dfu_verify_config verify_config = {
			.polls = 20,
			.poll_interval_ms = 250,
		};
		struct dfu_verify_report verify_report;
		const char *selector = ubertooth_device != NULL ? ubertooth_device : "";

		log_dfu_stage("post-reset application verification started");
		runtime.verify_identity = &selected_identity;
		r = dfu_verify_application(&VERIFY_OPS, &runtime, selector,
		                           &selected_identity, image_info.revision,
		                           image_info.image,
		                           &verify_config, &verify_report);
		if (r != DFU_VERIFY_OK) {
			char message[256];
			char stage[384];

			/* The raw codes go in the stage trace, which is where the rest
			 * of this command's internals are reported; the line the user
			 * reads says which outcome it was and what to do next. */
			snprintf(stage, sizeof(stage),
			         "post-reset application verification failed: result=%d "
			         "polls=%u expected_image=%s expected_revision=%s "
			         "actual_image=%s actual_revision=%s usb_error=%d "
			         "bus_error=%d",
			         verify_report.result, verify_report.polls,
			         image_info.image, image_info.revision,
			         verify_report.actual_image,
			         verify_report.actual_revision,
			         verify_report.operation_error,
			         runtime.verify_bus_error);
			log_dfu_stage(stage);
			dfu_verify_describe_failure(&verify_report, image_info.revision,
			                            image_info.image,
			                            message, sizeof(message));
			fprintf(stderr, "error: %s\n", message);
			exit_status = 1;
			goto cleanup;
		}
		/* The one thing this command reads back and reports: the revision the
		 * board now runs, read from the returned application. It is the
		 * result, so standard output, where a script can take it. The
		 * verification only passes when it matches the image's. */
		printf("Verified firmware revision: %s\n",
		       verify_report.actual_revision);
		fflush(stdout);
		log_dfu_stage("post-reset application verification completed");
	}

cleanup:
	log_dfu_stage("cleanup started");
	discard_output_temp(&upfile, &upfile_temp);
	/* A signed image has been renamed into place long before this, so there
	 * is normally nothing here; it stays so that moving the signing step
	 * cannot leave a temporary file behind. */
	discard_output_temp(&outfile, &outfile_temp);
	free(outfile_name);
	outfile_name = NULL;
	if (downfile != NULL)
		fclose(downfile);
	/* A failure once the board is in DFU must not leave it there: -r asked
	 * for a reset, so detach before closing the handle. The success path has
	 * already detached and cleared devh. A failed programming transfer closes
	 * the DFU handle on its way out, so reopen the device on the same path
	 * first; if DFU never answered at all there is nothing to detach.
	 *
	 * A board this run never claimed is left alone: something else has it,
	 * and a detach is a reset aimed at whatever that is doing. So is a board
	 * that already left the bus once: the reset happened, the handle it
	 * answered on is stale, and driving that handle again would add error
	 * lines about the handle instead of anything about the board. */
	if (exit_status != 0 && (functions & FUNC_RESET) &&
	    selected_identity.port_count != 0 &&
	    !runtime.board_left_the_bus &&
	    (devh == NULL || interface_claimed)) {
		if (devh == NULL)
			reopen_dfu_for_detach(&runtime, &selected_identity, &devh,
			                      &interface_claimed);
		if (devh != NULL) {
			log_dfu_stage("returning the board to its application after a failure");
			detach(&runtime, devh, &selected_identity);
			log_dfu_stage("detach after failure returned");
		}
	}
	close_dfu_device(devh, interface_claimed);
	exit_dfu_context(dfu_context, (functions & FUNC_RESET) != 0);
	log_dfu_stage("operation finished");
	return exit_status;

	/* Ends before any device was opened, so nothing USB to unwind: just the
	 * files, including either temporary output file that never filled. */
end_without_device:
	if (downfile != NULL)
		fclose(downfile);
	if (infile != NULL)
		fclose(infile);
	discard_output_temp(&outfile, &outfile_temp);
	free(outfile_name);
	discard_output_temp(&upfile, &upfile_temp);
	return exit_status;
}
