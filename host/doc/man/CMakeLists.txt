#
# Copyright 2017 Dominic Spill
#
# This file is part of Ubertooth.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.
#

include(GNUInstallDirs)

# The per-tool "<tool>.md" files in host/doc are the single source; the roff
# "<tool>.1" here are generated from them with ronn and committed, so that
# `man` and `make install` work on a machine without ronn. A committed page
# is exactly what a refresh writes, and a CI job checks that, so a .md edit
# that lands without one is caught.
#
# A build never writes to the source tree, and never needs ronn: the committed
# pages are the ones installed. Refreshing them is a separate, deliberate step
# after editing a .md:
#
#     cmake --build <build dir> --target refresh-manpages
#
# which renders the pages into the build directory and copies them over the
# committed ones; review the diff and commit the .1 with the .md. The
# `manpages` target renders them into the build directory only, for a look at
# what a refresh would write. Neither target is part of `all`.
#
# Each entry is name|section|manual, the manual being the page's top-centre
# heading; with the organization below it makes the .TH line of the committed
# pages, which ronn would otherwise leave empty.
set(MAN_PAGES_FROM_MARKDOWN
	"btle-extcap|1|User Commands"
	"ubertooth-afh|1|User Commands"
	"ubertooth-btbr|1|User Commands"
	"ubertooth-btle|1|User Commands"
	"ubertooth-dfu|1|User Commands"
	"ubertooth-ducky|1|User Commands"
	"ubertooth-dump|1|User Commands"
	"ubertooth-ego|1|User Commands"
	"ubertooth-follow|1|User Commands"
	"ubertooth-rx|1|User Commands"
	"ubertooth-scan|1|User Commands"
	"ubertooth-specan|1|User Commands"
	"ubertooth-specan-ui|1|User Commands"
	"ubertooth-tx|1|User Commands"
	"ubertooth-util|1|User Commands"
	"ubertooth|7|Project Ubertooth Overview")

# Pages with no generated counterpart: edited here by hand, in step with their
# .md in the same change. Installed like the rest.
set(MAN_PAGES_BY_HAND
	"ubertooth-debug|1|User Commands")

set(MAN_ORGANIZATION "Project Ubertooth")

# The one supported generator version, and the only place it is decided: the
# CI man-page job reads this line to know what to install. Two versions of
# ronn render the same .md differently, so the committed pages only match what
# a refresh writes when everyone runs this one; any other, and the refresh
# refuses rather than rewrite all sixteen pages in a style nobody asked for.
set(RONN_VERSION "0.10.1")

# The date in the page footer. ronn defaults it to the .md's modification
# time, which a fresh checkout sets to the time of the checkout: the pages
# would then differ from the committed ones purely by the month they were
# rendered in, and no check could tell that from a real change. Passing it
# makes a refresh reproducible. Bump it when refreshing the pages for a
# release; an ordinary .md edit does not need it moved.
set(MAN_DATE "2026-10-25")

# Stand-ins for `manpages` and `refresh-manpages` when nothing can render a
# page. They exist so the documented command explains itself instead of
# failing with "No rule to make target", and they fail, because a refresh that
# wrote nothing did not do what was asked: a check that runs the target cannot
# then go green having refreshed nothing. The reason is baked into a script
# rather than passed to the shell, so quotes and parentheses in it are safe.
function(man_refusing_targets reason)
	set(script "${CMAKE_CURRENT_BINARY_DIR}/man-refresh-refused.cmake")
	file(WRITE "${script}" "message(FATAL_ERROR \"${reason}\")\n")
	foreach(target manpages refresh-manpages)
		add_custom_target(${target} COMMAND ${CMAKE_COMMAND} -P "${script}")
	endforeach()
endfunction()

# Why a refresh cannot run, if it cannot: ronn missing, or the wrong version.
# Anything but the pinned version renders different roff, which a refresh
# would write over all sixteen pages at once.
set(MAN_REFUSE_REASON "")
find_program(RONN_EXECUTABLE NAMES ronn ronn-ng)
if(NOT RONN_EXECUTABLE)
	message(STATUS "ronn not found: the committed man pages are installed as "
	               "they are (ronn-ng ${RONN_VERSION} is needed only to "
	               "refresh them after editing host/doc/*.md)")
	set(MAN_REFUSE_REASON
		"ronn is not installed, so no man page was rendered: install it (gem install ronn-ng -v ${RONN_VERSION}) and configure again")
else()
	# The version is pulled out and compared whole: the pinned string on its
	# own is a regular expression, and a substring, that a later 0.10.10 would
	# match.
	execute_process(COMMAND ${RONN_EXECUTABLE} --version
		OUTPUT_VARIABLE ronn_version_output
		ERROR_VARIABLE ronn_version_output
		OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(ronn_version_output MATCHES "([0-9]+\\.[0-9]+(\\.[0-9]+)?)")
		set(ronn_version "${CMAKE_MATCH_1}")
	else()
		set(ronn_version "unknown")
	endif()
	if(ronn_version STREQUAL RONN_VERSION)
		message(STATUS "ronn ${ronn_version} found: refresh-manpages can "
		               "regenerate the committed man pages from host/doc/*.md")
	else()
		# A warning, not an error: having the wrong ronn installed must not
		# break the host build for someone who never refreshes a page. The
		# refusal is on the refresh, where the damage would be done.
		message(WARNING "ronn reports version ${ronn_version} "
		                "(\"${ronn_version_output}\"), but the committed man "
		                "pages are rendered with ronn-ng ${RONN_VERSION}: "
		                "refresh-manpages will refuse to run "
		                "(gem install ronn-ng -v ${RONN_VERSION})")
		set(MAN_REFUSE_REASON
			"ronn is version ${ronn_version}, not the ${RONN_VERSION} the committed pages are rendered with, so no man page was rendered: install that one (gem install ronn-ng -v ${RONN_VERSION}) and configure again, or change RONN_VERSION in host/doc/man/CMakeLists.txt to adopt this version for every page")
	endif()
endif()

if(MAN_REFUSE_REASON)
	man_refusing_targets("${MAN_REFUSE_REASON}")
else()
	set(DOC_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
	set(GENERATED_MAN_PAGES "")
	foreach(spec ${MAN_PAGES_FROM_MARKDOWN})
		string(REPLACE "|" ";" parts "${spec}")
		list(GET parts 0 pg)
		list(GET parts 1 section)
		list(GET parts 2 manual)
		set(generated "${CMAKE_CURRENT_BINARY_DIR}/${pg}.${section}")
		add_custom_command(
			OUTPUT "${generated}"
			COMMAND ${RONN_EXECUTABLE} --roff --pipe
			        "--organization=${MAN_ORGANIZATION}"
			        "--manual=${manual}"
			        "--date=${MAN_DATE}"
			        "${DOC_SRC_DIR}/${pg}.md" > "${generated}"
			DEPENDS "${DOC_SRC_DIR}/${pg}.md"
			COMMENT "ronn: ${pg}.md -> ${pg}.${section}")
		list(APPEND GENERATED_MAN_PAGES "${generated}")
	endforeach()

	add_custom_target(manpages DEPENDS ${GENERATED_MAN_PAGES})

	add_custom_target(refresh-manpages
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GENERATED_MAN_PAGES}
		        "${CMAKE_CURRENT_SOURCE_DIR}"
		DEPENDS ${GENERATED_MAN_PAGES}
		COMMENT "refreshing the committed man pages in host/doc/man")
endif()

# One list per section, from the generated and the hand-kept pages together.
set(MAN1_PAGE_FILES "")
set(MAN7_PAGE_FILES "")
foreach(spec ${MAN_PAGES_FROM_MARKDOWN} ${MAN_PAGES_BY_HAND})
	string(REPLACE "|" ";" parts "${spec}")
	list(GET parts 0 pg)
	list(GET parts 1 section)
	# A page in a section with no install() below would configure cleanly and
	# never be installed, which is how ubertooth-debug.1 went missing.
	if(NOT section STREQUAL "1" AND NOT section STREQUAL "7")
		message(FATAL_ERROR "man page ${pg} is in section ${section}, which "
		                    "nothing installs: add an install() for it")
	endif()
	list(APPEND MAN${section}_PAGE_FILES "${pg}.${section}")
endforeach()

install(FILES ${MAN1_PAGE_FILES}
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT doc)

install(FILES ${MAN7_PAGE_FILES}
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man7" COMPONENT doc)
