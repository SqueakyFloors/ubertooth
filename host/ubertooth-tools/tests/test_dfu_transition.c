/*
 * Host tests for the application-to-DFU transition executor.
 * USB access and sleeping are injected so every transition and cleanup path is
 * deterministic and hardware-independent.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "dfu_transition.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

struct fake_context {
	int open_result;
	int request_result;
	int discovery_results[16];
	unsigned discovery_count;
	unsigned discovery_index;
	unsigned open_calls;
	unsigned request_calls;
	unsigned close_calls;
	unsigned sleep_calls;
	unsigned slept_ms;
	const char *selector;
	struct dfu_usb_identity identity;
	void *app_handle;
	void *dfu_handle;
	int change_identity_after_first_open;
};

static int fake_open_application(void *context, const char *selector,
                                 void **handle,
                                 struct dfu_usb_identity *identity)
{
	struct fake_context *fake = context;
	fake->open_calls++;
	fake->selector = selector;
	if (fake->open_result != 0)
		return fake->open_result;
	*handle = fake->app_handle;
	*identity = fake->identity;
	if (fake->change_identity_after_first_open && fake->open_calls > 1)
		identity->port_numbers[identity->port_count - 1]++;
	return 0;
}

static int fake_request_dfu(void *context, void *handle)
{
	struct fake_context *fake = context;
	fake->request_calls++;
	mu_assert(handle == fake->app_handle, "request used wrong app handle");
	return fake->request_result;
}

static void fake_close_application(void *context, void *handle)
{
	struct fake_context *fake = context;
	fake->close_calls++;
	mu_assert(handle == fake->app_handle, "close used wrong app handle");
}

static int fake_open_matching_dfu(void *context,
                                  const struct dfu_usb_identity *identity,
                                  void **handle)
{
	struct fake_context *fake = context;
	int result;
	mu_assert(dfu_usb_identity_equal(identity, &fake->identity),
	          "discovery used wrong physical identity");
	if (fake->discovery_index >= fake->discovery_count)
		return 0;
	result = fake->discovery_results[fake->discovery_index++];
	if (result == 1)
		*handle = fake->dfu_handle;
	return result;
}

static void fake_sleep_ms(void *context, unsigned milliseconds)
{
	struct fake_context *fake = context;
	fake->sleep_calls++;
	fake->slept_ms += milliseconds;
}

static const struct dfu_transition_ops FAKE_OPS = {
	.open_application = fake_open_application,
	.request_dfu = fake_request_dfu,
	.close_application = fake_close_application,
	.open_matching_dfu = fake_open_matching_dfu,
	.sleep_ms = fake_sleep_ms,
};

static struct fake_context make_fake(void)
{
	struct fake_context fake;
	memset(&fake, 0, sizeof(fake));
	fake.identity.bus_number = 3;
	fake.identity.port_count = 2;
	fake.identity.port_numbers[0] = 5;
	fake.identity.port_numbers[1] = 2;
	fake.app_handle = (void *)(uintptr_t)0x1;
	fake.dfu_handle = (void *)(uintptr_t)0x2;
	return fake;
}

static struct dfu_transition_config make_config(unsigned attempts,
                                                unsigned polls)
{
	struct dfu_transition_config config = {
		.max_attempts = attempts,
		.polls_per_attempt = polls,
		.poll_interval_ms = 25,
	};
	return config;
}

MU_TEST(test_default_entry_budget_is_two_five_second_attempts)
{
	mu_assert_int_eq(2, DFU_ENTRY_DEFAULT_MAX_ATTEMPTS);
	mu_assert_int_eq(25, DFU_ENTRY_DEFAULT_POLLS_PER_ATTEMPT);
	mu_assert_int_eq(200, DFU_ENTRY_DEFAULT_POLL_INTERVAL_MS);
	mu_assert_int_eq(5000,
	                 DFU_ENTRY_DEFAULT_POLLS_PER_ATTEMPT *
	                 DFU_ENTRY_DEFAULT_POLL_INTERVAL_MS);
}

MU_TEST(test_explicit_selector_never_adopts_unidentified_dfu)
{
	mu_assert(dfu_selector_allows_unidentified_dfu(NULL),
	          "default operation should adopt a sole DFU device");
	mu_assert(dfu_selector_allows_unidentified_dfu(""),
	          "empty selector should behave as default");
	mu_assert(!dfu_selector_allows_unidentified_dfu("0"),
	          "index selector must not adopt unidentified DFU");
	mu_assert(!dfu_selector_allows_unidentified_dfu(
	          "1ff0ff1967102caf8334fa5ac52000f5"),
	          "serial selector must not adopt unidentified DFU");
}

MU_TEST(test_usb_identity_requires_same_bus_and_port_path)
{
	struct dfu_usb_identity a = {3, 2, {5, 2}};
	struct dfu_usb_identity b = {3, 2, {5, 2}};
	mu_assert(dfu_usb_identity_equal(&a, &b), "equal paths did not match");
	b.bus_number = 4;
	mu_assert(!dfu_usb_identity_equal(&a, &b), "different buses matched");
	b = a;
	b.port_numbers[1] = 3;
	mu_assert(!dfu_usb_identity_equal(&a, &b), "different ports matched");
	b = a;
	b.port_count = 1;
	mu_assert(!dfu_usb_identity_equal(&a, &b), "different depths matched");
}

MU_TEST(test_success_waits_for_matching_dfu_and_reports_state)
{
	struct fake_context fake = make_fake();
	struct dfu_transition_config config = make_config(3, 5);
	struct dfu_transition_report report;
	void *dfu_handle = NULL;
	fake.discovery_results[0] = 0;
	fake.discovery_results[1] = 0;
	fake.discovery_results[2] = 1;
	fake.discovery_count = 3;

	mu_assert_int_eq(DFU_TRANSITION_OK,
	                 dfu_transition_run(&FAKE_OPS, &fake, "serial-1",
	                                    &config, &dfu_handle, &report));
	mu_assert(dfu_handle == fake.dfu_handle, "wrong DFU handle returned");
	mu_assert_int_eq(DFU_STATE_DFU_FOUND, report.state);
	mu_assert_int_eq(1, report.attempts);
	mu_assert_int_eq(3, report.polls);
	mu_assert_int_eq(1, fake.open_calls);
	mu_assert_int_eq(1, fake.request_calls);
	mu_assert_int_eq(1, fake.close_calls);
	mu_assert_int_eq(2, fake.sleep_calls);
	mu_assert_int_eq(50, fake.slept_ms);
	mu_assert(strcmp(fake.selector, "serial-1") == 0,
	          "selector was not forwarded");
}

MU_TEST(test_request_error_is_propagated_and_app_is_closed)
{
	struct fake_context fake = make_fake();
	struct dfu_transition_config config = make_config(3, 2);
	struct dfu_transition_report report;
	void *dfu_handle = NULL;
	fake.request_result = -9;

	mu_assert_int_eq(DFU_TRANSITION_REQUEST_FAILED,
	                 dfu_transition_run(&FAKE_OPS, &fake, "0", &config,
	                                    &dfu_handle, &report));
	mu_assert_int_eq(DFU_STATE_FAILED, report.state);
	mu_assert_int_eq(-9, report.operation_error);
	mu_assert_int_eq(1, fake.close_calls);
	mu_assert_int_eq(0, fake.discovery_index);
	mu_assert(dfu_handle == NULL, "request failure returned a DFU handle");
}

MU_TEST(test_timeout_retries_complete_entry_sequence)
{
	struct fake_context fake = make_fake();
	struct dfu_transition_config config = make_config(2, 2);
	struct dfu_transition_report report;
	void *dfu_handle = NULL;
	int results[] = {0, 0, 0, 1};
	memcpy(fake.discovery_results, results, sizeof(results));
	fake.discovery_count = ARRAY_LEN(results);

	mu_assert_int_eq(DFU_TRANSITION_OK,
	                 dfu_transition_run(&FAKE_OPS, &fake, "serial-2",
	                                    &config, &dfu_handle, &report));
	mu_assert_int_eq(DFU_STATE_DFU_FOUND, report.state);
	mu_assert_int_eq(2, report.attempts);
	mu_assert_int_eq(4, report.polls);
	mu_assert_int_eq(2, fake.open_calls);
	mu_assert_int_eq(2, fake.request_calls);
	mu_assert_int_eq(2, fake.close_calls);
}

MU_TEST(test_retry_rejects_a_different_physical_device)
{
	struct fake_context fake = make_fake();
	struct dfu_transition_config config = make_config(2, 1);
	struct dfu_transition_report report;
	void *dfu_handle = NULL;
	fake.change_identity_after_first_open = 1;

	mu_assert_int_eq(DFU_TRANSITION_IDENTITY_CHANGED,
	                 dfu_transition_run(&FAKE_OPS, &fake, "0", &config,
	                                    &dfu_handle, &report));
	mu_assert_int_eq(DFU_STATE_FAILED, report.state);
	mu_assert_int_eq(2, report.attempts);
	mu_assert_int_eq(2, fake.open_calls);
	mu_assert_int_eq(1, fake.request_calls);
	mu_assert_int_eq(2, fake.close_calls);
	mu_assert(dfu_handle == NULL, "identity change returned a DFU handle");
}

MU_TEST(test_final_timeout_reports_attempt_and_poll_counts)
{
	struct fake_context fake = make_fake();
	struct dfu_transition_config config = make_config(2, 3);
	struct dfu_transition_report report;
	void *dfu_handle = NULL;

	mu_assert_int_eq(DFU_TRANSITION_DFU_TIMEOUT,
	                 dfu_transition_run(&FAKE_OPS, &fake, NULL, &config,
	                                    &dfu_handle, &report));
	mu_assert_int_eq(DFU_STATE_FAILED, report.state);
	mu_assert_int_eq(2, report.attempts);
	mu_assert_int_eq(6, report.polls);
	mu_assert_int_eq(2, fake.open_calls);
	mu_assert_int_eq(2, fake.close_calls);
	mu_assert(dfu_handle == NULL, "timeout returned a DFU handle");
}

MU_TEST(test_discovery_error_is_propagated_without_retry)
{
	struct fake_context fake = make_fake();
	struct dfu_transition_config config = make_config(3, 3);
	struct dfu_transition_report report;
	void *dfu_handle = NULL;
	fake.discovery_results[0] = -7;
	fake.discovery_count = 1;

	mu_assert_int_eq(DFU_TRANSITION_DISCOVERY_FAILED,
	                 dfu_transition_run(&FAKE_OPS, &fake, "1", &config,
	                                    &dfu_handle, &report));
	mu_assert_int_eq(-7, report.operation_error);
	mu_assert_int_eq(1, report.attempts);
	mu_assert_int_eq(1, report.polls);
	mu_assert_int_eq(1, fake.close_calls);
}

MU_TEST(test_application_open_error_is_reported)
{
	struct fake_context fake = make_fake();
	struct dfu_transition_config config = make_config(1, 3);
	struct dfu_transition_report report;
	void *dfu_handle = NULL;
	fake.open_result = -5;

	mu_assert_int_eq(DFU_TRANSITION_APP_OPEN_FAILED,
	                 dfu_transition_run(&FAKE_OPS, &fake, "missing", &config,
	                                    &dfu_handle, &report));
	mu_assert_int_eq(-5, report.operation_error);
	mu_assert_int_eq(1, report.attempts);
	mu_assert_int_eq(0, fake.request_calls);
	mu_assert_int_eq(0, fake.close_calls);
}

int main(void)
{
	printf("DFU transition tests\n");
	MU_RUN(test_default_entry_budget_is_two_five_second_attempts);
	MU_RUN(test_explicit_selector_never_adopts_unidentified_dfu);
	MU_RUN(test_usb_identity_requires_same_bus_and_port_path);
	MU_RUN(test_success_waits_for_matching_dfu_and_reports_state);
	MU_RUN(test_request_error_is_propagated_and_app_is_closed);
	MU_RUN(test_timeout_retries_complete_entry_sequence);
	MU_RUN(test_retry_rejects_a_different_physical_device);
	MU_RUN(test_final_timeout_reports_attempt_and_poll_counts);
	MU_RUN(test_discovery_error_is_propagated_without_retry);
	MU_RUN(test_application_open_error_is_reported);
	return mu_report();
}
