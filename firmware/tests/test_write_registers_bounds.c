/*
 * The bound on a multi-register write (UBERTOOTH_WRITE_REGISTERS).
 *
 * The request carries a register number and a 16-bit value for each register,
 * and the number of registers in wValue, a 16-bit field. The handler used to
 * walk that count in a uint8_t, so a count above 255 wrapped at the top of the
 * counter and the condition was true again: the loop never ended, writing
 * CC2400 registers from the request buffer until the board was power-cycled.
 * No host tool sends the request, but any program on the bus can.
 *
 * The handler is hardware code -- it writes radio registers -- so the fake
 * below is the shape of its case, over a request buffer with a guard region
 * behind it, and the last tests scan the firmware source, the way
 * test_tx_generic_bounds.c scans the generic transmit request, so the fake
 * cannot drift away from what the firmware does.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

/* VENDOR_REQ_DATA_LEN in firmware/bluetooth_rxtx/ubertooth_usb.h: the buffer
 * the USB stack hands the handler. Repeated here because the header needs the
 * firmware's hardware headers; the scans below check the two agree. */
#define VENDOR_REQ_DATA_LEN 258

/* Three bytes per register, so this is what the buffer holds. */
#define WRITE_REGISTERS_MAX (VENDOR_REQ_DATA_LEN / 3)

/*
 * The request buffer with the bytes that follow it in the image standing in
 * behind it. Reads go through request_byte(), which records how far they got,
 * so a read past the buffer is visible rather than being someone else's bytes.
 */
#define GUARD_LEN 1024

static struct {
	uint8_t request[VENDOR_REQ_DATA_LEN];
	uint8_t behind[GUARD_LEN];
} buffer;

static unsigned highest_byte_read;

/* Register writes the request made: the number and the value, in order. */
static struct {
	uint8_t reg;
	uint16_t val;
} writes[GUARD_LEN];
static unsigned write_count;

/* The byte the test puts at index `i` of the buffer: never zero, so a value
 * built from a byte past the buffer does not look like one from inside it. */
static uint8_t filler(unsigned i)
{
	return (uint8_t)(i + 1);
}

static void reset_state(void)
{
	unsigned i;

	memset(&buffer, 0, sizeof(buffer));
	for (i = 0; i < VENDOR_REQ_DATA_LEN; i++)
		buffer.request[i] = filler(i);
	highest_byte_read = 0;
	write_count = 0;
}

static uint8_t request_byte(unsigned index)
{
	if (index > highest_byte_read)
		highest_byte_read = index;
	return ((uint8_t *)&buffer)[index];
}

static void cc2400_set(uint8_t reg, uint16_t val)
{
	if (write_count < GUARD_LEN) {
		writes[write_count].reg = reg;
		writes[write_count].val = val;
	}
	++write_count;
}

/* What the handler's case does now. 1 accepts the request, 0 stalls it. */
static int write_registers_request(uint16_t count)
{
	uint16_t request_params[3] = { count, 0, 0 };
	uint16_t reg_val;

	if (request_params[0] > WRITE_REGISTERS_MAX)
		return 0;
	for (unsigned reg = 0; reg < request_params[0]; reg++) {
		reg_val = (request_byte((reg * 3) + 1) << 8) |
		          request_byte((reg * 3) + 2);
		cc2400_set(request_byte(reg * 3), reg_val);
	}
	return 1;
}

/* The limit is what the buffer holds, with no room for one more register. */
MU_TEST(test_the_limit_is_what_the_request_buffer_holds)
{
	mu_assert_int_eq(86, WRITE_REGISTERS_MAX);
	mu_assert(WRITE_REGISTERS_MAX * 3 <= VENDOR_REQ_DATA_LEN,
	          "the registers at the limit do not fit the request buffer");
	mu_assert((WRITE_REGISTERS_MAX + 1) * 3 > VENDOR_REQ_DATA_LEN,
	          "one register more would still fit, so the limit is below "
	          "what the buffer holds");
}

/* A count at the limit is written whole, and reads only the buffer. */
MU_TEST(test_a_count_at_the_limit_is_accepted)
{
	unsigned i;

	reset_state();
	mu_assert_int_eq(1, write_registers_request(WRITE_REGISTERS_MAX));
	mu_assert_int_eq(WRITE_REGISTERS_MAX, (int)write_count);
	for (i = 0; i < WRITE_REGISTERS_MAX; i++) {
		mu_assert_int_eq(filler(i * 3), writes[i].reg);
		mu_assert_int_eq((filler(i * 3 + 1) << 8) | filler(i * 3 + 2),
		                 writes[i].val);
	}
	mu_assert(highest_byte_read < VENDOR_REQ_DATA_LEN,
	          "a count at the limit read past the request buffer");
}

/* One register more is refused, and writes nothing. */
MU_TEST(test_a_count_one_over_the_limit_is_refused)
{
	reset_state();
	mu_assert_int_eq(0, write_registers_request(WRITE_REGISTERS_MAX + 1));
	mu_assert_int_eq(0, (int)write_count);
	mu_assert_int_eq(0, (int)highest_byte_read);
}

/*
 * The count that used to wrap the counter: 256 registers. It is refused like
 * any other count the buffer cannot hold, and the call returns at all, which
 * is the point.
 */
MU_TEST(test_the_count_that_wrapped_the_counter_is_refused)
{
	reset_state();
	mu_assert_int_eq(0, write_registers_request(256));
	mu_assert_int_eq(0, (int)write_count);
}

/*
 * Every count the 16-bit field can hold: the request either writes exactly
 * that many registers from inside the buffer, or is refused and writes
 * nothing. Nothing in between, and nothing that does not return.
 */
MU_TEST(test_no_count_writes_past_the_request_buffer)
{
	unsigned count, bad_count = 0;
	const char *bad = NULL;
	char msg[256];

	for (count = 0; count <= 0xffff && bad == NULL; count++) {
		reset_state();
		bad_count = count;
		if (write_registers_request((uint16_t)count)) {
			if (count > WRITE_REGISTERS_MAX)
				bad = "a count the buffer cannot hold was "
				      "accepted";
			else if (write_count != count)
				bad = "an accepted count wrote a different "
				      "number of registers";
			else if (count > 0 &&
			         highest_byte_read >= VENDOR_REQ_DATA_LEN)
				bad = "an accepted count read past the "
				      "request buffer";
		} else {
			if (count <= WRITE_REGISTERS_MAX)
				bad = "a count the buffer holds was refused";
			else if (write_count != 0)
				bad = "a refused count wrote registers";
		}
	}

	snprintf(msg, sizeof(msg), "count %u: %s", bad_count,
	         bad == NULL ? "" : bad);
	mu_assert(bad == NULL, msg);
}

/*
 * What the bound replaces, and why the loop counter is as wide as the count:
 * a uint8_t walking a count of 256 wraps to zero at the top of its range and
 * the condition is true again. Cut off here by a cap it must never reach, so
 * this test reports the loop rather than hanging in it.
 */
MU_TEST(test_the_old_narrow_counter_never_finished)
{
	unsigned count = 256, iterations = 0;
	uint8_t i;

	for (i = 0; i < count; i++)
		if (++iterations > 100000)
			break;

	mu_assert(iterations > 100000,
	          "a uint8_t counter should never have finished a count of "
	          "256, so the width of the counter above is checking "
	          "something");
}

/* ---- source scans ---- */

/*
 * The fake above is only worth something if the firmware has the same shape,
 * and the handler cannot run here. So: find the case, and check that it
 * refuses an impossible count before it writes anything, that the limit is
 * derived from the buffer the request lands in, and that the counter it walks
 * is as wide as the count.
 */
struct scan {
	int max_define;       /* WRITE_REGISTERS_MAX, from the buffer length */
	int buffer_len;       /* VENDOR_REQ_DATA_LEN, with the value here */
	int buffer_decl;      /* abVendorReqData, declared with that length */
	int case_line;        /* the handler's `case UBERTOOTH_WRITE_...:` */
	int count_check;      /* the count is at most WRITE_REGISTERS_MAX */
	int refusal;          /* the refusal that follows it */
	int loop;             /* the loop over the registers */
	int wide_counter;     /* ...walked in a counter as wide as the count */
	int write;            /* the register write inside it */
};

static void scan_line(const char *flat, int lineno, struct scan *s,
                      int *in_case)
{
	if (strstr(flat, "#define WRITE_REGISTERS_MAX (VENDOR_REQ_DATA_LEN / 3)")
	    != NULL)
		s->max_define = lineno;
	if (strstr(flat, "#define VENDOR_REQ_DATA_LEN 258") != NULL)
		s->buffer_len = lineno;
	if (strstr(flat, "abVendorReqData[VENDOR_REQ_DATA_LEN];") != NULL)
		s->buffer_decl = lineno;

	if (strstr(flat, "case UBERTOOTH_WRITE_REGISTERS:") != NULL) {
		s->case_line = lineno;
		*in_case = 1;
		return;
	}
	if (!*in_case)
		return;

	if (strstr(flat, "request_params[0] > WRITE_REGISTERS_MAX") != NULL)
		s->count_check = lineno;
	if (s->count_check > 0 && s->loop == 0 &&
	    strstr(flat, "return 0;") != NULL)
		s->refusal = lineno;
	if (strstr(flat, "reg < request_params[0];") != NULL) {
		s->loop = lineno;
		if (strstr(flat, "for (unsigned reg = 0;") != NULL)
			s->wide_counter = lineno;
	}
	if (strstr(flat, "cc2400_set(") != NULL)
		s->write = lineno;
	if (strstr(flat, "break;") != NULL)
		*in_case = 0;
}

static int scan_source(const char *path, struct scan *s, int *in_case)
{
	char line[512], flat[512];
	int lineno = 0, in_block = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));
		scan_line(flat, lineno, s, in_case);
	}
	fclose(f);
	return 1;
}

static struct scan s;
static int scanned;
static const char *rxtx = FW_RXTX_DIR "/bluetooth_rxtx.c";
static const char *usb_c = FW_RXTX_DIR "/ubertooth_usb.c";
static const char *usb_h = FW_RXTX_DIR "/ubertooth_usb.h";

MU_TEST(test_source_was_scanned)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "cannot open the firmware source to scan "
	         "(%s, %s, %s)", rxtx, usb_c, usb_h);
	mu_assert(scanned, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the multi-register write, so the "
	         "checks below would pass without reading anything", rxtx);
	mu_assert(s.case_line > 0, msg);
}

MU_TEST(test_the_limit_comes_from_the_request_buffer)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s: VENDOR_REQ_DATA_LEN is not 258, so the limit the fake "
	         "above checks is not the firmware's", usb_h);
	mu_assert(s.buffer_len > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: abVendorReqData is not declared as VENDOR_REQ_DATA_LEN "
	         "bytes, so the limit is no longer tied to the buffer the "
	         "request lands in", usb_c);
	mu_assert(s.buffer_decl > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: WRITE_REGISTERS_MAX is not defined as "
	         "VENDOR_REQ_DATA_LEN / 3, so the count is not bounded by the "
	         "registers the buffer can hold", rxtx);
	mu_assert(s.max_define > 0, msg);
}

MU_TEST(test_the_handler_refuses_before_it_writes)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s:%d: the multi-register write does not check its count "
	         "against WRITE_REGISTERS_MAX, so a count the buffer cannot "
	         "hold would be walked", rxtx, s.case_line);
	mu_assert(s.count_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the count check does not stall the request", rxtx,
	         s.count_check);
	mu_assert(s.refusal > s.count_check, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the scan did not find the loop over the registers",
	         rxtx, s.case_line);
	mu_assert(s.loop > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the loop over the registers comes before the check at "
	         "line %d or the refusal at line %d, so a request is written "
	         "before it is refused", rxtx, s.loop, s.count_check,
	         s.refusal);
	mu_assert(s.count_check < s.loop && s.refusal < s.loop, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the register write is not inside the bounded loop",
	         rxtx, s.write);
	mu_assert(s.write > s.loop, msg);
}

MU_TEST(test_the_counter_is_as_wide_as_the_count)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s:%d: the registers are not walked in an `unsigned` "
	         "counter. The count is a 16-bit field, so a counter narrower "
	         "than it wraps and the loop never ends", rxtx, s.loop);
	mu_assert(s.loop > 0 && s.wide_counter == s.loop, msg);
}

int main(void)
{
	int in_case = 0;

	printf("multi-register write bounds tests\n");
	MU_RUN(test_the_limit_is_what_the_request_buffer_holds);
	MU_RUN(test_a_count_at_the_limit_is_accepted);
	MU_RUN(test_a_count_one_over_the_limit_is_refused);
	MU_RUN(test_the_count_that_wrapped_the_counter_is_refused);
	MU_RUN(test_no_count_writes_past_the_request_buffer);
	MU_RUN(test_the_old_narrow_counter_never_finished);

	/* Scanned once here rather than from the first of the tests below,
	 * which would leave them depending on the order they run in. */
	scanned = scan_source(usb_h, &s, &in_case) &&
	          scan_source(usb_c, &s, &in_case) &&
	          scan_source(rxtx, &s, &in_case);
	MU_RUN(test_source_was_scanned);
	MU_RUN(test_the_limit_comes_from_the_request_buffer);
	MU_RUN(test_the_handler_refuses_before_it_writes);
	MU_RUN(test_the_counter_is_as_wide_as_the_count);
	return mu_report();
}
