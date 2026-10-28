/*
 * Shared command-line value parsing and the one argument rule for every
 * ubertooth-* host tool.
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
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
		fprintf(stderr,
		        "error: stdout is a tty, refusing to write binary %s to it\n",
		        what == NULL ? "output" : what);
		return -1;
	}
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

/* The word a rejected '-' came from, so the message can quote what the user
 * wrote rather than a bare dash.
 *
 * getopt reads one letter at a time and stops at the dash, so where it got to
 * says which word that is. A dash with more of its word left to read, as in
 * "--bogus" or "-r-x", leaves optind on that word. A dash that ends the word,
 * as in "-v-", is the word's last letter, and getopt moves optind past the
 * word as it starts on that letter, so the word is the one before.
 *
 * The two are told apart by the word optind is on: an option word that holds
 * a dash of its own is the one being rejected, and anything else -- the
 * option after it, a stray word, the end of the command line -- means the
 * dash was the last letter of the word before. A command line with a stray
 * dash in two words running ("-v- -r-x") cannot be told apart from an
 * attached option value that ends in one ("-Uabc- --bogus"), because the
 * option letters are not known here; the word optind is on wins, which names
 * a word the user wrote either way. Returns NULL if neither word holds a
 * dash, which leaves the caller its own wording.
 */
static const char *bad_option_word(int argc, char *argv[])
{
	const char *word;
	size_t length;

	if (optind < argc && argv[optind] != NULL && argv[optind][0] == '-' &&
	    strchr(argv[optind] + 1, '-') != NULL)
		return argv[optind];
	/* From argv[1] on: the guard below keeps argv[0], the program's own
	 * path, out of a diagnostic about what the user typed. */
	if (optind >= 2 && optind - 1 < argc && argv[optind - 1] != NULL) {
		word = argv[optind - 1];
		length = strlen(word);
		if (length > 1 && word[0] == '-' && word[length - 1] == '-')
			return word;
	}
	return NULL;
}

void opt_report_bad_option(int opt, int argc, char *argv[])
{
	const char *word;

	if (opt == ':' && optopt != 0 && isprint((unsigned char)optopt)) {
		fprintf(stderr, "error: option '-%c' requires a value\n", optopt);
		return;
	}
	if (opt == ':') {
		fputs("error: an option is missing its value\n", stderr);
		return;
	}
	/* A rejected '-' is a letter inside a word the user wrote, so the whole
	 * word is quoted back: a bare "-" would name an option nobody typed,
	 * and so would the word after it. No tool takes long options. */
	if (optopt == '-' && (word = bad_option_word(argc, argv)) != NULL) {
		if (strncmp(word, "--", 2) == 0 && word[2] != '\0')
			fprintf(stderr, "error: unrecognized option '%s': the tools take "
			        "single-letter options\n", word);
		else
			fprintf(stderr, "error: unrecognized option '-' in '%s'\n", word);
		return;
	}
	if (optopt != 0 && isprint((unsigned char)optopt))
		fprintf(stderr, "error: unrecognized option '-%c'\n", optopt);
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
