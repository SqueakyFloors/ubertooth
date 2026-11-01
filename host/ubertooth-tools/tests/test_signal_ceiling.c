/*
 * Telling a receiver that is being driven past what it can measure from one
 * that is merely hearing something loud (ubertooth_watch_signal_level()).
 *
 * The CC2400's signal figure runs out: raw 35 and above all come out as
 * 0 dBm, and a receiver held there is also past where it demodulates
 * reliably. The cases below are built from what the bench measured at
 * 2441 MHz on two lab boards, so they pin the rule against the two things it
 * has to tell apart:
 *
 *   quiet air    26 of 19937 records reached the ceiling, 0.13%. The channel
 *                carries Wi-Fi, which is loud, so this is not an idle band:
 *                it is the ordinary case the warning must stay silent on.
 *                The 26 are put next to each other here, so the case does not
 *                depend on where in the capture they fell.
 *   pinned       with the other board a few centimeters away transmitting a
 *                carrier, every record reached the ceiling, at every one of
 *                the eight transmit power levels.
 *
 * The real ubertooth_callback.c runs. The fifo calls are the test's own
 * because the real ring buffer is 72 MB, and nothing here uses them anyway:
 * the routine under test is handed a record directly.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <btbb.h>
#include <libusb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth.h"
#include "ubertooth_callback.h"
#include "ubertooth_fifo.h"

/* ---- fifo fake ---------------------------------------------------------- */
#define FAKE_SLOTS 4

static struct {
	usb_pkt_rx slots[FAKE_SLOTS];
	size_t write_ptr;
	size_t read_ptr;
} ring;

fifo_t* fifo_init(void)
{
	memset(&ring, 0, sizeof(ring));
	return (fifo_t*) malloc(1);
}

usb_pkt_rx* fifo_get_write_element(fifo_t* fifo)
{
	(void) fifo;
	return &ring.slots[ring.write_ptr];
}

void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns)
{
	(void) fifo;
	(void) arrival_ns;
	ring.write_ptr = (ring.write_ptr + 1) % FAKE_SLOTS;
}

void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns)
{
	ring.slots[ring.write_ptr] = *packet;
	fifo_inc_write_ptr(fifo, arrival_ns);
}

uint8_t fifo_empty(fifo_t* fifo)
{
	(void) fifo;
	return ring.read_ptr == ring.write_ptr;
}

uint64_t fifo_popped_arrival_ns(const fifo_t* fifo) { (void) fifo; return 0; }

usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	size_t selected = ring.read_ptr;

	(void) fifo;
	ring.read_ptr = (ring.read_ptr + 1) % FAKE_SLOTS;
	return ring.slots[selected];
}

/* ---- what the measurement says ------------------------------------------
 *
 * The numbers the cases are built from, and the rule's own two, which the
 * cases reach by counting rather than by naming: a block is 1000 records
 * carrying a signal figure, and 950 of them at the ceiling is what the
 * warning goes on.
 */
#define CEILING_RAW 35
#define BLOCK_RECORDS 1000
#define BLOCK_AT_CEILING 950

/* What the bench read on a pinned receiver, and the quiet channel's own
 * figures: its median, and the 26 records in 19937 that reached the ceiling. */
#define PINNED_RAW 37
#define QUIET_MEDIAN_RAW (-37)
#define QUIET_RECORDS 19937
#define QUIET_AT_CEILING 26

/* ---- the records the cases hand over ------------------------------------ */

/* One Basic Rate record stating a signal figure of `avg`, as the Basic Rate
 * stream sends one for every block of symbols it collects whether or not a
 * packet was in it. */
static void signal_record(usb_pkt_rx* rx, int8_t avg)
{
	memset(rx, 0, sizeof(*rx));
	rx->pkt_type = BR_PACKET;
	rx->status = SIGNAL_VALID;
	rx->channel = 39;
	rx->rssi_max = avg;
	rx->rssi_min = (int8_t) (avg - 6);
	rx->rssi_avg = avg;
	rx->rssi_count = 50;
}

static void hand_over(ubertooth_t* ut, int8_t avg, unsigned times)
{
	usb_pkt_rx rx;
	unsigned i;

	signal_record(&rx, avg);
	for (i = 0; i < times; i++)
		ubertooth_watch_signal_level(ut, &rx);
}

/* ---- reading what was written to standard error ------------------------- */

static char captured[4096];
static int saved_stderr = -1;
static FILE* capture_file;
static char capture_path[] = "/tmp/ut-signal-ceiling-XXXXXX";

/* Standard error into a file of its own, so a case can read back what the
 * routine said. Anything that goes wrong here aborts the run rather than
 * being reported as a case: a test that cannot see standard error cannot say
 * anything about what was written to it. */
static void capture_start(void)
{
	int fd = mkstemp(capture_path);

	if (fd < 0 || (saved_stderr = dup(fileno(stderr))) < 0 ||
	    (capture_file = fdopen(fd, "w+")) == NULL) {
		printf("FAIL could not redirect standard error\n");
		exit(1);
	}
	fflush(stderr);
	if (dup2(fd, fileno(stderr)) < 0) {
		printf("FAIL could not redirect standard error\n");
		exit(1);
	}
}

/* Everything written to standard error since capture_start(), and standard
 * error back where it was. */
static const char* capture_end(void)
{
	long length;

	fflush(stderr);
	dup2(saved_stderr, fileno(stderr));
	close(saved_stderr);
	saved_stderr = -1;

	fseek(capture_file, 0, SEEK_END);
	length = ftell(capture_file);
	if (length > (long) sizeof(captured) - 1)
		length = (long) sizeof(captured) - 1;
	fseek(capture_file, 0, SEEK_SET);
	length = (long) fread(captured, 1, (size_t) length, capture_file);
	captured[length > 0 ? length : 0] = '\0';
	fclose(capture_file);
	capture_file = NULL;
	unlink(capture_path);
	/* mkstemp() rewrote the template, so put it back for the next case. */
	strcpy(capture_path, "/tmp/ut-signal-ceiling-XXXXXX");
	return captured;
}

static unsigned warnings_in(const char* text)
{
	const char* at = text;
	unsigned count = 0;

	while ((at = strstr(at, "warning: the signal is at the top")) != NULL) {
		count++;
		at++;
	}
	return count;
}

/* ---- the cases ---------------------------------------------------------- */

static ubertooth_t handle;

static ubertooth_t* fresh(void)
{
	memset(&handle, 0, sizeof(handle));
	return &handle;
}

/* The quiet channel the bench measured: 19937 records, 26 of them at the
 * ceiling and next to each other so the case does not depend on where they
 * fell. Nothing is said, and no block is counted. What this pins is the
 * distance between the two regimes; the_threshold_is_where_it_says() below is
 * what probes the boundary itself. */
MU_TEST(quiet_air_says_nothing)
{
	ubertooth_t* ut = fresh();
	const char* said;

	capture_start();
	hand_over(ut, QUIET_MEDIAN_RAW, 1000);
	hand_over(ut, PINNED_RAW, QUIET_AT_CEILING);
	hand_over(ut, QUIET_MEDIAN_RAW, QUIET_RECORDS - 1000 - QUIET_AT_CEILING);
	said = capture_end();

	mu_assert(warnings_in(said) == 0, "quiet air warned");
	mu_assert(ubertooth_signal_ceiling_blocks(ut) == 0, "quiet air counted a block");
}

/* The pinned receiver: every record at the ceiling, as both boards read at
 * every transmit power. One block is enough to be told, and it is told once
 * however long the capture stays there. */
MU_TEST(a_pinned_receiver_is_reported_once)
{
	ubertooth_t* ut = fresh();
	const char* said;

	capture_start();
	hand_over(ut, PINNED_RAW, BLOCK_RECORDS * 10);
	said = capture_end();

	mu_assert(warnings_in(said) == 1, "a pinned receiver was not reported");
	mu_assert(ubertooth_signal_ceiling_blocks(ut) == 10, "the blocks were not all counted");
}

/* Nothing is said before a whole block has been judged, so a capture that
 * ends sooner than that says nothing at all. */
MU_TEST(a_part_block_says_nothing)
{
	ubertooth_t* ut = fresh();
	const char* said;

	capture_start();
	hand_over(ut, PINNED_RAW, BLOCK_RECORDS - 1);
	said = capture_end();

	mu_assert(warnings_in(said) == 0, "a part block warned");
	mu_assert(ubertooth_signal_ceiling_blocks(ut) == 0, "a part block counted a block");
}

/* Where the threshold sits, from both sides, within one block. */
MU_TEST(the_threshold_is_where_it_says)
{
	ubertooth_t* ut = fresh();
	const char* said;

	capture_start();
	hand_over(ut, PINNED_RAW, BLOCK_AT_CEILING - 1);
	hand_over(ut, QUIET_MEDIAN_RAW, BLOCK_RECORDS - BLOCK_AT_CEILING + 1);
	said = capture_end();
	mu_assert(warnings_in(said) == 0, "one short of the threshold warned");

	ut = fresh();
	capture_start();
	hand_over(ut, PINNED_RAW, BLOCK_AT_CEILING);
	hand_over(ut, QUIET_MEDIAN_RAW, BLOCK_RECORDS - BLOCK_AT_CEILING);
	said = capture_end();
	mu_assert(warnings_in(said) == 1, "the threshold itself did not warn");
}

/* A block is judged on its own air: two blocks that each fall short do not
 * add up to one that does not. */
MU_TEST(blocks_do_not_add_up)
{
	ubertooth_t* ut = fresh();
	const char* said;
	int block;

	capture_start();
	for (block = 0; block < 4; block++) {
		hand_over(ut, PINNED_RAW, BLOCK_AT_CEILING - 1);
		hand_over(ut, QUIET_MEDIAN_RAW,
		          BLOCK_RECORDS - BLOCK_AT_CEILING + 1);
	}
	said = capture_end();

	mu_assert(warnings_in(said) == 0, "four blocks short of the threshold warned");
}

/* The raw reading the rule turns on: the one below the ceiling is an ordinary
 * strong signal and says nothing, the ceiling itself is reported. */
MU_TEST(the_ceiling_is_where_the_scale_runs_out)
{
	ubertooth_t* ut = fresh();
	const char* said;

	capture_start();
	hand_over(ut, CEILING_RAW - 1, BLOCK_RECORDS);
	said = capture_end();
	mu_assert(warnings_in(said) == 0, "one dB below the ceiling warned");

	ut = fresh();
	capture_start();
	hand_over(ut, CEILING_RAW, BLOCK_RECORDS);
	said = capture_end();
	mu_assert(warnings_in(said) == 1, "the ceiling itself did not warn");
}

/* A record that does not state a signal figure says nothing about how loud
 * the air was: its average field holds either nothing or, from an older board
 * or dump, the channel's running average. Such records are not judged, and
 * they do not count towards a block either. */
MU_TEST(records_without_a_figure_are_left_out)
{
	ubertooth_t* ut = fresh();
	usb_pkt_rx rx;
	const char* said;
	unsigned i;

	signal_record(&rx, PINNED_RAW);
	rx.status = 0;

	capture_start();
	for (i = 0; i < BLOCK_RECORDS * 4; i++)
		ubertooth_watch_signal_level(ut, &rx);
	said = capture_end();

	mu_assert(warnings_in(said) == 0, "a record with no signal figure was judged");

	/* And a whole block of real figures afterwards still reports, which is
	 * what shows the ones above were skipped rather than counted as quiet. */
	capture_start();
	hand_over(ut, PINNED_RAW, BLOCK_RECORDS);
	said = capture_end();
	mu_assert(warnings_in(said) == 1, "the records with no figure were counted as quiet");
}

/* The continuation records of a long low-energy packet repeat the readings of
 * the record that started it, so counting them would weigh one packet by how
 * many records it took. */
MU_TEST(continuation_records_are_left_out)
{
	ubertooth_t* ut = fresh();
	usb_pkt_rx rx;
	const char* said;
	unsigned i;

	signal_record(&rx, PINNED_RAW);
	rx.pkt_type = LE_PACKET_CONT;

	capture_start();
	for (i = 0; i < BLOCK_RECORDS * 4; i++)
		ubertooth_watch_signal_level(ut, &rx);
	said = capture_end();

	mu_assert(warnings_in(said) == 0, "a continuation record was counted");
}

/* A keep-alive is one byte on the wire, so every field but its type holds
 * whatever the record before it left in the buffer -- which on a capture that
 * has just been loud is a signal figure at the ceiling, with its flag set.
 * Keep-alives are what a quiet capture gets instead of packets, so counting
 * them would report a receiver that is hearing nothing at all. */
MU_TEST(keep_alives_are_left_out)
{
	ubertooth_t* ut = fresh();
	usb_pkt_rx rx;
	const char* said;
	unsigned i;

	/* As it comes off the ring: the fields of the loud record before it,
	 * with only the type overwritten. */
	signal_record(&rx, PINNED_RAW);
	rx.pkt_type = KEEP_ALIVE;

	capture_start();
	for (i = 0; i < BLOCK_RECORDS * 4; i++)
		ubertooth_watch_signal_level(ut, &rx);
	said = capture_end();

	mu_assert(warnings_in(said) == 0, "a keep-alive was counted");
	mu_assert(ubertooth_signal_ceiling_blocks(ut) == 0,
	          "a keep-alive counted a block");

	/* And a real block afterwards still reports, which shows the
	 * keep-alives were skipped rather than counted as quiet. */
	capture_start();
	hand_over(ut, PINNED_RAW, BLOCK_RECORDS);
	said = capture_end();
	mu_assert(warnings_in(said) == 1, "the keep-alives were counted as quiet");
}

/* Neither argument is trusted, as the loss counting beside it does not trust
 * them: a tool that lost its handle must not take a second fault here. */
MU_TEST(nothing_is_trusted)
{
	usb_pkt_rx rx;

	signal_record(&rx, PINNED_RAW);
	ubertooth_watch_signal_level(NULL, &rx);
	ubertooth_watch_signal_level(fresh(), NULL);
	mu_assert(ubertooth_signal_ceiling_blocks(NULL) == 0, "a missing handle reported blocks");
}

int main(void)
{
	MU_RUN(quiet_air_says_nothing);
	MU_RUN(a_pinned_receiver_is_reported_once);
	MU_RUN(a_part_block_says_nothing);
	MU_RUN(the_threshold_is_where_it_says);
	MU_RUN(blocks_do_not_add_up);
	MU_RUN(the_ceiling_is_where_the_scale_runs_out);
	MU_RUN(records_without_a_figure_are_left_out);
	MU_RUN(continuation_records_are_left_out);
	MU_RUN(keep_alives_are_left_out);
	MU_RUN(nothing_is_trusted);
	return mu_report();
}
