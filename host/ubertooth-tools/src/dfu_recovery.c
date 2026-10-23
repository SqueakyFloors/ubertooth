#include "dfu_recovery.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
