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

These sources may be a source archive themselves, which is how two of the
continuous integration jobs are built: their container starts without git, so
the checkout arrives as a tarball. RELEASE-VERSION then holds the release the
archive was made from rather than the placeholder a checkout has, and the
cases that need a placeholder write one from the patterns the build asks git
to match. What such a checkout can be asked, and a git checkout cannot, is
the release it reports for itself.

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


def filled_in(version_file):
    """Whether git has already filled the release into <version_file>.

    git archive rewrites the placeholder as it makes an archive, so the
    "$Format:" the placeholder is wrapped in is there in a git checkout and
    gone in an archive. git takes the wrapper off even where it could not
    expand what was inside, and that is an archive too.
    """
    return "$Format:" not in version_file


def in_checkout(directory):
    """Whether git places <directory> at the top of a checkout of its own.

    The same question the module asks, from the top of the tree rather than
    from host/cmake. An unpacked archive is in no checkout, or inside some
    other repository, and git names neither this directory its top.
    """
    done = subprocess.run(["git", "rev-parse", "--show-prefix"],
                          cwd=directory, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, text=True)
    return done.returncode == 0 and done.stdout.strip() == ""


def missing_patterns(version_file, patterns):
    """Which of <patterns> the version file does not ask git archive to match.

    Only a checkout's version file can be asked: an archive's placeholder is
    already spent, so it asks for nothing and every pattern reads as missing.
    """
    return sorted(p for p in patterns if ("match=" + p) not in version_file)


def put_placeholder(repo_root, tree, archive, patterns):
    """The version file as a git checkout has it, with nothing filled in.

    These sources carry such a file, unless they are a source archive
    themselves: git filled their placeholder in as the archive was made, so
    there is none left to copy and one is written from the patterns the build
    asks git to match, which is what the file in a checkout asks for.
    """
    if not archive:
        shutil.copy(os.path.join(repo_root, VERSION_FILE),
                    os.path.join(tree, VERSION_FILE))
        return
    put_version_file(tree, "$Format:%%(describe:tags,%s)$\n"
                     % ",".join("match=" + p for p in sorted(patterns)))


def release_form(repo_root):
    """The form the module requires a release name to take.

    Read out of the module rather than written again here, so the release the
    sources carry is expected by the rule the build itself applies.
    """
    with open(os.path.join(repo_root, MODULE)) as handle:
        found = re.search(r'set\(RELEASE_TAG_FORM\s+"([^"]+)"\)', handle.read())
    if found is None:
        print("FAIL: %s sets no RELEASE_TAG_FORM, so there is no release name "
              "form to expect" % MODULE)
        sys.exit(1)
    return found.group(1)


def release_from(version_file, form):
    """The release <version_file> names, in the form a build reports it.

    The first line that is neither blank nor a comment is what git filled in:
    a release tag, that tag with how far past it the archive was made
    appended, or nothing where no release tag was in reach. Anything that is
    not a release name, an unspent placeholder among them, names no release.
    """
    for line in version_file.splitlines():
        line = line.strip()
        if line == "" or line.startswith("#"):
            continue
        ahead = ""
        # git appends "-<commits>-g<commit>" where the commit archived is past
        # the tag. A release name has dashes of its own, so the count comes
        # off the end rather than the name out of the front.
        found = re.match(r"^(.+)-([0-9]+)-g[0-9a-f]+$", line)
        if found:
            line, ahead = found.group(1), "+" + found.group(2)
        return line + ahead if re.match(form, line) else "unknown"
    return "unknown"


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
        version_file = handle.read()
    archive = filled_in(version_file)
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
    # The count has to run on a checkout's file, the only one that still
    # carries the placeholder. A source archive's was spent making it, and
    # the patterns go unchecked there rather than read as none. What must not
    # happen is a checkout taking them out of reach: the release procedure
    # writes a release into this file for an archive it builds by hand, and
    # such a file committed to a checkout would leave nothing to check with
    # nothing failing. So git is asked as well, and the two have to agree.
    if archive and in_checkout(repo_root):
        print("FAIL: %s names a release in a git checkout, so the placeholder "
              "that asks git archive for the release tag patterns is gone and "
              "they cannot be checked" % VERSION_FILE)
        sys.exit(1)
    if archive:
        print("these sources are a source archive, where git has already "
              "filled the release in, so the release tag patterns in %s go "
              "unchecked; a git checkout checks them" % VERSION_FILE)
    else:
        missing = missing_patterns(version_file, patterns)
        if missing:
            print("FAIL: %s does not ask git to match %s, so an archive made "
                  "at such a tag would report no release"
                  % (VERSION_FILE, ", ".join(repr(p) for p in missing)))
            sys.exit(1)
        if version_file.count("match=") != len(patterns):
            print("FAIL: %s asks git to match %d release tag patterns and the "
                  "build asks for %d" % (VERSION_FILE,
                                         version_file.count("match="),
                                         len(patterns)))
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

        # The same checkout with a filled-in version file beside it, which is
        # what an archive unpacked and committed to a repository of its own
        # has. git placed this tree in a checkout, so the file is not read:
        # what it says was written for the archive, and this HEAD is not that.
        put_version_file(untagged, "2015-09-R2\n")
        expect(cmake, untagged, "a checkout with no release tag, beside a "
               "filled-in version file", "unknown", "OFF")

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

        # A line with blanks around it, or carried through a download that
        # rewrote its line endings. Both readers have to make the same of it,
        # or a board and the tools that talk to it would name different
        # releases, so each of these is checked against the firmware's answer
        # as well, below.
        odd_lines = [("    2026-11-R2", "blanks before the release"),
                     ("2026-11-R2    ", "blanks after the release"),
                     ("\t2026-11-R2\t", "tabs around the release"),
                     ("2026-11-R2\r", "a carriage return after the release")]
        for line, what in odd_lines:
            put_version_file(plain, "# a comment\n%s\n" % line)
            expect(cmake, plain, "an archive with %s" % what,
                   "2026-11-R2", "ON")

        # The file as a git checkout has it, with nothing filled in, and the
        # cases that name no release: a history with no release tag leaves the
        # line empty, and a tag the globs let through is not a release.
        put_placeholder(repo_root, plain, archive, patterns)
        expect(cmake, plain, "the placeholder, untouched", "unknown", "ON")
        put_version_file(plain, "# a comment\n\n")
        expect(cmake, plain, "an archive with no release tag in its history",
               "unknown", "ON")
        put_version_file(plain, "2026-11-R1-local\n")
        expect(cmake, plain, "an archive of a tag that is not a release",
               "unknown", "ON")

        os.remove(os.path.join(plain, VERSION_FILE))

        # And where these sources are a source archive, the release they
        # themselves carry, which is the one a build of them reports. Only
        # such a checkout can be asked this: a git checkout's file has
        # nothing filled in to read. A tree of its own, so the firmware's
        # answer can be asked for it below as well.
        carried = None
        if archive:
            carried = os.path.join(work, "carried")
            put_module(repo_root, carried)
            put_version_file(carried, version_file)
            expect(cmake, carried, "the release these sources carry",
                   release_from(version_file, release_form(repo_root)), "ON")

        # And the real thing: a git archive of a tag, which is what a release
        # download is. git fills the release in as it makes the archive, so
        # this is the whole path end to end rather than a file written here.
        archived = None
        # 2.35 is where git archive understands every option in the
        # placeholder: %(describe) arrived in 2.32 and its "tags" option in
        # 2.35.
        if git_version() < (2, 35):
            print("git is older than 2.35, so the archive it makes goes "
                  "unchecked")
        else:
            source = new_tree(work, repo_root, "to-archive")
            shutil.copy(os.path.join(repo_root, ATTRIBUTES),
                        os.path.join(source, ATTRIBUTES))
            put_placeholder(repo_root, source, archive, patterns)
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
                     (untagged, "a checkout with no release tag, beside a "
                                "filled-in version file"),
                     (shallow, "a shallow clone")]
            if archived is not None:
                trees.append((archived, "a git archive of a release tag"))
            if carried is not None:
                trees.append((carried, "the release these sources carry"))
            for line, what in odd_lines:
                put_version_file(plain, "# a comment\n%s\n" % line)
                trees.append((plain, "an archive with %s" % what))
                host_release, _ = answers(
                    cmake, os.path.join(plain, os.path.dirname(MODULE)))
                firmware = firmware_version(repo_root, plain)
                if firmware != host_release:
                    fail("an archive with %s: the firmware reports %r and the "
                         "host tools %r; a board and the tools that talk to it "
                         "have to report one version"
                         % (what, firmware, host_release))
            os.remove(os.path.join(plain, VERSION_FILE))
            for tree, what in trees:
                if tree == plain:
                    continue
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
