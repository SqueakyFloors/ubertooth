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

/* One capture, with dumpfile set to the stream given, and whatever the
 * callback wrote to standard error returned in err. */
static void one_block(ubertooth_t* ut, FILE* out, uint8_t mode,
                      char* err, size_t err_size)
{
	usb_pkt_rx rx;
	uint8_t args[5];
	char path[] = "/tmp/specan-err-XXXXXX";
	int fd, saved;
	FILE* captured;
	long n;

	memset(ut, 0, sizeof(*ut));
	ut->fifo = fifo_init();
	fill_block(&rx);
	fifo_push(ut->fifo, &rx);
	specan_args(args, mode);

	output_failed = 0;
	dumpfile = out;

	fd = mkstemp(path);
	fflush(stderr);
	saved = dup(fileno(stderr));
	dup2(fd, fileno(stderr));
	cb_specan(ut, args);
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);

	captured = fopen(path, "r");
	n = (long)fread(err, 1, err_size - 1, captured);
	err[n] = '\0';
	fclose(captured);
	close(fd);
	unlink(path);
	free(ut->fifo);
}

MU_TEST(a_failed_sample_write_ends_the_run)
{
	ubertooth_t ut;
	FILE* unwritable;
	char err[512];

	/* A stream whose file descriptor is open for reading only, and
	 * unbuffered so the write reaches it rather than sitting in stdio. */
	unwritable = fopen("/dev/null", "r");
	mu_assert(unwritable != NULL, "/dev/null opened");
	setvbuf(unwritable, NULL, _IONBF, 0);

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
	MU_RUN(a_write_that_works_leaves_the_run_alone);
	MU_RUN(an_output_mode_that_is_not_one_of_ours_ends_the_run);
	return mu_report();
}
