/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared command-line value parsing and the one argument rule for every
 * ubertooth-* host tool.
 */

#include "opt_parse.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int opt_parse_int(const char *text, long min, long max, int *value)
{
	char *end;
	long v;

	if (text == NULL || value == NULL)
		return -1;
	errno = 0;
	v = strtol(text, &end, 10);
	/* strtol() skips leading whitespace, accepts a '+' sign and stops at
	 * the first character it cannot use; none of those is a value. */
	if (text[0] == '\0' || isspace((unsigned char)text[0]) || text[0] == '+' ||
	    *end != '\0' || errno == ERANGE || v < min || v > max)
		return -1;
	*value = (int)v;
	return 0;
}

int opt_require_int(const char *text, long min, long max,
                    const char *what, const char *unit, int *value)
{
	if (opt_parse_int(text, min, max, value) < 0) {
		/* "-128 to 127" reads better than "-128-127" */
		fprintf(stderr, "error: %s must be %ld%s%ld%s (got %s)\n",
		        what, min, min < 0 ? " to " : "-", max, unit,
		        text == NULL ? "" : text);
		return -1;
	}
	return 0;
}

int opt_require_count(const char *text, long min, const char *what,
                      const char *unit, const char *hint, int *value)
{
	if (opt_parse_int(text, min, INT_MAX, value) < 0) {
		fprintf(stderr, "error: %s must be a whole number of %s, %s (got %s)\n",
		        what, unit, hint, text == NULL ? "" : text);
		return -1;
	}
	return 0;
}

int opt_parse_hex(const char *text, size_t min_digits, size_t max_digits,
                  uint32_t *value)
{
	uint32_t v = 0;
	size_t i, n;

	if (text == NULL || value == NULL || max_digits > 8)
		return -1;
	n = strlen(text);
	if (n < min_digits || n > max_digits)
		return -1;
	for (i = 0; i < n; i++) {
		int c = (unsigned char)text[i];
		if (!isxdigit(c))
			return -1;
		v = (v << 4) |
		    (uint32_t)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
	}
	*value = v;
	return 0;
}

int opt_parse_lap(const char *text, uint32_t *lap)
{
	return opt_parse_hex(text, 6, 6, lap);
}

int opt_parse_uap(const char *text, uint8_t *uap)
{
	uint32_t value;

	if (uap == NULL || opt_parse_hex(text, 2, 2, &value) < 0)
		return -1;
	*uap = (uint8_t)value;
	return 0;
}

int opt_require_lap(const char *text, uint32_t *lap)
{
	if (opt_parse_lap(text, lap) < 0) {
		fprintf(stderr, "error: LAP must be 6 hex digits (got %s)\n",
		        text == NULL ? "" : text);
		return -1;
	}
	return 0;
}

int opt_require_uap(const char *text, uint8_t *uap)
{
	if (opt_parse_uap(text, uap) < 0) {
		fprintf(stderr, "error: UAP must be 2 hex digits (got %s)\n",
		        text == NULL ? "" : text);
		return -1;
	}
	return 0;
}

int opt_parse_access_address(const char *text, uint32_t *address)
{
	return opt_parse_hex(text, 1, 8, address);
}

int opt_parse_hex_bytes(const char *text, uint8_t *bytes, size_t max_bytes,
                        size_t *length)
{
	size_t digits, i;
	uint32_t value;

	if (text == NULL || bytes == NULL || length == NULL || max_bytes == 0)
		return -1;
	digits = strlen(text);
	if (digits == 0 || digits % 2 != 0 || digits / 2 > max_bytes)
		return -1;
	/* The whole value is checked before any of it is written, so a value
	 * refused part way through -- "01zz" -- leaves the caller's buffer as
	 * it found it rather than half filled. Nothing but hex digits, so "0x"
	 * or a sign is refused here rather than read as a byte. */
	for (i = 0; i < digits; i++)
		if (!isxdigit((unsigned char)text[i]))
			return -1;
	for (i = 0; i < digits; i += 2) {
		char pair[3];

		pair[0] = text[i];
		pair[1] = text[i + 1];
		pair[2] = '\0';
		/* Cannot fail: both characters are hex digits. */
		opt_parse_hex(pair, 2, 2, &value);
		bytes[i / 2] = (uint8_t)value;
	}
	*length = digits / 2;
	return 0;
}

int opt_require_hex_bytes(const char *text, const char *what, uint8_t *bytes,
                          size_t max_bytes, size_t *length)
{
	if (opt_parse_hex_bytes(text, bytes, max_bytes, length) < 0) {
		fprintf(stderr, "error: %s must be hex digits, two per byte,"
		        " 1 to %zu bytes (got %s)\n", what, max_bytes,
		        text == NULL ? "" : text);
		return -1;
	}
	return 0;
}

int opt_parse_bd_addr(const char *text, uint8_t *address)
{
	uint8_t bytes[OPT_BD_ADDR_BYTES];
	size_t i;

	if (text == NULL || address == NULL)
		return -1;
	if (strlen(text) != OPT_BD_ADDR_TEXT_LEN)
		return -1;
	for (i = 0; i < OPT_BD_ADDR_BYTES; i++) {
		char pair[3];
		uint32_t byte;

		if (i > 0 && text[i * 3 - 1] != ':')
			return -1;
		pair[0] = text[i * 3];
		pair[1] = text[i * 3 + 1];
		pair[2] = '\0';
		if (opt_parse_hex(pair, 2, 2, &byte) < 0)
			return -1;
		bytes[i] = (uint8_t)byte;
	}
	memcpy(address, bytes, sizeof(bytes));
	return 0;
}

int opt_require_bd_addr(const char *text, const char *what, const char *given,
                        uint8_t *address)
{
	/* The whole word where the caller split one up, otherwise the value. */
	const char *quoted = given != NULL ? given : text;

	if (opt_parse_bd_addr(text, address) < 0) {
		fprintf(stderr, "error: %s must be xx:xx:xx:xx:xx:xx (got %s)\n",
		        what, quoted == NULL ? "" : quoted);
		return -1;
	}
	return 0;
}

int opt_bd_addr_lap_uap(const uint8_t *address, uint32_t *lap, uint8_t *uap)
{
	if (address == NULL)
		return -1;
	/* "NN:NN:UU:LL:LL:LL", most significant byte first. */
	if (uap != NULL)
		*uap = address[2];
	if (lap != NULL)
		*lap = ((uint32_t)address[3] << 16) |
		       ((uint32_t)address[4] << 8) | (uint32_t)address[5];
	return 0;
}

int opt_check_stdout_not_tty(const char *what)
{
	if (isatty(STDOUT_FILENO)) {
		/* The whole description comes from the caller, the word "binary"
		 * included: most of these streams are binary, and one of them,
		 * ubertooth-dump -b, is text that is just as unreadable on a
		 * screen. A fixed "binary" here called that one binary too. */
		fprintf(stderr,
		        "error: stdout is a tty, refusing to write %s to it\n",
		        what == NULL ? "this output" : what);
		return -1;
	}
	return 0;
}

int opt_writes_to_stdout(const char *filename)
{
	return filename != NULL && strcmp(filename, "-") == 0;
}

int opt_check_capture_destination(const char *filename, const char *what)
{
	if (opt_writes_to_stdout(filename))
		return opt_check_stdout_not_tty(what);
	return 0;
}

/* Hand the real standard output to a capture and send this process's own
 * standard output to /dev/null for the rest of the run, so that nothing the
 * tool prints can land in the middle of the capture stream. Returns the
 * descriptor the capture is to be written to, or -1 once the reason has been
 * printed. <what> names the data, for the tty refusal above. */
static int take_stdout(const char *what)
{
	int saved_fd, null_fd;

	if (opt_check_stdout_not_tty(what) < 0)
		return -1;

	fflush(stdout);
	saved_fd = dup(STDOUT_FILENO);
	if (saved_fd < 0) {
		fprintf(stderr, "error: cannot save stdout for the capture: %s\n",
		        strerror(errno));
		return -1;
	}

	null_fd = open("/dev/null", O_WRONLY);
	if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0) {
		fprintf(stderr, "error: cannot redirect stdout to /dev/null: %s\n",
		        strerror(errno));
		if (null_fd >= 0)
			close(null_fd);
		close(saved_fd);
		return -1;
	}
	if (null_fd != STDOUT_FILENO)
		close(null_fd);
	return saved_fd;
}

/* strdup(), with the one line a caller would otherwise exit without: the
 * callers of opt_capture_path() have nothing else to say about a NULL. */
static char *copy_name(const char *name)
{
	char *copy = strdup(name);

	if (copy == NULL)
		fputs("error: out of memory\n", stderr);
	return copy;
}

char *opt_capture_path(const char *filename, const char *what, int *saved_fd)
{
	char path[64];

	if (saved_fd == NULL)
		return NULL;
	*saved_fd = -1;
	if (filename == NULL)
		return NULL;
	if (strcmp(filename, "-") != 0)
		return copy_name(filename);

	*saved_fd = take_stdout(what);
	if (*saved_fd < 0)
		return NULL;

	/* The writers in libbtbb open a file name rather than a descriptor, so
	 * the saved standard output is named as one: /dev/fd works on macOS and
	 * Linux, and /proc/self/fd is the fallback where /dev/fd is not
	 * mounted. Opening that path is a fresh open on Linux and a second
	 * descriptor on the same open file on macOS, so a capture whose
	 * standard output was appended to a file (">> capture.pcap") truncates
	 * that file on Linux and appends to it on macOS. A pipe, which is what
	 * "-" is for, behaves the same either way. */
	snprintf(path, sizeof(path), "/dev/fd/%d", *saved_fd);
	if (access(path, W_OK) != 0) {
		snprintf(path, sizeof(path), "/proc/self/fd/%d", *saved_fd);
		if (access(path, W_OK) != 0) {
			fputs("error: no writable descriptor filesystem for stdout "
			      "capture\n", stderr);
			close(*saved_fd);
			*saved_fd = -1;
			return NULL;
		}
	}
	return copy_name(path);
}

void opt_report_create_failure(const char *filename, int reason)
{
	fprintf(stderr, "error: %s: %s\n", filename == NULL ? "" : filename,
	        reason != 0 ? strerror(reason) : "could not be created");
}

void opt_discard_capture_file(const char *filename)
{
	struct stat info;
	int failed;

	if (filename == NULL || opt_writes_to_stdout(filename))
		return;

	/* lstat(), not stat(): what a symbolic link names was the user's choice
	 * of where the capture goes, and removing the link would take that
	 * choice away. The file it names keeps whatever the writer put there. */
	if (lstat(filename, &info) == 0) {
		if (!S_ISREG(info.st_mode))
			return;
		failed = unlink(filename) < 0;
	} else {
		/* Already gone, by another hand or because the writer never
		 * got as far as creating it. */
		failed = errno != ENOENT;
	}
	if (failed)
		fprintf(stderr, "warning: cannot remove %s: %s\n", filename,
		        strerror(errno));
}

FILE *opt_open_dump_file(const char *filename, const char *what)
{
	FILE *stream;
	int saved_fd;

	if (filename == NULL)
		return NULL;
	if (strcmp(filename, "-") != 0) {
		stream = fopen(filename, "w");
		if (stream == NULL)
			fprintf(stderr, "error: %s: %s\n", filename, strerror(errno));
		return stream;
	}

	saved_fd = take_stdout(what);
	if (saved_fd < 0)
		return NULL;
	stream = fdopen(saved_fd, "w");
	if (stream == NULL) {
		fprintf(stderr, "error: cannot write %s to stdout: %s\n",
		        what == NULL ? "output" : what, strerror(errno));
		close(saved_fd);
	}
	return stream;
}

int opt_check_no_operands(int argc, char *argv[])
{
	if (optind < argc) {
		fprintf(stderr, "error: unexpected argument '%s'\n", argv[optind]);
		return -1;
	}
	return 0;
}

int opt_no_arguments(int argc)
{
	return argc <= 1;
}

/* Put getopt back at the start of a command line, so that another can be
 * parsed from the beginning.
 *
 * Setting optind to 0 is what the GNU getopt takes as "start over": it
 * forgets how far into a word it had read, which setting optind to 1 does
 * not. It is a GNU getopt the tools call on every platform they are built
 * for: Linux links the system one, which is GNU, and macOS and MSVC link the
 * copy in host/ubertooth-tools/getopt (USE_OWN_GNU_GETOPT). The BSD getopt
 * reads optreset instead and would need it; assigning to that variable on
 * macOS does nothing at all, the bundled getopt being what the tools there
 * call. So a BSD without the bundled copy would lose this -- the
 * COMPATIBILITY.md matrix has none, and it could not be lost quietly, since
 * every dash case in test_opt_parse.c fails where this reset does not work.
 */
static void getopt_restart(void)
{
	optind = 0;
}

/* The word a rejected '-' came from, as the user wrote it, so the message can
 * quote that rather than a bare dash.
 *
 * getopt names the letter it rejected and not the word that held it, and
 * where it got to does not say either: it reads a word a letter at a time and
 * has already moved past a word whose last letter it is on, so a dash that
 * ends a word ("-v-") leaves optind on the word after, while a dash with more
 * of its word to come ("-r-x") leaves optind on the word itself. Which of the
 * two happened cannot be told from optind alone, and guessing it named the
 * wrong word on a command line that holds both: "-v- -Ufoo-" blamed
 * "-Ufoo-", where the dash is part of -U's value and nothing is wrong.
 *
 * Telling them apart needs the option letters, so this takes them and parses
 * the command line again, one word at a time: the first word whose arrival
 * makes getopt refuse a dash is the word that holds it.
 *
 * Two kinds of refusal on the way are not the one being reported, and each
 * is handled on its own:
 *
 * An option whose value is missing ("-U" with nothing after it yet) is kept.
 * A required value may be the next word, which the pass that stops at the
 * option has not reached: "ubertooth-util -U -v- -r-x" gives -U the value
 * "-v-", so that word is not a mistake and must not be named for one.
 * Dropping the option here would leave its value to be read as options.
 *
 * An option getopt does not know is left out of the passes that follow. That
 * is how a separate optional value is got past: an optional value may be
 * written as its own word ("ubertooth-util -z -90"), which getopt does not
 * read -- the tool picks it up with opt_separate_value() afterwards -- and
 * which getopt refuses on its own, '9' being no option. Leaving it out is
 * safe because the refusal the tool stopped on was the dash, so a word getopt
 * will not read here is one the tool's own loop read another way.
 *
 * getopt is left part-way through a command line of its own afterwards, so
 * this may only be called where the tool is giving up: every caller of
 * opt_report_bad_option() returns as soon as it has printed.
 *
 * Returns NULL when no word refuses a dash, which leaves the caller to name
 * the dash on its own.
 */
static const char *dash_word(int argc, char *argv[], const char *options)
{
	const char *word = NULL;
	char **block, **taken, **pass;
	int count, i, saved_opterr;

	if (options == NULL || argv == NULL || argc < 2)
		return NULL;

	/* Two lists, each of at most argc words and a null terminator: the
	 * words taken so far, in the order they are read here, and the copy
	 * getopt gets, which the GNU getopt is free to reorder. Keeping the
	 * two apart is what makes "the word just added" mean anything, and
	 * leaves the caller's argv alone -- in whatever order the tool's own
	 * getopt loop left it, which for a command line holding an operand is
	 * not the order the user typed, the GNU getopt having moved the
	 * operand to the end. Reading a moved operand late is harmless: it is
	 * no option, so it refuses nothing and names nothing. */
	block = malloc(2 * ((size_t)argc + 1) * sizeof(*block));
	if (block == NULL)
		return NULL;
	taken = block;
	pass = block + argc + 1;

	/* argv[0] is the program's own path: part of every pass, and never
	 * named in a diagnostic about what the user typed. */
	taken[0] = argv[0];
	count = 1;

	/* getopt's own messages off, as the caller's loop has them: these
	 * passes refuse things on purpose and must stay silent. Put back
	 * below, so this leaves nothing behind but getopt's position. */
	saved_opterr = opterr;
	opterr = 0;

	for (i = 1; i < argc && word == NULL; i++) {
		int refused = 0, opt, k;

		/* exec() gives no NULL before argv[argc], but a caller in this
		 * process could, and getopt would read through it. */
		if (argv[i] == NULL)
			break;

		for (k = 0; k < count; k++)
			pass[k] = taken[k];
		pass[count] = argv[i];
		pass[count + 1] = NULL;

		getopt_restart();
		while ((opt = getopt(count + 1, pass, options)) != -1) {
			/* getopt returns '?' for an option it does not know and
			 * ':' for one whose value is missing; anything else is
			 * an option it took. No tool has either as a letter. */
			if (opt == '?' || opt == ':') {
				refused = opt;
				break;
			}
		}

		if (refused == '?' && optopt == '-')
			word = argv[i];           /* the dash: this is the word */
		else if (refused != '?')
			taken[count++] = argv[i]; /* taken, or waiting for a value */
		/* else: an option getopt does not know, which the tool's own
		 * loop read another way, so it is left out of what follows. */
	}

	opterr = saved_opterr;
	free(block);
	return word;
}

void opt_report_bad_option(int opt, int argc, char *argv[], const char *options)
{
	/* dash_word() parses again below, which overwrites optopt, so the
	 * letter getopt rejected is read once here. */
	const int letter = optopt;
	const char *word;

	if (opt == ':' && letter != 0 && isprint((unsigned char)letter)) {
		fprintf(stderr, "error: option '-%c' requires a value\n", letter);
		return;
	}
	if (opt == ':') {
		fputs("error: an option is missing its value\n", stderr);
		return;
	}
	/* A rejected '-' is a letter inside a word the user wrote, so the whole
	 * word is quoted back. No tool takes long options: ubertooth-dfu's
	 * --update-bootloader is taken out of argv before getopt reads it, so a
	 * word starting with "--" still arrives here as a rejected '-'. */
	if (letter == '-') {
		word = dash_word(argc, argv, options);
		if (word == NULL)
			/* The search found no word to blame, which leaves the
			 * dash on its own -- rather than the "--" that naming it
			 * as an option would print. A caller that passed no
			 * option string lands here, and so would a command line
			 * the search cannot follow. */
			fputs("error: unrecognized option '-'\n", stderr);
		else if (strncmp(word, "--", 2) == 0 && word[2] != '\0')
			fprintf(stderr, "error: unrecognized option '%s': the tools take "
			        "single-letter options\n", word);
		else
			fprintf(stderr, "error: unrecognized option '-' in '%s'\n", word);
		return;
	}
	if (letter != 0 && isprint((unsigned char)letter))
		fprintf(stderr, "error: unrecognized option '-%c'\n", letter);
	else
		fputs("error: unrecognized option\n", stderr);
}

int opt_check_once(opt_once *state, int opt, const char *value)
{
	unsigned char letter;

	/* getopt returns ':' for an option whose value is missing and '?' for
	 * one it does not know. Neither names an option the user chose, and
	 * opt_report_bad_option() answers both. */
	if (state == NULL || value == NULL || opt == ':' || opt == '?')
		return 0;
	if (opt < 0 || opt > UCHAR_MAX || !isprint((unsigned char)opt))
		return 0;
	letter = (unsigned char)opt;
	if (state->seen[letter]) {
		fprintf(stderr, "error: option '-%c' given more than once\n", letter);
		return -1;
	}
	state->seen[letter] = 1;
	return 0;
}

char *opt_separate_value(int argc, char *argv[], opt_value_check valid)
{
	if (optind >= argc || valid == NULL || !valid(argv[optind]))
		return NULL;
	optind++;
	return argv[optind - 1];
}

int opt_value_decimal(const char *text)
{
	if (text == NULL || text[0] == '\0')
		return 0;
	return strspn(text, "0123456789") == strlen(text);
}

int opt_value_signed_decimal(const char *text)
{
	if (text == NULL)
		return 0;
	return opt_value_decimal(text[0] == '-' ? text + 1 : text);
}

int opt_value_not_option(const char *text)
{
	/* A bare "-" is an option-looking word too: it is only a value where
	 * getopt takes it as a required one (ubertooth-btle -q -). */
	return text != NULL && text[0] != '\0' && text[0] != '-';
}
