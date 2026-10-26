/*
 * Bounds on the data a vendor request carries, in both directions: how much of
 * the shared buffer a handler may read, and how much of it a reply may claim.
 *
 * Two things go wrong when a request handler reads more of the shared request
 * buffer than the request put there.
 *
 * The access code subcommand builds a 32-bit sync word from a body of one to
 * four bytes, and bounded its loop with the length of the *previous* code, so
 * a short code picked up whatever was left in the buffer behind it.
 *
 * More generally, the USB stack calls the vendor request handler at two
 * different points (USB 2.0 9.3, and USBHandleControlTransfer() in the
 * vendored lpcusb): for a host-to-device request it waits for the whole data
 * stage and calls once, which is what a handler that reads its data assumes,
 * but for a device-to-host request it calls at SETUP time, before any data
 * stage. A read-direction request of a number that expects data therefore ran
 * its handler against the previous request's bytes. The handler could not tell:
 * it was given wValue, wIndex and wLength but not the direction, and wLength
 * looks the same either way.
 *
 * The other direction has the same shape. The request that reads every CC2400
 * register fills the buffer with one register per three bytes and then reports
 * how long the reply is; the two came from separate expressions, and the length
 * claimed one register fewer than the fill wrote, so the host never saw the
 * last register.
 *
 * Neither the handler nor the stack can run here, so the fakes below are the
 * shape of both -- the dispatch rule the specification lays down, a request
 * buffer that keeps the previous request's bytes, two requests that read their
 * data, and the register dump's fill -- and the scans at the end read the
 * firmware source, the way test_tx_generic_bounds.c does, so the fakes cannot
 * drift away from it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"
#include "cc2400.h"

/* abVendorReqData in the two ubertooth_usb.c files: the buffer the stack fills
 * from the data stage and hands to every vendor request, kept across requests. */
#define VENDOR_REQ_BUF 258

/*
 * USB 2.0 9.3.1: bit 7 of bmRequestType is the data transfer direction, clear
 * for host-to-device. The last scan checks lpcusb spells it the same way.
 */
#define BM_REQUEST_TYPE_DIR 0x80
#define REQ_OUT (0x40)                       /* vendor, host to device */
#define REQ_IN  (0x40 | BM_REQUEST_TYPE_DIR) /* vendor, device to host */

/* -------------------------------------------------------------------------
 * The access code subcommand (RFCAT_SET_AA)
 * ------------------------------------------------------------------------- */

/* The code the request asks for: the body's bytes are the most significant of
 * the 32-bit sync word, and a code shorter than four bytes leaves the rest
 * zero. This is the contract, written out rather than built in a loop. */
static uint32_t expected_access_code(const uint8_t *body, int body_len)
{
	uint32_t code = 0;
	int i;

	for (i = 0; i < body_len; i++)
		code |= (uint32_t)body[i] << (8 * (3 - i));
	return code;
}

/* The firmware's loop, bounded by the body it was given. */
static uint32_t build_access_code(const uint8_t *body, int body_len)
{
	uint32_t code = 0;
	int i;

	for (i = 0; i < 4; ++i) {
		code <<= 8;
		if (i < body_len)
			code |= body[i];
	}
	return code;
}

/* The same loop bounded by the length of the previous code, as it was. */
static uint32_t old_build_access_code(const uint8_t *body, int previous_len)
{
	uint32_t code = 0;
	int i;

	for (i = 0; i < 4; ++i) {
		code <<= 8;
		if (i < previous_len)
			code |= body[i];
	}
	return code;
}

static uint8_t req_buf[VENDOR_REQ_BUF];

/* The buffer as a previous request left it, with `body` written over the front. */
static void put_body(const uint8_t *body, int body_len)
{
	memset(req_buf, 0xff, sizeof(req_buf));
	memcpy(req_buf, body, (size_t)body_len);
}

/* Every code the host can ask for is built from its own bytes. */
MU_TEST(test_an_access_code_is_built_from_its_own_body)
{
	static const uint8_t body[4] = { 0x11, 0x22, 0x33, 0x44 };
	int body_len;

	for (body_len = 1; body_len <= 4; body_len++) {
		char msg[128];

		put_body(body, body_len);
		snprintf(msg, sizeof(msg),
		         "a %d-byte access code was not built from its own "
		         "bytes", body_len);
		mu_assert(build_access_code(req_buf, body_len) ==
		              expected_access_code(body, body_len), msg);
	}
}

/* A four-byte code, the default and what most runs use, is the whole word. */
MU_TEST(test_a_four_byte_access_code_is_the_whole_sync_word)
{
	static const uint8_t body[4] = { 0x63, 0x0f, 0x9f, 0xfe };

	put_body(body, 4);
	mu_assert_int_eq(1, build_access_code(req_buf, 4) == 0x630f9ffeu);
}

/*
 * The bug: with the previous code's length as the bound, a shorter code took
 * its low bytes from whatever was behind the body in the buffer.
 */
MU_TEST(test_the_old_bound_read_past_a_short_body)
{
	static const uint8_t body[2] = { 0x11, 0x22 };

	put_body(body, 2);
	/* the previous request set a four-byte code */
	mu_assert_int_eq(1, old_build_access_code(req_buf, 4) == 0x1122ffffu);
	mu_assert_int_eq(1, build_access_code(req_buf, 2) == 0x11220000u);
}

/*
 * ...and only for a code shorter than the last one, which is why a run that
 * sets four bytes, or the same length twice, never saw it.
 */
MU_TEST(test_the_old_bound_was_right_when_the_length_did_not_shrink)
{
	static const uint8_t body[4] = { 0x11, 0x22, 0x33, 0x44 };
	int body_len;

	for (body_len = 1; body_len <= 4; body_len++) {
		put_body(body, body_len);
		mu_assert_int_eq(1, old_build_access_code(req_buf, body_len) ==
		                    build_access_code(req_buf, body_len));
	}
}

/* -------------------------------------------------------------------------
 * The dispatch rule, and a request that reads its data
 * ------------------------------------------------------------------------- */

/* State these requests set, so a refused one can be seen to have set none. */
static uint64_t target_address;
static uint16_t registers[0x100];
static uint8_t advertising[LE_ADV_MAX_LEN];
static int advertising_len = -1;

/*
 * Three of the requests that read their data stage. Two need a length -- an
 * address of eight bytes and a syncword of eight more, and a register number
 * plus a 16-bit value for each of wValue registers -- and the third takes
 * whatever it is given, including nothing, so only the direction tells a
 * request of it from a read of the same number.
 */
static int fake_request_handler(uint8_t request, const uint16_t *params,
                                const uint8_t *data, int data_from_host,
                                unsigned data_in_len)
{
	unsigned i;

	switch (request) {
	case UBERTOOTH_LE_SET_ADV_DATA:
		if (!data_from_host || data_in_len > LE_ADV_MAX_LEN)
			return 0;
		memcpy(advertising, data, data_in_len);
		advertising_len = (int)data_in_len;
		return 1;

	case UBERTOOTH_SET_BDADDR:
		if (data_in_len < 16)
			return 0;
		target_address = 0;
		for (i = 0; i < 8; i++)
			target_address |= (uint64_t)data[i] << (8 * i);
		return 1;

	case UBERTOOTH_WRITE_REGISTERS:
		if (data_in_len < params[0] * 3u)
			return 0;
		/* The firmware also bounds this count by the length of the
		 * buffer. That bound is not reproduced here, because what
		 * this case stands in for is the length check in front of
		 * it; test_write_registers_bounds.c covers the bound. */
		for (i = 0; i < params[0]; i++)
			registers[data[i * 3]] =
				(uint16_t)((data[i * 3 + 1] << 8) |
				           data[i * 3 + 2]);
		return 1;

	default:
		return 0;
	}
}

/* The same two without the length check, to show what the buffer held. */
static int unchecked_request_handler(uint8_t request, const uint16_t *params,
                                     const uint8_t *data, int data_from_host,
                                     unsigned data_in_len)
{
	(void)data_from_host;
	(void)data_in_len;
	switch (request) {
	case UBERTOOTH_SET_BDADDR:
	case UBERTOOTH_WRITE_REGISTERS:
		return fake_request_handler(request, params, data, 1, 0xffffu);
	default:
		return 0;
	}
}

typedef int (*handler_fn)(uint8_t request, const uint16_t *params,
                          const uint8_t *data, int data_from_host,
                          unsigned data_in_len);

/*
 * What the stack does with a vendor request, from the specification: a
 * host-to-device request with a data stage is handled once all of it has
 * arrived in the buffer, and anything else -- a read, or a request with no
 * data stage -- is handled straight away, with the buffer as the last request
 * left it. 1 accepts the request, 0 stalls it.
 */
static int control_transfer(handler_fn handler, uint8_t bmRequestType,
                            uint8_t request, uint16_t wValue,
                            const uint8_t *stage, uint16_t wLength)
{
	uint16_t params[3] = { wValue, 0, wLength };
	int data_from_host = (bmRequestType & BM_REQUEST_TYPE_DIR) == 0;
	unsigned data_in_len = 0;

	if (data_from_host && wLength > 0) {
		unsigned stored = wLength < VENDOR_REQ_BUF ? wLength
		                                           : VENDOR_REQ_BUF;

		memcpy(req_buf, stage, stored);
		data_in_len = wLength;
	}
	return handler(request, params, req_buf, data_from_host, data_in_len);
}

static void reset_state(void)
{
	memset(req_buf, 0, sizeof(req_buf));
	memset(registers, 0, sizeof(registers));
	memset(advertising, 0, sizeof(advertising));
	advertising_len = -1;
	target_address = 0;
}

/* A sixteen-byte address request from the host still does what it always did. */
MU_TEST(test_a_write_request_with_its_data_is_accepted)
{
	static const uint8_t body[16] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7
	};

	reset_state();
	mu_assert_int_eq(1, control_transfer(fake_request_handler, REQ_OUT,
	                                     UBERTOOTH_SET_BDADDR, 0, body,
	                                     sizeof(body)));
	mu_assert_int_eq(1, target_address == 0x0807060504030201ull);
}

/* A read of the same request number is refused, and sets nothing. */
MU_TEST(test_a_read_of_a_request_that_needs_data_is_refused)
{
	static const uint8_t body[16] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7
	};

	reset_state();
	/* a previous request leaves an address in the buffer */
	mu_assert_int_eq(1, control_transfer(fake_request_handler, REQ_OUT,
	                                     UBERTOOTH_SET_BDADDR, 0, body,
	                                     sizeof(body)));
	target_address = 0;

	/* the same number, read direction, declaring the same length */
	mu_assert_int_eq(0, control_transfer(fake_request_handler, REQ_IN,
	                                     UBERTOOTH_SET_BDADDR, 0, NULL,
	                                     sizeof(body)));
	mu_assert_int_eq(1, target_address == 0);
}

/* Without the length check, that read acted on the previous request's bytes. */
MU_TEST(test_without_the_check_a_read_acted_on_the_previous_request)
{
	static const uint8_t body[16] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7
	};

	reset_state();
	mu_assert_int_eq(1, control_transfer(unchecked_request_handler, REQ_OUT,
	                                     UBERTOOTH_SET_BDADDR, 0, body,
	                                     sizeof(body)));
	target_address = 0;

	mu_assert_int_eq(1, control_transfer(unchecked_request_handler, REQ_IN,
	                                     UBERTOOTH_SET_BDADDR, 0, NULL,
	                                     sizeof(body)));
	mu_assert(target_address == 0x0807060504030201ull,
	          "the unchecked handler should have taken the previous "
	          "request's address, so the test above is checking something");
}

/* A request that carries less than it reads is refused as well. */
MU_TEST(test_a_write_request_shorter_than_it_reads_is_refused)
{
	static const uint8_t body[16] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint16_t wLength;

	for (wLength = 0; wLength < 16; wLength++) {
		reset_state();
		mu_assert_int_eq(0, control_transfer(fake_request_handler,
		                                     REQ_OUT,
		                                     UBERTOOTH_SET_BDADDR, 0,
		                                     body, wLength));
		mu_assert_int_eq(1, target_address == 0);
	}
}

/* A register write needs three bytes per register it declares in wValue. */
MU_TEST(test_a_register_write_needs_three_bytes_per_register)
{
	static const uint8_t body[6] = { 0x10, 0x12, 0x34, 0x20, 0x56, 0x78 };

	reset_state();
	mu_assert_int_eq(1, control_transfer(fake_request_handler, REQ_OUT,
	                                     UBERTOOTH_WRITE_REGISTERS, 2,
	                                     body, sizeof(body)));
	mu_assert_int_eq(0x1234, registers[0x10]);
	mu_assert_int_eq(0x5678, registers[0x20]);

	/* one register's worth of data, two registers declared */
	reset_state();
	mu_assert_int_eq(0, control_transfer(fake_request_handler, REQ_OUT,
	                                     UBERTOOTH_WRITE_REGISTERS, 2,
	                                     body, 3));
	mu_assert_int_eq(0, registers[0x10]);

	/* and a read of the same number writes no register at all */
	reset_state();
	mu_assert_int_eq(0, control_transfer(fake_request_handler, REQ_IN,
	                                     UBERTOOTH_WRITE_REGISTERS, 2,
	                                     NULL, sizeof(body)));
	mu_assert_int_eq(0, registers[0x10]);
}

/*
 * A request whose body may be empty still works with an empty one: an
 * advertising payload of no bytes is something the host can ask for, and the
 * length alone cannot tell that apart from a read of the same number.
 */
MU_TEST(test_a_request_with_an_empty_body_is_accepted)
{
	static const uint8_t body[3] = { 0x02, 0x01, 0x05 };

	reset_state();
	mu_assert_int_eq(1, control_transfer(fake_request_handler, REQ_OUT,
	                                     UBERTOOTH_LE_SET_ADV_DATA, 0,
	                                     body, sizeof(body)));
	mu_assert_int_eq(3, advertising_len);

	mu_assert_int_eq(1, control_transfer(fake_request_handler, REQ_OUT,
	                                     UBERTOOTH_LE_SET_ADV_DATA, 0,
	                                     body, 0));
	mu_assert_int_eq(0, advertising_len);
}

/* A read of that number is still refused, and changes nothing. */
MU_TEST(test_a_read_of_a_request_with_an_empty_body_is_refused)
{
	static const uint8_t body[3] = { 0x02, 0x01, 0x05 };

	reset_state();
	mu_assert_int_eq(1, control_transfer(fake_request_handler, REQ_OUT,
	                                     UBERTOOTH_LE_SET_ADV_DATA, 0,
	                                     body, sizeof(body)));
	mu_assert_int_eq(0, control_transfer(fake_request_handler, REQ_IN,
	                                     UBERTOOTH_LE_SET_ADV_DATA, 0,
	                                     NULL, 0));
	mu_assert_int_eq(3, advertising_len);
}

/* -------------------------------------------------------------------------
 * The reply to a read of every register (UBERTOOTH_READ_ALL_REGISTERS)
 * ------------------------------------------------------------------------- */

/*
 * The registers the reply carries and how long that makes it, as the firmware
 * works them out: every address from MAIN (0x00) through SYNCH (0x2d), three
 * bytes each. The last scan checks the firmware spells both of these the same
 * way, from one count, so a reply cannot claim a different number of registers
 * than it filled.
 */
#define REPLY_REGISTERS (SYNCH + 1)
#define REPLY_LEN       (REPLY_REGISTERS * 3)

/* The length the handler used to report: one register short of the fill. */
#define OLD_REPLY_LEN ((REPLY_REGISTERS - 1) * 3)

/* A value per register, distinct from any neighbour's and from the 0xff the
 * buffer is filled with, so a byte from the wrong place is visible. */
static uint16_t fake_register_value(unsigned reg)
{
	return (uint16_t)(0x1000 + reg * 0x11);
}

/* The handler's fill: the register number and its 16-bit value, big end
 * first, for each of `count` registers. Returns the bytes written. */
static unsigned fill_register_reply(uint8_t *reply, unsigned count)
{
	unsigned reg;

	for (reg = 0; reg < count; reg++) {
		uint16_t reg_val = fake_register_value(reg);

		reply[reg * 3] = (uint8_t)reg;
		reply[reg * 3 + 1] = (reg_val >> 8) & 0xff;
		reply[reg * 3 + 2] = reg_val & 0xff;
	}
	return count * 3;
}

/* The reply the host gets: `claimed` bytes of the buffer and nothing else. */
MU_TEST(test_the_reply_claims_every_register_it_filled)
{
	unsigned written, reg;

	memset(req_buf, 0xff, sizeof(req_buf));
	written = fill_register_reply(req_buf, REPLY_REGISTERS);

	mu_assert_int_eq(46, REPLY_REGISTERS);
	mu_assert_int_eq(138, REPLY_LEN);
	mu_assert(REPLY_LEN == (int)written,
	          "the reply reports a different length than the fill wrote, "
	          "so the two are not coming from one count");

	for (reg = 0; reg < REPLY_REGISTERS; reg++) {
		char msg[160];
		uint16_t reg_val = fake_register_value(reg);

		snprintf(msg, sizeof(msg),
		         "register 0x%02x sits past the %d bytes the reply "
		         "claims, so the host never sees it", reg, REPLY_LEN);
		mu_assert((reg * 3 + 3) <= (unsigned)REPLY_LEN, msg);

		snprintf(msg, sizeof(msg),
		         "register 0x%02x is not in the reply where the host "
		         "looks for it", reg);
		mu_assert(req_buf[reg * 3] == reg &&
		          req_buf[reg * 3 + 1] == ((reg_val >> 8) & 0xff) &&
		          req_buf[reg * 3 + 2] == (reg_val & 0xff), msg);
	}
}

/* The bug: the length was three bytes short, so SYNCH was cut off the end. */
MU_TEST(test_the_old_length_dropped_the_last_register)
{
	memset(req_buf, 0xff, sizeof(req_buf));
	fill_register_reply(req_buf, REPLY_REGISTERS);

	mu_assert_int_eq(135, OLD_REPLY_LEN);
	/* the fill wrote 46 registers; the old length covered 45 of them */
	mu_assert_int_eq(REPLY_REGISTERS - 1, OLD_REPLY_LEN / 3);
	/* SYNCH's three bytes were in the buffer all along, behind that end */
	mu_assert_int_eq(SYNCH, req_buf[OLD_REPLY_LEN]);
	mu_assert_int_eq(fake_register_value(SYNCH) & 0xff,
	                 req_buf[OLD_REPLY_LEN + 2]);
}

/* The reply fits the buffer the stack hands the handler. */
MU_TEST(test_the_reply_fits_the_request_buffer)
{
	mu_assert(REPLY_LEN <= VENDOR_REQ_BUF,
	          "the register dump is longer than the request buffer, so the "
	          "fill runs past it");
}

/* -------------------------------------------------------------------------
 * The firmware's own shape
 * ------------------------------------------------------------------------- */

/*
 * Does `text` mention `data` as the request's buffer? Whole identifier, and
 * not a member of something else, so `f->data` and `data_in_len` do not count.
 */
static int mentions_request_data(const char *text)
{
	const char *p = text;

	while ((p = strstr(p, "data")) != NULL) {
		char prev = (p == text) ? ' ' : p[-1];

		if (!is_ident_char(prev) && prev != '.' && prev != '>' &&
		    !is_ident_char(p[4]))
			return 1;
		p += 4;
	}
	return 0;
}

/*
 * Is every `data[...]` on the line an assignment target, i.e. does the line
 * only write the reply? `data[0] = 1;` writes, `reg = data[0];` reads, and
 * `le.target[i] = data[5-i];` reads even though it assigns.
 */
static int only_writes_into_data(const char *text)
{
	const char *p = text;
	int found = 0;

	while ((p = strstr(p, "data[")) != NULL) {
		const char *q = strchr(p, ']');
		char prev = (p == text) ? ' ' : p[-1];

		if (is_ident_char(prev) || prev == '.' || prev == '>') {
			p += 5;
			continue;
		}
		if (q == NULL)
			return 0;
		q++;
		while (*q == ' ')
			q++;
		if (q[0] != '=' || q[1] == '=')
			return 0;
		found = 1;
		p += 5;
	}
	return found;
}

/*
 * The calls that fill `data` with a reply rather than reading the request.
 * Text cannot tell them from a read, so they are named here; a new request
 * that answers through `data` belongs in this list.
 */
static int writes_the_reply_through_data(const char *text)
{
	static const char *const calls[] = {
		"memcpy(data,",
		"memcpy(&data[",
		"get_part_num(data,",
		"get_device_serial(data,",
	};
	size_t i;

	for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++)
		if (strstr(text, calls[i]) != NULL)
			return 1;
	return 0;
}

/* One case label of a handler's switch. */
struct req_case {
	char name[64];
	int label_line;
	int first_read;   /* first line that reads the request's data */
	int length_seen;  /* first line that mentions data_in_len */
};

#define MAX_CASES 128

/* One image's request handler. */
struct handler_scan {
	const char *path;
	int scanned;
	int def_line;     /* the handler's own definition */
	int takes_len;    /* ...with data_in_len in its parameters */
	int takes_dir;    /* ...and the data stage direction */
	int case_count;
	struct req_case cases[MAX_CASES];
};

/* Walk a handler's switch, recording what each case does with `data`. */
static int scan_handler(struct handler_scan *h)
{
	char line[512], flat[512];
	int lineno = 0, in_block = 0, depth = 0, started = 0;
	FILE *f = fopen(h->path, "r");

	if (f == NULL)
		return 0;

	while (fgets(line, sizeof(line), f) != NULL) {
		const char *brace;
		struct req_case *c;

		lineno++;
		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));

		if (h->def_line == 0 &&
		    strstr(flat, "vendor_request_handler(uint8_t request,") !=
		    NULL) {
			h->def_line = lineno;
			continue;
		}
		if (h->def_line == 0)
			continue;
		if (!started) {
			if (strstr(flat, "unsigned data_in_len") != NULL)
				h->takes_len = lineno;
			if (strstr(flat, "int data_from_host,") != NULL)
				h->takes_dir = lineno;
		}

		for (brace = flat; *brace != '\0'; brace++) {
			if (*brace == '{') {
				depth++;
				started = 1;
			} else if (*brace == '}') {
				depth--;
			}
		}
		if (started && depth <= 0)
			break; /* end of the handler */
		if (depth == 0)
			continue;

		if (strncmp(flat, "case UBERTOOTH_", 15) == 0 ||
		    strncmp(flat, "default:", 8) == 0) {
			const char *end;

			if (h->case_count >= MAX_CASES) {
				fclose(f);
				return 0;
			}
			c = &h->cases[h->case_count++];
			memset(c, 0, sizeof(*c));
			c->label_line = lineno;
			end = strchr(flat, ':');
			snprintf(c->name, sizeof(c->name), "%.*s",
			         (int)(end != NULL ? end - flat
			                           : (long)strlen(flat)), flat);
			continue;
		}
		if (h->case_count == 0)
			continue;
		c = &h->cases[h->case_count - 1];

		if (c->length_seen == 0 &&
		    contains_identifier(flat, "data_in_len"))
			c->length_seen = lineno;
		if (c->first_read == 0 && mentions_request_data(flat) &&
		    !only_writes_into_data(flat) &&
		    !writes_the_reply_through_data(flat))
			c->first_read = lineno;
	}

	fclose(f);
	return 1;
}

/* Both images' handlers: the one that reads its requests' data, and the one
 * that does not, so the rule is enforced there before a request that does. */
static struct handler_scan handlers[] = {
	{ .path = FW_RXTX_DIR "/bluetooth_rxtx.c" },
	{ .path = FW_BTBR_DIR "/src/btctl/vendor_request_handler.c" },
};
#define HANDLER_COUNT (sizeof(handlers) / sizeof(handlers[0]))

/* The handler that carries the requests this test is about. */
#define RXTX (&handlers[0])

MU_TEST(test_the_handlers_were_scanned)
{
	char msg[512];
	int with_reads = 0, i;
	size_t n;

	for (n = 0; n < HANDLER_COUNT; n++) {
		const struct handler_scan *h = &handlers[n];

		snprintf(msg, sizeof(msg), "cannot open %s to scan", h->path);
		mu_assert(h->scanned, msg);

		snprintf(msg, sizeof(msg),
		         "%s: the scan did not find a vendor request handler, "
		         "so the checks below would pass without reading "
		         "anything", h->path);
		mu_assert(h->def_line > 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s:%d: the handler does not take the data stage "
		         "direction and `unsigned data_in_len`, so it cannot "
		         "tell how much of the buffer the host sent", h->path,
		         h->def_line);
		mu_assert(h->takes_len > 0 && h->takes_dir > 0, msg);
	}

	for (i = 0; i < RXTX->case_count; i++)
		if (RXTX->cases[i].first_read > 0)
			with_reads++;

	/* The requests that read their data today; a floor, not a ceiling, so
	 * the scan cannot pass by finding nothing. */
	snprintf(msg, sizeof(msg),
	         "%s: the scan found %d requests and %d of them reading their "
	         "data; the handler has at least a dozen that do, so the scan "
	         "is not reading the switch", RXTX->path, RXTX->case_count,
	         with_reads);
	mu_assert(RXTX->case_count > 50 && with_reads >= 12, msg);
}

/*
 * The rule: a request that reads its data stage has to know how much of it
 * arrived first, either by checking data_in_len or by passing it on with the
 * data. A read-direction request arrives with none, which is what keeps it
 * from running on the previous request's bytes.
 */
MU_TEST(test_every_request_that_reads_data_checks_its_length)
{
	size_t n;
	int i;

	for (n = 0; n < HANDLER_COUNT; n++) {
		const struct handler_scan *h = &handlers[n];

		for (i = 0; i < h->case_count; i++) {
			const struct req_case *c = &h->cases[i];
			char msg[640];

			if (c->first_read == 0)
				continue;

			snprintf(msg, sizeof(msg),
			         "%s:%d: %s reads the request's data at line %d "
			         "without checking data_in_len, so it acts on "
			         "whatever the previous request left in the "
			         "buffer when the host sends too little -- or "
			         "nothing at all, as a device-to-host request "
			         "does", h->path, c->label_line, c->name,
			         c->first_read);
			mu_assert(c->length_seen > 0, msg);

			snprintf(msg, sizeof(msg),
			         "%s:%d: %s reads the request's data at line %d "
			         "but only reaches data_in_len at line %d, "
			         "after the read", h->path, c->label_line,
			         c->name, c->first_read, c->length_seen);
			mu_assert(c->length_seen <= c->first_read, msg);
		}
	}
}

/*
 * Where data_in_len comes from: the stack's own setup packet, zero unless the
 * request is host-to-device. Scanned in both images' USB shims, which is the
 * one place that knows the direction.
 */
static int scan_shim(const char *path, int *dir_check, int *zero, int *passed)
{
	char line[512], flat[512];
	int lineno = 0, in_block = 0;
	FILE *f = fopen(path, "r");

	*dir_check = *zero = *passed = 0;
	if (f == NULL)
		return 0;

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));

		if (strstr(flat, "REQTYPE_GET_DIR(pSetup->bmRequestType) == "
		                 "REQTYPE_DIR_TO_DEVICE") != NULL)
			*dir_check = lineno;
		if (strstr(flat, "data_in_len = data_from_host ? "
		                 "pSetup->wLength : 0") != NULL)
			*zero = lineno;
		if (strstr(flat, "v_req_handler(pSetup->bRequest, params, "
		                 "*ppbData, data_from_host,") != NULL)
			*passed = lineno;
	}

	fclose(f);
	return 1;
}

MU_TEST(test_the_usb_shims_zero_the_length_for_a_read)
{
	static const char *const shims[] = {
		FW_RXTX_DIR "/ubertooth_usb.c",
		FW_BTBR_DIR "/src/drivers/ubertooth_usb.c",
	};
	size_t i;

	for (i = 0; i < sizeof(shims) / sizeof(shims[0]); i++) {
		int dir_check, zero, passed;
		char msg[640];

		snprintf(msg, sizeof(msg), "cannot open %s to scan", shims[i]);
		mu_assert(scan_shim(shims[i], &dir_check, &zero, &passed), msg);

		snprintf(msg, sizeof(msg),
		         "%s: the shim does not take the data stage direction "
		         "from bmRequestType, so every request would look like "
		         "one carrying data", shims[i]);
		mu_assert(dir_check > 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s: the shim does not pass wLength for a "
		         "host-to-device request and zero otherwise", shims[i]);
		mu_assert(zero > 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s: the shim does not hand the direction and the "
		         "length to the request handler", shims[i]);
		mu_assert(passed > 0, msg);
	}
}

/*
 * The direction bit the fakes above use is the one lpcusb reads, which is the
 * one the specification defines. Scanned rather than included so this test
 * stays a single translation unit.
 */
MU_TEST(test_lpcusb_spells_the_direction_bit_as_the_specification_does)
{
	static const char *const path =
		FW_COMMON_DIR "/lpcusb/target/usbstruct.h";
	char line[512], flat[512];
	int in_block = 0, get_dir = 0, to_device = 0;
	FILE *f = fopen(path, "r");
	char msg[512];

	snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
	mu_assert(f != NULL, msg);

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));
		if (strstr(flat, "#define REQTYPE_GET_DIR(x) (((x)>>7)&0x01)")
		    != NULL)
			get_dir = 1;
		if (strstr(flat, "#define REQTYPE_DIR_TO_DEVICE 0") != NULL)
			to_device = 1;
	}
	fclose(f);

	snprintf(msg, sizeof(msg),
	         "%s: REQTYPE_GET_DIR is not bit 7 of bmRequestType, so the "
	         "direction the fakes here use is not the one the firmware "
	         "reads", path);
	mu_assert(get_dir, msg);

	snprintf(msg, sizeof(msg),
	         "%s: REQTYPE_DIR_TO_DEVICE is not 0, so a host-to-device "
	         "request is not what the shim thinks it is", path);
	mu_assert(to_device, msg);
}

/* The access code loop, bounded by the body it was handed. */
MU_TEST(test_the_access_code_loop_is_bounded_by_its_body)
{
	static const char *const path = FW_RXTX_DIR "/ego.c";
	char line[512], flat[512];
	int lineno = 0, in_block = 0, in_case = 0;
	int bound = 0, old_bound = 0, guard = 0;
	FILE *f = fopen(path, "r");
	char msg[640];

	snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
	mu_assert(f != NULL, msg);

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));

		if (strstr(flat, "case RFCAT_SET_AA:") != NULL) {
			in_case = 1;
			continue;
		}
		if (!in_case)
			continue;
		if (strstr(flat, "body_len < 1 || body_len > 4") != NULL)
			guard = lineno;
		if (strstr(flat, "if (i < body_len)") != NULL)
			bound = lineno;
		if (strstr(flat, "if (i < access_code_len)") != NULL)
			old_bound = lineno;
		if (strstr(flat, "break;") != NULL)
			in_case = 0;
	}
	fclose(f);

	snprintf(msg, sizeof(msg),
	         "%s: the access code subcommand does not refuse a body of "
	         "other than one to four bytes", path);
	mu_assert(guard > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the access code loop is bounded by access_code_len, "
	         "the length of the previous code, so a shorter one is built "
	         "partly from the bytes behind it in the buffer", path,
	         old_bound);
	mu_assert(old_bound == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the access code loop is not bounded by body_len, so the "
	         "fakes above are not the loop the firmware runs", path);
	mu_assert(bound > 0 && bound > guard, msg);
}

/*
 * The register dump's fill and the length it reports, both from one count. The
 * two were separate expressions and disagreed, so what the scan looks for is
 * that neither spells a number of its own: the loop walks
 * READ_ALL_REGISTERS_COUNT registers and the reply reports
 * READ_ALL_REGISTERS_LEN, which is that count times three.
 */
MU_TEST(test_the_register_dump_reports_the_length_it_filled)
{
	static const char *const path = FW_RXTX_DIR "/bluetooth_rxtx.c";
	char line[512], flat[512];
	int lineno = 0, in_block = 0, in_case = 0;
	int count_def = 0, len_def = 0, bound = 0, reported = 0, old_name = 0;
	int wrote_number = 0, wrote_high = 0, wrote_low = 0;
	FILE *f = fopen(path, "r");
	char msg[640];

	snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
	mu_assert_fatal(f != NULL, msg);

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));

		if (strcmp(flat, "#define READ_ALL_REGISTERS_COUNT (SYNCH + 1)")
		    == 0)
			count_def = lineno;
		if (strcmp(flat, "#define READ_ALL_REGISTERS_LEN "
		                 "(READ_ALL_REGISTERS_COUNT * 3)") == 0)
			len_def = lineno;
		if (contains_identifier(flat, "MAX_READ_REG"))
			old_name = lineno;

		if (strstr(flat, "case UBERTOOTH_READ_ALL_REGISTERS:") != NULL) {
			in_case = 1;
			continue;
		}
		if (!in_case)
			continue;
		if (strcmp(flat, "for (i = 0; i < READ_ALL_REGISTERS_COUNT; "
		                 "i++) {") == 0)
			bound = lineno;
		/* the three bytes each register takes, in the order the host
		 * reads them: the register number, then its value */
		if (strcmp(flat, "data[i*3] = i;") == 0)
			wrote_number = lineno;
		if (strcmp(flat, "data[(i*3)+1] = (reg_val >> 8) & 0xff;") == 0)
			wrote_high = lineno;
		if (strcmp(flat, "data[(i*3)+2] = reg_val & 0xff;") == 0)
			wrote_low = lineno;
		if (strcmp(flat, "*data_len = READ_ALL_REGISTERS_LEN;") == 0)
			reported = lineno;
		if (strcmp(flat, "break;") == 0)
			in_case = 0;
	}
	fclose(f);

	snprintf(msg, sizeof(msg),
	         "%s: READ_ALL_REGISTERS_COUNT is not defined as (SYNCH + 1), "
	         "so the registers the test above fills are not the ones the "
	         "firmware reads", path);
	mu_assert(count_def > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: READ_ALL_REGISTERS_LEN is not three bytes for each of "
	         "READ_ALL_REGISTERS_COUNT registers, so the reply's length "
	         "does not follow from the count", path);
	mu_assert(len_def > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: MAX_READ_REG is back; the reply's length came from it "
	         "while the loop ran one register further, which is how the "
	         "last register went missing", path, old_name);
	mu_assert(old_name == 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the register dump's loop is not bounded by "
	         "READ_ALL_REGISTERS_COUNT", path);
	mu_assert(bound > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the register dump does not report READ_ALL_REGISTERS_LEN "
	         "as its length, so it can claim a length its fill did not "
	         "write", path);
	mu_assert(reported > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the register dump does not write the three bytes per "
	         "register the length counts on -- the number at i*3 and the "
	         "two value bytes after it -- so the reply can claim a length "
	         "the fill did not write even from the right count", path);
	mu_assert(wrote_number > 0 && wrote_high > 0 && wrote_low > 0, msg);
}

int main(void)
{
	int i;

	printf("vendor request data bounds tests\n");
	MU_RUN(test_an_access_code_is_built_from_its_own_body);
	MU_RUN(test_a_four_byte_access_code_is_the_whole_sync_word);
	MU_RUN(test_the_old_bound_read_past_a_short_body);
	MU_RUN(test_the_old_bound_was_right_when_the_length_did_not_shrink);

	MU_RUN(test_a_write_request_with_its_data_is_accepted);
	MU_RUN(test_a_read_of_a_request_that_needs_data_is_refused);
	MU_RUN(test_without_the_check_a_read_acted_on_the_previous_request);
	MU_RUN(test_a_write_request_shorter_than_it_reads_is_refused);
	MU_RUN(test_a_register_write_needs_three_bytes_per_register);
	MU_RUN(test_a_request_with_an_empty_body_is_accepted);
	MU_RUN(test_a_read_of_a_request_with_an_empty_body_is_refused);

	MU_RUN(test_the_reply_claims_every_register_it_filled);
	MU_RUN(test_the_old_length_dropped_the_last_register);
	MU_RUN(test_the_reply_fits_the_request_buffer);

	/* Scanned once here rather than from the first test that needs it,
	 * which would leave the others depending on the order they run in. */
	for (i = 0; i < (int)HANDLER_COUNT; i++)
		handlers[i].scanned = scan_handler(&handlers[i]);
	MU_RUN(test_the_handlers_were_scanned);
	MU_RUN(test_every_request_that_reads_data_checks_its_length);
	MU_RUN(test_the_usb_shims_zero_the_length_for_a_read);
	MU_RUN(test_lpcusb_spells_the_direction_bit_as_the_specification_does);
	MU_RUN(test_the_access_code_loop_is_bounded_by_its_body);
	MU_RUN(test_the_register_dump_reports_the_length_it_filled);
	return mu_report();
}
