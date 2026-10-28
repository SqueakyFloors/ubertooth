#include "dfu_recovery.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dfu.h"

static int idle_fail(struct dfu_idle_report *report,
                     enum dfu_idle_result result,
                     int operation_error)
{
	report->result = result;
	report->operation_error = operation_error;
	return result;
}

int dfu_reach_idle(const struct dfu_idle_ops *ops,
                   void *context,
                   void *handle,
                   const struct dfu_idle_config *config,
                   struct dfu_idle_report *report)
{
	unsigned poll;

	if (report != NULL) {
		memset(report, 0, sizeof(*report));
		report->last_state = -1;
	}
	if (ops == NULL || handle == NULL || config == NULL || report == NULL ||
	    config->max_polls == 0 || ops->get_state == NULL ||
	    ops->abort == NULL || ops->clear_status == NULL ||
	    ops->sleep_ms == NULL)
		return report == NULL ? DFU_IDLE_INVALID_ARGUMENT :
		       idle_fail(report, DFU_IDLE_INVALID_ARGUMENT, 0);

	for (poll = 0; poll < config->max_polls; poll++) {
		int state, result;

		report->polls++;
		state = ops->get_state(context, handle);
		/* A bootloader that will not say where it is cannot be driven
		 * anywhere: stop on the first failed read rather than asking
		 * again forever. */
		if (state < 0)
			return idle_fail(report, DFU_IDLE_QUERY_FAILED, state);
		report->last_state = state;
		if (state == STATE_DFU_IDLE) {
			report->result = DFU_IDLE_OK;
			return DFU_IDLE_OK;
		}

		switch (state) {
		case STATE_DFU_DNLOAD_SYNC:
		case STATE_DFU_DNLOAD_IDLE:
		case STATE_DFU_MANIFEST_SYNC:
		case STATE_DFU_UPLOAD_IDLE:
		case STATE_DFU_MANIFEST:
			/* A transfer left mid-way: end it and look again. */
			result = ops->abort(context, handle);
			if (result < 0)
				return idle_fail(report, DFU_IDLE_ABORT_FAILED,
				                 result);
			break;
		case STATE_APP_DETACH:
		case STATE_DFU_DNBUSY:
		case STATE_DFU_MANIFEST_WAIT_RESET:
			/* These pass with time, so wait and look again. */
			ops->sleep_ms(context, config->poll_interval_ms);
			break;
		case STATE_DFU_ERROR:
			result = ops->clear_status(context, handle);
			if (result < 0)
				return idle_fail(report, DFU_IDLE_CLEAR_FAILED,
				                 result);
			break;
		default:
			/* STATE_APP_IDLE and anything outside the DFU 1.1 state
			 * set: the application has no DFU interface for this
			 * host to drive, and polling would never change that. */
			return idle_fail(report, DFU_IDLE_UNEXPECTED_STATE, 0);
		}
	}

	return idle_fail(report, DFU_IDLE_NOT_REACHED, 0);
}

const char *dfu_idle_result_string(enum dfu_idle_result result)
{
	switch (result) {
	case DFU_IDLE_OK:
		return "the bootloader is idle";
	case DFU_IDLE_INVALID_ARGUMENT:
		return "invalid DFU idle configuration";
	case DFU_IDLE_QUERY_FAILED:
		return "the bootloader did not report its state";
	case DFU_IDLE_ABORT_FAILED:
		return "the bootloader would not abort its last transfer";
	case DFU_IDLE_CLEAR_FAILED:
		return "the bootloader would not clear its error state";
	case DFU_IDLE_NOT_REACHED:
		return "the bootloader did not become idle in time";
	case DFU_IDLE_UNEXPECTED_STATE:
		return "the device is not in a state this host can drive";
	default:
		return "unknown DFU idle result";
	}
}

static int transfer_fail(struct dfu_transfer_report *report,
                         enum dfu_transfer_result result,
                         int operation_error)
{
	report->result = result;
	report->operation_error = operation_error;
	return result;
}

int dfu_transfer_run(const struct dfu_transfer_ops *ops,
                     void *context,
                     const struct dfu_usb_identity *identity,
                     const struct dfu_transfer_config *config,
                     void **handle,
                     struct dfu_transfer_report *report)
{
	unsigned attempt;

	if (report != NULL)
		memset(report, 0, sizeof(*report));
	if (ops == NULL || identity == NULL || config == NULL || handle == NULL ||
	    *handle == NULL || report == NULL || config->max_attempts == 0 ||
	    config->reopen_polls == 0 || ops->transfer == NULL ||
	    ops->close_dfu == NULL || ops->reopen_matching_dfu == NULL ||
	    ops->claim_dfu == NULL || ops->sleep_ms == NULL)
		return report == NULL ? DFU_TRANSFER_INVALID_ARGUMENT :
		       transfer_fail(report, DFU_TRANSFER_INVALID_ARGUMENT, 0);

	for (attempt = 1; attempt <= config->max_attempts; attempt++) {
		unsigned poll;
		int result;

		report->attempts = attempt;
		result = ops->transfer(context, *handle);
		if (result == 0) {
			report->result = DFU_TRANSFER_OK;
			return DFU_TRANSFER_OK;
		}
		report->transfer_error = result;
		ops->close_dfu(context, *handle, 1);
		*handle = NULL;
		if (attempt == config->max_attempts)
			return transfer_fail(report, DFU_TRANSFER_RETRY_EXHAUSTED, 0);

		for (poll = 0; poll < config->reopen_polls; poll++) {
			result = ops->reopen_matching_dfu(context, identity, handle);
			report->reopen_polls++;
			if (result == 1)
				break;
			if (result < 0)
				return transfer_fail(report, DFU_TRANSFER_REOPEN_FAILED,
				                     result);
			if (poll + 1 < config->reopen_polls)
				ops->sleep_ms(context, config->poll_interval_ms);
		}
		if (*handle == NULL)
			return transfer_fail(report, DFU_TRANSFER_REOPEN_TIMEOUT, 0);

		result = ops->claim_dfu(context, *handle);
		if (result != 0) {
			report->operation_error = result;
			ops->close_dfu(context, *handle, 0);
			*handle = NULL;
			return transfer_fail(report, DFU_TRANSFER_CLAIM_FAILED, result);
		}
	}

	return transfer_fail(report, DFU_TRANSFER_RETRY_EXHAUSTED, 0);
}

static int verify_fail(struct dfu_verify_report *report,
                       enum dfu_verify_result result,
                       int operation_error)
{
	report->result = result;
	report->operation_error = operation_error;
	return result;
}

int dfu_verify_application(const struct dfu_verify_ops *ops,
                           void *context,
                           const char *selector,
                           const struct dfu_usb_identity *expected_identity,
                           const char *expected_revision,
                           const char *expected_image,
                           const struct dfu_verify_config *config,
                           struct dfu_verify_report *report)
{
	unsigned poll;

	if (report != NULL)
		memset(report, 0, sizeof(*report));
	if (ops == NULL || selector == NULL || expected_identity == NULL ||
	    expected_revision == NULL || expected_image == NULL ||
	    config == NULL || report == NULL ||
	    config->polls == 0 || ops->open_application == NULL ||
	    ops->get_revision == NULL || ops->get_image == NULL ||
	    ops->close_application == NULL || ops->sleep_ms == NULL)
		return report == NULL ? DFU_VERIFY_INVALID_ARGUMENT :
		       verify_fail(report, DFU_VERIFY_INVALID_ARGUMENT, 0);

	for (poll = 0; poll < config->polls; poll++) {
		struct dfu_usb_identity identity;
		void *handle = NULL;
		int result;

		memset(&identity, 0, sizeof(identity));
		report->polls++;
		result = ops->open_application(context, selector, &handle, &identity);
		if (result != 1) {
			/* Each poll overwrites this, so what is left at the end is where
			 * the last one stood: an error if a device was there and would
			 * not open, 0 if there was nothing to open. */
			report->operation_error = result < 0 ? result : 0;
			if (poll + 1 < config->polls)
				ops->sleep_ms(context, config->poll_interval_ms);
			continue;
		}
		if (!dfu_usb_identity_equal(expected_identity, &identity)) {
			ops->close_application(context, handle);
			return verify_fail(report, DFU_VERIFY_IDENTITY_MISMATCH, 0);
		}
		result = ops->get_revision(context, handle,
		                           report->actual_revision,
		                           sizeof(report->actual_revision));
		if (result != 0) {
			ops->close_application(context, handle);
			return verify_fail(report, DFU_VERIFY_REVISION_FAILED, result);
		}
		/* The revision is asked first, because it is the question "did the
		 * write take at all": a board still running what it had reports a
		 * different one, and that is the case worth naming on its own. The
		 * image name then separates the two images that share a revision. */
		if (strcmp(report->actual_revision, expected_revision) != 0) {
			ops->close_application(context, handle);
			return verify_fail(report, DFU_VERIFY_REVISION_MISMATCH, 0);
		}
		result = ops->get_image(context, handle, report->actual_image,
		                        sizeof(report->actual_image));
		ops->close_application(context, handle);
		if (result != 0)
			return verify_fail(report, DFU_VERIFY_IMAGE_FAILED, result);
		if (strcmp(report->actual_image, expected_image) != 0)
			return verify_fail(report, DFU_VERIFY_IMAGE_MISMATCH, 0);
		report->result = DFU_VERIFY_OK;
		return DFU_VERIFY_OK;
	}

	return verify_fail(report, DFU_VERIFY_APP_TIMEOUT,
	                   report->operation_error);
}

void dfu_verify_describe_failure(const struct dfu_verify_report *report,
                                 const char *expected_revision,
                                 const char *expected_image,
                                 char *message, size_t message_size)
{
	const char *expected = expected_revision != NULL ? expected_revision : "";
	const char *expected_name = expected_image != NULL ? expected_image : "";

	if (message == NULL || message_size == 0)
		return;
	if (report == NULL) {
		snprintf(message, message_size,
		         "the flash could not be checked after the reset");
		return;
	}
	/* The switch covers every enumerator; this only catches a value that is
	 * not one of them. */
	snprintf(message, message_size,
	         "the flash could not be checked after the reset (result %d)",
	         (int)report->result);
	switch (report->result) {
	case DFU_VERIFY_OK:
		snprintf(message, message_size, "the board came back running %s",
		         report->actual_revision);
		break;
	case DFU_VERIFY_INVALID_ARGUMENT:
		snprintf(message, message_size,
		         "the flash could not be checked after the reset");
		break;
	case DFU_VERIFY_APP_TIMEOUT:
		/* The write completed before the reset, so the board holds the new
		 * firmware whether or not it answered afterwards. Saying so is the
		 * point: the obvious reaction to a bare failure is to flash it
		 * again, which is what this board does not need.
		 *
		 * Two boards end up here, and only one of them is missing. The other
		 * is on the bus and will not open, because something else holds its
		 * interface or this user may not have it, and telling that board to
		 * power-cycle would be a wrong diagnosis of the same kind this line
		 * exists to stop. */
		if (report->operation_error == 0)
			snprintf(message, message_size,
			         "the firmware was written, but the board did not come "
			         "back on USB: power-cycle it and read its revision with "
			         "ubertooth-util -v before flashing it again");
		else
			snprintf(message, message_size,
			         "the firmware was written, but the board came back and "
			         "would not open, so the flash was not checked: read its "
			         "revision with ubertooth-util -v once nothing else is "
			         "using it");
		break;
	case DFU_VERIFY_IDENTITY_MISMATCH:
		snprintf(message, message_size,
		         "the device that came back is not on the flashed board's USB "
		         "path, so the flash was not checked: read the board's "
		         "revision with ubertooth-util -v");
		break;
	case DFU_VERIFY_REVISION_FAILED:
		snprintf(message, message_size,
		         "the board came back but would not report its firmware "
		         "revision, so the flash was not checked: read it with "
		         "ubertooth-util -v");
		break;
	case DFU_VERIFY_REVISION_MISMATCH:
		snprintf(message, message_size,
		         "the board came back running %s, not %s: the firmware was "
		         "not written, so flash it again",
		         report->actual_revision, expected);
		break;
	case DFU_VERIFY_IMAGE_FAILED:
		/* The revision was read off the board and matched, so the write took
		 * and the line must not say otherwise. What is missing is only which
		 * of this tree's images answered, and no command can read that back
		 * either: ubertooth-util -v falls back to the revision on a board
		 * that will not answer the block. So the line says exactly what was
		 * and was not established, and sends the reader nowhere. */
		snprintf(message, message_size,
		         "the board came back running %s, the revision that was "
		         "written, but would not say which firmware image it is, so "
		         "which of this tree's images it runs is unchecked",
		         report->actual_revision);
		break;
	case DFU_VERIFY_IMAGE_MISMATCH:
		/* Both images of one tree carry the same revision, so this is the
		 * only thing that catches a write that did not take between them. */
		snprintf(message, message_size,
		         "the board came back running the %s firmware, not the %s "
		         "image that was written: the firmware was not written, so "
		         "flash it again",
		         report->actual_image, expected_name);
		break;
	}
}

unsigned long dfu_max_image_size(void)
{
	const struct dfu_part *part;
	uint32_t largest = DFU_ASSUMED_FLASH_SIZE;
	size_t i;

	for (i = 0; (part = dfu_part_at(i)) != NULL; i++) {
		if (part->flash_size > largest)
			largest = part->flash_size;
	}
	/* The bootloader refuses an address at or above its own limit, so flash
	 * past it cannot be written however much a part has. */
	if (largest > BOOTLOADER_FLASH_LIMIT)
		largest = BOOTLOADER_FLASH_LIMIT;
	/* The image is the application region plus the DFU suffix the file
	 * carries and the bootloader never sees. */
	return (unsigned long)dfu_upload_length(largest) + DFU_SUFFIX_LENGTH;
}

enum dfu_file_size_result dfu_image_size_check(FILE *file,
                                               unsigned long maximum,
                                               long long *size)
{
	struct stat state;
	int descriptor;

	if (size != NULL)
		*size = 0;
	if (file == NULL)
		return DFU_FILE_SIZE_INVALID_ARGUMENT;
	descriptor = fileno(file);
	if (descriptor < 0 || fstat(descriptor, &state) != 0)
		return DFU_FILE_SIZE_UNKNOWN;
	/* A directory, a FIFO, a device node or a socket: its length says
	 * nothing about how much a read would return, so there is no size to
	 * allocate from. */
	if (!S_ISREG(state.st_mode))
		return DFU_FILE_SIZE_NOT_REGULAR;
	if (state.st_size < 0)
		return DFU_FILE_SIZE_UNKNOWN;
	if (size != NULL)
		*size = (long long)state.st_size;
	if ((unsigned long long)state.st_size > maximum)
		return DFU_FILE_SIZE_TOO_LARGE;
	return DFU_FILE_SIZE_OK;
}

int dfu_read_image_info(FILE *image, struct ubertooth_fw_info *info)
{
	uint8_t raw[UBERTOOTH_FW_INFO_LENGTH];
	long original_position;
	int result;

	if (info != NULL)
		memset(info, 0, sizeof(*info));
	if (image == NULL || info == NULL)
		return UBERTOOTH_FW_INFO_SHORT;
	/* The block sits at a fixed offset, so only those 64 bytes are read: no
	 * allocation, and no size check beyond what the file holds. */
	original_position = ftell(image);
	if (original_position < 0 ||
	    fseek(image, UBERTOOTH_FW_INFO_OFFSET, SEEK_SET) != 0)
		return UBERTOOTH_FW_INFO_SHORT;
	if (fread(raw, 1, sizeof(raw), image) != sizeof(raw))
		result = UBERTOOTH_FW_INFO_SHORT;
	else
		result = ubertooth_fw_info_parse(raw, sizeof(raw), info);
	fseek(image, original_position, SEEK_SET);
	return result;
}

/*
 * CRC-32 as the DFU 1.1 suffix uses it (the zlib/IEEE 802.3 CRC: reflected
 * polynomial 0xedb88320, initial value 0xffffffff, no final XOR). The 256-entry
 * lookup table is built on first use instead of being spelled out: entry n is
 * the CRC of the single byte n.
 */
#define DFU_CRC32_POLY 0xedb88320u

static uint32_t dfu_crc_table[256];
static int dfu_crc_table_ready;

static void dfu_crc_table_init(void)
{
	uint32_t n, c;
	int k;

	for (n = 0; n < 256; n++) {
		c = n;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? DFU_CRC32_POLY ^ (c >> 1) : c >> 1;
		dfu_crc_table[n] = c;
	}
	dfu_crc_table_ready = 1;
}

uint32_t dfu_image_crc32(const uint8_t *data, size_t length)
{
	uint32_t crc = 0xffffffff;
	size_t i;

	if (!dfu_crc_table_ready)
		dfu_crc_table_init();
	for (i = 0; i < length; i++)
		crc = (crc >> 8) ^ dfu_crc_table[(crc ^ data[i]) & 0xff];
	return crc;
}

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int image_result(struct dfu_image_report *report,
                        enum dfu_image_result result)
{
	report->result = result;
	return result;
}

/* The suffix may name this device, or 0xffff for any device (DFU 1.1). */
static int id_accepted(uint16_t found, uint16_t expected)
{
	return found == expected || found == DFU_ID_WILDCARD;
}

int dfu_image_check(FILE *image, uint16_t id_vendor, uint16_t id_product,
                    struct dfu_image_report *report)
{
	uint8_t *data = NULL;
	const uint8_t *suffix;
	long original_position = -1, image_size;
	long long measured_size;
	int result;

	if (report == NULL)
		return DFU_IMAGE_INVALID_ARGUMENT;
	memset(report, 0, sizeof(*report));
	report->expected_vendor = id_vendor;
	report->expected_product = id_product;
	if (image == NULL)
		return image_result(report, DFU_IMAGE_INVALID_ARGUMENT);

	/* The whole file is read into one allocation below, so what it is and
	 * how big it is are settled before anything is allocated. */
	switch (dfu_image_size_check(image, dfu_max_image_size(),
	                             &measured_size)) {
	case DFU_FILE_SIZE_OK:
		break;
	case DFU_FILE_SIZE_NOT_REGULAR:
		return image_result(report, DFU_IMAGE_NOT_REGULAR);
	case DFU_FILE_SIZE_TOO_LARGE:
		report->image_size = measured_size;
		return image_result(report, DFU_IMAGE_TOO_LARGE);
	default:
		return image_result(report, DFU_IMAGE_UNREADABLE);
	}

	original_position = ftell(image);
	if (original_position < 0 || fseek(image, 0, SEEK_END) != 0)
		return image_result(report, DFU_IMAGE_UNREADABLE);
	image_size = ftell(image);
	report->image_size = image_size;
	if (image_size < 0) {
		result = image_result(report, DFU_IMAGE_UNREADABLE);
		goto done;
	}
	/* The allocation is sized from this read rather than from the check
	 * above, so it gets the cap too: a file a writer is still growing
	 * cannot slip past between the two. */
	if ((unsigned long)image_size > dfu_max_image_size()) {
		result = image_result(report, DFU_IMAGE_TOO_LARGE);
		goto done;
	}
	if (image_size < DFU_SUFFIX_LENGTH) {
		result = image_result(report, DFU_IMAGE_TOO_SHORT);
		goto done;
	}
	if (fseek(image, 0, SEEK_SET) != 0) {
		result = image_result(report, DFU_IMAGE_UNREADABLE);
		goto done;
	}
	data = malloc((size_t)image_size);
	if (data == NULL) {
		result = image_result(report, DFU_IMAGE_OUT_OF_MEMORY);
		goto done;
	}
	if (fread(data, 1, (size_t)image_size, image) != (size_t)image_size) {
		result = image_result(report, DFU_IMAGE_UNREADABLE);
		goto done;
	}

	suffix = data + image_size - DFU_SUFFIX_LENGTH;
	report->suffix_length = suffix[DFU_SUFFIX_OFF_BLENGTH];
	report->bcd_dfu = read_le16(suffix + DFU_SUFFIX_OFF_BCDDFU);
	report->id_vendor = read_le16(suffix + DFU_SUFFIX_OFF_IDVENDOR);
	report->id_product = read_le16(suffix + DFU_SUFFIX_OFF_IDPRODUCT);
	report->crc_found = read_le32(suffix + DFU_SUFFIX_OFF_DWCRC);
	/* The suffix CRC covers the whole file except the four CRC bytes. */
	report->crc_calculated = dfu_image_crc32(data, (size_t)image_size - 4);

	if (report->suffix_length != DFU_SUFFIX_LENGTH)
		result = image_result(report, DFU_IMAGE_SUFFIX_LENGTH);
	else if (report->bcd_dfu != DFU_VERSION_1_0 &&
	         report->bcd_dfu != DFU_VERSION_1_1)
		result = image_result(report, DFU_IMAGE_DFU_VERSION);
	else if (!(suffix[DFU_SUFFIX_OFF_UCDFUSIG + 0] == 0x55 &&
	           suffix[DFU_SUFFIX_OFF_UCDFUSIG + 1] == 0x46 &&
	           suffix[DFU_SUFFIX_OFF_UCDFUSIG + 2] == 0x44))
		result = image_result(report, DFU_IMAGE_SIGNATURE);
	else if (image_size == DFU_SUFFIX_LENGTH)
		result = image_result(report, DFU_IMAGE_NO_FIRMWARE);
	else if (report->crc_calculated != report->crc_found)
		result = image_result(report, DFU_IMAGE_CRC);
	else if (!id_accepted(report->id_vendor, id_vendor))
		result = image_result(report, DFU_IMAGE_VENDOR);
	else if (!id_accepted(report->id_product, id_product))
		result = image_result(report, DFU_IMAGE_PRODUCT);
	else
		result = image_result(report, DFU_IMAGE_OK);

done:
	free(data);
	if (original_position >= 0)
		fseek(image, original_position, SEEK_SET);
	return result;
}

void dfu_image_describe_failure(const struct dfu_image_report *report,
                                char *message, size_t message_size)
{
	if (message == NULL || message_size == 0)
		return;
	if (report == NULL) {
		snprintf(message, message_size, "DFU image check did not run");
		return;
	}
	/* The switch covers every enumerator; this only catches a value that is
	 * not one of them. */
	snprintf(message, message_size, "DFU image rejected (result %d)",
	         (int)report->result);
	switch (report->result) {
	case DFU_IMAGE_OK:
		snprintf(message, message_size, "DFU image accepted");
		break;
	case DFU_IMAGE_INVALID_ARGUMENT:
		snprintf(message, message_size, "DFU image check did not run");
		break;
	case DFU_IMAGE_TOO_SHORT:
		snprintf(message, message_size,
		         "Too short for a %d-byte DFU suffix: the file is %lld byte(s)",
		         DFU_SUFFIX_LENGTH, report->image_size);
		break;
	case DFU_IMAGE_TOO_LARGE:
		snprintf(message, message_size,
		         "The file is %lld bytes; an Ubertooth image is at most %lu",
		         report->image_size, dfu_max_image_size());
		break;
	case DFU_IMAGE_NOT_REGULAR:
		snprintf(message, message_size, "Not a regular file");
		break;
	case DFU_IMAGE_UNREADABLE:
		snprintf(message, message_size, "Could not read the whole file");
		break;
	case DFU_IMAGE_OUT_OF_MEMORY:
		snprintf(message, message_size,
		         "Cannot allocate %lld byte(s) to check the image",
		         report->image_size);
		break;
	case DFU_IMAGE_SUFFIX_LENGTH:
		snprintf(message, message_size, "Unknown DFU suffix length: %u",
		         report->suffix_length);
		break;
	case DFU_IMAGE_DFU_VERSION:
		snprintf(message, message_size, "Unknown DFU version: %04x",
		         report->bcd_dfu);
		break;
	case DFU_IMAGE_SIGNATURE:
		snprintf(message, message_size,
		         "DFU Signature mismatch: not a DFU file");
		break;
	case DFU_IMAGE_NO_FIRMWARE:
		snprintf(message, message_size,
		         "DFU suffix with no firmware: nothing to write");
		break;
	case DFU_IMAGE_CRC:
		snprintf(message, message_size,
		         "CRC mismatch: calculated: 0x%x, found: 0x%x",
		         report->crc_calculated, report->crc_found);
		break;
	case DFU_IMAGE_VENDOR:
		snprintf(message, message_size,
		         "DFU suffix is for USB vendor 0x%04x, not 0x%04x: "
		         "this image is for another device",
		         report->id_vendor, report->expected_vendor);
		break;
	case DFU_IMAGE_PRODUCT:
		snprintf(message, message_size,
		         "DFU suffix is for USB product 0x%04x, not 0x%04x: "
		         "this image is for another device",
		         report->id_product, report->expected_product);
		break;
	}
}

/* The LPC175x/176x parts and the flash each one has, from the part
 * identification numbers in the NXP LPC176x/5x user manual (UM10360, "Part
 * identification numbers"). The Ubertooth One schematic specifies an LPC1754
 * (128 KiB), other Ubertooth designs use other LPC175x parts, and boards in
 * the field differ (one reports an LPC1754; others read back more than
 * 128 KiB), so the table covers the family rather than one part.
 *
 * An entry that claims more flash than the part has is what wedges a board,
 * so these figures are the load-bearing part of this file. The bootloader
 * keeps the same table, without the names, and bounds its own read-back by it
 * (firmware/bootloader/flash.h); a firmware unit test reads both tables and
 * fails if they disagree. The largest here, 512 KiB, is also what a
 * bootloader falls back to for a part it has no entry for, and what older
 * bootloaders allow on every part. */
static const struct dfu_part DFU_PARTS[] = {
	{ 0x25001110, "LPC1751",  32 * 1024 },
	{ 0x25001118, "LPC1751",  32 * 1024 },
	{ 0x25001121, "LPC1752",  64 * 1024 },
	{ 0x25011722, "LPC1754", 128 * 1024 },
	{ 0x25011723, "LPC1756", 256 * 1024 },
	{ 0x25013F37, "LPC1758", 512 * 1024 },
	{ 0x25113737, "LPC1759", 512 * 1024 },
	{ 0x26012033, "LPC1763", 256 * 1024 },
	{ 0x26011922, "LPC1764", 128 * 1024 },
	{ 0x26013733, "LPC1765", 256 * 1024 },
	{ 0x26013F33, "LPC1766", 256 * 1024 },
	{ 0x26012837, "LPC1767", 512 * 1024 },
	{ 0x26013F37, "LPC1768", 512 * 1024 },
	{ 0x26113F37, "LPC1769", 512 * 1024 },
};

#define DFU_PART_COUNT (sizeof(DFU_PARTS) / sizeof(DFU_PARTS[0]))

const struct dfu_part *dfu_part_lookup(uint32_t part_id)
{
	size_t i;

	for (i = 0; i < DFU_PART_COUNT; i++) {
		if (DFU_PARTS[i].part_id == part_id)
			return &DFU_PARTS[i];
	}
	return NULL;
}

const struct dfu_part *dfu_part_at(size_t index)
{
	return index < DFU_PART_COUNT ? &DFU_PARTS[index] : NULL;
}

uint32_t dfu_upload_length(uint32_t flash_size)
{
	const uint32_t application = BOOTLOADER_OFFSET + BOOTLOADER_SIZE;

	if (flash_size == 0)
		flash_size = DFU_ASSUMED_FLASH_SIZE;
	/* A part whose flash ends at or before the application region has
	 * nothing to read back: say so rather than asking for one block of it. */
	if (flash_size <= application)
		return 0;
	return flash_size - application;
}

unsigned long dfu_download_length(unsigned long image_size)
{
	/* The last block is padded with 0xff and written in full, so a file that
	 * ends part-way through a block still costs the whole block of flash.
	 *
	 * This is a block larger than dfu_max_image_size() assumes: that cap is
	 * the largest part's application region plus the suffix, as if the suffix
	 * cost nothing, so an image at the cap needs one block more flash than
	 * even the largest part has and is refused on every part this host can
	 * identify. Refusing is the right answer -- the block really would not
	 * fit -- so the cap is left as the bound on what is read into memory,
	 * which is all it is for. */
	return (image_size + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;
}
