# What this tree is, as git sees it, and the two build settings that follow:
#
#   RELEASE_STRING     the version string a build reports -- the release this
#                      tree is, or how far past a release it is, with a * when
#                      the checkout has uncommitted changes:
#
#                        2026-11-RC1      built from that release
#                        2026-11-RC1+22   22 commits after that release
#                        2026-11-RC1+22*  the same, with local changes
#
#                      It names a release rather than a commit, because that
#                      is what a user reporting a problem can read off their
#                      own build. The firmware works the same string out the
#                      same way, in firmware/version.mk, so a board and the
#                      tools that talk to it report one version between them.
#
#   UBERTOOTH_RELEASE  ON for a release build, OFF for a development build.
#                      A development build warns about firmware of another API
#                      version and runs the command; a release build refuses
#                      it, so host tools that shipped never run against
#                      firmware they were not written for.
#
# Both rest on the same question -- is this a git checkout, and which release
# is it -- so they are worked out here, together, from one look at git. They
# used to be worked out in two places, which could come to disagree about
# that.
#
# A build counts as a development build only where that can be established: a
# git checkout whose HEAD is not a release tag. Everything else -- a release
# tag, a source archive with no git history, a tree where git will not run --
# is a release build, so nothing that ships with no history to read loses the
# guard. The probe is not proof: a clone made without tags reads a tagged
# commit as a development build. Nothing here does that -- a release goes out
# through git archive, with no .git at all -- and the override below settles
# it where it matters.
#
# Either setting can be decided from the command line instead:
# -DRELEASE_STRING="your version" and -DUBERTOOTH_RELEASE=ON|OFF. CI passes
# UBERTOOTH_RELEASE=ON, so what CI builds always refuses a mismatch. It is not
# an option(), which would define the variable on the first configure and
# leave the probe below with nothing to decide.
#
# LATEST_RELEASE is the version string a tree with no git history reports;
# developers should set it before a release is tagged. A checkout with no
# release tag in reach -- a clone fetched without tags, a shallow one --
# reports it with "+unknown" after it: that names the release the tree is at
# least as new as, and says plainly that the distance is not known. A count
# from the start of history would be a different number, and a shallow clone
# would answer it with its own depth.

set(LATEST_RELEASE "2020-12-R1")

# Release tags are named YYYY-MM-Rn, with YYYY-MM-RCn and YYYY-MM-DEVn for the
# candidate and development tags that come before one, so those are the three
# patterns git is asked to look for; it takes a --match for each. The
# repository holds other tags, which name no release and must not be reported
# as one. firmware/version.mk uses the same three.
set(RELEASE_TAG_MATCH
	--match "[0-9][0-9][0-9][0-9]-[0-9][0-9]-R[0-9]*"
	--match "[0-9][0-9][0-9][0-9]-[0-9][0-9]-RC[0-9]*"
	--match "[0-9][0-9][0-9][0-9]-[0-9][0-9]-DEV[0-9]*")

# A glob cannot say "digits and then nothing", so the name git found is
# checked here as well: "2026-11-R1-local" matches the first glob above and is
# not a release.
set(RELEASE_TAG_FORM "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-(R|RC|DEV)[0-9]+$")

# A setting given as an empty value says nothing, so it is read as not given
# rather than taken as the answer. An empty UBERTOOTH_RELEASE would otherwise
# leave a build that asked for a release build reading as a development one,
# which is the direction that matters: it warns about firmware of another API
# version where it should refuse it.
foreach(setting RELEASE_STRING UBERTOOTH_RELEASE)
	if(DEFINED ${setting} AND "${${setting}}" STREQUAL "")
		unset(${setting})
		# A -D on the command line is a cache entry, which the line
		# above does not reach.
		unset(${setting} CACHE)
	endif()
endforeach()

# Is there git history to read, and is it this tree's? Asked from this file's
# own directory, so the answer is about this checkout whoever includes the
# file and whatever their PROJECT_SOURCE_DIR is -- and so the version string's
# * is about this checkout rather than about whatever tree the caller's
# working directory was in.
#
# git looks up the directory tree, so these sources inside another repository
# -- unpacked there, or committed there as a vendored copy -- would be given
# that repository's release, and its tags are not this project's. What tells
# the two apart is where this file sits in the checkout git answered about:
# the question is asked from this directory, and in a checkout of this project
# this directory is host/cmake and nothing else. Anywhere else, the answer is
# some other repository's and is not used.
#
# Not asked at all when both settings are given on the command line: a tree
# where git is blocked rather than absent would otherwise have to wait for it
# to fail, for an answer nothing goes on to use.
if(NOT DEFINED RELEASE_STRING OR NOT DEFINED UBERTOOTH_RELEASE)
	execute_process(
		COMMAND git rev-parse --show-prefix
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		RESULT_VARIABLE GIT_DIR_ERROR
		OUTPUT_VARIABLE GIT_PREFIX
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
	if(NOT GIT_DIR_ERROR AND NOT "${GIT_PREFIX}" STREQUAL "host/cmake/")
		set(GIT_DIR_ERROR 1)
	endif()
endif()

# The nearest release tag, named on its own (--abbrev=0 asks for the tag name
# with nothing appended), and how many commits HEAD is past it. Two plain
# questions rather than one `git describe --long` line to take apart, because
# a release name has dashes of its own and the parsing would have to know how
# many. RELEASE_AHEAD of 0 means HEAD is the release tag itself, which is both
# the version string to report and the answer to the release-build question.
if(NOT GIT_DIR_ERROR AND (NOT DEFINED RELEASE_STRING OR NOT DEFINED UBERTOOTH_RELEASE))
	execute_process(
		COMMAND git describe --tags --abbrev=0 ${RELEASE_TAG_MATCH}
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		RESULT_VARIABLE GIT_DESCRIBE_ERROR
		ERROR_QUIET
		OUTPUT_VARIABLE RELEASE_TAG
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(NOT GIT_DESCRIBE_ERROR AND NOT RELEASE_TAG MATCHES "${RELEASE_TAG_FORM}")
		# A tag the glob let through that is not a release name after
		# all. Read as no release tag, rather than reported as the
		# release this build came from.
		set(GIT_DESCRIBE_ERROR 1)
	endif()
	if(GIT_DESCRIBE_ERROR)
		# No release tag in reach. The tree is at least as new as the
		# last release there was, and how much newer is not known.
		set(RELEASE_TAG "${LATEST_RELEASE}")
		set(RELEASE_AHEAD "unknown")
	else()
		execute_process(
			COMMAND git rev-list --count ${RELEASE_TAG}..HEAD
			WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
			RESULT_VARIABLE GIT_COUNT_ERROR
			ERROR_QUIET
			OUTPUT_VARIABLE RELEASE_AHEAD
			OUTPUT_STRIP_TRAILING_WHITESPACE
		)
		if(GIT_COUNT_ERROR)
			# Nothing to count, in a checkout with no commit in it.
			# That is the same answer as none: the release name on
			# its own.
			set(RELEASE_AHEAD "0")
		endif()
	endif()
endif()

if(NOT DEFINED RELEASE_STRING)
	if(GIT_DIR_ERROR)
		set(RELEASE_STRING ${LATEST_RELEASE})
	else()
		# A git that cannot answer here leaves the flag off rather than
		# printing to the configure output: the release is already
		# known, so the version string is right either way.
		execute_process(
			COMMAND git status -s --untracked-files=no
			WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
			ERROR_QUIET
			OUTPUT_VARIABLE DIRTY
		)
		if(NOT "${DIRTY}" STREQUAL "")
			set(DIRTY_FLAG "*")
		else()
			set(DIRTY_FLAG "")
		endif()
		if("${RELEASE_AHEAD}" STREQUAL "0")
			set(RELEASE_STRING "${RELEASE_TAG}${DIRTY_FLAG}")
		else()
			set(RELEASE_STRING "${RELEASE_TAG}+${RELEASE_AHEAD}${DIRTY_FLAG}")
		endif()
	endif()
endif()

if(NOT DEFINED UBERTOOTH_RELEASE)
	if(GIT_DIR_ERROR)
		set(UBERTOOTH_RELEASE ON)
	elseif(NOT GIT_DESCRIBE_ERROR AND "${RELEASE_AHEAD}" STREQUAL "0")
		set(UBERTOOTH_RELEASE ON)
	else()
		set(UBERTOOTH_RELEASE OFF)
	endif()
endif()

# The two answers are what this file is for; what it asked git along the way
# is not, and these names are generic enough to collide in a directory that
# inherited them.
unset(LATEST_RELEASE)
unset(RELEASE_TAG_MATCH)
unset(RELEASE_TAG_FORM)
unset(RELEASE_TAG)
unset(RELEASE_AHEAD)
unset(GIT_DIR_ERROR)
unset(GIT_PREFIX)
unset(GIT_DESCRIBE_ERROR)
unset(GIT_COUNT_ERROR)
unset(DIRTY)
unset(DIRTY_FLAG)
