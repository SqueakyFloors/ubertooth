/*
 * The decision behind -V's line about another libbtbb
 * (libbtbb_origin.c): given the file the loader opened libbtbb from, the file
 * this library came from, and the directory the build linked libbtbb out of,
 * is this run using a second install?
 *
 * The warning exists for a machine with a distribution or Homebrew libbtbb
 * beside this fork's build, because the two decode packets differently and
 * nothing else in a run says which one is in use. It has to stay quiet
 * otherwise: a correct install that printed it on every -V would teach its
 * reader to ignore it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
#include "libbtbb_origin.h"

/* Long enough for any of the paths below, and short enough that one case can
 * overflow it on purpose. */
#define DIR_LEN 256

#define mu_assert_dir_eq(expected, actual) do {                          \
	mu_assert(strcmp((expected), (actual)) == 0,                     \
	          "the directory reported is not " expected);            \
} while (0)

static int elsewhere(const char *loaded, const char *self,
                     const char *built_dir, char *dir, size_t size)
{
	return ubertooth_libbtbb_is_elsewhere(loaded, self, built_dir, dir,
	                                      size);
}

MU_TEST(a_second_install_is_reported)
{
	char dir[DIR_LEN];

	mu_assert_int_eq(1, elsewhere("/opt/homebrew/lib/libbtbb.1.dylib",
	                              "/usr/local/lib/libubertooth.1.dylib",
	                              "/usr/local/lib", dir, sizeof(dir)));
	/* The directory, for the warning to name. */
	mu_assert_dir_eq("/opt/homebrew/lib", dir);
}

/* The build records the library it linked and the loader reports the file it
 * opened, which are two names in one directory. */
MU_TEST(the_same_directory_is_not_another_install)
{
	char dir[DIR_LEN];

	mu_assert_int_eq(0, elsewhere("/usr/local/lib/libbtbb.so.1", NULL,
	                              "/usr/local/lib", dir, sizeof(dir)));
}

/* A build that could not say where its libbtbb was, and a loader that will
 * not say which file it opened: nothing to compare, so nothing is claimed. */
MU_TEST(an_unknown_end_says_nothing)
{
	char dir[DIR_LEN];

	mu_assert_int_eq(0, elsewhere("/opt/homebrew/lib/libbtbb.1.dylib",
	                              NULL, "", dir, sizeof(dir)));
	mu_assert_int_eq(0, elsewhere("/opt/homebrew/lib/libbtbb.1.dylib",
	                              NULL, NULL, dir, sizeof(dir)));
	mu_assert_int_eq(0, elsewhere(NULL, NULL, "/usr/local/lib", dir,
	                              sizeof(dir)));
	mu_assert_int_eq(0, elsewhere("", NULL, "/usr/local/lib", dir,
	                              sizeof(dir)));
}

/* libbtbb inside this very file, which a static link and a build without
 * position-independent code both give. Not a second install. */
MU_TEST(libbtbb_inside_this_binary_is_not_another_install)
{
	char dir[DIR_LEN];

	mu_assert_int_eq(0, elsewhere("/usr/local/bin/ubertooth-util",
	                              "/usr/local/bin/ubertooth-util",
	                              "/usr/local/lib", dir, sizeof(dir)));
}

/* A path with no directory in it, which names nothing to compare, and one
 * whose directory will not fit the buffer. */
MU_TEST(a_path_with_no_directory_says_nothing)
{
	char dir[DIR_LEN];
	char deep[DIR_LEN * 2];

	mu_assert_int_eq(0, elsewhere("libbtbb.so.1", NULL, "/usr/local/lib",
	                              dir, sizeof(dir)));

	/* A directory longer than the buffer the caller offered. The file name
	 * after it is what makes the long part the directory. */
	memset(deep, 'a', sizeof(deep) - 1);
	deep[0] = '/';
	strcpy(deep + sizeof(deep) - 1 - strlen("/libbtbb.so.1"),
	       "/libbtbb.so.1");
	mu_assert_int_eq(0, elsewhere(deep, NULL, "/usr/local/lib", dir,
	                              sizeof(dir)));
}

/* The root directory is one slash, not none, so a library sitting in it is
 * compared rather than passed over. */
MU_TEST(the_root_directory_is_a_directory)
{
	char dir[DIR_LEN];

	mu_assert_int_eq(1, elsewhere("/libbtbb.so.1", NULL, "/usr/local/lib",
	                              dir, sizeof(dir)));
	mu_assert_dir_eq("/", dir);
	mu_assert_int_eq(0, elsewhere("/libbtbb.so.1", NULL, "/", dir,
	                              sizeof(dir)));
}

/*
 * One directory under two names: a prefix reached through a symbolic link,
 * which is also what /lib against /usr/lib is on a system that merged them.
 * Warning about those would warn about every correct build on such a machine.
 */
MU_TEST(one_directory_under_two_names_is_one_install)
{
	char dir[DIR_LEN];
	char real[DIR_LEN], link[DIR_LEN], loaded[DIR_LEN];
	const char *tmp = getenv("TMPDIR");
	char work[DIR_LEN];

	snprintf(work, sizeof(work), "%s/ubertooth-origin-test-%ld",
	         tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp", (long)getpid());
	snprintf(real, sizeof(real), "%s/real", work);
	snprintf(link, sizeof(link), "%s/link", work);
	mkdir(work, 0700);
	mkdir(real, 0700);
	unlink(link);
	mu_assert_int_eq(0, symlink(real, link));
	snprintf(loaded, sizeof(loaded), "%s/libbtbb.so.1", link);

	mu_assert_int_eq(0, elsewhere(loaded, NULL, real, dir, sizeof(dir)));

	unlink(link);
	rmdir(real);
	rmdir(work);
}

/* A directory name that does not resolve -- removed since the build, or one
 * this process may not look at -- is compared as written. */
MU_TEST(a_directory_that_does_not_resolve_is_compared_as_written)
{
	char dir[DIR_LEN];

	mu_assert_int_eq(0, elsewhere("/no/such/place/libbtbb.so.1", NULL,
	                              "/no/such/place", dir, sizeof(dir)));
	mu_assert_int_eq(1, elsewhere("/no/such/place/libbtbb.so.1", NULL,
	                              "/no/other/place", dir, sizeof(dir)));
}

MU_TEST(the_directory_part_is_read_out)
{
	char dir[8];

	mu_assert_int_eq(1, ubertooth_directory_of("/a/b/c", dir, sizeof(dir)));
	mu_assert_dir_eq("/a/b", dir);
	/* No room for the directory and its terminator. */
	mu_assert_int_eq(0, ubertooth_directory_of("/aaaaaaaa/b", dir,
	                                           sizeof(dir)));
	mu_assert_int_eq(0, ubertooth_directory_of("b", dir, sizeof(dir)));
	mu_assert_int_eq(0, ubertooth_directory_of(NULL, dir, sizeof(dir)));
}

int main(void)
{
	MU_RUN(a_second_install_is_reported);
	MU_RUN(the_same_directory_is_not_another_install);
	MU_RUN(an_unknown_end_says_nothing);
	MU_RUN(libbtbb_inside_this_binary_is_not_another_install);
	MU_RUN(a_path_with_no_directory_says_nothing);
	MU_RUN(the_root_directory_is_a_directory);
	MU_RUN(one_directory_under_two_names_is_one_install);
	MU_RUN(a_directory_that_does_not_resolve_is_compared_as_written);
	MU_RUN(the_directory_part_is_read_out);
	return mu_report();
}
