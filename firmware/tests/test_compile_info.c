/*
 * The two text replies both firmware images carry: the build string and the
 * revision on its own.
 *
 * It is what ubertooth-util -V reports, so both images spell it the same way:
 * a board running either one describes its build in the same terms. (Which
 * image a board or a file holds, and which revision it was built from, come
 * from the firmware info block instead -- see test_fw_info.c.)
 *
 * Neither image's request handler can run here, so this test reads the
 * compile_info definition out of each image's source and checks both say the
 * same thing. The equality check pins the spelling, which is stricter than
 * anything needs: a reformat nothing would mind fails it too. That is the
 * direction to err in, because nothing here can build an image to look at.
 *
 * The rest is about the replies these strings go out in: a one-byte length and
 * then the characters, which the host reads at most 255 characters of. A longer
 * string has to be cut to 255, because a one-byte length cannot describe more.
 * The old code cast the length to a byte, which wrapped it modulo 256 and sent
 * that many characters, so a 300-character string reached the host as its
 * first 44 and a 256-character one as nothing. Both the build string and the
 * revision reply were written that way; neither is reachable with the
 * `git-<8 hex>` revisions this tree produces.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"

/*
 * The definition both images must carry, spaces collapsed. The literal pieces
 * matter to the host in this order: "ubertooth ", then the revision, then
 * " (" to end it.
 */
#define COMPILE_INFO_DEFINITION \
	"const char compile_info[] = \"ubertooth \" GIT_REVISION " \
	"\" (\" COMPILE_BY \"@\" COMPILE_HOST \") \" TIMESTAMP;"

/* The two sources that define it, and what to call them in a message. */
static const char *const source_paths[] = {
	FW_RXTX_DIR "/bluetooth_rxtx.c",
	FW_BTBR_DIR "/src/btctl/vendor_request_handler.c",
};
#define SOURCE_COUNT (int)(sizeof(source_paths) / sizeof(source_paths[0]))

/* abVendorReqData in the two ubertooth_usb.c files: the buffer a handler
 * writes its reply into. The last test reads the declared size out of both
 * rather than trusting this one. */
#define VENDOR_REQ_BUF 258

/*
 * The definition as the named source spells it, comments stripped and spaces
 * collapsed, or an empty string when the file has none. It is written over two
 * lines, so lines are joined from "const char compile_info[]" up to the ';'.
 * Returns 0 when the file will not open, so a moved source is reported as
 * that rather than as a definition that does not match.
 */
static int read_definition(const char *path, char *out, size_t size)
{
	char line[512], joined[1024];
	FILE *file = fopen(path, "r");
	int in_block = 0, collecting = 0;
	size_t used = 0;

	out[0] = '\0';
	joined[0] = '\0';
	if (file == NULL)
		return 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		size_t written;

		strip_comments(line, &in_block);
		if (!collecting && strstr(line, "compile_info[]") == NULL)
			continue;
		collecting = 1;
		/* snprintf() returns the length it would have written, so the
		 * total has to be held inside the buffer or the next append
		 * would get a wrapped size and a pointer past the end. */
		written = (size_t)snprintf(joined + used, sizeof(joined) - used,
		                           "%s", line);
		used += written;
		if (used > sizeof(joined) - 1)
			used = sizeof(joined) - 1;
		if (used >= sizeof(joined) - 1 || strchr(line, ';') != NULL)
			break;
	}
	fclose(file);
	if (collecting)
		collapse_spaces(joined, out, size);
	return 1;
}

/*
 * The number the named header gives a #define, or 0 when the header has no
 * such line. One shim sizes its buffer with a name rather than a number, so
 * the size below is looked up here.
 */
static size_t read_define_value(const char *path, const char *name)
{
	char line[512];
	FILE *file = fopen(path, "r");
	int in_block = 0;
	size_t value = 0;

	if (file == NULL)
		return 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		char *at;

		strip_comments(line, &in_block);
		if (strstr(line, "#define") == NULL)
			continue;
		if (!contains_identifier(line, name))
			continue;
		at = strstr(line, name);
		value = (size_t)strtoul(at + strlen(name), NULL, 10);
		break;
	}
	fclose(file);
	return value;
}

/*
 * The size of the reply buffer the named USB shim declares, from
 * "abVendorReqData[<size>]", or 0 when the file or the declaration is not
 * there. The size is either a number or the name of a #define in the shim's
 * own header, which is where the other argument comes in.
 */
static size_t read_reply_buffer_size(const char *path, const char *header)
{
	char line[512];
	FILE *file = fopen(path, "r");
	int in_block = 0;
	size_t size = 0;

	if (file == NULL)
		return 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		char *open, *close, *end;

		strip_comments(line, &in_block);
		if (!contains_identifier(line, "abVendorReqData"))
			continue;
		open = strchr(line, '[');
		close = open != NULL ? strchr(open, ']') : NULL;
		if (close == NULL)
			continue;
		*close = '\0';
		size = (size_t)strtoul(open + 1, &end, 10);
		if (end == open + 1) {
			/* Not a number: the name of a #define in the header. */
			char *last;

			while (*end == ' ' || *end == '\t')
				end++;
			for (last = end + strlen(end); last > end; last--)
				if (last[-1] != ' ' && last[-1] != '\t')
					break;
			*last = '\0';
			size = read_define_value(header, end);
		}
		break;
	}
	fclose(file);
	return size;
}

MU_TEST(both_images_define_the_build_string_the_same_way)
{
	char definition[1024], message[2048];
	int i;

	for (i = 0; i < SOURCE_COUNT; i++) {
		snprintf(message, sizeof(message), "%s would not open",
		         source_paths[i]);
		mu_assert_fatal(read_definition(source_paths[i], definition,
		                                sizeof(definition)), message);
		snprintf(message, sizeof(message),
		         "%s: compile_info is defined as `%s`, not `%s`. "
		         "ubertooth-util -V reports this string, so a board "
		         "running either image has to describe its build in "
		         "the same terms",
		         source_paths[i], definition, COMPILE_INFO_DEFINITION);
		mu_assert(strcmp(definition, COMPILE_INFO_DEFINITION) == 0,
		          message);
	}
}

/* -------------------------------------------------------------------------
 * The revision reply
 * ------------------------------------------------------------------------- */

/*
 * The reply UBERTOOTH_GET_REV_NUM builds, as both handlers build it: two zero
 * bytes where an old firmware put a 16-bit revision number, then a one-byte
 * length, then that many characters. Returns the reply length.
 */
static int revision_reply(const char *revision, uint8_t *data)
{
	size_t length = strlen(revision);

	if (length > UBERTOOTH_REVISION_MAX)
		length = UBERTOOTH_REVISION_MAX;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = (uint8_t)length;
	memcpy(&data[3], revision, length);
	return (int)(2 + 1 + length);
}

/* The same with the length cast to a byte, as it was: the cast wraps a length
 * of 256 or more modulo 256 and that many characters go out behind it. */
static int old_revision_reply(const char *revision, uint8_t *data)
{
	size_t length = (uint8_t)strlen(revision);

	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = (uint8_t)length;
	memcpy(&data[3], revision, length);
	return (int)(2 + 1 + length);
}

/*
 * A revision of `length` characters. Either reply above writes at most
 * 3 + 255 bytes for any of these, which is the reply buffer both images
 * declare -- the last case puts that size against the cap -- so neither can
 * run off the end of a VENDOR_REQ_BUF buffer.
 */
#define LONGEST_REVISION 512
static const char *revision_of_length(size_t length)
{
	static char revision[LONGEST_REVISION + 1];
	size_t i;

	if (length > LONGEST_REVISION)
		length = LONGEST_REVISION;
	for (i = 0; i < length; i++)
		revision[i] = (char)('a' + i % 26);
	revision[length] = '\0';
	return revision;
}

/* The revision this tree produces goes out whole, as it always did. */
MU_TEST(a_normal_revision_goes_out_whole)
{
	uint8_t reply[VENDOR_REQ_BUF];

	mu_assert_int_eq(15, revision_reply("git-deadbeef", reply));
	mu_assert_int_eq(12, reply[2]);
	mu_assert(memcmp(&reply[3], "git-deadbeef", 12) == 0,
	          "the reply does not carry the revision");
}

/* A revision too long for a one-byte length is cut to the cap. */
MU_TEST(a_long_revision_is_cut_to_the_cap)
{
	static const size_t lengths[] = { 255, 256, 300, 511, 512 };
	uint8_t reply[VENDOR_REQ_BUF];
	size_t i;

	for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		const char *revision = revision_of_length(lengths[i]);
		char message[128];

		snprintf(message, sizeof(message),
		         "a %d-character revision was not reported as the "
		         "%d-character cap", (int)lengths[i],
		         UBERTOOTH_REVISION_MAX);
		mu_assert_int_eq(3 + UBERTOOTH_REVISION_MAX,
		                 revision_reply(revision, reply));
		mu_assert(reply[2] == UBERTOOTH_REVISION_MAX, message);
		mu_assert(memcmp(&reply[3], revision,
		                 UBERTOOTH_REVISION_MAX) == 0,
		          "the cut reply does not carry the leading "
		          "characters of the revision");
	}
}

/*
 * The bug: the cast reported the length modulo 256, so a 256-character
 * revision went out as none at all and a 300-character one as its first 44.
 * Either reads back as a revision the board is not running, and
 * `ubertooth-dfu -r` compares exactly that against the image it wrote.
 */
MU_TEST(the_old_cast_wrapped_a_long_revision)
{
	uint8_t reply[VENDOR_REQ_BUF];
	int capped;

	mu_assert_int_eq(3, old_revision_reply(revision_of_length(256), reply));
	mu_assert_int_eq(0, reply[2]);

	mu_assert_int_eq(3 + 44,
	                 old_revision_reply(revision_of_length(300), reply));
	mu_assert_int_eq(44, reply[2]);

	/* ...and it was right for every revision short enough to describe,
	 * which is why nothing on the bench ever saw this. */
	capped = revision_reply(revision_of_length(255), reply);
	mu_assert_int_eq(capped,
	                 old_revision_reply(revision_of_length(255), reply));
	mu_assert_int_eq(UBERTOOTH_REVISION_MAX, reply[2]);
}

/*
 * The cap is only safe because the buffer a handler writes its reply into is
 * bigger than the biggest reply the cap allows. That buffer is declared in
 * each image's USB shim, nowhere near the cap, so this reads the declared size
 * out of both and puts both caps against it. The revision reply is the tighter
 * of the two: it carries three bytes of header rather than one.
 */
MU_TEST(the_cap_fits_the_buffer_both_shims_declare)
{
	static const struct {
		const char *source;
		const char *header;
	} shims[] = {
		{ FW_RXTX_DIR "/ubertooth_usb.c",
		  FW_RXTX_DIR "/ubertooth_usb.h" },
		{ FW_BTBR_DIR "/src/drivers/ubertooth_usb.c",
		  FW_BTBR_DIR "/include/ubtbr/ubertooth_usb.h" },
	};
	char message[1024];
	size_t i;

	for (i = 0; i < sizeof(shims) / sizeof(shims[0]); i++) {
		const char *path = shims[i].source;
		size_t declared = read_reply_buffer_size(path,
		                                         shims[i].header);

		snprintf(message, sizeof(message),
		         "%s: no abVendorReqData[<size>] declaration to read",
		         path);
		mu_assert_fatal(declared > 0, message);

		snprintf(message, sizeof(message),
		         "%s: abVendorReqData is %d bytes, too small for the "
		         "%d-byte reply UBERTOOTH_COMPILE_INFO_MAX allows "
		         "(one length byte and %d characters)",
		         path, (int)declared,
		         1 + UBERTOOTH_COMPILE_INFO_MAX,
		         UBERTOOTH_COMPILE_INFO_MAX);
		mu_assert(1 + (size_t)UBERTOOTH_COMPILE_INFO_MAX <= declared,
		          message);

		snprintf(message, sizeof(message),
		         "%s: abVendorReqData is %d bytes, too small for the "
		         "%d-byte reply UBERTOOTH_REVISION_MAX allows (two "
		         "zero bytes, one length byte and %d characters)",
		         path, (int)declared,
		         3 + UBERTOOTH_REVISION_MAX, UBERTOOTH_REVISION_MAX);
		mu_assert(3 + (size_t)UBERTOOTH_REVISION_MAX <= declared,
		          message);

		snprintf(message, sizeof(message),
		         "%s: abVendorReqData is %d bytes, not the %d the "
		         "fakes above write their replies into", path,
		         (int)declared, VENDOR_REQ_BUF);
		mu_assert(declared == VENDOR_REQ_BUF, message);
	}
}

/*
 * Both handlers cut, rather than cast. The handlers are hardware code that
 * cannot run here, so this scans the named case in each source for the cap,
 * which is what keeps the fakes above from drifting away from the firmware.
 */
static void assert_both_handlers_cap(const char *request, const char *cap,
                                     int cap_value)
{
	char line[512], message[1024];
	int i;

	for (i = 0; i < SOURCE_COUNT; i++) {
		FILE *file = fopen(source_paths[i], "r");
		int in_block = 0, in_case = 0, capped = 0;

		snprintf(message, sizeof(message), "%s would not open",
		         source_paths[i]);
		mu_assert_fatal(file != NULL, message);
		while (fgets(line, sizeof(line), file) != NULL) {
			strip_comments(line, &in_block);
			if (contains_identifier(line, request)) {
				in_case = 1;
				continue;
			}
			if (!in_case)
				continue;
			if (contains_identifier(line, cap))
				capped = 1;
			/* The case ends at its break. */
			if (contains_identifier(line, "break"))
				break;
		}
		fclose(file);
		snprintf(message, sizeof(message),
		         "%s: the %s case does not cut the string to %s (%d), "
		         "so a longer one would reach the host as its first "
		         "strlen-modulo-256 characters, or as none at all",
		         source_paths[i], request, cap, cap_value);
		mu_assert(in_case, message);
		mu_assert(capped, message);
	}
}

MU_TEST(both_handlers_cut_the_reply_to_that_cap)
{
	assert_both_handlers_cap("UBERTOOTH_GET_COMPILE_INFO",
	                         "UBERTOOTH_COMPILE_INFO_MAX",
	                         UBERTOOTH_COMPILE_INFO_MAX);
}

MU_TEST(both_handlers_cut_the_revision_to_its_cap)
{
	assert_both_handlers_cap("UBERTOOTH_GET_REV_NUM",
	                         "UBERTOOTH_REVISION_MAX",
	                         UBERTOOTH_REVISION_MAX);
}

int main(void)
{
	printf("firmware text reply tests\n");
	MU_RUN(both_images_define_the_build_string_the_same_way);
	MU_RUN(a_normal_revision_goes_out_whole);
	MU_RUN(a_long_revision_is_cut_to_the_cap);
	MU_RUN(the_old_cast_wrapped_a_long_revision);
	MU_RUN(both_handlers_cut_the_reply_to_that_cap);
	MU_RUN(both_handlers_cut_the_revision_to_its_cap);
	MU_RUN(the_cap_fits_the_buffer_both_shims_declare);
	return mu_report();
}
