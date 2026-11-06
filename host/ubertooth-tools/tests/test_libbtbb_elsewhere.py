#!/usr/bin/env python3
"""-V says so when the run loads a libbtbb from somewhere else.

A machine can have a distribution or Homebrew libbtbb beside this fork's
build, and which one a tool loads is decided by its link path at run time, not
by what the build linked. The two decode packets their own way, so a run that
picked up the other one has to say which it is using; nothing else in a run
does.

The decision behind the line is tested on paths of its own (libbtbb-origin).
This test is about the wiring: it gives the loader another copy of libbtbb to
find, through the loader's own search path, and asks whether the tool noticed.
Without this, swapping the two paths the library asks the loader for would
pass every other test on a machine with one libbtbb.

Where the loader ignores the search path -- a tool linked against an absolute
path on a system whose loader does not consult it -- there is nothing to
notice, and the test says so rather than failing: it is the loader's answer
that decides, and that is a property of the machine.

Usage: test_libbtbb_elsewhere.py <tool> <libbtbb library file>
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile

WARNING = "warning: libbtbb was loaded from "
HOST_LINE = "Host tools version: "


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def run(tool, directory):
    """`-V` with <directory> first on the loader's search path."""
    environment = dict(os.environ)
    # No board is looked for: this is about the host half of the answer.
    environment["UBERTOOTH_TEST_NO_DEVICES"] = "1"
    for name in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        existing = environment.get(name)
        environment[name] = directory if not existing \
            else directory + os.pathsep + existing
    return subprocess.run([tool, "-V"], env=environment,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, timeout=60)


def main():
    if len(sys.argv) != 3:
        fail("usage: test_libbtbb_elsewhere.py <tool> <libbtbb library file>")
    tool, library = sys.argv[1:]
    if not os.path.isabs(library) or not os.path.exists(library):
        # A build given a bare link name has no file to copy, and then this
        # machine cannot be given a second one.
        print("the libbtbb this build linked is not a file here (%s), so "
              "there is nothing to copy" % library)
        return

    with tempfile.TemporaryDirectory() as work:
        # Every name libbtbb installs under, so whichever one the loader looks
        # for is in the copy: the build linked libbtbb.so and the loader asks
        # for the soname, libbtbb.so.1.
        source = os.path.dirname(library)
        stem = os.path.basename(library).split(".")[0]
        copied = 0
        for path in glob.glob(os.path.join(source, stem + ".*")):
            if os.path.isdir(path):
                continue
            # Resolved, so a symbolic link becomes a file of its own: a link
            # copied as a link would point back into the original directory.
            shutil.copyfile(os.path.realpath(path),
                            os.path.join(work, os.path.basename(path)))
            copied += 1
        if copied == 0:
            fail("no libbtbb file was found beside %s" % library)

        result = run(tool, work)
        if result.returncode != 0:
            fail("-V exited %d with another libbtbb on the search path:\n%s%s"
                 % (result.returncode, result.stdout, result.stderr))
        # The answer is still the answer: the warning is a line beside it, not
        # instead of it.
        if not result.stdout.startswith(HOST_LINE):
            fail("-V did not print the host version:\n%s" % result.stdout)

        notices = [line for line in result.stderr.splitlines()
                   if line.startswith(WARNING)]
        if not notices:
            if result.stderr.strip() != "":
                fail("-V wrote something other than the one warning to "
                     "standard error:\n%s" % result.stderr)
            print("this loader did not take the copied libbtbb, so there was "
                  "nothing for -V to report")
            return
        if len(notices) != 1:
            fail("-V wrote the warning %d times; one run loads one libbtbb:\n%s"
                 % (len(notices), result.stderr))
        if work not in notices[0]:
            fail("the warning does not name the directory the libbtbb was "
                 "loaded from (%s):\n%s" % (work, notices[0]))
        print("PASS")


if __name__ == "__main__":
    main()
