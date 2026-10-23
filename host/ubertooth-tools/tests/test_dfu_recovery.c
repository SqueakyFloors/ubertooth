#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "dfu_recovery.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

struct fake_recovery {
	int download_results[4];
	unsigned download_count;
	unsigned download_index;
	int reopen_results[8];
	unsigned reopen_count;
	unsigned reopen_index;
	int claim_result;
	unsigned close_calls;
	int close_claimed[4];
	unsigned claim_calls;
	unsigned sleep_calls;
	struct dfu_usb_identity identity;
	void *initial_handle;
	void *reopened_handle;

	int app_open_results[8];
	unsigned app_open_count;
	unsigned app_open_index;
	struct dfu_usb_identity app_identity;
	const char *app_revision;
	int revision_result;
	unsigned app_close_calls;
	void *app_handle;
};

static struct fake_recovery make_fake(void)
{
	struct fake_recovery fake;
	memset(&fake, 0, sizeof(fake));
	fake.identity.bus_number = 3;
	fake.identity.port_count = 1;
	fake.identity.port_numbers[0] = 5;
	fake.app_identity = fake.identity;
	fake.initial_handle = (void *)(uintptr_t)0x1;
	fake.reopened_handle = (void *)(uintptr_t)0x2;
	fake.app_handle = (void *)(uintptr_t)0x3;
	fake.app_revision = "git-expected";
	return fake;
}

static int fake_download(void *context, void *handle)
{
	struct fake_recovery *fake = context;
	int result = 0;
	mu_assert(handle == fake->initial_handle || handle == fake->reopened_handle,
	          "download received unknown handle");
	if (fake->download_index < fake->download_count)
		result = fake->download_results[fake->download_index++];
	return result;
}

static void fake_close_dfu(void *context, void *handle, int claimed)
{
	struct fake_recovery *fake = context;
	mu_assert(handle != NULL, "close received null handle");
	fake->close_claimed[fake->close_calls] = claimed;
	fake->close_calls++;
}

static int fake_reopen_dfu(void *context,
                           const struct dfu_usb_identity *identity,
                           void **handle)
{
	struct fake_recovery *fake = context;
	int result = 0;
	mu_assert(dfu_usb_identity_equal(identity, &fake->identity),
	          "reopen used wrong physical path");
	if (fake->reopen_index < fake->reopen_count)
		result = fake->reopen_results[fake->reopen_index++];
	if (result == 1)
		*handle = fake->reopened_handle;
	return result;
}

static int fake_claim_dfu(void *context, void *handle)
{
	struct fake_recovery *fake = context;
	fake->claim_calls++;
	mu_assert(handle == fake->reopened_handle,
	          "claim received wrong reopened handle");
	return fake->claim_result;
}

static void fake_sleep(void *context, unsigned milliseconds)
{
	struct fake_recovery *fake = context;
	mu_assert(milliseconds > 0, "sleep interval must be positive");
	fake->sleep_calls++;
}

static const struct dfu_transfer_ops TRANSFER_OPS = {
	.download = fake_download,
	.close_dfu = fake_close_dfu,
	.reopen_matching_dfu = fake_reopen_dfu,
	.claim_dfu = fake_claim_dfu,
	.sleep_ms = fake_sleep,
};

static int fake_open_app(void *context, const char *selector, void **handle,
                         struct dfu_usb_identity *identity)
{
	struct fake_recovery *fake = context;
	int result = 0;
	mu_assert(strcmp(selector, "serial") == 0,
	          "verification selector changed");
	if (fake->app_open_index < fake->app_open_count)
		result = fake->app_open_results[fake->app_open_index++];
	if (result == 0) {
		*handle = fake->app_handle;
		*identity = fake->app_identity;
	}
	return result;
}

static int fake_get_revision(void *context, void *handle,
                             char *revision, size_t revision_size)
{
	struct fake_recovery *fake = context;
	mu_assert(handle == fake->app_handle, "revision used wrong app handle");
	if (fake->revision_result != 0)
		return fake->revision_result;
	snprintf(revision, revision_size, "%s", fake->app_revision);
	return 0;
}

static void fake_close_app(void *context, void *handle)
{
	struct fake_recovery *fake = context;
	mu_assert(handle == fake->app_handle, "app close used wrong handle");
	fake->app_close_calls++;
}

static const struct dfu_verify_ops VERIFY_OPS = {
	.open_application = fake_open_app,
	.get_revision = fake_get_revision,
	.close_application = fake_close_app,
	.sleep_ms = fake_sleep,
};

MU_TEST(test_transfer_reopens_same_path_and_retries_from_start)
{
	struct fake_recovery fake = make_fake();
	struct dfu_transfer_config config = {2, 3, 10};
	struct dfu_transfer_report report;
	void *handle = fake.initial_handle;
	int downloads[] = {-99, 0};
	int reopens[] = {0, 1};
	memcpy(fake.download_results, downloads, sizeof(downloads));
	fake.download_count = ARRAY_LEN(downloads);
	memcpy(fake.reopen_results, reopens, sizeof(reopens));
	fake.reopen_count = ARRAY_LEN(reopens);

	mu_assert_int_eq(DFU_TRANSFER_OK,
	                 dfu_transfer_run(&TRANSFER_OPS, &fake, &fake.identity,
	                                  &config, &handle, &report));
	mu_assert(handle == fake.reopened_handle, "reopened handle not returned");
	mu_assert_int_eq(2, report.attempts);
	mu_assert_int_eq(2, report.reopen_polls);
	mu_assert_int_eq(1, fake.close_calls);
	mu_assert_int_eq(1, fake.close_claimed[0]);
	mu_assert_int_eq(1, fake.claim_calls);
	mu_assert_int_eq(1, fake.sleep_calls);
}

MU_TEST(test_transfer_reopen_timeout_is_distinct)
{
	struct fake_recovery fake = make_fake();
	struct dfu_transfer_config config = {2, 2, 10};
	struct dfu_transfer_report report;
	void *handle = fake.initial_handle;
	fake.download_results[0] = -99;
	fake.download_count = 1;

	mu_assert_int_eq(DFU_TRANSFER_REOPEN_TIMEOUT,
	                 dfu_transfer_run(&TRANSFER_OPS, &fake, &fake.identity,
	                                  &config, &handle, &report));
	mu_assert(handle == NULL, "timeout retained stale DFU handle");
	mu_assert_int_eq(-99, report.transfer_error);
	mu_assert_int_eq(2, report.reopen_polls);
	mu_assert_int_eq(1, fake.close_calls);
}

MU_TEST(test_transfer_claim_failure_closes_unclaimed_handle)
{
	struct fake_recovery fake = make_fake();
	struct dfu_transfer_config config = {2, 1, 10};
	struct dfu_transfer_report report;
	void *handle = fake.initial_handle;
	fake.download_results[0] = -99;
	fake.download_count = 1;
	fake.reopen_results[0] = 1;
	fake.reopen_count = 1;
	fake.claim_result = -6;

	mu_assert_int_eq(DFU_TRANSFER_CLAIM_FAILED,
	                 dfu_transfer_run(&TRANSFER_OPS, &fake, &fake.identity,
	                                  &config, &handle, &report));
	mu_assert(handle == NULL, "claim failure retained handle");
	mu_assert_int_eq(-6, report.operation_error);
	mu_assert_int_eq(2, fake.close_calls);
	mu_assert_int_eq(0, fake.close_claimed[1]);
}

MU_TEST(test_transfer_retry_exhaustion_closes_final_handle)
{
	struct fake_recovery fake = make_fake();
	struct dfu_transfer_config config = {2, 1, 10};
	struct dfu_transfer_report report;
	void *handle = fake.initial_handle;
	int downloads[] = {-99, -7};
	memcpy(fake.download_results, downloads, sizeof(downloads));
	fake.download_count = ARRAY_LEN(downloads);
	fake.reopen_results[0] = 1;
	fake.reopen_count = 1;

	mu_assert_int_eq(DFU_TRANSFER_RETRY_EXHAUSTED,
	                 dfu_transfer_run(&TRANSFER_OPS, &fake, &fake.identity,
	                                  &config, &handle, &report));
	mu_assert(handle == NULL, "retry exhaustion retained handle");
	mu_assert_int_eq(2, report.attempts);
	mu_assert_int_eq(-7, report.transfer_error);
	mu_assert_int_eq(2, fake.close_calls);
	mu_assert_int_eq(1, fake.close_claimed[1]);
}

MU_TEST(test_verify_waits_for_same_app_and_exact_revision)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {3, 10};
	struct dfu_verify_report report;
	int opens[] = {-4, 0};
	memcpy(fake.app_open_results, opens, sizeof(opens));
	fake.app_open_count = ARRAY_LEN(opens);

	mu_assert_int_eq(DFU_VERIFY_OK,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        &config, &report));
	mu_assert_int_eq(2, report.polls);
	mu_assert_int_eq(1, fake.app_close_calls);
	mu_assert_int_eq(1, fake.sleep_calls);
}

MU_TEST(test_verify_rejects_wrong_physical_path)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	fake.app_identity.port_numbers[0] = 6;

	mu_assert_int_eq(DFU_VERIFY_IDENTITY_MISMATCH,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        &config, &report));
	mu_assert_int_eq(1, fake.app_close_calls);
}

MU_TEST(test_verify_rejects_wrong_revision)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	fake.app_revision = "git-old";

	mu_assert_int_eq(DFU_VERIFY_REVISION_MISMATCH,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        &config, &report));
	mu_assert(strcmp(report.actual_revision, "git-old") == 0,
	          "actual revision not reported");
	mu_assert_int_eq(1, fake.app_close_calls);
}

MU_TEST(test_extract_revision_from_existing_compile_info_marker)
{
	FILE *image = tmpfile();
	char revision[64];
	const char bytes[] = "\x01\x02ubertooth git-deadbee* (builder@host) date\0tail";
	mu_assert(image != NULL, "tmpfile failed");
	mu_assert_int_eq((int)sizeof(bytes),
	                 (int)fwrite(bytes, 1, sizeof(bytes), image));
	mu_assert_int_eq(0,
	                 dfu_extract_firmware_revision(image, revision,
	                                               sizeof(revision)));
	mu_assert(strcmp(revision, "git-deadbee*") == 0,
	          "wrong revision extracted");
	fclose(image);
}

int main(void)
{
	printf("DFU recovery tests\n");
	MU_RUN(test_transfer_reopens_same_path_and_retries_from_start);
	MU_RUN(test_transfer_reopen_timeout_is_distinct);
	MU_RUN(test_transfer_claim_failure_closes_unclaimed_handle);
	MU_RUN(test_transfer_retry_exhaustion_closes_final_handle);
	MU_RUN(test_verify_waits_for_same_app_and_exact_revision);
	MU_RUN(test_verify_rejects_wrong_physical_path);
	MU_RUN(test_verify_rejects_wrong_revision);
	MU_RUN(test_extract_revision_from_existing_compile_info_marker);
	return mu_report();
}
