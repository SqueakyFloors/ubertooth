/*
 * How a signal stops a capture: Ctrl-C, SIGTERM, SIGHUP, SIGQUIT and the -t
 * alarm all have to end the run, and every loop that waits for packets has to
 * notice.
 *
 * A signal handler may touch very little. Of the objects that outlive it,
 * C17 7.14.1.1p5 lets it assign to one declared volatile sig_atomic_t and
 * read nothing else, and these handlers used to find the capture handle
 * through a pointer variable and store a byte in it. The handler now sets a
 * flag of its own, and ubertooth_stopping() is what folds that flag into the
 * handle, on the run's own thread. The cases below pin the result a tool
 * depends on -- the run stops -- rather than how the flag gets there.
 *
 * A replay of a saved capture is here too: it is the one receive loop that
 * reads a file rather than a board, and it used to run to the end of the file
 * whatever the user asked.
 *
 * The real ubertooth.c, ubertooth_callback.c and ubertooth_control.c are
 * compiled in. No device is opened and no transfer is made: the ring buffer
 * is faked (a real one is 72 MB, and nothing here receives a real packet), so
 * the signal and the stop are all that is under test.
 *
 * A loop that misses the stop does not fail, it waits forever, so this test
 * has a CTest timeout and the waits below are the cases: a regression hangs
 * and the timeout fails it.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth.h"
#include "ubertooth_fifo.h"

/* ---- fifo fake ---------------------------------------------------------- */
/* ubertooth_fifo.c is not linked. Empty unless a case queues a block, which
 * is what a quiet board looks like: the waits below then have nothing but the
 * stop to end them. */

static int blocks_queued;
static usb_pkt_rx one_slot;

fifo_t* fifo_init(void) { return (fifo_t*)malloc(1); }
uint8_t fifo_empty(fifo_t* fifo) { (void)fifo; return blocks_queued == 0; }
usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return &one_slot; }
void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns)
{ (void)fifo; (void)arrival_ns; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns)
{ (void)fifo; (void)packet; (void)arrival_ns; }
uint64_t fifo_popped_arrival_ns(const fifo_t* fifo) { (void)fifo; return 0; }
usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	usb_pkt_rx p;

	(void)fifo;
	if (blocks_queued > 0)
		blocks_queued--;
	memset(&p, 0, sizeof(p));
	return p;
}

/* ---- helpers ------------------------------------------------------------ */

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* A handle that never opened a device, with the library's stop handlers
 * installed. They are the process's, not the handle's: the handle is what the
 * stop is read through, with ubertooth_stopping(). */
static ubertooth_t* new_handle(void)
{
	ubertooth_t* ut = ubertooth_init();

	if (ut == NULL)
		return NULL;
	ubertooth_catch_stop_signals();
	return ut;
}

/* Back to the signal dispositions the test process started with, and to a
 * handle nothing has asked to stop, so one case cannot leave the next one
 * already stopping. */
static void release_handle(ubertooth_t* ut)
{
	alarm(0);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGALRM, SIG_DFL);
	blocks_queued = 0;
	ubertooth_reset_stop_signals();
	if (ut == NULL)
		return;
	ubertooth_free(ut);
}

/* Counts the blocks a capture handed over. */
static void count_blocks(ubertooth_t* ut, void* args)
{
	fifo_pop(ut->fifo);
	(*(int*)args)++;
}

/* ---- the cases ---------------------------------------------------------- */

/* Every signal the library catches stops the run. SIGALRM is in the list
 * because that is how a -t timeout ends a capture. */
MU_TEST(every_caught_signal_stops_the_run)
{
	static const int signals[] = { SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGALRM };
	size_t i;

	for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		ubertooth_t* ut = new_handle();

		mu_assert_fatal(ut != NULL, "a handle");
		mu_assert_int_eq(0, ubertooth_stopping(ut));

		raise(signals[i]);

		mu_assert_int_eq(1, ubertooth_stopping(ut));
		/* And through the handle every receive loop reads, so a loop that
		 * has already read it once keeps seeing it. */
		mu_assert_int_eq(1, ut->stop_ubertooth);

		release_handle(ut);
	}
}

/* The wait for a packet is the loop a capture spends its time in, and on a
 * quiet board nothing but the stop ends it. It must return once the signal
 * has arrived; a loop that misses the stop waits forever and this test's
 * timeout fails it. */
MU_TEST(the_wait_for_a_packet_ends_on_a_signal)
{
	ubertooth_t* ut = new_handle();

	mu_assert_fatal(ut != NULL, "a handle");

	raise(SIGINT);
	ubertooth_bulk_wait(ut);

	mu_assert_int_eq(1, ubertooth_stopping(ut));
	release_handle(ut);
}

/* A board that is sending is stopped too, and the block in hand is still
 * handed over: a user who interrupts a capture keeps what it had captured.
 * The 1 the library returns is how a tool's loop is told the run is over. */
MU_TEST(a_sending_board_is_stopped_and_its_block_kept)
{
	ubertooth_t* ut = new_handle();
	int blocks = 0;

	mu_assert_fatal(ut != NULL, "a handle");

	/* Still capturing: the block is handed over and the capture carries on. */
	blocks_queued = 1;
	mu_assert_int_eq(0, ubertooth_bulk_receive(ut, count_blocks, &blocks));
	mu_assert_int_eq(1, blocks);

	raise(SIGINT);

	blocks_queued = 1;
	mu_assert_int_eq(1, ubertooth_bulk_receive(ut, count_blocks, &blocks));
	mu_assert_int_eq(2, blocks);
	mu_assert_int_eq(1, ubertooth_stopping(ut));

	release_handle(ut);
}

/* Puts this process where a tool is while it is still opening a board: the
 * handlers are installed, no handle exists yet, and SIGINT was inherited
 * ignored -- which is what a tool started as a background job by a script
 * inherits. A signal arriving before the handlers went in was discarded
 * outright, so the user's first Ctrl-C did nothing and a second was needed.
 * Opening a board is long enough for one to land there: selecting by serial
 * number reads the serial of every attached board, and one that does not
 * answer costs a second. */
static void interrupt_before_the_board_is_open(void)
{
	signal(SIGINT, SIG_IGN);
	ubertooth_catch_stop_signals();
	raise(SIGINT);
}

/* A board that sends nothing: the wait is the whole capture, and it returns on
 * a signal that arrived before the handle existed. */
MU_TEST(a_signal_from_before_the_board_was_open_ends_a_quiet_capture)
{
	ubertooth_t* ut;

	interrupt_before_the_board_is_open();
	mu_assert_int_eq(1, ubertooth_stop_requested());

	ut = ubertooth_init();
	mu_assert_fatal(ut != NULL, "a handle");
	ubertooth_bulk_wait(ut);

	mu_assert_int_eq(1, ubertooth_stopping(ut));
	release_handle(ut);
}

/* And a board that is in the middle of sending: the block in hand is handed
 * over and the loop is told the run is over, on that same signal. */
MU_TEST(a_signal_from_before_the_board_was_open_ends_a_sending_capture)
{
	ubertooth_t* ut;
	int blocks = 0;

	interrupt_before_the_board_is_open();

	ut = ubertooth_init();
	mu_assert_fatal(ut != NULL, "a handle");
	blocks_queued = 1;
	mu_assert_int_eq(1, ubertooth_bulk_receive(ut, count_blocks, &blocks));
	mu_assert_int_eq(1, blocks);

	release_handle(ut);
	/* And nothing is left asking the next run to stop. */
	mu_assert_int_eq(0, ubertooth_stop_requested());
}

/* A handle that runs two captures in turn (ubertooth-afh detects a map, then
 * monitors it): the -t timer that ends the first phase must not end the second
 * before it starts, so that stop is forgotten between them. */
MU_TEST(a_cleared_timeout_lets_a_second_capture_run)
{
	ubertooth_t* ut = new_handle();

	mu_assert_fatal(ut != NULL, "a handle");

	/* The -t handler, which a tool installs over the one above, and which
	 * nothing else here reaches: the alarm itself is never waited for, so
	 * the long timer only has to be longer than the test. */
	ubertooth_set_timeout(ut, 300);
	raise(SIGALRM);
	mu_assert_int_eq(1, ubertooth_stopping(ut));

	ubertooth_clear_timeout_stop(ut);

	mu_assert_int_eq(0, ubertooth_stopping(ut));
	/* And the second phase still stops on its own Ctrl-C. */
	raise(SIGINT);
	mu_assert_int_eq(1, ubertooth_stopping(ut));

	release_handle(ut);
}

/* The other half of that: a stop the user asked for is not forgotten with the
 * timer. Clearing between the phases of a -t run used to drop it, so a Ctrl-C
 * made during detection was swallowed and the user had to interrupt the
 * monitoring that started anyway. */
MU_TEST(a_cleared_timeout_keeps_an_interrupt)
{
	ubertooth_t* ut = new_handle();

	mu_assert_fatal(ut != NULL, "a handle");

	ubertooth_set_timeout(ut, 300);
	/* Both arrive during the first phase: the user interrupts, and the
	 * timer the tool set goes off as well. */
	raise(SIGINT);
	raise(SIGALRM);
	mu_assert_int_eq(1, ubertooth_stopping(ut));

	ubertooth_clear_timeout_stop(ut);

	mu_assert_int_eq(1, ubertooth_stopping(ut));

	release_handle(ut);
}

/* ---- replays ------------------------------------------------------------ */

/* A capture file of n empty records, as ubertooth-dump -f writes them, open
 * for reading at its first record. The name is left in path so the case can
 * remove it. */
static FILE* capture_file(char* path, size_t path_len, int records)
{
	static const char template[] = "/tmp/replay-stop-XXXXXX";
	uint8_t record[RX_FILE_RECORD_LEN];
	FILE* fp;
	int fd, i;

	if (path_len < sizeof(template))
		return NULL;
	memcpy(path, template, sizeof(template));
	fd = mkstemp(path);
	if (fd < 0)
		return NULL;
	fp = fdopen(fd, "w+");
	if (fp == NULL) {
		close(fd);
		return NULL;
	}
	memset(record, 0, sizeof(record));
	for (i = 0; i < records; i++)
		fwrite(record, 1, sizeof(record), fp);
	rewind(fp);
	return fp;
}

/* Counts the records a replay handed over. */
static void count_records(ubertooth_t* ut, void* args)
{
	(void)ut;
	(*(int*)args)++;
}

/* The same, interrupting the replay at its first record. */
static void interrupt_at_the_first_record(ubertooth_t* ut, void* args)
{
	(void)ut;
	if (++(*(int*)args) == 1)
		raise(SIGINT);
}

/* A replay that nothing interrupts reads the whole file, which is what makes
 * the case below mean something. */
MU_TEST(a_replay_reads_the_whole_file)
{
	ubertooth_t* ut = new_handle();
	char path[64];
	FILE* fp = capture_file(path, sizeof(path), 5);
	int records = 0;

	mu_assert_fatal(ut != NULL, "a handle");
	mu_assert_fatal(fp != NULL, "a capture file");

	mu_assert_int_eq(0, stream_rx_file(ut, fp, count_records, &records));
	mu_assert_int_eq(5, records);

	fclose(fp);
	unlink(path);
	release_handle(ut);
}

/* And one the user interrupts stops there. It used to read on to the end of
 * the file whatever the user asked, because the loop had no stop check at
 * all, and a capture file can hold hours of packets. Stopping is a clean end
 * -- it is what was asked for -- so the result is the 0 of an end of file and
 * the tool reports no error. */
MU_TEST(a_replay_stops_when_the_user_asks)
{
	ubertooth_t* ut = new_handle();
	char path[64];
	FILE* fp = capture_file(path, sizeof(path), 5);
	int records = 0;

	mu_assert_fatal(ut != NULL, "a handle");
	mu_assert_fatal(fp != NULL, "a capture file");

	mu_assert_int_eq(0, stream_rx_file(ut, fp, interrupt_at_the_first_record,
	                                   &records));
	mu_assert_int_eq(1, records);

	fclose(fp);
	unlink(path);
	release_handle(ut);
}

/* Nothing but a signal, a callback or the poll thread stops a run: a handle
 * that has had none of those keeps capturing. Without this the cases above
 * would pass on a library that stopped every run at once. */
MU_TEST(a_run_nothing_interrupted_keeps_going)
{
	ubertooth_t* ut = new_handle();
	int i;

	mu_assert_fatal(ut != NULL, "a handle");

	for (i = 0; i < 100; i++)
		mu_assert_int_eq(0, ubertooth_stopping(ut));

	/* The other way a capture ends early, which does go through the
	 * handle: a callback that cannot go on sets it. */
	ut->stop_ubertooth = 1;
	mu_assert_int_eq(1, ubertooth_stopping(ut));

	release_handle(ut);
}

/* A tool may give its handle back while the library's handlers are still
 * installed, which is what every tool does on its way out now that it frees
 * the handle. This pins the part of that a test can see: the handlers survive
 * it, so a run started afterwards still stops on a signal.
 *
 * What it cannot see is the reason ubertooth_free() lets go of the handle the
 * library was holding. The handler these tools install only sets a flag; the
 * one that would dereference the freed handle is cleanup_exit(), which
 * ubertooth.c installs for the deprecated register_cleanup_handler(ut, 1) and
 * which no tool asks for. Nor is a handle registered here any more, now that
 * the tools install the handlers before they have one. So this case passes
 * either way, and the read-after-free it guards against would need a caller
 * that passed 1 and a sanitizer to catch. */
MU_TEST(a_freed_handle_is_let_go_of)
{
	ubertooth_t* ut = new_handle();
	int i;

	mu_assert_fatal(ut != NULL, "a handle");
	/* Freed with the handlers still installed, as a tool leaves it. */
	ubertooth_free(ut);

	for (i = 0; i < 5; i++)
		raise(SIGINT);
	raise(SIGTERM);
	raise(SIGALRM);

	/* The handlers are still the library's, and still work: a run started
	 * after all that stops on the next signal rather than running on. */
	ubertooth_reset_stop_signals();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a second handle");
	mu_assert_int_eq(0, ubertooth_stopping(ut));
	raise(SIGINT);
	mu_assert_int_eq(1, ubertooth_stopping(ut));
	release_handle(ut);
}

/* The deprecated register_cleanup_handler(ut, 0) leaves every one of the five
 * signals with the same handler its replacement does, which is what an outside
 * program calling it in this release gets. The rest of this file calls the
 * replacement, as the tools do; this case keeps the superseded entry point
 * covered until it is removed.
 *
 * It reads the handlers back and compares them rather than raising a signal,
 * because the handler the do_exit = 1 form installs ends the process with
 * exit(0). Raised here, that would end this test binary mid-case and ctest,
 * which judges it by its exit status, would read the silence as a pass. Each
 * handler is read with signal(sig, SIG_DFL), which returns the one that was
 * installed; release_handle() restores SIG_DFL in any case.
 *
 * The deprecation warning is switched off just for the call, so the rest of
 * the build stays strict. */
MU_TEST(the_deprecated_entry_point_installs_the_same_handlers)
{
	static const int signals[] = { SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGALRM };
	void (*replacement[ARRAY_LEN(signals)])(int);
	ubertooth_t* ut = ubertooth_init();
	size_t i;

	mu_assert_fatal(ut != NULL, "a handle");

	ubertooth_catch_stop_signals();
	for (i = 0; i < ARRAY_LEN(signals); i++)
		replacement[i] = signal(signals[i], SIG_DFL);

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	register_cleanup_handler(ut, 0);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

	for (i = 0; i < ARRAY_LEN(signals); i++)
		mu_assert(signal(signals[i], SIG_DFL) == replacement[i],
		          "the handler ubertooth_catch_stop_signals() installs");

	/* The handle the library took is given back, so nothing it holds
	 * outlives this case. */
	release_handle(ut);
}

int main(void)
{
	MU_RUN(every_caught_signal_stops_the_run);
	MU_RUN(the_wait_for_a_packet_ends_on_a_signal);
	MU_RUN(a_sending_board_is_stopped_and_its_block_kept);
	MU_RUN(a_signal_from_before_the_board_was_open_ends_a_quiet_capture);
	MU_RUN(a_signal_from_before_the_board_was_open_ends_a_sending_capture);
	MU_RUN(a_cleared_timeout_lets_a_second_capture_run);
	MU_RUN(a_cleared_timeout_keeps_an_interrupt);
	MU_RUN(a_replay_reads_the_whole_file);
	MU_RUN(a_replay_stops_when_the_user_asks);
	MU_RUN(a_run_nothing_interrupted_keeps_going);
	MU_RUN(a_freed_handle_is_let_go_of);
	MU_RUN(the_deprecated_entry_point_installs_the_same_handlers);
	return mu_report();
}
