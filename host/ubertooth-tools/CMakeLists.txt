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
# installed libubertooth, so it needs its own version range rather than
# inheriting the one host/CMakeLists.txt declares. The same range, so a build
# through the top level is unchanged. Without it, CMake 4 refuses to generate
# for a top-level project that never said which version it was written for.
cmake_minimum_required(VERSION 3.5...3.27)

project(ubertooth-tools C)
include(CTest)
set(PACKAGE ubertooth-tools)

# The Find modules for libbtbb, libusb and libubertooth live beside this
# directory, and host/CMakeLists.txt points at them for a build of the whole
# tree. Configured on its own, nothing had, so the first find_package() below
# failed and the caller had to know where they were kept.
if(NOT ubertooth_all_SOURCE_DIR)
	set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake/modules
		CACHE STRING "CMake module path")
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