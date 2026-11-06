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

# What to report when git finds no release tag at all, which happens in a
# clone fetched without tags and in a shallow one. The last release of the
# original project is the honest floor -- the tree is at least that new -- and
# it is also what a tree with no git history reports (LATEST_RELEASE in
# host/cmake/set_release.cmake).
UBERTOOTH_LATEST_RELEASE := 2020-12-R1

DIRTY := $(shell git status -s --untracked-files=no 2>/dev/null)
ifneq ($(DIRTY), )
DIRTY_FLAG = *
endif

# Is this a real git checkout of THIS tree? A distribution archive (GitHub
# "Download ZIP", a release tarball, git-archive output) has no .git, so there
# is nothing to ask. One unpacked inside an unrelated repository has that
# repository's .git above it, and its tags are not this project's releases, so
# whether the checkout tracks anything here is asked as well. No file is named,
# so moving these sources does not quietly turn the answer off.
#
# tools/ubertooth-release.sh also passes HAVE_GIT=false on the command line, to
# build the shipped sources as the archive they are about to become.
HAVE_GIT := $(shell git ls-files --error-unmatch . > /dev/null 2>&1 \
	&& git rev-parse --is-inside-work-tree 2>/dev/null)

ifeq ($(HAVE_GIT),true)

# The nearest release tag, named on its own: --abbrev=0 asks for the tag name
# with nothing appended. Then how many commits HEAD is past it, which is 0
# when HEAD is the tag itself. Two plain questions rather than one `git
# describe --long` line to take apart, because a release name has dashes of
# its own and the parsing would have to know how many.
RELEASE_TAG := $(shell git describe --tags --abbrev=0 $(UBERTOOTH_RELEASE_TAG_MATCH) 2>/dev/null)

ifeq ($(RELEASE_TAG),)

# No release tag in reach. The latest release stands in for it, so the version
# still names a release, and the distance is reported as unknown rather than
# counted from the start of history: that count is not the distance from the
# release, and a shallow clone would answer it with its own depth. A clone of
# one commit saying "+1" would read exactly like a real build one commit past
# a release.
VERSION_NOW := $(UBERTOOTH_LATEST_RELEASE)+unknown$(DIRTY_FLAG)

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

# No git history to read. The including makefile uses a version.h shipped
# beside these sources where there is one; this is what it reports when there
# is not, and then the version really is unknown.
VERSION_NOW := unknown

endif
