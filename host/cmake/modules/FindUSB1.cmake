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

  # in cache already. USB1_FOUND as well as LIBUSB_FOUND: USB1 is the name
  # find_package() is called with, and it is USB1_FOUND that a REQUIRED call
  # reads, so a configure after the first would otherwise find the answer in
  # the cache and still report nothing found.
  set(LIBUSB_FOUND TRUE)
  set(USB1_FOUND TRUE)

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
  # USB1, the name find_package() is called with: it is USB1_FOUND that
  # find_package(USB1 REQUIRED) reads, so naming anything else here leaves
  # REQUIRED with nothing to stop a configure on, and a tree with no libusb
  # gets as far as the compiler.
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(USB1 DEFAULT_MSG LIBUSB_LIBRARIES LIBUSB_INCLUDE_DIR)
  # The name this module has always reported, kept in step with the one
  # find_package() reads.
  set(LIBUSB_FOUND ${USB1_FOUND})

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
#
# Quoted, and only when libusb was found: an unquoted expansion would split a
# path holding a semicolon into entries that are not directories, and deriving
# the list from a failed search would hand a caller that did not ask for
# REQUIRED the literal LIBUSB_INCLUDE_DIR-NOTFOUND as an include directory.
if(LIBUSB_INCLUDE_DIR)
  get_filename_component(_libusb_include_parent "${LIBUSB_INCLUDE_DIR}" DIRECTORY)
  set(LIBUSB_INCLUDE_DIRS "${LIBUSB_INCLUDE_DIR}" "${_libusb_include_parent}")
  unset(_libusb_include_parent)
endif()
