#!/usr/bin/env python3
"""The version a build reports, and whether it counts as a release build.

host/cmake/set_release.cmake works both out from what git says the checkout
is. A user reporting a problem reads the version off their own build, so it
has to name the release they have -- the release tag the build came from, or
the nearest release and how many commits are past it -- and never a number
that is not that distance.

Each case here is a checkout of its own that this test builds with git, with
a copy of the module in it, so the answers are about a history written for
the case rather than about whatever the source tree happens to be. The module asks git about its own
directory and only answers for a checkout that has it where this project does,
so each scratch checkout is laid out the way this one is: the module in
host/cmake, the makefile in firmware.

A build with no git history to ask -- a source archive -- reads the release
out of RELEASE-VERSION instead, which git archive fills in as it makes the
archive. The cases for that write the file themselves, and one of them makes
a real archive of a tag and builds the answer from it.

firmware/version.mk works the same string out the same way for the firmware
images, and the two are checked against each other at the end: a board and
the tools that talk to it have to report one version between them.

Usage: test_release_version.py <cmake> <repository root>
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

MODULE = os.path.join("host", "cmake", "set_release.cmake")
FIRMWARE_MK = os.path.join("firmware", "version.mk")
# The file git archive fills the release into, and the attribute that asks it
# to. A build with no git history reads the release there.
VERSION_FILE = "RELEASE-VERSION"
ATTRIBUTES = ".gitattributes"

# What the driver below prints, one line each.
DRIVER = """\
include("${CMAKE_CURRENT_LIST_DIR}/set_release.cmake")
message(STATUS "RELEASE_STRING=${RELEASE_STRING}")
message(STATUS "UBERTOOTH_RELEASE=${UBERTOOTH_RELEASE}")
"""

# The identity goes on each git command and the commits are not signed, so
# this does not depend on the git configuration of whoever runs it.
GIT_ID = ["-c", "user.name=test", "-c", "user.email=test@example.invalid",
          "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false"]

failures = 0


def fail(message):
    global failures
    print("FAIL: %s" % message)
    failures += 1


def git(directory, *arguments):
    done = subprocess.run(["git"] + GIT_ID + list(arguments), cwd=directory,
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                          text=True)
    if done.returncode != 0:
        print("FAIL: git %s in %s: %s"
              % (" ".join(arguments), directory, done.stderr.strip()))
        sys.exit(1)


def commit(directory, text):
    """One commit, with a file whose contents change so no two are the same."""
    with open(os.path.join(directory, "file.txt"), "w") as handle:
        handle.write(text + "\n")
    git(directory, "add", "file.txt")
    git(directory, "commit", "-q", "-m", text)


def put_module(repo_root, tree):
    """The module and a driver for it, where this project keeps the module."""
    cmake_dir = os.path.join(tree, os.path.dirname(MODULE))
    os.makedirs(cmake_dir, exist_ok=True)
    shutil.copy(os.path.join(repo_root, MODULE),
                os.path.join(cmake_dir, os.path.basename(MODULE)))
    with open(os.path.join(cmake_dir, "driver.cmake"), "w") as handle:
        handle.write(DRIVER)
    return cmake_dir


def put_version_file(tree, contents):
    """The file a source archive carries the release in, with <contents>."""
    with open(os.path.join(tree, VERSION_FILE), "w") as handle:
        handle.write(contents)


def git_version():
    """git's own version, as (major, minor)."""
    done = subprocess.run(["git", "--version"], stdout=subprocess.PIPE,
                          text=True)
    found = re.search(r"(\d+)\.(\d+)", done.stdout)
    return (int(found.group(1)), int(found.group(2))) if found else (0, 0)


def new_tree(root, repo_root, name, commits=1):
    """A checkout with the module in it, committed, and <commits> commits."""
    tree = os.path.join(root, name)
    os.makedirs(tree)
    git(tree, "init", "-q")
    put_module(repo_root, tree)
    git(tree, "add", os.path.dirname(MODULE))
    git(tree, "commit", "-q", "-m", "the module")
    for number in range(commits):
        commit(tree, "commit %d" % number)
    return tree


def answers(cmake, cmake_dir, *arguments):
    """What the module says, as (release string, release build)."""
    done = subprocess.run([cmake] + list(arguments) + ["-P", "driver.cmake"],
                          cwd=cmake_dir, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    if done.returncode != 0:
        print("FAIL: the module would not run in %s:\n%s"
              % (cmake_dir, done.stdout))
        sys.exit(1)
    found = dict(re.findall(r"^-- (RELEASE_STRING|UBERTOOTH_RELEASE)=(.*)$",
                            done.stdout, re.M))
    if "RELEASE_STRING" not in found:
        print("FAIL: the module printed no version:\n%s" % done.stdout)
        sys.exit(1)
    return found["RELEASE_STRING"], found["UBERTOOTH_RELEASE"]


def expect(cmake, tree, what, release, release_build, *arguments):
    cmake_dir = os.path.join(tree, os.path.dirname(MODULE))
    got_release, got_build = answers(cmake, cmake_dir, *arguments)
    if got_release != release:
        fail("%s: version %r, expected %r" % (what, got_release, release))
    if got_build != release_build:
        fail("%s: release build %r, expected %r"
             % (what, got_build, release_build))


def firmware_version(repo_root, directory):
    """What firmware/version.mk works out for the checkout in <directory>.

    The makefile goes where this project keeps it, inside the checkout, and
    is asked from there: that is where the image directories include it from,
    and it only answers for a checkout that has it in firmware/.
    """
    where = os.path.join(directory, os.path.dirname(FIRMWARE_MK))
    os.makedirs(where, exist_ok=True)
    makefile = os.path.join(where, "version.mk")
    wrapper = os.path.join(where, "ask-version.mk")
    shutil.copy(os.path.join(repo_root, FIRMWARE_MK), makefile)
    # A wrapper rather than a target in version.mk itself: the makefiles that
    # include it must keep their own first target as the default goal.
    with open(wrapper, "w") as handle:
        handle.write("include version.mk\n"
                     "print-version:\n"
                     "\t@printf '%s\\n' '$(VERSION_NOW)'\n")
    done = subprocess.run(
        ["make", "-s", "-f", "ask-version.mk", "print-version"],
        cwd=where, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, input="")
    os.remove(makefile)
    os.remove(wrapper)
    if done.returncode != 0:
        print("FAIL: version.mk would not run in %s:\n%s"
              % (directory, done.stdout))
        sys.exit(1)
    return done.stdout.strip()


def main():
    global failures
    if len(sys.argv) != 3:
        print("usage: test_release_version.py <cmake> <repository root>")
        sys.exit(1)
    cmake, repo_root = sys.argv[1:]
    repo_root = os.path.abspath(repo_root)
    # Without the attribute, git archive would copy the placeholder through
    # untouched and every source archive would report no release at all,
    # which nothing else here would notice.
    if not os.path.exists(os.path.join(repo_root, VERSION_FILE)):
        print("FAIL: these sources carry no %s" % VERSION_FILE)
        sys.exit(1)
    with open(os.path.join(repo_root, ATTRIBUTES)) as handle:
        if not re.search(r"%s\s+export-subst" % VERSION_FILE, handle.read()):
            print("FAIL: %s does not mark %s export-subst, so git archive "
                  "would not fill the release in" % (ATTRIBUTES, VERSION_FILE))
            sys.exit(1)
    # The release tag patterns are written three times: as --match arguments
    # in the makefile, as globs in the module, and inside the placeholder in
    # the version file, which is what git matches against when it makes an
    # archive. A kind of tag added to one and not the others would cost
    # archives their release with nothing failing, so they are counted
    # against each other.
    with open(os.path.join(repo_root, VERSION_FILE)) as handle:
        placeholder = handle.read()
    patterns = set()
    for path in (MODULE, FIRMWARE_MK):
        with open(os.path.join(repo_root, path)) as handle:
            found = re.findall(r"""--match ["']([^"'\s]+)["']""",
                               handle.read())
        if not found:
            print("FAIL: %s asks git to match no release tag pattern" % path)
            sys.exit(1)
        if patterns and set(found) != patterns:
            print("FAIL: %s and %s ask git to match different release tag "
                  "patterns" % (MODULE, FIRMWARE_MK))
            sys.exit(1)
        patterns = set(found)
    if placeholder.count("match=") != len(patterns):
        print("FAIL: %s asks git to match %d release tag patterns and the "
              "build asks for %d" % (VERSION_FILE,
                                     placeholder.count("match="),
                                     len(patterns)))
        sys.exit(1)
    for pattern in sorted(patterns):
        if ("match=" + pattern) not in placeholder:
            print("FAIL: %s does not ask git to match %r, so an archive made "
                  "at such a tag would report no release"
                  % (VERSION_FILE, pattern))
            sys.exit(1)

    with tempfile.TemporaryDirectory() as work:
        # HEAD is the release tag itself: that release, and a release build,
        # which is what makes the tools refuse firmware of another API
        # version.
        tagged = new_tree(work, repo_root, "tagged")
        git(tagged, "tag", "2026-11-RC1")
        expect(cmake, tagged, "a release tag", "2026-11-RC1", "ON")

        # The same checkout with uncommitted changes. Still a build of that
        # release tag, so still a release build.
        with open(os.path.join(tagged, "file.txt"), "a") as handle:
            handle.write("edited\n")
        expect(cmake, tagged, "a release tag, modified", "2026-11-RC1*", "ON")
        git(tagged, "checkout", "-q", "--", "file.txt")

        # Commits past the tag: the release it comes after, how far past, and
        # a development build, which warns about another API version and runs
        # the command.
        commit(tagged, "after the tag")
        commit(tagged, "after that")
        expect(cmake, tagged, "two commits past a release tag",
               "2026-11-RC1+2", "OFF")
        with open(os.path.join(tagged, "file.txt"), "a") as handle:
            handle.write("edited\n")
        expect(cmake, tagged, "two commits past a release tag, modified",
               "2026-11-RC1+2*", "OFF")
        git(tagged, "checkout", "-q", "--", "file.txt")

        # A detached HEAD, which is what a build of a checked-out commit is.
        git(tagged, "checkout", "-q", "--detach", "HEAD")
        expect(cmake, tagged, "a detached HEAD", "2026-11-RC1+2", "OFF")

        # Tags that are not release tags are not read as one: a developer's
        # own, and one dated like a release without being one. There is then
        # no release to count from, and the distance is reported as unknown
        # rather than counted from the start of history -- that count is not
        # the distance from a release, and a shallow clone would answer it
        # with its own depth.
        # A name that starts like a release and goes on, which a glob cannot
        # rule out, is in the same position: not a release.
        untagged = new_tree(work, repo_root, "untagged")
        git(untagged, "tag", "wip-something")
        git(untagged, "tag", "2025-03-snapshot")
        git(untagged, "tag", "2026-11-R1-local")
        expect(cmake, untagged, "no release tag in reach", "unknown", "OFF")

        # A shallow clone, which is what a continuous integration checkout is
        # by default.
        shallow = os.path.join(work, "shallow")
        git(work, "clone", "-q", "--depth", "1", "--no-tags",
            "file://%s" % tagged, shallow)
        expect(cmake, shallow, "a shallow clone", "unknown", "OFF")

        # No git history and no version file: nothing says which release this
        # is. Still a release build, so sources that shipped with no history
        # to read keep the guard that refuses firmware of another API version.
        plain = os.path.join(work, "plain")
        put_module(repo_root, plain)
        expect(cmake, plain, "no git history", "unknown", "ON")

        # No git history, and the version file as git archive fills it in:
        # that release, or that release and how far past it the archive was
        # made. The forms are git's own describe output.
        put_version_file(plain, "# a comment\n2026-11-R2\n")
        expect(cmake, plain, "an archive of a release tag", "2026-11-R2", "ON")
        put_version_file(plain, "2026-11-R2-22-gf0fe176\n")
        expect(cmake, plain, "an archive made 22 commits past a release",
               "2026-11-R2+22", "ON")

        # The file as a git checkout has it, with nothing filled in, and the
        # cases that name no release: a history with no release tag leaves the
        # line empty, and a tag the globs let through is not a release.
        shutil.copy(os.path.join(repo_root, VERSION_FILE),
                    os.path.join(plain, VERSION_FILE))
        expect(cmake, plain, "the placeholder, untouched", "unknown", "ON")
        put_version_file(plain, "# a comment\n\n")
        expect(cmake, plain, "an archive with no release tag in its history",
               "unknown", "ON")
        put_version_file(plain, "2026-11-R1-local\n")
        expect(cmake, plain, "an archive of a tag that is not a release",
               "unknown", "ON")
        os.remove(os.path.join(plain, VERSION_FILE))

        # And the real thing: a git archive of a tag, which is what a release
        # download is. git fills the release in as it makes the archive, so
        # this is the whole path end to end rather than a file written here.
        archived = None
        if git_version() < (2, 32):
            print("git is older than 2.32, so the archive it makes goes "
                  "unchecked")
        else:
            source = new_tree(work, repo_root, "to-archive")
            for name in (VERSION_FILE, ATTRIBUTES):
                shutil.copy(os.path.join(repo_root, name),
                            os.path.join(source, name))
            git(source, "add", VERSION_FILE, ATTRIBUTES)
            git(source, "commit", "-q", "-m", "the version file")
            git(source, "tag", "2026-11-R3")
            archived = os.path.join(work, "archived")
            tarball = os.path.join(work, "archived.tar")
            git(source, "archive", "--format=tar", "-o", tarball, "2026-11-R3")
            shutil.unpack_archive(tarball, archived, "tar")
            expect(cmake, archived, "a git archive of a release tag",
                   "2026-11-R3", "ON")

            commit(source, "past the tag")
            past = os.path.join(work, "archived-past")
            git(source, "archive", "--format=tar", "-o", tarball, "HEAD")
            shutil.unpack_archive(tarball, past, "tar")
            expect(cmake, past, "a git archive made past a release tag",
                   "2026-11-R3+1", "ON")

        # Both settings given on the command line are what the build is told,
        # whatever git would have said: that is how the release procedure
        # builds, and how CI asks for a release build.
        expect(cmake, tagged, "a version given on the command line",
               "a version of my own", "ON",
               "-DRELEASE_STRING=a version of my own",
               "-DUBERTOOTH_RELEASE=ON")
        # One given as an empty value says nothing, so git decides it. That
        # matters most for the release build: an empty value left as given
        # would warn about firmware of another API version where a release
        # build has to refuse it.
        expect(cmake, tagged, "an empty version on the command line",
               "2026-11-RC1+2", "OFF", "-DRELEASE_STRING=")
        expect(cmake, tagged, "an empty release setting on the command line",
               "2026-11-RC1+2", "OFF", "-DUBERTOOTH_RELEASE=")

        # The same archive unpacked inside an unrelated repository, which is a
        # vendored copy or just a tarball in a working directory. git looks up
        # the directory tree, so that repository is the one it would answer
        # about, and its tags are not this project's releases.
        nested = os.path.join(tagged, "vendor", "ubertooth")
        put_module(repo_root, nested)
        expect(cmake, nested, "an archive inside another repository",
               "unknown", "ON")
        # The same copy, committed to that repository, which is what a
        # vendored copy is. Still not a checkout of this project: the tag it
        # would otherwise be given is that repository's.
        git(tagged, "add", "vendor")
        git(tagged, "commit", "-q", "-m", "vendor a copy")
        expect(cmake, nested, "a vendored copy another repository tracks",
               "unknown", "ON")

        # The firmware works the string out the same way, so a board and the
        # tools report one version between them. Asked of the same checkouts,
        # through a target of its own that prints what it decided.
        if shutil.which("make") is None:
            print("no make was found, so the firmware's answer goes unchecked")
        else:
            trees = [(tagged, "a checkout two commits past a tag"),
                     (untagged, "no release tag in reach"),
                     (shallow, "a shallow clone")]
            if archived is not None:
                trees.append((archived, "a git archive of a release tag"))
            for tree, what in trees:
                host_release, _ = answers(
                    cmake, os.path.join(tree, os.path.dirname(MODULE)))
                firmware = firmware_version(repo_root, tree)
                if firmware != host_release:
                    fail("%s: the firmware reports %r and the host tools %r; "
                         "a board and the tools that talk to it have to "
                         "report one version" % (what, firmware, host_release))

    if failures:
        print("%d case(s) failed" % failures)
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
