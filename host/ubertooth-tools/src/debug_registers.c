/*
 * Register reads and writes for ubertooth-debug. See debug_registers.h.
 */
#include "debug_registers.h"
#include "cc2400.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int debug_read_registers(debug_read_register_fn read_register, void *context,
                         const int *regs, int n, int verbose,
                         FILE *out, FILE *err)
{
	int status = 0;
	int i;

	for (i = 0; i < n; i++) {
		int value = read_register(context, (uint8_t)regs[i]);

		if (value < 0) {
			if (value != DEBUG_ERROR_SHORT_REPLY)
				fprintf(err, "error: register 0x%02x read failed\n", regs[i]);
			status = 1;
			continue;
		}
		cc2400_decode(out, regs[i], (unsigned short)value, verbose);
	}
	return status;
}

/* The longest item -w accepts: the longest register name in cc2400.c is well
 * under this, and so is "0x00=0xffff". An item longer than this is refused as
 * one that could not be read, rather than silently cut. */
#define WRITE_ITEM_MAX 32

/*
 * One whole number in [0, max]: hex with a leading 0x, decimal otherwise.
 * Returns 0, or -1 for anything that is not the whole of text or is out of
 * range. Nothing is printed: the caller words the failure around the item the
 * user wrote.
 *
 * The base is chosen here rather than left to strtol's base 0, which reads a
 * leading zero as octal: `-w 010=1` would then mean register 8, and `08` would
 * be refused as not a number. A register value is written in hex or in plain
 * decimal, and that is what the man page says.
 */
static int parse_number(const char *text, long max, long *value)
{
	char *end;
	long v;
	int base = 10;

	if (*text == '\0')
		return -1;
	/* A leading '-' or space would make strtol() accept what is not a
	 * register or a value, so they are refused before it runs. */
	if (*text == '-' || *text == '+' || isspace((unsigned char)*text))
		return -1;
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		base = 16;
		text += 2;
		/* strtol would take the '-' in "0x-1" as the number's sign. */
		if (!isxdigit((unsigned char)*text))
			return -1;
	}
	errno = 0;
	v = strtol(text, &end, base);
	if (errno != 0 || *end != '\0' || v < 0 || v > max)
		return -1;
	*value = v;
	return 0;
}

int debug_parse_register_writes(const char *spec, debug_register_write *writes,
                                int max, FILE *err)
{
	int n = 0;

	/* An empty value asks for nothing, which is a mistake rather than a
	 * list of no writes: say so here, where the item can be quoted. A
	 * trailing comma is the same mistake, and the loop below would not see
	 * it -- it ends at the comma with every item already parsed. */
	if (spec == NULL || *spec == '\0' || spec[strlen(spec) - 1] == ',') {
		fprintf(err, "error: register write list must be"
		        " <reg>=<value>[,...] (got %s)\n",
		        (spec == NULL || *spec == '\0') ? "an empty value" : spec);
		return -1;
	}

	while (*spec != '\0') {
		char item[WRITE_ITEM_MAX];
		char *equals;
		size_t length = strcspn(spec, ",");
		long reg, value;
		int named;

		if (length == 0 || length >= sizeof(item)) {
			/* Both the empty item between two commas and an item too
			 * long to be a register and a value. */
			fprintf(err, "error: register write must be <reg>=<value>"
			        " (got %.*s in %s)\n", (int)length, spec, spec);
			return -1;
		}
		memcpy(item, spec, length);
		item[length] = '\0';

		equals = strchr(item, '=');
		if (equals == NULL || equals == item || equals[1] == '\0') {
			fprintf(err, "error: register write must be <reg>=<value>"
			        " (got %s in %s)\n", item, spec);
			return -1;
		}
		*equals = '\0';

		/* A %name as -r accepts, or an address. The name lookup carries
		 * the '%', which is how cc2400.c's table spells them. */
		named = (item[0] == '%');
		if (named) {
			int r = cc2400_name2reg(item);

			if (r < 0) {
				fprintf(err, "error: no CC2400 register is called"
				        " %s (in %s)\n", item, spec);
				return -1;
			}
			reg = r;
		} else if (parse_number(item, 0xff, &reg) < 0) {
			/* The same line -r gives for the same mistake, with the
			 * whole value after it as -r's does. */
			fprintf(err, "error: register address must be 0x00-0xff"
			        " (got %s in %s)\n", item, spec);
			return -1;
		}

		if (parse_number(equals + 1, 0xffff, &value) < 0) {
			fprintf(err, "error: register value must be 0x0000-0xffff"
			        " (got %s in %s)\n", equals + 1, spec);
			return -1;
		}

		if (n >= max) {
			fprintf(err, "error: at most %d register writes in one -w"
			        " (got more in %s)\n", max, spec);
			return -1;
		}
		writes[n].reg = (uint8_t)reg;
		writes[n].value = (uint16_t)value;
		n++;

		spec += length;
		if (*spec == ',')
			spec++;
	}

	return n;
}

int debug_write_registers(debug_write_register_fn write_register, void *context,
                          const debug_register_write *writes, int n,
                          FILE *err)
{
	int status = 0;
	int i;

	for (i = 0; i < n; i++) {
		if (write_register(context, writes[i].reg, writes[i].value) < 0) {
			/* A write has no reply to come up short, so every failure is
			 * a refused request, named here on top of the line
			 * libubertooth prints for it. */
			fprintf(err, "error: register 0x%02x write failed\n",
			        writes[i].reg);
			status = 1;
		}
	}
	return status;
}
