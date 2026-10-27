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
# it. It is repeated here so the failure below can name the commit to rebuild
# at, which is the one thing a reader needs and cannot guess.
set(LIBBTBB_PINNED_REPO "SqueakyFloors/libbtbb")
set(LIBBTBB_PINNED_REF "2fd0d391d99e9b0620146f63f439c1ec74f79861")

# Every libbtbb function this tree calls. libbtbb carries no version macro, so
# "new enough" is decided by asking the installed one for the declarations the
# code needs: a libbtbb that has all of these builds, and one that does not is
# named in the error below instead of arriving as an undeclared-function error
# in the middle of libubertooth.
#
# tools/check-btbb-symbols.sh compares this list with the calls in the host
# sources, and the scripts CI job runs it, so a call added to a function that
# is not listed here fails that check rather than going unnoticed.
set(LIBBTBB_REQUIRED_FUNCTIONS
  btbb_find_ac
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
  lell_allocate_and_decode_n
  lell_get_access_address
  lell_get_access_address_offenses
  lell_get_payload_length
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
)

# Fail the configure step when the libbtbb that was found is missing any of
# them, saying which ones and how to install one that has them.
#
# A function, so the CMAKE_REQUIRED_* variables the probe needs stay out of the
# caller's scope.
function(btbb_require_functions)
  include(CheckCSourceCompiles)
  set(CMAKE_REQUIRED_INCLUDES ${LIBBTBB_INCLUDE_DIR})
  set(CMAKE_REQUIRED_LIBRARIES ${LIBBTBB_LIBRARIES})

  # Taking the address of each function needs a declaration from btbb.h and a
  # definition in the library, so one compile and link answers for the header
  # and the library together. All of them at once, because the common case is
  # that nothing is missing and that case has to cost one compile.
  set(body "#include <btbb.h>\nint main(void) {\n\tconst void *used[] = {\n")
  foreach(function ${LIBBTBB_REQUIRED_FUNCTIONS})
    string(APPEND body "\t\t(const void *)&${function},\n")
  endforeach()
  string(APPEND body "\t};\n\treturn used[0] == 0;\n}\n")

  # Not cached: the installed libbtbb can change under a build directory that
  # configured successfully once, which is exactly the developer who built the
  # tools months ago and has come back to them.
  unset(LIBBTBB_HAS_REQUIRED_FUNCTIONS CACHE)
  check_c_source_compiles("${body}" LIBBTBB_HAS_REQUIRED_FUNCTIONS)
  if(LIBBTBB_HAS_REQUIRED_FUNCTIONS)
    return()
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
  foreach(function ${LIBBTBB_REQUIRED_FUNCTIONS})
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
    # old: the header, the library or the compiler flags disagree with each
    # other.
    message(FATAL_ERROR
      "the libbtbb found at ${LIBBTBB_INCLUDE_DIR} declares the functions the "
      "Ubertooth host tools call, but a program that uses all of them and "
      "links ${LIBBTBB_LIBRARIES} does not build. The last attempt is in "
      "${CMAKE_BINARY_DIR}/CMakeFiles/CMakeConfigureLog.yaml (older CMake: "
      "CMakeFiles/CMakeError.log).")
  endif()

  string(REPLACE ";" ", " missing_names "${missing}")
  # One line for what is wrong, then the commands that fix it. The pinned
  # commit is in it because that is what the reader has to check out, and the
  # pin lives in a CI workflow file where nobody would look.
  message(FATAL_ERROR
    "the libbtbb found at ${LIBBTBB_INCLUDE_DIR} does not have "
    "${missing_names}, which the Ubertooth host tools call.\n"
    "Rebuild and install libbtbb at the commit this fork is built against:\n"
    "    git clone https://github.com/${LIBBTBB_PINNED_REPO}\n"
    "    cd libbtbb\n"
    "    git checkout ${LIBBTBB_PINNED_REF}\n"
    "    cmake -S . -B build -DENABLE_PYTHON=OFF\n"
    "    cmake --build build\n"
    "    sudo cmake --install build\n"
    "then configure this build again; ./install.sh does all of it for you.")
endfunction()


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
  get_property(btbb_functions_checked GLOBAL PROPERTY LIBBTBB_FUNCTIONS_CHECKED)
  if (NOT btbb_functions_checked)
    btbb_require_functions()
    set_property(GLOBAL PROPERTY LIBBTBB_FUNCTIONS_CHECKED TRUE)
  endif()
endif (LIBBTBB_FOUND)
