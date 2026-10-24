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

static void log_dfu_stage(const char *stage)
{
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

/* Add a DFU suffix naming idVendor/idProduct to infile and write to outfile */
int sign(FILE* infile, FILE* outfile, uint16_t idVendor, uint16_t idProduct) {
	uint8_t* suffix;
	long file_length;
	size_t data_length, buffer_length;
	uint8_t* buffer;

	fseek(infile, 0, SEEK_END);
	file_length = ftell(infile);
	if (file_length < 0) {
		fprintf(stderr, "Cannot determine the size of the input file\n");
		return 1;
	}
	buffer_length = (size_t)file_length + DFU_SUFFIX_LENGTH;
	buffer = malloc(buffer_length);
	if(buffer == NULL) {
		fprintf(stderr, "Cannot allocate buffer to calculate CRC\n");
		return 1;
	}

	fseek(infile, 0, SEEK_SET);
	data_length = fread(buffer, 1, (size_t)file_length, infile);
	if (data_length != (size_t)file_length) {
		fprintf(stderr, "error: read %zu of %ld byte(s) from the input file\n",
		        data_length, file_length);
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
		fprintf(stderr, "error: could not write the whole signed file\n");
		free(buffer);
		return 1;
	}
	free(buffer);
	return 0;
}

/* Check the image before the board is asked to leave its application, so a
 * file this tool will not write costs the user nothing. Prints what is wrong
 * and returns 1, or 0 when the image can be written to this device. */
static int check_download_image(FILE* downfile, int want_revision,
                                char* revision, size_t revision_size)
{
	struct dfu_image_report image_report;
	char message[160];

	printf("Checking firmware signature\n");
	fflush(stdout);  /* so it stays ahead of the failure on a pipe */
	if (dfu_image_check(downfile, U1_DFU_VENDORID, U1_DFU_PRODUCTID,
	                    &image_report) != DFU_IMAGE_OK) {
		dfu_image_describe_failure(&image_report, message, sizeof(message));
		fprintf(stderr, "%s\n", message);
		fprintf(stderr, "Signature check failed, firmware will not be updated\n");
		return 1;
	}
	if (want_revision &&
	    dfu_extract_firmware_revision(downfile, revision, revision_size) != 0) {
		fprintf(stderr, "Unable to determine expected firmware revision from image\n");
		return 1;
	}
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
 * libusb error. With no expected path, fail rather than choosing ambiguously. */
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
	result = libusb_open(match, devh);
	if (result == 0)
		result = 1;

done:
	libusb_free_device_list(usb_list, 1);
	return result;
}

/* Return 1 when the application device on the expected path is opened, 0 when
 * absent, or a negative libusb error. */
static int find_ubertooth_application_device(
	libusb_context *ctx,
	const struct dfu_usb_identity *expected_identity,
	libusb_device_handle **devh)
{
	libusb_device **usb_list = NULL;
	struct libusb_device_descriptor desc;
	struct dfu_usb_identity identity;
	ssize_t usb_devs;
	int i, result;

	*devh = NULL;
	/* As in find_ubertooth_dfu_device(): the switch hides the bus from this
	 * search too. */
	if (ubertooth_devices_hidden())
		return 0;
	usb_devs = libusb_get_device_list(ctx, &usb_list);
	if (usb_devs < 0)
		return (int)usb_devs;

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

struct dfu_runtime_context {
	libusb_context *usb_context;
	FILE *download_file;
	FILE *upload_file;
	uint32_t upload_length;  /* bytes the read-back asks the bootloader for */
	uint32_t part_id;        /* the part ID read from the application */
	int part_id_read;        /* ...and whether it was read at all */
	unsigned app_attempts;
	unsigned dfu_polls;
	unsigned transfer_attempts;
	unsigned verify_polls;
	const struct dfu_usb_identity *verify_identity;
};

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
	/* Read the part ID while the application is still answering: it says how
	 * much flash a read-back may ask for, and the bootloader has no request
	 * that reports it (#233). A board that will not say keeps the assumed
	 * size, so this failing does not fail the command. */
	if (cmd_get_part_id(ut->devh, &runtime->part_id) == 0) {
		runtime->part_id_read = 1;
		snprintf(stage, sizeof(stage), "part ID read: 0x%08x",
		         runtime->part_id);
		log_dfu_stage(stage);
	} else {
		runtime->part_id_read = 0;
		log_dfu_stage("part ID could not be read");
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
		fprintf(stdout, "Switching selected device to DFU mode...\n");
		fflush(stdout);
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

int dfu_get_state(libusb_device_handle* devh) {
	int rv;
	uint8_t state;
	rv = libusb_control_transfer(devh, DFU_IN, REQ_GETSTATE, 0, 0, &state, 1, 1000);
	if (rv < 0) {
		if (rv == LIBUSB_ERROR_PIPE)
			fprintf(stderr, "control message unsupported\n");
		else
			show_libusb_error(rv);
		return rv;
	}
	return state;
}

int dfu_clear_status(libusb_device_handle* devh) {
	int rv;
	rv = libusb_control_transfer(devh, DFU_OUT, REQ_CLRSTATUS, 0, 0, NULL, 0, 1000);
	if (rv < 0) {
		if (rv == LIBUSB_ERROR_PIPE)
			fprintf(stderr, "control message unsupported\n");
		else
			show_libusb_error(rv);
		return rv;
	}
	return 0;
}

int dfu_abort(libusb_device_handle* devh) {
	int rv;
	rv = libusb_control_transfer(devh, DFU_OUT, REQ_ABORT, 0, 0, NULL, 0, 1000);
	if (rv < 0) {
		if (rv == LIBUSB_ERROR_PIPE)
			fprintf(stderr, "control message unsupported\n");
		else
			show_libusb_error(rv);
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
		fprintf(stderr, "DFU gave up: %s (polls=%u, state=%d, usb_error=%d)\n",
		        dfu_idle_result_string(report.result), report.polls,
		        report.last_state, report.operation_error);
		return rv;
	}
	return 0;
}

int detach(libusb_device_handle* devh) {
	int rv;
	rv = enter_dfu_mode(devh);
	if(rv < 0) {
		fprintf(stderr, "Detach failed: could not enter DFU mode\n");
		return rv;
	}
	rv = libusb_control_transfer(devh, DFU_OUT, REQ_DETACH, 0, 0, NULL, 0, 1000);
	if (rv != LIBUSB_SUCCESS) {
		if (rv == LIBUSB_ERROR_PIPE)
			fprintf(stderr, "control message unsupported\n");
		else
			show_libusb_error(rv);
		fprintf(stderr, "Detach failed: the board is still in DFU\n");
		return rv;
	}
	printf("Detached\n");
	return 0;
}

/* Read length bytes of the application flash region into upfile. The caller
 * settles length from the part's flash size: the bootloader faults on a read
 * with no flash behind it instead of refusing it (#233), so this must never
 * be asked for more than the part has. */
int upload(libusb_device_handle* devh, FILE* upfile, uint32_t length) {
	int address, block, rv;
	uint8_t buffer[BLOCK_SIZE];
	address = BOOTLOADER_OFFSET + BOOTLOADER_SIZE;
	block = address / BLOCK_SIZE;

	if ((address & (BLOCK_SIZE - 1)) != 0) {
		fprintf(stderr, "Upload failed: must start at block boundary\n");
		return -1;
	}
	if (length == 0 || (length & (BLOCK_SIZE - 1)) != 0) {
		fprintf(stderr, "Upload failed: %u bytes is not a whole number of "
		        "%d-byte blocks\n", length, BLOCK_SIZE);
		return -1;
	}

	rv = enter_dfu_mode(devh);
	if(rv < 0) {
		fprintf(stderr, "Upload failed: could not enter DFU mode\n");
		return rv;
	}

	while(length > 0) {
		rv = libusb_control_transfer(devh, DFU_IN, REQ_UPLOAD, block, 0,
		                             buffer, BLOCK_SIZE, 1000);
		if (rv < 0) {
			if (rv == LIBUSB_ERROR_PIPE)
				fprintf(stderr, "control message unsupported\n");
			else
				show_libusb_error(rv);
			fprintf(stdout, "\n");
			fprintf(stderr, "Upload failed: block %d was not read\n",
			        block);
			return rv;
		}
		if(rv != BLOCK_SIZE) {
			fprintf(stdout, "\n");
			fprintf(stderr, "Upload failed: did not read full block\n");
			return -1;
		}
		fprintf(stdout, ".");
		/* A short write would be renamed over the target as a truncated
		 * dump, so it ends the read-back too. */
		if(fwrite(buffer, 1, BLOCK_SIZE, upfile) != BLOCK_SIZE) {
			fprintf(stdout, "\n");
			fprintf(stderr, "Upload failed: could not write the dump\n");
			return -1;
		}
		block++;
		length -= BLOCK_SIZE;
	}
	fprintf(stdout, "\n");
	return 0;
}

int dfu_get_status(libusb_device_handle* devh) {
	uint8_t buffer[6];
	int rv;
	rv = libusb_control_transfer(devh, DFU_IN, REQ_GETSTATUS, 0, 0, buffer, 6, 1000);
	if (rv < 0) {
		if (rv == LIBUSB_ERROR_PIPE)
			fprintf(stderr, "control message unsupported\n");
		else
			show_libusb_error(rv);
		/* The reply carries the state the next block depends on, so a
		 * read that failed cannot be treated as status OK. */
		return rv;
	}
	return 0;
}

int download(libusb_device_handle* devh, FILE* downfile) {
	int address, length, block, rv, block_count=1;
	uint8_t buffer[BLOCK_SIZE];
	address = BOOTLOADER_OFFSET + BOOTLOADER_SIZE;
	block = address / BLOCK_SIZE;
	fseek(downfile, 0, SEEK_SET);

	if ((address & (SECTOR_SIZE - 1)) != 0) {
		fprintf(stderr, "Download failed: must start at sector boundary\n");
		return -1;
	}

	rv = enter_dfu_mode(devh);
	if(rv < 0) {
		fprintf(stderr, "Download failed: could not enter DFU mode\n");
		return rv;
	}
	while((length=fread(buffer, 1, BLOCK_SIZE, downfile))) {
		for(;length<BLOCK_SIZE;length++)
			buffer[length] = 0xFF;
		rv = libusb_control_transfer(devh, DFU_OUT, REQ_DNLOAD, block, 0,
		                             buffer, BLOCK_SIZE, 1000);
		if (rv < 0) {
			if (rv == LIBUSB_ERROR_PIPE)
				fprintf(stderr, "control message unsupported\n");
			else
				show_libusb_error(rv);
			return rv;
		}
		rv = dfu_get_status(devh);
		if (rv < 0)
			return rv;
		fprintf(stdout, ".");
		if(block_count % 40)
			fflush(stdout);
		else
			fprintf(stdout, "\n");
		block++;
		block_count++;
	}
	fprintf(stdout, "\n");
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

/* The read-back gets the same one bounded retry as programming: #208 saw a
 * single REQ_UPLOAD fail a second after the board came back from the
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
		perror("upload file");
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
		runtime->usb_context, runtime->verify_identity, &devh);
	if (result == 0)
		return LIBUSB_ERROR_NO_DEVICE;
	if (result < 0)
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
	return 0;
}

static int verify_get_revision(void *context __attribute__((unused)),
                               void *handle, char *revision,
                               size_t revision_size)
{
	return cmd_get_firmware_revision(handle, revision, revision_size) < 0 ? -1 : 0;
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

/* Open a temporary file beside target to upload into, so a failed read-back
 * leaves any earlier file alone; it is renamed over target only once the whole
 * upload has arrived. The name is stored in *temp_name for the caller to free
 * and, on failure, unlink. */
static FILE* open_upload_temp(const char* target, char** temp_name) {
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

/* Close and remove the temporary upload file, so no command that ends before
 * a complete upload leaves one behind. */
static void discard_upload_temp(FILE** file, char** name) {
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

static void usage()
{
	printf("ubertooth-dfu - Ubertooth firmware update tool\n");
	printf("\n");
	printf("To update firmware, run:\n");
	printf("\tubertooth-dfu -d bluetooth_rxtx.dfu -r\n");
	printf("\n");
	printf("Usage:\n");
	printf("\t-u <filename> upload - read the application flash region to a\n");
	printf("\t              raw file (from 0x4000 to the end of the part's\n");
	printf("\t              flash, no DFU suffix: it is not a file -d can\n");
	printf("\t              write back)\n");
	printf("\t-d <filename> download - write DFU file to device\n");
	printf("\t-r reset Ubertooth after other operations complete\n");
	printf("\n");
	printf("Miscellaneous:\n");
	printf("\t-s <filename> add DFU suffix to binary firmware file\n");
	fputs(OPT_USAGE_SELECT_DEVICE, stdout);
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
		fprintf(stderr, "Upload failed: part 0x%08x (%s) has no flash above "
		        "the bootloader\n", runtime->part_id,
		        part != NULL ? part->name : "unknown");
		return 1;
	}
	printf("Reading back %u bytes, 0x%x-0x%x: ", length, address,
	       address + length - 1);
	if (part != NULL)
		printf("part 0x%08x (%s) has %u KiB of flash\n", runtime->part_id,
		       part->name, part->flash_size / 1024);
	else if (runtime->part_id_read)
		printf("part 0x%08x is not known here, so assuming the %u KiB of "
		       "flash this firmware is linked for\n", runtime->part_id,
		       DFU_ASSUMED_FLASH_SIZE / 1024);
	else
		printf("the part could not be read, so assuming the %u KiB of flash "
		       "this firmware is linked for\n", DFU_ASSUMED_FLASH_SIZE / 1024);
	fflush(stdout);
	return 0;
}

#define FUNC_DOWNLOAD (1<<0)
#define FUNC_UPLOAD   (1<<1)
#define FUNC_SIGN     (1<<2)
#define FUNC_RESET    (1<<3)

int main(int argc, char **argv) {
	FILE* downfile = NULL;
	FILE* upfile = NULL;
	FILE* infile = NULL;
	FILE* outfile = NULL;
	char* outfile_name = NULL;
	const char* upfile_name = NULL;
	char* upfile_temp = NULL;
	libusb_context* dfu_context = NULL;
	libusb_device_handle* devh = NULL;
	struct dfu_usb_identity selected_identity;
	struct dfu_runtime_context runtime;
	char expected_revision[64] = {0};
	uint8_t functions = 0;
	int interface_claimed = 0;
	int exit_status = 0;
	int opt;
	/* -h ends the command once the whole command line has parsed (see below). */
	int first_exit_option = 0;
	char* ubertooth_device = NULL;
	int r;

	while ((opt=getopt(argc,argv,"hd:u:s:rU:")) != EOF) {
		switch(opt) {
		case 'd':
			/* Read-only: the image is never written to. */
			downfile = fopen(optarg, "rb");
			if (downfile == NULL) {
				perror(optarg);
				exit_status = 1;
				goto end_without_device;
			}
			functions |= FUNC_DOWNLOAD;
			break;
		case 'u':
			/* A repeated -u takes the last file, and the one opened for the
			 * earlier name goes away with it. */
			discard_upload_temp(&upfile, &upfile_temp);
			upfile_name = optarg;
			upfile = open_upload_temp(upfile_name, &upfile_temp);
			if (upfile == NULL) {
				perror(optarg);
				exit_status = 1;
				goto end_without_device;
			}
			functions |= FUNC_UPLOAD;
			break;
		case 's':
			infile = fopen(optarg, "rb");
			if (infile == NULL) {
				perror(optarg);
				exit_status = 1;
				goto end_without_device;
			}
			r = get_outfile(optarg, &outfile_name);
			if (r == 0) {
				printf("Error: -s requires a .bin\n");
				exit_status = 1;
				goto end_without_device;
			}
			/* Signing an already-signed file would bury the first suffix in
			 * the firmware payload, so refuse before creating anything. */
			if (ends_with_dfu_suffix(infile)) {
				fprintf(stderr,
				        "error: %s already has a DFU suffix, so %s was not written\n",
				        optarg, outfile_name);
				free(outfile_name);
				outfile_name = NULL;
				exit_status = 1;
				goto end_without_device;
			}
			outfile = fopen(outfile_name, "w+b");
			if (outfile == NULL) {
				perror(outfile_name);
				free(outfile_name);
				outfile_name = NULL;
				exit_status = 1;
				goto end_without_device;
			}
			free(outfile_name);
			outfile_name = NULL;
			functions |= FUNC_SIGN;
			break;
		case 'r':
			functions |= FUNC_RESET;
			break;
		case 'U':
			ubertooth_device = optarg;
			break;
		case 'h':
			if (!first_exit_option)
				first_exit_option = 'h';
			break;
		default:
			usage();
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

	/* -h ends the command, but only once the whole command line has
	 * parsed: a bad option value or a stray word is reported even
	 * alongside it. */
	if (first_exit_option == 'h') {
		usage();
		exit_status = 1;
		goto end_without_device;
	}

	if(functions & FUNC_SIGN) {
		exit_status = sign(infile, outfile, U1_DFU_VENDORID,
		                   U1_DFU_PRODUCTID);
		fclose(infile);
		infile = NULL;
		fclose(outfile);
		outfile = NULL;
		if (exit_status != 0)
			goto end_without_device;
	}

	if (!(functions & (FUNC_UPLOAD|FUNC_DOWNLOAD|FUNC_RESET)))
		goto end_without_device;

	/* The whole image is checked here, with no device touched yet: a bad
	 * file leaves the board running its application. */
	if (functions & FUNC_DOWNLOAD) {
		if (check_download_image(downfile, (functions & FUNC_RESET) != 0,
		                         expected_revision,
		                         sizeof(expected_revision)) != 0) {
			exit_status = 1;
			goto end_without_device;
		}
		if (functions & FUNC_RESET) {
			fprintf(stderr, "DFU expected revision: %s\n", expected_revision);
			fflush(stderr);
		}
	}

	start_operation_timer();
	log_dfu_stage("operation started");
	memset(&selected_identity, 0, sizeof(selected_identity));
	memset(&runtime, 0, sizeof(runtime));
	r = libusb_init(&dfu_context);
	if (r < 0) {
		fprintf(stderr, "libusb_init failed: %d\n", r);
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
			fprintf(stderr, "Multiple DFU devices found; leave only the intended device in DFU mode\n");
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
			log_dfu_stage("DFU entry failed");
			fprintf(stderr, "%s (attempts=%u, polls=%u, usb_error=%d)\n",
			        dfu_transition_result_string(report.result),
			        report.attempts, report.polls, report.operation_error);
			/* These two failures happen after the board was asked to enter
			 * DFU, so it may be sitting in the bootloader with no handle of
			 * ours: keep the path for cleanup to look again. */
			if (report.result == DFU_TRANSITION_DFU_TIMEOUT ||
			    report.result == DFU_TRANSITION_DISCOVERY_FAILED)
				selected_identity = report.identity;
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
	}

	r = libusb_claim_interface(devh, 0);
	if (r < 0) {
		fprintf(stderr, "usb_claim_interface error %d\n", r);
		fprintf(stderr, "Correct permissions to access Ubertooth?\n");
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
			        "Firmware upload failed after %u attempt(s): result=%d transfer_error=%d usb_error=%d\n",
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
			perror(upfile_name);
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
			        "Firmware update failed after %u attempt(s): result=%d transfer_error=%d usb_error=%d\n",
			        transfer_report.attempts, transfer_report.result,
			        transfer_report.transfer_error,
			        transfer_report.operation_error);
			exit_status = 1;
			goto cleanup;
		}
		log_dfu_stage("firmware programming completed");
	}

	if(functions & FUNC_RESET) {
		log_dfu_stage("DFU detach/reset request started");
		r = detach(devh);
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
		                           &selected_identity, expected_revision,
		                           &verify_config, &verify_report);
		if (r != DFU_VERIFY_OK) {
			fprintf(stderr,
			        "Post-reset verification failed: result=%d expected=%s actual=%s usb_error=%d\n",
			        verify_report.result, expected_revision,
			        verify_report.actual_revision,
			        verify_report.operation_error);
			exit_status = 1;
			goto cleanup;
		}
		fprintf(stderr, "Verified firmware revision: %s\n", expected_revision);
		log_dfu_stage("post-reset application verification completed");
	}

cleanup:
	log_dfu_stage("cleanup started");
	discard_upload_temp(&upfile, &upfile_temp);
	if (downfile != NULL)
		fclose(downfile);
	/* A failure once the board is in DFU must not leave it there: -r asked
	 * for a reset, so detach before closing the handle. The success path has
	 * already detached and cleared devh. A failed programming transfer closes
	 * the DFU handle on its way out, so reopen the device on the same path
	 * first; if DFU never answered at all there is nothing to detach. */
	if (exit_status != 0 && (functions & FUNC_RESET) &&
	    selected_identity.port_count != 0) {
		if (devh == NULL)
			reopen_dfu_for_detach(&runtime, &selected_identity, &devh,
			                      &interface_claimed);
		if (devh != NULL) {
			log_dfu_stage("returning the board to its application after a failure");
			detach(devh);
			log_dfu_stage("detach after failure returned");
		}
	}
	close_dfu_device(devh, interface_claimed);
	exit_dfu_context(dfu_context, (functions & FUNC_RESET) != 0);
	log_dfu_stage("operation finished");
	return exit_status;

	/* Ends before any device was opened, so nothing USB to unwind: just the
	 * files, including the temporary upload file that never filled. */
end_without_device:
	if (downfile != NULL)
		fclose(downfile);
	if (infile != NULL)
		fclose(infile);
	if (outfile != NULL)
		fclose(outfile);
	free(outfile_name);
	discard_upload_temp(&upfile, &upfile_temp);
	return exit_status;
}
