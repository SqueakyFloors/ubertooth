#!/usr/bin/env python3
"""The configure step must say which libbtbb it found and which one is pinned.

FindBTBB.cmake refuses a libbtbb that is missing an entry point the host
calls, which is what a pin bump that adds one leaves behind. A bump that only
changes how libbtbb behaves adds no entry point, so that check passes and the
tools run with the older library's behavior -- which, measured on a bench,
looks exactly like a change in the host's own code. So the configure step also
reports which libbtbb it found, and warns when that is not the pinned one.

libbtbb compiles the commit it was built from into the library and returns it
from btbb_get_release(); nothing it installs records the commit anywhere else
(libbtbb.pc carries the library's ABI version, which does not move with a
commit). The module reads that string out of the library file, so these cases
are files holding the string a real libbtbb would hold.

This runs the real FindBTBB.cmake from a one-line project against:

  * a library built from the pinned commit, which must be named as the pinned
    one and must not warn;
  * a library built from another commit, which must warn, must name both
    commits, and must still configure -- the report is advisory, and making it
    fatal would stop a build against a distribution-packaged libbtbb that is
    otherwise fine;
  * a library built from the pinned commit with uncommitted changes on top,
    which must warn and say so;
  * a library recording no commit, and one recording two, where the commit
    cannot be read: each must say so and name the pin, and neither must warn,
    because nothing here knows that library is the wrong one;
  * a library whose only candidate string is too short to be a commit, which
    must not be compared against the pin on a few characters.

Every case must name the pinned commit, whichever way it went: that is the
line a reader of a measurement needs in the configure log.

Usage: test_btbb_pin_report.py <cmake> <repository root>
"""

import os
import re
import subprocess
import sys
import tempfile

# The guard the module sets once it has reported, and the one it sets once it
# has run the function probe. The cases below set the second themselves, so a
# configure costs no compiler: the probe builds a program against the library,
# and these libraries are files with a string in them.
REPORT_GUARD = "LIBBTBB_PIN_REPORTED"
PROBE_GUARD = "LIBBTBB_FUNCTIONS_CHECKED"

# The probe's result variable. It is in the output of any configure that ran
# the probe, so a case that no longer skips it is told apart from one that did.
PROBE = "LIBBTBB_HAS_REQUIRED_FUNCTIONS"

PROJECT = """\
cmake_minimum_required(VERSION 3.5...3.27)
project(btbb_pin_report_test NONE)
set(CMAKE_MODULE_PATH [[%(modules)s]])
set_property(GLOBAL PROPERTY %(probe_guard)s TRUE)
find_package(BTBB REQUIRED)
"""


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def configure(cmake, source, build, include_dir, library):
    """Configure the test project, returning (status, output on one line)."""
    done = subprocess.run(
        [cmake, "-S", source, "-B", build,
         "-DLIBBTBB_INCLUDE_DIR=%s" % include_dir,
         "-DLIBBTBB_LIBRARIES=%s" % library],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # CMake wraps and indents a message(), so assertions on wording compare
    # against one line.
    return done.returncode, " ".join(done.stdout.split())


def module_guards(module):
    """Check the module still has the two guards this test relies on."""
    with open(module, encoding="utf-8") as handle:
        text = handle.read()
    for guard in (REPORT_GUARD, PROBE_GUARD):
        if guard not in text:
            fail("%s does not mention %s, so this test is checking a module "
                 "it was not written against" % (module, guard))


def pinned_ref(repo_root):
    """The pinned libbtbb commit, or None where the workflow file is absent.

    Read from .github/workflows/build.yml rather than from FindBTBB.cmake, so
    the commit the report names is checked against the pin itself and not
    against its own copy of it.
    """
    build_yml = os.path.join(repo_root, ".github", "workflows", "build.yml")
    if not os.path.exists(build_yml):
        return None
    with open(build_yml, encoding="utf-8") as handle:
        found = re.search(r"^  LIBBTBB_REF: *(\S+)$", handle.read(), re.M)
    if not found:
        fail("%s does not set LIBBTBB_REF" % build_yml)
    return found.group(1)


def library(work, name, *releases):
    """A file holding the release strings a libbtbb would hold, and nothing else.

    Each string is surrounded the way it is in a real library: the bytes
    around it are whatever the compiler put there, so the run of printable
    text the string sits in carries other characters too, and a NUL ends it.
    """
    path = os.path.join(work, name)
    with open(path, "wb") as handle:
        handle.write(b"btbb_get_release\0")
        for release in releases:
            handle.write(b"\t" + release.encode("ascii") + b"\0")
        handle.write(b"1.3\0")
    return path


def main():
    if len(sys.argv) != 3:
        fail("usage: test_btbb_pin_report.py <cmake> <repository root>")
    cmake, repo_root = sys.argv[1:]
    # Absolute, because the project written below names the module directory
    # and is configured from a directory of its own.
    repo_root = os.path.abspath(repo_root)
    modules = os.path.join(repo_root, "host", "cmake", "modules")
    module_guards(os.path.join(modules, "FindBTBB.cmake"))

    ref = pinned_ref(repo_root)
    if ref is None:
        print("no .github/workflows/build.yml here; reading the pin from "
              "FindBTBB.cmake instead")
        with open(os.path.join(modules, "FindBTBB.cmake"),
                  encoding="utf-8") as handle:
            found = re.search(r'set\(LIBBTBB_PINNED_REF "(\S+)"\)',
                              handle.read())
        if not found:
            fail("FindBTBB.cmake does not set LIBBTBB_PINNED_REF")
        ref = found.group(1)
    # What libbtbb records: git's abbreviation of the commit, not the full
    # hash the pin is written as. The report has to see through that.
    short = ref[:7]
    other = "0176769" if not ref.startswith("0176769") else "52b2afc"

    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "project")
        os.mkdir(source)
        with open(os.path.join(source, "CMakeLists.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write(PROJECT % {"modules": modules,
                                    "probe_guard": PROBE_GUARD})

        # The header has to be where the module left it or the module searches
        # for libbtbb again and these cases lose their library. Nothing is
        # compiled, so an empty one is enough.
        include_dir = os.path.join(work, "include")
        os.mkdir(include_dir)
        open(os.path.join(include_dir, "btbb.h"), "w").close()

        cases = (
            # name, release strings in the library, must warn, wording the
            # report has to carry
            ("pinned", ["git-%s" % short], False, [short]),
            ("other-commit", ["git-%s" % other], True, [short, other]),
            ("modified", ["git-%s*" % short], True,
             [short, "uncommitted changes"]),
            ("no-commit", [], False, ["commit unknown"]),
            ("two-commits", ["git-%s" % short, "git-%s" % other], False,
             ["commit unknown"]),
            ("too-short", ["git-abc"], False, ["commit unknown"]),
        )

        for name, releases, must_warn, wanted in cases:
            path = library(work, "libbtbb-%s.bin" % name, *releases)
            status, output = configure(cmake, source,
                                       os.path.join(work, "build-%s" % name),
                                       include_dir, path)
            # Advisory, every case: a mismatch must not stop a build that
            # would otherwise work.
            if status != 0:
                fail("the %s libbtbb was refused; the report is advisory and "
                     "must not fail the configure:\n%s" % (name, output))
            if PROBE in output:
                fail("the %s case ran the function probe, so it is no longer "
                     "testing the report on its own:\n%s" % (name, output))
            if "libbtbb: %s" % path not in output:
                fail("the %s case does not report which libbtbb was found:\n%s"
                     % (name, output))
            # The pin, in every case, including the ones that cannot read a
            # commit: that is what makes the mismatch visible in the log a
            # measurement is taken from.
            if ref not in output:
                fail("the %s case does not name the pinned commit %s:\n%s"
                     % (name, ref, output))
            warned = "CMake Warning" in output
            if must_warn and not warned:
                fail("the %s case does not warn, so a mismatch is only a "
                     "status line among hundreds:\n%s" % (name, output))
            if warned and not must_warn:
                fail("the %s case warns, and nothing here says that libbtbb "
                     "is the wrong one:\n%s" % (name, output))
            for text in wanted:
                if text not in output:
                    fail("the %s case does not say %r:\n%s"
                         % (name, text, output))
            if must_warn and "git checkout %s" % ref not in output:
                fail("the %s case does not say how to install the pinned "
                     "libbtbb:\n%s" % (name, output))
            # A commit that cannot be read must not be reported as one that
            # can, whatever was in the library.
            if "commit unknown" in wanted and "built from commit" in output:
                fail("the %s case names a commit it cannot have read:\n%s"
                     % (name, output))

    print("PASS")


if __name__ == "__main__":
    main()
