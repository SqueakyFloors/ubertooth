#include "dfu_recovery.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dfu.h"

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
	    config->reopen_polls == 0 || ops->download == NULL ||
	    ops->close_dfu == NULL || ops->reopen_matching_dfu == NULL ||
	    ops->claim_dfu == NULL || ops->sleep_ms == NULL)
		return report == NULL ? DFU_TRANSFER_INVALID_ARGUMENT :
		       transfer_fail(report, DFU_TRANSFER_INVALID_ARGUMENT, 0);

	for (attempt = 1; attempt <= config->max_attempts; attempt++) {
		unsigned poll;
		int result;

		report->attempts = attempt;
		result = ops->download(context, *handle);
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
                           const struct dfu_verify_config *config,
                           struct dfu_verify_report *report)
{
	unsigned poll;

	if (report != NULL)
		memset(report, 0, sizeof(*report));
	if (ops == NULL || selector == NULL || expected_identity == NULL ||
	    expected_revision == NULL || config == NULL || report == NULL ||
	    config->polls == 0 || ops->open_application == NULL ||
	    ops->get_revision == NULL || ops->close_application == NULL ||
	    ops->sleep_ms == NULL)
		return report == NULL ? DFU_VERIFY_INVALID_ARGUMENT :
		       verify_fail(report, DFU_VERIFY_INVALID_ARGUMENT, 0);

	for (poll = 0; poll < config->polls; poll++) {
		struct dfu_usb_identity identity;
		void *handle = NULL;
		int result;

		memset(&identity, 0, sizeof(identity));
		report->polls++;
		result = ops->open_application(context, selector, &handle, &identity);
		if (result != 0) {
			report->operation_error = result;
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
		ops->close_application(context, handle);
		if (result != 0)
			return verify_fail(report, DFU_VERIFY_REVISION_FAILED, result);
		if (strcmp(report->actual_revision, expected_revision) != 0)
			return verify_fail(report, DFU_VERIFY_REVISION_MISMATCH, 0);
		report->result = DFU_VERIFY_OK;
		return DFU_VERIFY_OK;
	}

	return verify_fail(report, DFU_VERIFY_APP_TIMEOUT,
	                   report->operation_error);
}

int dfu_extract_firmware_revision(FILE *image, char *revision,
                                  size_t revision_size)
{
	static const unsigned char marker[] = "ubertooth ";
	unsigned char *data = NULL;
	long original_position, image_size;
	size_t marker_size = sizeof(marker) - 1;
	int result = -1;
	long i;

	if (image == NULL || revision == NULL || revision_size < 2)
		return -1;
	original_position = ftell(image);
	if (original_position < 0 || fseek(image, 0, SEEK_END) != 0)
		return -1;
	image_size = ftell(image);
	if (image_size <= 0 || fseek(image, 0, SEEK_SET) != 0)
		goto done;
	data = malloc((size_t)image_size);
	if (data == NULL)
		goto done;
	if (fread(data, 1, (size_t)image_size, image) != (size_t)image_size)
		goto done;

	for (i = 0; i + (long)marker_size < image_size; i++) {
		long start, end;
		if (memcmp(data + i, marker, marker_size) != 0)
			continue;
		start = i + (long)marker_size;
		for (end = start; end + 1 < image_size; end++) {
			if (data[end] == ' ' && data[end + 1] == '(') {
				size_t length = (size_t)(end - start);
				if (length > 0 && length < revision_size) {
					memcpy(revision, data + start, length);
					revision[length] = '\0';
					result = 0;
				}
				goto done;
			}
			if (data[end] == '\0' || data[end] < 0x20 || data[end] > 0x7e)
				break;
		}
	}

done:
	free(data);
	if (original_position >= 0)
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
	int result;

	if (report == NULL)
		return DFU_IMAGE_INVALID_ARGUMENT;
	memset(report, 0, sizeof(*report));
	report->expected_vendor = id_vendor;
	report->expected_product = id_product;
	if (image == NULL)
		return image_result(report, DFU_IMAGE_INVALID_ARGUMENT);

	original_position = ftell(image);
	if (original_position < 0 || fseek(image, 0, SEEK_END) != 0)
		return image_result(report, DFU_IMAGE_UNREADABLE);
	image_size = ftell(image);
	report->image_size = image_size;
	if (image_size < 0) {
		result = image_result(report, DFU_IMAGE_UNREADABLE);
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
		         "Too short for a %d-byte DFU suffix: the file is %ld byte(s)",
		         DFU_SUFFIX_LENGTH, report->image_size);
		break;
	case DFU_IMAGE_UNREADABLE:
		snprintf(message, message_size, "Could not read the whole file");
		break;
	case DFU_IMAGE_OUT_OF_MEMORY:
		snprintf(message, message_size,
		         "Cannot allocate %ld byte(s) to check the image",
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
