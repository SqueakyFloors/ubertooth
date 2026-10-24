/*
 * Line-scanning helpers for the tests that read firmware source as text.
 *
 * Some firmware logic cannot be run here: main()'s mode dispatch and
 * le_phy_main() are hardware code. What those tests check is the shape of the
 * source, so they scan it, and these are the parts every such scan needs:
 * strip the comments (the source explains the invariant in prose, so a scan
 * that read the comments would find whatever it was looking for), then match
 * whole identifiers, calls and assignments rather than substrings.
 *
 * Defined inline in the header so each test stays a single translation unit
 * that links nothing from the firmware.
 */
#ifndef UBERTOOTH_FW_TESTS_SOURCE_SCAN_H
#define UBERTOOTH_FW_TESTS_SOURCE_SCAN_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * Strip C comments from a line, in place, carrying block-comment state across
 * lines.
 */
static inline void strip_comments(char *line, int *in_block)
{
	char *r = line, *w = line;

	while (*r != '\0') {
		if (*in_block) {
			if (r[0] == '*' && r[1] == '/') {
				*in_block = 0;
				r += 2;
			} else {
				r++;
			}
			continue;
		}
		if (r[0] == '/' && r[1] == '*') {
			*in_block = 1;
			r += 2;
			continue;
		}
		if (r[0] == '/' && r[1] == '/')
			break;
		*w++ = *r++;
	}
	*w = '\0';
}

static inline int is_ident_char(char c)
{
	return c == '_' || isalnum((unsigned char)c);
}

/*
 * Does `text` contain `name` as a whole identifier? A plain substring search
 * would find MODE_BT_FOLLOW inside MODE_BT_FOLLOW_LE.
 */
static inline int contains_identifier(const char *text, const char *name)
{
	size_t len = strlen(name);
	const char *p = text;

	while ((p = strstr(p, name)) != NULL) {
		char prev = (p == text) ? ' ' : p[-1];

		if (!is_ident_char(prev) && !is_ident_char(p[len]))
			return 1;
		p += len;
	}
	return 0;
}

/*
 * The identifier a line calls or defines: the word in front of the first
 * '('. 0 when the line has none.
 */
static inline int call_name(const char *line, char *out, size_t size)
{
	const char *open = strchr(line, '(');
	const char *start, *end;

	if (open == NULL)
		return 0;
	end = open;
	while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	start = end;
	while (start > line && is_ident_char(start[-1]))
		start--;
	if (start == end || (size_t)(end - start) >= size)
		return 0;
	memcpy(out, start, (size_t)(end - start));
	out[end - start] = '\0';
	return 1;
}

/*
 * Does the text assign to `name`? Looks for "name = " with no identifier
 * character in front, so `mode` does not match inside `requested_mode`.
 */
static inline int is_assignment_to(const char *text, const char *name)
{
	char needle[64];
	const char *p = text;

	snprintf(needle, sizeof(needle), "%s =", name);
	while ((p = strstr(p, needle)) != NULL) {
		char prev = (p == text) ? ' ' : p[-1];
		/* "==" is a comparison, not an assignment */
		if (p[strlen(needle)] != '=' &&
		    (prev == ' ' || prev == '\t' || prev == '(' || prev == ';'))
			return 1;
		p += strlen(needle);
	}
	return 0;
}

/* The same, for an assignment of one particular value: "name = value". */
static inline int is_assignment_of(const char *text, const char *name,
                                   const char *value)
{
	char needle[96];

	snprintf(needle, sizeof(needle), "%s = %s", name, value);
	return strstr(text, needle) != NULL;
}

#endif
