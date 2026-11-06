/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The decision behind -V's line about another libbtbb. Internal to the
 * library: not installed, and nothing outside this tree calls it.
 */

#ifndef LIBBTBB_ORIGIN_H
#define LIBBTBB_ORIGIN_H

#include <stddef.h>

/*
 * The directory part of <path>, written into <out>. Returns 0, leaving <out>
 * untouched, where the path has no directory in it or will not fit.
 */
int ubertooth_directory_of(const char* path, char* out, size_t size);

/*
 * Whether <loaded>, the file the loader opened libbtbb from, is a different
 * install from <built_dir>, the directory of the libbtbb the build linked.
 * <self> is the file this library itself was loaded from, or NULL.
 *
 * Returns 1 when they differ, with the directory of <loaded> written into
 * <dir> for the report; 0 when they are the same install, when either end is
 * unknown, or when libbtbb is inside this very file.
 *
 * The directories are compared, not the whole paths, because a build records
 * the library it linked (libbtbb.so) and the loader reports the file it
 * opened (libbtbb.so.1).
 */
int ubertooth_libbtbb_is_elsewhere(const char* loaded, const char* self,
                                   const char* built_dir, char* dir,
                                   size_t size);

#endif
