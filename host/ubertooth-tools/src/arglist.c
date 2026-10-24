/*
 * Copyright 2013
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

#include <stdlib.h>
#include <ctype.h>

#include "arglist.h"

#define OCTAL 8
#define BASE10 10
#define HEX 16

// no built-in test for Octal digits
int isodigit(int c)
{
    return (c >= '0' && c <= '7');
}


/* formatted with: indent -kr */

/*
 * Parse a list of comma separated integers or ranges into an array
 *
 * <number> = 0x[0-9A-Fa-f] | 0[0-7] | [1-9][0-9] | %<token>
 * <range> = <number> - <number>
 * <item> = <range> | <number>
 * <list> = <item>[,<list>]
 *
 * Examples:
 * "1"        {1}
 * "1,2,4-6"  {1,2,4,5,6}
 * "4-1"      {4,3,2,1}
 * "1-4"      {1,2,3,4}
 *
 * A user supplied token parser can be invoked to covert tokens
 * to integer values.  The token parser returns the integer value,
 * and the number of characters consumed.
 *
 * Nothing is printed here: on failure *reason (when given) names what could
 * not be read, so the caller reports the whole thing in one line, in the form
 * the rest of the tools use.
 */

int *listOfInts(char *arg, int *n,
		int (*token_parser) (char *p, int *nchars),
		const char **reason)
{
    int *r = NULL;
    int done = 0;
    const char *why = "not a list of registers";

    *n = 0;
    if (reason != NULL)
	*reason = why;

    // loop runs twice, first to count, second to populate allocated memory
    while (arg != NULL && !done) {
	char *p;
	int value = 0;
	int value_valid = 0;	// TRUE if value has a calculated value
	int first_value = -1;
	int base = BASE10;
	int first_char = 1;	// TRUE if reading the 1st char of token
	int have_token = 0;	// TRUE if just parsed token
	int size;

	p = arg;

	while (1) {
	    if (first_char && *p == '%' && token_parser != NULL) {
		value = token_parser(p, &size);
		if (size == -1) {
		    why = "unknown name";
		    goto fail;
		}
                value_valid = 1;
		have_token = 1;
		p += size;
		continue;
	    } else if (*p == ',' || *p == 0) {
		// end-of-value or end-of-string
		if (!value_valid) {
		    why = "empty value";
		    goto fail;
		}
		if (first_value >= 0) {
		    int i;
		    // preserve user supplied ordering
		    if (value > first_value)
			// upward range
			for (i = first_value; i <= value; i++) {
			    if (r)
				r[*n] = i;
			    (*n)++;
		    } else
			// downward range
			for (i = first_value; i >= value; i--) {
			    if (r)
				r[*n] = i;
			    (*n)++;
			}
		} else {
		    // single value
		    if (r)
			r[*n] = value;
		    (*n)++;
		}

		value = 0;
		value_valid = 0;
		first_value = -1;
		first_char = 1;
		have_token = 0;

		if (*p == 0)
		    break;	// end-of-string
	    } else if (*p == '-') {
		// range
		if (!value_valid) {
		    why = "range with no start";
		    goto fail;
		}
		first_value = value;
		value = 0;
		value_valid = 0;
		first_char = 1;
		have_token = 0;
	    } else if (have_token == 0 && isxdigit(*p)) {
		// valid digit
		value_valid = 1;
		if (first_char) {
		    if (*p == '0' && *(p + 1) == 'x') {
			base = HEX;
			p += 2;
		    } else if (*p == '0') {
			base = OCTAL;
		    } else if (isxdigit(*p) && !isdigit(*p)) {
			base = HEX;
		    } else
			base = BASE10;
		    first_char = 0;
		} else {
		    if (base == OCTAL && !isodigit(*p)) {
			why = "not an octal digit";
			goto fail;
		    }
		}

		// update the value, digit by digit
		value *= base;
		if (isdigit(*p))
		    value += (*p - '0');
		else if (isxdigit(*p))
		    value += (tolower(*p) - 'a') + BASE10;
	    } else {
		why = "not a name, address or range";
		goto fail;
	    }

	    p++;
	}
	// if array is populated we're done, otherwise allocate array
	if (r != NULL)
	    done = 1;
	else {
	    if ((r = (int *) malloc(*n * sizeof(int))) == 0) {
		why = "out of memory";
		goto fail;
	    }
	    *n = 0;
	}
    }

    return r;

  fail:			// make sure that we have a consistent error state
    if (reason != NULL)
	*reason = why;
    free(r);
    *n = -1;
    return NULL;
}
