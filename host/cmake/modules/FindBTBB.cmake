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
set(LIBBTBB_PINNED_REF "7e1cdf0336857a714da1af8c7496fe32453ac6d0")

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


# The commit the libbtbb that was found was built from, where that can be
# read, with libbtbb's trailing "*" if the tree it was built from had
# uncommitted changes. <reason> says why it could not be read, for the report
# to give the right one: "none", "ambiguous", or "no-file".
#
# libbtbb's build compiles its release string into the library
# (-DRELEASE="git-<abbreviated commit>") and returns it from
# btbb_get_release(). That is the only commit it records anywhere: the version
# in libbtbb.pc is the library's ABI version, which moves with the ABI and not
# with a commit, and btbb.h has no version macro. So the string is read out of
# the library file, which needs no compiler to build a program and no loader
# to run one, and so works for a static library and for a cross-compiled
# build.
# The pin is a commit while libbtbb fixes are landing, and a release points it
# at a matching tag of the libbtbb fork instead. A tag names no commit here,
# and resolving it would need libbtbb's repository, which a build of these
# tools does not have, so with a tag pin nothing below can say whether the
# library it found is the right one. The libbtbb pin workflow draws the same
# line, and reads a pin of forty hex digits as a commit whatever case it is
# written in. The length is asked on its own because CMake's regular
# expressions have no repetition count.
function(btbb_pin_is_commit out)
  string(LENGTH "${LIBBTBB_PINNED_REF}" length)
  if(length EQUAL 40 AND LIBBTBB_PINNED_REF MATCHES "^[0-9a-fA-F]+$")
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()


function(btbb_library_commit out reason)
  set(${out} "" PARENT_SCOPE)

  # The first entry, which is the libbtbb find_library() found; a hand-set
  # list names it first too. Only that one, so the commit reported is always
  # the commit of the library the report names. An entry that is not an
  # absolute path is a bare link name the user set, and there is no file.
  list(GET LIBBTBB_LIBRARIES 0 library)
  if(NOT IS_ABSOLUTE "${library}" OR NOT EXISTS "${library}"
     OR IS_DIRECTORY "${library}")
    set(${reason} "no-file" PARENT_SCOPE)
    return()
  endif()

  # Seven hex digits at least, written out because CMake's regular
  # expressions have no repetition count: a shorter match would be compared
  # against the pin on too few characters to mean anything. Not anchored, and
  # the hex digits are matched greedily, because libbtbb's string sits in the
  # library next to whatever the compiler put beside it. Text run up against
  # the string leaves a second candidate, and more hex digits run up against
  # it read as a longer abbreviation that then does not match the pin, so such
  # a library warns rather than passing quietly. libbtbb writes the string as
  # its own NUL-terminated literal, so nothing is run up against it.
  set(hex "[0-9a-fA-F]")
  set(pattern "git-${hex}${hex}${hex}${hex}${hex}${hex}${hex}+\\*?")

  # Each run of printable bytes holding the pattern, then the pattern out of
  # each run. Two steps, because the pattern cannot be anchored.
  #
  # No LENGTH_MAXIMUM, which would lose the commit rather than save work: a
  # run longer than the limit is dropped, so a library with one long stretch
  # of printable bytes around libbtbb's string would report no commit at all,
  # which is the silence this whole report exists to remove. Reading a 34 MB
  # library costs about a second, and libbtbb installs at about 250 KB.
  file(STRINGS "${library}" runs REGEX "${pattern}")
  set(candidates "")
  foreach(run IN LISTS runs)
    string(REGEX MATCHALL "${pattern}" matches "${run}")
    list(APPEND candidates ${matches})
  endforeach()

  list(REMOVE_DUPLICATES candidates)
  list(LENGTH candidates count)
  if(count EQUAL 1)
    string(REGEX REPLACE "^git-" "" commit "${candidates}")
    set(${out} "${commit}" PARENT_SCOPE)
    set(${reason} "" PARENT_SCOPE)
  elseif(count EQUAL 0)
    set(${reason} "none" PARENT_SCOPE)
  else()
    # Something else in the library matched as well, a build directory named
    # after a commit for instance, and nothing here can tell which one is
    # libbtbb's. Reported as unknown rather than guessed, either way round: a
    # guess that picked the pin would hide a stale library, and one that
    # picked the other would warn about a correct install.
    set(${reason} "ambiguous" PARENT_SCOPE)
  endif()
endfunction()


# Say which libbtbb this build found and which commit the tree is built
# against, every time, and warn when they are not the same.
#
# The check below refuses a libbtbb missing a function the host calls, which
# is what a pin bump that adds one leaves behind. A bump that only changes how
# libbtbb behaves adds no function, so that check passes and the tools then
# run with the older library's behavior, which looks exactly like a change in
# the host's own code. This line is what tells a reader of a measurement which
# libbtbb it was measured against.
#
# A warning and not a refusal, because a libbtbb from a distribution package
# records no commit at all and may be fine for what its user is doing. So
# "cannot be read" is reported and nothing warns, except where commit-looking
# strings disagree: that is the one unknown where the library may well be the
# wrong one, and a silent line is how this went unnoticed before.
#
# The line that is always printed is a STATUS, with the rest of the configure
# report; a WARNING, which CMake puts on standard error, is for the cases a
# reader has to see among the hundreds of lines a build produces.
function(btbb_report_pin)
  btbb_library_commit(commit reason)
  btbb_pin_is_commit(pin_is_commit)
  list(GET LIBBTBB_LIBRARIES 0 library)

  # STREQUAL rather than if(NOT commit), which asks whether the value is one
  # of CMake's false constants and would answer for a commit of "0".
  if(commit STREQUAL "")
    if(reason STREQUAL "ambiguous")
      set(why "more than one commit is recorded in it")
    elseif(reason STREQUAL "no-file")
      set(why "there is no library file behind that name")
    else()
      # No cause asserted: a libbtbb built from a source archive records a
      # release name instead of a commit, and one built with git's
      # abbreviation set unusually short records a commit this does not read.
      set(why "nothing in it reads as the commit it was built from")
    endif()
    message(STATUS "libbtbb: ${library}, commit unknown (${why}); this tree "
      "is built against ${LIBBTBB_PINNED_REF}")
    # Which one libbtbb's own is would decide nothing against a pin that is
    # not a commit, so that pin warns about neither this nor a commit it can
    # read, and the status line above is the whole report.
    if(NOT reason STREQUAL "ambiguous" OR NOT pin_is_commit)
      return()
    endif()
    # Its own sentence rather than the parenthetical above: that one is read
    # as part of a status line, and this one has to stand alone. No rebuild
    # commands with it, unlike the mismatch below: nothing here is known to
    # be wrong, and the library may well be the pinned one.
    message(WARNING
      "the libbtbb found at ${library} records more than one commit, and "
      "nothing here can tell which one libbtbb was built from, so this build "
      "cannot say whether it is the libbtbb this tree is built against "
      "(${LIBBTBB_PINNED_REF}).")
    return()
  endif()

  # libbtbb appends "*" to the commit for a tree with uncommitted changes, so
  # a library built from the pinned commit plus local edits is not the pinned
  # libbtbb and is reported as its own case.
  set(modified FALSE)
  if(commit MATCHES "\\*$")
    set(modified TRUE)
    string(REGEX REPLACE "\\*$" "" commit "${commit}")
  endif()

  if(modified)
    set(built "built from commit ${commit} with uncommitted changes")
  else()
    set(built "built from commit ${commit}")
  endif()

  # A library built from an edited tree is not the pinned libbtbb whatever its
  # commit is, and that is decided without the pin, so the warning below is
  # reached either way round.
  if(NOT modified)
    # Where the pin is not a commit there is nothing to compare this one
    # with, so both ends are named and nothing is claimed.
    if(NOT pin_is_commit)
      message(STATUS "libbtbb: ${library}, ${built}; this tree is built "
        "against ${LIBBTBB_PINNED_REF}, which names no commit to compare it "
        "with")
      return()
    endif()

    # libbtbb records git's abbreviation and the pin is written in full,
    # which the libbtbb pin workflow enforces by reading it out of build.yml,
    # so the recorded commit has to be a prefix of the pin and not the other
    # way about. A prefix search rather than a regular expression match, so a
    # commit is only ever compared as text.
    string(TOLOWER "${commit}" commit_lower)
    string(TOLOWER "${LIBBTBB_PINNED_REF}" pin_lower)
    string(FIND "${pin_lower}" "${commit_lower}" commit_at)

    # The pin in full even where they agree, as in every other case: the line
    # is read out of the log a measurement was taken from, where "the pinned
    # commit" alone would send the reader looking for which commit that was.
    if(commit_at EQUAL 0)
      message(STATUS "libbtbb: ${library}, built from commit ${commit}, which "
        "is the commit this tree is built against (${LIBBTBB_PINNED_REF})")
      return()
    endif()
  endif()

  message(STATUS "libbtbb: ${library}, ${built}; this tree is built against "
    "${LIBBTBB_PINNED_REF}")

  btbb_rebuild_instructions(fix)
  # Nothing here about whether the build will succeed: the check below has not
  # run yet, and it refuses a libbtbb missing something these tools call. This
  # is about the libbtbb that will be linked if the build does go on.
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
