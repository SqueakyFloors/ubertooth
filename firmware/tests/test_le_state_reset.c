/*
 * What reset_le() clears, field by field.
 *
 * The low-energy modes keep one struct, le_state_t, and reset_le() puts it
 * back to its advertising-channel starting point: at the start of a mode, and
 * again whenever a followed connection dies, so the next connection does not
 * inherit the last one's channel index, hop increment, CRCInit or timing. A
 * field left out is a field the next connection starts with the wrong value
 * of, and nothing shows that at the time -- the capture simply decodes badly.
 *
 * The function is in a hardware file and cannot run here, so this reads the
 * struct out of bluetooth_le.h and reset_le() out of bluetooth_rxtx.c, and
 * checks that every field of the one is accounted for in the other: either
 * cleared, or named below as one the host configured and that must survive.
 * Adding a field to the struct and forgetting it here fails this test.
 *
 * It was worth writing because reset_le() cleared le.conn_interval twice,
 * which read like a line meant for some other field.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

#ifndef FW_RXTX_DIR
#error "FW_RXTX_DIR must name firmware/bluetooth_rxtx"
#endif

/*
 * The fields reset_le() deliberately leaves alone: the follow flag and the
 * address filter. It runs in the middle of a mode, so clearing these would
 * turn a targeted capture into an untargeted one as soon as a connection
 * ended.
 *
 * crc_verify is host-settable as well and is not among them: the firmware
 * turns it on and off itself while it recovers a connection's CRCInit, so a
 * new connection has to start without it, and ubertooth-btle applies the -v
 * options after the mode has started for that reason.
 */
static const char *const kept_fields[] = {
	"do_follow",      /* UBERTOOTH_BTLE_PROMISC's follow flag */
	"target",         /* the address filter, and the two below it */
	"target_mask",
	"target_set",
};

/*
 * The fields reset_le() clears through le_set_access_address() rather than by
 * name: that helper sets the access address and the two syncword halves the
 * radio matches on, which is why the struct's own names do not appear.
 */
static const char *const fields_via_access_address[] = {
	"access_address",
	"synch",
	"syncl",
};

/* le_state_t's fields, in declaration order. */
#define MAX_FIELDS 64
static char fields[MAX_FIELDS][64];
static int field_count;

/* reset_le()'s body, comments and literals removed, spaces collapsed. */
static char body[8000];

static FILE *open_source(const char *dir, const char *name)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "r");
	if (f == NULL) {
		/* Every check below reads this text, so a file that cannot be
		 * read must stop the test rather than leave it checking
		 * nothing. */
		fprintf(stderr, "cannot open %s\n", path);
		exit(1);
	}
	return f;
}

/*
 * The field name a struct member declaration ends in: the last identifier
 * before the ';', with any array bound dropped. "u8 target[6];" gives
 * "target", "u16 volatile interval_timer;" gives "interval_timer".
 */
static int member_name(const char *text, char *out, size_t size)
{
	const char *semi = strchr(text, ';');
	const char *end, *start;

	if (semi == NULL)
		return 0;
	end = semi;
	/* Drop an array bound, then any spaces in front of the ';'. */
	if (end > text && end[-1] == ']') {
		while (end > text && end[-1] != '[')
			end--;
		if (end == text)
			return 0;
		end--;              /* the '[' itself */
	}
	while (end > text && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	start = end;
	while (start > text && is_ident_char(start[-1]))
		start--;
	if (start == end || (size_t)(end - start) >= size)
		return 0;
	memcpy(out, start, (size_t)(end - start));
	out[end - start] = '\0';
	return 1;
}

/* Read le_state_t's fields out of bluetooth_le.h. */
static void load_fields(void)
{
	FILE *f = open_source(FW_RXTX_DIR, "bluetooth_le.h");
	char line[SCAN_LINE_MAX];
	char clean[SCAN_LINE_MAX];
	char name[64];
	int in_block = 0;
	int inside = 0;

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		strip_string_literals(line);
		collapse_spaces(line, clean, sizeof(clean));
		if (clean[0] == '\0')
			continue;
		if (!inside) {
			if (strstr(clean, "typedef struct _le_state_t {") != NULL)
				inside = 1;
			continue;
		}
		if (strstr(clean, "} le_state_t;") != NULL)
			break;
		if (!member_name(clean, name, sizeof(name)))
			continue;
		if (field_count < MAX_FIELDS)
			snprintf(fields[field_count], sizeof(fields[0]), "%s", name);
		field_count++;
	}
	fclose(f);
	if (field_count > MAX_FIELDS) {
		fprintf(stderr, "le_state_t has more fields than this test holds\n");
		exit(1);
	}
}

/* Read reset_le()'s body out of bluetooth_rxtx.c. */
static void load_reset_body(void)
{
	FILE *f = open_source(FW_RXTX_DIR, "bluetooth_rxtx.c");
	char line[SCAN_LINE_MAX];
	char clean[SCAN_LINE_MAX];
	int in_block = 0;
	int inside = 0;
	size_t used = 0;

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		strip_string_literals(line);
		collapse_spaces(line, clean, sizeof(clean));
		if (clean[0] == '\0')
			continue;
		if (!inside) {
			if (strstr(clean, "void reset_le()") != NULL)
				inside = 1;
			continue;
		}
		/* The body's closing brace, which is a line of its own. */
		if (strcmp(clean, "}") == 0)
			break;
		used += (size_t)snprintf(body + used, sizeof(body) - used,
		                         "%s ", clean);
		if (used >= sizeof(body)) {
			fprintf(stderr, "reset_le() is longer than this test holds\n");
			exit(1);
		}
	}
	fclose(f);
}

static int in_list(const char *const *list, size_t count, const char *name)
{
	size_t i;

	for (i = 0; i < count; i++)
		if (strcmp(list[i], name) == 0)
			return 1;
	return 0;
}

/* Both files were found and read, or every check below would pass on nothing. */
MU_TEST(the_struct_and_the_reset_were_both_read)
{
	mu_assert(field_count > 10, "le_state_t's fields were not found");
	mu_assert(strstr(body, "le_set_access_address(") != NULL,
	          "reset_le()'s body was not found");
}

/*
 * Every field of le_state_t is either cleared by reset_le(), set by the access
 * address helper it calls, or one of the host's own settings that must survive
 * a connection ending.
 */
MU_TEST(every_field_is_accounted_for)
{
	char msg[512];
	char assignment[80];
	int i;

	for (i = 0; i < field_count; i++) {
		const char *name = fields[i];

		if (in_list(kept_fields, sizeof(kept_fields) / sizeof(kept_fields[0]),
		            name))
			continue;
		if (in_list(fields_via_access_address,
		            sizeof(fields_via_access_address) /
		            sizeof(fields_via_access_address[0]), name))
			continue;
		snprintf(assignment, sizeof(assignment), "le.%s", name);
		snprintf(msg, sizeof(msg),
		         "le_state_t.%s is neither cleared by reset_le() nor "
		         "listed in this test as one to keep, so the next "
		         "connection would inherit it", name);
		mu_assert(is_assignment_to(body, assignment), msg);
	}
}

/*
 * The host's own settings are not cleared. reset_le() runs when a followed
 * connection dies, so a capture that was told to follow one address would
 * quietly start following every address.
 */
MU_TEST(the_hosts_own_settings_survive)
{
	char msg[512];
	char assignment[80];
	size_t i;

	for (i = 0; i < sizeof(kept_fields) / sizeof(kept_fields[0]); i++) {
		snprintf(assignment, sizeof(assignment), "le.%s", kept_fields[i]);
		snprintf(msg, sizeof(msg),
		         "reset_le() clears le_state_t.%s, which the host set: a "
		         "connection ending would undo it", kept_fields[i]);
		mu_assert(!is_assignment_to(body, assignment), msg);
		/* An address filter is cleared with memset() rather than an
		 * assignment, so look for that too. */
		snprintf(msg, sizeof(msg),
		         "reset_le() memsets le_state_t.%s, which the host set",
		         kept_fields[i]);
		snprintf(assignment, sizeof(assignment), "memset(le.%s,",
		         kept_fields[i]);
		mu_assert(strstr(body, assignment) == NULL, msg);
	}
}

/* No field is cleared twice. A duplicate is harmless in itself, which is why
 * `le.conn_interval = 0;` sat there twice: it reads like a line meant for
 * another field, and the only way to tell is to count. */
MU_TEST(no_field_is_cleared_twice)
{
	char msg[512];
	char needle[80];
	int i;

	for (i = 0; i < field_count; i++) {
		const char *p = body;
		int seen = 0;

		snprintf(needle, sizeof(needle), "le.%s = ", fields[i]);
		while ((p = strstr(p, needle)) != NULL) {
			seen++;
			p += strlen(needle);
		}
		snprintf(msg, sizeof(msg),
		         "reset_le() assigns le_state_t.%s %d times; one of them "
		         "was probably meant for another field", fields[i], seen);
		mu_assert(seen <= 1, msg);
	}
}

int main(void)
{
	printf("LE state reset tests\n");
	load_fields();
	load_reset_body();
	MU_RUN(the_struct_and_the_reset_were_both_read);
	MU_RUN(every_field_is_accounted_for);
	MU_RUN(the_hosts_own_settings_survive);
	MU_RUN(no_field_is_cleared_twice);
	return mu_report();
}
