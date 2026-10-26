/*
 * Tests that each firmware image compiles the shared sources under
 * firmware/common into its own directory.
 *
 * The images do not compile that shared code the same way -- btbr builds it at
 * -O3, the others at -Os -- so an object built for one image is wrong in
 * another. When the objects landed beside their sources, in firmware/common,
 * the second image built in a tree found the first one's objects there, make
 * took them as up to date (it compares timestamps, not compiler flags) and
 * linked them. That image was quietly not the one a clean build produces, and
 * a release build or CI run builds every image in one tree.
 *
 * Building both images twice over, once in each order, and comparing them is
 * how that was found, but it needs the ARM toolchain and several minutes. This
 * scans the makefiles instead. It reads the two that decide where object files
 * go and checks every path runs through per_image, which puts the output of a
 * source above the image directory under the image's own shared/. It then
 * walks the image directories, so an image added later cannot quietly bring
 * its own object paths back.
 *
 * It reads make the way a careful `grep` would: `#` starts a comment, and the
 * phrases it looks for are the ones the makefiles write today. Reformatting
 * one of those lines fails the scan, which is the right direction -- the test
 * says what it could not find rather than passing on nothing.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "minunit.h"

#ifndef FW_DIR
#define FW_DIR ".."
#endif

#define COMMON_MK FW_DIR "/common.mk"
#define BOOTLOADER_MK FW_DIR "/bootloader/Makefile"

/* The one directory under firmware/ that holds no firmware image. */
#define TESTS_DIR "tests"

/*
 * The tree has eight images that build the shared sources: the seven in
 * firmware/Makefile's DIRS, plus the bootloader. The sweep below checks it
 * found at least that many, so a sweep that stopped finding them -- a renamed
 * directory, a wrong path -- fails rather than passing on nothing.
 */
#define IMAGES_EXPECTED 8

/* Longer than any makefile line in the tree, with room to spare. */
#define MAX_LINE 512

static int file_exists(const char *path)
{
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;
	fclose(f);
	return 1;
}

/*
 * Does any line of `path` contain `needle` outside a comment?
 *
 * make has one comment form, `#` to end of line. A `#` in a recipe is a
 * comment to the shell, which is what the makefiles' `@#` notes are, so
 * stripping those too is right: this scan is about what make does, not about
 * what it says.
 */
static int file_has(const char *path, const char *needle)
{
	char line[MAX_LINE];
	FILE *f = fopen(path, "r");
	int found = 0;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *hash = strchr(line, '#');

		if (hash != NULL)
			*hash = '\0';
		if (strstr(line, needle) != NULL) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

/*
 * The shape a makefile needs to keep each image's objects to itself. Each
 * entry is a phrase that must be there and what its absence means.
 */
static const struct {
	const char *needle;
	const char *what;
} per_image_shape[] = {
	/* The substitution itself: the output of a source above the image
	 * directory moves under shared/. */
	{ "patsubst ../%,shared/%", "the per_image path substitution" },
	/* The object and listing lists both go through it, so the build and
	 * clean cannot disagree about where output lands. */
	{ "OBJ = $(call per_image,", "OBJ built through per_image" },
	{ "LST = $(call per_image,", "LST built through per_image" },
	/* The depfiles too. The compiler writes each one beside its object, so
	 * a makefile that looked for them anywhere else would read none. */
	{ "DEPFILES = $(call per_image,", "DEPFILES built through per_image" },
	/* And it must read them back, or editing a header rebuilds nothing and
	 * the image goes stale in its own tree -- the same class of bug from
	 * the other direction. */
	{ "-include $(DEPFILES)", "the depfiles being read back" },
	/* A rule for each kind of source above the image directory. Without
	 * one, make falls back to the in-directory rule and writes the object
	 * beside the shared source again. */
	{ "$(OBJDIR)/shared/%.o : ../%.c", "the rule for a shared C source" },
	{ "$(OBJDIR)/shared/%.o : ../%.cpp", "the rule for a shared C++ source" },
	{ "$(OBJDIR)/shared/%.o : ../%.S", "the rule for a shared assembler source" },
	/* clean must take the tree those rules fill, or the next build in the
	 * tree starts from objects clean left behind. */
	{ "/shared,$(OBJDIR)", "clean removing the shared tree" },
};

/*
 * The substitution behind the bug: `$(SRC:%.c=$(OBJDIR)/%.o)` keeps the
 * source's own directory in the object path, so ../common/ubertooth.c became
 * ../common/ubertooth.o, in the shared directory. Neither object nor listing
 * paths may be written that way again.
 */
static const char *const banned_shape[] = {
	"=$(OBJDIR)/%.o",
	"=$(OBJDIR)/%.lst",
	"=$(OBJDIR)/%.d",
};

static void check_one_makefile(const char *path)
{
	int readable = file_exists(path);
	size_t i;
	char why[320];

	snprintf(why, sizeof(why), "cannot read %s", path);
	mu_assert(readable, why);
	if (!readable)
		return;

	for (i = 0; i < sizeof(per_image_shape) / sizeof(per_image_shape[0]); i++) {
		snprintf(why, sizeof(why), "%s is missing %s", path,
		         per_image_shape[i].what);
		mu_assert(file_has(path, per_image_shape[i].needle), why);
	}
	for (i = 0; i < sizeof(banned_shape) / sizeof(banned_shape[0]); i++) {
		snprintf(why, sizeof(why),
		         "%s still maps a source to %s, which keeps the source's "
		         "own directory in the path",
		         path, banned_shape[i]);
		mu_assert(!file_has(path, banned_shape[i]), why);
	}
}

/* The two makefiles that decide where object files go. */
MU_TEST(test_object_paths_are_per_image)
{
	check_one_makefile(COMMON_MK);
	check_one_makefile(BOOTLOADER_MK);
}

/*
 * Every image that builds the shared sources takes its object paths from one
 * of those two makefiles: six include ../common.mk, and the bootloader has its
 * own because it is the one image written in C++. An image added later has to
 * do the same.
 *
 * A makefile that never mentions ../common is not part of this: firmware/
 * also holds the ToorCon badge, which is a different part with a different
 * toolchain and shares no sources.
 */
MU_TEST(test_every_image_uses_a_scanned_makefile)
{
	DIR *d = opendir(FW_DIR);
	struct dirent *entry;
	int images = 0;
	char why[640];

	mu_assert_fatal(d != NULL, "cannot read " FW_DIR);
	while ((entry = readdir(d)) != NULL) {
		char path[512];

		if (entry->d_name[0] == '.' ||
		    strcmp(entry->d_name, TESTS_DIR) == 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/Makefile", FW_DIR,
		         entry->d_name);
		if (!file_exists(path) || !file_has(path, "../common"))
			continue;
		images++;
		snprintf(why, sizeof(why),
		         "%s builds sources from ../common but neither includes "
		         "../common.mk nor defines per_image itself, so where its "
		         "objects land is unchecked",
		         path);
		mu_assert(file_has(path, "include ../common.mk") ||
		                  file_has(path, "per_image ="),
		          why);
	}
	closedir(d);
	snprintf(why, sizeof(why),
	         "the sweep found %d image makefiles under " FW_DIR
	         " that build the shared sources, fewer than the %d the tree has",
	         images, IMAGES_EXPECTED);
	mu_assert(images >= IMAGES_EXPECTED, why);
}

int main(void)
{
	printf("Firmware per-image object path tests\n");
	MU_RUN(test_object_paths_are_per_image);
	MU_RUN(test_every_image_uses_a_scanned_makefile);
	return mu_report();
}
