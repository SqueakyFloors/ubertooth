#ifndef UBERTOOTH_ARGLIST_H
#define UBERTOOTH_ARGLIST_H

#include <stddef.h>

/* How a list failed, so the caller can word each kind its own way. */
enum int_list_failure {
	INT_LIST_OK = 0,
	INT_LIST_SYNTAX, /* could not be read at all; reason says what */
	INT_LIST_RANGE,  /* read, but outside the [min, max] the caller gave */
};

/*
 * Why a list was refused, and which item did it. Nothing is printed from
 * listOfInts(), so the caller reports the whole failure in one line, in the
 * form the rest of the tools use.
 *
 * item points into the caller's arg and is not terminated, so it is printed
 * with "%.*s" and item_length. item_length is 0 when the offending item is
 * empty, in which case quote the whole value instead.
 */
struct int_list_error {
	enum int_list_failure kind;
	const char *reason;
	const char *item;
	size_t item_length;
};

/*
 * Parse a comma separated list of integers, ranges and tokens, every value in
 * [min, max].
 *
 * On success returns a malloc'd array of *n values. On failure returns NULL,
 * sets *n to -1 and fills *error (when given).
 */
int *
listOfInts (char *arg, int *n, int (*token_parser) (char *p, int *nchars),
	    long min, long max, struct int_list_error *error);

#endif
