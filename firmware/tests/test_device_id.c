/*
 * The part number and the serial number: the bytes the host gets back, and
 * where the boot ROM is read to produce them.
 *
 * Both numbers come from the part's In-Application Programming entry point in
 * the boot ROM. The firmware answers control requests from inside whichever
 * mode loop is running, so a boot-ROM call that does not return promptly stops
 * that loop: a live capture asked for its serial number was measured answering
 * nothing at all for the watchdog's whole 3000 ms and then being reset, which
 * is what a user meets when any tool is started with `-U` while a capture runs
 * (#505). Starting a tool with `-U` reads every attached board's serial number.
 *
 * So the numbers are read once at startup and every later request is answered
 * from that copy. Two things have to hold for that to be a fix rather than a
 * change:
 *
 *   - the host sees the same bytes as before, and
 *   - no boot-ROM call is left anywhere a request or a mode can reach.
 *
 * The first is runnable: the layout lives in firmware/common/device_id.h and
 * the cases below feed it known boot-ROM words and check every byte. The
 * second cannot run here -- reading the numbers needs the ROM -- so it is read
 * out of the sources as text, the way test_reset_order.c reads set_isp().
 *
 * PATH_MAX is used for the directory walk, so this test is POSIX, as the
 * source-scanning tests beside it are.
 */
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

#include "device_id.h"

#ifndef FW_COMMON_DIR
#define FW_COMMON_DIR "../common"
#endif
#ifndef FW_RXTX_DIR
#define FW_RXTX_DIR "../bluetooth_rxtx"
#endif
#ifndef FW_BTBR_DIR
#define FW_BTBR_DIR "../btbr"
#endif

/*
 * What the boot ROM returns: result[0] is its status and the rest are the
 * number's words. These are a plausible shape rather than a real part's -- the
 * bytes are all distinct, so a byte that moves shows up as the wrong value and
 * not as a duplicate.
 */
static const uint32_t rom_result[5] = {
	0x00000000,
	0x11223344,
	0x55667788,
	0x99aabbcc,
	0xddeeff00,
};

/*
 * A 17-byte serial-number reply, and a 5-byte part-number reply, as the code
 * this change replaced wrote them: the status byte, then each word least
 * significant byte first. Spelled out here so a change to the layout fails
 * even if it changes device_id.h and the code that calls it together.
 */
static const uint8_t expected_serial[17] = {
	0x00,
	0x44, 0x33, 0x22, 0x11,
	0x88, 0x77, 0x66, 0x55,
	0xcc, 0xbb, 0xaa, 0x99,
	0x00, 0xff, 0xee, 0xdd,
};
static const uint8_t expected_part_num[5] = {
	0x00,
	0x44, 0x33, 0x22, 0x11,
};

MU_TEST(test_the_serial_number_reply_is_unchanged)
{
	uint8_t reply[17];
	char msg[256];
	int i;

	memset(reply, 0xa5, sizeof(reply));
	device_id_pack(reply, rom_result, 4);

	for (i = 0; i < 17; i++) {
		snprintf(msg, sizeof(msg),
		         "byte %d of the serial-number reply is 0x%02x, not the "
		         "0x%02x the host has always read there",
		         i, reply[i], expected_serial[i]);
		mu_assert(reply[i] == expected_serial[i], msg);
	}
}

MU_TEST(test_the_part_number_reply_is_unchanged)
{
	uint8_t reply[5];
	char msg[256];
	int i;

	memset(reply, 0xa5, sizeof(reply));
	device_id_pack(reply, rom_result, 1);

	for (i = 0; i < 5; i++) {
		snprintf(msg, sizeof(msg),
		         "byte %d of the part-number reply is 0x%02x, not the "
		         "0x%02x the host has always read there",
		         i, reply[i], expected_part_num[i]);
		mu_assert(reply[i] == expected_part_num[i], msg);
	}
}

/* A failed read still reports the boot ROM's status, so the host says so. */
MU_TEST(test_a_failed_read_keeps_the_status_byte)
{
	uint32_t failed[5] = { 7, 0, 0, 0, 0 };
	uint8_t reply[17];

	memset(reply, 0xa5, sizeof(reply));
	device_id_pack(reply, failed, 4);
	mu_assert(reply[0] == 7, "the boot ROM's status byte is not passed on");
}

/*
 * The range test puts the serial number on the air with each word's most
 * significant byte first, the opposite way round from the reply. Both boards
 * compare all 16 bytes to decide whether a packet is their own exchange
 * (test_rangetest_radio.c), so a reordering here would stop the pair matching.
 */
MU_TEST(test_the_range_test_sends_each_word_most_significant_byte_first)
{
	static const uint8_t expected[16] = {
		0x11, 0x22, 0x33, 0x44,
		0x55, 0x66, 0x77, 0x88,
		0x99, 0xaa, 0xbb, 0xcc,
		0xdd, 0xee, 0xff, 0x00,
	};
	uint8_t reply[17];
	uint8_t air[16];
	char msg[256];
	int i;

	device_id_pack(reply, rom_result, 4);
	memset(air, 0xa5, sizeof(air));
	device_serial_msb_first(air, &reply[1]);

	for (i = 0; i < 16; i++) {
		snprintf(msg, sizeof(msg),
		         "byte %d of the range test's serial number is 0x%02x, "
		         "not 0x%02x", i, air[i], expected[i]);
		mu_assert(air[i] == expected[i], msg);
	}
}

/* One source file, scanned for calls to the boot ROM by the function they are
 * in. Returns 0 if the file cannot be read. */
struct rom_calls {
	int count;
	int first_line;
	char first_function[96];
	char functions[8][96];
	int function_count;
};

static int scan_rom_calls(const char *path, struct rom_calls *out)
{
	char line[SCAN_LINE_MAX], name[96];
	char current[96] = "(file scope)";
	int lineno = 0, in_block = 0;
	FILE *file = fopen(path, "r");

	memset(out, 0, sizeof(*out));
	if (file == NULL)
		return 0;

	while (fgets(line, sizeof(line), file) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		strip_string_literals(line);

		/*
		 * A function definition starts in the first column and ends the
		 * line with its opening brace or nothing else. Close enough to
		 * name the function a call sits in, which is all this needs.
		 */
		if (line[0] != '\0' && !isspace((unsigned char)line[0]) &&
		    line[0] != '#' && line[0] != '}' && strchr(line, '(') != NULL &&
		    strstr(line, ";") == NULL) {
			const char *open = strchr(line, '(');
			const char *p = open;
			int n = 0;

			while (p > line && is_ident_char(p[-1]))
				p--;
			while (p < open && n < (int)sizeof(name) - 1)
				name[n++] = *p++;
			name[n] = '\0';
			if (n > 0)
				snprintf(current, sizeof(current), "%s", name);
		}

		if (!calls_identifier(line, "iap_entry"))
			continue;

		if (out->count == 0) {
			out->first_line = lineno;
			snprintf(out->first_function, sizeof(out->first_function),
			         "%s", current);
		}
		out->count++;
		if (out->function_count < 8) {
			snprintf(out->functions[out->function_count],
			         sizeof(out->functions[0]), "%s", current);
			out->function_count++;
		}
	}
	fclose(file);
	return 1;
}

/*
 * Two calls in the shared file and no others: device_id_init(), which runs at
 * startup, and set_isp(), which hands the board over to the on-chip programmer
 * and is meant to stop everything (it masks interrupts around its call for its
 * own reasons -- see test_reset_order.c).
 */
MU_TEST(test_only_startup_and_the_hand_over_call_the_boot_rom)
{
	const char *path = FW_COMMON_DIR "/ubertooth.c";
	struct rom_calls calls;
	char msg[512];
	int i;

	snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
	mu_assert_fatal(scan_rom_calls(path, &calls), msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan found no boot-ROM call at all, so the checks "
	         "here would pass without reading anything", path);
	mu_assert_fatal(calls.count > 0, msg);

	for (i = 0; i < calls.function_count; i++) {
		snprintf(msg, sizeof(msg),
		         "%s: %s() calls the boot ROM. Only device_id_init(), at "
		         "startup, and set_isp(), which hands the board over, may: "
		         "a call the host or a mode can reach stops the mode loop "
		         "and the watchdog resets the board (#505)",
		         path, calls.functions[i]);
		mu_assert(strcmp(calls.functions[i], "device_id_init") == 0 ||
		          strcmp(calls.functions[i], "set_isp") == 0, msg);
	}
}

/* Every .c file under a directory, scanned. Returns the number scanned. */
static int scan_tree(const char *dir, char *culprit, size_t culprit_size)
{
	char path[PATH_MAX];
	struct dirent *entry;
	struct rom_calls calls;
	int scanned = 0;
	DIR *d = opendir(dir);

	if (d == NULL)
		return 0;

	while ((entry = readdir(d)) != NULL) {
		const char *dot;

		if (entry->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
		dot = strrchr(entry->d_name, '.');
		if (dot == NULL || strcmp(dot, ".c") != 0) {
			int sub = scan_tree(path, culprit, culprit_size);

			scanned += sub;
			if (culprit[0] != '\0')
				break;
			continue;
		}
		if (!scan_rom_calls(path, &calls))
			continue;
		scanned++;
		if (calls.count > 0) {
			snprintf(culprit, culprit_size, "%s:%d: %s()",
			         path, calls.first_line, calls.first_function);
			break;
		}
	}
	closedir(d);
	return scanned;
}

/*
 * And nowhere else in either shipped image: not in a request handler, not in a
 * mode, not in anything they reach. Both image directories are walked rather
 * than a list of files being named, so a boot-ROM call added to a file nobody
 * thought of is caught too.
 *
 * assembly_test is not walked. It answers both requests from the same copies
 * now, but its SET_ISP case hands the board over to the on-chip programmer
 * where it stands, which is a boot-ROM call this check would report; the
 * shipped images hand over from set_isp() instead.
 */
MU_TEST(test_no_shipped_image_calls_the_boot_rom_outside_startup)
{
	static const char *const trees[] = { FW_RXTX_DIR, FW_BTBR_DIR };
	char culprit[PATH_MAX + 128];
	char msg[PATH_MAX + 512];
	size_t i;

	for (i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		int scanned;

		culprit[0] = '\0';
		scanned = scan_tree(trees[i], culprit, sizeof(culprit));

		snprintf(msg, sizeof(msg),
		         "no sources found under %s, so this check would pass "
		         "without reading anything", trees[i]);
		mu_assert_fatal(scanned > 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s calls the boot ROM. The host can reach this while a "
		         "mode is running, and a boot-ROM call that does not come "
		         "back stops the mode loop until the watchdog resets the "
		         "board (#505): read the number from the copy "
		         "device_id_init() made at startup instead", culprit);
		mu_assert(culprit[0] == '\0', msg);
	}
}

/*
 * The copy is made before the host can ask for it. Each image calls
 * device_id_init() from main() ahead of its USB stack -- which reads the serial
 * number for its own descriptor -- and well ahead of anything that arms the
 * watchdog, so the one slow read happens where nothing is waiting on it. The
 * bootloader is deliberately not among them: it needs neither number, and it is
 * the path a board is recovered on.
 */
static const char *const image_mains[] = {
	FW_RXTX_DIR "/bluetooth_rxtx.c",
	FW_BTBR_DIR "/src/btctl/main.c",
};

MU_TEST(test_every_image_reads_the_numbers_at_startup)
{
	char line[SCAN_LINE_MAX], msg[PATH_MAX + 512];
	size_t i;

	for (i = 0; i < sizeof(image_mains) / sizeof(image_mains[0]); i++) {
		const char *path = image_mains[i];
		int lineno = 0, in_block = 0, in_main = 0, read = 0, usb = 0;
		FILE *file = fopen(path, "r");

		snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
		mu_assert_fatal(file != NULL, msg);

		while (fgets(line, sizeof(line), file) != NULL) {
			lineno++;
			strip_comments(line, &in_block);
			if (!in_main) {
				if (strstr(line, "int main(") != NULL)
					in_main = 1;
				continue;
			}
			if (calls_identifier(line, "device_id_init") && read == 0)
				read = lineno;
			if (calls_identifier(line, "ubertooth_usb_init") && usb == 0)
				usb = lineno;
		}
		fclose(file);

		snprintf(msg, sizeof(msg),
		         "%s: main() does not call device_id_init(), so both "
		         "numbers would be reported as a failed read", path);
		mu_assert_fatal(read > 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s:%d: main() starts the USB stack before it reads the "
		         "two numbers at line %d, so the serial number in the USB "
		         "descriptor would be the unread value", path, usb, read);
		mu_assert(usb > read, msg);
	}
}

/*
 * Until the read has happened the status byte says so. Zeroes would read as a
 * successful report of a part with no serial number.
 */
MU_TEST(test_an_unread_copy_reports_a_failure)
{
	const char *path = FW_COMMON_DIR "/ubertooth.c";
	char line[SCAN_LINE_MAX], msg[PATH_MAX + 256];
	int in_block = 0, part = 0, serial = 0;
	FILE *file = fopen(path, "r");

	snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
	mu_assert_fatal(file != NULL, msg);

	while (fgets(line, sizeof(line), file) != NULL) {
		char flat[SCAN_LINE_MAX];

		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));
		if (strstr(flat, "part_num_reply[5] = { 0xff }") != NULL)
			part = 1;
		if (strstr(flat, "device_serial_reply[17] = { 0xff }") != NULL)
			serial = 1;
	}
	fclose(file);

	snprintf(msg, sizeof(msg),
	         "%s: the two copies do not start with a 0xff status byte, so an "
	         "image that never read the boot ROM would report a part number "
	         "and a serial number of zeroes as if the read had worked", path);
	mu_assert(part && serial, msg);
}

int main(void)
{
	printf("device id tests\n");
	MU_RUN(test_the_serial_number_reply_is_unchanged);
	MU_RUN(test_the_part_number_reply_is_unchanged);
	MU_RUN(test_a_failed_read_keeps_the_status_byte);
	MU_RUN(test_the_range_test_sends_each_word_most_significant_byte_first);
	MU_RUN(test_only_startup_and_the_hand_over_call_the_boot_rom);
	MU_RUN(test_no_shipped_image_calls_the_boot_rom_outside_startup);
	MU_RUN(test_every_image_reads_the_numbers_at_startup);
	MU_RUN(test_an_unread_copy_reports_a_failure);
	return mu_report();
}
