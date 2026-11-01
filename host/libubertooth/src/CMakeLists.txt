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
set(c_sources ${CMAKE_CURRENT_SOURCE_DIR}/ubertooth.c
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
foreach(dir IN LISTS LIBUSB_INCLUDE_DIRS)
	if(EXISTS "${dir}/libusb-1.0/libusb.h" AND
	   NOT "${dir}" STREQUAL "/usr/include")
		string(APPEND LIBUBERTOOTH_PC_CFLAGS " -I${dir}")
	endif()
endforeach()

# Libs.private, which is what a program linking the static library is given on
# top of Libs: the same two libraries the targets above link beyond libusb and
# libbtbb. CMAKE_THREAD_LIBS_INIT is already a linker flag (empty where libc
# carries the pthread calls), CMAKE_DL_LIBS is a library name (empty on macOS,
# where libc carries dladdr()).
set(LIBUBERTOOTH_PC_LIBS_PRIVATE "")
if(CMAKE_THREAD_LIBS_INIT)
	string(APPEND LIBUBERTOOTH_PC_LIBS_PRIVATE " ${CMAKE_THREAD_LIBS_INIT}")
endif()
foreach(lib IN LISTS CMAKE_DL_LIBS)
	string(APPEND LIBUBERTOOTH_PC_LIBS_PRIVATE " -l${lib}")
endforeach()

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
		message(STATUS "libubertooth.pc will name "
			"${CMAKE_INSTALL_FULL_INCLUDEDIR} as the header directory, "
			"because an absolute library directory leaves the file with no "
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
