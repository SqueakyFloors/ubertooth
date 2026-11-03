#!/usr/bin/env python3
"""A CMake project outside this tree finds and links the installed library.

Run as: test_cmake_package_install.py <cmake> <build dir> <install libdir>
            <source dir>

The install writes ubertoothConfig.cmake, so a project of its own says
`find_package(ubertooth REQUIRED)` and links `ubertooth::ubertooth` rather
than being told where the library is. Four things have to hold, and each is
checked here:

  * the file is installed where CMake looks for a package of that name, so
    nothing but CMAKE_PREFIX_PATH is needed to find it;
  * the imported target it defines is enough on its own. A program that
    includes <ubertooth.h> -- which reaches <btbb.h> and
    <libusb-1.0/libusb.h> -- is compiled and linked against that target and
    nothing else;
  * the library and headers it names are the ones the install really wrote.
    The same build is installed into a second prefix and the target follows
    it, because the file reads its prefix back from where it is itself;
  * a build that installs to lib64, the library directory a 64-bit Fedora,
    RHEL or SUSE uses, writes a config file that describes that install too.
    The consumer is pointed straight at it with ubertooth_DIR there, because
    whether CMake searches <prefix>/lib64 at all is a property of the system
    running the test -- it does on a system that keeps libraries in lib64 and
    not on Debian or Ubuntu -- and that is not what this change decides.

Nothing is installed outside the temporary prefixes this test makes, no device
is opened, and the program it builds is never run.
"""

import os
import shutil
import subprocess
import sys
import tempfile


CMAKE, BUILD_DIR, LIBDIR, SOURCE_DIR = sys.argv[1:5]
# LIBDIR is the directory under the prefix this build installs the library to,
# passed in rather than assumed: GNUInstallDirs picks it for the system, and a
# build command can ask for another one.

# What the install puts in each of these is what a CMake project building
# against the library needs: the library, the headers, and the package config
# file that describes them. The rest of the build is left where it is.
COMPONENTS = ("sharedlibs", "staticlibs", "headers", "cmakeconfig")

# What the configure below takes from this build rather than searching for
# again: the libbtbb and libusb it found, and how it compiles.
FORWARDED_CACHE_PREFIXES = ("LIBBTBB_", "LIBUSB_")
FORWARDED_CACHE_ENTRIES = ("CMAKE_C_COMPILER", "CMAKE_C_FLAGS",
                           "CMAKE_PREFIX_PATH", "CMAKE_OSX_SYSROOT",
                           "CMAKE_OSX_DEPLOYMENT_TARGET")

# The consumer project: nothing of this tree in it, and no include path or
# library path of its own. The version is asked for as well, so the version
# file installed beside the config file is read.
#
# The target's location and include directories are written out, because the
# point is not only that the program builds but that it was built against the
# install under test: a libubertooth already on this machine would satisfy a
# link that resolved -lubertooth by luck.
CONSUMER_CMAKE = """cmake_minimum_required(VERSION 3.5...3.27)
project(ubertooth-consumer C)
find_package(ubertooth 1.0 REQUIRED)
get_target_property(location ubertooth::ubertooth IMPORTED_LOCATION)
get_target_property(includes ubertooth::ubertooth
\tINTERFACE_INCLUDE_DIRECTORIES)
file(WRITE "${REPORT}" "${location}\\n${includes}\\n")
add_executable(consumer consumer.c)
target_link_libraries(consumer PRIVATE ubertooth::ubertooth)
"""

# The include <ubertooth.h> is what makes this a real check rather than a link
# test: it pulls in libbtbb's header and libusb's, each spelled the way the
# installed headers spell it. The address is kept in a volatile pointer so the
# compiler cannot drop the reference, which is what makes the linker resolve
# the symbol in libubertooth itself.
CONSUMER_PROGRAM = """#include <ubertooth.h>

int main(void)
{
\tubertooth_t *(*volatile start)(int) = ubertooth_start;
\t(void)start;
\treturn 0;
}
"""


def run(command, **kwargs):
    return subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True,
                          timeout=300, **kwargs)


def install_into(prefix, build_dir):
    """Install the library, its headers and its config file into prefix.

    cmake_install.cmake rather than `cmake --install`: the project supports
    CMake 3.5, where that option does not exist yet.
    """
    for component in COMPONENTS:
        result = run([CMAKE,
                      "-DCMAKE_INSTALL_PREFIX=" + prefix,
                      "-DCMAKE_INSTALL_COMPONENT=" + component,
                      "-P", os.path.join(build_dir, "cmake_install.cmake")])
        # A component this build has nothing in (staticlibs, where only the
        # shared library is built) installs no files and succeeds.
        assert result.returncode == 0, (component, result)


def forwarded_cache_arguments():
    """This build's dependencies and compiler, as cache entries to pass on."""
    arguments = []
    with open(os.path.join(BUILD_DIR, "CMakeCache.txt")) as cache:
        for line in cache:
            entry, separator, kind_and_value = line.partition(":")
            kind, kind_separator, value = kind_and_value.partition("=")
            if not separator or not kind_separator or kind == "INTERNAL":
                continue
            if entry in FORWARDED_CACHE_ENTRIES or \
                    entry.startswith(FORWARDED_CACHE_PREFIXES):
                arguments.append("-D%s:%s=%s" % (entry, kind, value.rstrip()))
    return arguments


def consume(name, work, prefix, forwarded, build=True, package_dir=None):
    """Configure the consumer against that prefix; return what it found.

    CMAKE_PREFIX_PATH is the whole of what the consumer is told, so finding
    the package is part of what is being checked. The compiler is forwarded
    along with it, because the program is linked against a library this build
    compiled.

    package_dir sets ubertooth_DIR instead, for an install in a directory
    this system does not search.
    """
    source = os.path.join(work, name + "-src")
    os.makedirs(source)
    with open(os.path.join(source, "CMakeLists.txt"), "w") as f:
        f.write(CONSUMER_CMAKE)
    with open(os.path.join(source, "consumer.c"), "w") as f:
        f.write(CONSUMER_PROGRAM)

    report = os.path.join(work, name + "-target.txt")
    binary = os.path.join(work, name + "-build")
    # CMAKE_PREFIX_PATH is forwarded from this build as well (a Homebrew
    # prefix, for one), so the install under test is put in front of it.
    prefix_path = [prefix] + [argument.partition("=")[2] for argument in
                              forwarded
                              if argument.startswith("-DCMAKE_PREFIX_PATH")]
    arguments = [argument for argument in forwarded
                 if not argument.startswith("-DCMAKE_PREFIX_PATH")]
    if package_dir:
        arguments.append("-Dubertooth_DIR=" + package_dir)
    configured = run([CMAKE, "-S", source, "-B", binary,
                      "-DCMAKE_PREFIX_PATH=" + ";".join(prefix_path),
                      "-DREPORT=" + report] + arguments)
    assert configured.returncode == 0, (name, configured)
    assert os.path.isfile(report), (name, configured)
    with open(report) as f:
        location, includes = f.read().splitlines()[:2]

    if build:
        built = run([CMAKE, "--build", binary])
        assert built.returncode == 0, (name, built)

    return location, includes.split(";")


def check_install(name, work, prefix, libdir, forwarded, build=True,
                  package_dir=None):
    """The target names the library and headers of that install."""
    location, includes = consume(name, work, prefix, forwarded, build,
                                 package_dir)
    installed_libdir = os.path.realpath(os.path.join(prefix, libdir))
    assert os.path.realpath(os.path.dirname(location)) == installed_libdir, \
        (name, location, installed_libdir)
    assert os.path.isfile(location), (name, location)
    resolved = [os.path.realpath(directory) for directory in includes]
    assert os.path.realpath(os.path.join(prefix, "include")) in resolved, \
        (name, prefix, includes)


work = tempfile.mkdtemp(prefix="libubertooth-cmake-package.")
try:
    forwarded = forwarded_cache_arguments()

    # This build's own library directory, installed into a prefix of its own.
    # An absolute library directory is installed to exactly that, wherever the
    # prefix is, so such a build cannot be put in a prefix of the test's own;
    # the lib64 layout below covers the directory computation either way.
    if os.path.isabs(LIBDIR):
        print("note: this build installs the library to %s, an absolute "
              "directory, so only the lib64 layout below is installed"
              % LIBDIR)
    else:
        first = os.path.join(work, "own-first")
        install_into(first, BUILD_DIR)
        check_install("own-layout", work, first, LIBDIR, forwarded)

        # The same build installed somewhere else is described as being
        # there: the config file reads its prefix back from where it is.
        second = os.path.join(work, "own-second")
        install_into(second, BUILD_DIR)
        check_install("relocated", work, second, LIBDIR, forwarded,
                      build=False)

    # lib64, which a 64-bit Fedora, RHEL or SUSE keeps its libraries in. The
    # library alone is built, so this is seconds rather than a build of the
    # tree. The consumer is pointed straight at the config file: a system that
    # does not keep libraries in lib64 does not search <prefix>/lib64 for a
    # package either, and what is being checked here is the file the install
    # wrote, not which directories this machine's CMake looks in.
    lib64_build = os.path.join(work, "lib64-library")
    configured = run([CMAKE, "-S", SOURCE_DIR, "-B", lib64_build,
                      "-DENABLE_PYTHON=OFF", "-DBUILD_TESTING=OFF",
                      "-DCMAKE_INSTALL_LIBDIR=lib64",
                      "-DCMAKE_INSTALL_INCLUDEDIR=include"] + forwarded)
    assert configured.returncode == 0, configured
    built = run([CMAKE, "--build", lib64_build, "--target", "ubertooth"])
    assert built.returncode == 0, built
    lib64_prefix = os.path.join(work, "lib64-prefix")
    install_into(lib64_prefix, lib64_build)
    check_install("lib64", work, lib64_prefix, "lib64", forwarded,
                  package_dir=os.path.join(lib64_prefix, "lib64", "cmake",
                                           "ubertooth"))
finally:
    shutil.rmtree(work, ignore_errors=True)

print("ubertoothConfig.cmake: a CMake project outside the tree finds the "
      "installed library with find_package(ubertooth) and links the imported "
      "target, in whichever prefix and whichever library directory the "
      "library is installed")
