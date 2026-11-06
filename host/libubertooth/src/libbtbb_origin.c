/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Whether this run decodes packets with a different libbtbb install from the
 * one the build linked, which is what -V warns about.
 *
 * It is here, apart from the code that asks the loader, so the decision can
 * be tested on paths chosen by a test rather than on whatever the machine
 * this is built on happens to have installed.
 */

#include <stdlib.h>
#include <string.h>

#include "libbtbb_origin.h"

int ubertooth_directory_of(const char* path, char* out, size_t size)
{
	const char* slash;
	size_t length;

	if (path == NULL || out == NULL || size == 0)
		return 0;
	slash = strrchr(path, '/');
	if (slash == NULL)
		return 0;
	/* The root directory is one slash, not none. */
	length = (size_t)(slash == path ? 1 : slash - path);
	if (length >= size)
		return 0;
	memcpy(out, path, length);
	out[length] = '\0';
	return 1;
}

/*
 * Whether two directories are the same one. The names are compared first,
 * which answers it without touching the file system for the usual case, and
 * then what they resolve to: a prefix reached through a symbolic link, and
 * /lib against /usr/lib on a system that merged them, are one directory under
 * two names, and reporting those as two installs would warn about every
 * correct build on such a machine.
 *
 * A name that does not resolve -- a directory that has been removed since the
 * build, or one this process may not look at -- is compared as written, which
 * is the best that can be said about it.
 */
static int same_directory(const char* one, const char* other)
{
	char* one_real;
	char* other_real;
	int same;

	if (strcmp(one, other) == 0)
		return 1;
	one_real = realpath(one, NULL);
	other_real = realpath(other, NULL);
	same = one_real != NULL && other_real != NULL
	       && strcmp(one_real, other_real) == 0;
	free(one_real);
	free(other_real);
	return same;
}

int ubertooth_libbtbb_is_elsewhere(const char* loaded, const char* self,
                                   const char* built_dir, char* dir,
                                   size_t size)
{
	if (dir != NULL && size > 0)
		dir[0] = '\0';
	/* Nothing to compare against: a build that could not say where the
	 * libbtbb it linked was, or a loader that will not say which file it
	 * opened. */
	if (built_dir == NULL || built_dir[0] == '\0')
		return 0;
	if (loaded == NULL || loaded[0] == '\0')
		return 0;
	/* libbtbb is inside the same file as this library -- a static link, or
	 * a build without position-independent code -- which is not a second
	 * install at all. */
	if (self != NULL && strcmp(loaded, self) == 0)
		return 0;
	if (!ubertooth_directory_of(loaded, dir, size))
		return 0;
	if (same_directory(dir, built_dir))
		return 0;
	return 1;
}
