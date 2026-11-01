# Try to find the libubertooth library
#
# Once done this defines:
#  LIBUBERTOOTH_FOUND - system has libubertooth
#  LIBUBERTOOTH_INCLUDE_DIR - the libubertooth include directory
#  LIBUBERTOOTH_LIBRARIES - Link these to use libubertooth
#
# Find libbtbb and libusb first: ubertooth.h includes both of their headers,
# and the check below compiles against it, so where LIBBTBB_INCLUDE_DIR and
# LIBUSB_INCLUDE_DIRS are already set they are used. Without them, a libbtbb
# or a libusb outside the compiler's own include path leaves that check
# reporting a libubertooth it cannot build against.
#
# Copyright (c) 2013  Dominic Spill


# A few of the libubertooth functions these tools call, chosen to mark the API
# they are written against. libubertooth reports no version a build can read,
# so "new enough" is decided by asking the installed one for them: an install
# from an older commit of this tree has the header and the library in place,
# configures without a word, and then fails in the compiler with about forty
# errors naming symbols its header has not got. The check below says what is
# wrong in one line instead.
#
# Not every symbol the tools use: four, spread over the areas that have moved,
# and each the first of its kind, so one of them is missing from an install
# made at any point these tools would not build against. The comment on each
# says what it is for, which is what a reader has to know to decide whether a
# function belongs here.
set(LIBUBERTOOTH_REQUIRED_FUNCTIONS
  ubertooth_start_select           # opening a board by index or serial
  ubertooth_report_capture_losses  # the gaps and errors a capture reports
  cmd_set_modulation_quiet         # setting the modulation without a warning
  cmd_get_bootloader_info          # reading the board's bootloader info
)

# Fail the configure step when the libubertooth that was found is missing any
# of them, saying which ones and how to install one that has them.
#
# A function, so the CMAKE_REQUIRED_* variables the probe needs stay out of the
# caller's scope.
function(ubertooth_require_functions)
  include(CheckCSourceCompiles)
  # ubertooth.h includes btbb.h and libusb.h, and the library needs both to
  # link, so where the caller has already found them their paths come too.
  # Quoted: a path with a space in it, or a variable holding more than one
  # library, would otherwise be split into separate arguments.
  set(CMAKE_REQUIRED_INCLUDES "${LIBUBERTOOTH_INCLUDE_DIR}"
      "${LIBBTBB_INCLUDE_DIR}" "${LIBUSB_INCLUDE_DIRS}")
  set(CMAKE_REQUIRED_LIBRARIES "${LIBUBERTOOTH_LIBRARIES}"
      "${LIBBTBB_LIBRARIES}" "${LIBUSB_LIBRARIES}" "${CMAKE_DL_LIBS}")

  # Taking the address of each function needs a declaration from the installed
  # headers, and keeping that address needs a definition in the library, so
  # one compile and link answers for both. The addresses go to a call through
  # a volatile function pointer, which is what keeps them from being optimized
  # away whatever the user's CFLAGS ask for, and they are passed as variadic
  # arguments so that not one of them is cast; FindBTBB.cmake sets out why
  # each of those matters.
  set(body "#include <ubertooth.h>\n")
  string(APPEND body
    "static int ubertooth_probe_sink(int n, ...) { (void)n; return 0; }\n"
    "static int (*volatile ubertooth_probe_call)(int, ...) ="
    " ubertooth_probe_sink;\n"
    "int main(int argc, char **argv) {\n"
    "\t(void)argv;\n"
    "\treturn ubertooth_probe_call(argc")
  foreach(function ${LIBUBERTOOTH_REQUIRED_FUNCTIONS})
    string(APPEND body ",\n\t\t&${function}")
  endforeach()
  string(APPEND body ");\n}\n")

  # Not cached: the installed libubertooth can change under a build directory
  # that configured successfully once, which is exactly the developer who
  # built the tools months ago and has come back to them.
  unset(LIBUBERTOOTH_HAS_REQUIRED_FUNCTIONS CACHE)
  check_c_source_compiles("${body}" LIBUBERTOOTH_HAS_REQUIRED_FUNCTIONS)
  if(LIBUBERTOOTH_HAS_REQUIRED_FUNCTIONS)
    return()
  endif()

  # The fix, whichever way the probe failed. One quoted string, and referenced
  # as one below: a semicolon in an unquoted CMake variable is a list
  # separator and would be eaten. The commands are the build guide's, which
  # work on the CMake 3.5 this project still supports: cmake -S/-B needs 3.13
  # and cmake --install needs 3.15.
  set(fix "Build and install libubertooth from this tree:\n")
  string(APPEND fix
    "    cd host\n"
    "    mkdir build && cd build\n"
    "    cmake .. && make\n"
    "    sudo make install\n"
    "    sudo ldconfig     # Linux only\n"
    "then configure this build again. Configuring host/ instead of "
    "host/ubertooth-tools/ builds the tools against the libubertooth beside "
    "them and needs no install at all.")

  # The headers have to be read to say which functions are missing. One was
  # there when the library was found, so this is the libubertooth that has
  # been uninstalled or moved under a build directory that configured once --
  # the same fix, with nothing to name.
  if(NOT EXISTS "${LIBUBERTOOTH_INCLUDE_DIR}/ubertooth.h")
    message(FATAL_ERROR
      "ubertooth.h is gone from ${LIBUBERTOOTH_INCLUDE_DIR}, where this "
      "build found libubertooth.\n" "${fix}")
  endif()

  # Which functions are missing, read out of the installed headers rather than
  # probed one at a time: the compile above has already proved that something
  # is missing, so this only has to name it, and a probe per function would
  # cost half a minute of nested builds to say what the headers plainly do not
  # contain. All of libubertooth's headers, because ubertooth.h includes the
  # others and the control requests are declared there. A name this search
  # fails to find in a header that does declare it would only make the message
  # below less specific, never refuse a libubertooth the compile accepted.
  file(GLOB headers "${LIBUBERTOOTH_INCLUDE_DIR}/ubertooth*.h")
  set(declarations "")
  foreach(header ${headers})
    file(READ "${header}" text)
    string(APPEND declarations "${text}")
  endforeach()
  set(missing "")
  foreach(function ${LIBUBERTOOTH_REQUIRED_FUNCTIONS})
    # Surrounded by something that cannot be part of a C identifier, so a
    # function whose name is the start of another's is not found in it.
    string(REGEX MATCH "[^A-Za-z0-9_]${function}[^A-Za-z0-9_]" declared
           "${declarations}")
    if(NOT declared)
      list(APPEND missing ${function})
    endif()
  endforeach()

  if(NOT missing)
    # The headers name every one of them, so this is not a libubertooth that
    # is too old: the headers and the library are from different installs, or
    # the library cannot be linked where it is. Reinstalling is still the
    # likely fix, so the commands come too.
    message(FATAL_ERROR
      "the libubertooth found at ${LIBUBERTOOTH_INCLUDE_DIR} has everything "
      "the Ubertooth host tools were checked for, but a program that uses "
      "all of it and links ${LIBUBERTOOTH_LIBRARIES} does not build. The "
      "attempt is in ${CMAKE_BINARY_DIR}/CMakeFiles/CMakeConfigureLog.yaml "
      "(CMake older than 3.26: CMakeFiles/CMakeError.log).\n" "${fix}")
  endif()

  # One line for what is wrong, then the commands that fix it.
  string(REPLACE ";" ", " missing_names "${missing}")
  message(FATAL_ERROR
    "the libubertooth installed at ${LIBUBERTOOTH_INCLUDE_DIR} is older than "
    "these tools: it does not have ${missing_names}, which they call.\n"
    "${fix}")
endfunction()


# A remembered answer is only as good as the files it names. A rebuild into
# another prefix, or an uninstall, leaves the remembered header and library
# gone, and taking them on trust would report libubertooth found and leave the
# build to fail on the missing header, so whichever of the two paths has gone
# is searched for again.
#
# The header and the library are checked one at a time, because a configure
# can remember either without the other.
#
# Each drops the cache entry and any ordinary variable of the same name:
# find_path() and find_library() below skip their search while either holds a
# path, so one left behind would be reported again.
if (LIBUBERTOOTH_INCLUDE_DIR AND
    NOT EXISTS "${LIBUBERTOOTH_INCLUDE_DIR}/ubertooth.h")
  message(STATUS "libubertooth's header has moved since the last configure "
    "(${LIBUBERTOOTH_INCLUDE_DIR}/ubertooth.h is gone); searching for it "
    "again")
  unset(LIBUBERTOOTH_INCLUDE_DIR CACHE)
  unset(LIBUBERTOOTH_INCLUDE_DIR)
endif()

# A library entry that is not an absolute path is left alone: a user may have
# set it to a bare link name, which nothing here can check. Where one entry of
# a list has gone the whole list is dropped, and find_library() answers with
# the one libubertooth it finds.
foreach(_libubertooth_library IN LISTS LIBUBERTOOTH_LIBRARIES)
  if (IS_ABSOLUTE "${_libubertooth_library}" AND
      NOT EXISTS "${_libubertooth_library}")
    message(STATUS "libubertooth's library has moved since the last configure "
      "(${_libubertooth_library} is gone); searching for it again")
    unset(LIBUBERTOOTH_LIBRARIES CACHE)
    unset(LIBUBERTOOTH_LIBRARIES)
    break()
  endif()
endforeach()
unset(_libubertooth_library)

# pkg-config's answer is remembered too, in FindPkgConfig's own marker, and
# pkg_check_modules() below does nothing while it is set: the search would
# take the directory that has gone as its hint instead of asking pkg-config
# where libubertooth is now. So it goes whenever there is a search to do.
if (NOT LIBUBERTOOTH_INCLUDE_DIR OR NOT LIBUBERTOOTH_LIBRARIES)
  unset(__pkg_config_checked_PC_LIBUBERTOOTH CACHE)
endif()

if (LIBUBERTOOTH_INCLUDE_DIR AND LIBUBERTOOTH_LIBRARIES)

  # in cache already
  set(LIBUBERTOOTH_FOUND TRUE)

else (LIBUBERTOOTH_INCLUDE_DIR AND LIBUBERTOOTH_LIBRARIES)
  IF (NOT WIN32)
    # use pkg-config to get the directories and then use these values
    # in the FIND_PATH() and FIND_LIBRARY() calls
    find_package(PkgConfig)
    pkg_check_modules(PC_LIBUBERTOOTH QUIET libubertooth)
  ENDIF(NOT WIN32)

  FIND_PATH(LIBUBERTOOTH_INCLUDE_DIR
    NAMES ubertooth.h
    HINTS $ENV{LIBUBERTOOTH_DIR}/include ${PC_LIBUBERTOOTH_INCLUDEDIR}
    PATHS /usr/include /usr/local/include /opt/local/include
    ${CMAKE_SOURCE_DIR}/../libubertooth/src
    ${LIBUBERTOOTH_INCLUDE_DIR}
  )

  set(libubertooth_library_names ubertooth ubertooth-static)

  FIND_LIBRARY(LIBUBERTOOTH_LIBRARIES
    NAMES ${libubertooth_library_names}
    HINTS $ENV{LIBUBERTOOTH_DIR}/lib ${PC_LIBUBERTOOTH_LIBDIR}
    PATHS /usr/local/lib /usr/lib /opt/local/lib ${PC_LIBUBERTOOTH_LIBDIR} ${PC_LIBUBERTOOTH_LIBRARY_DIRS} ${CMAKE_SOURCE_DIR}/../libubertooth/src
  )

  include(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBUBERTOOTH DEFAULT_MSG LIBUBERTOOTH_LIBRARIES LIBUBERTOOTH_INCLUDE_DIR)

  MARK_AS_ADVANCED(LIBUBERTOOTH_INCLUDE_DIR LIBUBERTOOTH_LIBRARIES)

endif (LIBUBERTOOTH_INCLUDE_DIR AND LIBUBERTOOTH_LIBRARIES)

# Outside the branch above, so a build directory that has the paths cached is
# checked too: a libubertooth replaced by an older one is the case this
# catches. Once per configure, not once per find_package(UBERTOOTH) and not
# cached, through a global property rather than a cache entry: CMake throws
# the property away at the end of the run, so the next configure asks the
# installed libubertooth again.
if (LIBUBERTOOTH_FOUND)
  get_property(ubertooth_functions_checked GLOBAL PROPERTY
               LIBUBERTOOTH_FUNCTIONS_CHECKED)
  if (NOT ubertooth_functions_checked)
    ubertooth_require_functions()
    set_property(GLOBAL PROPERTY LIBUBERTOOTH_FUNCTIONS_CHECKED TRUE)
  endif()
endif (LIBUBERTOOTH_FOUND)