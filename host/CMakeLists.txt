#
# Copyright 2013 Dominic Spill
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
#top level cmake project for ubertooth lib + tools

cmake_minimum_required(VERSION 3.5...3.27)
project(ubertooth_all)
include(CTest)
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules
	CACHE STRING "CMake module path")

set(MAJOR_VERSION 1 CACHE STRING "libUbertooth major version number")
set(MINOR_VERSION 1 CACHE STRING "libUbertooth minor version number")
set( VERSION ${MAJOR_VERSION}.${MINOR_VERSION} )
add_definitions( -DVERSION="${VERSION}" )

# The version string this build reports, and whether it is a release build or
# a development build. Both come from what git says this tree is, so one file
# works them out together; it also documents the two options that decide them
# from the command line.
#
# What the release build decides: the host requires firmware of exactly its
# own API version. A release build refuses anything else, as every build used
# to -- someone running a release has no reason to drive firmware these tools
# were not written for -- while a development build warns and runs the
# command, so host tools and firmware from different commits can be used
# together without a reflash first.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/set_release.cmake)
message(STATUS "Setting version string ${RELEASE_STRING}")
if(UBERTOOTH_RELEASE)
	set(UBERTOOTH_RELEASE_BUILD 1)
	message(STATUS "Release build: firmware of another API version is refused")
else()
	set(UBERTOOTH_RELEASE_BUILD 0)
	message(STATUS "Development build: firmware of another API version is a warning")
endif()
add_definitions( -DUBERTOOTH_RELEASE_BUILD=${UBERTOOTH_RELEASE_BUILD} )

# Ask glibc for the interfaces these sources use. They need POSIX.1-2008
# (clock_gettime, fdopen, fileno, open_memstream, strcasecmp) and two things
# outside it: usleep(), dropped from POSIX in 2008, and the byte-order helpers
# in <endian.h> (le32toh, htobe32, be32toh), which are a glibc extension.
# _DEFAULT_SOURCE is the one macro that covers both: glibc's <features.h>
# turns it into _POSIX_C_SOURCE 200809L plus the extensions, so there is no
# need to set _POSIX_C_SOURCE as well. musl reads it the same way.
#
# Without it, a strict standard (-std=c17, -std=c2x) hides all of the above
# and the build fails; it only worked because gcc and clang default to a GNU
# standard, which defines _DEFAULT_SOURCE for us. macOS and the BSDs ignore
# the macro and declare these without being asked.
#
# This is set here, for every target below, rather than in the sources: the
# library, the tools and the tests all use these interfaces.
add_definitions( -D_DEFAULT_SOURCE )

# Comment the following out for releases.
set(CMAKE_C_FLAGS "$ENV{CFLAGS}" CACHE STRING "C Flags")

# Where an install writes: GNUInstallDirs works out the directories this
# system uses and puts them in CMAKE_INSTALL_BINDIR, CMAKE_INSTALL_LIBDIR,
# CMAKE_INSTALL_INCLUDEDIR and CMAKE_INSTALL_MANDIR, which the install
# commands in the directories below use. The library's directory is the one
# that differs between systems: lib64 on most 64-bit Linux systems,
# lib/<triplet> where the prefix is /usr on a multiarch Debian or Ubuntu, and
# lib on macOS, on Arch and Alpine, and for a conda prefix. libbtbb chooses
# its own the same way, so both libraries land in the same directory instead
# of two, which is what the loader, pkg-config and a packager's file list all
# expect. CMAKE_INSTALL_LIBDIR and CMAKE_INSTALL_INCLUDEDIR choose them.
include(GNUInstallDirs)

# LIB_SUFFIX, the older way of asking for the library's directory, still
# decides where it is set to something, so a build command that passes it
# installs where it always did, as it does in libbtbb. It names the library's
# directory only, and the headers follow CMAKE_INSTALL_INCLUDEDIR either way.
# It is tested for being set to something rather than for being true, so that
# a suffix CMake reads as a false constant -- "0", "N", "NO" -- is still a
# suffix, and an empty one leaves CMAKE_INSTALL_LIBDIR in charge rather than
# quietly turning it into lib.
set(UBERTOOTH_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
if(DEFINED LIB_SUFFIX AND NOT "${LIB_SUFFIX}" STREQUAL "")
	set(UBERTOOTH_INSTALL_LIBDIR "lib${LIB_SUFFIX}")
	# Which of the two was followed, where they disagree: a build command can
	# carry both, one of them from the system rather than from its author.
	if(NOT "${UBERTOOTH_INSTALL_LIBDIR}" STREQUAL "${CMAKE_INSTALL_LIBDIR}")
		message(STATUS "LIB_SUFFIX asks for ${UBERTOOTH_INSTALL_LIBDIR}, "
			"which is used rather than the ${CMAKE_INSTALL_LIBDIR} of "
			"CMAKE_INSTALL_LIBDIR")
	endif()
endif()

# Written to the cache as well, because this, rather than CMAKE_INSTALL_LIBDIR,
# is the directory the library goes to, and install.sh reads it back out to
# give the installed tools a run path that holds it. INTERNAL implies FORCE,
# so a build command that changes either setting is followed.
set(UBERTOOTH_INSTALL_LIBDIR "${UBERTOOTH_INSTALL_LIBDIR}" CACHE INTERNAL
	"The directory libubertooth is installed to, under the prefix")

# The library's directory as it will be on disk: one given as an absolute path
# is installed to exactly that, wherever the prefix is.
set(UBERTOOTH_FULL_LIBDIR "${UBERTOOTH_INSTALL_LIBDIR}")
if(NOT IS_ABSOLUTE "${UBERTOOTH_FULL_LIBDIR}")
	set(UBERTOOTH_FULL_LIBDIR
		"${CMAKE_INSTALL_PREFIX}/${UBERTOOTH_INSTALL_LIBDIR}")
endif()
# What this build will install where, said once and in full: a packager who
# set one of these reads it back, and a plain build on a lib64 system sees why
# the library is not going where the last one did.
message(STATUS "Installing the library to ${UBERTOOTH_FULL_LIBDIR} and the "
	"headers to ${CMAKE_INSTALL_FULL_INCLUDEDIR}")

# Defaults: BUILD_SHARED=ON, BUILD_STATIC=OFF
set(BUILD_SHARED_LIB ON CACHE BOOL "Build shared library")
set(BUILD_STATIC_LIB OFF CACHE BOOL "Build static library")
set(BUILD_STATIC_BINS OFF CACHE BOOL "Build static library")
set(ENABLE_PYTHON ON CACHE BOOL "Build python tools")

# Check that we're building at least one library
if( NOT ${BUILD_SHARED_LIB} AND NOT ${BUILD_STATIC_LIB} )
	message( FATAL_ERROR "Not building static or shared library - at least one must be built")
endif( NOT ${BUILD_SHARED_LIB} AND NOT ${BUILD_STATIC_LIB} )

# Build static bins if we're only building the static lib
if( ${BUILD_STATIC_LIB} AND NOT ${BUILD_SHARED_LIB} )
	set(BUILD_STATIC_BINS ON)
	message(STATUS "Building static executables because only building static library")
endif( ${BUILD_STATIC_LIB} AND NOT ${BUILD_SHARED_LIB} )

# Complain if we're building the shared lib but static bins
if( ${BUILD_STATIC_BINS} AND NOT ${BUILD_STATIC_LIB} )
	message(FATAL_ERROR "Building static executables not possible with shared library")
endif( ${BUILD_STATIC_BINS} AND NOT ${BUILD_STATIC_LIB} )

add_subdirectory(libubertooth)
add_subdirectory(ubertooth-tools)
add_subdirectory(doc)
add_subdirectory(misc)
if(${ENABLE_PYTHON})
	add_subdirectory(python)
endif()

# Create uninstall target
configure_file(
	${PROJECT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
@ONLY)

add_custom_target(uninstall
	${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
)
