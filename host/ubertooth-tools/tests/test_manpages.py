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

Exits 77 (CTest's skip code here) when CMake finds no ronn, which is also the
state in which a build must work: without ronn neither target renders
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
# must leave them as they are. Mirrors MAN_PAGES_BY_HAND in the CMake file.
HAND_MAINTAINED = {"ubertooth-debug.1"}

# The .TH fields the committed pages carry. ronn writes an empty organization
# and manual unless it is given them, so a refresh that dropped these would
# quietly degrade every page.
ORGANIZATION = "Project Ubertooth"
MANUALS = {"1": "User Commands", "7": "Project Ubertooth Overview"}

SKIP = 77


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def ronn_directories():
    """The directories to keep CMake's find_program away from ronn.

    Only ronn's own, by both its symlink and its real path: hiding a shared
    prefix such as /usr/bin would take the compiler and make with it. Empty
    when ronn lives somewhere that cannot be hidden, in which case the caller
    skips the check that needs it.
    """
    found = shutil.which("ronn") or shutil.which("ronn-ng")
    if not found:
        return []
    paths = []
    for path in (os.path.dirname(found),
                 os.path.dirname(os.path.realpath(found))):
        if path in ("/usr/bin", "/bin", "/usr/sbin", "/sbin"):
            return []
        if path not in paths:
            paths.append(path)
    return paths


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
        missing = HAND_MAINTAINED - set(committed)
        if missing:
            fail("%s not committed; update this test" % sorted(missing))

        # Whether ronn is there is CMake's answer, not this script's: it
        # searches more than PATH, so asking separately can skip the test
        # while the behaviour under test is live.
        configure = run([cmake, "-S", work, "-B", build], "configure")
        if "ronn not found" in configure:
            print("SKIP: CMake found no ronn, so nothing renders man pages")
            sys.exit(SKIP)
        if "found: refresh-manpages can" not in configure:
            fail("the configure step said nothing usable about ronn:\n%s"
                 % configure)

        run([cmake, "--build", build], "the default build")
        if digests(man) != committed:
            fail("an ordinary build rewrote the man pages in the source tree")

        run([cmake, "--build", build, "--target", "manpages"], "manpages")
        if digests(man) != committed:
            fail("the manpages target wrote to the source tree")
        generated = digests(rendered)
        expected = len(committed) - len(HAND_MAINTAINED)
        if len(generated) != expected:
            fail("manpages rendered %d pages into the build directory, "
                 "expected %d: %s"
                 % (len(generated), expected, sorted(generated)))
        if HAND_MAINTAINED & set(generated):
            fail("%s was generated; it is maintained by hand"
                 % sorted(HAND_MAINTAINED & set(generated)))

        # A refresh must not strip the page headings: without --organization
        # and --manual, ronn writes `.TH "NAME" "1" "<date>" ""`. The date
        # must not move either: ronn defaults it to the .md's modification
        # time, every .md was touched above, and a page whose date follows the
        # clock would differ from the committed one in a month's time, which
        # no check could tell from a real change.
        for name in sorted(generated):
            title = title_line(os.path.join(rendered, name))
            manual = MANUALS[name.rsplit(".", 1)[1]]
            for field in ('"%s"' % ORGANIZATION, '"%s"' % manual):
                if field not in title:
                    fail("%s lost %s from its .TH line: %s"
                         % (name, field, title))
            if title != title_line(os.path.join(man, name)):
                fail("%s renders a different .TH line from the committed "
                     "page, so a refresh is not reproducible:\n  rendered:  "
                     "%s\n  committed: %s"
                     % (name, title, title_line(os.path.join(man, name))))

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
        for name in sorted(HAND_MAINTAINED):
            if refreshed[name] != committed[name]:
                fail("refresh-manpages rewrote %s" % name)
        if set(refreshed) != set(committed):
            fail("refresh-manpages added or removed a page: %s"
                 % sorted(set(refreshed) ^ set(committed)))

        # Without a usable ronn the targets must fail, not print and succeed.
        # A check that runs refresh-manpages and then looks for a change would
        # otherwise pass having refreshed nothing. Hiding ronn's directory is
        # how such a machine looks to CMake's find_program.
        hidden_from = ronn_directories()
        if not hidden_from:
            print("ronn cannot be hidden from CMake here; not checking that "
                  "the targets refuse without it")
        else:
            hidden = os.path.join(work, "no-ronn")
            configure = run([cmake, "-S", work, "-B", hidden,
                             "-DCMAKE_IGNORE_PATH=%s" % ";".join(hidden_from)],
                            "configure with ronn hidden")
            if "ronn not found" not in configure:
                fail("hiding %s did not stop CMake finding ronn, so the "
                     "refusing targets are untested:\n%s"
                     % (hidden_from, configure))
            for target in ("manpages", "refresh-manpages"):
                done = subprocess.run([cmake, "--build", hidden, "--target",
                                       target], stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True)
                if done.returncode == 0:
                    fail("%s succeeded without ronn; it rendered nothing:\n%s"
                         % (target, done.stdout))
                # CMake wraps a long message, so compare on one line.
                if "gem install ronn-ng" not in " ".join(done.stdout.split()):
                    fail("%s failed without saying how to fix it:\n%s"
                         % (target, done.stdout))
            if digests(man) != refreshed:
                fail("a refusing target wrote to the source tree")

    print("PASS")


if __name__ == "__main__":
    main()
