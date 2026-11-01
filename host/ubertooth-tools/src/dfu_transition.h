/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hardware-independent application-to-DFU transition executor.
 *
 * Platform USB operations are injected by the caller so state handling can be
 * tested without an attached Ubertooth.
 */
#ifndef UBERTOOTH_DFU_TRANSITION_H
#define UBERTOOTH_DFU_TRANSITION_H

#include <stdint.h>

#define DFU_USB_PATH_MAX 8
#define DFU_ENTRY_DEFAULT_MAX_ATTEMPTS 2
#define DFU_ENTRY_DEFAULT_POLLS_PER_ATTEMPT 25
#define DFU_ENTRY_DEFAULT_POLL_INTERVAL_MS 200

struct dfu_usb_identity {
	uint8_t bus_number;
	uint8_t port_count;
	uint8_t port_numbers[DFU_USB_PATH_MAX];
};

enum dfu_transition_state {
	DFU_STATE_IDLE = 0,
	DFU_STATE_APP_SELECTED,
	DFU_STATE_WAITING_FOR_DFU,
	DFU_STATE_DFU_FOUND,
	DFU_STATE_FAILED,
};

enum dfu_transition_result {
	DFU_TRANSITION_OK = 0,
	DFU_TRANSITION_INVALID_ARGUMENT = -1,
	DFU_TRANSITION_APP_OPEN_FAILED = -2,
	DFU_TRANSITION_REQUEST_FAILED = -3,
	DFU_TRANSITION_DISCOVERY_FAILED = -4,
	DFU_TRANSITION_DFU_TIMEOUT = -5,
	DFU_TRANSITION_IDENTITY_CHANGED = -6,
};

struct dfu_transition_config {
	unsigned max_attempts;
	unsigned polls_per_attempt;
	unsigned poll_interval_ms;
};

struct dfu_transition_report {
	enum dfu_transition_state state;
	enum dfu_transition_result result;
	unsigned attempts;
	unsigned polls;
	int operation_error;
	struct dfu_usb_identity identity;
};

struct dfu_transition_ops {
	/* Return 0 when the application was opened and non-zero when it was not.
	 * Note the verification's member of the same name and signature
	 * (dfu_verify_ops in dfu_recovery.h) answers 1 for success, because it
	 * also has to say whether the device was there at all. */
	int (*open_application)(void *context, const char *selector,
	                        void **handle,
	                        struct dfu_usb_identity *identity);
	int (*request_dfu)(void *context, void *handle);
	void (*close_application)(void *context, void *handle);
	/* Return 1 when found, 0 when absent, and a negative value on error. */
	int (*open_matching_dfu)(void *context,
	                         const struct dfu_usb_identity *identity,
	                         void **handle);
	void (*sleep_ms)(void *context, unsigned milliseconds);
};

int dfu_selector_allows_unidentified_dfu(const char *selector);

int dfu_usb_identity_equal(const struct dfu_usb_identity *left,
                           const struct dfu_usb_identity *right);

int dfu_transition_run(const struct dfu_transition_ops *ops,
                       void *context,
                       const char *selector,
                       const struct dfu_transition_config *config,
                       void **dfu_handle,
                       struct dfu_transition_report *report);

const char *dfu_transition_result_string(enum dfu_transition_result result);

#endif /* UBERTOOTH_DFU_TRANSITION_H */
