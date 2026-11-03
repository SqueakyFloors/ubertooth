# Try to find the libbtbb library
#
# Once done this defines:
#  LIBBTBB_FOUND - system has libbtbb
#  LIBBTBB_INCLUDE_DIR - the libbtbb include directory
#  LIBBTBB_LIBRARIES - Link these to use libbtbb
#
# Copyright (c) 2013  Dominic Spill


# The libbtbb these tools are built against, pinned to a commit so a push to
# libbtbb cannot change an Ubertooth build. The pin is bumped in
# .github/workflows/build.yml; the libbtbb-pin workflow fails until this copy
# and the others (install.sh, COMPATIBILITY.md, the build guides) agree with
# it. It is repeated here so the messages below can name the commit to build
# at, which is the one thing a reader needs and cannot guess, and so the
# configure step can compare it with the libbtbb it found.
set(LIBBTBB_PINNED_REPO "SqueakyFloors/libbtbb")
set(LIBBTBB_PINNED_REF "5c4e535968b58db2f28e2f3bce0f02ca1c02b03e")

# Every libbtbb function this tree calls. libbtbb carries no version macro, so
# "new enough" is decided by asking the installed one for the declarations the
# code needs: a libbtbb that has all of these builds, and one that does not is
# named in the error below instead of arriving as an undeclared-function error
# in the middle of libubertooth.
#
# tools/check-btbb-symbols.sh compares this list with the calls in the host
# sources, and the scripts CI job runs it, so a call added to a function that
# is not listed here fails that check rather than going unnoticed.
#
# Every call in the tree, not the calls this configuration will compile: a few
# of these are reached only by ubertooth-scan and ubertooth-follow, which are
# skipped where there is no libbluetooth, and asking for a different set per
# machine would mean a libbtbb that satisfies a macOS build and fails a Linux
# one later.
set(LIBBTBB_REQUIRED_FUNCTIONS
  btbb_company_name
  btbb_find_ac_n
  btbb_gen_syncword
  btbb_get_release
  btbb_get_version
  btbb_init
  btbb_init_piconet
  btbb_init_survey
  btbb_next_survey_result
  btbb_packet_get_ac_errors
  btbb_packet_get_channel
  btbb_packet_get_clkn
  btbb_packet_get_lap
  btbb_packet_set_data
  btbb_packet_set_modulation
  btbb_packet_set_transport
  btbb_packet_unref
  btbb_pcap_append_packet
  btbb_pcap_close
  btbb_pcap_create_file
  btbb_pcapng_append_packet
  btbb_pcapng_close
  btbb_pcapng_create_file
  btbb_pcapng_record_bdaddr
  btbb_piconet_clear_channel_seen
  btbb_piconet_drop_clock
  btbb_piconet_get_afh_map
  btbb_piconet_get_bdaddr
  btbb_piconet_get_channel_seen
  btbb_piconet_get_clk_offset
  btbb_piconet_get_flag
  btbb_piconet_get_lap
  btbb_piconet_get_uap
  btbb_piconet_new
  btbb_piconet_set_afh_map
  btbb_piconet_set_channel_seen
  btbb_piconet_set_clk_offset
  btbb_piconet_set_flag
  btbb_piconet_set_uap
  btbb_piconet_unref
  btbb_print_afh_map
  btbb_process_packet
  btbb_survey_piconet
  lell_allocate_and_decode_n
  lell_get_access_address
  lell_get_access_address_offenses
  lell_get_payload_length
  lell_malformed_count
  lell_packet_is_data
  lell_packet_unref
  lell_pcap_append_packet
  lell_pcap_append_ppi_packet
  lell_pcap_close
  lell_pcap_create_file
  lell_pcap_ppi_create_file
  lell_pcapng_append_packet
  lell_pcapng_close
  lell_pcapng_create_file
  lell_print
  lell_set_malformed_verbose
)

# The libbtbb macros this tree reads. Two of its functions report something a
# tool has to act on through a named value rather than a return code of its
# own, so a libbtbb without them is as unusable here as one missing a
# function, and the probe below asks about both together. An address cannot be
# taken of a macro, so each one is a compile-time check in the probe instead.
set(LIBBTBB_REQUIRED_MACROS
  BTBB_PROCESS_LOCK_LOST
  LELL_PCAPNG_CONNECT_INFO_NOT_RECORDED
)

# The commands that install the libbtbb this tree is built against, for the
# messages below to end with. The pinned commit is in them because that is
# what the reader has to check out, and the pin otherwise lives in a CI
# workflow file where nobody would look. The commands are the build guide's,
# which work on the CMake 3.5 this project still supports: cmake -S/-B needs
# 3.13 and cmake --install needs 3.15.
#
# One quoted string, and referenced as one wherever it is used: a semicolon in
# an unquoted CMake variable is a list separator and would be eaten.
function(btbb_rebuild_instructions out)
  set(fix "Rebuild and install libbtbb at the commit this fork is built against:\n")
  string(APPEND fix
    "    git clone https://github.com/${LIBBTBB_PINNED_REPO}.git\n"
    "    cd libbtbb\n"
    "    git checkout ${LIBBTBB_PINNED_REF}\n"
    "    mkdir build && cd build\n"
    "    cmake -DENABLE_PYTHON=OFF .. && make\n"
    "    sudo make install\n"
    "    sudo ldconfig     # Linux only\n"
    "then configure this build again, or let ./install.sh do all of it for you.")
  set(${out} "${fix}" PARENT_SCOPE)
endfunction()


# The commit the libbtbb that was found was built from, where it can be read.
#
# libbtbb's build compiles its own release string into the library
# (-DRELEASE="git-<abbreviated commit>") and returns it from
# btbb_get_release(), and that is the only commit it records anywhere: the
# version in libbtbb.pc is the library's ABI version, 1.3, which changes when
# the ABI does and not when a commit does, and btbb.h has no version macro at
# all. So the string is read out of the library file, which needs no compiler
# to build a program, no loader to run one, and works just as well for a
# static library or a cross-compiled build.
#
# Sets <out> to the abbreviated commit, with libbtbb's trailing "*" if the
# tree it was built from had uncommitted changes, or to the empty string when
# it cannot be read. Two different candidates mean some other string in the
# library matched as well -- a build directory named after a commit, say --
# and nothing here can tell which one is libbtbb's, so that is "cannot be
# read" rather than a guess, and <ambiguous> says which of the two it was so
# the report can give the right reason.
function(btbb_library_commit out ambiguous)
  # Seven hex digits at least, written out because CMake's regular
  # expressions have no repetition count: that is the shortest abbreviation
  # git produces for a repository this size, and a shorter match would be
  # compared against the pin on too few characters to mean anything.
  set(hex "[0-9a-fA-F]")
  set(pattern "git-${hex}${hex}${hex}${hex}${hex}${hex}${hex}+\\*?")

  set(candidates "")
  foreach(library IN LISTS LIBBTBB_LIBRARIES)
    # A library entry that is not an absolute path is a bare link name the
    # user set, and there is no file here to read.
    if(NOT IS_ABSOLUTE "${library}" OR NOT EXISTS "${library}")
      continue()
    endif()
    # Each run of printable bytes that holds the pattern, then the pattern
    # itself out of each run. Two steps because the pattern is not anchored:
    # libbtbb's string sits in the library next to whatever the compiler put
    # beside it, so the run file(STRINGS) returns can carry other text on
    # either side of it.
    file(STRINGS "${library}" runs REGEX "${pattern}" LENGTH_MAXIMUM 4096)
    foreach(run IN LISTS runs)
      string(REGEX MATCHALL "${pattern}" matches "${run}")
      list(APPEND candidates ${matches})
    endforeach()
  endforeach()

  list(REMOVE_DUPLICATES candidates)
  list(LENGTH candidates count)
  if(count GREATER 1)
    set(${out} "" PARENT_SCOPE)
    set(${ambiguous} TRUE PARENT_SCOPE)
    return()
  endif()
  set(${ambiguous} FALSE PARENT_SCOPE)
  if(count EQUAL 0)
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  string(REGEX REPLACE "^git-" "" commit "${candidates}")
  set(${out} "${commit}" PARENT_SCOPE)
endfunction()


# Say which libbtbb this build found and which commit the tree is built
# against, every time, and warn when the two are not the same.
#
# The function check below refuses a libbtbb that cannot satisfy a call the
# host makes, which is what a pin bump that adds an entry point leaves behind.
# A bump that only changes how libbtbb behaves adds no entry point, so that
# check passes and the tools then run with the older library's behavior, which
# looks exactly like a change in the host's own code. One line in the
# configure log is what tells a reader of a measurement which libbtbb it was
# measured against.
#
# Not a refusal: a libbtbb from a distribution package is unlikely to record a
# commit at all, and may well be fine for what its user is doing. The
# mismatch a reader has to see is reported, and the build goes on.
#
# message(STATUS) for the line that is always printed, because it belongs with
# the rest of the configure report on standard output, beside the other
# STATUS lines this module writes. message(WARNING) for the mismatch, because
# the user has to see it among hundreds of lines of configure and build
# output, and CMake puts a warning on standard error.
function(btbb_report_pin)
  btbb_library_commit(commit ambiguous)
  # The library rather than the header: the behavior a measurement sees comes
  # from the library, and this is the path the commit was read from. The first
  # entry, because that is the one find_library() found; a hand-set list of
  # several is reported by its first too, which still names the install.
  list(GET LIBBTBB_LIBRARIES 0 library)

  # STREQUAL rather than if(NOT commit), which asks whether the value is one
  # of CMake's false constants and would answer for a commit of "0".
  if(commit STREQUAL "")
    if(ambiguous)
      set(why "more than one commit is recorded in it, so which one is "
              "libbtbb's own cannot be told")
    else()
      set(why "a libbtbb records the commit it was built from only when it "
              "was built from a git clone")
    endif()
    message(STATUS "libbtbb: ${library}, commit unknown (" ${why} "); this "
      "tree is built against ${LIBBTBB_PINNED_REF}")
    return()
  endif()

  # libbtbb marks a tree with uncommitted changes by appending "*" to the
  # commit, so a library built from the pinned commit plus local edits is not
  # the pinned libbtbb and is reported as its own case.
  set(modified FALSE)
  if(commit MATCHES "\\*$")
    set(modified TRUE)
    string(REGEX REPLACE "\\*$" "" commit "${commit}")
  endif()

  # Either can be the abbreviation of the other: libbtbb records git's short
  # form, the pin is written in full, and a pin could be written short. A
  # prefix search rather than a regular expression match, so a commit is only
  # ever compared as text.
  string(TOLOWER "${commit}" commit_lower)
  string(TOLOWER "${LIBBTBB_PINNED_REF}" pin_lower)
  string(FIND "${pin_lower}" "${commit_lower}" commit_at)
  string(FIND "${commit_lower}" "${pin_lower}" pin_at)
  if(commit_at EQUAL 0 OR pin_at EQUAL 0)
    set(agrees TRUE)
  else()
    set(agrees FALSE)
  endif()

  # The pin in full even here, as in every other case: the line is read out of
  # the log a measurement was taken from, where "the pinned commit" alone
  # would send the reader looking for which commit that was.
  if(agrees AND NOT modified)
    message(STATUS "libbtbb: ${library}, built from commit ${commit}, which "
      "is the commit this tree is built against (${LIBBTBB_PINNED_REF})")
    return()
  endif()

  if(modified)
    set(built "built from commit ${commit} with uncommitted changes")
  else()
    set(built "built from commit ${commit}")
  endif()
  message(STATUS "libbtbb: ${library}, ${built}; this tree is built against "
    "${LIBBTBB_PINNED_REF}")

  btbb_rebuild_instructions(fix)
  # Nothing here about whether the build will succeed: the check below has not
  # run yet, and it refuses a libbtbb missing something these tools call. What
  # this warning is about is the libbtbb that will be linked if the build does
  # go on.
  message(WARNING
    "the libbtbb found at ${library} was ${built}, which is not the libbtbb "
    "this tree is built against (${LIBBTBB_PINNED_REF}). These tools behave "
    "the way the libbtbb they link behaves, so a capture, a recovered clock "
    "or a channel map measured against this build is a result for this "
    "libbtbb and not for the one the tree names.\n" "${fix}")
endfunction()


# Fail the configure step when the libbtbb that was found is missing any of
# them, saying which ones and how to install one that has them.
#
# A function, so the CMAKE_REQUIRED_* variables the probe needs stay out of the
# caller's scope.
function(btbb_require_functions)
  include(CheckCSourceCompiles)
  # Quoted: a path with a space in it, or a LIBBTBB_LIBRARIES holding more
  # than one library, would otherwise be split into separate arguments.
  set(CMAKE_REQUIRED_INCLUDES "${LIBBTBB_INCLUDE_DIR}")
  set(CMAKE_REQUIRED_LIBRARIES "${LIBBTBB_LIBRARIES}")

  # Taking the address of each function needs a declaration from btbb.h, and
  # keeping that address needs a definition in the library, so one compile and
  # link answers for the header and the library together. All of them at once,
  # because the common case is that nothing is missing and that case has to
  # cost one compile.
  #
  # Every address is handed to a call the compiler is not allowed to remove,
  # and that is what makes the link half real. Anything it can follow to a
  # dead end it deletes, and the addresses go with it: a table of them inside
  # main() is folded away at -O2, a table at file scope survives the compiler
  # but not -flto or a dead-stripping link, and each address compared against
  # null folds too, because a function's address is never null. A call through
  # a volatile function pointer has to be made, so every argument has to be
  # materialised first, and each one is then a symbol the linker must resolve
  # whatever optimisation the user's CFLAGS ask for.
  #
  # The addresses are passed as variadic arguments, so not one of them is
  # cast. Casting a function pointer to void * is not something C promises,
  # and casting it to another function pointer type is what
  # -Wcast-function-type -Werror refuses: either would turn a correct libbtbb
  # into a refusal that has nothing to do with it.
  set(body "#include <btbb.h>\n")
  # The macros first, so a libbtbb without one fails the same compile.
  foreach(macro ${LIBBTBB_REQUIRED_MACROS})
    string(APPEND body
      "#if !defined(${macro})\n"
      "#error ${macro}\n"
      "#endif\n")
  endforeach()
  string(APPEND body
    "static int btbb_probe_sink(int n, ...) { (void)n; return 0; }\n"
    "static int (*volatile btbb_probe_call)(int, ...) = btbb_probe_sink;\n"
    "int main(int argc, char **argv) {\n"
    "\t(void)argv;\n"
    "\treturn btbb_probe_call(argc")
  foreach(function ${LIBBTBB_REQUIRED_FUNCTIONS})
    string(APPEND body ",\n\t\t&${function}")
  endforeach()
  string(APPEND body ");\n}\n")

  # Not cached: the installed libbtbb can change under a build directory that
  # configured successfully once, which is exactly the developer who built the
  # tools months ago and has come back to them.
  unset(LIBBTBB_HAS_REQUIRED_FUNCTIONS CACHE)
  check_c_source_compiles("${body}" LIBBTBB_HAS_REQUIRED_FUNCTIONS)
  if(LIBBTBB_HAS_REQUIRED_FUNCTIONS)
    return()
  endif()

  # The fix, whichever way the probe failed.
  btbb_rebuild_instructions(fix)

  # The header has to be read to say which functions are missing. It was there
  # when it was found, so this is the libbtbb that has been uninstalled or
  # moved under a build directory that configured once -- the same fix, with
  # nothing to name.
  if(NOT EXISTS "${LIBBTBB_INCLUDE_DIR}/btbb.h")
    message(FATAL_ERROR
      "btbb.h is gone from ${LIBBTBB_INCLUDE_DIR}, where this build found "
      "libbtbb.\n" "${fix}")
  endif()

  # Which functions are missing, read out of btbb.h rather than probed one at
  # a time: the compile above has already proved that something is missing, so
  # this only has to name it, and a probe per function would cost half a
  # minute of nested builds to say what the header plainly does not contain.
  # A name this search fails to find in a header that does declare it would
  # only make the message below less specific, never refuse a libbtbb the
  # compile accepted.
  file(READ "${LIBBTBB_INCLUDE_DIR}/btbb.h" header)
  set(missing "")
  foreach(function ${LIBBTBB_REQUIRED_FUNCTIONS} ${LIBBTBB_REQUIRED_MACROS})
    # Surrounded by something that cannot be part of a C identifier, so a
    # function whose name is the start of another's is not found in it.
    string(REGEX MATCH "[^A-Za-z0-9_]${function}[^A-Za-z0-9_]" declared
           "${header}")
    if(NOT declared)
      list(APPEND missing ${function})
    endif()
  endforeach()

  if(NOT missing)
    # btbb.h names every one of them, so this is not a libbtbb that is too
    # old: the header and the library are from different installs, or the
    # library cannot be linked where it is. Reinstalling the pinned one is
    # still the likely fix, so the commands come too.
    message(FATAL_ERROR
      "the libbtbb found at ${LIBBTBB_INCLUDE_DIR} has everything the "
      "Ubertooth host tools use, but a program that uses all of them and "
      "links ${LIBBTBB_LIBRARIES} does not build. The attempt is in "
      "${CMAKE_BINARY_DIR}/CMakeFiles/CMakeConfigureLog.yaml (CMake older "
      "than 3.26: CMakeFiles/CMakeError.log).\n" "${fix}")
  endif()

  # One line for what is wrong, then the commands that fix it.
  string(REPLACE ";" ", " missing_names "${missing}")
  message(FATAL_ERROR
    "the libbtbb found at ${LIBBTBB_INCLUDE_DIR} does not have "
    "${missing_names}, which the Ubertooth host tools use.\n" "${fix}")
endfunction()


# A remembered answer is only as good as the files it names. A rebuild into
# another prefix, a package upgrade that puts each version in a directory of
# its own, or an uninstall leaves them gone. Taking them on trust would point
# the function check above at a header and a library that are not there, so it
# would fail the configure over a libbtbb that is installed and fine.
# Whichever of the two paths has gone is searched for again.
#
# The header and the library are checked one at a time, because a configure
# can remember either without the other.
#
# Each drops the cache entry and any ordinary variable of the same name:
# find_path() and find_library() below skip their search while either holds a
# path, so one left behind would be reported again.
if (LIBBTBB_INCLUDE_DIR AND NOT EXISTS "${LIBBTBB_INCLUDE_DIR}/btbb.h")
  message(STATUS "libbtbb's header has moved since the last configure "
    "(${LIBBTBB_INCLUDE_DIR}/btbb.h is gone); searching for it again")
  unset(LIBBTBB_INCLUDE_DIR CACHE)
  unset(LIBBTBB_INCLUDE_DIR)
endif()

# A library entry that is not an absolute path is left alone: a user may have
# set it to a bare link name, which nothing here can check. Where one entry of
# a list has gone the whole list is dropped, and find_library() answers with
# the one libbtbb it finds.
foreach(_libbtbb_library IN LISTS LIBBTBB_LIBRARIES)
  if (IS_ABSOLUTE "${_libbtbb_library}" AND NOT EXISTS "${_libbtbb_library}")
    message(STATUS "libbtbb's library has moved since the last configure "
      "(${_libbtbb_library} is gone); searching for it again")
    unset(LIBBTBB_LIBRARIES CACHE)
    unset(LIBBTBB_LIBRARIES)
    break()
  endif()
endforeach()
unset(_libbtbb_library)

# pkg-config's answer is remembered too, in FindPkgConfig's own marker, and
# pkg_check_modules() below does nothing while it is set: the search would
# take the directory that has gone as its hint instead of asking pkg-config
# where libbtbb is now. So it goes whenever there is a search to do.
if (NOT LIBBTBB_INCLUDE_DIR OR NOT LIBBTBB_LIBRARIES)
  unset(__pkg_config_checked_PC_LIBBTBB CACHE)
endif()

if (LIBBTBB_INCLUDE_DIR AND LIBBTBB_LIBRARIES)

  # in cache already
  set(LIBBTBB_FOUND TRUE)

else (LIBBTBB_INCLUDE_DIR AND LIBBTBB_LIBRARIES)
  IF (NOT WIN32)
    # use pkg-config to get the directories and then use these values
    # in the FIND_PATH() and FIND_LIBRARY() calls
    find_package(PkgConfig)
    pkg_check_modules(PC_LIBBTBB QUIET libbtbb)
  ENDIF(NOT WIN32)

  FIND_PATH(LIBBTBB_INCLUDE_DIR
    NAMES btbb.h
    HINTS $ENV{LIBBTBB_DIR}/include ${PC_LIBBTBB_INCLUDEDIR}
    PATHS /usr/include /usr/local/include
    /opt/local/include
    ${CMAKE_SOURCE_DIR}/../libbtbb/src
    ${LIBBTBB_INCLUDE_DIR}
  )

  set(libbtbb_library_names btbb btbb-static)

  FIND_LIBRARY(LIBBTBB_LIBRARIES
    NAMES ${libbtbb_library_names}
    HINTS $ENV{LIBBTBB_DIR}/lib${LIB_SUFFIX} ${PC_LIBBTBB_LIBDIR}
    PATHS /usr/local/lib${LIB_SUFFIX} /usr/lib${LIB_SUFFIX} /opt/local/lib${LIB_SUFFIX} ${PC_LIBBTBB_LIBDIR}
    ${PC_LIBBTBB_LIBRARY_DIRS} ${CMAKE_SOURCE_DIR}/../libbtbb/src
  )

  include(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBBTBB DEFAULT_MSG LIBBTBB_LIBRARIES LIBBTBB_INCLUDE_DIR)

  MARK_AS_ADVANCED(LIBBTBB_INCLUDE_DIR LIBBTBB_LIBRARIES)

endif (LIBBTBB_INCLUDE_DIR AND LIBBTBB_LIBRARIES)

# Outside the branch above, so a build directory that has the paths cached is
# checked too: a libbtbb replaced by an older one is the case this catches.
# Once per configure, not once per find_package(BTBB) and not cached, through
# a global property rather than a cache entry: CMake throws the property away
# at the end of the run, so the next configure asks the installed libbtbb
# again.
if (LIBBTBB_FOUND)
  # Which libbtbb this is, before the check below, so the line is in the log
  # of a configure that then fails as well as one that succeeds. Its own
  # property, because the two answer different questions and either can be
  # wanted without the other.
  get_property(btbb_pin_reported GLOBAL PROPERTY LIBBTBB_PIN_REPORTED)
  if (NOT btbb_pin_reported)
    btbb_report_pin()
    set_property(GLOBAL PROPERTY LIBBTBB_PIN_REPORTED TRUE)
  endif()

  get_property(btbb_functions_checked GLOBAL PROPERTY LIBBTBB_FUNCTIONS_CHECKED)
  if (NOT btbb_functions_checked)
    btbb_require_functions()
    set_property(GLOBAL PROPERTY LIBBTBB_FUNCTIONS_CHECKED TRUE)
  endif()
endif (LIBBTBB_FOUND)
