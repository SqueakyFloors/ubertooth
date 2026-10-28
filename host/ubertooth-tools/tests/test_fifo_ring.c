/*
 * The capture ring buffer (ubertooth_fifo.c): what a consumer gets back, what
 * a full ring does, and that a producer thread and a consumer thread cannot
 * see each other's half-written work.
 *
 * The ring is built small here (the build sets UBERTOOTH_FIFO_SLOTS_FOR_TESTS),
 * because the shipped 1000000 slots put the full-ring paths out of reach of a
 * test: a case would have to produce a million records faster than it drains
 * them. Seven usable slots reach them on every lap.
 *
 * One thing is deliberately not tested: that fifo_popped_arrival_ns() is kept
 * rather than read back out of the slot the record came from. No case can
 * tell the two apart. Publishing into that slot again needs the read pointer
 * to have moved a whole lap past it, and every pop on the way replaces the
 * answer, so a read-back gives the same reading as a kept copy. The reading
 * is kept because it makes the handover one rule rather than two, not because
 * reading it back was wrong.
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
 * clk100ns holds the whole sequence number, and that is what a record is
 * known by: record_is_whole() reads it back and what it is matched against
 * the arrival reading published with it. Every other byte of the record
 * carries one byte of that number, the four of them repeating from the start
 * of the record to the end. No two sequence numbers fill a record alike, so a
 * copy that took part of one record and part of another shows a byte that
 * does not belong where it sits.
 */

/* The byte that belongs at `position` bytes into a record of this sequence
 * number. */
static uint8_t sequence_byte(uint32_t sequence, int position)
{
	return (uint8_t)(sequence >> ((position % 4) * 8));
}

static void fill_record(usb_pkt_rx* record, uint32_t sequence)
{
	int i;

	record->pkt_type = sequence_byte(sequence, 0);
	record->status = sequence_byte(sequence, 1);
	record->channel = sequence_byte(sequence, 2);
	record->clkn_high = sequence_byte(sequence, 3);
	record->clk100ns = sequence;
	record->rssi_max = (int8_t)sequence_byte(sequence, 8);
	record->rssi_min = (int8_t)sequence_byte(sequence, 9);
	record->rssi_avg = (int8_t)sequence_byte(sequence, 10);
	record->rssi_count = sequence_byte(sequence, 11);
	record->reserved[0] = sequence_byte(sequence, 12);
	record->reserved[1] = sequence_byte(sequence, 13);
	for (i = 0; i < DMA_SIZE; i++)
		record->data[i] = sequence_byte(sequence, 14 + i);
}

/* Whether every byte of the record is still the one that belongs there for
 * the sequence number the record itself carries. */
static int record_is_whole(const usb_pkt_rx* record)
{
	uint32_t sequence = record->clk100ns;
	int i;

	if (record->pkt_type != sequence_byte(sequence, 0)
	    || record->status != sequence_byte(sequence, 1)
	    || record->channel != sequence_byte(sequence, 2)
	    || record->clkn_high != sequence_byte(sequence, 3)
	    || record->rssi_max != (int8_t)sequence_byte(sequence, 8)
	    || record->rssi_min != (int8_t)sequence_byte(sequence, 9)
	    || record->rssi_avg != (int8_t)sequence_byte(sequence, 10)
	    || record->rssi_count != sequence_byte(sequence, 11)
	    || record->reserved[0] != sequence_byte(sequence, 12)
	    || record->reserved[1] != sequence_byte(sequence, 13))
		return 0;
	for (i = 0; i < DMA_SIZE; i++)
		if (record->data[i] != sequence_byte(sequence, 14 + i))
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

/* How many records the ring said it discarded, counted from the warnings it
 * wrote while a case had standard error. Call it once standard error is back,
 * so nothing is still writing to the file. */
static uint32_t count_overflow_warnings(FILE* warnings)
{
	char line[256];
	uint32_t count = 0;

	rewind(warnings);
	while (fgets(line, sizeof(line), warnings) != NULL)
		if (strstr(line, "FIFO overflow") != NULL)
			count++;
	return count;
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

/* The other way in: fifo_push() copies a record the caller already holds,
 * rather than the caller filling the slot in place. That is the path a
 * replayed dump takes, so a record it carried short or published before it
 * finished copying would lose the tail of every replayed packet. */
MU_TEST(a_pushed_record_comes_back_whole)
{
	fifo_t* fifo = fifo_init();
	uint32_t i;

	mu_assert_fatal(fifo != NULL, "cannot allocate the ring buffer");

	for (i = 0; i < RING_RECORDS; i++) {
		usb_pkt_rx record;

		fill_record(&record, 5000 + i);
		fifo_push(fifo, &record, 90000 + i);
		/* Published, and the copy is complete: a publish that came
		 * first would leave the record half written in the ring, which
		 * the pops below read. */
		mu_assert_int_eq(0, fifo_empty(fifo));
	}

	for (i = 0; i < RING_RECORDS; i++) {
		usb_pkt_rx popped = fifo_pop(fifo);

		mu_assert_int_eq(5000 + i, popped.clk100ns);
		mu_assert(record_is_whole(&popped),
		          "the pushed record came back short or torn");
		mu_assert_int_eq(90000 + i, fifo_popped_arrival_ns(fifo));
	}
	mu_assert_int_eq(1, fifo_empty(fifo));

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
	 * being the one the ring leaves empty.
	 *
	 * The step count is high because this is the case that would catch a
	 * ring refusing a record it had room for, and a ring that did so only
	 * now and then needs a long run to be caught at it. Sixty thousand
	 * publishes take a few milliseconds: it is arithmetic and a memcpy of
	 * 64 bytes each, with nothing to wait for. */
	for (step = 0; step < 20000; step++) {
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

/* ---- the two threads -----------------------------------------------------
 * The real arrangement: one thread fills slots and publishes them, the other
 * drains them. The producer runs far ahead of the consumer, so the ring is
 * full nearly all the time, which is where a slot handed to a transfer while
 * the consumer was still copying it out gave one corrupted record.
 *
 * This is a race detector, so it catches a regression often rather than
 * always. Putting the pop's copy back after the slot is handed over -- the
 * bug this ring was fixed for -- failed 19 runs in 20 here at a million
 * records. Take a failure seriously on one run; do not read a single pass as
 * proof.
 *
 * Two things it cannot show at all. One is the release and acquire ordering:
 * on the strongly ordered processors this fork builds for, three of the four
 * can each be weakened on their own and every run still passes, because the
 * processor was already doing it, and on x86-64 the generated code does not
 * even differ. That the ordering is there for the machines where it is not is
 * argued in ubertooth_fifo.c rather than shown here. The other is whether the
 * ring discards only when it is really full: that needs a reader that knows
 * how full the ring was, which a racing consumer is not, so it is
 * the_ring_behaves_as_a_queue_across_many_laps that asks.
 */

/* Records the producer publishes. Large enough that the two threads
 * interleave at every point of the ring many times over; a run takes a
 * fraction of a second. */
#define THREADED_RECORDS 1000000

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
	uint32_t discarded;        /* gaps in what the consumer was given */
	uint32_t last_sequence;    /* the last record the consumer was given */
	/* How many records the ring itself said it was throwing away, read
	 * back from its warnings once the threads have finished. The gaps
	 * above are held against this: a record that went missing without the
	 * ring discarding it leaves a gap nothing accounts for. */
	uint32_t announced_discards;
};

static void* producer_main(void* argument)
{
	struct run* run = (struct run*)argument;
	uint32_t sequence;

	for (sequence = 1; sequence <= THREADED_RECORDS; sequence++) {
		/* Both ways of publishing take a turn, so each is exposed to a
		 * consumer draining the ring at the same time. Either one
		 * publishing a slot before it had finished filling it would
		 * show up as a torn record. */
		if (sequence % 2 == 0) {
			publish_in_place(run->fifo, sequence, sequence);
		} else {
			usb_pkt_rx record;

			fill_record(&record, sequence);
			fifo_push(run->fifo, &record, sequence);
		}
	}

	atomic_store(&run->producer_done, 1);
	return NULL;
}

static void* consumer_main(void* argument)
{
	struct run* run = (struct run*)argument;

	for (;;) {
		usb_pkt_rx popped;
		uint64_t reading;

		if (fifo_empty(run->fifo)) {
			/* Asked again after the flag, not before it: a record
			 * published between the two questions would otherwise
			 * end the run unexamined. Once the flag is set the
			 * producer has published everything it is going to, so
			 * an empty ring now is a drained one. */
			if (atomic_load(&run->producer_done)
			    && fifo_empty(run->fifo))
				return NULL;
			continue;
		}

		popped = fifo_pop(run->fifo);
		reading = fifo_popped_arrival_ns(run->fifo);

		if (!record_is_whole(&popped))
			run->torn++;
		if (popped.clk100ns <= run->last_sequence)
			run->out_of_order++;
		else
			run->discarded +=
				popped.clk100ns - run->last_sequence - 1;
		if (reading != popped.clk100ns)
			run->reading_mismatch++;
		run->last_sequence = popped.clk100ns;
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
	/* The refusals go here rather than to /dev/null: counting them is how
	 * the gaps the consumer saw are checked against what the ring says it
	 * threw away. A ring this small refuses a record on nearly every lap,
	 * so there are thousands of them. */
	FILE* warnings = tmpfile();
	pthread_t producer, consumer;

	memset(&run, 0, sizeof(run));
	atomic_init(&run.producer_done, 0);
	run.fifo = fifo_init();
	mu_assert_fatal(run.fifo != NULL, "cannot allocate the ring buffer");
	mu_assert_fatal(warnings != NULL, "tmpfile failed");
	redirect_stderr_to(fileno(warnings));

	if (pthread_create(&producer, NULL, producer_main, &run) != 0) {
		restore_stderr();
		fclose(warnings);
		free(run.fifo);
		mu_assert_fatal(0, "the producer thread could not be started");
	}
	if (pthread_create(&consumer, NULL, consumer_main, &run) != 0) {
		/* The producer is left to finish rather than detached part way:
		 * it writes the ring this thread is about to free. */
		pthread_join(producer, NULL);
		restore_stderr();
		fclose(warnings);
		free(run.fifo);
		mu_assert_fatal(0, "the consumer thread could not be started");
	}

	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);
	restore_stderr();

	/* The gaps the consumer counted stop at the last record it was given,
	 * so anything missing after that one is added here. */
	run.discarded += THREADED_RECORDS - run.last_sequence;
	run.announced_discards = count_overflow_warnings(warnings);
	fclose(warnings);

	printf("  %u records through, %u discarded\n",
	       run.popped, run.discarded);
	mu_assert_int_eq(0, run.torn);
	mu_assert_int_eq(0, run.out_of_order);
	mu_assert_int_eq(0, run.reading_mismatch);
	/* The run has to have carried records, or the three counters above are
	 * all about nothing. */
	mu_assert(run.popped > 0, "the consumer got nothing at all");
	/* Nothing went missing unannounced: every record the consumer did not
	 * get is one the ring said it was discarding. */
	mu_assert_int_eq(run.announced_discards, run.discarded);
	/* Without discards the run never had a full ring, so it did not cover
	 * the handover this case is here for. */
	mu_assert(run.discarded > 0, "the ring never filled up");

	free(run.fifo);
}

int main(void)
{
	MU_RUN(records_come_back_in_order);
	MU_RUN(a_pushed_record_comes_back_whole);
	MU_RUN(a_full_ring_discards_the_newest_record);
	MU_RUN(the_ring_behaves_as_a_queue_across_many_laps);
	MU_RUN(two_threads_never_see_a_half_written_record);
	return mu_report();
}
