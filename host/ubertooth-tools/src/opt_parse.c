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
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Length of a BD_ADDR in bytes, and of its printed form "xx:xx:...:xx". */
#define BD_ADDR_BYTES 6
#define BD_ADDR_TEXT_LEN (BD_ADDR_BYTES * 3 - 1)

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
	uint8_t bytes[BD_ADDR_BYTES];
	size_t i;

	if (text == NULL || address == NULL)
		return -1;
	if (strlen(text) != BD_ADDR_TEXT_LEN)
		return -1;
	for (i = 0; i < BD_ADDR_BYTES; i++) {
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

int opt_check_no_operands(int argc, char *argv[])
{
	if (optind < argc) {
		fprintf(stderr, "error: unexpected argument '%s'\n", argv[optind]);
		return -1;
	}
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
