# Try to find the libubertooth library
#
# Once done this defines:
#  LIBUBERTOOTH_FOUND - system has libubertooth
#  LIBUBERTOOTH_INCLUDE_DIR - the libubertooth include directory
#  LIBUBERTOOTH_LIBRARIES - Link these to use libubertooth
#
# Copyright (c) 2013  Dominic Spill


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

  if(LIBUBERTOOTH_INCLUDE_DIR)
    set(CMAKE_REQUIRED_INCLUDES ${LIBUBERTOOTH_INCLUDE_DIR})
  endif()

  if(LIBUBERTOOTH_LIBRARIES)
    set(CMAKE_REQUIRED_LIBRARIES ${LIBUBERTOOTH_LIBRARIES})
  endif()

  include(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBUBERTOOTH DEFAULT_MSG LIBUBERTOOTH_LIBRARIES LIBUBERTOOTH_INCLUDE_DIR)

  MARK_AS_ADVANCED(LIBUBERTOOTH_INCLUDE_DIR LIBUBERTOOTH_LIBRARIES)

endif (LIBUBERTOOTH_INCLUDE_DIR AND LIBUBERTOOTH_LIBRARIES)