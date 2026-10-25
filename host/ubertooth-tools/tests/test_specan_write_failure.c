/*
 * What ubertooth-specan does when it cannot write the samples it captured.
 *
 * The sample write is the one place a specan run can fail after the device is
 * open and the sweep is running: a redirect to a full disk, or `-d -` into a
 * reader that went away. Reporting that and then exiting 0 told a script the
 * capture it never received had succeeded, so the failure is recorded, the
 * receive loop stops, and the run exits 1.
 *
 * cb_specan() is static in ubertooth-specan.c, so that source is compiled in
 * here with main() renamed, as test_util_main.c does. No device is opened:
 * the callback is handed a fifo this test filled itself.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_specan_main
#include "ubertooth-specan.c"
#undef main

/* A block of samples the board could have sent, all inside 2402-2480 MHz so
 * none is dropped as left over from an earlier run. */
static void fill_block(usb_pkt_rx* rx)
{
	int j;

	memset(rx, 0, sizeof(*rx));
	for (j = 0; j < DMA_SIZE - 2; j += 3) {
		uint16_t freq = (uint16_t)(BT_FREQ_MIN + (j / 3) % 79);

		rx->data[j] = (uint8_t)(freq >> 8);
		rx->data[j + 1] = (uint8_t)(freq & 0xff);
		rx->data[j + 2] = (uint8_t)-70; /* RSSI */
	}
}

/* The arguments cb_specan() unpacks: the whole band, writing to a file. */
static void specan_args(uint8_t args[5], uint8_t mode)
{
	args[0] = (uint8_t)(BT_FREQ_MAX & 0xff);
	args[1] = (uint8_t)(BT_FREQ_MAX >> 8);
	args[2] = mode;
	args[3] = (uint8_t)(BT_FREQ_MIN & 0xff);
	args[4] = (uint8_t)(BT_FREQ_MIN >> 8);
}

/*
 * A stream whose writes cannot reach anywhere: a real file opened for
 * writing, whose descriptor is then replaced by a read-only one, so a write
 * to it fails with EBADF. The descriptor stays open, so its number cannot be
 * handed to something else while the stream still holds it. Buffered, the
 * 3-byte writes are accepted and only the flush fails; unbuffered, each write
 * fails as it is made.
 */
static FILE* broken_stream(int buffered)
{
	char path[] = "/tmp/specan-broken-XXXXXX";
	int fd = mkstemp(path);
	int readonly;
	FILE* f;

	if (fd < 0)
		return NULL;
	f = fdopen(fd, "w");
	if (f == NULL) {
		close(fd);
		return NULL;
	}
	unlink(path);
	if (!buffered)
		setvbuf(f, NULL, _IONBF, 0);
	readonly = open("/dev/null", O_RDONLY);
	if (readonly < 0) {
		fclose(f);
		return NULL;
	}
	dup2(readonly, fileno(f));
	close(readonly);
	return f;
}

/* Standard error, redirected to a temp file and read back. */
static char capture_path[] = "/tmp/specan-err-XXXXXX";
static int capture_fd = -1;
static int capture_saved = -1;

static void capture_stderr_begin(void)
{
	char template[] = "/tmp/specan-err-XXXXXX";

	memcpy(capture_path, template, sizeof(template));
	capture_fd = mkstemp(capture_path);
	fflush(stderr);
	capture_saved = dup(fileno(stderr));
	dup2(capture_fd, fileno(stderr));
}

static void capture_stderr_end(char* err, size_t err_size)
{
	FILE* captured;
	long n;

	fflush(stderr);
	dup2(capture_saved, fileno(stderr));
	close(capture_saved);
	captured = fopen(capture_path, "r");
	n = (long)fread(err, 1, err_size - 1, captured);
	err[n] = '\0';
	fclose(captured);
	close(capture_fd);
	unlink(capture_path);
}

/* One capture, with dumpfile set to the stream given, and whatever the
 * callback wrote to standard error returned in err. */
static void one_block(ubertooth_t* ut, FILE* out, uint8_t mode,
                      char* err, size_t err_size)
{
	usb_pkt_rx rx;
	uint8_t args[5];

	memset(ut, 0, sizeof(*ut));
	ut->fifo = fifo_init();
	fill_block(&rx);
	fifo_push(ut->fifo, &rx);
	specan_args(args, mode);

	output_failed = 0;
	write_failure_said = 0;
	dumpfile = out;

	capture_stderr_begin();
	cb_specan(ut, args);
	capture_stderr_end(err, err_size);
	free(ut->fifo);
}

MU_TEST(a_failed_sample_write_ends_the_run)
{
	ubertooth_t ut;
	FILE* unwritable;
	char err[512];

	/* Unbuffered, so the write goes straight to a descriptor that is gone:
	 * it fails in fwrite() rather than at the flush, which is the full-disk
	 * case the sweep has to notice while it is running. */
	unwritable = broken_stream(0);
	mu_assert_fatal(unwritable != NULL, "the unbuffered stream opened");

	one_block(&ut, unwritable, SPECAN_FILE, err, sizeof(err));
	fclose(unwritable);

	mu_assert_int_eq(1, output_failed);
	/* The receive loop in main() watches this, so the run ends here rather
	 * than repeating the message for every block of a live sweep. */
	mu_assert_int_eq(1, ut.stop_ubertooth);
	mu_assert(strncmp(err, "error: writing samples failed: wrote 0 of 3 bytes",
	                  48) == 0, "one error: line, with the byte count");
	mu_assert(strchr(err, '(') != NULL, "and the reason the write failed");
	/* Said once per run, not once per sample in the block. */
	mu_assert(strchr(err, '\n') == strrchr(err, '\n'), "said once");
}

/*
 * A stream can take a write into its buffer and only fail when that buffer is
 * flushed, so every write can look fine and the samples still never arrive.
 * The run is not finished until the output has been flushed, which is what
 * main() checks after the sweep ends.
 */
MU_TEST(samples_lost_in_a_buffer_end_the_run_too)
{
	ubertooth_t ut;
	FILE* buffered;
	char err[512];
	char out[512];

	/* Buffered: the 3-byte writes land in stdio's buffer and are accepted,
	 * and nothing reaches the descriptor until the flush, which fails. */
	buffered = broken_stream(1);
	mu_assert_fatal(buffered != NULL, "the buffered stream opened");

	one_block(&ut, buffered, SPECAN_FILE, err, sizeof(err));
	/* Every write succeeded, so the sweep ran to the end as normal. */
	mu_assert_int_eq(0, output_failed);
	mu_assert_int_eq(0, ut.stop_ubertooth);
	mu_assert(strcmp(err, "") == 0, "nothing said while the writes worked");

	/* What main() does once the sweep has ended. */
	write_failure_said = 0;
	capture_stderr_begin();
	mu_assert_int_eq(-1, specan_output_flushed(buffered));
	capture_stderr_end(out, sizeof(out));
	fclose(buffered);

	mu_assert(strncmp(out, "error: writing samples failed: could not flush",
	                  45) == 0, "one error: line, naming the flush");
	mu_assert(strchr(out, '\n') == strrchr(out, '\n'), "said once");

	/* A stream that is fine reports nothing and does not fail the run. */
	write_failure_said = 0;
	capture_stderr_begin();
	mu_assert_int_eq(0, specan_output_flushed(stderr));
	/* No output stream at all (the text modes leave dumpfile NULL). */
	mu_assert_int_eq(0, specan_output_flushed(NULL));
	capture_stderr_end(out, sizeof(out));
	mu_assert(strcmp(out, "") == 0, "a working stream says nothing");
}

MU_TEST(a_write_that_works_leaves_the_run_alone)
{
	ubertooth_t ut;
	char path[] = "/tmp/specan-out-XXXXXX";
	int fd;
	FILE* writable;
	char err[512];
	long size;

	fd = mkstemp(path);
	writable = fdopen(fd, "w");
	mu_assert(writable != NULL, "temp file opened");

	one_block(&ut, writable, SPECAN_FILE, err, sizeof(err));
	fflush(writable);
	fseek(writable, 0, SEEK_END);
	size = ftell(writable);
	fclose(writable);
	unlink(path);

	mu_assert_int_eq(0, output_failed);
	mu_assert_int_eq(0, ut.stop_ubertooth);
	mu_assert(strcmp(err, "") == 0, "nothing printed");
	/* Three bytes per sample, for every sample in the block. */
	mu_assert_int_eq(0, (int)(size % 3));
	mu_assert(size > 0, "the samples were written");
}

MU_TEST(an_output_mode_that_is_not_one_of_ours_ends_the_run)
{
	ubertooth_t ut;
	char err[512];

	one_block(&ut, NULL, (uint8_t)200, err, sizeof(err));

	mu_assert_int_eq(1, output_failed);
	mu_assert_int_eq(1, ut.stop_ubertooth);
	mu_assert(strcmp(err, "error: unrecognized output mode (200)\n") == 0,
	          "one error: line, naming the mode");
}

int main(void)
{
	MU_RUN(a_failed_sample_write_ends_the_run);
	MU_RUN(samples_lost_in_a_buffer_end_the_run_too);
	MU_RUN(a_write_that_works_leaves_the_run_alone);
	MU_RUN(an_output_mode_that_is_not_one_of_ours_ends_the_run);
	return mu_report();
}
