/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dfu_transition.h"

#include <stddef.h>
#include <string.h>

static int fail_transition(struct dfu_transition_report *report,
                           enum dfu_transition_result result,
                           int operation_error)
{
	report->state = DFU_STATE_FAILED;
	report->result = result;
	report->operation_error = operation_error;
	return result;
}

int dfu_selector_allows_unidentified_dfu(const char *selector)
{
	return selector == NULL || selector[0] == '\0';
}

int dfu_usb_identity_equal(const struct dfu_usb_identity *left,
                           const struct dfu_usb_identity *right)
{
	if (left == NULL || right == NULL)
		return 0;
	if (left->port_count > DFU_USB_PATH_MAX ||
	    right->port_count > DFU_USB_PATH_MAX)
		return 0;
	return left->bus_number == right->bus_number &&
	       left->port_count == right->port_count &&
	       memcmp(left->port_numbers, right->port_numbers,
	              left->port_count) == 0;
}

int dfu_transition_run(const struct dfu_transition_ops *ops,
                       void *context,
                       const char *selector,
                       const struct dfu_transition_config *config,
                       void **dfu_handle,
                       struct dfu_transition_report *report)
{
	unsigned attempt;
	void *app_handle;
	struct dfu_usb_identity current_identity;
	int operation_result;

	if (report != NULL)
		memset(report, 0, sizeof(*report));
	if (dfu_handle != NULL)
		*dfu_handle = NULL;
	if (ops == NULL || config == NULL || dfu_handle == NULL ||
	    report == NULL || config->max_attempts == 0 ||
	    config->polls_per_attempt == 0 ||
	    ops->open_application == NULL || ops->request_dfu == NULL ||
	    ops->close_application == NULL || ops->open_matching_dfu == NULL ||
	    ops->sleep_ms == NULL)
		return report == NULL ? DFU_TRANSITION_INVALID_ARGUMENT :
		       fail_transition(report, DFU_TRANSITION_INVALID_ARGUMENT, 0);

	report->state = DFU_STATE_IDLE;
	for (attempt = 1; attempt <= config->max_attempts; attempt++) {
		unsigned poll;
		app_handle = NULL;
		memset(&current_identity, 0, sizeof(current_identity));
		report->attempts = attempt;
		operation_result = ops->open_application(
			context, selector, &app_handle, &current_identity);
		if (operation_result != 0)
			return fail_transition(report,
			                       DFU_TRANSITION_APP_OPEN_FAILED,
			                       operation_result);
		if (attempt == 1) {
			report->identity = current_identity;
		} else if (!dfu_usb_identity_equal(&report->identity,
		                                   &current_identity)) {
			ops->close_application(context, app_handle);
			return fail_transition(report,
			                       DFU_TRANSITION_IDENTITY_CHANGED, 0);
		}

		report->state = DFU_STATE_APP_SELECTED;
		operation_result = ops->request_dfu(context, app_handle);
		ops->close_application(context, app_handle);
		app_handle = NULL;
		if (operation_result != 0)
			return fail_transition(report,
			                       DFU_TRANSITION_REQUEST_FAILED,
			                       operation_result);

		report->state = DFU_STATE_WAITING_FOR_DFU;
		for (poll = 0; poll < config->polls_per_attempt; poll++) {
			operation_result = ops->open_matching_dfu(
				context, &report->identity, dfu_handle);
			report->polls++;
			if (operation_result == 1) {
				report->state = DFU_STATE_DFU_FOUND;
				report->result = DFU_TRANSITION_OK;
				return DFU_TRANSITION_OK;
			}
			if (operation_result < 0)
				return fail_transition(
					report, DFU_TRANSITION_DISCOVERY_FAILED,
					operation_result);
			if (poll + 1 < config->polls_per_attempt ||
			    attempt < config->max_attempts)
				ops->sleep_ms(context, config->poll_interval_ms);
		}
	}

	return fail_transition(report, DFU_TRANSITION_DFU_TIMEOUT, 0);
}

const char *dfu_transition_result_string(enum dfu_transition_result result)
{
	switch (result) {
	case DFU_TRANSITION_OK:
		return "DFU device found";
	case DFU_TRANSITION_INVALID_ARGUMENT:
		return "invalid DFU transition configuration";
	case DFU_TRANSITION_APP_OPEN_FAILED:
		return "unable to open selected application device";
	case DFU_TRANSITION_REQUEST_FAILED:
		return "application rejected DFU entry request";
	case DFU_TRANSITION_DISCOVERY_FAILED:
		return "DFU device discovery failed";
	case DFU_TRANSITION_DFU_TIMEOUT:
		return "selected device did not enter DFU mode";
	case DFU_TRANSITION_IDENTITY_CHANGED:
		return "device selector resolved to a different USB path during retry";
	default:
		return "unknown DFU transition result";
	}
}
