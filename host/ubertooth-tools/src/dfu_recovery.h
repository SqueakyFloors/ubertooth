#ifndef UBERTOOTH_DFU_RECOVERY_H
#define UBERTOOTH_DFU_RECOVERY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "dfu_transition.h"

enum dfu_transfer_result {
	DFU_TRANSFER_OK = 0,
	DFU_TRANSFER_INVALID_ARGUMENT = -1,
	DFU_TRANSFER_REOPEN_FAILED = -2,
	DFU_TRANSFER_REOPEN_TIMEOUT = -3,
	DFU_TRANSFER_CLAIM_FAILED = -4,
	DFU_TRANSFER_RETRY_EXHAUSTED = -5,
};

struct dfu_transfer_config {
	unsigned max_attempts;
	unsigned reopen_polls;
	unsigned poll_interval_ms;
};

struct dfu_transfer_report {
	enum dfu_transfer_result result;
	unsigned attempts;
	unsigned reopen_polls;
	int transfer_error;
	int operation_error;
};

struct dfu_transfer_ops {
	int (*download)(void *context, void *handle);
	void (*close_dfu)(void *context, void *handle, int claimed);
	/* Return 1 when found, 0 when absent, and a negative value on error. */
	int (*reopen_matching_dfu)(void *context,
	                           const struct dfu_usb_identity *identity,
	                           void **handle);
	int (*claim_dfu)(void *context, void *handle);
	void (*sleep_ms)(void *context, unsigned milliseconds);
};

int dfu_transfer_run(const struct dfu_transfer_ops *ops,
                     void *context,
                     const struct dfu_usb_identity *identity,
                     const struct dfu_transfer_config *config,
                     void **handle,
                     struct dfu_transfer_report *report);

enum dfu_verify_result {
	DFU_VERIFY_OK = 0,
	DFU_VERIFY_INVALID_ARGUMENT = -1,
	DFU_VERIFY_APP_TIMEOUT = -2,
	DFU_VERIFY_IDENTITY_MISMATCH = -3,
	DFU_VERIFY_REVISION_FAILED = -4,
	DFU_VERIFY_REVISION_MISMATCH = -5,
};

struct dfu_verify_config {
	unsigned polls;
	unsigned poll_interval_ms;
};

struct dfu_verify_report {
	enum dfu_verify_result result;
	unsigned polls;
	int operation_error;
	char actual_revision[64];
};

struct dfu_verify_ops {
	int (*open_application)(void *context, const char *selector,
	                        void **handle,
	                        struct dfu_usb_identity *identity);
	int (*get_revision)(void *context, void *handle,
	                    char *revision, size_t revision_size);
	void (*close_application)(void *context, void *handle);
	void (*sleep_ms)(void *context, unsigned milliseconds);
};

int dfu_verify_application(const struct dfu_verify_ops *ops,
                           void *context,
                           const char *selector,
                           const struct dfu_usb_identity *expected_identity,
                           const char *expected_revision,
                           const struct dfu_verify_config *config,
                           struct dfu_verify_report *report);

int dfu_extract_firmware_revision(FILE *image, char *revision,
                                  size_t revision_size);

/* Why an image is not firmware this device can be given. Checked in the order
 * listed, so the first thing wrong with a file is the thing reported. */
enum dfu_image_result {
	DFU_IMAGE_OK = 0,
	DFU_IMAGE_INVALID_ARGUMENT = -1,
	DFU_IMAGE_TOO_SHORT = -2,       /* too short to hold a suffix at all */
	DFU_IMAGE_SUFFIX_LENGTH = -3,   /* bLength is not 16: no DFU suffix here */
	DFU_IMAGE_DFU_VERSION = -4,
	DFU_IMAGE_SIGNATURE = -5,
	DFU_IMAGE_NO_FIRMWARE = -6,     /* a suffix and nothing to write */
	DFU_IMAGE_CRC = -7,
	DFU_IMAGE_VENDOR = -8,
	DFU_IMAGE_PRODUCT = -9,
	DFU_IMAGE_UNREADABLE = -10,     /* the file would not read */
	DFU_IMAGE_OUT_OF_MEMORY = -11,
};

struct dfu_image_report {
	enum dfu_image_result result;
	long image_size;
	uint8_t suffix_length;
	uint16_t bcd_dfu;
	uint16_t id_vendor;
	uint16_t id_product;
	uint16_t expected_vendor;
	uint16_t expected_product;
	uint32_t crc_calculated;
	uint32_t crc_found;
};

/* The DFU suffix CRC32, over length bytes of data. */
uint32_t dfu_image_crc32(const uint8_t *data, size_t length);

/* Check that image is a DFU file for this device: suffix length, DFU version,
 * signature, a non-empty payload, the suffix CRC, and idVendor/idProduct
 * (id_vendor/id_product, or the 0xffff wildcard). Reads the whole file and
 * restores its position. Returns DFU_IMAGE_OK or a negative
 * enum dfu_image_result, also left in report->result. */
int dfu_image_check(FILE *image, uint16_t id_vendor, uint16_t id_product,
                    struct dfu_image_report *report);

/* One line saying what is wrong with the image described by report, for a
 * result other than DFU_IMAGE_OK. */
void dfu_image_describe_failure(const struct dfu_image_report *report,
                                char *message, size_t message_size);

#endif /* UBERTOOTH_DFU_RECOVERY_H */
