/*
 * Bounds on the e-go receive buffer (firmware/bluetooth_rxtx/ego.c).
 *
 * do_rx() reads as many bytes off the radio as the host last asked for, into
 * a buffer that is a local of the state handler that called it. The length
 * came straight off the wire, so a host that asked for 255 made the loop
 * write past a 48-byte buffer, over the timestamp beside it and then down the
 * stack, where the handler's return address is. RFCAT_CAP_LEN now refuses a
 * length outside 1 to 48, the range `ubertooth-ego` offers, and do_rx()
 * clamps its own loop as well, so neither one alone has to be right.
 *
 * The second, smaller problem is in the same buffer. Every captured packet
 * goes to enqueue_with_ts(), which takes no length and copies DMA_SIZE (50)
 * bytes, so a 48-byte buffer was read two bytes past its end -- and what
 * followed it was the low half of the timestamp, which then went out in the
 * data field of the record carrying that same timestamp.
 *
 * do_rx() and the request handler are hardware code that cannot run here
 * (they read the SSP's data register and the radio's status), so the fakes
 * below are the shape of both -- the buffer with a guard region behind it,
 * the SSP handing over bytes, the record copy -- and the last tests scan the
 * firmware source, the way test_tx_generic_bounds.c does for the sibling
 * problem in the generic transmit packet, so the fakes cannot drift away from
 * what the firmware does.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"

/* As ego.c has them. */
#define EGO_MAX_PACKET_LEN 48

typedef struct {
	uint8_t rxbuf[DMA_SIZE];
	uint32_t rxtime;
} ego_packet_t;

/* The old buffer, for the test that shows how far the unbounded loop ran. */
typedef struct {
	uint8_t rxbuf[EGO_MAX_PACKET_LEN];
	uint32_t rxtime;
} ego_packet_old_t;

/*
 * The packet with the stack behind it standing in for the state handler's
 * frame. Written only through `pkt`, so a receive that stays inside the
 * buffer leaves `behind` as it was.
 */
#define GUARD_LEN 256

static struct {
	ego_packet_t pkt;
	uint8_t behind[GUARD_LEN];
} frame;

/* What RFCAT_CAP_LEN set, as the firmware's file-scope packet_len. */
static uint8_t packet_len;

/* The bytes the radio hands over, one per read of the SSP data register. */
static unsigned ssp_reads;

static void reset_state(void)
{
	memset(&frame, 0, sizeof(frame));
	packet_len = 18;        /* the firmware's power-on value */
	ssp_reads = 0;
}

static uint8_t ssp_read_byte(void)
{
	ssp_reads++;
	return 0xa5;
}

/* The handler for RFCAT_CAP_LEN. 1 accepts the request, 0 refuses it. */
static int rfcat_cap_len(const uint8_t *body, int body_len)
{
	if (body_len != 1)
		return 0;
	if (body[0] == 0 || body[0] > EGO_MAX_PACKET_LEN)
		return 0;
	packet_len = body[0];
	return 1;
}

/* do_rx() as it is now: the loop is clamped to the buffer whatever set
 * packet_len. */
static void do_rx(ego_packet_t *packet)
{
	int i;
	int len = packet_len > EGO_MAX_PACKET_LEN ? EGO_MAX_PACKET_LEN
	                                          : (int)packet_len;

	for (i = 0; i < len; i++)
		packet->rxbuf[i] = ssp_read_byte();
}

/* Is the stack behind the packet still as it was? */
static int guard_is_clean(void)
{
	unsigned i;

	for (i = 0; i < GUARD_LEN; i++)
		if (frame.behind[i] != 0)
			return 0;
	return 1;
}

/* ---- the length the host may ask for ---------------------------------- */

MU_TEST(test_a_length_in_range_is_accepted)
{
	uint8_t body[1];
	unsigned n;

	for (n = 1; n <= EGO_MAX_PACKET_LEN; n++) {
		reset_state();
		body[0] = (uint8_t)n;
		mu_assert_int_eq(1, rfcat_cap_len(body, 1));
		mu_assert_int_eq((int)n, packet_len);
	}
}

/* One byte more than the buffer takes is refused, and the length is left as
 * it was rather than half applied. */
MU_TEST(test_a_length_one_over_the_limit_is_refused)
{
	uint8_t body[1] = { EGO_MAX_PACKET_LEN + 1 };

	reset_state();
	mu_assert_int_eq(0, rfcat_cap_len(body, 1));
	mu_assert_int_eq(18, packet_len);
}

/* So is the worst case: the longest a byte can say. */
MU_TEST(test_the_longest_possible_length_is_refused)
{
	uint8_t body[1] = { 0xff };

	reset_state();
	mu_assert_int_eq(0, rfcat_cap_len(body, 1));
	mu_assert_int_eq(18, packet_len);
}

/* A capture of no bytes only enqueues a record of nothing, and the host tool
 * does not offer it either (EGO_PACKET_LENGTH_MIN is 1). */
MU_TEST(test_a_length_of_zero_is_refused)
{
	uint8_t body[1] = { 0 };

	reset_state();
	mu_assert_int_eq(0, rfcat_cap_len(body, 1));
	mu_assert_int_eq(18, packet_len);
}

/* The body is one byte, as it always was. */
MU_TEST(test_a_body_of_the_wrong_length_is_refused)
{
	uint8_t body[4] = { 16, 16, 16, 16 };

	reset_state();
	mu_assert_int_eq(0, rfcat_cap_len(body, 0));
	mu_assert_int_eq(0, rfcat_cap_len(body, 2));
	mu_assert_int_eq(0, rfcat_cap_len(body, 4));
	mu_assert_int_eq(18, packet_len);
}

/* ---- the receive itself ----------------------------------------------- */

/* A capture at the limit fills the buffer and stops there. */
MU_TEST(test_a_capture_at_the_limit_stays_in_the_buffer)
{
	uint8_t body[1] = { EGO_MAX_PACKET_LEN };
	int i;

	reset_state();
	mu_assert_int_eq(1, rfcat_cap_len(body, 1));
	do_rx(&frame.pkt);

	mu_assert_int_eq(EGO_MAX_PACKET_LEN, (int)ssp_reads);
	for (i = 0; i < EGO_MAX_PACKET_LEN; i++)
		mu_assert_int_eq(0xa5, frame.pkt.rxbuf[i]);
	mu_assert(guard_is_clean(), "a capture at the limit wrote past the frame");
}

/*
 * And a length the handler would never set is clamped as well: the same belt
 * and braces the generic transmit path keeps between its handler and
 * tx_generic(), so one missing check does not reach the stack.
 */
MU_TEST(test_a_capture_longer_than_the_buffer_is_clamped)
{
	unsigned n;

	for (n = EGO_MAX_PACKET_LEN; n <= 0xff; n++) {
		reset_state();
		packet_len = (uint8_t)n;
		do_rx(&frame.pkt);
		mu_assert(ssp_reads <= EGO_MAX_PACKET_LEN,
		          "do_rx() read more bytes than the buffer holds");
		mu_assert(guard_is_clean(),
		          "do_rx() wrote past the buffer and over the frame");
	}
}

/*
 * The guard region is what makes the two tests above mean something: the loop
 * the firmware used to run, with the same length, reaches well past the
 * buffer. Carried out on a frame big enough to hold it, so this test shows
 * how far it ran rather than running past an array itself.
 */
MU_TEST(test_the_old_unbounded_loop_ran_past_the_buffer)
{
	unsigned past = 0, i;

	reset_state();
	packet_len = 0xff;

	/* `for (i = 0; i < packet_len; i++) packet->rxbuf[i] = ...;` */
	for (i = 0; i < packet_len; i++)
		((uint8_t *)&frame.pkt)[i] = 0xa5;

	for (i = sizeof(ego_packet_old_t); i < packet_len; i++)
		if (((uint8_t *)&frame.pkt)[i] != 0)
			past++;
	mu_assert(past > 200, "the old loop should have run well past the "
	                      "48-byte buffer, so the guard region above is "
	                      "checking something");
}

/* ---- the record the host receives ------------------------------------- */

/*
 * enqueue_with_ts() copies DMA_SIZE bytes out of whatever it is handed, so
 * the buffer has to be that size: the old one was two bytes short, and those
 * two bytes were the low half of the timestamp beside it.
 */
MU_TEST(test_the_buffer_is_the_size_the_record_copies)
{
	mu_assert_int_eq(DMA_SIZE, (int)sizeof(frame.pkt.rxbuf));
	mu_assert(EGO_MAX_PACKET_LEN <= DMA_SIZE,
	          "the longest capture must fit in a record's data field");
}

MU_TEST(test_the_record_copy_stays_in_the_buffer)
{
	uint8_t record[DMA_SIZE];
	unsigned i;

	reset_state();
	frame.pkt.rxtime = 0xdeadbeef;
	packet_len = EGO_MAX_PACKET_LEN;
	do_rx(&frame.pkt);

	memcpy(record, frame.pkt.rxbuf, DMA_SIZE);

	/* The captured bytes, then the two the old buffer did not have. They
	 * are part of the buffer now, so they are whatever was received there
	 * -- zero on a capture this length -- and not the timestamp. */
	for (i = 0; i < EGO_MAX_PACKET_LEN; i++)
		mu_assert_int_eq(0xa5, record[i]);
	for (i = EGO_MAX_PACKET_LEN; i < DMA_SIZE; i++)
		mu_assert_int_eq(0, record[i]);
}

/* The old buffer's last two record bytes, for comparison: the copy ran into
 * the timestamp field, which is how every e-go record carried two bytes of
 * its own timestamp in its data. */
MU_TEST(test_the_old_record_copy_read_the_timestamp)
{
	struct {
		ego_packet_old_t pkt;
		uint8_t behind[8];
	} old;
	uint8_t record[DMA_SIZE];

	memset(&old, 0, sizeof(old));
	old.pkt.rxtime = 0xdeadbeef;
	memcpy(record, old.pkt.rxbuf, DMA_SIZE);

	mu_assert_int_eq(0xef, record[EGO_MAX_PACKET_LEN]);
	mu_assert_int_eq(0xbe, record[EGO_MAX_PACKET_LEN + 1]);
}

/* ---- the shape of the firmware ---------------------------------------- */

struct scan {
	int buffer_decl;        /* rxbuf declared at DMA_SIZE */
	int static_assert_line; /* the compile-time check that the max fits */
	int cap_len_case;       /* the RFCAT_CAP_LEN case */
	int cap_len_check;      /* ...refusing an out-of-range length */
	int cap_len_assign;     /* ...and the assignment it guards */
	int do_rx_line;         /* where do_rx() is defined */
	int do_rx_clamp;        /* the clamp on the loop's bound */
	int do_rx_loop;         /* the loop itself */
	int unclamped_loop;     /* a loop straight off packet_len */
};

static int scan_source(const char *path, struct scan *s)
{
	char line[SCAN_LINE_MAX], flat[SCAN_LINE_MAX];
	int lineno = 0, in_block = 0, in_cap_len = 0, in_do_rx = 0;
	int do_rx_depth = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;
	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		strip_string_literals(line);
		collapse_spaces(line, flat, sizeof(flat));

		if (strstr(flat, "u8 rxbuf[DMA_SIZE];") != NULL)
			s->buffer_decl = lineno;
		if (strstr(flat, "_Static_assert(EGO_MAX_PACKET_LEN <= DMA_SIZE") !=
		    NULL)
			s->static_assert_line = lineno;

		if (strstr(flat, "static void do_rx(") != NULL) {
			s->do_rx_line = lineno;
			in_do_rx = 1;
			do_rx_depth = 0;
		}
		if (in_do_rx) {
			const char *c;

			if (strstr(flat, "> EGO_MAX_PACKET_LEN ? EGO_MAX_PACKET_LEN") !=
			    NULL)
				s->do_rx_clamp = lineno;
			if (strstr(flat, "for (i = 0; i < len; i++)") != NULL)
				s->do_rx_loop = lineno;
			if (strstr(flat, "i < packet_len") != NULL)
				s->unclamped_loop = lineno;
			/* The function ends when its own brace closes, counted
			 * rather than taken as the first line that is a lone
			 * '}': the loop inside it closes one too, and a window
			 * that stopped there would miss anything added after
			 * it. */
			for (c = flat; *c != '\0'; c++) {
				if (*c == '{')
					do_rx_depth++;
				else if (*c == '}' && --do_rx_depth == 0)
					in_do_rx = 0;
			}
		}

		if (strstr(flat, "case RFCAT_CAP_LEN:") != NULL) {
			s->cap_len_case = lineno;
			in_cap_len = 1;
			continue;
		}
		if (!in_cap_len)
			continue;
		if (strstr(flat, "body[0] == 0 || body[0] > EGO_MAX_PACKET_LEN") !=
		    NULL)
			s->cap_len_check = lineno;
		if (is_assignment_of(flat, "packet_len", "body[0]"))
			s->cap_len_assign = lineno;
		if (strstr(flat, "break;") != NULL)
			in_cap_len = 0;
	}

	fclose(f);
	return 1;
}

static struct scan s;
static int scanned;
static const char *path = FW_RXTX_DIR "/ego.c";

MU_TEST(test_source_was_scanned)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
	mu_assert(scanned, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan found neither do_rx() nor the capture-length "
	         "request, so the checks below would pass without reading "
	         "anything", path);
	mu_assert(s.do_rx_line > 0 && s.cap_len_case > 0, msg);
}

MU_TEST(test_the_buffer_is_declared_at_the_record_size)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s: the receive buffer is not declared as u8 rxbuf[DMA_SIZE], "
	         "so the record copy in enqueue_with_ts() reads past its end",
	         path);
	mu_assert(s.buffer_decl > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: there is no compile-time check that EGO_MAX_PACKET_LEN "
	         "fits in DMA_SIZE, so raising the capture limit could outgrow "
	         "the buffer in silence", path);
	mu_assert(s.static_assert_line > 0, msg);
}

MU_TEST(test_the_request_refuses_before_it_sets_the_length)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s:%d: the capture-length request does not refuse a length "
	         "outside 1 to EGO_MAX_PACKET_LEN, so do_rx() would write past "
	         "the buffer", path, s.cap_len_case);
	mu_assert(s.cap_len_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the capture-length request sets packet_len at line %d, "
	         "which is not after the check at line %d", path,
	         s.cap_len_assign, s.cap_len_check);
	mu_assert(s.cap_len_assign > 0 && s.cap_len_check < s.cap_len_assign,
	          msg);
}

MU_TEST(test_the_receive_loop_is_clamped)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s:%d: do_rx() does not clamp its length to "
	         "EGO_MAX_PACKET_LEN, so a packet_len the request handler did "
	         "not set would write down the stack", path, s.do_rx_line);
	mu_assert(s.do_rx_clamp > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: do_rx()'s loop does not run to the clamped length",
	         path, s.do_rx_line);
	mu_assert(s.do_rx_loop > s.do_rx_clamp, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: do_rx() still has a loop bounded by packet_len itself",
	         path, s.unclamped_loop);
	mu_assert_int_eq(0, s.unclamped_loop);
}

/* The host tool's range and the firmware's have to be the same number, or
 * `ubertooth-ego -l 48` would be refused by the board. */
MU_TEST(test_the_host_tool_offers_the_range_the_firmware_takes)
{
	char line[SCAN_LINE_MAX], flat[SCAN_LINE_MAX], msg[640];
	const char *host = HOST_EGO_OPTIONS;
	FILE *f = fopen(host, "r");
	int in_block = 0, min_ok = 0, max_ok = 0;

	snprintf(msg, sizeof(msg), "cannot open %s to scan", host);
	mu_assert(f != NULL, msg);

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));
		if (strcmp(flat, "#define EGO_PACKET_LENGTH_MIN 1") == 0)
			min_ok = 1;
		if (strcmp(flat, "#define EGO_PACKET_LENGTH_MAX 48") == 0)
			max_ok = 1;
	}
	fclose(f);

	snprintf(msg, sizeof(msg),
	         "%s: EGO_PACKET_LENGTH_MIN/MAX are not 1 and 48, so the tool "
	         "and the firmware disagree about the capture lengths a board "
	         "takes", host);
	mu_assert(min_ok && max_ok, msg);
	mu_assert_int_eq(48, EGO_MAX_PACKET_LEN);
}

int main(void)
{
	printf("e-go receive buffer bounds tests\n");
	MU_RUN(test_a_length_in_range_is_accepted);
	MU_RUN(test_a_length_one_over_the_limit_is_refused);
	MU_RUN(test_the_longest_possible_length_is_refused);
	MU_RUN(test_a_length_of_zero_is_refused);
	MU_RUN(test_a_body_of_the_wrong_length_is_refused);
	MU_RUN(test_a_capture_at_the_limit_stays_in_the_buffer);
	MU_RUN(test_a_capture_longer_than_the_buffer_is_clamped);
	MU_RUN(test_the_old_unbounded_loop_ran_past_the_buffer);
	MU_RUN(test_the_buffer_is_the_size_the_record_copies);
	MU_RUN(test_the_record_copy_stays_in_the_buffer);
	MU_RUN(test_the_old_record_copy_read_the_timestamp);

	/* Scanned once here rather than from the first of the tests below,
	 * which would leave them depending on the order they run in. */
	scanned = scan_source(path, &s);
	MU_RUN(test_source_was_scanned);
	MU_RUN(test_the_buffer_is_declared_at_the_record_size);
	MU_RUN(test_the_request_refuses_before_it_sets_the_length);
	MU_RUN(test_the_receive_loop_is_clamped);
	MU_RUN(test_the_host_tool_offers_the_range_the_firmware_takes);
	return mu_report();
}
