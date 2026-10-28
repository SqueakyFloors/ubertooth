#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
#include "dfu.h"
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
	const char *app_image;
	int image_result;
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
	fake.app_image = "bluetooth_rxtx";
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
	.transfer = fake_download,
	.close_dfu = fake_close_dfu,
	.reopen_matching_dfu = fake_reopen_dfu,
	.claim_dfu = fake_claim_dfu,
	.sleep_ms = fake_sleep,
};

static int fake_open_app(void *context, const char *selector, void **handle,
                         struct dfu_usb_identity *identity)
{
	struct fake_recovery *fake = context;
	int result = 1;
	mu_assert(strcmp(selector, "serial") == 0,
	          "verification selector changed");
	if (fake->app_open_index < fake->app_open_count)
		result = fake->app_open_results[fake->app_open_index++];
	if (result == 1) {
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

static int fake_get_image(void *context, void *handle,
                          char *image, size_t image_size)
{
	struct fake_recovery *fake = context;
	mu_assert(handle == fake->app_handle, "image used wrong app handle");
	if (fake->image_result != 0)
		return fake->image_result;
	snprintf(image, image_size, "%s", fake->app_image);
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
	.get_image = fake_get_image,
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
	/* Nothing on the bus, then the application. */
	int opens[] = {0, 1};
	memcpy(fake.app_open_results, opens, sizeof(opens));
	fake.app_open_count = ARRAY_LEN(opens);

	mu_assert_int_eq(DFU_VERIFY_OK,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
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
	                                        "bluetooth_rxtx",
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
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	mu_assert(strcmp(report.actual_revision, "git-old") == 0,
	          "actual revision not reported");
	mu_assert_int_eq(1, fake.app_close_calls);
}

/* The gap the revision alone leaves: both images of one tree carry the same
 * revision, so a write of one over the other that did not take passes a
 * revision check. The image name is what separates them. */
MU_TEST(test_verify_rejects_the_other_image_of_the_same_revision)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	fake.app_image = "btbr";

	mu_assert_int_eq(DFU_VERIFY_IMAGE_MISMATCH,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	mu_assert(strcmp(report.actual_image, "btbr") == 0,
	          "actual image not reported");
	mu_assert(strcmp(report.actual_revision, "git-expected") == 0,
	          "the matching revision was not reported as well");
	mu_assert_int_eq(1, fake.app_close_calls);
}

/* A board that answers the revision but not the info block leaves the image
 * unknown, which is not the same as running the wrong one. */
MU_TEST(test_verify_separates_an_unreadable_image_from_a_wrong_one)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	fake.image_result = -9;

	mu_assert_int_eq(DFU_VERIFY_IMAGE_FAILED,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	mu_assert_int_eq(-9, report.operation_error);
	mu_assert_int_eq(1, fake.app_close_calls);
}

/* A revision that does not match is settled before the image is asked for:
 * that case is the one that says the write did not take at all, and it reads
 * the same whichever image answered. */
MU_TEST(test_verify_does_not_ask_for_the_image_of_a_wrong_revision)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	fake.app_revision = "git-old";
	fake.image_result = -9;

	mu_assert_int_eq(DFU_VERIFY_REVISION_MISMATCH,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	mu_assert_int_eq(0, report.operation_error);
	mu_assert_int_eq(1, fake.app_close_calls);
}

/* A verification with no image to expect, or no way to read one, cannot run
 * at all rather than passing on the revision alone. */
MU_TEST(test_verify_needs_an_expected_image_and_a_way_to_read_it)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	struct dfu_verify_ops without_image = VERIFY_OPS;

	mu_assert_int_eq(DFU_VERIFY_INVALID_ARGUMENT,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        NULL, &config, &report));
	without_image.get_image = NULL;
	mu_assert_int_eq(DFU_VERIFY_INVALID_ARGUMENT,
	                 dfu_verify_application(&without_image, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
}

/* ---- the line each outcome is described by ------------------------------ */

/* The one that matters most: the write completed and the board stayed off the
 * bus, so its flash already holds the new firmware. The line has to say so,
 * because a line that only says the verification failed sends the reader back
 * to flash a board that does not need it. */
MU_TEST(test_a_board_that_never_came_back_is_told_apart_from_a_failed_write)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {2, 10};
	struct dfu_verify_report report;
	char message[256];
	/* Nothing on the bus for either poll. */
	int opens[] = {0, 0};
	memcpy(fake.app_open_results, opens, sizeof(opens));
	fake.app_open_count = ARRAY_LEN(opens);

	mu_assert_int_eq(DFU_VERIFY_APP_TIMEOUT,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	mu_assert_int_eq(0, report.operation_error);
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strstr(message, "the firmware was written") != NULL,
	          "the line does not say the image reached the board");
	mu_assert(strstr(message, "did not come back on USB") != NULL,
	          "the line does not say the board stayed off the bus");
	mu_assert(strstr(message, "ubertooth-util -v") != NULL,
	          "the line does not say how to read the board's revision");
	mu_assert(strstr(message, "not written") == NULL,
	          "the line claims the write did not take");
	mu_assert(strchr(message, '\n') == NULL, "the line is more than one line");
}

/* A board that is on the bus and will not open is not a board that stayed
 * away: it is there, so power-cycling it is not the answer and claiming it
 * came back is not a guess. Something else holding its interface, or no
 * permission to it, reads the same way from here. */
MU_TEST(test_a_board_that_will_not_open_is_not_a_board_that_stayed_away)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {3, 10};
	struct dfu_verify_report report;
	char message[256];
	/* Absent, then there and refusing: the last poll is what is described. */
	int opens[] = {0, -6, -6};
	memcpy(fake.app_open_results, opens, sizeof(opens));
	fake.app_open_count = ARRAY_LEN(opens);

	mu_assert_int_eq(DFU_VERIFY_APP_TIMEOUT,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	mu_assert_int_eq(-6, report.operation_error);
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strstr(message, "the firmware was written") != NULL,
	          "the line does not say the image reached the board");
	mu_assert(strstr(message, "came back and would not open") != NULL,
	          "the line does not say the board refused to open");
	mu_assert(strstr(message, "once nothing else is using it") != NULL,
	          "the line sends the reader to a command that would be refused "
	          "the same way");
	mu_assert(strstr(message, "did not come back on USB") == NULL,
	          "the line calls a board that is there missing");
	mu_assert(strstr(message, "power-cycle") == NULL,
	          "the line prescribes a power cycle for a board that is there");
	mu_assert(strstr(message, "ubertooth-util -v") != NULL,
	          "the line does not say how to read the board's revision");
	mu_assert(strchr(message, '\n') == NULL, "the line is more than one line");
}

/* A board running something else did not take the write, and this is the one
 * outcome that asks for another flash. */
MU_TEST(test_a_board_running_another_revision_is_described_as_a_failed_write)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	char message[256];
	fake.app_revision = "git-old";

	mu_assert_int_eq(DFU_VERIFY_REVISION_MISMATCH,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strstr(message, "running git-old, not git-expected") != NULL,
	          "the line does not name both revisions");
	mu_assert(strstr(message, "the firmware was not written") != NULL,
	          "the line does not say the write did not take");
	mu_assert(strstr(message, "flash it again") != NULL,
	          "the line does not say to flash the board again");
}

/* A board that came back but would not answer is not a board running the
 * wrong firmware: nothing is known about what it runs. */
MU_TEST(test_a_board_that_will_not_report_its_revision_is_not_a_mismatch)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	char message[256];
	fake.revision_result = -7;

	mu_assert_int_eq(DFU_VERIFY_REVISION_FAILED,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strstr(message, "would not report its firmware revision")
	          != NULL, "the line does not say what the board withheld");
	mu_assert(strstr(message, "the flash was not checked") != NULL,
	          "the line does not say the flash is unverified");
	mu_assert(strstr(message, "not written") == NULL,
	          "the line claims the write did not take");
}

/* A board running the other image of the same tree did not take the write
 * either, and the line has to name both images: the revisions match, so
 * naming those would say nothing. */
MU_TEST(test_the_other_image_is_described_as_a_failed_write)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	char message[256];
	fake.app_image = "btbr";

	mu_assert_int_eq(DFU_VERIFY_IMAGE_MISMATCH,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strstr(message, "running the btbr firmware, not the "
	                 "bluetooth_rxtx image") != NULL,
	          "the line does not name both images");
	mu_assert(strstr(message, "the firmware was not written") != NULL,
	          "the line does not say the write did not take");
	mu_assert(strstr(message, "flash it again") != NULL,
	          "the line does not say to flash the board again");
	mu_assert(strchr(message, '\n') == NULL, "the line is more than one line");
}

/* The write took, and which image answered is unknown: that is unchecked, not
 * a failed write. */
MU_TEST(test_an_unreadable_image_is_described_as_unchecked)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	char message[256];
	fake.image_result = -9;

	mu_assert_int_eq(DFU_VERIFY_IMAGE_FAILED,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strstr(message, "running git-expected") != NULL,
	          "the line does not say the revision matched");
	mu_assert(strstr(message, "would not say which firmware image it is")
	          != NULL, "the line does not say what the board withheld");
	mu_assert(strstr(message, "the flash was not checked") != NULL,
	          "the line does not say the flash is unverified");
	/* No command can read the image back off a board that will not answer,
	 * so the line must not send the reader to one. */
	mu_assert(strstr(message, "ubertooth-util") == NULL,
	          "the line sends the reader to a command that cannot answer");
	mu_assert(strstr(message, "not written") == NULL,
	          "the line claims the write did not take");
	mu_assert(strchr(message, '\n') == NULL, "the line is more than one line");
}

/* A device that answered on another path says nothing about the board that
 * was flashed. */
MU_TEST(test_a_device_on_another_path_is_described_as_unchecked)
{
	struct fake_recovery fake = make_fake();
	struct dfu_verify_config config = {1, 10};
	struct dfu_verify_report report;
	char message[256];
	fake.app_identity.port_numbers[0] = 6;

	mu_assert_int_eq(DFU_VERIFY_IDENTITY_MISMATCH,
	                 dfu_verify_application(&VERIFY_OPS, &fake, "serial",
	                                        &fake.identity, "git-expected",
	                                        "bluetooth_rxtx",
	                                        &config, &report));
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strstr(message, "not on the flashed board's USB path") != NULL,
	          "the line does not say which device answered");
	mu_assert(strstr(message, "the flash was not checked") != NULL,
	          "the line does not say the flash is unverified");
}

/* Nothing a caller can pass leaves the message unwritten or overruns it. */
MU_TEST(test_describing_a_verification_survives_what_a_caller_passes)
{
	struct dfu_verify_report report;
	char message[256];
	char small[8];

	memset(&report, 0, sizeof(report));
	snprintf(report.actual_revision, sizeof(report.actual_revision),
	         "git-expected");
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strstr(message, "came back running git-expected") != NULL,
	          "a passed verification is not described");

	report.result = DFU_VERIFY_REVISION_MISMATCH;
	dfu_verify_describe_failure(&report, NULL, NULL, message,
	                            sizeof(message));
	mu_assert(strstr(message, "running git-expected, not ") != NULL,
	          "a missing expected revision was not left blank");

	report.result = DFU_VERIFY_IMAGE_MISMATCH;
	dfu_verify_describe_failure(&report, "git-expected", NULL, message,
	                            sizeof(message));
	mu_assert(strstr(message, "not the  image that was written") != NULL,
	          "a missing expected image name was not left blank");

	report.result = DFU_VERIFY_INVALID_ARGUMENT;
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strcmp(message,
	                 "the flash could not be checked after the reset") == 0,
	          "a verification that did not run is not described");

	dfu_verify_describe_failure(NULL, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strcmp(message,
	                 "the flash could not be checked after the reset") == 0,
	          "a missing report is not described");

	/* A value that is not one of the enumerators still gets a line, and the
	 * line names it. */
	report.result = (enum dfu_verify_result)42;
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            message, sizeof(message));
	mu_assert(strcmp(message, "the flash could not be checked after the "
	                 "reset (result 42)") == 0,
	          "a result this host has no case for is not described");

	/* A buffer too small truncates rather than overruns, and a zero-length
	 * one is left alone. */
	report.result = DFU_VERIFY_APP_TIMEOUT;
	memset(small, 'x', sizeof(small));
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            small, sizeof(small));
	mu_assert_int_eq((int)sizeof(small) - 1, (int)strlen(small));
	dfu_verify_describe_failure(&report, "git-expected", "bluetooth_rxtx",
	                            small, 0);
	mu_assert_int_eq((int)sizeof(small) - 1, (int)strlen(small));
}

/*
 * Reaching DFU idle: a faked DFU transport, no device. The states are
 * scripted in turn and the last one is reported for as long as it is asked
 * for, so "it never answers" and "it never settles" are both expressible.
 */
#define IDLE_STATES_MAX 8

struct fake_idle {
	int states[IDLE_STATES_MAX];
	unsigned state_count;
	unsigned state_index;
	unsigned get_state_calls;
	int abort_result;
	int clear_result;
	unsigned abort_calls;
	unsigned clear_calls;
	unsigned sleep_calls;
	void *handle;
};

static struct fake_idle make_idle_fake(void)
{
	struct fake_idle fake;
	memset(&fake, 0, sizeof(fake));
	fake.handle = (void *)(uintptr_t)0x11;
	return fake;
}

static int fake_get_state(void *context, void *handle)
{
	struct fake_idle *fake = context;
	unsigned index;

	mu_assert(handle == fake->handle, "get_state received unknown handle");
	fake->get_state_calls++;
	mu_assert(fake->state_count > 0, "no state scripted");
	index = fake->state_index;
	if (fake->state_index + 1 < fake->state_count)
		fake->state_index++;
	return fake->states[index];
}

static int fake_abort(void *context, void *handle)
{
	struct fake_idle *fake = context;
	mu_assert(handle == fake->handle, "abort received unknown handle");
	fake->abort_calls++;
	return fake->abort_result;
}

static int fake_clear_status(void *context, void *handle)
{
	struct fake_idle *fake = context;
	mu_assert(handle == fake->handle, "clear received unknown handle");
	fake->clear_calls++;
	return fake->clear_result;
}

static void fake_idle_sleep(void *context, unsigned milliseconds)
{
	struct fake_idle *fake = context;
	mu_assert(milliseconds > 0, "sleep interval must be positive");
	fake->sleep_calls++;
}

static const struct dfu_idle_ops IDLE_OPS = {
	.get_state = fake_get_state,
	.abort = fake_abort,
	.clear_status = fake_clear_status,
	.sleep_ms = fake_idle_sleep,
};

/* Set one state, reported for every read. */
static void idle_always(struct fake_idle *fake, int state)
{
	fake->states[0] = state;
	fake->state_count = 1;
}

/* A bootloader that will not report its state used to be asked forever, one
 * error line per attempt. The first failed read now ends the wait. */
MU_TEST(test_idle_stops_on_the_first_failed_state_read)
{
	struct fake_idle fake = make_idle_fake();
	struct dfu_idle_config config = {40, 250};
	struct dfu_idle_report report;

	idle_always(&fake, -7); /* LIBUSB_ERROR_TIMEOUT */
	mu_assert_int_eq(DFU_IDLE_QUERY_FAILED,
	                 dfu_reach_idle(&IDLE_OPS, &fake, fake.handle, &config,
	                                &report));
	mu_assert_int_eq(-7, report.operation_error);
	mu_assert_int_eq(1, report.polls);
	mu_assert_int_eq(1, fake.get_state_calls);
	mu_assert_int_eq(0, fake.sleep_calls);
	mu_assert_int_eq(-1, report.last_state);
}

/* A state that only time resolves is polled a fixed number of times. */
MU_TEST(test_idle_is_bounded_when_the_state_never_settles)
{
	struct fake_idle fake = make_idle_fake();
	struct dfu_idle_config config = {5, 10};
	struct dfu_idle_report report;

	idle_always(&fake, STATE_DFU_DNBUSY);
	mu_assert_int_eq(DFU_IDLE_NOT_REACHED,
	                 dfu_reach_idle(&IDLE_OPS, &fake, fake.handle, &config,
	                                &report));
	mu_assert_int_eq(5, report.polls);
	mu_assert_int_eq(5, fake.get_state_calls);
	mu_assert_int_eq(5, fake.sleep_calls);
	mu_assert_int_eq(STATE_DFU_DNBUSY, report.last_state);
}

MU_TEST(test_idle_aborts_a_half_finished_transfer_then_succeeds)
{
	struct fake_idle fake = make_idle_fake();
	struct dfu_idle_config config = {40, 250};
	struct dfu_idle_report report;
	int states[] = {STATE_DFU_DNLOAD_IDLE, STATE_DFU_IDLE};

	memcpy(fake.states, states, sizeof(states));
	fake.state_count = ARRAY_LEN(states);
	mu_assert_int_eq(DFU_IDLE_OK,
	                 dfu_reach_idle(&IDLE_OPS, &fake, fake.handle, &config,
	                                &report));
	mu_assert_int_eq(1, fake.abort_calls);
	mu_assert_int_eq(0, fake.sleep_calls);
	mu_assert_int_eq(2, report.polls);
	mu_assert_int_eq(STATE_DFU_IDLE, report.last_state);
}

MU_TEST(test_idle_clears_an_error_state_then_succeeds)
{
	struct fake_idle fake = make_idle_fake();
	struct dfu_idle_config config = {40, 250};
	struct dfu_idle_report report;
	int states[] = {STATE_DFU_ERROR, STATE_DFU_IDLE};

	memcpy(fake.states, states, sizeof(states));
	fake.state_count = ARRAY_LEN(states);
	mu_assert_int_eq(DFU_IDLE_OK,
	                 dfu_reach_idle(&IDLE_OPS, &fake, fake.handle, &config,
	                                &report));
	mu_assert_int_eq(1, fake.clear_calls);
	mu_assert_int_eq(2, report.polls);
}

MU_TEST(test_idle_reports_a_failed_abort_or_clear)
{
	struct fake_idle fake = make_idle_fake();
	struct dfu_idle_config config = {40, 250};
	struct dfu_idle_report report;

	idle_always(&fake, STATE_DFU_UPLOAD_IDLE);
	fake.abort_result = -9;
	mu_assert_int_eq(DFU_IDLE_ABORT_FAILED,
	                 dfu_reach_idle(&IDLE_OPS, &fake, fake.handle, &config,
	                                &report));
	mu_assert_int_eq(-9, report.operation_error);
	mu_assert_int_eq(1, fake.abort_calls);

	fake = make_idle_fake();
	idle_always(&fake, STATE_DFU_ERROR);
	fake.clear_result = -7;
	mu_assert_int_eq(DFU_IDLE_CLEAR_FAILED,
	                 dfu_reach_idle(&IDLE_OPS, &fake, fake.handle, &config,
	                                &report));
	mu_assert_int_eq(-7, report.operation_error);
	mu_assert_int_eq(1, fake.clear_calls);
}

/* The application state and anything outside the DFU 1.1 set used to be
 * polled forever; there is nothing this host can do with either. */
MU_TEST(test_idle_refuses_a_state_it_cannot_drive)
{
	struct fake_idle fake = make_idle_fake();
	struct dfu_idle_config config = {40, 250};
	struct dfu_idle_report report;

	idle_always(&fake, STATE_APP_IDLE);
	mu_assert_int_eq(DFU_IDLE_UNEXPECTED_STATE,
	                 dfu_reach_idle(&IDLE_OPS, &fake, fake.handle, &config,
	                                &report));
	mu_assert_int_eq(1, report.polls);
	mu_assert_int_eq(STATE_APP_IDLE, report.last_state);

	fake = make_idle_fake();
	idle_always(&fake, 0x7f);
	mu_assert_int_eq(DFU_IDLE_UNEXPECTED_STATE,
	                 dfu_reach_idle(&IDLE_OPS, &fake, fake.handle, &config,
	                                &report));
	mu_assert_int_eq(1, report.polls);
	mu_assert_int_eq(0x7f, report.last_state);
	mu_assert_int_eq(0, fake.sleep_calls);
}

MU_TEST(test_idle_refuses_an_unbounded_configuration)
{
	struct fake_idle fake = make_idle_fake();
	struct dfu_idle_config config = {0, 250};
	struct dfu_idle_report report;

	idle_always(&fake, STATE_DFU_IDLE);
	mu_assert_int_eq(DFU_IDLE_INVALID_ARGUMENT,
	                 dfu_reach_idle(&IDLE_OPS, &fake, fake.handle, &config,
	                                &report));
	mu_assert_int_eq(0, fake.get_state_calls);
	mu_assert(strstr(dfu_idle_result_string(DFU_IDLE_QUERY_FAILED),
	                 "did not report its state") != NULL,
	          "the query failure has no message");
}

/*
 * Image checking: hand-built DFU files, no device.
 */

#define IMAGE_MAX 512
#define PAYLOAD_LENGTH 64

struct image_spec {
	uint16_t id_vendor;
	uint16_t id_product;
	uint16_t bcd_dfu;
	uint8_t suffix_length;
	int bad_signature;
	int corrupt_crc;
};

static struct image_spec good_spec(void)
{
	struct image_spec spec;
	memset(&spec, 0, sizeof(spec));
	spec.id_vendor = U1_DFU_VENDORID;
	spec.id_product = U1_DFU_PRODUCTID;
	spec.bcd_dfu = DFU_VERSION_1_0;
	spec.suffix_length = DFU_SUFFIX_LENGTH;
	return spec;
}

static void put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)(value & 0xff);
	bytes[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value & 0xff);
	bytes[1] = (uint8_t)((value >> 8) & 0xff);
	bytes[2] = (uint8_t)((value >> 16) & 0xff);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Append a DFU suffix to the payload_length bytes already in buffer and
 * return the length of the whole image. */
static size_t append_suffix(const struct image_spec *spec, uint8_t *buffer,
                            size_t payload_length, size_t buffer_size)
{
	size_t length = payload_length + DFU_SUFFIX_LENGTH;
	uint8_t *suffix = buffer + payload_length;
	uint32_t crc;

	mu_assert(length <= buffer_size, "image buffer too small");
	if (length > buffer_size)
		return 0;
	put_le16(suffix + DFU_SUFFIX_OFF_BCDDEVICE, 0);
	put_le16(suffix + DFU_SUFFIX_OFF_IDPRODUCT, spec->id_product);
	put_le16(suffix + DFU_SUFFIX_OFF_IDVENDOR, spec->id_vendor);
	put_le16(suffix + DFU_SUFFIX_OFF_BCDDFU, spec->bcd_dfu);
	suffix[DFU_SUFFIX_OFF_UCDFUSIG + 0] = spec->bad_signature ? 0x00 : 0x55;
	suffix[DFU_SUFFIX_OFF_UCDFUSIG + 1] = 0x46;
	suffix[DFU_SUFFIX_OFF_UCDFUSIG + 2] = 0x44;
	suffix[DFU_SUFFIX_OFF_BLENGTH] = spec->suffix_length;
	crc = dfu_image_crc32(buffer, length - 4);
	if (spec->corrupt_crc)
		crc ^= 0xffffffff;
	put_le32(suffix + DFU_SUFFIX_OFF_DWCRC, crc);
	return length;
}

static size_t build_image(const struct image_spec *spec, size_t payload_length,
                          uint8_t *buffer, size_t buffer_size)
{
	if (payload_length > buffer_size)
		payload_length = 0;
	memset(buffer, 0x5a, payload_length);
	return append_suffix(spec, buffer, payload_length, buffer_size);
}

/* Run the check over these bytes and confirm the file position survives it. */
static int check_image_bytes(const uint8_t *bytes, size_t length,
                             struct dfu_image_report *report,
                             char *message, size_t message_size)
{
	FILE *image = tmpfile();
	int result;

	mu_assert(image != NULL, "tmpfile failed");
	if (image == NULL)
		return DFU_IMAGE_INVALID_ARGUMENT;
	mu_assert(fwrite(bytes, 1, length, image) == length, "short image write");
	/* From a position that is neither the start nor the end, so a missing
	 * restore shows up. */
	mu_assert(fseek(image, 3, SEEK_SET) == 0, "seek failed");
	result = dfu_image_check(image, U1_DFU_VENDORID, U1_DFU_PRODUCTID, report);
	mu_assert(ftell(image) == 3,
	          "image check did not restore the file position");
	if (message != NULL)
		dfu_image_describe_failure(report, message, message_size);
	fclose(image);
	return result;
}

MU_TEST(test_image_accepts_a_signed_ubertooth_image)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	size_t length = build_image(&spec, PAYLOAD_LENGTH, image, sizeof(image));

	mu_assert_int_eq(DFU_IMAGE_OK,
	                 check_image_bytes(image, length, &report, NULL, 0));
	mu_assert_int_eq(PAYLOAD_LENGTH + DFU_SUFFIX_LENGTH, report.image_size);
	mu_assert_int_eq(U1_DFU_VENDORID, report.id_vendor);
	mu_assert_int_eq(U1_DFU_PRODUCTID, report.id_product);
}

/* The generated table gives the standard CRC-32 (the check value of
 * "123456789" is 0xcbf43926 after the final XOR, which the DFU suffix does not
 * apply), so images signed by this tool and by other DFU tools agree. */
MU_TEST(test_crc32_matches_the_standard_check_value)
{
	static const uint8_t check[] = "123456789";
	static const uint8_t zero[1] = { 0 };

	mu_assert_int_eq(0x340bc6d9u, dfu_image_crc32(check, 9));
	mu_assert_int_eq(0xffffffffu, dfu_image_crc32(check, 0));
	/* one byte: the table's entry 0xff, the first one a byte reaches */
	mu_assert_int_eq(0x2dfd1072u, dfu_image_crc32(zero, 1));
}

MU_TEST(test_image_rejects_a_wrong_crc)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	char message[160];
	size_t length;

	spec.corrupt_crc = 1;
	length = build_image(&spec, PAYLOAD_LENGTH, image, sizeof(image));
	mu_assert_int_eq(DFU_IMAGE_CRC,
	                 check_image_bytes(image, length, &report, message,
	                                   sizeof(message)));
	mu_assert(report.crc_calculated != report.crc_found,
	          "report did not record both CRCs");
	/* The harness and the reported output match on this wording. */
	mu_assert(strstr(message, "CRC mismatch") != NULL,
	          "CRC failure not named in the message");
}

MU_TEST(test_image_rejects_a_truncated_image)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	char message[160];
	size_t length = build_image(&spec, PAYLOAD_LENGTH, image, sizeof(image));

	/* Losing the tail leaves payload bytes where the suffix should be. */
	mu_assert_int_eq(DFU_IMAGE_SUFFIX_LENGTH,
	                 check_image_bytes(image, length - 20, &report, message,
	                                   sizeof(message)));
	mu_assert_int_eq(0x5a, report.suffix_length);
	mu_assert(strstr(message, "Unknown DFU suffix length") != NULL,
	          "suffix length failure not named in the message");
}

MU_TEST(test_image_rejects_a_file_shorter_than_a_suffix)
{
	uint8_t image[8];
	struct dfu_image_report report;
	char message[160];

	memset(image, 0x5a, sizeof(image));
	mu_assert_int_eq(DFU_IMAGE_TOO_SHORT,
	                 check_image_bytes(image, sizeof(image), &report, message,
	                                   sizeof(message)));
	mu_assert_int_eq(8, report.image_size);
	mu_assert(strstr(message, "8 byte") != NULL,
	          "message does not say how short the file is");
}

MU_TEST(test_image_rejects_a_binary_with_no_suffix)
{
	uint8_t image[PAYLOAD_LENGTH];
	struct dfu_image_report report;
	char message[160];

	/* A .bin straight out of the build: the last 16 bytes are padding. */
	memset(image, 0, sizeof(image));
	mu_assert_int_eq(DFU_IMAGE_SUFFIX_LENGTH,
	                 check_image_bytes(image, sizeof(image), &report, message,
	                                   sizeof(message)));
	mu_assert(strstr(message, "Unknown DFU suffix length: 0") != NULL,
	          "message does not report a zero suffix length");
}

MU_TEST(test_image_rejects_a_missing_ufd_signature)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	char message[160];
	size_t length;

	spec.bad_signature = 1;
	length = build_image(&spec, PAYLOAD_LENGTH, image, sizeof(image));
	mu_assert_int_eq(DFU_IMAGE_SIGNATURE,
	                 check_image_bytes(image, length, &report, message,
	                                   sizeof(message)));
	mu_assert(strstr(message, "DFU Signature mismatch") != NULL,
	          "signature failure not named in the message");
}

MU_TEST(test_image_rejects_an_unknown_dfu_version)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	char message[160];
	size_t length;

	spec.bcd_dfu = 0x0200;
	length = build_image(&spec, PAYLOAD_LENGTH, image, sizeof(image));
	mu_assert_int_eq(DFU_IMAGE_DFU_VERSION,
	                 check_image_bytes(image, length, &report, message,
	                                   sizeof(message)));
	mu_assert(strstr(message, "Unknown DFU version: 0200") != NULL,
	          "message does not report the DFU version found");
}

MU_TEST(test_image_accepts_dfu_1_1)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	size_t length;

	spec.bcd_dfu = DFU_VERSION_1_1;
	length = build_image(&spec, PAYLOAD_LENGTH, image, sizeof(image));
	mu_assert_int_eq(DFU_IMAGE_OK,
	                 check_image_bytes(image, length, &report, NULL, 0));
}

MU_TEST(test_image_rejects_a_suffix_with_no_firmware)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	char message[160];
	size_t length = build_image(&spec, 0, image, sizeof(image));

	mu_assert_int_eq(DFU_SUFFIX_LENGTH, length);
	mu_assert_int_eq(DFU_IMAGE_NO_FIRMWARE,
	                 check_image_bytes(image, length, &report, message,
	                                   sizeof(message)));
	mu_assert(strstr(message, "no firmware") != NULL,
	          "message does not say the image has no firmware");
}

MU_TEST(test_image_rejects_another_vendor)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	char message[160];
	size_t length;

	spec.id_vendor = 0x1234;
	length = build_image(&spec, PAYLOAD_LENGTH, image, sizeof(image));
	mu_assert_int_eq(DFU_IMAGE_VENDOR,
	                 check_image_bytes(image, length, &report, message,
	                                   sizeof(message)));
	mu_assert_int_eq(0x1234, report.id_vendor);
	mu_assert(strstr(message, "vendor 0x1234") != NULL,
	          "message does not name the vendor the image is for");
}

MU_TEST(test_image_rejects_another_product)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	char message[160];
	size_t length;

	spec.id_product = 0x6004;
	length = build_image(&spec, PAYLOAD_LENGTH, image, sizeof(image));
	mu_assert_int_eq(DFU_IMAGE_PRODUCT,
	                 check_image_bytes(image, length, &report, message,
	                                   sizeof(message)));
	mu_assert_int_eq(0x6004, report.id_product);
	mu_assert(strstr(message, "product 0x6004") != NULL,
	          "message does not name the product the image is for");
}

MU_TEST(test_image_accepts_the_wildcard_vendor_and_product)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	size_t length;

	/* 0xffff means "any device" in DFU 1.1. */
	spec.id_vendor = DFU_ID_WILDCARD;
	spec.id_product = DFU_ID_WILDCARD;
	length = build_image(&spec, PAYLOAD_LENGTH, image, sizeof(image));
	mu_assert_int_eq(DFU_IMAGE_OK,
	                 check_image_bytes(image, length, &report, NULL, 0));
}

MU_TEST(test_image_accepts_a_doubly_suffixed_file)
{
	struct image_spec spec = good_spec();
	uint8_t image[IMAGE_MAX];
	struct dfu_image_report report;
	size_t length = build_image(&spec, PAYLOAD_LENGTH, image, sizeof(image));

	/* A suffix appended to an already-suffixed file is a valid DFU file whose
	 * firmware happens to end in a suffix, and this check cannot tell the
	 * difference. Stopping -s from signing a signed file therefore needs its
	 * own check in -s, which this one is not. */
	length = append_suffix(&spec, image, length, sizeof(image));
	mu_assert_int_eq(PAYLOAD_LENGTH + 2 * DFU_SUFFIX_LENGTH, length);
	mu_assert_int_eq(DFU_IMAGE_OK,
	                 check_image_bytes(image, length, &report, NULL, 0));
}

/*
 * The size cap: every whole-file read is sized from the file, so the file is
 * measured before anything is allocated.
 */

/* A sparse file of exactly length bytes: one byte written past the end, so a
 * half-megabyte case costs nothing. */
static FILE *sparse_file(long long length)
{
	FILE *file = tmpfile();

	mu_assert(file != NULL, "tmpfile failed");
	if (file == NULL)
		return NULL;
	/* Each step is checked, and the file is dropped rather than returned
	 * half-made, so a failure here cannot look like a passing case. */
	if (fseek(file, (long)(length - 1), SEEK_SET) != 0 ||
	    fputc(0, file) == EOF || fflush(file) != 0 ||
	    fseek(file, 0, SEEK_SET) != 0) {
		mu_assert(0, "could not make the sparse file");
		fclose(file);
		return NULL;
	}
	return file;
}

MU_TEST(test_max_image_size_comes_from_the_part_table)
{
	const uint32_t application = BOOTLOADER_OFFSET + BOOTLOADER_SIZE;
	const struct dfu_part *part;
	uint32_t largest = 0;
	size_t i;

	for (i = 0; (part = dfu_part_at(i)) != NULL; i++) {
		if (part->flash_size > largest)
			largest = part->flash_size;
	}
	/* The application region of the largest part, plus the DFU suffix the
	 * file carries and the bootloader never sees. */
	mu_assert_int_eq((int)(largest - application + DFU_SUFFIX_LENGTH),
	                 (int)dfu_max_image_size());
	/* Nothing above the bootloader's address limit can be written, so the
	 * cap cannot exceed that region either. */
	mu_assert(dfu_max_image_size() <=
	          BOOTLOADER_FLASH_LIMIT - application + DFU_SUFFIX_LENGTH,
	          "the cap is past what the bootloader can write");
	/* A real image still fits: the firmware is a few tens of KiB. */
	mu_assert(dfu_max_image_size() > 64u * 1024u, "the cap is too small");
}

MU_TEST(test_image_rejects_a_file_past_the_size_cap)
{
	long long over = (long long)dfu_max_image_size() + 1;
	FILE *image = sparse_file(over);
	struct dfu_image_report report;
	char message[160];

	if (image == NULL)
		return;
	mu_assert_int_eq(DFU_IMAGE_TOO_LARGE,
	                 dfu_image_check(image, U1_DFU_VENDORID,
	                                 U1_DFU_PRODUCTID, &report));
	mu_assert(report.image_size == over,
	          "the report does not say how big the file is");
	dfu_image_describe_failure(&report, message, sizeof(message));
	mu_assert(strstr(message, "an Ubertooth image is at most") != NULL,
	          "message does not name the limit");
	fclose(image);
}

/* A file of exactly the cap is accepted, so with the case above the cap is
 * the size of the largest image and not a byte either side of it. */
MU_TEST(test_image_accepts_a_file_at_the_size_cap)
{
	size_t length = dfu_max_image_size();
	struct image_spec spec = good_spec();
	struct dfu_image_report report;
	uint8_t *buffer = malloc(length);
	FILE *image = tmpfile();

	mu_assert(buffer != NULL && image != NULL, "allocation failed");
	if (buffer == NULL || image == NULL) {
		free(buffer);
		if (image != NULL)
			fclose(image);
		return;
	}
	mu_assert_int_eq((int)length,
	                 (int)build_image(&spec, length - DFU_SUFFIX_LENGTH,
	                                  buffer, length));
	mu_assert(fwrite(buffer, 1, length, image) == length, "short write");
	mu_assert(fflush(image) == 0, "flush failed");
	mu_assert_int_eq(DFU_IMAGE_OK,
	                 dfu_image_check(image, U1_DFU_VENDORID,
	                                 U1_DFU_PRODUCTID, &report));
	mu_assert(report.image_size == (long long)length,
	          "wrong image size reported");
	fclose(image);
	free(buffer);
}

MU_TEST(test_image_rejects_a_file_that_is_not_a_regular_file)
{
	struct dfu_image_report report;
	char message[160];
	const char *directory_name = getenv("TMPDIR");
	char path[256];
	FILE *directory;
	FILE *fifo;
	int descriptor;

	/* A directory: the tool is handed one by a careless -d. */
	directory = fopen(".", "rb");
	if (directory != NULL) {
		mu_assert_int_eq(DFU_IMAGE_NOT_REGULAR,
		                 dfu_image_check(directory, U1_DFU_VENDORID,
		                                 U1_DFU_PRODUCTID, &report));
		dfu_image_describe_failure(&report, message, sizeof(message));
		mu_assert(strstr(message, "regular file") != NULL,
		          "message does not say the file is not a regular one");
		fclose(directory);
	} else {
		printf("  (a directory could not be opened here)\n");
	}

	/* A FIFO: ftell() says nothing about what a read would return, and the
	 * writer could keep sending for as long as it likes. Opened read-write
	 * so this test is its own reader and the open does not block. */
	if (directory_name == NULL || directory_name[0] == '\0')
		directory_name = "/tmp";
	snprintf(path, sizeof(path), "%s/ubertooth-dfu-test-fifo-%ld",
	         directory_name, (long)getpid());
	unlink(path);  /* a FIFO an earlier run left behind */
	mu_assert(mkfifo(path, 0600) == 0, "mkfifo failed");
	descriptor = open(path, O_RDWR);
	mu_assert(descriptor >= 0, "the FIFO would not open");
	fifo = fdopen(descriptor, "rb");
	mu_assert(fifo != NULL, "fdopen failed");
	if (fifo != NULL) {
		mu_assert_int_eq(DFU_IMAGE_NOT_REGULAR,
		                 dfu_image_check(fifo, U1_DFU_VENDORID,
		                                 U1_DFU_PRODUCTID, &report));
		/* A FIFO cannot be seeked, so there is no fixed offset to read
		 * the firmware info block from either. */
		{
			struct ubertooth_fw_info info;

			mu_assert_int_eq(UBERTOOTH_FW_INFO_SHORT,
			                 dfu_read_image_info(fifo, &info));
		}
		fclose(fifo);
	} else if (descriptor >= 0) {
		close(descriptor);
	}
	unlink(path);
}

/* The cap is the caller's, so -s can hold a .bin to the suffix less than a
 * whole image and the message can name the right limit. */
MU_TEST(test_size_check_measures_against_the_cap_it_is_given)
{
	unsigned long image_cap = dfu_max_image_size();
	unsigned long bin_cap = image_cap - DFU_SUFFIX_LENGTH;
	FILE *file = sparse_file((long long)bin_cap + 1);
	long long size = 0;

	if (file == NULL)
		return;
	mu_assert_int_eq(DFU_FILE_SIZE_OK,
	                 dfu_image_size_check(file, image_cap, &size));
	mu_assert(size == (long long)bin_cap + 1, "wrong size measured");
	mu_assert_int_eq(DFU_FILE_SIZE_TOO_LARGE,
	                 dfu_image_size_check(file, bin_cap, &size));
	mu_assert(size == (long long)bin_cap + 1,
	          "the size is not reported for a refused file");
	/* The guard reads nothing, so the position is still the start. */
	mu_assert_int_eq(0, (int)ftell(file));
	fclose(file);
}

/*
 * The firmware info block every image carries, at the fixed offset
 * ubertooth-dfu reads it from. These build files by hand rather than from a
 * real image, so the offset, the magic and the field positions are pinned
 * here; firmware/tests/test_fw_info.c checks that a block the firmware builds
 * is read the same way.
 */

/* A 512-byte file of filler with the 64-byte block written over it at offset,
 * so that every case is a file long enough to hold a block at 0x100 whether
 * or not one is there. */
static FILE *file_with_block(long offset, const char *magic, uint16_t format,
                             uint16_t api, const char *image,
                             const char *revision)
{
	uint8_t block[UBERTOOTH_FW_INFO_LENGTH];
	uint8_t filler[0x200];
	FILE *file = tmpfile();
	size_t i;

	if (file == NULL)
		return NULL;
	/* Filler that is not a block, and not 0xff padding either, so a read at
	 * the wrong offset cannot accidentally pass. */
	for (i = 0; i < sizeof(filler); i++)
		filler[i] = (uint8_t)(i & 0xff);
	memset(block, 0, sizeof(block));
	memcpy(block + UBERTOOTH_FW_INFO_OFF_MAGIC, magic,
	       UBERTOOTH_FW_INFO_MAGIC_LEN);
	block[UBERTOOTH_FW_INFO_OFF_FORMAT] = (uint8_t)(format & 0xff);
	block[UBERTOOTH_FW_INFO_OFF_FORMAT + 1] = (uint8_t)(format >> 8);
	block[UBERTOOTH_FW_INFO_OFF_API] = (uint8_t)(api & 0xff);
	block[UBERTOOTH_FW_INFO_OFF_API + 1] = (uint8_t)(api >> 8);
	memcpy(block + UBERTOOTH_FW_INFO_OFF_IMAGE, image, strlen(image));
	memcpy(block + UBERTOOTH_FW_INFO_OFF_REVISION, revision,
	       strlen(revision));
	if (fwrite(filler, 1, sizeof(filler), file) != sizeof(filler) ||
	    fseek(file, offset, SEEK_SET) != 0 ||
	    fwrite(block, 1, sizeof(block), file) != sizeof(block)) {
		fclose(file);
		return NULL;
	}
	rewind(file);
	return file;
}

/* A file holding a well-formed block where an image carries one. */
static FILE *image_with_block(const char *image, const char *revision)
{
	return file_with_block(UBERTOOTH_FW_INFO_OFFSET,
	                       UBERTOOTH_FW_INFO_MAGIC,
	                       UBERTOOTH_FW_INFO_FORMAT, 0x0134, image,
	                       revision);
}

MU_TEST(test_image_info_is_read_from_the_fixed_offset)
{
	struct ubertooth_fw_info info;
	FILE *image = image_with_block("bluetooth_rxtx", "git-deadbee*");

	mu_assert_fatal(image != NULL, "could not build the image");
	/* The read starts somewhere else to show it seeks, and the position is
	 * put back for the caller that is part-way through its own checks. */
	mu_assert_int_eq(0, fseek(image, 7, SEEK_SET));
	mu_assert_int_eq(UBERTOOTH_FW_INFO_OK,
	                 dfu_read_image_info(image, &info));
	mu_assert(strcmp(info.image, "bluetooth_rxtx") == 0,
	          "wrong image name read from the block");
	mu_assert(strcmp(info.revision, "git-deadbee*") == 0,
	          "wrong revision read from the block");
	mu_assert_int_eq(UBERTOOTH_FW_INFO_FORMAT, info.format_version);
	mu_assert_int_eq(0x0134, info.api_version);
	mu_assert_int_eq(7, (int)ftell(image));
	fclose(image);
}

/* The other image, so the block is what tells the two apart. */
MU_TEST(test_image_info_names_the_btbr_image)
{
	struct ubertooth_fw_info info;
	FILE *image = image_with_block("btbr", "git-deadbee*");

	mu_assert_fatal(image != NULL, "could not build the image");
	mu_assert_int_eq(UBERTOOTH_FW_INFO_OK,
	                 dfu_read_image_info(image, &info));
	mu_assert(strcmp(info.image, "btbr") == 0, "wrong image name");
	fclose(image);
}

/* A field filled to its last byte carries no terminator, so a reader that
 * treated it as a C string would run into the next field. */
MU_TEST(test_image_info_reads_a_field_with_no_room_for_a_terminator)
{
	struct ubertooth_fw_info info;
	FILE *image = image_with_block("0123456789abcdef",
	                               "0123456789abcdef0123456789abcdef");

	mu_assert_fatal(image != NULL, "could not build the image");
	mu_assert_int_eq(UBERTOOTH_FW_INFO_OK,
	                 dfu_read_image_info(image, &info));
	mu_assert(strcmp(info.image, "0123456789abcdef") == 0,
	          "a full image field was not read whole");
	mu_assert(strcmp(info.revision,
	                 "0123456789abcdef0123456789abcdef") == 0,
	          "a full revision field was not read whole");
	fclose(image);
}

/* A file that is not an image from this tree: nothing at the offset. This is
 * what ubertooth-dfu -d refuses before it erases anything. */
MU_TEST(test_image_info_refuses_a_file_with_no_block)
{
	struct ubertooth_fw_info info;
	FILE *image = tmpfile();
	uint8_t bytes[0x200];
	size_t i;

	mu_assert_fatal(image != NULL, "tmpfile failed");
	for (i = 0; i < sizeof(bytes); i++)
		bytes[i] = (uint8_t)(i & 0xff);
	mu_assert_int_eq((int)sizeof(bytes),
	                 (int)fwrite(bytes, 1, sizeof(bytes), image));
	mu_assert_int_eq(UBERTOOTH_FW_INFO_NO_MAGIC,
	                 dfu_read_image_info(image, &info));
	mu_assert(info.revision[0] == '\0' && info.image[0] == '\0',
	          "a refused block left text behind");
	fclose(image);
}

/* A block the firmware put somewhere else is not found: the whole point of
 * the fixed offset is that nothing searches the file. */
MU_TEST(test_image_info_refuses_a_block_at_another_offset)
{
	struct ubertooth_fw_info info;
	FILE *image = file_with_block(0x80, UBERTOOTH_FW_INFO_MAGIC,
	                              UBERTOOTH_FW_INFO_FORMAT, 0x0134,
	                              "bluetooth_rxtx", "git-deadbee*");

	mu_assert_fatal(image != NULL, "could not build the image");
	mu_assert_int_eq(UBERTOOTH_FW_INFO_NO_MAGIC,
	                 dfu_read_image_info(image, &info));
	fclose(image);
}

/* A format this host does not know: the fields may have moved, so nothing is
 * read out of them, but the number found is reported so a message can name it. */
MU_TEST(test_image_info_refuses_an_unknown_block_format)
{
	struct ubertooth_fw_info info;
	FILE *image = file_with_block(UBERTOOTH_FW_INFO_OFFSET,
	                              UBERTOOTH_FW_INFO_MAGIC, 99, 0x0134,
	                              "bluetooth_rxtx", "git-deadbee*");

	mu_assert_fatal(image != NULL, "could not build the image");
	mu_assert_int_eq(UBERTOOTH_FW_INFO_FORMAT_UNKNOWN,
	                 dfu_read_image_info(image, &info));
	mu_assert_int_eq(99, info.format_version);
	mu_assert(info.image[0] == '\0' && info.revision[0] == '\0',
	          "fields of an unknown format were read anyway");
	fclose(image);
}

/* An empty or unprintable string is not a name or a revision: the magic can
 * be matched by a file that is not an image, so the text is checked too. */
MU_TEST(test_image_info_refuses_unreadable_text)
{
	struct ubertooth_fw_info info;
	FILE *image;

	image = image_with_block("", "git-deadbee*");
	mu_assert_fatal(image != NULL, "could not build the image");
	mu_assert_int_eq(UBERTOOTH_FW_INFO_BAD_TEXT,
	                 dfu_read_image_info(image, &info));
	fclose(image);

	image = image_with_block("bluetooth_rxtx", "");
	mu_assert_fatal(image != NULL, "could not build the image");
	mu_assert_int_eq(UBERTOOTH_FW_INFO_BAD_TEXT,
	                 dfu_read_image_info(image, &info));
	fclose(image);

	image = image_with_block("rxtx\x01\x02", "git-deadbee*");
	mu_assert_fatal(image != NULL, "could not build the image");
	mu_assert_int_eq(UBERTOOTH_FW_INFO_BAD_TEXT,
	                 dfu_read_image_info(image, &info));
	fclose(image);
}

/* A file with nothing at the offset to read: the 64 bytes are not there. */
MU_TEST(test_image_info_refuses_a_file_too_short_for_a_block)
{
	struct ubertooth_fw_info info;
	FILE *image = tmpfile();
	uint8_t bytes[UBERTOOTH_FW_INFO_OFFSET + 8];

	mu_assert_fatal(image != NULL, "tmpfile failed");
	memset(bytes, 0, sizeof(bytes));
	mu_assert_int_eq((int)sizeof(bytes),
	                 (int)fwrite(bytes, 1, sizeof(bytes), image));
	mu_assert_int_eq(UBERTOOTH_FW_INFO_SHORT,
	                 dfu_read_image_info(image, &info));
	fclose(image);
}

/* The read-back length follows the part's flash, so the bootloader is never
 * asked for an address with no flash behind it. */
MU_TEST(test_part_lookup_knows_the_lpc17xx_flash_sizes)
{
	const struct dfu_part *part;

	part = dfu_part_lookup(0x25011722);
	mu_assert_fatal(part != NULL, "LPC1754 is not in the table");
	mu_assert(strcmp(part->name, "LPC1754") == 0, "wrong part name");
	mu_assert_int_eq(128 * 1024, (int)part->flash_size);

	part = dfu_part_lookup(0x25011723);
	mu_assert_fatal(part != NULL, "LPC1756 is not in the table");
	mu_assert_int_eq(256 * 1024, (int)part->flash_size);

	part = dfu_part_lookup(0x26113F37);
	mu_assert_fatal(part != NULL, "LPC1769 is not in the table");
	mu_assert_int_eq(512 * 1024, (int)part->flash_size);

	mu_assert(dfu_part_lookup(0x12345678) == NULL,
	          "an unknown part ID was matched");
	mu_assert(dfu_part_lookup(0) == NULL, "a zero part ID was matched");
}

/* The table is the load-bearing data of the read-back: an entry claiming more
 * flash than its part has is what takes a bootloader down, so check the shape
 * of every entry rather than the three looked up above. */
MU_TEST(test_every_part_entry_is_usable_and_within_the_bootloader_limit)
{
	const uint32_t application = BOOTLOADER_OFFSET + BOOTLOADER_SIZE;
	const struct dfu_part *part;
	size_t i, j;

	mu_assert(dfu_part_at(0) != NULL, "the part table is empty");
	for (i = 0; (part = dfu_part_at(i)) != NULL; i++) {
		mu_assert(part->part_id != 0, "a part entry has no ID");
		mu_assert(part->name != NULL && part->name[0] != '\0',
		          "a part entry has no name");
		mu_assert(part->flash_size > application,
		          "a part entry has no flash above the bootloader");
		mu_assert_int_eq(0, (int)(part->flash_size % BLOCK_SIZE));
		/* A bootloader bounds a part it cannot identify at 512 KiB, and an
		 * older one bounds every part there, so an entry above it would ask
		 * for an address a bootloader refuses. */
		mu_assert(part->flash_size <= BOOTLOADER_FLASH_LIMIT,
		          "a part entry claims more flash than the bootloader allows");
		mu_assert(dfu_part_lookup(part->part_id) != NULL,
		          "a part in the table cannot be looked up");
		for (j = 0; j < i; j++) {
			mu_assert(dfu_part_at(j)->part_id != part->part_id ||
			          dfu_part_at(j)->flash_size == part->flash_size,
			          "one part ID is listed with two flash sizes");
		}
	}
}

MU_TEST(test_upload_length_covers_the_flash_above_the_bootloader)
{
	const uint32_t application = BOOTLOADER_OFFSET + BOOTLOADER_SIZE;

	mu_assert_int_eq((int)(128 * 1024 - application),
	                 (int)dfu_upload_length(128 * 1024));
	mu_assert_int_eq((int)(256 * 1024 - application),
	                 (int)dfu_upload_length(256 * 1024));
	mu_assert_int_eq((int)(512 * 1024 - application),
	                 (int)dfu_upload_length(512 * 1024));
	/* An unknown part gets the size the application is linked for. */
	mu_assert_int_eq((int)(DFU_ASSUMED_FLASH_SIZE - application),
	                 (int)dfu_upload_length(0));
	/* A part with no flash past the bootloader has nothing to read. */
	mu_assert_int_eq(0, (int)dfu_upload_length(application));
	mu_assert_int_eq(0, (int)dfu_upload_length(application / 2));
	/* Every length is a whole number of blocks, so no read straddles the
	 * end of flash. */
	mu_assert_int_eq(0, (int)(dfu_upload_length(0) % BLOCK_SIZE));
	mu_assert_int_eq(0, (int)(dfu_upload_length(32 * 1024) % BLOCK_SIZE));
}

MU_TEST(test_download_length_rounds_the_whole_file_up_to_a_block)
{
	/* A file that fills whole blocks costs exactly those. */
	mu_assert_int_eq(0, (int)dfu_download_length(0));
	mu_assert_int_eq(BLOCK_SIZE, (int)dfu_download_length(BLOCK_SIZE));
	mu_assert_int_eq(4 * BLOCK_SIZE, (int)dfu_download_length(4 * BLOCK_SIZE));
	/* One that does not is still written as whole blocks, padded. */
	mu_assert_int_eq(BLOCK_SIZE, (int)dfu_download_length(1));
	mu_assert_int_eq(BLOCK_SIZE, (int)dfu_download_length(BLOCK_SIZE - 1));
	mu_assert_int_eq(2 * BLOCK_SIZE, (int)dfu_download_length(BLOCK_SIZE + 1));
	/* The DFU suffix is part of the file, so an image that exactly fills the
	 * application region of a part needs one more block than the region once
	 * the suffix is added. */
	mu_assert_int_eq((int)dfu_upload_length(128 * 1024),
	                 (int)dfu_download_length(dfu_upload_length(128 * 1024)));
	mu_assert_int_eq((int)dfu_upload_length(128 * 1024) + BLOCK_SIZE,
	                 (int)dfu_download_length(
		                 dfu_upload_length(128 * 1024) + DFU_SUFFIX_LENGTH));
}

int main(void)
{
	printf("DFU recovery tests\n");
	MU_RUN(test_part_lookup_knows_the_lpc17xx_flash_sizes);
	MU_RUN(test_every_part_entry_is_usable_and_within_the_bootloader_limit);
	MU_RUN(test_upload_length_covers_the_flash_above_the_bootloader);
	MU_RUN(test_download_length_rounds_the_whole_file_up_to_a_block);
	MU_RUN(test_transfer_reopens_same_path_and_retries_from_start);
	MU_RUN(test_transfer_reopen_timeout_is_distinct);
	MU_RUN(test_transfer_claim_failure_closes_unclaimed_handle);
	MU_RUN(test_transfer_retry_exhaustion_closes_final_handle);
	MU_RUN(test_verify_waits_for_same_app_and_exact_revision);
	MU_RUN(test_verify_rejects_wrong_physical_path);
	MU_RUN(test_verify_rejects_wrong_revision);
	MU_RUN(test_verify_rejects_the_other_image_of_the_same_revision);
	MU_RUN(test_verify_separates_an_unreadable_image_from_a_wrong_one);
	MU_RUN(test_verify_does_not_ask_for_the_image_of_a_wrong_revision);
	MU_RUN(test_verify_needs_an_expected_image_and_a_way_to_read_it);
	MU_RUN(test_a_board_that_never_came_back_is_told_apart_from_a_failed_write);
	MU_RUN(test_a_board_that_will_not_open_is_not_a_board_that_stayed_away);
	MU_RUN(test_a_board_running_another_revision_is_described_as_a_failed_write);
	MU_RUN(test_a_board_that_will_not_report_its_revision_is_not_a_mismatch);
	MU_RUN(test_the_other_image_is_described_as_a_failed_write);
	MU_RUN(test_an_unreadable_image_is_described_as_unchecked);
	MU_RUN(test_a_device_on_another_path_is_described_as_unchecked);
	MU_RUN(test_describing_a_verification_survives_what_a_caller_passes);
	MU_RUN(test_idle_stops_on_the_first_failed_state_read);
	MU_RUN(test_idle_is_bounded_when_the_state_never_settles);
	MU_RUN(test_idle_aborts_a_half_finished_transfer_then_succeeds);
	MU_RUN(test_idle_clears_an_error_state_then_succeeds);
	MU_RUN(test_idle_reports_a_failed_abort_or_clear);
	MU_RUN(test_idle_refuses_a_state_it_cannot_drive);
	MU_RUN(test_idle_refuses_an_unbounded_configuration);
	MU_RUN(test_image_accepts_a_signed_ubertooth_image);
	MU_RUN(test_crc32_matches_the_standard_check_value);
	MU_RUN(test_image_rejects_a_wrong_crc);
	MU_RUN(test_image_rejects_a_truncated_image);
	MU_RUN(test_image_rejects_a_file_shorter_than_a_suffix);
	MU_RUN(test_image_rejects_a_binary_with_no_suffix);
	MU_RUN(test_image_rejects_a_missing_ufd_signature);
	MU_RUN(test_image_rejects_an_unknown_dfu_version);
	MU_RUN(test_image_accepts_dfu_1_1);
	MU_RUN(test_image_rejects_a_suffix_with_no_firmware);
	MU_RUN(test_image_rejects_another_vendor);
	MU_RUN(test_image_rejects_another_product);
	MU_RUN(test_image_accepts_the_wildcard_vendor_and_product);
	MU_RUN(test_image_accepts_a_doubly_suffixed_file);
	MU_RUN(test_max_image_size_comes_from_the_part_table);
	MU_RUN(test_size_check_measures_against_the_cap_it_is_given);
	MU_RUN(test_image_rejects_a_file_past_the_size_cap);
	MU_RUN(test_image_accepts_a_file_at_the_size_cap);
	MU_RUN(test_image_rejects_a_file_that_is_not_a_regular_file);
	MU_RUN(test_image_info_is_read_from_the_fixed_offset);
	MU_RUN(test_image_info_names_the_btbr_image);
	MU_RUN(test_image_info_reads_a_field_with_no_room_for_a_terminator);
	MU_RUN(test_image_info_refuses_a_file_with_no_block);
	MU_RUN(test_image_info_refuses_a_block_at_another_offset);
	MU_RUN(test_image_info_refuses_an_unknown_block_format);
	MU_RUN(test_image_info_refuses_unreadable_text);
	MU_RUN(test_image_info_refuses_a_file_too_short_for_a_block);
	return mu_report();
}
