#!/usr/bin/env python3
"""A CMake project outside this tree finds and links the installed library.

Run as: test_cmake_package_install.py <cmake> <build dir> <install libdir>
            <install includedir> <source dir>

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
  * the library and headers it names are the ones the install really wrote,
    in this build's own layout and in each of the layouts other systems ask
    for. The same build is installed into a second prefix and the target
    follows it, because the file reads its prefix back from where it is
    itself;
  * it describes that install as libubertooth.pc beside it does. The two
    files are compared directory by directory, because a consumer that asks
    one of them and a consumer that asks the other have to be sent to the
    same place.

Nothing is installed outside the temporary prefixes this test makes, no device
is opened, and the program it builds is never run.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile


CMAKE, BUILD_DIR, LIBDIR, INCLUDEDIR, SOURCE_DIR = sys.argv[1:6]
# LIBDIR and INCLUDEDIR are the directories under the prefix this build
# installs the library and the headers to, passed in rather than assumed:
# GNUInstallDirs picks them for the system, and a build command can ask for
# others.

# The layouts installed here beyond this build's own: a name, the cache
# entries that ask for it, and the library and header directories under the
# prefix it is then installed to. They are taken in turn in one build
# directory, so each one's entries are set over the ones before and every
# entry a layout cares about is passed by that layout. Only the first of them
# pays for a configure from nothing, and the library is compiled once.
#
# The last two are the cases where a directory is named as it stands rather
# than relative to the prefix: an absolute library directory, which the
# install writes to wherever the prefix is, and an absolute header directory
# that happens to sit under the prefix the build was configured with, which
# the install writes to just the same.
LAYOUTS = (
    ("lib64", ["-ULIB_SUFFIX", "-DCMAKE_INSTALL_LIBDIR=lib64",
               "-DCMAKE_INSTALL_INCLUDEDIR=include"],
     "lib64", "include"),
    ("multiarch", ["-DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu",
                   "-DCMAKE_INSTALL_INCLUDEDIR=include/ubertooth"],
     "lib/x86_64-linux-gnu", "include/ubertooth"),
    ("lib-suffix", ["-DLIB_SUFFIX=64",
                    "-DCMAKE_INSTALL_INCLUDEDIR=include"],
     "lib64", "include"),
)

# What the install puts in each of these is what a CMake project building
# against the library needs, plus the pkg-config file the two are compared
# through. The rest of the build is left where it is.
COMPONENTS = ("sharedlibs", "staticlibs", "headers", "cmakeconfig",
              "pkgconfig")

# What the configures below take from this build rather than working out
# again: the libbtbb and libusb it found, how it compiles, and which
# generator and configuration it is. Without the last two a multi-config
# generator or a cross-build would configure and install something other than
# what this build holds.
FORWARDED_CACHE_PREFIXES = ("LIBBTBB_", "LIBUSB_")
FORWARDED_CACHE_ENTRIES = ("CMAKE_C_COMPILER", "CMAKE_C_FLAGS",
                           "CMAKE_PREFIX_PATH", "CMAKE_OSX_SYSROOT",
                           "CMAKE_OSX_DEPLOYMENT_TARGET", "CMAKE_BUILD_TYPE",
                           "CMAKE_GENERATOR", "CMAKE_TOOLCHAIN_FILE")

# The consumer project. Nothing of this tree is in it, and it is given no
# include path or library path of its own. The version is asked for as well,
# so the version file installed beside the config file is read.
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
                          timeout=600, **kwargs)


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
    """This build's dependencies, compiler and generator, to pass on."""
    arguments = []
    with open(os.path.join(BUILD_DIR, "CMakeCache.txt")) as cache:
        for line in cache:
            entry, separator, kind_and_value = line.partition(":")
            kind, kind_separator, value = kind_and_value.partition("=")
            if not separator or not kind_separator:
                continue
            # CMAKE_GENERATOR is an INTERNAL entry, and is the one of these
            # that has to be forwarded as -G rather than as a cache entry.
            if entry == "CMAKE_GENERATOR":
                arguments.append("-G" + value.rstrip())
                continue
            if kind == "INTERNAL":
                continue
            if entry in FORWARDED_CACHE_ENTRIES or \
                    entry.startswith(FORWARDED_CACHE_PREFIXES):
                arguments.append("-D%s:%s=%s" % (entry, kind, value.rstrip()))
    return arguments


def consume(name, work, prefix, forwarded, build=True, package_dir=None,
            binary=None):
    """Configure the consumer against that prefix; return what it found.

    CMAKE_PREFIX_PATH is the whole of what the consumer is told, so finding
    the package is part of what is being checked. package_dir sets
    ubertooth_DIR instead, for an install in a directory this system does not
    search -- whether CMake looks in <prefix>/lib64 at all is a property of
    the machine running the test, not something this change decides.

    binary reuses a consumer build directory, so that the compiler is
    detected once rather than once per install.
    """
    source = os.path.join(work, "consumer-src")
    if not os.path.isdir(source):
        os.makedirs(source)
        with open(os.path.join(source, "CMakeLists.txt"), "w") as f:
            f.write(CONSUMER_CMAKE)
        with open(os.path.join(source, "consumer.c"), "w") as f:
            f.write(CONSUMER_PROGRAM)

    report = os.path.join(work, name + "-target.txt")
    binary = binary or os.path.join(work, name + "-build")
    # CMAKE_PREFIX_PATH is forwarded from this build as well (a Homebrew
    # prefix, for one), so the install under test is put in front of it.
    prefix_path = [prefix] + [argument.partition("=")[2] for argument in
                              forwarded
                              if argument.startswith("-DCMAKE_PREFIX_PATH")]
    arguments = [argument for argument in forwarded
                 if not argument.startswith("-DCMAKE_PREFIX_PATH")]
    # Set either way, so that a reused build directory does not find the
    # previous install through a remembered ubertooth_DIR.
    arguments.append("-Dubertooth_DIR=" + (package_dir or ""))
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


def pkgconfig_directories(prefix, libdir):
    """The library and header directories libubertooth.pc names.

    Read rather than asked of pkg-config, which this test does not otherwise
    need: the file's three directory lines refer to each other and to
    pcfiledir, the directory the file is in, so resolving them is substituting
    two names.
    """
    pkgconfig_dir = os.path.join(prefix, libdir, "pkgconfig")
    path = os.path.join(pkgconfig_dir, "libubertooth.pc")
    assert os.path.isfile(path), path
    values = {}
    with open(path) as pc:
        for line in pc:
            match = re.match(r"(prefix|libdir|includedir)=(.*)", line.strip())
            if not match:
                continue
            name, value = match.groups()
            value = value.replace("${pcfiledir}", pkgconfig_dir)
            value = value.replace("${prefix}", values.get("prefix", ""))
            values[name] = value
    assert set(values) == {"prefix", "libdir", "includedir"}, (path, values)
    return os.path.realpath(values["libdir"]), \
        os.path.realpath(values["includedir"])


def check_install(name, work, prefix, libdir, includedir, forwarded,
                  build=True, package_dir=None, binary=None):
    """The target names the library and headers of that install."""
    location, includes = consume(name, work, prefix, forwarded, build,
                                 package_dir, binary)
    installed_libdir = os.path.realpath(
        libdir if os.path.isabs(libdir) else os.path.join(prefix, libdir))
    installed_includedir = os.path.realpath(
        includedir if os.path.isabs(includedir)
        else os.path.join(prefix, includedir))

    assert os.path.realpath(os.path.dirname(location)) == installed_libdir, \
        (name, location, installed_libdir)
    assert os.path.isfile(location), (name, location)
    resolved = [os.path.realpath(directory) for directory in includes]
    assert installed_includedir in resolved, (name, includedir, includes)

    # The same two directories libubertooth.pc names, so a consumer gets the
    # same answer whichever of the two files it asks.
    pc_libdir, pc_includedir = pkgconfig_directories(prefix, libdir)
    assert pc_libdir == installed_libdir, (name, pc_libdir, installed_libdir)
    assert pc_includedir == installed_includedir, \
        (name, pc_includedir, installed_includedir)


def build_library(build_dir, cache_entries, forwarded):
    """Configure a build of the library alone with those entries, and build it.

    The library is what the install under test holds, so the tools, the Python
    tools and the tests are left out: this is seconds, not a build of the
    tree.
    """
    configured = run([CMAKE, "-S", SOURCE_DIR, "-B", build_dir,
                      "-DENABLE_PYTHON=OFF", "-DBUILD_TESTING=OFF"]
                     + cache_entries + forwarded)
    assert configured.returncode == 0, (cache_entries, configured)
    built = run([CMAKE, "--build", build_dir, "--target", "ubertooth"])
    assert built.returncode == 0, (cache_entries, built)


work = tempfile.mkdtemp(prefix="libubertooth-cmake-package.")
try:
    forwarded = forwarded_cache_arguments()
    # One consumer build directory for every install after the first, so the
    # compiler is detected once.
    reused = os.path.join(work, "consumer-build")

    # This build's own library directory, installed into a prefix of its own
    # and found with nothing but CMAKE_PREFIX_PATH: the directory it installs
    # to is by definition one this system searches. An absolute library
    # directory is installed to exactly that wherever the prefix is, so such a
    # build cannot be put in a prefix of the test's own; the layouts below
    # cover the directories a system asks for either way.
    if os.path.isabs(LIBDIR):
        print("note: this build installs the library to %s, an absolute "
              "directory, so only the layouts below are installed" % LIBDIR)
    else:
        first = os.path.join(work, "own-first")
        install_into(first, BUILD_DIR)
        check_install("own-layout", work, first, LIBDIR, INCLUDEDIR,
                      forwarded)

        # The same build installed somewhere else is described as being
        # there: the config file reads its prefix back from where it is.
        second = os.path.join(work, "own-second")
        install_into(second, BUILD_DIR)
        check_install("relocated", work, second, LIBDIR, INCLUDEDIR,
                      forwarded, build=False, binary=reused)

    # The layouts other systems ask for, each configured and installed here.
    # The consumer is pointed straight at the config file, because a system
    # that does not keep its libraries in lib64 does not search <prefix>/lib64
    # for a package either, and what is checked here is the file the install
    # wrote.
    build_dir = os.path.join(work, "library-build")
    for name, cache_entries, libdir, includedir in LAYOUTS:
        build_library(build_dir, cache_entries, forwarded)
        prefix = os.path.join(work, name + "-prefix")
        install_into(prefix, build_dir)
        check_install(name, work, prefix, libdir, includedir, forwarded,
                      build=False, binary=reused,
                      package_dir=os.path.join(prefix, libdir, "cmake",
                                               "ubertooth"))

    # A library directory given as an absolute path, which the install writes
    # to wherever the prefix is, so the config file names it as it stands. The
    # prefix is given at configure time, because the headers go under it.
    absolute_libdir = os.path.join(work, "absolute-libdir")
    absolute_prefix = os.path.join(work, "absolute-prefix")
    build_library(build_dir,
                  ["-ULIB_SUFFIX",
                   "-DCMAKE_INSTALL_PREFIX=" + absolute_prefix,
                   "-DCMAKE_INSTALL_LIBDIR=" + absolute_libdir,
                   "-DCMAKE_INSTALL_INCLUDEDIR=include"], forwarded)
    install_into(absolute_prefix, build_dir)
    check_install("absolute-libdir", work, absolute_prefix, absolute_libdir,
                  "include", forwarded, build=False, binary=reused,
                  package_dir=os.path.join(absolute_libdir, "cmake",
                                           "ubertooth"))

    # An absolute header directory that sits under the prefix the build was
    # configured with, and then installed with a different prefix. The
    # install writes the headers to exactly that absolute directory and the
    # library under the new prefix, so the config file has to name the header
    # directory as it stands and the library directory relative to itself.
    #
    # Naming the header directory relative to the prefix would be right for
    # the install made with the prefix the build was configured with and
    # wrong for this one, which is why the two are installed to different
    # prefixes here.
    configured_prefix = os.path.join(work, "absolute-includedir-configured")
    absolute_includedir = os.path.join(configured_prefix, "include")
    moved_prefix = os.path.join(work, "absolute-includedir-moved")
    build_library(build_dir,
                  ["-DCMAKE_INSTALL_PREFIX=" + configured_prefix,
                   "-DCMAKE_INSTALL_LIBDIR=lib",
                   "-DCMAKE_INSTALL_INCLUDEDIR=" + absolute_includedir],
                  forwarded)
    install_into(moved_prefix, build_dir)
    check_install("absolute-includedir", work, moved_prefix, "lib",
                  absolute_includedir, forwarded, build=False, binary=reused,
                  package_dir=os.path.join(moved_prefix, "lib", "cmake",
                                           "ubertooth"))
finally:
    shutil.rmtree(work, ignore_errors=True)

print("ubertoothConfig.cmake: a CMake project outside the tree finds the "
      "installed library with find_package(ubertooth) and links the imported "
      "target, in whichever prefix and whichever library directory the "
      "library is installed, and is sent to the same directories "
      "libubertooth.pc names")
