/*
 * The build string both firmware images carry, and the one thing the host
 * needs out of it.
 *
 * ubertooth-dfu -r checks, after the reset, that the board came back running
 * the revision that was just written. It learns which revision to expect by
 * reading the firmware FILE, not the board: dfu_extract_firmware_revision()
 * searches the image for "ubertooth ", takes what follows as the revision, and
 * stops at " (". The btbr image carried no such string, so -r refused to flash
 * it at all and the user was left to flash without -r and power-cycle the
 * board by hand.
 *
 * Neither image's request handler can run here, so this test reads the
 * compile_info definition out of each image's source and checks both say the
 * same thing, then hands the string that definition produces to the host's
 * real extractor. The equality check pins the spelling, which is stricter than
 * the host needs: a reformat the tool would still read fine fails it too. That
 * is the direction to err in, because the thing the host really needs is a
 * property of the built image, and nothing here can build one.
 *
 * The last two cases are about the reply the same string goes out in: a
 * one-byte length and then the characters, which the host reads at most
 * 1 + 255 bytes of. A longer string has to be cut to 255, because a one-byte
 * length cannot describe more. The old code cast the length to a byte, which
 * wrapped it modulo 256 and sent that many characters, so a 300-character
 * string reached the host as its first 44 and a 256-character one as nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "dfu_recovery.h"
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
		         "ubertooth-dfu -r reads the revision out of the image "
		         "by searching for that exact text, so an image whose "
		         "definition differs cannot be flashed with -r",
		         source_paths[i], definition, COMPILE_INFO_DEFINITION);
		mu_assert(strcmp(definition, COMPILE_INFO_DEFINITION) == 0,
		          message);
	}
}

/*
 * What the definition above expands to once the build fills in the generated
 * version.h, placed in a file the way it sits in an image: binary either side,
 * nothing that looks like a string boundary to help the search.
 */
MU_TEST(the_host_finds_the_revision_in_that_string)
{
	static const char image[] =
		"\x12\x34\x00\xff"
		"ubertooth git-deadbee* (builder@buildhost) Mon Oct 27 11:22:33 2025"
		"\x00\x00\xaa";
	FILE *file = tmpfile();
	char revision[64];

	mu_assert_fatal(file != NULL, "tmpfile failed");
	mu_assert_int_eq((int)sizeof(image),
	                 (int)fwrite(image, 1, sizeof(image), file));
	mu_assert_int_eq(0, dfu_extract_firmware_revision(file, revision,
	                                                  sizeof(revision)));
	mu_assert(strcmp(revision, "git-deadbee*") == 0,
	          "the host did not read the revision the build string carries");
	fclose(file);
}

/*
 * The cap is only safe because the buffer a handler writes its reply into is
 * bigger than the biggest reply the cap allows. That buffer is declared in
 * each image's USB shim, nowhere near the cap, so this reads the declared size
 * out of both and puts the cap against it.
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
	}
}

MU_TEST(both_handlers_cut_the_reply_to_that_cap)
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
			if (contains_identifier(line, "UBERTOOTH_GET_COMPILE_INFO")) {
				in_case = 1;
				continue;
			}
			if (!in_case)
				continue;
			if (contains_identifier(line, "UBERTOOTH_COMPILE_INFO_MAX"))
				capped = 1;
			/* The case ends at its break. */
			if (contains_identifier(line, "break"))
				break;
		}
		fclose(file);
		snprintf(message, sizeof(message),
		         "%s: the UBERTOOTH_GET_COMPILE_INFO case does not cut "
		         "the string to UBERTOOTH_COMPILE_INFO_MAX (%d), so a "
		         "longer one would reach the host as its first "
		         "strlen-modulo-256 characters, or as none at all",
		         source_paths[i], UBERTOOTH_COMPILE_INFO_MAX);
		mu_assert(in_case, message);
		mu_assert(capped, message);
	}
}

int main(void)
{
	printf("firmware build string tests\n");
	MU_RUN(both_images_define_the_build_string_the_same_way);
	MU_RUN(the_host_finds_the_revision_in_that_string);
	MU_RUN(both_handlers_cut_the_reply_to_that_cap);
	MU_RUN(the_cap_fits_the_buffer_both_shims_declare);
	return mu_report();
}
