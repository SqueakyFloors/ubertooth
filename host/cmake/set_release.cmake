# What this tree is, as git sees it, and the two build settings that follow:
#
#   RELEASE_STRING     the version string a build reports -- git-<hash>, with
#                      a * when the checkout has uncommitted changes
#   UBERTOOTH_RELEASE  ON for a release build, OFF for a development build.
#                      A development build warns about firmware of another API
#                      version and runs the command; a release build refuses
#                      it, so host tools that shipped never run against
#                      firmware they were not written for.
#
# Both answer the same question -- is this a git checkout, and what is it --
# so they are worked out here, together, from one look at git. They used to be
# worked out in two places, which could come to disagree.
#
# A build counts as a development build only where that can be established: a
# git checkout whose HEAD is not a tag. Everything else -- a tag, a source
# archive with no git history, a tree where git will not run -- is a release
# build, so nothing that ships with no history to read loses the guard. The
# probe is not proof: a clone made without tags reads a tagged commit as a
# development build. Nothing here does that -- a release goes out through git
# archive, with no .git at all -- and the override below settles it where it
# matters.
#
# Either setting can be decided from the command line instead:
# -DRELEASE_STRING="your version" and -DUBERTOOTH_RELEASE=ON|OFF. CI passes
# UBERTOOTH_RELEASE=ON, so what CI builds always refuses a mismatch. It is not
# an option(), which would define the variable on the first configure and
# leave the probe below with nothing to decide.
#
# LATEST_RELEASE is the version string a tree with no git history reports;
# developers should set it before a release is tagged.

set(LATEST_RELEASE "2020-12-R1")

# Is there git history to read at all? Asked from this file's own directory,
# so the answer is about this checkout whoever includes the file and whatever
# their PROJECT_SOURCE_DIR is.
execute_process(
	COMMAND git rev-parse --git-dir
	WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
	RESULT_VARIABLE GIT_DIR_ERROR
	OUTPUT_QUIET
	ERROR_QUIET
)

if(NOT DEFINED RELEASE_STRING)
	if(GIT_DIR_ERROR)
		set(RELEASE_STRING ${LATEST_RELEASE})
	else()
		execute_process(
			COMMAND git log -n 1 --format=%h
			WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
			RESULT_VARIABLE GIT_LOG_ERROR
			ERROR_QUIET
			OUTPUT_VARIABLE GIT_VERSION
			OUTPUT_STRIP_TRAILING_WHITESPACE
		)
		if(GIT_LOG_ERROR)
			# A checkout with no commit in it has no hash to report.
			set(RELEASE_STRING ${LATEST_RELEASE})
		else()
			execute_process(
				COMMAND git status -s --untracked-files=no
				WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
				OUTPUT_VARIABLE DIRTY
			)
			if(NOT "${DIRTY}" STREQUAL "")
				set(DIRTY_FLAG "*")
			else()
				set(DIRTY_FLAG "")
			endif()
			set(RELEASE_STRING "git-${GIT_VERSION}${DIRTY_FLAG}")
		endif()
	endif()
endif()

if(NOT DEFINED UBERTOOTH_RELEASE)
	if(GIT_DIR_ERROR)
		set(UBERTOOTH_RELEASE ON)
	else()
		execute_process(
			COMMAND git describe --tags --exact-match HEAD
			WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
			RESULT_VARIABLE GIT_TAG_ERROR
			OUTPUT_QUIET
			ERROR_QUIET
		)
		if(GIT_TAG_ERROR)
			set(UBERTOOTH_RELEASE OFF)
		else()
			set(UBERTOOTH_RELEASE ON)
		endif()
	endif()
endif()
