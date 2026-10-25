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
# `man` and `make install` work on a machine without ronn.
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
	"ubertooth-afh|1|User Commands"
	"ubertooth-btle|1|User Commands"
	"ubertooth-dfu|1|User Commands"
	"ubertooth-dump|1|User Commands"
	"ubertooth-ego|1|User Commands"
	"ubertooth-rx|1|User Commands"
	"ubertooth-scan|1|User Commands"
	"ubertooth-specan|1|User Commands"
	"ubertooth-specan-ui|1|User Commands"
	"ubertooth-util|1|User Commands"
	"ubertooth|7|Project Ubertooth Overview")

# Pages with no generated counterpart: edited here by hand, in step with their
# .md in the same change. Installed like the rest.
set(MAN_PAGES_BY_HAND
	"ubertooth-debug|1|User Commands")

set(MAN_ORGANIZATION "Project Ubertooth")

find_program(RONN_EXECUTABLE NAMES ronn ronn-ng)
if(RONN_EXECUTABLE)
	message(STATUS "ronn found: refresh-manpages can regenerate the committed "
	               "man pages from host/doc/*.md")
	set(DOC_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
	set(GENERATED_MAN_PAGES "")
	foreach(spec ${MAN_PAGES_FROM_MARKDOWN})
		string(REPLACE "|" ";" parts "${spec}")
		list(GET parts 0 pg)
		list(GET parts 1 section)
		list(GET parts 2 manual)
		set(generated "${CMAKE_CURRENT_BINARY_DIR}/${pg}.${section}")
		# The committed pages were produced by go-md2man rather than ronn, so
		# the first refresh of a page rewrites its body wholesale in ronn's
		# style. The .TH fields stay as they are.
		add_custom_command(
			OUTPUT "${generated}"
			COMMAND ${RONN_EXECUTABLE} --roff --pipe
			        "--organization=${MAN_ORGANIZATION}"
			        "--manual=${manual}"
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
else()
	message(STATUS "ronn not found: the committed man pages are installed as "
	               "they are (needed only to refresh them after editing "
	               "host/doc/*.md)")
	# Both targets still exist, so the documented command explains itself
	# instead of failing with "No rule to make target".
	foreach(target manpages refresh-manpages)
		add_custom_target(${target}
			COMMAND ${CMAKE_COMMAND} -E echo
			        "ronn is not installed, so nothing was rendered: install it"
			        "(gem install ronn-ng) and run this again")
	endforeach()
endif()

# One list per section, from the generated and the hand-kept pages together.
set(MAN1_PAGE_FILES "")
set(MAN7_PAGE_FILES "")
foreach(spec ${MAN_PAGES_FROM_MARKDOWN} ${MAN_PAGES_BY_HAND})
	string(REPLACE "|" ";" parts "${spec}")
	list(GET parts 0 pg)
	list(GET parts 1 section)
	list(APPEND MAN${section}_PAGE_FILES "${pg}.${section}")
endforeach()

install(FILES ${MAN1_PAGE_FILES}
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT doc)

install(FILES ${MAN7_PAGE_FILES}
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man7" COMPONENT doc)
