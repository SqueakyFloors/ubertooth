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

set(SPECAN_WRAPPER ${CMAKE_CURRENT_BINARY_DIR}/ubertooth-specan-ui)

# Configure only the installed wrapper. The source script remains directly
# runnable; its placeholder path is ignored when that directory does not exist.
configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/ubertooth-specan-ui.in
	${SPECAN_WRAPPER}
	@ONLY)

# Pure Python: install source files directly. CMake applies CMAKE_INSTALL_PREFIX,
# --prefix overrides, and DESTDIR without invoking pip or touching system Python.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/specan
	DESTINATION ${PYTHON_PURELIB_REL}
	FILES_MATCHING PATTERN "*.py"
	PATTERN "__pycache__" EXCLUDE)
install(PROGRAMS ${SPECAN_WRAPPER} DESTINATION ${INSTALL_DEFAULT_BINDIR})

# Which stream a refused range lands on, the exit status, and the frequency
# each Wi-Fi channel converts to. All three are in specan/limits.py, which
# imports nothing outside the standard library, so they run everywhere; the
# same cases are then run through the script itself, along with -h and -V.
# The script reads its whole command line before the window's own imports, so
# those runs need neither numpy nor a Qt binding, and two of them shadow
# those modules with ones that refuse to import to show it. No window opens
# and no board is looked for.
add_test(NAME specan-ui-streams
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_specan_ui_streams.py
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/ubertooth-specan-ui.in)
set_tests_properties(specan-ui-streams PROPERTIES TIMEOUT 120)

# The command-line contract (host/ubertooth-tools/tests/cli_spec.json) says
# this tool's command line cannot be reached without its Qt binding and numpy,
# so the matrix runs nothing here. Its man page is still checked, which is
# what holds the documentation findings for it. Named by relative path, and
# registered only when the spec and the test are there, so host/python can be
# configured on its own.
set(UBERTOOTH_CLI_MATRIX
	"${CMAKE_CURRENT_SOURCE_DIR}/../../ubertooth-tools/tests/test_cli_matrix.py")
set(UBERTOOTH_CLI_SPEC
	"${CMAKE_CURRENT_SOURCE_DIR}/../../ubertooth-tools/tests/cli_spec.json")
if(EXISTS "${UBERTOOTH_CLI_MATRIX}" AND EXISTS "${UBERTOOTH_CLI_SPEC}")
	add_test(NAME cli-matrix-ubertooth-specan-ui
		COMMAND ${Python3_EXECUTABLE}
			${UBERTOOTH_CLI_MATRIX}
			${UBERTOOTH_CLI_SPEC}
			${SPECAN_WRAPPER})
	set_tests_properties(cli-matrix-ubertooth-specan-ui PROPERTIES TIMEOUT 30)
endif()
