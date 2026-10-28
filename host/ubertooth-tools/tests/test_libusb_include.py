#!/usr/bin/env python3
"""FindUSB1.cmake must report every directory a source needs for libusb.

The sources spell libusb's header both ways: the tests include <libusb.h>,
which resolves against the directory libusb.h sits in, and
ubertooth_control.h includes <libusb-1.0/libusb.h>, which resolves against the
directory above that. A Homebrew install keeps those apart, so a target given
only the first cannot compile the second -- which is what stopped a plain
macOS build of the ring buffer test. It built everywhere else because every
other target also passed libbtbb's include directory, and on Homebrew that is
the prefix include directory, which happens to hold a libusb-1.0 symlink.

This runs the real FindUSB1.cmake from a one-line project and, given nothing
but the directories it reports, compiles a source for each spelling. On a
Homebrew install the second one fails unless the module reports the parent
directory as well; where libusb sits under a directory the compiler searches
anyway, Linux for example, both compile either way, so the list itself is
checked too: it must name the directory holding libusb.h and that directory's
parent, and nothing it reports may be missing.

No include flags reach the project: CMAKE_C_FLAGS is set empty and CFLAGS is
dropped from the environment, because either one carrying
-I/opt/homebrew/include would hide exactly the gap this test is here for.

Usage: test_libusb_include.py <cmake> <repository root>
"""

import os
import subprocess
import sys
import tempfile

PROJECT = """\
cmake_minimum_required(VERSION 3.5...3.27)
project(libusb_include_test C)
set(CMAKE_MODULE_PATH "%s")
find_package(USB1 REQUIRED)
file(WRITE "${CMAKE_BINARY_DIR}/include-dirs.txt" "${LIBUSB_INCLUDE_DIRS}")
add_library(both_spellings OBJECT subdirectory.c plain.c)
target_include_directories(both_spellings PRIVATE ${LIBUSB_INCLUDE_DIRS})
"""

SOURCES = {
    # The spelling the installed ubertooth_control.h uses.
    "subdirectory.c": "#include <libusb-1.0/libusb.h>\nint subdirectory;\n",
    # The spelling libusb's own pkg-config file is written for.
    "plain.c": "#include <libusb.h>\nint plain;\n",
}


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def run(command, work):
    """Run a command with no include flags of its own, returning its output."""
    environment = dict(os.environ)
    environment.pop("CFLAGS", None)
    done = subprocess.run(command, cwd=work, env=environment,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True)
    return done.returncode, done.stdout


def main():
    if len(sys.argv) != 3:
        fail("usage: test_libusb_include.py <cmake> <repository root>")
    cmake, repo_root = sys.argv[1:]
    modules = os.path.join(repo_root, "host", "cmake", "modules")

    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "project")
        os.mkdir(source)
        with open(os.path.join(source, "CMakeLists.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write(PROJECT % modules)
        for name, text in SOURCES.items():
            with open(os.path.join(source, name), "w",
                      encoding="utf-8") as handle:
                handle.write(text)

        build = os.path.join(work, "build")
        status, output = run([cmake, "-S", source, "-B", build,
                              "-DCMAKE_C_FLAGS="], work)
        if status != 0:
            fail("the module did not find the libusb this build uses:\n%s"
                 % output)

        with open(os.path.join(build, "include-dirs.txt"),
                  encoding="utf-8") as handle:
            dirs = [entry for entry in handle.read().split(";") if entry]
        if not dirs:
            fail("FindUSB1.cmake reported no include directories")
        for entry in dirs:
            if not os.path.isdir(entry):
                fail("FindUSB1.cmake reports %s, which is not a directory"
                     % entry)
        header = [entry for entry in dirs
                  if os.path.exists(os.path.join(entry, "libusb.h"))]
        if not header:
            fail("none of the reported directories holds libusb.h: %s"
                 % ", ".join(dirs))
        parent = os.path.dirname(header[0])
        if parent not in dirs:
            fail("libusb.h is in %s, so <libusb-1.0/libusb.h> needs %s, which "
                 "is not reported: %s" % (header[0], parent, ", ".join(dirs)))

        status, output = run([cmake, "--build", build], work)
        if status != 0:
            fail("a source including libusb's header does not compile with "
                 "the directories FindUSB1.cmake reports (%s):\n%s"
                 % (", ".join(dirs), output))

    print("PASS")


if __name__ == "__main__":
    main()
