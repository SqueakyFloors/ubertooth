#!/usr/bin/env python3
"""A program outside this tree builds against the installed libubertooth.

Run as: test_pkgconfig_install.py <cmake> <build dir> <pkg-config>
            <C compiler> <install libdir> <source dir>

The install writes libubertooth.pc, so the flags a program needs come from
`pkg-config --cflags --libs libubertooth` rather than being worked out by hand.
Three things have to hold for that to be true, and each is checked here:

  * the file is installed, and pkg-config can resolve it: its Requires names
    libbtbb and libusb-1.0, which the installed headers include;
  * the flags it gives are enough on their own. A program that includes
    <ubertooth.h> -- which reaches <btbb.h> and <libusb-1.0/libusb.h> -- is
    compiled and linked with those flags and nothing else;
  * the directories it names are the ones the install really wrote to. The
    same build is installed into a second prefix, and the flags follow it:
    libubertooth.pc reads its prefix back from where the file itself is, so an
    install staged or relocated somewhere else is still described correctly.

All three are checked for this build's own library directory, and then for the
layouts other systems ask for, each configured and built here: lib64, which a
64-bit Fedora, RHEL or SUSE keeps its libraries in; lib/x86_64-linux-gnu, the
Debian multiarch layout, which is two directories deep and so needs a longer
walk back out of libubertooth.pc, with the headers in a directory of their own
as well; and LIB_SUFFIX, the older setting, which still decides the directory
where a build command passes it.

Nothing is installed outside the temporary prefixes this test makes, no device
is opened, and the programs it builds are never run.
"""

import os
import shutil
import subprocess
import sys
import tempfile


CMAKE, BUILD_DIR, PKG_CONFIG, CC, LIBDIR, SOURCE_DIR = sys.argv[1:7]
# LIBDIR is the directory under the prefix this build installs the library and
# its pkg-config file to, passed in rather than assumed: GNUInstallDirs picks
# it for the system, and a build command can ask for another one.

# The layouts built here: a name, the cache entries that ask for it, and the
# library and header directories under the prefix it is then installed to.
# They are taken in turn in one build directory, so each one's entries are set
# over the ones before: the last of them has the multiarch
# CMAKE_INSTALL_LIBDIR of the one before it as well as its own LIB_SUFFIX, and
# LIB_SUFFIX decides, which is the rule where a build command carries both.
# Every entry a layout cares about is passed by that layout, rather than left
# to the one before.
LAYOUTS = (
    ("lib64", ["-DCMAKE_INSTALL_LIBDIR=lib64",
               "-DCMAKE_INSTALL_INCLUDEDIR=include"],
     "lib64", "include"),
    ("multiarch", ["-DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu",
                   "-DCMAKE_INSTALL_INCLUDEDIR=include/ubertooth"],
     "lib/x86_64-linux-gnu", "include/ubertooth"),
    ("lib-suffix", ["-DLIB_SUFFIX=64",
                    "-DCMAKE_INSTALL_INCLUDEDIR=include"],
     "lib64", "include"),
)

# What the configures below take from this build rather than searching for
# again: the libbtbb and libusb it found, and how it compiles. So they test
# the layouts against the same dependencies, and they do not fail where the
# build was pointed at a libbtbb that only it knows about.
FORWARDED_CACHE_PREFIXES = ("LIBBTBB_", "LIBUSB_")
FORWARDED_CACHE_ENTRIES = ("CMAKE_C_COMPILER", "CMAKE_C_FLAGS",
                           "CMAKE_PREFIX_PATH", "CMAKE_OSX_SYSROOT",
                           "CMAKE_OSX_DEPLOYMENT_TARGET")

# What the install puts in each of these is what a program building against
# the library needs: the shared or static library, the headers, and the
# pkg-config file that describes them. The rest of the build (the tools, the
# man pages, the Python modules) is left where it is.
COMPONENTS = ("sharedlibs", "staticlibs", "headers", "pkgconfig")

# The include <ubertooth.h> is what makes this a real check rather than a link
# test: it pulls in libbtbb's header and libusb's, each spelled the way the
# installed headers spell it. The address is kept in a volatile pointer so the
# compiler cannot drop the reference, which is what makes the linker resolve
# the symbol in libubertooth itself.
PROGRAM = """#include <ubertooth.h>

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
                          timeout=120, **kwargs)


def pkg_config(arguments, path):
    """Ask pkg-config, with PKG_CONFIG_PATH holding the test's prefix first."""
    environment = dict(os.environ)
    existing = environment.get("PKG_CONFIG_PATH")
    environment["PKG_CONFIG_PATH"] = \
        path + ((os.pathsep + existing) if existing else "")
    return run([PKG_CONFIG, "--print-errors"] + arguments, env=environment)


def install_into(prefix, build_dir=None):
    """Install the library, its headers and its .pc file into prefix.

    cmake_install.cmake rather than `cmake --install`: the project supports
    CMake 3.5, where that option does not exist yet.
    """
    for component in COMPONENTS:
        result = run([CMAKE,
                      "-DCMAKE_INSTALL_PREFIX=" + prefix,
                      "-DCMAKE_INSTALL_COMPONENT=" + component,
                      "-P", os.path.join(build_dir or BUILD_DIR,
                                         "cmake_install.cmake")])
        # A component this build has nothing in (staticlibs, where only the
        # shared library is built) installs no files and succeeds.
        assert result.returncode == 0, (component, result)


def flags_for(prefix, libdir=None):
    """The compile and link flags pkg-config gives for that install."""
    pkgconfig_dir = os.path.join(prefix, libdir or LIBDIR, "pkgconfig")
    assert os.path.isfile(os.path.join(pkgconfig_dir, "libubertooth.pc")), \
        "the install wrote no libubertooth.pc in " + pkgconfig_dir

    found = pkg_config(["--modversion", "libubertooth"], pkgconfig_dir)
    assert found.returncode == 0, found
    assert found.stdout.strip(), found

    cflags = pkg_config(["--cflags", "libubertooth"], pkgconfig_dir)
    assert cflags.returncode == 0, cflags
    libs = pkg_config(["--libs", "libubertooth"], pkgconfig_dir)
    assert libs.returncode == 0, libs
    return cflags.stdout.split(), libs.stdout.split()


def directories_in(flags, option):
    """The directories the -I or -L flags name, as the filesystem sees them.

    libubertooth.pc names them relative to its own location, so the paths
    arrive with a "pkgconfig/../.." in them and are compared resolved.
    """
    return [os.path.realpath(flag[len(option):])
            for flag in flags if flag.startswith(option)]


# libubertooth.pc names libbtbb and libusb-1.0 in its Requires, because the
# headers installed beside it include theirs. Without those two files
# pkg-config cannot answer for libubertooth at all, and that is about this
# machine rather than about the install, so it is reported and skipped: a
# libbtbb found by CMake's own search rather than through pkg-config is enough
# to build this tree.
for module in ("libbtbb", "libusb-1.0"):
    probe = pkg_config(["--exists", module], "")
    if probe.returncode != 0:
        print("skipped: this machine has no %s.pc, which libubertooth.pc "
              "names in its Requires" % module)
        sys.exit(0)

def check_install(name, work, build_dir, libdir, includedir="include"):
    """That build, installed into two prefixes, described correctly in both."""
    first = os.path.join(work, name + "-first")
    install_into(first, build_dir)
    cflags, libs = flags_for(first, libdir)

    # The install's own directories, named by the flags a consumer is given.
    assert os.path.realpath(os.path.join(first, includedir)) in \
        directories_in(cflags, "-I"), (name, first, cflags)
    assert os.path.realpath(os.path.join(first, libdir)) in \
        directories_in(libs, "-L"), (name, first, libs)
    assert "-lubertooth" in libs, (name, libs)

    # The flags on their own, with nothing of this build directory and no
    # include path of the test's own: what a program outside the tree has.
    source = os.path.join(work, name + ".c")
    with open(source, "w") as program:
        program.write(PROGRAM)
    built = run([CC] + cflags + [source, "-o", os.path.join(work, name)] +
                libs)
    assert built.returncode == 0, (name, built)

    # The same build installed somewhere else is described as being there.
    second = os.path.join(work, name + "-second")
    install_into(second, build_dir)
    moved_cflags, moved_libs = flags_for(second, libdir)
    assert os.path.realpath(os.path.join(second, includedir)) in \
        directories_in(moved_cflags, "-I"), (name, second, moved_cflags)
    assert os.path.realpath(os.path.join(second, libdir)) in \
        directories_in(moved_libs, "-L"), (name, second, moved_libs)
    assert os.path.realpath(os.path.join(first, includedir)) not in \
        directories_in(moved_cflags, "-I"), (name, first, second, moved_cflags)


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


def build_library(build_dir, cache_entries, forwarded):
    """Configure a build of the library alone with those entries, and build it.

    The library is what the install under test holds, so the tools, the Python
    tools and the tests are left out: this is several seconds, not a build of
    the tree. One build directory takes the layouts in turn, so only the first
    of them pays for a configure from nothing and the library is compiled once.
    """
    configured = run([CMAKE, "-S", SOURCE_DIR, "-B", build_dir,
                      "-DENABLE_PYTHON=OFF", "-DBUILD_TESTING=OFF"]
                     + cache_entries + forwarded)
    assert configured.returncode == 0, (cache_entries, configured)
    built = run([CMAKE, "--build", build_dir, "--target", "ubertooth"])
    assert built.returncode == 0, (cache_entries, built)


work = tempfile.mkdtemp(prefix="libubertooth-pkgconfig.")
try:
    check_install("own-layout", work, BUILD_DIR, LIBDIR)

    forwarded = forwarded_cache_arguments()
    build_dir = os.path.join(work, "build")
    for name, cache_entries, libdir, includedir in LAYOUTS:
        build_library(build_dir, cache_entries, forwarded)
        check_install(name, work, build_dir, libdir, includedir)
finally:
    shutil.rmtree(work, ignore_errors=True)

print("libubertooth.pc: a program outside the tree compiles and links with "
      "the flags pkg-config gives, in whichever prefix and whichever library "
      "directory the library is installed")
