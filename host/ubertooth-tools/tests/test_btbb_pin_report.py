#!/usr/bin/env python3
"""The configure step must say which libbtbb it found and which one is pinned.

FindBTBB.cmake refuses a libbtbb missing a function the host calls. A pin bump
that only changes how libbtbb behaves adds no function, so it also reports the
commit the library it found was built from beside the commit the tree is built
against, and warns when they differ.

libbtbb compiles that commit into the library behind a prefix of its own, and
the module reads the string back out of the library file, so each case here is
a file holding the strings a real libbtbb would hold. Every case must configure successfully: the
report is advisory, and making it fatal would stop a build against a
distribution-packaged libbtbb that is otherwise fine. Every case must name the
pin, which is what a reader of a measurement needs in the log.

Usage: test_btbb_pin_report.py <cmake> <repository root>
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

# The guard the module sets once it has reported, and the one it sets once it
# has run the function probe. The cases below set the second themselves, so a
# configure costs no compiler: the probe builds a program against the library,
# and these libraries are files with a string in them.
REPORT_GUARD = "LIBBTBB_PIN_REPORTED"
PROBE_GUARD = "LIBBTBB_FUNCTIONS_CHECKED"

MODULE = "FindBTBB.cmake"

# The prefix libbtbb puts in front of the commit it records, which is what
# makes it findable in the library file.
MARKER = "libbtbb-commit:"

# The pin is a commit while libbtbb fixes are landing, and the release
# procedure tags both repositories and pins the tag. Both kinds are covered,
# whichever one the tree carries today: the other is given to a copy of the
# module. Neither of these has to exist, because the module compares text and
# resolves nothing; the commit is plainly not one of libbtbb's, and the tag is
# in the form the release procedure uses.
SAMPLE_COMMIT = "0123456789abcdef0123456789abcdef01234567"
SAMPLE_TAG = "2021-01-R1"

# The probe's result variable. It is in the output of any configure that ran
# the probe, so a case that no longer skips it is told apart from one that did.
PROBE = "LIBBTBB_HAS_REQUIRED_FUNCTIONS"

# How many printable bytes to put in front of the recorded string for the
# long-run cases: each one is a few bytes short of a power-of-two boundary, so
# the string lies across it.
LONG_RUN_OFFSETS = (4090, 8186, 16378)

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


def modules_pinned_at(work, modules, ref, name):
    """A copy of the module directory whose pin is ref.

    The module sets the pin itself, so a report against a pin the tree does
    not carry today has to be read from a copy that carries it.
    """
    # The whole directory, so a module that comes to include a sibling finds
    # it here rather than failing for a reason that is not what is tested.
    copy = os.path.join(work, "modules-%s" % name)
    shutil.copytree(modules, copy)
    with open(os.path.join(modules, MODULE), encoding="utf-8") as handle:
        text = handle.read()
    text, replaced = re.subn(r"(?m)^set\(LIBBTBB_PINNED_REF .*\)$",
                             'set(LIBBTBB_PINNED_REF "%s")' % ref, text)
    if replaced != 1:
        fail("%s sets LIBBTBB_PINNED_REF %d times, not once, so this test "
             "cannot give the module another pin"
             % (os.path.join(modules, MODULE), replaced))
    with open(os.path.join(copy, MODULE), "w", encoding="utf-8") as handle:
        handle.write(text)
    return copy


def project(work, name, modules, calls=1):
    """A source directory that finds libbtbb through the modules given.

    calls is how many find_package(BTBB) calls it makes, which is what a tree
    with several subdirectories does.
    """
    source = os.path.join(work, "project-%s" % name)
    os.mkdir(source)
    with open(os.path.join(source, "CMakeLists.txt"), "w",
              encoding="utf-8") as handle:
        handle.write(PROJECT % {"modules": modules,
                                "probe_guard": PROBE_GUARD})
        handle.write("find_package(BTBB REQUIRED)\n" * (calls - 1))
    return source


def pinned_ref(repo_root):
    """The pinned libbtbb commit.

    Read from .github/workflows/build.yml, where the pin is decided, rather
    than from FindBTBB.cmake, so the commit the report names is checked
    against the pin itself and not against its own copy of it.
    """
    build_yml = os.path.join(repo_root, ".github", "workflows", "build.yml")
    with open(build_yml, encoding="utf-8") as handle:
        found = re.search(r"^  LIBBTBB_REF: *(\S+)$", handle.read(), re.M)
    if not found:
        fail("%s does not set LIBBTBB_REF" % build_yml)
    return found.group(1)


def library(work, name, *strings, padding=0):
    """A file holding the strings a libbtbb would hold, and nothing else.

    Each string is surrounded the way it is in a real library: the bytes
    around it are whatever the compiler put there, so the run of printable
    text the string sits in carries other characters too, and a NUL ends it.

    padding puts that many printable bytes in front of each string, so the
    string sits deep inside one long run rather than next to a NUL. See
    LONG_RUN_OFFSETS.
    """
    path = os.path.join(work, name)
    with open(path, "wb") as handle:
        handle.write(b"btbb_get_commit\0")
        for text in strings:
            handle.write(b"A" * padding + b"\t"
                         + text.encode("ascii") + b"\0")
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
    module_guards(os.path.join(modules, MODULE))

    ref = pinned_ref(repo_root)
    pin_is_commit = re.fullmatch("[0-9a-fA-F]{40}", ref) is not None

    with tempfile.TemporaryDirectory() as work:
        # The cases below need a pin of each kind. The tree's own pin is used
        # for its kind, so the commit the report names is checked against the
        # pin itself, and the other kind comes from a copy of the module.
        if pin_is_commit:
            commit_modules, commit_pin = modules, ref
            tag_pin = SAMPLE_TAG
            tag_modules = modules_pinned_at(work, modules, tag_pin, "tag")
        else:
            commit_pin = SAMPLE_COMMIT
            commit_modules = modules_pinned_at(work, modules, commit_pin,
                                               "commit")
            tag_modules, tag_pin = modules, ref

        # libbtbb records the commit in full, as the pin is written.
        recorded = commit_pin
        # A commit the pin cannot be, for the cases that need a different one.
        other = "0176769" * 5 + "01234"
        if other == commit_pin:
            other = "52b2afc" * 5 + "56789"

        source = project(work, "commit-pin", commit_modules)

        # The header has to be where the module left it or the module searches
        # for libbtbb again and these cases lose their library. Nothing is
        # compiled, so an empty one is enough.
        include_dir = os.path.join(work, "include")
        os.mkdir(include_dir)
        open(os.path.join(include_dir, "btbb.h"), "w").close()

        cases = (
            # name, release strings in the library, must warn, wording the
            # report has to carry
            ("pinned", [MARKER + recorded], False,
             ["built from commit %s" % recorded]),
            # The same commit written in capitals, which must read as the
            # same commit and not as another one. The commit is echoed as the
            # library records it; only the comparison against the pin folds
            # case.
            ("pinned-upper", [MARKER + recorded.upper()], False,
             ["built from commit %s" % recorded.upper()]),
            ("other-commit", [MARKER + other], True,
             ["built from commit %s" % other]),
            ("modified", [MARKER + recorded + "*"], True,
             ["built from commit %s with uncommitted changes" % recorded]),
            # Nothing a commit can be read from. No warning: a libbtbb from a
            # distribution package looks like this and may be fine.
            ("no-commit", [], False, ["commit unknown"]),
            # A libbtbb installed before the commit field, which recorded
            # the commit in its release string instead. That is the library a
            # machine with an older install has, and it is not the pinned one,
            # so it has to be read and warned about rather than passing as a
            # library that records nothing.
            ("older-libbtbb", ["git-%s" % other[:7]], True,
             ["built from commit %s" % other[:7]]),
            # The same form, holding the pinned commit: a libbtbb built at the
            # pin before the field existed is the right library, and must not
            # be warned about.
            ("older-libbtbb-pinned", ["git-%s" % recorded[:7]], False,
             ["built from commit %s" % recorded[:7]]),
            # The library's own commit and another string that looks like one.
            # This one does warn: the library may well be the wrong one, and a
            # status line among hundreds is how that went unnoticed before.
            ("two-commits", [MARKER + recorded, MARKER + other], True,
             ["commit unknown", "more than one commit"]),
        )

        for name, strings, must_warn, wanted in cases:
            path = library(work, "libbtbb-%s.bin" % name, *strings)
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
            if commit_pin not in output:
                fail("the %s case does not name the pinned commit %s:\n%s"
                     % (name, commit_pin, output))
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
            # The commands only where the library is known not to be the
            # pinned one. Where nothing is known to be wrong, telling the
            # reader to reinstall is advice for a library that may be right.
            commands = "git checkout %s" % commit_pin in output
            if must_warn and "commit unknown" not in wanted and not commands:
                fail("the %s case does not say how to install the pinned "
                     "libbtbb:\n%s" % (name, output))
            if "commit unknown" in wanted and commands:
                fail("the %s case tells the reader to reinstall a libbtbb "
                     "that nothing here says is wrong:\n%s" % (name, output))
            # Neither answer may be given as the other: a commit that cannot
            # be read must not be reported as one that can, and a commit the
            # library does record must not come out as unknown. The pin is a
            # superstring of its own abbreviation, so a line saying "commit
            # unknown" still carries it and an assertion on the abbreviation
            # alone would pass here.
            if "commit unknown" in wanted and "built from commit" in output:
                fail("the %s case names a commit it cannot have read:\n%s"
                     % (name, output))
            if "commit unknown" not in wanted and "commit unknown" in output:
                fail("the %s case cannot read the commit its library "
                     "records:\n%s" % (name, output))

        # The pinned library again, with its string deep inside one long run
        # of printable bytes. A reader that bounds how long a run it will
        # look at loses the string that straddles its boundary and reports no
        # commit, which is the silence this whole report is here to remove.
        # Each offset puts the string across one of the boundaries such a
        # bound would fall on.
        for offset in LONG_RUN_OFFSETS:
            name = "long-run-%d" % offset
            path = library(work, "libbtbb-%s.bin" % name, MARKER + recorded,
                           padding=offset)
            status, output = configure(cmake, source,
                                       os.path.join(work, "build-%s" % name),
                                       include_dir, path)
            if status != 0:
                fail("the %s libbtbb was refused:\n%s" % (name, output))
            if "built from commit %s" % recorded not in output:
                fail("a commit %d printable bytes into one run is not read, "
                     "so a library whose string sits in a long run of data "
                     "reports no commit:\n%s" % (offset, output))

        # Two find_package(BTBB) calls, which is what a tree with several
        # subdirectories does, must report once: the line is read by a person,
        # and a report per call would be noise they learn to skip.
        twice = project(work, "twice", commit_modules, calls=2)
        path = library(work, "libbtbb-twice.bin", MARKER + recorded)
        status, output = configure(cmake, twice,
                                   os.path.join(work, "build-twice"),
                                   include_dir, path)
        if status != 0:
            fail("two find_package(BTBB) calls were refused:\n%s" % output)
        if output.count("libbtbb: %s" % path) != 1:
            fail("two find_package(BTBB) calls report %d times, not once:\n%s"
                 % (output.count("libbtbb: %s" % path), output))

        # A LIBBTBB_LIBRARIES the user set to a bare link name: there is no
        # file to read, which is a different answer from a library that
        # records no commit, and saying the wrong one sends the reader to the
        # wrong place.
        status, output = configure(cmake, source,
                                   os.path.join(work, "build-no-file"),
                                   include_dir, "btbb")
        if status != 0:
            fail("a bare library name was refused:\n%s" % output)
        if "no library file behind that name" not in output:
            fail("a library name with no file behind it is not reported as "
                 "such:\n%s" % output)

        # A pin that is not a commit. The release procedure tags libbtbb and
        # this tree with the release version and the pin then names the tag,
        # which no commit in a library can be compared with: a text
        # comparison against a tag matches nothing, so comparing anyway would
        # warn about every correct build. The report must name both ends and
        # claim nothing.
        tag_source = project(work, "tag-pin", tag_modules)
        tag_cases = (
            # name, release strings in the library, must warn, wording it has
            # to carry
            ("tag-pin", [MARKER + other], False,
             ["built from commit %s" % other,
              "names no commit to compare it with"]),
            ("tag-pin-no-commit", [], False, ["commit unknown"]),
            # Two commit-looking strings warn against a commit pin, because
            # one of them may be a stale library's and the other something
            # else in the file. Against a tag neither would decide anything,
            # so this one is as quiet as the case above.
            ("tag-pin-two-commits", [MARKER + recorded, MARKER + other],
             False, ["commit unknown", "more than one commit"]),
            # A library built from an edited tree is not the pinned libbtbb
            # whatever the pin is, which needs no comparison, so this one
            # warns as it does against a commit pin.
            ("tag-pin-modified", [MARKER + other + "*"], True,
             ["built from commit %s with uncommitted changes" % other]),
        )
        for name, strings, must_warn, wanted in tag_cases:
            path = library(work, "libbtbb-%s.bin" % name, *strings)
            status, output = configure(cmake, tag_source,
                                       os.path.join(work, "build-%s" % name),
                                       include_dir, path)
            if status != 0:
                fail("the %s libbtbb was refused:\n%s" % (name, output))
            if tag_pin not in output:
                fail("the %s case does not name the pinned %s, so the log "
                     "does not say which libbtbb the tree asks for:\n%s"
                     % (name, tag_pin, output))
            warned = "CMake Warning" in output
            if warned and not must_warn:
                fail("the %s case warns, and a tag names no commit for the "
                     "library's to differ from, so every correct build would "
                     "be warned about:\n%s" % (name, output))
            if must_warn and not warned:
                fail("the %s case does not warn, so a library that is not "
                     "the pinned one is only a status line among "
                     "hundreds:\n%s" % (name, output))
            if "is the commit this tree is built against" in output:
                fail("the %s case says the library is the pinned one, which "
                     "a tag cannot tell it:\n%s" % (name, output))
            for text in wanted:
                if text not in output:
                    fail("the %s case does not say %r:\n%s"
                         % (name, text, output))

    print("PASS")


if __name__ == "__main__":
    main()
