# Copyright 2013 Dominic Spill
#
# This file is part of Ubertooth
#
# GNU Radio is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# GNU Radio is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with GNU Radio; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.

project(ubertooth-python)

# Check for a Python interpreter.
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
	return()
endif()

# Pure-Python modules are installed by CMake, not setup.py/pip. Ask the selected
# interpreter for the standard posix_prefix purelib directory under the
# configured prefix, then keep it RELATIVE so `cmake --install --prefix` and
# DESTDIR continue to work normally.
execute_process(
	COMMAND "${Python3_EXECUTABLE}" -c
		"import os, sys, sysconfig; p=sys.argv[1]; d=sysconfig.get_path('purelib', 'posix_prefix', vars={'base': p, 'platbase': p}); print(os.path.relpath(d, p))"
		"${CMAKE_INSTALL_PREFIX}"
	OUTPUT_VARIABLE PYTHON_PURELIB_REL
	OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE PYTHON_PURELIB_RESULT
)
if(NOT PYTHON_PURELIB_RESULT EQUAL 0 OR
   PYTHON_PURELIB_REL STREQUAL "" OR
   IS_ABSOLUTE "${PYTHON_PURELIB_REL}" OR
   PYTHON_PURELIB_REL MATCHES "^\\.\\.")
	message(FATAL_ERROR "Could not determine a safe Python purelib path under CMAKE_INSTALL_PREFIX")
endif()

# Installed wrappers locate their private modules relative to their own bin
# directory. This makes custom prefixes and DESTDIR staging work without
# PYTHONPATH, including when --prefix overrides the configured prefix.
file(RELATIVE_PATH PYTHON_PURELIB_FROM_BINDIR
	"${CMAKE_INSTALL_PREFIX}/${INSTALL_DEFAULT_BINDIR}"
	"${CMAKE_INSTALL_PREFIX}/${PYTHON_PURELIB_REL}")
message(STATUS "Python modules install to: ${PYTHON_PURELIB_REL}")

add_subdirectory(specan_ui)
# add_subdirectory(usb_dfu)
add_subdirectory(ubtbr)

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
