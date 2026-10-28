/*
 * The capture ring buffer (ubertooth_fifo.c): what a consumer gets back, what
 * a full ring does, and that a producer thread and a consumer thread cannot
 * see each other's half-written work.
 *
 * The ring is built small here (FIFO_SIZE comes from the build), because the
 * shipped 1000000 slots put the full-ring paths out of reach of a test: a
 * case would have to produce a million records faster than it drains them.
 * Seven usable slots reach them on every lap.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth_fifo.h"

/* Records the ring can hold at once: it always leaves one slot empty. */
#define RING_RECORDS (FIFO_SIZE - 1)

/* ---- records -------------------------------------------------------------
 * A record is filled with one repeated byte, every named field of it, so a
 * copy that picked up part of another record shows as a byte that does not
 * match the rest. clk100ns carries the whole sequence number, so a record can
 * also be matched against the arrival reading published with it.
 */

static void fill_record(usb_pkt_rx* record, uint32_t sequence)
{
	uint8_t byte = (uint8_t)sequence;
	int i;

	record->pkt_type = byte;
	record->status = byte;
	record->channel = byte;
	record->clkn_high = byte;
	record->clk100ns = sequence;
	record->rssi_max = (int8_t)byte;
	record->rssi_min = (int8_t)byte;
	record->rssi_avg = (int8_t)byte;
	record->rssi_count = byte;
	record->reserved[0] = byte;
	record->reserved[1] = byte;
	for (i = 0; i < DMA_SIZE; i++)
		record->data[i] = byte;
}

/* Whether every byte of the record still carries the one value fill_record()
 * wrote, that of its own sequence number. */
static int record_is_whole(const usb_pkt_rx* record)
{
	uint8_t byte = (uint8_t)record->clk100ns;
	int i;

	if (record->pkt_type != byte || record->status != byte
	    || record->channel != byte || record->clkn_high != byte
	    || record->rssi_max != (int8_t)byte
	    || record->rssi_min != (int8_t)byte
	    || record->rssi_avg != (int8_t)byte
	    || record->rssi_count != byte
	    || record->reserved[0] != byte || record->reserved[1] != byte)
		return 0;
	for (i = 0; i < DMA_SIZE; i++)
		if (record->data[i] != byte)
			return 0;
	return 1;
}

/* Publishes one record the way a USB transfer does: the slot is filled where
 * it lies and then published, rather than copied in with fifo_push(). */
static void publish_in_place(fifo_t* fifo, uint32_t sequence,
                             uint64_t arrival_ns)
{
	fill_record(fifo_get_write_element(fifo), sequence);
	fifo_inc_write_ptr(fifo, arrival_ns);
}

/* ---- the warnings a full ring prints -------------------------------------
 * A refused publish says so on standard error. One case reads that line back;
 * the rest only need it out of the way, because a case that fills the ring
 * over and over prints thousands of them.
 */

static int saved_stderr = -1;

static void redirect_stderr_to(int descriptor)
{
	fflush(stderr);
	if (saved_stderr < 0)
		saved_stderr = dup(STDERR_FILENO);
	mu_assert(saved_stderr >= 0, "standard error could not be saved");
	mu_assert(dup2(descriptor, STDERR_FILENO) >= 0,
	          "standard error could not be redirected");
}

static void restore_stderr(void)
{
	fflush(stderr);
	if (saved_stderr < 0)
		return;
	mu_assert(dup2(saved_stderr, STDERR_FILENO) >= 0,
	          "standard error could not be put back");
	close(saved_stderr);
	saved_stderr = -1;
}

/* ---- cases --------------------------------------------------------------- */

/* Up to a ring's worth of records come back in the order they went in, each
 * with the arrival reading it was published with. */
MU_TEST(records_come_back_in_order)
{
	fifo_t* fifo = fifo_init();
	uint32_t i;

	mu_assert_fatal(fifo != NULL, "cannot allocate the ring buffer");
	mu_assert_int_eq(1, fifo_empty(fifo));

	for (i = 0; i < RING_RECORDS; i++)
		publish_in_place(fifo, i + 1, 1000 + i);

	mu_assert_int_eq(0, fifo_empty(fifo));

	for (i = 0; i < RING_RECORDS; i++) {
		usb_pkt_rx popped = fifo_pop(fifo);

		mu_assert_int_eq(i + 1, popped.clk100ns);
		mu_assert(record_is_whole(&popped), "the record came back torn");
		mu_assert_int_eq(1000 + i, fifo_popped_arrival_ns(fifo));
	}

	mu_assert_int_eq(1, fifo_empty(fifo));
	free(fifo);
}

/* A publish into a ring with no room left is refused: the record is
 * discarded, every record already in the ring is still there, and the
 * refusal is reported. */
MU_TEST(a_full_ring_discards_the_newest_record)
{
	fifo_t* fifo = fifo_init();
	FILE* warnings = tmpfile();
	char line[256];
	uint32_t i;

	mu_assert_fatal(fifo != NULL, "cannot allocate the ring buffer");
	mu_assert_fatal(warnings != NULL, "tmpfile failed");

	for (i = 0; i < RING_RECORDS; i++)
		publish_in_place(fifo, i + 1, 1000 + i);

	redirect_stderr_to(fileno(warnings));
	/* Twice, so that a refusal that advanced the write pointer by one each
	 * time would be caught by the records read back below. */
	publish_in_place(fifo, 0xee, 9999);
	publish_in_place(fifo, 0xef, 9998);
	restore_stderr();

	for (i = 0; i < RING_RECORDS; i++) {
		usb_pkt_rx popped = fifo_pop(fifo);

		mu_assert_int_eq(i + 1, popped.clk100ns);
		mu_assert(record_is_whole(&popped), "the record came back torn");
		mu_assert_int_eq(1000 + i, fifo_popped_arrival_ns(fifo));
	}
	mu_assert_int_eq(1, fifo_empty(fifo));

	rewind(warnings);
	mu_assert(fgets(line, sizeof(line), warnings) != NULL,
	          "the refusal was not reported");
	mu_assert(strstr(line, "warning: FIFO overflow, packet discarded")
	          != NULL, "the refusal was reported in other words");
	fclose(warnings);
	free(fifo);
}

/* Many laps of mixed publishing and draining, against a queue the case keeps
 * itself: every record comes back in order with its own reading, a publish is
 * refused exactly when the ring is full, and nothing changes across the wrap
 * at the end of the slots. */
MU_TEST(the_ring_behaves_as_a_queue_across_many_laps)
{
	fifo_t* fifo = fifo_init();
	int devnull = open("/dev/null", O_WRONLY);
	/* The queue the ring is expected to hold, oldest first. */
	uint32_t expected[RING_RECORDS];
	size_t held = 0;
	uint32_t next_sequence = 1;
	uint32_t refused = 0;
	int step;

	mu_assert_fatal(fifo != NULL, "cannot allocate the ring buffer");
	mu_assert_fatal(devnull >= 0, "/dev/null could not be opened");
	redirect_stderr_to(devnull);

	/* Publishing three and draining two each step walks the ring round
	 * many times and fills it on the way, so every slot takes a turn at
	 * being the one the ring leaves empty. */
	for (step = 0; step < 200; step++) {
		int i;

		for (i = 0; i < 3; i++) {
			publish_in_place(fifo, next_sequence, next_sequence);
			if (held < RING_RECORDS)
				expected[held++] = next_sequence;
			else
				refused++;
			next_sequence++;
		}
		for (i = 0; i < 2 && held > 0; i++) {
			usb_pkt_rx popped = fifo_pop(fifo);

			mu_assert_int_eq(expected[0], popped.clk100ns);
			mu_assert_int_eq(expected[0],
			                 fifo_popped_arrival_ns(fifo));
			mu_assert(record_is_whole(&popped),
			          "the record came back torn");
			held--;
			memmove(expected, expected + 1,
			        held * sizeof(expected[0]));
		}
		mu_assert_int_eq(held == 0, fifo_empty(fifo));
	}

	restore_stderr();
	close(devnull);
	/* Without refusals the case never reached a full ring, so it did not
	 * test what it is here for. */
	mu_assert(refused > 0, "the ring never filled up");
	free(fifo);
}

/* The reading of the record just popped is taken during that pop, so later
 * records published into the slot it came from cannot replace it. */
MU_TEST(the_popped_reading_belongs_to_the_popped_record)
{
	fifo_t* fifo = fifo_init();
	int devnull = open("/dev/null", O_WRONLY);
	usb_pkt_rx popped;
	uint32_t i;

	mu_assert_fatal(fifo != NULL, "cannot allocate the ring buffer");
	mu_assert_fatal(devnull >= 0, "/dev/null could not be opened");

	publish_in_place(fifo, 1, 11);
	publish_in_place(fifo, 2, 22);

	popped = fifo_pop(fifo);
	mu_assert_int_eq(1, popped.clk100ns);
	mu_assert_int_eq(11, fifo_popped_arrival_ns(fifo));

	/* Enough publishing to go right round the slots and fill the ring,
	 * including the slot the record above came out of. */
	redirect_stderr_to(devnull);
	for (i = 0; i < 2 * FIFO_SIZE; i++)
		publish_in_place(fifo, 100 + i, 5000 + i);
	restore_stderr();
	close(devnull);

	mu_assert_int_eq(11, fifo_popped_arrival_ns(fifo));

	popped = fifo_pop(fifo);
	mu_assert_int_eq(2, popped.clk100ns);
	mu_assert_int_eq(22, fifo_popped_arrival_ns(fifo));

	free(fifo);
}

/* ---- the two threads -----------------------------------------------------
 * The real arrangement: one thread fills slots and publishes them, the other
 * drains them. The producer runs far ahead of the consumer, so the ring is
 * full nearly all the time, which is where a slot handed to a transfer while
 * the consumer was still copying it out gave one corrupted record.
 */

/* Records the producer publishes. Large enough that the two threads
 * interleave at every point of the ring many times over; a run takes a
 * fraction of a second. */
#define THREADED_RECORDS 200000

struct run {
	fifo_t* fifo;
	/* Set once the producer has published its last record. Written by the
	 * producer and read by the consumer, so it is an atomic for the same
	 * reason the ring's own pointers are. */
	_Atomic int producer_done;
	/* What the consumer saw, read only once both threads have finished. */
	uint32_t popped;
	uint32_t torn;             /* records that came back part another's */
	uint32_t out_of_order;     /* sequence numbers that did not go up */
	uint32_t reading_mismatch; /* readings that belonged to another record */
	uint32_t discarded;        /* gaps, so records the full ring refused */
};

static void* producer_main(void* argument)
{
	struct run* run = (struct run*)argument;
	uint32_t sequence;

	for (sequence = 1; sequence <= THREADED_RECORDS; sequence++)
		publish_in_place(run->fifo, sequence, sequence);

	atomic_store(&run->producer_done, 1);
	return NULL;
}

static void* consumer_main(void* argument)
{
	struct run* run = (struct run*)argument;
	uint32_t previous = 0;

	for (;;) {
		usb_pkt_rx popped;
		uint64_t reading;

		if (fifo_empty(run->fifo)) {
			if (atomic_load(&run->producer_done))
				return NULL;
			continue;
		}

		popped = fifo_pop(run->fifo);
		reading = fifo_popped_arrival_ns(run->fifo);

		if (!record_is_whole(&popped))
			run->torn++;
		if (popped.clk100ns <= previous)
			run->out_of_order++;
		else
			run->discarded += popped.clk100ns - previous - 1;
		if (reading != popped.clk100ns)
			run->reading_mismatch++;
		previous = popped.clk100ns;
		run->popped++;

		/* A pause now and then keeps the consumer behind, so the ring
		 * stays full and the producer keeps reaching the refusal. */
		if (run->popped % 1000 == 0)
			usleep(1);
	}
}

MU_TEST(two_threads_never_see_a_half_written_record)
{
	struct run run;
	int devnull = open("/dev/null", O_WRONLY);
	pthread_t producer, consumer;

	memset(&run, 0, sizeof(run));
	atomic_init(&run.producer_done, 0);
	run.fifo = fifo_init();
	mu_assert_fatal(run.fifo != NULL, "cannot allocate the ring buffer");
	mu_assert_fatal(devnull >= 0, "/dev/null could not be opened");
	/* The producer refuses a record on nearly every lap of a ring this
	 * small, and each refusal prints a line. */
	redirect_stderr_to(devnull);

	if (pthread_create(&producer, NULL, producer_main, &run) != 0) {
		restore_stderr();
		close(devnull);
		free(run.fifo);
		mu_assert_fatal(0, "the producer thread could not be started");
	}
	if (pthread_create(&consumer, NULL, consumer_main, &run) != 0) {
		/* The producer is left to finish rather than detached part way:
		 * it writes the ring this thread is about to free. */
		pthread_join(producer, NULL);
		restore_stderr();
		close(devnull);
		free(run.fifo);
		mu_assert_fatal(0, "the consumer thread could not be started");
	}

	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);
	restore_stderr();
	close(devnull);

	printf("  %u records through, %u refused by a full ring\n",
	       run.popped, run.discarded);
	mu_assert_int_eq(0, run.torn);
	mu_assert_int_eq(0, run.out_of_order);
	mu_assert_int_eq(0, run.reading_mismatch);
	mu_assert(run.popped > 0, "the consumer got nothing at all");
	/* Without refusals the run never had a full ring, so it did not cover
	 * the handover this case is here for. */
	mu_assert(run.discarded > 0, "the ring never filled up");

	free(run.fifo);
}

int main(void)
{
	MU_RUN(records_come_back_in_order);
	MU_RUN(a_full_ring_discards_the_newest_record);
	MU_RUN(the_ring_behaves_as_a_queue_across_many_laps);
	MU_RUN(the_popped_reading_belongs_to_the_popped_record);
	MU_RUN(two_threads_never_see_a_half_written_record);
	return mu_report();
}
