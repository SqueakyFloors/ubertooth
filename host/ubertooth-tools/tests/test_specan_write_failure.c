/*
 * What ubertooth-specan does when it cannot write the samples it captured,
 * and what it reports about the sweep while it runs.
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
 * The last cases cover -v, which used to count a flag nothing read. Every
 * line it adds belongs on standard error, so the samples are untouched: the
 * bytes a "-d -" run writes to standard output have to be the same with it
 * and without it, which is checked by running both and comparing them.
 *
 * No device is opened either way: the libubertooth device functions are the
 * fakes here, and only its fifo is the real one. The clock is faked too
 * (util_clock.c is not linked), so the summary lines -v prints once a second
 * fall at times this file chooses rather than at whatever speed the machine
 * runs the fake sweep.
 */
#include <errno.h>
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

/* The library's stop route, in miniature, so a case can raise a real signal
 * and see what the tool makes of it. fake_cleanup() below sets this flag and
 * nothing else, and ubertooth_stopping() is the only place it reaches the
 * handle: a sweep whose loop read the handle directly would not notice the
 * interrupt here either. The handle's own flag still stops a sweep, which is
 * what the callback and a dead stream use. */
static volatile sig_atomic_t stop_requested;

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
 * to it ("-d -"). Its size is what most cases read back: the samples are raw
 * bytes, and what matters is that they reached the file rather than a stdio
 * buffer the run never flushed. The -v cases read the bytes as well, to
 * compare two runs. */
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

/* How many bytes of the run reached the file, with standard output put back.
 * Nothing is flushed before the size is read: what the run left in a stdio
 * buffer does not count, which is what makes this check main()'s own flush.
 * Anything still buffered is then pushed into the temp file, before standard
 * output goes back, so it cannot land in the test's own output. */
static long capture_stdout_end_bytes(char* out, size_t size)
{
	struct stat info;
	long written;

	written = fstat(out_fd, &info) == 0 ? (long)info.st_size : -1;
	if (out != NULL && written > 0) {
		long want = written < (long)size ? written : (long)size;

		if (pread(out_fd, out, (size_t)want, 0) != want)
			written = -1;
	}
	fflush(stdout);
	dup2(out_saved, fileno(stdout));
	close(out_saved);
	close(out_fd);
	unlink(out_path);
	return written;
}

static long capture_stdout_end(void)
{
	return capture_stdout_end_bytes(NULL, 0);
}

/* The monotonic clock ubertooth-specan times its -v summary lines by. The
 * fake sweep below moves it on by a fixed step per block, so a case decides
 * how many summary lines a run produces. */
static uint64_t fake_now_ms;

uint64_t util_clock_now_ms(void)
{
	return fake_now_ms;
}

/* How far the clock moves per block of samples. Two blocks is just over the
 * second between summary lines, so a run of a few blocks prints a few lines
 * rather than one per block or one per sample. */
#define FAKE_MS_PER_BLOCK 600u

/* The serial number the fake board answers with, which -v names. */
#define FAKE_SERIAL "1ff0ff1967102caf8334fa5ac52000f5"

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
	fifo_push(ut->fifo, &rx, 0);
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
	fifo_push(ut.fifo, &ka, 0);
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

/* The -V answer, which libubertooth gives in every tool: faked here, so this
 * test stays about the tool's own command-line handling. The answer's own
 * cases are in test_version_report.c. */
int ubertooth_print_versions(FILE *out, const char *selector)
{
	(void)selector;
	fprintf(out, "version (test)\n");
	return 0;
}

/* ---- a scripted device, so the real main() can be run ----------------- */

/* The report about a refused request asks whether the board has left the bus
 * before it blames the board's firmware. Nothing here unplugs a board, so it
 * is always still there. */
int ubertooth_device_gone(const void* board)
{
	(void)board;
	return 0;
}

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
	int device_closes;  /* ubertooth_close() calls */
	int device_frees;   /* ubertooth_free() calls */
	int blocks;            /* blocks handed to the callback */
	int blocks_to_sweep;   /* ...before the fake ends the endless sweep */
	int stream_dies_after; /* ...after which the stream fails, 0 for never */
	int stream_failed;     /* what ubertooth_stream_failed() reports */
	int interrupt_after;   /* blocks after which a Ctrl-C arrives */
	int start_fails;
	int serial_reads;      /* how often -v asked which board this is */
	int serial_read_fails; /* a board that would not say */
	int check_api_rc, bulk_init_rc, specan_rc, stop_rc, thread_start_rc;
	int stop_before_bulk_init; /* STOP was sent before the transfer was set up */
	uint16_t low_freq, high_freq;
	char selector[64];
	enum break_output break_output;
	ubertooth_t ut;
	/* 1 to have the teardown's STOP refused, so the board is left running
	 * the mode the capture started. */
	int stop_refused;
} device;

static void reset_device(void)
{
	memset(&device, 0, sizeof(device));
	stop_requested = 0;
	device.blocks_to_sweep = 1;
	device.ut.devh = (struct libusb_device_handle*)&device;
	fake_now_ms = 0;
}

int ubertooth_read_serial_string(struct libusb_device_handle* devh, char* out,
                                 size_t size, unsigned timeout_ms)
{
	(void)devh;
	(void)timeout_ms;
	device.serial_reads++;
	if (device.serial_read_fails)
		return -1;
	snprintf(out, size, "%s", FAKE_SERIAL);
	return 0;
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

static void fake_cleanup(int sig)
{
	(void)sig;
	stop_requested = 1;
}

int ubertooth_stopping(ubertooth_t* ut)
{
	if (stop_requested)
		ut->stop_ubertooth = 1;
	return ut->stop_ubertooth;
}

void register_cleanup_handler(ubertooth_t* ut, int do_exit)
{
	(void)ut;
	(void)do_exit;
	device.cleanup_handlers++;
	signal(SIGINT, fake_cleanup);
}


/* The end-of-capture gap report, which the tool calls and no case here reads:
 * a capture whose records never carried FIFO_OVERFLOW has nothing to say. */
void ubertooth_report_queue_gaps(ubertooth_t* ut)
{
	(void)ut;
}

/* The gap count the specan callback keeps, which belongs to libubertooth. */
void ubertooth_count_queue_gap(ubertooth_t* ut, const usb_pkt_rx* rx)
{
	(void)ut; (void)rx;
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
	fifo_push(ut->fifo, &rx, 0);
	cb(ut, cb_args);
	device.blocks++;
	if (device.stream_dies_after && device.blocks >= device.stream_dies_after) {
		fprintf(stderr, "error: receive transfer failed: Device "
		        "disconnected. (5)\n");
		device.stream_failed = 1;
		ut->stop_ubertooth = 1;
		return 1;
	}
	fake_now_ms += FAKE_MS_PER_BLOCK;
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

/* Whether the teardown's STOP was refused, which leaves the board running the
 * mode the capture started. Scripted, because the fake board above always
 * takes it. */
int ubertooth_stop_failed(ubertooth_t* ut)
{
	(void)ut;
	return device.stop_refused;
}

void ubertooth_close(ubertooth_t* ut)
{
	(void)ut;
	device.device_closes++;
}

void ubertooth_free(ubertooth_t* ut)
{
	(void)ut;
	device.device_frees++;
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
	verbose = 0;
	memset(&progress, 0, sizeof(progress));
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
	/* The handle and its 72 MB fifo go back, rather than being left to
	 * process exit. */
	mu_assert_int_eq(1, device.device_frees);
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
/* A sweep the board would not stop leaves that board sweeping, and no other
 * tool can open it until it is reset or re-plugged, so the run did not do what
 * was asked. The samples that arrived are still written out, and libubertooth
 * has already named the refused request, so nothing is added here. */
MU_TEST(a_sweep_whose_teardown_was_refused_exits_one)
{
	char path[64];
	char err[512];
	struct stat info;

	reset_device();
	device.blocks_to_sweep = 3;
	device.stop_refused = 1;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	mu_assert_int_eq(1, RUN_SPECAN(err, "-d", path));
	mu_assert(strcmp(err, "Ubertooth stopped\n") == 0, err);
	mu_assert_int_eq(3, device.blocks);
	mu_assert_int_eq(0, stat(path, &info));
	mu_assert(info.st_size > 0, "the samples that arrived were written");
	unlink(path);

	/* The same sweep on a board that takes the stop. */
	reset_device();
	device.blocks_to_sweep = 3;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");
	mu_assert_int_eq(0, RUN_SPECAN(err, "-d", path));
	unlink(path);
}

/* A run refused for its firmware creates no -d file: an empty capture left
 * behind is one a user then has to tell apart from a sweep that caught
 * nothing. The board is given back, and the reason is libubertooth's. */
MU_TEST(a_run_refused_for_its_firmware_creates_no_file)
{
	char path[64];
	char err[512];
	struct stat info;

	reset_device();
	device.check_api_rc = -1;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");
	/* The name of a file that is not there: make_temp_file() created one,
	 * so it is removed before the run that must not re-create it. */
	unlink(path);

	mu_assert_int_eq(1, RUN_SPECAN(err, "-d", path));
	mu_assert_int_eq(1, device.check_api_calls);
	mu_assert(stat(path, &info) != 0, "no file was created");
	mu_assert_int_eq(1, device.device_closes);
	mu_assert_int_eq(1, device.device_frees);
	/* No mode was started, so nothing was swept and nothing stopped. */
	mu_assert_int_eq(0, device.specan_calls);
}

/* A -d file that cannot be opened is reported like every other failure: one
 * "error: " line on standard error, nothing on standard output, exit 1. It
 * used to be perror's unprefixed "<file>: <reason>". Checked here rather than
 * from the command line, because the file is opened only once the board has
 * been accepted. */
MU_TEST(a_dump_file_that_cannot_be_opened_is_named)
{
	char err[512];

	reset_device();
	mu_assert_int_eq(1, RUN_SPECAN(err, "-d", "/no/such/dir/samples.bin"));
	mu_assert(strcmp(err, "error: /no/such/dir/samples.bin: "
	                 "No such file or directory\n") == 0, err);
	mu_assert_int_eq(1, device.device_closes);
	mu_assert_int_eq(1, device.device_frees);
	mu_assert_int_eq(0, device.specan_calls);
}

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

/*
 * The thread that collects a sweep could not be started. pthread_create()
 * reports a failure as a positive errno, and a "< 0" test let that through:
 * the sweep was then started with nobody to complete its transfers and waited
 * forever with no samples arriving, and no line said why.
 */
MU_TEST(a_sweep_whose_collecting_thread_failed_exits_one)
{
	char path[64];
	char err[512];
	char expected[256];
	struct stat info;
	long written;

	reset_device();
	device.thread_start_rc = EAGAIN;
	device.blocks_to_sweep = 50;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	/* Captured only to prove the error went nowhere near the samples'
	 * stream; this run writes them to the -d file. */
	capture_stdout_begin();
	mu_assert_int_eq(1, RUN_SPECAN(err, "-d", path));
	written = capture_stdout_end();

	snprintf(expected, sizeof(expected),
	         "error: could not start the thread that collects the capture:"
	         " %s\n", strerror(EAGAIN));
	mu_assert(strcmp(err, expected) == 0, err);
	mu_assert_int_eq(0, (int)written);
	/* Nothing was started on top of the failure: the board was never asked
	 * to sweep, so no block arrived and the run ended instead of waiting
	 * out the fifty the fake would have supplied. The board is given back. */
	mu_assert_int_eq(1, device.thread_starts);
	mu_assert_int_eq(0, device.specan_calls);
	mu_assert_int_eq(0, device.blocks);
	mu_assert_int_eq(1, device.device_stops);
	/* Not joined: there is no thread to join. */
	mu_assert_int_eq(0, device.thread_stops);
	mu_assert_int_eq(0, stat(path, &info));
	mu_assert_int_eq(0, (int)info.st_size);
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
	int status;

	reset_device();
	device.interrupt_after = 2;
	device.blocks_to_sweep = 50;

	/* The samples are raw bytes, so this process's standard output is sent
	 * to a temp file for the run: writing them to the test's own output
	 * would mix them into the CTest log. */
	capture_stdout_begin();
	status = RUN_SPECAN(err, "-d", "-");
	written = capture_stdout_end();

	/* Nothing is asserted while standard output is the temp file: minunit
	 * prints a failure there, and the file is gone by the time CTest looks. */
	mu_assert_int_eq(0, status);
	mu_assert(strcmp(err, "Ubertooth stopped\n") == 0, err);
	mu_assert_int_eq(2, device.blocks);
	mu_assert(written > 0, "the samples up to the interrupt were written");
	mu_assert_int_eq(0, (int)(written % 3));
	signal(SIGINT, SIG_DFL);
}

/* ---- what -v reports --------------------------------------------------- */

static int count_lines(const char* text)
{
	int lines = 0;

	for (; *text != '\0'; text++)
		if (*text == '\n')
			lines++;
	return lines;
}

/*
 * -v says which board, what is being swept, where the samples go, and how the
 * sweep is getting on. The range here is 2402-2410 MHz, and every fake block
 * holds 2402-2417, so each block contributes nine samples in range, seven
 * dropped as left over, and one pass over the range: the three counters are
 * pinned to numbers a reader can check by hand.
 */
MU_TEST(verbose_reports_the_sweep_on_stderr)
{
	char path[64];
	char err[1024];
	char expected[256];
	long written;

	reset_device();
	device.blocks_to_sweep = 2;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	/* Standard output is captured to prove nothing of -v reached it: the
	 * samples of this run go to the -d file, so it must stay empty. */
	capture_stdout_begin();
	mu_assert_int_eq(0, RUN_SPECAN(err, "-v", "-l", "2402", "-u", "2410",
	                               "-d", path));
	written = capture_stdout_end();
	mu_assert_int_eq(0, (int)written);

	mu_assert(strstr(err, "Using Ubertooth " FAKE_SERIAL "\n") != NULL, err);
	/* -U may name an index, so which board it is comes from the board. */
	mu_assert_int_eq(1, device.serial_reads);
	mu_assert(strstr(err, "Sweeping 2402-2410 MHz in 1 MHz steps, "
	                 "9 samples per sweep\n") != NULL, err);
	snprintf(expected, sizeof(expected), "Writing raw samples to %s\n", path);
	mu_assert(strstr(err, expected) != NULL, err);
	/* The clock moves 600 ms per block, so the second block is the first
	 * one a second after the start: one summary line, then the total. */
	mu_assert(strstr(err, "After 1.2 s: 2 sweeps, 18 samples, "
	                 "14 out of range\n") != NULL, err);
	mu_assert(strstr(err, "Total after 1.2 s: 2 sweeps, 18 samples, "
	                 "14 out of range\n") != NULL, err);
	mu_assert(strstr(err, "Ubertooth stopped\n") != NULL, err);
	/* Six lines for a two-block sweep of 32 samples: a line per sample, or
	 * per block, would bury a terminal in a run of a few seconds. */
	mu_assert_int_eq(6, count_lines(err));
	unlink(path);

	/* A board that will not say which it is leaves the rest of the run
	 * alone: the line says so instead of naming a serial number it never
	 * read. */
	reset_device();
	device.serial_read_fails = 1;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	mu_assert_int_eq(0, RUN_SPECAN(err, "-v", "-d", path));
	mu_assert(strstr(err, "Using an Ubertooth whose serial number could not "
	                 "be read\n") != NULL, err);
	unlink(path);
}

/* Without -v the run says only what it always said. */
MU_TEST(a_run_without_verbose_reports_nothing)
{
	char path[64];
	char err[512];

	reset_device();
	device.blocks_to_sweep = 3;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	mu_assert_int_eq(0, RUN_SPECAN(err, "-d", path));
	mu_assert(strcmp(err, "Ubertooth stopped\n") == 0, err);
	/* Not even the serial read -v makes: a run that was not asked for it
	 * sends the board nothing extra. */
	mu_assert_int_eq(0, device.serial_reads);
	unlink(path);
}

/*
 * The whole point of putting every -v line on standard error: with "-d -" the
 * samples are standard output, and they have to come out the same either way.
 * The two runs are identical but for the option, so their bytes are compared
 * directly.
 */
MU_TEST(verbose_leaves_the_samples_on_stdout_alone)
{
	char quiet_err[512], verbose_err[1024];
	char quiet[4096], loud[4096];
	long quiet_len, loud_len;

	reset_device();
	device.blocks_to_sweep = 3;
	capture_stdout_begin();
	mu_assert_int_eq(0, RUN_SPECAN(quiet_err, "-d", "-"));
	quiet_len = capture_stdout_end_bytes(quiet, sizeof(quiet));

	reset_device();
	device.blocks_to_sweep = 3;
	capture_stdout_begin();
	mu_assert_int_eq(0, RUN_SPECAN(verbose_err, "-d", "-", "-v"));
	loud_len = capture_stdout_end_bytes(loud, sizeof(loud));

	mu_assert(quiet_len > 0, "the quiet run wrote samples");
	/* The buffers hold the whole run, so the compare below reads only what
	 * was captured. Three blocks are 144 bytes; raise the buffers if a case
	 * ever sweeps for longer. */
	mu_assert(quiet_len <= (long)sizeof(quiet),
	          "the run fit in the capture buffer");
	mu_assert_int_eq((int)quiet_len, (int)loud_len);
	mu_assert(memcmp(quiet, loud, (size_t)quiet_len) == 0,
	          "-v changed the samples on standard output");

	/* The lines went somewhere, and that somewhere is standard error. */
	mu_assert(strstr(verbose_err, "Writing raw samples to standard output\n")
	          != NULL, verbose_err);
	mu_assert(strstr(quiet_err, "Writing") == NULL, quiet_err);
}

/* -v names the output the options chose, not the one -d would have given. */
MU_TEST(verbose_names_the_output_the_options_chose)
{
	char err[1024];
	char path[64];

	reset_device();
	capture_stdout_begin();
	mu_assert_int_eq(0, RUN_SPECAN(err, "-v"));
	capture_stdout_end();
	mu_assert(strstr(err, "Writing text to standard output\n") != NULL, err);

	reset_device();
	capture_stdout_begin();
	mu_assert_int_eq(0, RUN_SPECAN(err, "-v", "-g"));
	capture_stdout_end();
	mu_assert(strstr(err, "Writing feedgnuplot text to standard output\n")
	          != NULL, err);

	reset_device();
	capture_stdout_begin();
	mu_assert_int_eq(0, RUN_SPECAN(err, "-v", "-G"));
	capture_stdout_end();
	mu_assert(strstr(err, "Writing 3D feedgnuplot text to standard output\n")
	          != NULL, err);

	/* The case report_output_target() exists for: -d chose the file and -g
	 * the format, so -v names both, and the samples really do go there. */
	mu_assert_int_eq(0, make_temp_file(path, sizeof(path)));
	reset_device();
	capture_stdout_begin();
	mu_assert_int_eq(0, RUN_SPECAN(err, "-v", "-d", path, "-g"));
	capture_stdout_end();
	mu_assert(strstr(err, "Writing feedgnuplot text to ") != NULL, err);
	mu_assert(strstr(err, path) != NULL, err);
	mu_assert(strstr(err, "to standard output") == NULL, err);
	unlink(path);
}

/* -d chooses where the samples go and -g how they are written, so the text
 * lands in the file rather than on the screen. The file used to be created
 * and left empty while -g took the samples to standard output. */
MU_TEST(a_text_format_writes_into_the_file_d_named)
{
	char path[64];
	char err[512];
	char first[256] = {0};
	struct stat info;
	FILE *written;

	reset_device();
	device.blocks_to_sweep = 3;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	capture_stdout_begin();
	mu_assert_int_eq(0, RUN_SPECAN(err, "-d", path, "-g"));
	capture_stdout_end();

	/* The file holds the samples, and as text: a gnuplot line is a
	 * frequency and a level, so the first character is a digit rather
	 * than one of the raw record's bytes. */
	mu_assert_int_eq(0, stat(path, &info));
	mu_assert(info.st_size > 0, "the samples were written to the file");
	written = fopen(path, "r");
	mu_assert_fatal(written != NULL, "the file opens for reading");
	mu_assert(fgets(first, sizeof(first), written) != NULL, "a first line");
	fclose(written);
	mu_assert(first[0] >= '0' && first[0] <= '9', first);
	mu_assert(strchr(first, ' ') != NULL, first);
	mu_assert(strchr(first, '\n') != NULL, first);
	unlink(path);
}

/*
 * The text formats check every write too, now that -d can send them to a
 * file. Each one used to fprintf() without looking: a full disk was noticed
 * only at the closing flush, so the whole sweep ran and the report was the
 * vaguer "could not flush the samples".
 */
MU_TEST(a_failed_text_write_ends_the_run)
{
	static const uint8_t modes[] = {
		SPECAN_STDOUT, SPECAN_GNUPLOT_NORMAL, SPECAN_GNUPLOT_3D
	};
	ubertooth_t ut;
	FILE* unwritable;
	char err[512];
	unsigned m;

	for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
		/* Unbuffered, so the write goes straight to a descriptor that is
		 * gone and fails as it is made. */
		unwritable = broken_stream(0);
		mu_assert_fatal(unwritable != NULL, "the unbuffered stream opened");

		one_block(&ut, unwritable, modes[m], err, sizeof(err));
		fclose(unwritable);

		mu_assert_int_eq(1, output_failed);
		mu_assert_int_eq(1, ut.stop_ubertooth);
		mu_assert(strstr(err, "error: writing samples failed: could not "
		                 "write the sample (") == err, err);
		mu_assert(strchr(err, '\n') == strrchr(err, '\n'), "said once");
	}
}

/* And the run a user sees: a text format into a -d file that cannot be
 * written ends at the first block and exits 1, rather than sweeping to the
 * end and reporting only the flush. */
MU_TEST(a_text_sweep_that_could_not_be_written_exits_one)
{
	char path[64];
	char err[512];

	reset_device();
	device.break_output = OUTPUT_FAILS_AT_THE_WRITE;
	device.blocks_to_sweep = 10;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	mu_assert_int_eq(1, RUN_SPECAN(err, "-d", path, "-g"));
	mu_assert(strstr(err, "error: writing samples failed: could not write "
	                 "the sample (") != NULL, err);
	/* One block, not the ten the fake would have supplied. */
	mu_assert_int_eq(1, device.blocks);
	unlink(path);
}

/* The two gnuplot formats end a pass over the range with a blank line, which
 * is written and checked with the sample before it. Every other case here
 * sweeps the whole band, where the fake block never reaches the upper
 * frequency, so this is the one that reaches that branch. */
MU_TEST(a_gnuplot_sweep_ends_each_pass_with_a_blank_line)
{
	char path[64];
	char err[512];
	char text[4096] = {0};
	FILE *written;
	size_t n;

	reset_device();
	device.blocks_to_sweep = 2;
	mu_assert_fatal(make_temp_file(path, sizeof(path)) == 0,
	                "temp file created");

	/* 2402-2410 MHz: the fake block holds 2402-2417, so every block
	 * completes a pass over that range and ends a record. */
	mu_assert_int_eq(0, RUN_SPECAN(err, "-g", "-l", "2402", "-u", "2410",
	                               "-d", path));
	written = fopen(path, "r");
	mu_assert_fatal(written != NULL, "the file opens for reading");
	n = fread(text, 1, sizeof(text) - 1, written);
	text[n] = '\0';
	fclose(written);
	unlink(path);

	/* The last sample of a pass is the upper frequency, and a blank line
	 * follows it. */
	mu_assert(strstr(text, "2410 -70\n\n") != NULL, text);
	/* Two passes arrived, so there are two of them. */
	mu_assert(strstr(strstr(text, "2410 -70\n\n") + 1, "2410 -70\n\n")
	          != NULL, text);
}

/* There is one level of verbosity, so a second -v is a mistyped command line
 * rather than a request for more: that one error: line, and nothing opened. */
MU_TEST(a_second_verbose_is_refused)
{
	char err[512];

	reset_device();
	mu_assert_int_eq(1, RUN_SPECAN(err, "-v", "-v"));
	mu_assert(strcmp(err, "error: -v may be given only once\n") == 0, err);
	mu_assert_int_eq(0, device.start_calls);
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
	MU_RUN(a_text_format_writes_into_the_file_d_named);
	MU_RUN(a_failed_text_write_ends_the_run);
	MU_RUN(a_text_sweep_that_could_not_be_written_exits_one);
	MU_RUN(a_gnuplot_sweep_ends_each_pass_with_a_blank_line);
	MU_RUN(a_sweep_whose_teardown_was_refused_exits_one);
	MU_RUN(a_run_refused_for_its_firmware_creates_no_file);
	MU_RUN(a_dump_file_that_cannot_be_opened_is_named);
	MU_RUN(a_sweep_whose_stream_died_exits_one);
	MU_RUN(a_sweep_whose_collecting_thread_failed_exits_one);
	MU_RUN(an_interrupted_sweep_still_exits_zero);
	MU_RUN(an_interrupted_sweep_to_stdout_still_exits_zero);
	MU_RUN(verbose_reports_the_sweep_on_stderr);
	MU_RUN(a_run_without_verbose_reports_nothing);
	MU_RUN(verbose_leaves_the_samples_on_stdout_alone);
	MU_RUN(verbose_names_the_output_the_options_chose);
	MU_RUN(a_second_verbose_is_refused);
	MU_RUN(a_refused_command_line_opens_no_device);
	return mu_report();
}
