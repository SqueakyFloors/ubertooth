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

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/UbertoothPythonFloor.cmake)

# The floor decision is unit-tested without an interpreter.
add_test(NAME python-floor
	COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_python_floor.cmake)

# Check for a Python interpreter. No version is passed to find_package: that
# would make CMake skip an older interpreter and pick a different one than
# before. Check the one it found instead, and name it so the choice is visible.
find_package(Python3 COMPONENTS Interpreter)
ubertooth_python_floor("${Python3_FOUND}" "${Python3_VERSION}" "${REQUIRE_ALL}"
	PYTHON_FLOOR_RESULT PYTHON_FLOOR_MESSAGE)
if(PYTHON_FLOOR_RESULT STREQUAL "FATAL")
	message(FATAL_ERROR "${PYTHON_FLOOR_MESSAGE}")
elseif(PYTHON_FLOOR_RESULT STREQUAL "SKIPPED")
	message(WARNING "${PYTHON_FLOOR_MESSAGE}")
	return()
endif()
message(STATUS "${PYTHON_FLOOR_MESSAGE}: ${Python3_EXECUTABLE}")

# btle-extcap.py's command line and its interface listing, against a fake
# ubertooth-util on PATH: no board is involved. Several dozen runs of a few
# milliseconds each, plus one case that stops a running capture from outside
# and so waits on a child, take a few seconds in all.
#
# The built ubertooth-util is passed as well, for the one case that needs the
# real thing: the script matches ubertooth-util's "no Ubertooth devices found"
# to tell an empty listing from a failed one, and that case checks the tool
# still prints it. It is run with UBERTOOTH_TEST_NO_DEVICES set, so no board
# is opened.
add_test(NAME btle-extcap
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/extcap/tests/test_btle_extcap.py
		${CMAKE_CURRENT_SOURCE_DIR}/extcap/btle-extcap.py
		$<TARGET_FILE:ubertooth-util>
		# The script parses ubertooth-util's device listing, so the listing
		# lines it is given come from that tool's man page rather than from
		# a fake of its own: a change to the listing then fails here instead
		# of costing Wireshark its capture interface unnoticed.
		${CMAKE_CURRENT_SOURCE_DIR}/../doc/ubertooth-util.md)
# Under ten seconds on an idle machine, dominated by interpreter start-up for
# each of some eighty runs, and seen at three times that on a loaded one,
# which is what the old thirty-second limit tripped over.
set_tests_properties(btle-extcap PROPERTIES TIMEOUT 60)

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

# ubertooth-btbr and the ubtbr modules ending when they are asked to, against
# a fake usb1 module: no board is involved. Around twenty seconds, nearly all
# of it waiting: several cases give a thread a bounded time to stop, and each
# run of the tool spends a second settling the device it thinks it opened.
add_test(NAME ubtbr
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/ubtbr/tests/test_ubtbr.py
		${UBTBR_WRAPPER})
set_tests_properties(ubtbr PROPERTIES TIMEOUT 90)

# ubertooth-btbr's command line, against the same contract as every other
# installed tool (host/ubertooth-tools/tests/cli_spec.json). It is registered
# here because the wrapper CMake configured is what gets run, and the spec and
# the test live with the other tools, so both are named by relative path and
# the entry appears only when they are there: host/python can be configured on
# its own. The test gives the tool a usb1 that finds nothing, so no board is
# opened. Two dozen runs of a few milliseconds each.
set(UBERTOOTH_CLI_MATRIX
	"${CMAKE_CURRENT_SOURCE_DIR}/../ubertooth-tools/tests/test_cli_matrix.py")
set(UBERTOOTH_CLI_SPEC
	"${CMAKE_CURRENT_SOURCE_DIR}/../ubertooth-tools/tests/cli_spec.json")
if(EXISTS "${UBERTOOTH_CLI_MATRIX}" AND EXISTS "${UBERTOOTH_CLI_SPEC}")
	add_test(NAME cli-matrix-ubertooth-btbr
		COMMAND ${Python3_EXECUTABLE}
			${UBERTOOTH_CLI_MATRIX}
			${UBERTOOTH_CLI_SPEC}
			${UBTBR_WRAPPER}
			# The tool answers -V by running ubertooth-util, so the one from
			# this build goes first on the PATH of every run.
			$<TARGET_FILE:ubertooth-util>)
	set_tests_properties(cli-matrix-ubertooth-btbr PROPERTIES TIMEOUT 60)
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
