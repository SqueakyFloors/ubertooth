#
# Copyright 2013 Dominic Spill
#
# This file is part of Ubertooth
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

# Based heavily upon the hackrf cmake setup.

# This directory is also configured on its own, to build the tools against an
# installed libubertooth, and CMake wants the version range from whichever
# file is the top-level one: without this, CMake 4 refuses to generate at all.
#
# The same range as host/CMakeLists.txt, written out again rather than shared:
# CMake counts the command only where the top-level file itself calls it, and
# refuses an include() that makes the call ("No cmake_minimum_required command
# is present"). A build through the top level does not read this call, so the
# two cannot disagree about anything a whole-tree build does; raising the
# floor means raising it here as well, and in COMPATIBILITY.md.
cmake_minimum_required(VERSION 3.5...3.27)

project(ubertooth-tools C)
include(CTest)
set(PACKAGE ubertooth-tools)

# What a build of the whole tree gets from host/CMakeLists.txt and a build of
# this directory on its own has to set for itself.
if(NOT ubertooth_all_SOURCE_DIR)
	# The Find modules for libbtbb, libusb and libubertooth live beside this
	# directory. Without this the first find_package() below failed and the
	# caller had to know where they were kept.
	set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake/modules
		CACHE STRING "CMake module path")
	# Ask glibc for the interfaces these sources use: usleep(), which POSIX
	# dropped in 2008, and the byte-order helpers in <endian.h>. Without it a
	# strict standard (-std=c17) hides both and the build fails, as it did
	# for the whole tree until host/CMakeLists.txt asked for the macro by
	# name; it goes unnoticed because compilers default to a GNU standard,
	# which sets the macro for us. That file says more about the choice.
	add_definitions( -D_DEFAULT_SOURCE )
endif()

if(MSVC OR APPLE)
    set(USE_OWN_GNU_GETOPT 1)
endif()

add_subdirectory(src)

if(BUILD_TESTING)
	add_subdirectory(tests)
endif()

# Create uninstall target
if(NOT ubertooth_all_SOURCE_DIR)
configure_file(
    ${PROJECT_SOURCE_DIR}/../cmake/cmake_uninstall.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
@ONLY)

add_custom_target(uninstall
    ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
)
endif()