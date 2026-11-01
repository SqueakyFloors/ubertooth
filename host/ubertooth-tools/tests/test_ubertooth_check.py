#!/usr/bin/env python3
"""The configure step must refuse a libubertooth older than these tools.

Run as: test_ubertooth_check.py <cmake> <repository root>

libubertooth reports no version a build can read, so FindUBERTOOTH.cmake asks
the installed one for a few of the functions the tools call. Without that
check, an install from an older commit of this tree configured without a word
and then failed in the compiler with dozens of errors naming symbols its
header has not got, mentioning neither libubertooth nor what to do.

This runs the real FindUBERTOOTH.cmake from a one-line project against three
installs built here, so the refusal is pinned rather than left to a developer
to run into:

  * a complete stand-in, where it must configure and must have run the probe,
    so a check that stopped checking fails here;
  * the same with one required function missing from the header, which is what
    an older libubertooth looks like, where it must fail naming that function
    and how to install a current one;
  * a complete header over a library that defines none of them, at -O2 and
    again at -O2 -flto, where it must also fail. That case is the library half
    of the probe rather than the header half, and optimization is what can
    erase it: a table of function addresses can be folded away or dropped,
    leaving a program that links against anything.

The stand-ins come from libubertooth_standin.py, which reads the names out of
the module, so this test cannot drift from the list the module checks.
"""

import os
import subprocess
import sys
import tempfile

import libubertooth_standin as standin

# The project each case configures: nothing but the find_package, with the
# answer written out so a case can tell "found and accepted" from "not found".
PROJECT = """\
cmake_minimum_required(VERSION 3.5...3.27)
project(ubertooth_check_test C)
set(CMAKE_MODULE_PATH [[%(modules)s]])
find_package(UBERTOOTH REQUIRED)
file(WRITE "${CMAKE_BINARY_DIR}/found.txt" "${LIBUBERTOOTH_FOUND}")
"""

# The probe's result variable, so a configure that no longer runs it is told
# apart from one that ran it and passed.
PROBE = "LIBUBERTOOTH_HAS_REQUIRED_FUNCTIONS"

# What the refusal has to offer whichever way the probe failed.
FIX = "Build and install libubertooth from this tree"


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def configure(cmake, source, build, include_dir, library, c_flags=None):
    """Configure the test project, returning (status, output on one line)."""
    command = [cmake, "-S", source, "-B", build,
               "-DLIBUBERTOOTH_INCLUDE_DIR=%s" % include_dir,
               "-DLIBUBERTOOTH_LIBRARIES=%s" % library]
    if c_flags is not None:
        command.append("-DCMAKE_C_FLAGS=%s" % c_flags)
    done = subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    # CMake wraps and indents a message(FATAL_ERROR), so assertions on its
    # wording compare against one line.
    return done.returncode, " ".join(done.stdout.split())


def main():
    if len(sys.argv) != 3:
        fail("usage: test_ubertooth_check.py <cmake> <repository root>")
    cmake, repo_root = sys.argv[1:]
    modules = os.path.join(repo_root, "host", "cmake", "modules")
    names = standin.required_functions(modules)

    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "project")
        os.mkdir(source)
        with open(os.path.join(source, "CMakeLists.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write(PROJECT % {"modules": modules})

        # A libubertooth with everything the module asks for.
        include, library = standin.build(cmake, work, "complete", names, names)
        build = os.path.join(work, "case-complete")
        status, output = configure(cmake, source, build, include, library)
        if status != 0:
            fail("the configure refused a libubertooth that has every "
                 "function the module asks for:\n%s" % output)
        if "%s - Success" % PROBE not in output:
            fail("the configure accepted the complete stand-in without "
                 "running the probe, so nothing here is checked:\n%s" % output)
        with open(os.path.join(build, "found.txt"), encoding="utf-8") as f:
            if f.read().strip() != "TRUE":
                fail("the module did not report the complete stand-in found")

        # An older libubertooth: one function gone from the header. The last
        # of the list, which is the newest of them, so the case is the one a
        # developer meets.
        missing = names[-1]
        kept = [name for name in names if name != missing]
        include, library = standin.build(cmake, work, "older", kept, kept)
        status, output = configure(cmake, source,
                                   os.path.join(work, "case-older"),
                                   include, library)
        if status == 0:
            fail("the configure accepted a libubertooth whose header has no "
                 "%s, so the build would have failed in the compiler instead"
                 % missing)
        if missing not in output:
            fail("the refusal does not name %s, which is what is missing:\n%s"
                 % (missing, output))
        for name in kept:
            if name in output:
                fail("the refusal names %s, which the stand-in has: it has to "
                     "name what is missing and nothing else:\n%s"
                     % (name, output))
        if FIX not in output:
            fail("the refusal does not say how to install a libubertooth that "
                 "has it:\n%s" % output)

        # The library half: the header declares all of them and the library
        # defines none. Under optimization too, which is what can erase the
        # addresses the probe links.
        include, library = standin.build(cmake, work, "no-symbols", names, [])
        for flags in (None, "-O2", "-O2 -flto"):
            tag = (flags or "default").replace(" ", "").replace("-", "")
            status, output = configure(
                cmake, source, os.path.join(work, "case-no-symbols-%s" % tag),
                include, library, flags)
            if status == 0:
                fail("the configure accepted a libubertooth that defines "
                     "none of the functions it declares%s, so the build "
                     "would have failed in the linker instead"
                     % ("" if flags is None else " at %s" % flags))
            if FIX not in output:
                fail("the refusal of a library without the symbols does not "
                     "say how to install one that has them:\n%s" % output)

    print("PASS")


if __name__ == "__main__":
    main()
