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
    so a check that stopped checking fails here, and at -O2 and -O2 -flto as
    well, because a probe that the compiler can erase is as wrong as one that
    refuses what it should accept;
  * the same with one required function missing from the header, which is what
    an older libubertooth looks like, where it must fail naming that function
    and how to install a current one;
  * a complete header over a library that defines none of them, where it must
    fail saying so rather than calling the headers old, at the same three sets
    of flags. That case is the library half of the probe, and optimization is
    what can erase it: a table of function addresses can be folded away or
    dropped, leaving a program that links against anything.

The names themselves are checked against this tree as well: each one has to
be declared by libubertooth here and called by a tool, or the module would be
refusing current installs or asking for something nothing needs.

The stand-ins come from libubertooth_standin.py, which reads the names out of
the module, so this test cannot drift from the list the module checks. They
include no other header, so what the module does to compile against the real
ubertooth.h, which includes btbb.h and libusb.h, is exercised by the
host-installed-libubertooth CI job rather than here.
"""

import glob
import os
import re
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

# The module's two refusals, which say different things to the reader: the
# headers do not declare a function, or they declare them all and the program
# still does not build.
OLDER_HALF = "is older than these tools"
LIBRARY_HALF = "does not build"

# The flags each case is run at. A probe that links function addresses can be
# erased by optimization, which would leave it accepting anything, and a probe
# written against that can refuse what it should accept, so both directions
# are run at all three.
FLAGS = (None, "-O2", "-O2 -flto")


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


def flag_tag(flags):
    """A build directory name for a set of flags."""
    return (flags or "default").replace(" ", "").replace("-", "")


def at_flags(flags):
    """The flags, for a message, or nothing where there are none."""
    return "" if flags is None else " at %s" % flags


def check_names_are_real(repo_root, names):
    """Each name the module asks for is declared here and called by a tool.

    The module decides "new enough" by these four names, so a name that
    libubertooth does not have would refuse every install, current ones
    included, and a name no tool calls would be asking an installed library
    for something nothing needs. Both are checked against this tree, which is
    where the names come from.
    """
    library = os.path.join(repo_root, "host", "libubertooth", "src")
    declarations = ""
    for path in sorted(glob.glob(os.path.join(library, "ubertooth*.h"))):
        with open(path, encoding="utf-8", errors="replace") as handle:
            declarations += handle.read()
    if not declarations:
        fail("no libubertooth headers under %s" % library)
    calls = ""
    for path in sorted(glob.glob(os.path.join(repo_root, "host",
                                              "ubertooth-tools", "src",
                                              "*.c"))):
        with open(path, encoding="utf-8", errors="replace") as handle:
            calls += handle.read()
    if not calls:
        fail("no tool sources under %s" % repo_root)
    for name in names:
        if not re.search(r"\b%s\b" % re.escape(name), declarations):
            fail("the module asks an installed libubertooth for %s, which "
                 "libubertooth in this tree does not declare, so it would "
                 "refuse a current install too" % name)
        if not re.search(r"\b%s\s*\(" % re.escape(name), calls):
            fail("the module asks an installed libubertooth for %s, which no "
                 "tool calls; it is there to mark what the tools need" % name)


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

        check_names_are_real(repo_root, names)

        # A libubertooth with everything the module asks for, at the flags a
        # user's CFLAGS can bring: the probe keeps the addresses it links
        # through a volatile call, and a regression in that would show up as
        # a complete libubertooth refused under optimization rather than as
        # anything wrong here.
        include, library = standin.build(cmake, work, "complete", names, names)
        for flags in FLAGS:
            tag = flag_tag(flags)
            build = os.path.join(work, "case-complete-%s" % tag)
            status, output = configure(cmake, source, build, include, library,
                                       flags)
            if status != 0:
                fail("the configure refused a libubertooth that has every "
                     "function the module asks for%s:\n%s"
                     % (at_flags(flags), output))
            if "%s - Success" % PROBE not in output:
                fail("the configure accepted the complete stand-in%s without "
                     "running the probe, so nothing here is checked:\n%s"
                     % (at_flags(flags), output))
            with open(os.path.join(build, "found.txt"),
                      encoding="utf-8") as handle:
                if handle.read().strip() != "TRUE":
                    fail("the module did not report the complete stand-in "
                         "found%s" % at_flags(flags))

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
        for flags in FLAGS:
            status, output = configure(
                cmake, source,
                os.path.join(work, "case-no-symbols-%s" % flag_tag(flags)),
                include, library, flags)
            if status == 0:
                fail("the configure accepted a libubertooth that defines "
                     "none of the functions it declares%s, so the build "
                     "would have failed in the linker instead"
                     % at_flags(flags))
            if FIX not in output:
                fail("the refusal of a library without the symbols does not "
                     "say how to install one that has them:\n%s" % output)
            # The refusal for the library half, not the one for an older
            # header: the headers here declare every function, and saying
            # they do not would send the reader after the wrong thing.
            if LIBRARY_HALF not in output:
                fail("the refusal%s does not say that the headers have "
                     "everything and the program still does not build, which "
                     "is what is wrong:\n%s" % (at_flags(flags), output))
            if OLDER_HALF in output:
                fail("the refusal%s calls the stand-in older than these "
                     "tools, and its headers declare every function the "
                     "module asks for:\n%s" % (at_flags(flags), output))

    print("PASS")


if __name__ == "__main__":
    main()
