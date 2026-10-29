/*
 * What the low-energy callback does with what the PCAPNG writer tells it.
 *
 * lell_pcapng_append_packet() has two things to say beyond "written". It
 * returns LELL_PCAPNG_CONNECT_INFO_NOT_RECORDED, a positive value, when the
 * packet reached the file but the connection parameters it carried did not,
 * because the room set aside for them in the interface description block has
 * run out. And it returns a negative value when the write itself failed.
 *
 * The callback used to ignore the result, so a long capture quietly stopped
 * recording the parameters Wireshark needs to follow a connection, and a
 * capture nobody could write to looked like one that was being written. Now
 * each is one warning on standard error, and each is said once: the condition
 * holds for every packet that follows, so a line per packet would bury the
 * capture's own output.
 *
 * This runs the real cb_btle(), by compiling ubertooth_callback.c into itself,
 * with the ring buffer, the decoder and the PCAPNG writer replaced by the
 * fakes below. Nothing touches a device or a file.
 *
 * The cases run in order and share one process, because "once per capture" is
 * a property of the process: the callback remembers what it has already said.
 * So a case that expects a warning is followed by one that feeds the same
 * report again and expects silence.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
#include "../../libubertooth/src/ubertooth_callback.c"

/* ---- fakes -------------------------------------------------------------- */

/* The records the faked capture delivers, in order. */
static usb_pkt_rx next_packets[8];
static unsigned next_count;
static unsigned next_index;

/* What the faked writer returns, and how many times it was called. */
static int append_result;
static int appends;

fifo_t* fifo_init(void) { return (fifo_t*)malloc(1); }
void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns)
{ (void)fifo; (void)arrival_ns; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns)
{ (void)fifo; (void)packet; (void)arrival_ns; }
usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return NULL; }
uint8_t fifo_empty(fifo_t* fifo) { (void)fifo; return next_index >= next_count; }
uint64_t fifo_popped_arrival_ns(const fifo_t* fifo) { (void)fifo; return 0; }

usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	(void)fifo;
	if (next_index < next_count)
		return next_packets[next_index++];
	return next_packets[0];
}

/* Stands in for the decoder. The rest of cb_btle() uses the packet, so it is a
 * real but empty libbtbb packet; what the bytes mean is libbtbb's business and
 * no case here depends on it. */
void lell_allocate_and_decode_n(const uint8_t* stream, size_t stream_len,
                                uint16_t phys_channel, uint32_t clk100ns,
                                lell_packet** pkt)
{
	(void)stream; (void)stream_len; (void)phys_channel; (void)clk100ns;
	*pkt = lell_packet_new();
}

/* Stands in for the PCAPNG writer, returning whatever the case under way
 * wants it to say. */
int lell_pcapng_append_packet(lell_pcapng_handle* h, const uint64_t ns,
                              const int8_t sigdbm, const int8_t noisedbm,
                              const uint32_t refAA, const lell_packet* pkt)
{
	(void)h; (void)ns; (void)sigdbm; (void)noisedbm; (void)refAA; (void)pkt;
	appends++;
	return append_result;
}

/* ---- helpers ------------------------------------------------------------ */

/* A plain advertising packet, one record long. */
static void fill_packet(usb_pkt_rx* rx)
{
	memset(rx, 0, sizeof(*rx));
	rx->pkt_type = LE_PACKET;
	rx->channel = 0;
	rx->clk100ns = htole32(1234);
	/* The advertising access address, little endian, then a PDU header
	 * whose payload fits in the record. */
	rx->data[0] = 0xd6;
	rx->data[1] = 0xbe;
	rx->data[2] = 0x89;
	rx->data[3] = 0x8e;
	rx->data[4] = 0x00;
	rx->data[5] = 10;
}

/* Everything the callback wrote to standard error during the last run. */
static char warnings[4096];

/* Runs `count` identical packets through the real callback, with the writer
 * returning `result` for each. Standard output goes to /dev/null, because the
 * callback dumps every packet there; standard error is collected in
 * warnings[], which is what each case is about. */
static void run_capture(int result, unsigned count)
{
	ubertooth_t ut;
	int saved_out, saved_err, devnull, fd;
	char path[] = "/tmp/ut-le-reports-XXXXXX";
	ssize_t got;
	unsigned i;

	memset(&ut, 0, sizeof(ut));
	ut.fifo = fifo_init();
	ut.clk100ns_period = CLK100NS_TIMER1_PERIOD;
	/* Never dereferenced: the writer above is a fake. It only has to be
	 * something other than NULL, which is what makes the callback write. */
	ut.h_pcapng_le = (lell_pcapng_handle*)&ut;

	append_result = result;
	appends = 0;
	next_index = 0;
	next_count = count;
	for (i = 0; i < count; i++)
		fill_packet(&next_packets[i]);

	fd = mkstemp(path);
	if (fd < 0) {
		fprintf(stderr, "error: cannot make a temporary file\n");
		exit(1);
	}
	unlink(path);

	fflush(stdout);
	fflush(stderr);
	saved_out = dup(fileno(stdout));
	saved_err = dup(fileno(stderr));
	devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, fileno(stdout));
		close(devnull);
	}
	dup2(fd, fileno(stderr));

	while (next_index < next_count)
		cb_btle(&ut, NULL);

	fflush(stdout);
	fflush(stderr);
	dup2(saved_out, fileno(stdout));
	dup2(saved_err, fileno(stderr));
	close(saved_out);
	close(saved_err);

	memset(warnings, 0, sizeof(warnings));
	got = pread(fd, warnings, sizeof(warnings) - 1, 0);
	if (got > 0)
		warnings[got] = '\0';
	close(fd);

	free(ut.fifo);
}

/* The two lines the callback can write, in full: the man page promises these
 * words, and a test that pinned a fragment of one would let the page drift. */
#define NO_ROOM_WARNING \
	"warning: the capture file has no more room for connection " \
	"parameters; the packets are still recorded, but later connections " \
	"cannot be followed in Wireshark\n"
#define WRITE_WARNING "warning: cannot write to the PCAPNG capture file\n"

/* How many lines of what the callback wrote contain `text`. */
static int lines_with(const char* text)
{
	const char* at = warnings;
	int found = 0;

	while ((at = strstr(at, text)) != NULL) {
		found++;
		at += strlen(text);
	}
	return found;
}

/* ---- cases -------------------------------------------------------------- */

MU_TEST(a_written_packet_says_nothing)
{
	run_capture(0, 3);

	mu_assert_int_eq(3, appends);
	mu_assert(warnings[0] == '\0', "the callback said something");
}

MU_TEST(unrecorded_connection_parameters_warn_once)
{
	run_capture(LELL_PCAPNG_CONNECT_INFO_NOT_RECORDED, 3);

	/* Every packet still goes to the file; it is the parameters that do
	 * not, and that is what the warning has to name. */
	mu_assert_int_eq(3, appends);
	mu_assert_int_eq(1, lines_with("warning: "));
	mu_assert_int_eq(1, lines_with(NO_ROOM_WARNING));
	/* A positive report is not a failed write. Taking it for one would
	 * tell the user a capture that is still being written is broken. */
	mu_assert_int_eq(0, lines_with("cannot write"));
}

MU_TEST(unrecorded_connection_parameters_do_not_warn_again)
{
	run_capture(LELL_PCAPNG_CONNECT_INFO_NOT_RECORDED, 3);

	mu_assert_int_eq(3, appends);
	mu_assert(warnings[0] == '\0', "the callback said something");
}

MU_TEST(a_failed_write_warns_once)
{
	run_capture(-1, 3);

	mu_assert_int_eq(3, appends);
	mu_assert_int_eq(1, lines_with("warning: "));
	mu_assert_int_eq(1, lines_with(WRITE_WARNING));
}

MU_TEST(a_failed_write_does_not_warn_again)
{
	run_capture(-1, 3);

	mu_assert_int_eq(3, appends);
	mu_assert(warnings[0] == '\0', "the callback said something");
}

int main(void)
{
	MU_RUN(a_written_packet_says_nothing);
	MU_RUN(unrecorded_connection_parameters_warn_once);
	MU_RUN(unrecorded_connection_parameters_do_not_warn_again);
	MU_RUN(a_failed_write_warns_once);
	MU_RUN(a_failed_write_does_not_warn_again);
	return mu_report();
}
