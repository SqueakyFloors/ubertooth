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

# Option A: the per-tool "<tool>.md" files in host/doc are the single source;
# the roff "<tool>.1" here are generated from them with ronn. ronn is OPTIONAL:
# if found, regenerate the .1 at build time so man pages track the .md; if not,
# the committed .1 are installed as-is (so `man` works without ronn).
#
# The committed pages were produced by go-md2man rather than ronn, so ronn
# rewrites a page wholesale the first time it regenerates it. Only the pages in
# the foreach below are regenerated; the rest, including ubertooth-debug.1, are
# kept in step with their .md by hand in the same change.
find_program(RONN_EXECUTABLE NAMES ronn ronn-ng)
if(RONN_EXECUTABLE)
	message(STATUS "ronn found: man pages will be regenerated from host/doc/*.md")
	set(DOC_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
	# Regenerate each page from its .md into the source tree so the committed
	# .1 stay in sync. --date keeps the roff date stable (not build-synced).
	foreach(pg ubertooth-afh ubertooth-btle ubertooth-dfu ubertooth-dump
	           ubertooth-ego ubertooth-rx ubertooth-scan ubertooth-specan
	           ubertooth-specan-ui ubertooth-util)
		add_custom_command(
			OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/${pg}.1"
			COMMAND ${RONN_EXECUTABLE} --roff --pipe
			        "${DOC_SRC_DIR}/${pg}.md" > "${CMAKE_CURRENT_SOURCE_DIR}/${pg}.1"
			DEPENDS "${DOC_SRC_DIR}/${pg}.md"
			COMMENT "ronn: ${pg}.md -> ${pg}.1")
	endforeach()
	add_custom_command(
		OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/ubertooth.7"
		COMMAND ${RONN_EXECUTABLE} --roff --pipe
		        "${DOC_SRC_DIR}/ubertooth.md" > "${CMAKE_CURRENT_SOURCE_DIR}/ubertooth.7"
		DEPENDS "${DOC_SRC_DIR}/ubertooth.md"
		COMMENT "ronn: ubertooth.md -> ubertooth.7")
	add_custom_target(manpages ALL
		DEPENDS
			"${CMAKE_CURRENT_SOURCE_DIR}/ubertooth-util.1"
			"${CMAKE_CURRENT_SOURCE_DIR}/ubertooth.7")
else()
	message(STATUS "ronn not found: installing committed man pages as-is "
	               "(edit host/doc/*.md then regenerate with ronn)")
endif()

install(FILES
	"ubertooth-btle.1"
	"ubertooth-debug.1"
	"ubertooth-dump.1"
	"ubertooth-rx.1"
	"ubertooth-specan.1"
	"ubertooth-specan-ui.1"
	"ubertooth-afh.1"
	"ubertooth-dfu.1"
	"ubertooth-ego.1"
	"ubertooth-scan.1"
	"ubertooth-util.1"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT doc)

install(FILES
	"ubertooth.7"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man7" COMPONENT doc)
