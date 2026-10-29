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

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

/* Bytes in a BD_ADDR, and the length of its printed form
 * "xx:xx:xx:xx:xx:xx" without the terminating NUL. */
#define OPT_BD_ADDR_BYTES 6
#define OPT_BD_ADDR_TEXT_LEN (OPT_BD_ADDR_BYTES * 3 - 1)

/* A BD_ADDR / MAC address, "xx:xx:xx:xx:xx:xx", most significant byte first.
 * Exactly two hex digits per byte and exactly five colons; either case of hex
 * digit. Writes OPT_BD_ADDR_BYTES bytes. */
int opt_parse_bd_addr(const char *text, uint8_t *address);

/* The same, for an option's value: on failure it prints
 * "error: <what> must be xx:xx:xx:xx:xx:xx (got <text>)" so the caller only
 * has to exit. One wording for every option that takes an address; <what>
 * names which address it is, for example "target address".
 *
 * <given> is the option's value as the user wrote it, for a caller that took
 * the address out of a longer word and wants the whole word quoted back
 * (ubertooth-btle -t takes "<address>/<bits>"). NULL quotes <text> itself,
 * which is what an option whose whole value is an address passes. */
int opt_require_bd_addr(const char *text, const char *what, const char *given,
                        uint8_t *address);

/* The two parts of a BD_ADDR that identify a Bluetooth piconet: the lower
 * address part (LAP), its three least significant bytes, and the upper
 * address part (UAP), the byte above them. The two most significant bytes are
 * the non-significant address part, which a sniffer never sees.
 *
 * address is OPT_BD_ADDR_BYTES bytes, most significant first, as
 * opt_parse_bd_addr() writes them. Either output may be NULL. Returns 0, or
 * -1 for a NULL address. */
int opt_bd_addr_lap_uap(const uint8_t *address, uint32_t *lap, uint8_t *uap);

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
 * Whether a file option's value says to write to standard output, which is
 * the lone "-" and nothing else (ubertooth(7), OPTION VALUES). The one place
 * that rule is written down, so a tool that has to know before it opens the
 * destination does not spell it out again. NULL, a tool given no such option,
 * is not standard output.
 */
int opt_writes_to_stdout(const char *filename);

/*
 * The same refusal for a destination that may or may not be standard output:
 * 0 for any file name, and the check above for a lone "-". A tool whose
 * capture file is created later -- once the board is known to be usable --
 * calls this while it is still reading its command line, so a terminal is
 * refused before any board is opened. NULL is a tool that was given no such
 * option, and passes.
 */
int opt_check_capture_destination(const char *filename, const char *what);

/*
 * Where a capture option's value says to write, with a lone "-" meaning
 * standard output (ubertooth(7), OPTION VALUES).
 *
 * opt_capture_path() is for a writer that opens a file name, as libbtbb's
 * PCAP writers do: it returns the name to open, which the caller frees. Any
 * value but "-" comes back as itself, including one that starts with "-", so
 * "-q -x" writes the file "-x". A lone "-" gives back a path on this
 * process's original standard output and sets *saved_fd to the descriptor
 * behind it, which the caller closes once the writer has opened the path.
 *
 * opt_open_dump_file() is for a tool that writes the stream itself: it
 * returns the open file, or standard output for a lone "-".
 *
 * Either way, taking standard output sends the tool's own standard output to
 * /dev/null for the rest of the run, so nothing it prints can land in the
 * middle of the capture; a tool whose lines the user still needs there has to
 * print them to standard error instead. Both return NULL once the reason has
 * been printed, which for "-" includes standard output being a terminal.
 * <what> names the data in that refusal, for example "capture data".
 */
char *opt_capture_path(const char *filename, const char *what, int *saved_fd);
FILE *opt_open_dump_file(const char *filename, const char *what);

/*
 * The one line for a capture file a writer would not create: the name the
 * user gave, and the reason when there is a trustworthy one.
 *
 * Pass errno where the writer sets it, having cleared errno before the
 * create so that a reason left by an earlier failed call is not reported as
 * this one's. Pass 0 where it does not, and the line reads "<file>: could not
 * be created": libbtbb's PCAP writer prints the real reason itself and leaves
 * errno holding whatever its last internal call set, which on Linux is
 * EINVAL for a path that does not exist.
 */
void opt_report_create_failure(const char *filename, int reason);

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

/*
 * The option letters a tool has seen, for opt_check_once(). Declare one
 * beside the getopt loop and zero it:
 *
 *     opt_once once = {0};
 */
typedef struct {
	/* One flag per option letter, indexed by the letter itself. */
	unsigned char seen[UCHAR_MAX + 1];
} opt_once;

/*
 * The one-value-each rule: no option may be given a value twice.
 *
 * "-l 446688 -l aabbcc" names two LAPs for one capture. Keeping the last
 * throws the first away without a word, so the command line said two things
 * and meant one; refuse it instead of guessing, the way a combination that
 * cannot work is refused.
 *
 * Only a value counts. Repeating an option that takes none says the same
 * thing twice and changes nothing, and an option whose value is optional has
 * two jobs: "-c 2426 -c" sets the channel and then reads it back, which is
 * one value and one read, not two values. A tool that wants to refuse a
 * repeat of a valueless option of its own does it itself, where it can say
 * why (ubertooth-specan -v).
 *
 * Call this from the getopt loop for every option getopt returns, before the
 * switch, passing the value getopt found (optarg, which is NULL when there
 * is none). It prints "error: option '-l' given more than once" and returns
 * -1 the second time a letter arrives with a value; otherwise it returns 0.
 * An option getopt would not take is left to opt_report_bad_option().
 */
int opt_check_once(opt_once *state, int opt, const char *value);

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
