# - Try to find the freetype library
# Once done this defines
#
#  LIBBLUETOOTH_FOUND - system has libbluetooth
#  LIBBLUETOOTH_INCLUDE_DIR - the libbluetooth include directory
#  LIBBLUETOOTH_LIBRARIES - Link these to use libbluetooth

# Copyright (c) 2013  Dominic Spill, <dominicgs@gmail.com>
# Copyright (c) 2006, 2008  Laurent Montel, <montel@kde.org>
#
# Redistribution and use is allowed according to the terms of the BSD license.
# For details see the accompanying COPYING-CMAKE-SCRIPTS file.


# A remembered answer is only as good as the files it names. A package upgrade
# or an uninstall leaves the remembered header and library gone, and taking
# them on trust would report libbluetooth found and leave the build to fail on
# the missing header, so whichever of the two paths has gone is searched for
# again.
#
# The header and the library are checked one at a time, because a configure
# can remember either without the other.
#
# Each drops the cache entry and any ordinary variable of the same name:
# find_path() and find_library() below skip their search while either holds a
# path, so one left behind would be reported again.
if (LIBBLUETOOTH_INCLUDE_DIR AND
    NOT EXISTS "${LIBBLUETOOTH_INCLUDE_DIR}/hci.h")
  message(STATUS "libbluetooth's header has moved since the last configure "
    "(${LIBBLUETOOTH_INCLUDE_DIR}/hci.h is gone); searching for it again")
  unset(LIBBLUETOOTH_INCLUDE_DIR CACHE)
  unset(LIBBLUETOOTH_INCLUDE_DIR)
endif()

# A library entry that is not an absolute path is left alone: a user may have
# set it to a bare link name, which nothing here can check. Where one entry of
# a list has gone the whole list is dropped, and find_library() answers with
# the one libbluetooth it finds.
foreach(_libbluetooth_library IN LISTS LIBBLUETOOTH_LIBRARIES)
  if (IS_ABSOLUTE "${_libbluetooth_library}" AND
      NOT EXISTS "${_libbluetooth_library}")
    message(STATUS "libbluetooth's library has moved since the last configure "
      "(${_libbluetooth_library} is gone); searching for it again")
    unset(LIBBLUETOOTH_LIBRARIES CACHE)
    unset(LIBBLUETOOTH_LIBRARIES)
    break()
  endif()
endforeach()
unset(_libbluetooth_library)

# pkg-config's answer is remembered too, in FindPkgConfig's own marker, and
# pkg_check_modules() below does nothing while it is set: the search would
# take the directory that has gone as its hint instead of asking pkg-config
# where libbluetooth is now. So it goes whenever there is a search to do.
if (NOT LIBBLUETOOTH_INCLUDE_DIR OR NOT LIBBLUETOOTH_LIBRARIES)
  unset(__pkg_config_checked_PC_LIBBLUETOOTH CACHE)
endif()

if (LIBBLUETOOTH_INCLUDE_DIR AND LIBBLUETOOTH_LIBRARIES)

  # in cache already
  set(LIBBLUETOOTH_FOUND TRUE)

else (LIBBLUETOOTH_INCLUDE_DIR AND LIBBLUETOOTH_LIBRARIES)
  IF (NOT WIN32)
    # use pkg-config to get the directories and then use these values
    # in the FIND_PATH() and FIND_LIBRARY() calls
    find_package(PkgConfig)
    pkg_check_modules(PC_LIBBLUETOOTH bluez)
  ENDIF(NOT WIN32)

  FIND_PATH(LIBBLUETOOTH_INCLUDE_DIR hci.h
    PATHS ${PC_LIBBLUETOOTH_INCLUDEDIR} ${PC_LIBBLUETOOTH_INCLUDE_DIRS}
    PATHS /usr/include /usr/local/include /usr/include/bluetooth
    /usr/local/include/bluetooth)

  FIND_LIBRARY(LIBBLUETOOTH_LIBRARIES NAMES bluetooth
    PATHS ${PC_LIBBLUETOOTH_LIBDIR} ${PC_LIBBLUETOOTH_LIBRARY_DIRS})

  include(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBBLUETOOTH DEFAULT_MSG LIBBLUETOOTH_LIBRARIES LIBBLUETOOTH_INCLUDE_DIR)

  MARK_AS_ADVANCED(LIBBLUETOOTH_INCLUDE_DIR LIBBLUETOOTH_LIBRARIES)

endif (LIBBLUETOOTH_INCLUDE_DIR AND LIBBLUETOOTH_LIBRARIES)