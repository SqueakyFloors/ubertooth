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
# Based slightly upon the hackrf cmake setup.

# RELEASE_STRING comes from set_release.cmake, and UBERTOOTH_INSTALL_LIBDIR
# from the install directories worked out beside it; the top-level
# CMakeLists.txt does both. Only this library reports the release string.
add_definitions( -DRELEASE="${RELEASE_STRING}" )

# Targets
set(c_sources ${CMAKE_CURRENT_SOURCE_DIR}/libbtbb_origin.c
              ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth.c
              ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_api_version.c
              ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_callback.c
              ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_control.c
              ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_fifo.c
              ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_le_frag.c
              ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_version.c
			  CACHE INTERNAL "List of C sources")
set(c_headers ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth.h
              ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_callback.h
              ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_control.h
              ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_fifo.h
			  ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_interface.h
			  ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth_le_frag.h
			  CACHE INTERNAL "List of C headers")

# Include and link to libbtbb and libusb-1.0
find_package(BTBB REQUIRED)
find_package(USB1 REQUIRED)

# The directory the libbtbb this build links lives in. -V compares it with the
# one the loader actually opened and warns when they differ, which is how a
# machine with a second libbtbb install shows up: the two decode packets
# differently, and nothing else in a run would say so. An entry that is not an
# absolute path is a bare link name the user set, and names no directory, so
# nothing is recorded and nothing is compared.
list(GET LIBBTBB_LIBRARIES 0 _btbb_library)
set(_btbb_libdir "")
if(IS_ABSOLUTE "${_btbb_library}")
	get_filename_component(_btbb_libdir "${_btbb_library}" DIRECTORY)
endif()
add_definitions( -DBTBB_BUILD_LIBDIR="${_btbb_libdir}" )
unset(_btbb_library)
unset(_btbb_libdir)

# ubertooth.c runs the capture ring buffer on its own thread. glibc 2.34 and
# later fold the pthread calls into libc, so a missing -lpthread goes unnoticed
# there; on this fork's declared minimum (Ubuntu 20.04, glibc 2.31) the link
# fails without it.
find_package(Threads REQUIRED)

include_directories(${LIBUSB_INCLUDE_DIRS} ${LIBBTBB_INCLUDE_DIR})
# CMAKE_DL_LIBS is the loader library, which print_version() needs for
# dladdr(): empty on macOS, where it is part of libc, and -ldl on a glibc
# older than 2.34, which includes this fork's declared minimum (Ubuntu 20.04).
LIST(APPEND LIBUBERTOOTH_LIBS ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
     Threads::Threads ${CMAKE_DL_LIBS})

if( ${BUILD_SHARED_LIB} )
	# Shared library
	message(STATUS "Building shared library")
	add_library(ubertooth SHARED ${c_sources})
	target_link_libraries(ubertooth ${LIBUBERTOOTH_LIBS})
	set_target_properties(ubertooth PROPERTIES VERSION ${MAJOR_VERSION}.${MINOR_VERSION} SOVERSION 1)
	set_target_properties(ubertooth PROPERTIES CLEAN_DIRECT_OUTPUT 1)
	install(TARGETS ubertooth
			LIBRARY DESTINATION ${UBERTOOTH_INSTALL_LIBDIR}
			COMPONENT sharedlibs)
endif( ${BUILD_SHARED_LIB} )

if( ${BUILD_STATIC_LIB} )
	# Static library
	message(STATUS "Building static library")
	add_library(ubertooth-static STATIC ${c_sources})
	target_link_libraries(ubertooth-static ${LIBUBERTOOTH_LIBS})
	set_target_properties(ubertooth-static PROPERTIES OUTPUT_NAME "ubertooth")
	set_target_properties(ubertooth-static PROPERTIES CLEAN_DIRECT_OUTPUT 1)
	install(TARGETS ubertooth-static
			DESTINATION ${UBERTOOTH_INSTALL_LIBDIR}
			COMPONENT staticlibs)
endif( ${BUILD_STATIC_LIB} )

install(FILES ${c_headers}
		DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
		COMPONENT headers)

# libubertooth.pc, so that a program built against the installed library asks
# pkg-config for its compiler and linker flags instead of being given them by
# hand. The file itself says what each line is for; two of them are filled in
# here.
#
# The include directory for <libusb-1.0/libusb.h>: LIBUSB_INCLUDE_DIRS holds
# the directory libusb.h sits in and the one above it, and it is the one above
# that this include resolves against. It is picked out by looking for the
# header at the spelling the sources use, so a libusb laid out some other way
# cannot put the wrong directory in the file. /usr/include, which is where the
# distributions keep libusb, is left out: the compiler searches it anyway, and
# naming it would only make every consumer's command line longer. A Homebrew
# install keeps the header under the Cellar, where the version is part of the
# path, so an install that is still here after a `brew upgrade libusb` names a
# directory that has gone; configuring the build again writes the new one.
set(LIBUBERTOOTH_PC_CFLAGS "")
# The same directory is kept whole, with no exception for /usr/include, as the
# hint ubertoothConfig.cmake gives its own search for that header.
set(UBERTOOTH_CONFIG_LIBUSB_INCLUDEDIR "")
foreach(dir IN LISTS LIBUSB_INCLUDE_DIRS)
	if(EXISTS "${dir}/libusb-1.0/libusb.h")
		if(NOT UBERTOOTH_CONFIG_LIBUSB_INCLUDEDIR)
			set(UBERTOOTH_CONFIG_LIBUSB_INCLUDEDIR "${dir}")
		endif()
		if(NOT "${dir}" STREQUAL "/usr/include")
			string(APPEND LIBUBERTOOTH_PC_CFLAGS " -I${dir}")
		endif()
	endif()
endforeach()

# Libs.private, which is what a program linking the static library is given on
# top of Libs: the same two libraries the targets above link beyond libusb and
# libbtbb. CMAKE_THREAD_LIBS_INIT is already a linker flag (empty where libc
# carries the pthread calls), CMAKE_DL_LIBS is a library name (empty on macOS,
# where libc carries dladdr()).
set(LIBUBERTOOTH_PC_LIBS_PRIVATE "")
# The same two, as a CMake list, for ubertoothConfig.cmake below.
set(UBERTOOTH_CONFIG_PRIVATE_LIBS "")
if(CMAKE_THREAD_LIBS_INIT)
	string(APPEND LIBUBERTOOTH_PC_LIBS_PRIVATE " ${CMAKE_THREAD_LIBS_INIT}")
	list(APPEND UBERTOOTH_CONFIG_PRIVATE_LIBS "${CMAKE_THREAD_LIBS_INIT}")
endif()
foreach(lib IN LISTS CMAKE_DL_LIBS)
	string(APPEND LIBUBERTOOTH_PC_LIBS_PRIVATE " -l${lib}")
	list(APPEND UBERTOOTH_CONFIG_PRIVATE_LIBS "-l${lib}")
endforeach()

# Where this build found libbtbb, as hints for ubertoothConfig.cmake's own
# search. The library is a path from find_library(), so the directory it is in
# is the one to look in; one set by hand to a bare link name leaves the hint
# empty, and the search then looks where it would have anyway.
#
# A hint inside a libbtbb source tree beside this one, inside this tree, or
# inside this build directory is dropped. FindBTBB.cmake looks in such a
# source tree, and a libbtbb named through $ENV{LIBBTBB_DIR} or pkg-config can
# be anywhere, so a build that found libbtbb in one of those three would
# otherwise write that path into the config file and send every consumer of
# the install to a directory only this machine has -- ahead of the installed
# libbtbb, because HINTS are searched before the default directories, which is
# where libubertooth.pc's Requires sends a consumer. Those three, rather than
# every libbtbb that is not installed, which nothing here can tell: a libbtbb
# staged under the build directory loses a hint it could have kept, and the
# search then looks where it would have anyway.
set(UBERTOOTH_CONFIG_BTBB_HINT_DROPPED 0)
get_filename_component(libbtbb_source_tree
	"${CMAKE_SOURCE_DIR}/../libbtbb" REALPATH)
set(UBERTOOTH_CONFIG_BTBB_INCLUDEDIR "${LIBBTBB_INCLUDE_DIR}")
set(UBERTOOTH_CONFIG_BTBB_LIBDIR "")
foreach(lib IN LISTS LIBBTBB_LIBRARIES)
	if(IS_ABSOLUTE "${lib}" AND NOT UBERTOOTH_CONFIG_BTBB_LIBDIR)
		get_filename_component(UBERTOOTH_CONFIG_BTBB_LIBDIR "${lib}" DIRECTORY)
	endif()
endforeach()
foreach(hint UBERTOOTH_CONFIG_BTBB_INCLUDEDIR UBERTOOTH_CONFIG_BTBB_LIBDIR)
	if(NOT ${hint})
		continue()
	endif()
	get_filename_component(hint_path "${${hint}}" REALPATH)
	foreach(tree "${libbtbb_source_tree}" "${CMAKE_SOURCE_DIR}"
			"${CMAKE_BINARY_DIR}")
		get_filename_component(tree_path "${tree}" REALPATH)
		# Whether the hint is that directory or sits under it, decided by the
		# way from one to the other rather than by matching the path as text:
		# a directory name can hold a regular expression's own characters.
		# Empty for the same directory, starting with ".." for one outside it,
		# and absolute where there is no way between them at all.
		file(RELATIVE_PATH way_in "${tree_path}" "${hint_path}")
		# "..", a whole directory name, rather than a name that begins with
		# it: a sibling called "..libbtbb" is outside the tree and its hint
		# would be kept.
		if(NOT way_in MATCHES "^\\.\\.(/|$)" AND NOT IS_ABSOLUTE "${way_in}")
			message(STATUS "ubertoothConfig.cmake will not hint at "
				"${hint_path} as a libbtbb directory, because it is inside "
				"${tree_path}: a consumer of this install would be sent to "
				"a directory only this machine has")
			set(${hint} "")
			set(UBERTOOTH_CONFIG_BTBB_HINT_DROPPED 1)
			break()
		endif()
	endforeach()
endforeach()
unset(libbtbb_source_tree)
unset(hint_path)
unset(tree_path)
unset(way_in)

# Read back by the test, which cannot use an install whose libbtbb was only
# ever in one of those directories.
set(UBERTOOTH_CONFIG_BTBB_HINT_DROPPED
	"${UBERTOOTH_CONFIG_BTBB_HINT_DROPPED}" CACHE INTERNAL
	"Whether the libbtbb this build found is in a directory an install cannot be described by")
set(UBERTOOTH_CONFIG_LIBUSB_LIBDIR "")
foreach(lib IN LISTS LIBUSB_LIBRARIES)
	if(IS_ABSOLUTE "${lib}" AND NOT UBERTOOTH_CONFIG_LIBUSB_LIBDIR)
		get_filename_component(UBERTOOTH_CONFIG_LIBUSB_LIBDIR "${lib}" DIRECTORY)
	endif()
endforeach()

# The name libusb is linked under, which FindUSB1.cmake chooses by system.
# Written into the config file rather than chosen there, so that the search
# asks for the same library this build linked. FindUSB1.cmake sets the name
# only on the configure that searches, not on one that reads the answer back
# from the cache, so it is worked out here from the same rule.
set(UBERTOOTH_CONFIG_LIBUSB_NAME usb-1.0)
if(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
	set(UBERTOOTH_CONFIG_LIBUSB_NAME usb)
endif()

# The directories libubertooth.pc names have to be the ones the install writes
# to: a consumer whose pkg-config -L names a directory holding no library
# links by luck or not at all.
#
# A directory under the prefix is named relative to this file's own location,
# so that the file works wherever the install ends up. The way back out of the
# pkgconfig directory is one ".." for that directory and one for each
# directory of the library's own, which can be a path -- lib/x86_64-linux-gnu,
# the Debian multiarch layout -- rather than a single name, so it is worked
# out here rather than written into libubertooth.pc.in.
#
# A library directory given as an absolute path is installed to exactly that,
# wherever the prefix is, so the file names the directories as they stand: it
# lands outside the prefix and has no way to tell that the prefix has moved.
if(IS_ABSOLUTE "${UBERTOOTH_INSTALL_LIBDIR}")
	set(PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
	set(PC_LIBDIR "${UBERTOOTH_FULL_LIBDIR}")
	set(PC_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
	# Which is worth saying while the directories are being chosen: such an
	# install cannot be staged somewhere else and still be found, because the
	# headers would move with the prefix and this file would still name the
	# ones at the old place. Said only where the headers are the ones that
	# would move: absolute ones are named as they stand and do not move
	# either.
	if(NOT IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
		message(STATUS "libubertooth.pc and ubertoothConfig.cmake will name "
			"${CMAKE_INSTALL_FULL_INCLUDEDIR} as the header directory, "
			"because an absolute library directory leaves them with no "
			"relation to the prefix: an install staged elsewhere with "
			"--prefix needs its own configure")
	endif()
else()
	# RELATIVE_PATH leaves a trailing slash on the way out of a directory,
	# which pkg-config reads the same but nobody wants to read.
	file(RELATIVE_PATH pc_prefix_up
		"${UBERTOOTH_FULL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
	string(REGEX REPLACE "/+$" "" pc_prefix_up "${pc_prefix_up}")
	set(PC_PREFIX "\${pcfiledir}/${pc_prefix_up}")
	set(PC_LIBDIR "\${prefix}/${UBERTOOTH_INSTALL_LIBDIR}")
	if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
		set(PC_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
	else()
		set(PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
	endif()
endif()

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/libubertooth.pc.in
	${CMAKE_CURRENT_BINARY_DIR}/libubertooth.pc
	@ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libubertooth.pc
		DESTINATION ${UBERTOOTH_INSTALL_LIBDIR}/pkgconfig
		COMPONENT pkgconfig)

# ubertoothConfig.cmake, so that a CMake project outside this tree reaches the
# installed library the way CMake projects reach a library: find_package() and
# an imported target. libubertooth.pc, written above, answers the same
# questions for pkg-config, and FindUBERTOOTH.cmake in this tree reads that
# file -- but a CMake consumer tries find_package() first, and with no config
# file to find it had to be given a Find module of its own or the paths by
# hand. The file itself says what each part of it is for.
include(CMakePackageConfigHelpers)

# Not install(EXPORT), which would write the target definition for us. An
# export set records the library's link interface as this build resolved it,
# so it would name the libbtbb and the libusb of the machine the install was
# built on. libubertooth.pc names them in its Requires and leaves them to be
# resolved where the consumer is, and this file is meant to say the same
# thing, so the target is written out here and the dependencies are searched
# for in it.
#
# Beside the library, in the directory CMake searches for a package of this
# name: <prefix>/<libdir>/cmake/<name>. configure_package_config_file works
# out the way back from here to the prefix, so the file describes the install
# it is in rather than the prefix this build was configured with.
set(UBERTOOTH_INSTALL_CMAKEDIR "${UBERTOOTH_INSTALL_LIBDIR}/cmake/ubertooth")

# The same two directories libubertooth.pc names, worked out the same way and
# for the same reason: a consumer sent to a directory holding no library links
# by luck or not at all. One under the prefix is named relative to the file's
# own location, PACKAGE_PREFIX_DIR, which the block the file gets from
# configure_package_config_file() works out; an absolute one is named as it
# stands, because the install writes to exactly that wherever the prefix is.
#
# Not PATH_VARS, which looks the prefix up in the value as text: an absolute
# directory that happens to sit under the prefix this build was configured
# with would come out relative to PACKAGE_PREFIX_DIR, and an install made
# with another prefix would then be described as having its headers under
# that other prefix while the install had written them to the absolute path.
if(IS_ABSOLUTE "${UBERTOOTH_INSTALL_LIBDIR}")
	set(CONFIG_LIBDIR "${UBERTOOTH_FULL_LIBDIR}")
else()
	set(CONFIG_LIBDIR "\${PACKAGE_PREFIX_DIR}/${UBERTOOTH_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
	set(CONFIG_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
else()
	set(CONFIG_INCLUDEDIR "\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()

configure_package_config_file(
	${CMAKE_CURRENT_SOURCE_DIR}/ubertoothConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/ubertoothConfig.cmake
	INSTALL_DESTINATION "${UBERTOOTH_INSTALL_CMAKEDIR}")

# SameMajorVersion, which is what the library's SOVERSION promises: a consumer
# asking for 1.0 is served by this 1.1, and one asking for 2.0 is not.
write_basic_package_version_file(
	${CMAKE_CURRENT_BINARY_DIR}/ubertoothConfigVersion.cmake
	VERSION ${VERSION}
	COMPATIBILITY SameMajorVersion)

install(FILES
		${CMAKE_CURRENT_BINARY_DIR}/ubertoothConfig.cmake
		${CMAKE_CURRENT_BINARY_DIR}/ubertoothConfigVersion.cmake
		DESTINATION ${UBERTOOTH_INSTALL_CMAKEDIR}
		COMPONENT cmakeconfig)
