# The version string a build of this tree reports: the release it is, or how
# far past a release it is. Included by common.mk for the firmware images and
# by the bootloader's Makefile, and matched by host/cmake/set_release.cmake,
# so the firmware, the library and every tool report the same version.
#
# A reader wants to know which release they are running, so the string names a
# release rather than a commit:
#
#   2026-11-RC1      built from that release
#   2026-11-RC1+22   22 commits after that release
#   2026-11-RC1+22*  the same, with uncommitted changes in the tree
#
# Release tags are named YYYY-MM-Rn, with YYYY-MM-RCn and YYYY-MM-DEVn for the
# candidate and development tags that come before one, so those are the three
# patterns git is asked to look for. The repository holds other tags, which
# name no release and must not be reported as one.
UBERTOOTH_RELEASE_TAG_MATCH := \
	--match '[0-9][0-9][0-9][0-9]-[0-9][0-9]-R[0-9]*' \
	--match '[0-9][0-9][0-9][0-9]-[0-9][0-9]-RC[0-9]*' \
	--match '[0-9][0-9][0-9][0-9]-[0-9][0-9]-DEV[0-9]*'

DIRTY := $(shell git status -s --untracked-files=no 2>/dev/null)
ifneq ($(DIRTY), )
DIRTY_FLAG = *
endif

# Is this a real git checkout of THIS tree? A distribution archive (GitHub
# "Download ZIP", a release tarball, git-archive output) has no .git, so there
# is nothing to ask. These sources inside another repository -- unpacked
# there, or committed there as a vendored copy -- have that repository's .git
# above them, and its tags are not this project's releases. What tells the two
# apart is where this makefile sits in the checkout git answered about: in a
# checkout of this project it is in firmware/ and nowhere else.
#
# tools/ubertooth-release.sh also passes HAVE_GIT=false on the command line, to
# build the shipped sources as the archive they are about to become.
VERSION_MK_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
HAVE_GIT := $(shell cd '$(VERSION_MK_DIR)' 2>/dev/null \
	&& test "`git rev-parse --show-prefix 2>/dev/null`" = firmware/ \
	&& git rev-parse --is-inside-work-tree 2>/dev/null)

ifeq ($(HAVE_GIT),true)

# The nearest release tag, named on its own: --abbrev=0 asks for the tag name
# with nothing appended. Then how many commits HEAD is past it, which is 0
# when HEAD is the tag itself. Two plain questions rather than one `git
# describe --long` line to take apart, because a release name has dashes of
# its own and the parsing would have to know how many.
# A glob cannot say "digits and then nothing", so the name git found is
# checked as well: "2026-11-R1-local" matches the first pattern above and is
# not a release. A name that does not hold up leaves this empty, which is the
# no-release-tag branch below.
RELEASE_TAG := $(shell git describe --tags --abbrev=0 $(UBERTOOTH_RELEASE_TAG_MATCH) 2>/dev/null \
	| grep -E '^[0-9]{4}-[0-9]{2}-(R|RC|DEV)[0-9]+$$')

ifeq ($(RELEASE_TAG),)

# No release tag in reach -- a clone fetched without tags, a shallow one, a
# history with none yet. git names no release here, and counting from the
# start of history would not be the distance from one: a shallow clone would
# answer that with its own depth, and a clone of one commit saying "+1" would
# read exactly like a real build one commit past a release.
#
# The archive's release is not read here either: git placed this tree in a
# checkout of its own, and a RELEASE-VERSION beside it was written for some
# other source -- an archive unpacked and committed to a repository of its own
# carries the release the archive was made from, and this checkout's HEAD is
# not that.
VERSION_NOW := unknown

else

RELEASE_AHEAD := $(shell git rev-list --count $(RELEASE_TAG)..HEAD 2>/dev/null)
# Empty when there was nothing to count, in a checkout with no commit in it.
# That is the same answer as none: the release name on its own.
ifeq ($(RELEASE_AHEAD),)
RELEASE_AHEAD := 0
endif

ifeq ($(RELEASE_AHEAD),0)
VERSION_NOW := $(RELEASE_TAG)$(DIRTY_FLAG)
else
VERSION_NOW := $(RELEASE_TAG)+$(RELEASE_AHEAD)$(DIRTY_FLAG)
endif

endif

else

# No git history to read, so these sources came out of an archive if they came
# from anywhere, and the release it carries is in RELEASE-VERSION at the top of
# the tree; git archive fills it in as the archive is made, and see that file.
# The including makefile uses a version.h shipped beside these sources where
# there is one, which is what a release archive carries; this is what it
# reports when there is not.
#
# The first line of the file that is neither blank nor a comment is the answer
# git wrote: the nearest release tag, with "-<commits>-g<commit>" after it
# where the commit archived is past the tag. A release name has dashes of its
# own, so the count is taken off the end rather than the name read out of the
# front, and what is left has to be a release name. An untouched placeholder,
# which is what a git checkout has, is not one.
#
# Surrounding blanks and a carriage return are taken off the line first, so
# that a file edited by hand, or carried through a download that rewrote its
# line endings, reads the same here as in host/cmake/set_release.cmake, which
# strips the line as CMake's file(STRINGS) gives it. The two have to agree, or
# a board and the tools that talk to it would name different releases.
#
# No release this file could carry instead would be better than "unknown": the
# distance from it would not be known, and whoever edited it last would have
# decided what every archive claimed.
ARCHIVE_VERSION_FILE := $(VERSION_MK_DIR)/../RELEASE-VERSION
ARCHIVE_RELEASE := $(shell \
	sed -e '/^[[:space:]]*\#/d' -e '/^[[:space:]]*$$/d' -e q \
		'$(ARCHIVE_VERSION_FILE)' 2>/dev/null \
	| tr -d '\r' \
	| sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$$//' \
	| sed -E -e 's/-([0-9]+)-g[0-9a-f]+$$/+\1/' \
	| grep -E '^[0-9]{4}-[0-9]{2}-(R|RC|DEV)[0-9]+(\+[0-9]+)?$$')

ifeq ($(ARCHIVE_RELEASE),)
VERSION_NOW := unknown
else
VERSION_NOW := $(ARCHIVE_RELEASE)
endif

endif
