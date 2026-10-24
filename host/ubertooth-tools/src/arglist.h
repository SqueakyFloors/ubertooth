/*
 * Parse a comma separated list of integers, ranges and tokens.
 *
 * On success returns a malloc'd array of *n values. On failure returns NULL,
 * sets *n to -1 and, if reason is not NULL, points *reason at a short static
 * phrase saying what could not be read ("value omitted", "invalid character",
 * ...), so the caller can report the whole failure in one line. Nothing is
 * printed from here.
 */
int *
listOfInts (char *arg, int *n, int (*token_parser) (char *p, int *nchars),
	    const char **reason);
