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

# Which stream a refused range lands on, and the exit status, against the
# source script: the checks run before the Qt application is built, so no
# window opens and no board is looked for. A few short runs; the timeout only
# catches a case that opened a window after all.
add_test(NAME specan-ui-streams
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_specan_ui_streams.py
		${CMAKE_CURRENT_SOURCE_DIR}/ubertooth-specan-ui.in)
set_tests_properties(specan-ui-streams PROPERTIES TIMEOUT 120)

