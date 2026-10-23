#ifndef UBERTOOTH_DFU_RECOVERY_H
#define UBERTOOTH_DFU_RECOVERY_H

#include <stddef.h>
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

#endif /* UBERTOOTH_DFU_RECOVERY_H */
