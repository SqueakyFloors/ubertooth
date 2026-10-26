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

#ifndef UBERTOOTH_OPT_PARSE_H
#define UBERTOOTH_OPT_PARSE_H

#include <stddef.h>
#include <stdint.h>

/* The -U line every tool's usage text prints, so the tools cannot drift
 * apart. The man pages repeat it in words and point at ubertooth(7). */
#define OPT_USAGE_SELECT_DEVICE \
	"\t-U <index|serial> select Ubertooth by index or serial; see ubertooth(7)\n"

/*
 * Every parser here returns 0 on success and -1 on failure, and writes its
 * result only on success. Each one refuses an empty string, a sign, leading
 * whitespace, trailing junk and an out-of-range value, so a typo cannot be
 * read as 0 the way atoi() reads it.
 */

/* A whole decimal integer in [min, max]. Silent: for checking a word before
 * taking it as an option's value (see opt_separate_value()). */
int opt_parse_int(const char *text, long min, long max, int *value);

/* The same, for an option's value: on failure it prints
 * "error: <what> must be <min>-<max><unit> (got <text>)" so the caller only
 * has to exit. <unit> is appended to the range (" MHz") and may be "". */
int opt_require_int(const char *text, long min, long max,
                    const char *what, const char *unit, int *value);

/* A whole decimal count of at least <min> with no upper limit of its own (up
 * to INT_MAX), such as a timeout. On failure it prints
 * "error: <what> must be a whole number of <unit>, <hint> (got <text>)",
 * for example hint "0 for none" or "1 or more". */
int opt_require_count(const char *text, long min, const char *what,
                      const char *unit, const char *hint, int *value);

/* Hex digits only, between min_digits and max_digits of them, no 0x prefix,
 * no sign and no spaces. Fewer digits than max_digits means leading zeros. */
int opt_parse_hex(const char *text, size_t min_digits, size_t max_digits,
                  uint32_t *value);

/* A Bluetooth lower address part: exactly 6 hex digits (3 bytes). */
int opt_parse_lap(const char *text, uint32_t *lap);

/* A Bluetooth upper address part: exactly 2 hex digits (1 byte). */
int opt_parse_uap(const char *text, uint8_t *uap);

/* The same two for an option's value: on failure each prints
 * "error: <LAP|UAP> must be <n> hex digits (got <text>)". */
int opt_require_lap(const char *text, uint32_t *lap);
int opt_require_uap(const char *text, uint8_t *uap);

/* A BLE access address: 1 to 8 hex digits ("12345" is 0x00012345). */
int opt_parse_access_address(const char *text, uint32_t *address);

/* A BD_ADDR / MAC address, "xx:xx:xx:xx:xx:xx", most significant byte first.
 * Exactly two hex digits per byte and exactly five colons. */
int opt_parse_bd_addr(const char *text, uint8_t *address);

/*
 * A bare "-" means standard output, which is only useful for a pipe or a
 * redirect: binary written to a terminal fills it with control characters and
 * can leave it in a strange state. Call this where a tool is about to stream
 * binary to stdout. Returns 0 when stdout is not a terminal, or prints
 * "error: stdout is a tty, refusing to write binary <what> to it" and returns
 * -1, so the caller only has to exit. <what> names what would be written,
 * for example "samples"; NULL reads "output".
 */
int opt_check_stdout_not_tty(const char *what);

/*
 * The one argument rule.
 *
 * No ubertooth-* tool takes operands, so a leftover word is always a mistake:
 * a typo such as "- v", or an optional value written as a separate word that
 * the tool does not expect. Refuse it before anything happens, rather than
 * silently running the rest of the command.
 *
 * Call this right after the getopt loop. Returns 0 if nothing is left over,
 * or prints "error: unexpected argument '<word>'" and returns -1.
 */
int opt_check_no_operands(int argc, char *argv[]);

/*
 * True when the user gave no arguments at all.
 *
 * A bare command line is the one refusal that carries the usage text: a tool
 * that needs arguments prints its one `error: ` line and then the usage text,
 * both on standard error. Every other refusal is that line alone, and leaves
 * the usage text to -h. Call this where a tool refuses a command line with
 * nothing to do:
 *
 *     if (opt_no_arguments(argc))
 *             usage(stderr);
 */
int opt_no_arguments(int argc);

/*
 * The diagnostic for an option getopt would not take.
 *
 * getopt's own message names the whole argv[0] path and carries no prefix, so
 * a tool sets opterr = 0 and calls this instead. Start the option string with
 * ':' so a missing option value comes back as ':' rather than '?', then call
 * this from the getopt loop's default case. The usage text does not follow it:
 * the line it prints describes the problem (see opt_no_arguments()).
 *
 * <opt> is what getopt returned: ':' for an option whose value is missing,
 * anything else for an option it does not know.
 */
void opt_report_bad_option(int opt, int argc, char *argv[]);

/* True if <text> could be the value of the option being parsed. */
typedef int (*opt_value_check)(const char *text);

/*
 * getopt only reads an optional option value when it is attached (-c2440).
 * Since no tool takes operands, a word that looks like a value for this
 * option can only be that value: take it, so "-c 2440" works like "-c2440".
 *
 * Call this when optarg is NULL for an option with an optional value. Returns
 * the next word and advances optind, or NULL if there is no next word or
 * <valid> rejects it, which leaves a following option to be parsed as an
 * option and any other word to be refused by opt_check_no_operands().
 *
 * <valid> only decides whether the word was meant as this option's value; the
 * parser above then reports a malformed or out-of-range one.
 */
char *opt_separate_value(int argc, char *argv[], opt_value_check valid);

/* Value checks for opt_separate_value(). */
int opt_value_decimal(const char *text);        /* digits only */
int opt_value_signed_decimal(const char *text); /* digits, optionally signed */
int opt_value_not_option(const char *text);     /* anything but -... or empty */

#endif
