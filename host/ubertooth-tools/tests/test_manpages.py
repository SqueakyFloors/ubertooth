#!/usr/bin/env python3
"""A build must not write to host/doc/man, and refresh-manpages must.

The man pages are generated from host/doc/*.md with ronn, but the generated
.1 files are committed: they are what `make install` installs, so `man` works
without ronn. The generation used to run as part of `all`, writing straight
into the source tree, which left every page modified after an ordinary build
and reverted any hand edit at the next one.

This builds the real host/doc CMake project against a copy of the source
tree, so a regression shows up as a modified copy instead of a dirty
repository:

  * `all` renders nothing and changes nothing;
  * `manpages` renders every page into the build directory, still changing
    nothing;
  * `refresh-manpages` copies them over the committed ones, and leaves the
    pages that have no generated counterpart alone;
  * every rendered page keeps the `.TH` organization and manual fields, which
    ronn leaves empty unless it is told them.

Exits 77 (CTest's skip code here) when ronn is not installed, which is also
the state in which a build must work: without ronn neither target renders
anything.

Usage: test_manpages.py <cmake> <host source dir>
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

# Pages with no .md counterpart to render from: edited by hand, so a refresh
# must leave them as they are.
HAND_MAINTAINED = "ubertooth-debug.1"

# The .TH fields the committed pages carry. ronn writes an empty organization
# and manual unless it is given them, so a refresh that dropped these would
# quietly degrade every page.
ORGANIZATION = "Project Ubertooth"
MANUALS = {"1": "User Commands", "7": "Project Ubertooth Overview"}

SKIP = 77


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def title_line(path):
    """The page's .TH line, which carries its name, section and headings."""
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith(".TH"):
                return line.rstrip("\n")
    fail("%s has no .TH line" % path)


def digests(directory):
    """sha256 of every man page in directory, by file name."""
    result = {}
    for name in sorted(os.listdir(directory)):
        if not (name.endswith(".1") or name.endswith(".7")):
            continue
        with open(os.path.join(directory, name), "rb") as handle:
            result[name] = hashlib.sha256(handle.read()).hexdigest()
    return result


def run(command, what):
    done = subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    if done.returncode != 0:
        fail("%s failed (%d):\n%s" % (what, done.returncode, done.stdout))
    return done.stdout


def main():
    if len(sys.argv) != 3:
        fail("usage: test_manpages.py <cmake> <host source dir>")
    cmake, host_src = sys.argv[1], sys.argv[2]

    ronn = shutil.which("ronn") or shutil.which("ronn-ng")
    if ronn is None:
        print("SKIP: ronn is not installed, so nothing generates man pages")
        sys.exit(SKIP)

    with tempfile.TemporaryDirectory() as work:
        # The doc project reads ../cmake/cmake_uninstall.cmake.in, so the copy
        # needs both directories.
        doc = os.path.join(work, "doc")
        shutil.copytree(os.path.join(host_src, "doc"), doc)
        shutil.copytree(os.path.join(host_src, "cmake"),
                        os.path.join(work, "cmake"))
        man = os.path.join(doc, "man")
        build = os.path.join(work, "build")
        rendered = os.path.join(build, "doc", "man")

        # host/doc is a subproject of host, so it carries no
        # cmake_minimum_required of its own: give it the smallest parent that
        # lets the real host/doc/man/CMakeLists.txt configure on its own.
        with open(os.path.join(work, "CMakeLists.txt"), "w") as handle:
            handle.write("cmake_minimum_required(VERSION 3.5...3.27)\n"
                         "project(manpage_test C)\n"
                         "add_subdirectory(doc)\n")

        # Make every .md newer than its .1, so a rule that regenerates into
        # the source tree certainly fires: copytree keeps the checkout's
        # timestamps, under which make may consider the .1 up to date.
        for name in os.listdir(doc):
            if name.endswith(".md"):
                os.utime(os.path.join(doc, name), None)

        committed = digests(man)
        if not committed:
            fail("no committed man pages in %s" % man)
        if HAND_MAINTAINED not in committed:
            fail("%s is not committed; update this test" % HAND_MAINTAINED)

        configure = run([cmake, "-S", work, "-B", build], "configure")
        if "ronn found" not in configure:
            fail("the configure step did not find ronn:\n%s" % configure)

        run([cmake, "--build", build], "the default build")
        if digests(man) != committed:
            fail("an ordinary build rewrote the man pages in the source tree")

        run([cmake, "--build", build, "--target", "manpages"], "manpages")
        if digests(man) != committed:
            fail("the manpages target wrote to the source tree")
        generated = digests(rendered)
        if len(generated) != len(committed) - 1:
            fail("manpages rendered %d pages into the build directory, "
                 "expected %d: %s"
                 % (len(generated), len(committed) - 1, sorted(generated)))
        if HAND_MAINTAINED in generated:
            fail("%s was generated; it is maintained by hand" % HAND_MAINTAINED)

        # A refresh must not strip the page headings: without --organization
        # and --manual, ronn writes `.TH "NAME" "1" "<date>" ""`.
        for name in sorted(generated):
            title = title_line(os.path.join(rendered, name))
            manual = MANUALS[name.rsplit(".", 1)[1]]
            for field in ('"%s"' % ORGANIZATION, '"%s"' % manual):
                if field not in title:
                    fail("%s lost %s from its .TH line: %s"
                         % (name, field, title))

        # Make one committed page stale, so the refresh has something to do
        # whatever ronn renders: a copy that did nothing would otherwise pass
        # on pages that already match.
        stale = sorted(generated)[0]
        with open(os.path.join(man, stale), "wb") as handle:
            handle.write(b"stale\n")

        run([cmake, "--build", build, "--target", "refresh-manpages"],
            "refresh-manpages")
        refreshed = digests(man)
        for name, digest in generated.items():
            if refreshed.get(name) != digest:
                fail("refresh-manpages did not copy %s into the source tree"
                     % name)
        if refreshed[HAND_MAINTAINED] != committed[HAND_MAINTAINED]:
            fail("refresh-manpages rewrote %s" % HAND_MAINTAINED)
        if set(refreshed) != set(committed):
            fail("refresh-manpages added or removed a page: %s"
                 % sorted(set(refreshed) ^ set(committed)))

    print("PASS")


if __name__ == "__main__":
    main()
