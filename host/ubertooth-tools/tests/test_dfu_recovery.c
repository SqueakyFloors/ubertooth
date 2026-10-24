#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
	/* The harness and the issue reports match on this wording. */
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
	 * own check in -s, which is issue #200, not this one. */
	length = append_suffix(&spec, image, length, sizeof(image));
	mu_assert_int_eq(PAYLOAD_LENGTH + 2 * DFU_SUFFIX_LENGTH, length);
	mu_assert_int_eq(DFU_IMAGE_OK,
	                 check_image_bytes(image, length, &report, NULL, 0));
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
	MU_RUN(test_extract_revision_from_existing_compile_info_marker);
	return mu_report();
}
