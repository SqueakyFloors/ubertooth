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
 * The real ubertooth.c, ubertooth_callback.c and ubertooth_control.c are
 * compiled in. No device is opened and no transfer is made: the ring buffer
 * is faked (a real one is 64 MB, and nothing here receives a real packet), so
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
void fifo_inc_write_ptr(fifo_t* fifo) { (void)fifo; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet) { (void)fifo; (void)packet; }
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

/* A handle that never opened a device, with the library's own handlers
 * installed on it. */
static ubertooth_t* new_handle(void)
{
	ubertooth_t* ut = ubertooth_init();

	if (ut == NULL)
		return NULL;
	register_cleanup_handler(ut, 0);
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
	if (ut == NULL)
		return;
	ubertooth_clear_stop(ut);
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

/* A handle that runs two captures in turn (ubertooth-afh detects a map, then
 * monitors it): the alarm that ends the first phase must not end the second
 * before it starts, so the stop is cleared between them. */
MU_TEST(a_cleared_stop_lets_a_second_capture_run)
{
	ubertooth_t* ut = new_handle();

	mu_assert_fatal(ut != NULL, "a handle");

	/* The -t handler, which a tool installs over the one above, and which
	 * nothing else here reaches: the alarm itself is never waited for, so
	 * the long timer only has to be longer than the test. */
	ubertooth_set_timeout(ut, 300);
	raise(SIGALRM);
	mu_assert_int_eq(1, ubertooth_stopping(ut));

	ubertooth_clear_stop(ut);

	mu_assert_int_eq(0, ubertooth_stopping(ut));
	/* And the second phase still stops on its own Ctrl-C. */
	raise(SIGINT);
	mu_assert_int_eq(1, ubertooth_stopping(ut));

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

int main(void)
{
	MU_RUN(every_caught_signal_stops_the_run);
	MU_RUN(the_wait_for_a_packet_ends_on_a_signal);
	MU_RUN(a_sending_board_is_stopped_and_its_block_kept);
	MU_RUN(a_cleared_stop_lets_a_second_capture_run);
	MU_RUN(a_run_nothing_interrupted_keeps_going);
	return mu_report();
}
