# - Try to find the freetype library
# Once done this defines
#
#  LIBUSB_FOUND - system has libusb
#  LIBUSB_INCLUDE_DIR - the directory holding libusb.h
#  LIBUSB_INCLUDE_DIRS - the directories to compile with: both the one holding
#                        libusb.h and the one above it
#  LIBUSB_LIBRARIES - Link these to use libusb

# Copyright (c) 2006, 2008  Laurent Montel, <montel@kde.org>
#
# Redistribution and use is allowed according to the terms of the BSD license.
# For details see the accompanying COPYING-CMAKE-SCRIPTS file.


if (LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARIES)

  # in cache already
  set(LIBUSB_FOUND TRUE)

else (LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARIES)
  IF (NOT WIN32)
    # use pkg-config to get the directories and then use these values
    # in the FIND_PATH() and FIND_LIBRARY() calls
    find_package(PkgConfig)
    pkg_check_modules(PC_LIBUSB libusb-1.0)
  ENDIF(NOT WIN32)

  set(LIBUSB_LIBRARY_NAME usb-1.0)
  IF(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
    set(LIBUSB_LIBRARY_NAME usb)
  ENDIF(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")

  FIND_PATH(LIBUSB_INCLUDE_DIR libusb.h
    PATHS ${PC_LIBUSB_INCLUDEDIR} ${PC_LIBUSB_INCLUDE_DIRS})

  FIND_LIBRARY(LIBUSB_LIBRARIES NAMES ${LIBUSB_LIBRARY_NAME}
    PATHS ${PC_LIBUSB_LIBDIR} ${PC_LIBUSB_LIBRARY_DIRS})

  include(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBUSB DEFAULT_MSG LIBUSB_LIBRARIES LIBUSB_INCLUDE_DIR)

  MARK_AS_ADVANCED(LIBUSB_INCLUDE_DIR LIBUSB_LIBRARIES)

endif (LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARIES)

# Two directories, because the sources spell the header both ways. libusb's
# own <libusb.h> resolves against the directory libusb.h sits in, which is
# what LIBUSB_INCLUDE_DIR names; ubertooth_control.h includes
# <libusb-1.0/libusb.h>, which resolves against the directory above that. A
# Homebrew install keeps the header a level below the prefix include
# directory (/opt/homebrew/Cellar/libusb/1.0.30/include/libusb-1.0/libusb.h),
# so there the two are different places and a target given only
# LIBUSB_INCLUDE_DIR cannot compile <libusb-1.0/libusb.h>. On Linux the
# header is under /usr/include, which the compiler searches anyway.
#
# Reporting both is what lets a target ask libusb for libusb's header. Before
# this, every target also passed libbtbb's include directory, which on a
# Homebrew install is the prefix include directory and so happens to hold a
# libusb-1.0 symlink: the libusb path came from another library by
# coincidence, and the one target that had no reason to name libbtbb did not
# build.
#
# A normal variable, as a find module's output is: it is derived from
# LIBUSB_INCLUDE_DIR every time the module runs, so a cache entry would only
# be another copy to go stale, and one a user could not override. Every
# directory that uses it calls find_package(USB1) of its own.
get_filename_component(_libusb_include_parent "${LIBUSB_INCLUDE_DIR}" DIRECTORY)
set(LIBUSB_INCLUDE_DIRS ${LIBUSB_INCLUDE_DIR} ${_libusb_include_parent})
unset(_libusb_include_parent)
