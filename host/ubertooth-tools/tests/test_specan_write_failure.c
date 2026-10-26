/*
 * What ubertooth-specan does when it cannot write the samples it captured.
 *
 * The sample write is the one place a specan run can fail after the device is
 * open and the sweep is running: a redirect to a full disk, or `-d -` into a
 * reader that went away. Reporting that and then exiting 0 told a script the
 * capture it never received had succeeded, so the failure is recorded, the
 * receive loop stops, and the run exits 1.
 *
 * The other way a running sweep ends early is the board's USB stream failing,
 * which libubertooth records on the handle; the main() cases below cover that
 * too, next to the ordinary ends it must not be confused with.
 *
 * cb_specan() is static in ubertooth-specan.c, so that source is compiled in
 * here with main() renamed, as test_util_main.c does. The first half of this
 * file drives the callback directly, with a fifo it fills itself. The second
 * half runs the real main() against the scripted device below, so that the
 * exit status a failed write produces is checked where a user sees it and not
 * only in the flag the callback sets.
 *
 * No device is opened either way: the libubertooth device functions are the
 * fakes here, and only its fifo is the real one.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_specan_main
#include "ubertooth-specan.c"
#undef main

/* The one libubertooth global ubertooth-specan uses. It lives in
 * ubertooth.c, which is not linked here: its device functions are faked
 * below. */
FILE* dumpfile;

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

/* Standard output, redirected to a temp file for the runs that write samples
 * to it ("-d -"). Only its size is read back: the samples are raw bytes, and
 * what matters is that they reached the file rather than a stdio buffer the
 * run never flushed. */
static char out_path[] = "/tmp/specan-out-XXXXXX";
static int out_fd = -1;
static int out_saved = -1;

static void capture_stdout_begin(void)
{
	char template[] = "/tmp/specan-out-XXXXXX";

	memcpy(out_path, template, sizeof(template));
	out_fd = mkstemp(out_path);
	fflush(stdout);
	out_saved = dup(fileno(stdout));
	dup2(out_fd, fileno(stdout));
}

/* How many bytes the run wrote, with standard output put back. */
static long capture_stdout_end(void)
{
	struct stat info;
	long size;

	fflush(stdout);
	dup2(out_saved, fileno(stdout));
	close(out_saved);
	size = fstat(out_fd, &info) == 0 ? (long)info.st_size : -1;
	close(out_fd);
	unlink(out_path);
	return size;
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

/* A keep-alive is what a quiet sweep gets, and it carries no samples. Reading
 * frequencies and levels out of it would write whatever the slot held before,
 * so nothing is written for it and the sweep carries on. */
MU_TEST(a_keep_alive_writes_no_samples)
{
	ubertooth_t ut;
	usb_pkt_rx ka;
	uint8_t args[5];
	char path[] = "/tmp/specan-ka-XXXXXX";
	int fd;
	FILE* writable;
	char err[512];
	long size;

	fd = mkstemp(path);
	writable = fdopen(fd, "w");
	mu_assert_fatal(writable != NULL, "temp file opened");

	memset(&ut, 0, sizeof(ut));
	ut.fifo = fifo_init();
	/* Samples in every byte, so anything written out is recognisable as
	 * the block the keep-alive never replaced. */
	fill_block(&ka);
	ka.pkt_type = KEEP_ALIVE;
	fifo_push(ut.fifo, &ka);
	specan_args(args, SPECAN_FILE);

	output_failed = 0;
	write_failure_said = 0;
	dumpfile = writable;

	capture_stderr_begin();
	cb_specan(&ut, args);
	capture_stderr_end(err, sizeof(err));
	free(ut.fifo);

	fflush(writable);
	fseek(writable, 0, SEEK_END);
	size = ftell(writable);
	fclose(writable);
	unlink(path);

	mu_assert_int_eq(0, (int)size);
	mu_assert_int_eq(0, output_failed);
	mu_assert_int_eq(0, ut.stop_ubertooth);
	mu_assert(strcmp(err, "") == 0, "nothing said about a quiet sweep");
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

/* ---- a scripted device, so the real main() can be run ----------------- */

/* How the samples written during the sweep are made to fail. */
enum break_output {
	OUTPUT_WORKS,
	/* Unbuffered, so each 3-byte write fails as it is made: the full-disk
	 * case the sweep has to notice while it is running. */
	OUTPUT_FAILS_AT_THE_WRITE,
	/* Buffered, so the writes are accepted and only the flush after the
	 * sweep fails. */
	OUTPUT_FAILS_AT_THE_FLUSH
};

static struct {
	int start_calls, check_api_calls, cleanup_handlers, bulk_inits;
	int thread_starts, thread_stops, specan_calls, stops, device_stops;
	int blocks;            /* blocks handed to the callback */
	int blocks_to_sweep;   /* ...before the fake ends the endless sweep */
	int stream_dies_after; /* ...after which the stream fails, 0 for never */
	int stream_failed;     /* what ubertooth_stream_failed() reports */
	int interrupt_after;   /* blocks after which a Ctrl-C arrives */
	int start_fails;
	int check_api_rc, bulk_init_rc, specan_rc, stop_rc, thread_start_rc;
	int stop_before_bulk_init; /* STOP was sent before the transfer was set up */
	uint16_t low_freq, high_freq;
	char selector[64];
	enum break_output break_output;
	ubertooth_t ut;
} device;

static void reset_device(void)
{
	memset(&device, 0, sizeof(device));
	device.blocks_to_sweep = 1;
	device.ut.devh = (struct libusb_device_handle*)&device;
}

ubertooth_t* ubertooth_start_select(const char* u_arg)
{
	device.start_calls++;
	snprintf(device.selector, sizeof(device.selector), "%s",
	         u_arg ? u_arg : "");
	if (device.start_fails)
		return NULL;
	device.ut.fifo = fifo_init();
	return &device.ut;
}

int ubertooth_check_api(ubertooth_t* ut)
{
	(void)ut;
	device.check_api_calls++;
	return device.check_api_rc;
}

/* The one the library installs stops the capture on Ctrl-C, which is how
 * every sweep ends in practice, so the fake does the same: a case can then
 * raise a real signal and check what the tool makes of it. */
static void fake_cleanup(int sig)
{
	(void)sig;
	device.ut.stop_ubertooth = 1;
}

/* The real one folds the signal handler's own flag into the handle; the
 * fake handler below sets the handle directly, so reading it is enough here. */
int ubertooth_stopping(ubertooth_t* ut)
{
	return ut->stop_ubertooth;
}

void register_cleanup_handler(ubertooth_t* ut, int do_exit)
{
	(void)ut;
	(void)do_exit;
	device.cleanup_handlers++;
	signal(SIGINT, fake_cleanup);
}

int ubertooth_stream_failed(ubertooth_t* ut)
{
	(void)ut;
	return device.stream_failed;
}

int cmd_stop(struct libusb_device_handle* devh)
{
	(void)devh;
	device.stops++;
	if (device.bulk_inits == 0)
		device.stop_before_bulk_init = 1;
	return device.stop_rc;
}

/*
 * Called once the -d file is open and before any sample is written, which is
 * the only moment at which this test can get at that stream: main() opens it
 * itself. Standing its descriptor on a read-only one makes every write to it
 * fail the way a full disk does. Nothing has been written yet, so setvbuf()
 * is still allowed here.
 *
 * Only a stream of its own is broken: with "-d -" dumpfile is this process's
 * own standard output, and pointing that at /dev/null would break the test
 * run itself rather than the run under test.
 */
int ubertooth_bulk_init(ubertooth_t* ut)
{
	(void)ut;
	device.bulk_inits++;
	if (device.break_output != OUTPUT_WORKS && dumpfile != NULL &&
	    dumpfile != stdout) {
		int readonly = open("/dev/null", O_RDONLY);

		if (readonly >= 0) {
			if (device.break_output == OUTPUT_FAILS_AT_THE_WRITE)
				setvbuf(dumpfile, NULL, _IONBF, 0);
			dup2(readonly, fileno(dumpfile));
			close(readonly);
		}
	}
	return device.bulk_init_rc;
}

int ubertooth_bulk_thread_start(void)
{
	device.thread_starts++;
	return device.thread_start_rc;
}

void ubertooth_bulk_thread_stop(void) { device.thread_stops++; }

int cmd_specan(struct libusb_device_handle* devh, u16 low_freq, u16 high_freq)
{
	(void)devh;
	device.specan_calls++;
	device.low_freq = low_freq;
	device.high_freq = high_freq;
	return device.specan_rc;
}

/* One block of samples per call, as the board would send them. A sweep never
 * ends by itself, so the fake ends it after blocks_to_sweep blocks.
 *
 * It can also end it the two other ways a real sweep ends: a stream that
 * fails, which libubertooth reports and records on the handle before stopping
 * the capture, and a Ctrl-C, which only stops it. */
int ubertooth_bulk_receive(ubertooth_t* ut, rx_callback cb, void* cb_args)
{
	usb_pkt_rx rx;

	fill_block(&rx);
	fifo_push(ut->fifo, &rx);
	cb(ut, cb_args);
	device.blocks++;
	if (device.stream_dies_after && device.blocks >= device.stream_dies_after) {
		fprintf(stderr, "error: receive transfer failed: Device "
		        "disconnected. (5)\n");
		device.stream_failed = 1;
		ut->stop_ubertooth = 1;
		return 1;
	}
	if (device.interrupt_after && device.blocks >= device.interrupt_after)
		raise(SIGINT);
	if (device.blocks >= device.blocks_to_sweep)
		ut->stop_ubertooth = 1;
	return 0;
}

void ubertooth_stop(ubertooth_t* ut)
{
	(void)ut;
	device.device_stops++;
}

/* A fresh empty temp file for a -d value, with its name in path. mkstemp()
 * replaces the template in place, so each call needs the template written
 * back rather than the name the last one produced. */
static int make_temp_file(char* path, size_t size)
{
	int fd;

	snprintf(path, size, "/tmp/specan-main-XXXXXX");
	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	close(fd);
	return 0;
}

/* The real main(), with standard error captured. The -d file is left open by
 * main(), so it is closed here once the run is over. */
static int run_specan(const char* const* args, char* err, size_t err_size)
{
	char* argv[16];
	int argc = 0;
	int rc;

	while (args[argc] != NULL && argc < 15) {
		argv[argc] = (char*)args[argc];
		argc++;
	}
	argv[argc] = NULL;

	output_failed = 0;
	write_failure_said = 0;
	dumpfile = NULL;
	debug = 0;
	optind = 0; /* restart option parsing for each run */

	capture_stderr_begin();
	rc = ubertooth_specan_main(argc, argv);
	capture_stderr_end(err, err_size);

	if (dumpfile != NULL && dumpfile != stdout) {
		fclose(dumpfile);
		dumpfile = NULL;
	}
	free(device.ut.fifo);
	device.ut.fifo = NULL;
	return rc;
}

#define RUN_SPECAN(err, ...) \
	run_specan((const char* const[]){ "ubertooth-specan", __VA_ARGS__, NULL }, \
	           (err), sizeof(err))

MU_TEST(a_sweep_that_could_not_be_written_exits_one)
{
	char path[64];
	char err[512];
	struct stat info;

	/* Every write fails as it is made, so the sweep ends at the first block. */
	reset_device();
	device.break_output = OUTPUT_FAILS_AT_THE_WRITE;
	device.blocks_to_sweep = 10;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	mu_assert_int_eq(1, RUN_SPECAN(err, "-d", path));
	mu_assert(strstr(err, "error: writing samples failed: wrote 0 of 3 bytes")
	          != NULL, err);
	/* One block, not the ten the fake would have supplied: the failure ends
	 * the sweep rather than repeating for every block of it. */
	mu_assert_int_eq(1, device.blocks);
	mu_assert_int_eq(0, stat(path, &info));
	mu_assert_int_eq(0, (int)info.st_size);
	unlink(path);

	/* The writes are accepted into a buffer and only the flush after the
	 * sweep fails, which main() has to check too. */
	reset_device();
	device.break_output = OUTPUT_FAILS_AT_THE_FLUSH;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	mu_assert_int_eq(1, RUN_SPECAN(err, "-d", path));
	mu_assert(strstr(err, "error: writing samples failed: could not flush")
	          != NULL, err);
	mu_assert_int_eq(1, device.blocks);
	unlink(path);
}

MU_TEST(a_sweep_that_was_written_exits_zero)
{
	char path[64];
	char err[512];
	struct stat info;

	reset_device();
	device.blocks_to_sweep = 3;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	mu_assert_int_eq(0, RUN_SPECAN(err, "-U", "0", "-d", path));
	mu_assert(strcmp(err, "Ubertooth stopped\n") == 0, err);
	mu_assert_int_eq(3, device.blocks);
	/* Three bytes per sample, and the samples of three blocks arrived. */
	mu_assert_int_eq(0, stat(path, &info));
	mu_assert(info.st_size > 0, "the samples were written");
	mu_assert_int_eq(0, (int)(info.st_size % 3));
	unlink(path);

	/* The command line reached the device as given, the whole band by
	 * default, and the sweep was started on an idle board with the transfer
	 * already set up. */
	mu_assert(strcmp(device.selector, "0") == 0, device.selector);
	mu_assert_int_eq(BT_FREQ_MIN, device.low_freq);
	mu_assert_int_eq(BT_FREQ_MAX, device.high_freq);
	mu_assert_int_eq(1, device.stop_before_bulk_init);
	mu_assert_int_eq(1, device.specan_calls);
	mu_assert_int_eq(1, device.thread_starts);
	mu_assert_int_eq(1, device.thread_stops);
	mu_assert_int_eq(1, device.device_stops);

	/* A narrower range reaches the device too, and the filter in the
	 * callback drops the samples outside it: every frequency in the fake
	 * block is below 2470, so this run writes none of them and still
	 * succeeds. */
	reset_device();
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	mu_assert_int_eq(0, RUN_SPECAN(err, "-l", "2470", "-u", "2480",
	                               "-d", path));
	mu_assert_int_eq(2470, device.low_freq);
	mu_assert_int_eq(2480, device.high_freq);
	mu_assert_int_eq(0, stat(path, &info));
	mu_assert_int_eq(0, (int)info.st_size);
	unlink(path);
}

/*
 * A sweep whose USB stream fails part-way holds fewer samples than were
 * asked for. It used to exit 0: the error line went by, the receive loop kept
 * spinning with nothing arriving, and the run ended on its own timeout or a
 * Ctrl-C looking like a success.
 */
MU_TEST(a_sweep_whose_stream_died_exits_one)
{
	char path[64];
	char err[512];
	struct stat info;

	reset_device();
	device.stream_dies_after = 2;
	device.blocks_to_sweep = 50;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	mu_assert_int_eq(1, RUN_SPECAN(err, "-d", path));
	/* On standard error, where the library put it, not mixed into samples. */
	mu_assert(strstr(err, "error: receive transfer failed") != NULL, err);
	/* Two blocks, not the fifty the fake would have supplied: the capture
	 * stops where the stream died instead of waiting out the run. */
	mu_assert_int_eq(2, device.blocks);
	/* The samples that did arrive were still written out. */
	mu_assert_int_eq(0, stat(path, &info));
	mu_assert(info.st_size > 0, "the samples before the failure were written");
	unlink(path);
}

/* A Ctrl-C is a user stopping a sweep that was working: the receive loop
 * leaves at once, the run exits 0, and the samples it took are on disk rather
 * than left in a buffer. A sweep that cannot be stopped has to be killed,
 * which leaves the board in its mode for the next tool to clear. */
MU_TEST(an_interrupted_sweep_still_exits_zero)
{
	char path[64];
	char err[512];
	struct stat info;

	reset_device();
	device.interrupt_after = 2;
	device.blocks_to_sweep = 50;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	mu_assert_int_eq(0, RUN_SPECAN(err, "-d", path));
	mu_assert(strcmp(err, "Ubertooth stopped\n") == 0, err);
	/* Two blocks, not the fifty the fake would have supplied. */
	mu_assert_int_eq(2, device.blocks);
	/* The file is read after main() has returned and before the test closes
	 * the stream, so what is here is what main() flushed. */
	mu_assert_int_eq(0, stat(path, &info));
	mu_assert(info.st_size > 0, "the samples up to the interrupt were written");
	mu_assert_int_eq(0, (int)(info.st_size % 3));
	unlink(path);
	signal(SIGINT, SIG_DFL);
}

/* The same with "-d -", which writes the samples to standard output. It is
 * the same output mode and the same receive loop as a named file, so it has
 * to stop the same way; a run that reported stopping on one and not the other
 * would be the difference showing up here. */
MU_TEST(an_interrupted_sweep_to_stdout_still_exits_zero)
{
	char err[512];
	long written;

	reset_device();
	device.interrupt_after = 2;
	device.blocks_to_sweep = 50;

	/* The samples are raw bytes, so this process's standard output is sent
	 * to a temp file for the run: writing them to the test's own output
	 * would mix them into the CTest log. */
	capture_stdout_begin();
	mu_assert_int_eq(0, RUN_SPECAN(err, "-d", "-"));
	written = capture_stdout_end();

	mu_assert(strcmp(err, "Ubertooth stopped\n") == 0, err);
	mu_assert_int_eq(2, device.blocks);
	mu_assert(written > 0, "the samples up to the interrupt were written");
	mu_assert_int_eq(0, (int)(written % 3));
	signal(SIGINT, SIG_DFL);
}

MU_TEST(a_refused_command_line_opens_no_device)
{
	char err[512];

	/* An option the tool does not know: that one error: line on standard
	 * error, with no usage text after it, and nothing was opened. */
	reset_device();
	mu_assert_int_eq(1, RUN_SPECAN(err, "-Z"));
	mu_assert(strcmp(err, "error: unrecognized option '-Z'\n") == 0, err);
	mu_assert_int_eq(0, device.start_calls);

	/* An option whose value is missing reads the same way. */
	reset_device();
	mu_assert_int_eq(1, RUN_SPECAN(err, "-d"));
	mu_assert(strncmp(err, "error: option '-d' requires a value\n", 36) == 0,
	          err);
	mu_assert_int_eq(0, device.start_calls);
}

int main(void)
{
	MU_RUN(a_failed_sample_write_ends_the_run);
	MU_RUN(samples_lost_in_a_buffer_end_the_run_too);
	MU_RUN(a_write_that_works_leaves_the_run_alone);
	MU_RUN(a_keep_alive_writes_no_samples);
	MU_RUN(an_output_mode_that_is_not_one_of_ours_ends_the_run);
	MU_RUN(a_sweep_that_could_not_be_written_exits_one);
	MU_RUN(a_sweep_that_was_written_exits_zero);
	MU_RUN(a_sweep_whose_stream_died_exits_one);
	MU_RUN(an_interrupted_sweep_still_exits_zero);
	MU_RUN(an_interrupted_sweep_to_stdout_still_exits_zero);
	MU_RUN(a_refused_command_line_opens_no_device);
	return mu_report();
}
