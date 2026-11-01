#!/usr/bin/env python3
"""A program outside this tree builds against the installed libubertooth.

Run as: test_pkgconfig_install.py <cmake> <build dir> <pkg-config>
            <C compiler> [LIB_SUFFIX]

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

Nothing is installed outside the temporary prefixes this test makes, no device
is opened, and the program it builds is never run.
"""

import os
import shutil
import subprocess
import sys
import tempfile


CMAKE, BUILD_DIR, PKG_CONFIG, CC = sys.argv[1:5]
# Which directory under the prefix the library and its pkg-config file are
# installed to: "lib", or lib plus whatever LIB_SUFFIX the build was given
# (lib64 on the systems that keep 64-bit libraries there). Passed in rather
# than assumed, so a build configured with -DLIB_SUFFIX=64 is looked for where
# it really put things.
LIBDIR = "lib" + (sys.argv[5] if len(sys.argv) > 5 else "")

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


def install_into(prefix):
    """Install the library, its headers and its .pc file into prefix.

    cmake_install.cmake rather than `cmake --install`: the project supports
    CMake 3.5, where that option does not exist yet.
    """
    for component in COMPONENTS:
        result = run([CMAKE,
                      "-DCMAKE_INSTALL_PREFIX=" + prefix,
                      "-DCMAKE_INSTALL_COMPONENT=" + component,
                      "-P", os.path.join(BUILD_DIR, "cmake_install.cmake")])
        # A component this build has nothing in (staticlibs, where only the
        # shared library is built) installs no files and succeeds.
        assert result.returncode == 0, (component, result)


def flags_for(prefix):
    """The compile and link flags pkg-config gives for that install."""
    pkgconfig_dir = os.path.join(prefix, LIBDIR, "pkgconfig")
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

work = tempfile.mkdtemp(prefix="libubertooth-pkgconfig.")
try:
    first = os.path.join(work, "first")
    install_into(first)
    cflags, libs = flags_for(first)

    # The install's own directories, named by the flags a consumer is given.
    assert os.path.realpath(os.path.join(first, "include")) in \
        directories_in(cflags, "-I"), (first, cflags)
    assert os.path.realpath(os.path.join(first, LIBDIR)) in \
        directories_in(libs, "-L"), (first, libs)
    assert "-lubertooth" in libs, libs

    # The flags on their own, with nothing of this build directory and no
    # include path of the test's own: what a program outside the tree has.
    source = os.path.join(work, "probe.c")
    with open(source, "w") as program:
        program.write(PROGRAM)
    built = run([CC] + cflags + [source, "-o", os.path.join(work, "probe")] +
                libs)
    assert built.returncode == 0, built

    # The same build installed somewhere else is described as being there.
    second = os.path.join(work, "second")
    install_into(second)
    moved_cflags, moved_libs = flags_for(second)
    assert os.path.realpath(os.path.join(second, "include")) in \
        directories_in(moved_cflags, "-I"), (second, moved_cflags)
    assert os.path.realpath(os.path.join(second, LIBDIR)) in \
        directories_in(moved_libs, "-L"), (second, moved_libs)
    assert os.path.realpath(os.path.join(first, "include")) not in \
        directories_in(moved_cflags, "-I"), (first, second, moved_cflags)
finally:
    shutil.rmtree(work, ignore_errors=True)

print("libubertooth.pc: a program outside the tree compiles and links with "
      "the flags pkg-config gives, in whichever prefix the library is "
      "installed")
