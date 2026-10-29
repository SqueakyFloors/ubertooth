#!/usr/bin/env python3
"""The configure step must refuse a libbtbb that is missing what the tools use.

libbtbb carries no version number a build can read, so FindBTBB.cmake asks the
installed one for the functions this tree calls and the macros it reads.
Without that check, a libbtbb older than the pinned commit got as far as the
compiler and failed with "call to undeclared function" in the middle of
libubertooth, naming neither libbtbb nor the commit to rebuild it at.

This runs the real FindBTBB.cmake from a one-line project, against:

  * the libbtbb this build found, where it must configure and must have run
    the probe, so a check that stopped checking fails here;
  * a header with two of those functions renamed out of it, which is what an
    older libbtbb looks like, where it must fail naming both functions and
    the pinned commit;
  * a header with one of those macros renamed out of it and every function
    left in place, where it must fail naming the macro;
  * this build's own header and a library that defines none of those
    functions, at -O2 and again at -O2 -flto, where it must also fail. That
    case is the library half of the probe rather than the header half, and
    optimisation is what can erase it: a table of function addresses inside
    main() is folded away at -O2, and one at file scope is dropped under
    -flto, either way leaving a program that links against anything.

Usage: test_btbb_check.py <cmake> <repository root> <include dir> <library>
"""

import os
import re
import subprocess
import sys
import tempfile

# The probe's result variable, so a configure that no longer runs it is told
# apart from one that ran it and passed.
PROBE = "LIBBTBB_HAS_REQUIRED_FUNCTIONS"


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def configure(cmake, source, build, include_dir, library, c_flags=None):
    """Configure the test project, returning (status, output on one line)."""
    command = [cmake, "-S", source, "-B", build,
               "-DLIBBTBB_INCLUDE_DIR=%s" % include_dir,
               "-DLIBBTBB_LIBRARIES=%s" % library]
    if c_flags is not None:
        command.append("-DCMAKE_C_FLAGS=%s" % c_flags)
    done = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # CMake wraps and indents a message(FATAL_ERROR), so assertions on its
    # wording compare against one line.
    return done.returncode, " ".join(done.stdout.split())


def required_functions(module):
    """The names in LIBBTBB_REQUIRED_FUNCTIONS, in the order they are written."""
    with open(module, encoding="utf-8") as handle:
        text = handle.read()
    block = re.search(r"^set\(LIBBTBB_REQUIRED_FUNCTIONS\n(.*?)^\)$",
                      text, re.S | re.M)
    if not block:
        fail("no set(LIBBTBB_REQUIRED_FUNCTIONS ...) block in %s" % module)
    names = re.findall(r"^  ([a-z_][a-z0-9_]*)$", block.group(1), re.M)
    if len(names) < 2:
        fail("%s lists %d functions; this test needs at least two"
             % (module, len(names)))
    return names


def required_macros(module):
    """The names in LIBBTBB_REQUIRED_MACROS, in the order they are written."""
    with open(module, encoding="utf-8") as handle:
        text = handle.read()
    block = re.search(r"^set\(LIBBTBB_REQUIRED_MACROS\n(.*?)^\)$",
                      text, re.S | re.M)
    if not block:
        fail("no set(LIBBTBB_REQUIRED_MACROS ...) block in %s" % module)
    names = re.findall(r"^  ([A-Z_][A-Z0-9_]*)$", block.group(1), re.M)
    if not names:
        fail("%s lists no macros" % module)
    return names


def pinned_ref(repo_root):
    """The pinned libbtbb commit, or None where the workflow file is absent.

    Read from .github/workflows/build.yml rather than from FindBTBB.cmake, so
    the commit the error message offers is checked against the pin itself and
    not against its own copy of it.
    """
    build_yml = os.path.join(repo_root, ".github", "workflows", "build.yml")
    if not os.path.exists(build_yml):
        return None
    with open(build_yml, encoding="utf-8") as handle:
        found = re.search(r"^  LIBBTBB_REF: *(\S+)$", handle.read(), re.M)
    if not found:
        fail("%s does not set LIBBTBB_REF" % build_yml)
    return found.group(1)


def fake_header(real_include_dir, work, hidden, where="old-include"):
    """A copy of btbb.h with the named functions and macros renamed out of it.

    Renaming rather than deleting: a declaration can span lines, and a rename
    leaves a header that still compiles, which is what an older libbtbb is --
    a working header without these names in it.
    """
    include_dir = os.path.join(work, where)
    os.mkdir(include_dir)
    with open(os.path.join(real_include_dir, "btbb.h"),
              encoding="utf-8", errors="replace") as handle:
        header = handle.read()
    for name in hidden:
        header, count = re.subn(r"\b%s\b" % re.escape(name),
                                "%s_renamed" % name, header)
        if not count:
            fail("btbb.h in %s does not have %s in it, so the libbtbb this "
                 "build found is not the one the check is written against"
                 % (real_include_dir, name))
    with open(os.path.join(include_dir, "btbb.h"), "w",
              encoding="utf-8") as handle:
        handle.write(header)
    return include_dir


def empty_library(cmake, work):
    """A library with no symbols in it at all, built the way CMake would.

    Stands in for an installed libbtbb whose header declares the functions and
    whose library does not define them -- a header and a library from
    different installs, or a stale library beside a new header. Built rather
    than faked, so it is a real archive for this platform and compiler.
    """
    source = os.path.join(work, "empty")
    os.mkdir(source)
    with open(os.path.join(source, "empty.c"), "w", encoding="utf-8") as handle:
        handle.write("/* deliberately defines nothing */\n"
                     "int btbb_empty_library_marker;\n")
    with open(os.path.join(source, "CMakeLists.txt"), "w",
              encoding="utf-8") as handle:
        handle.write("cmake_minimum_required(VERSION 3.5...3.27)\n"
                     "project(btbb_empty C)\n"
                     "add_library(btbb_empty STATIC empty.c)\n")
    build = os.path.join(work, "empty-build")
    for command in ([cmake, "-S", source, "-B", build],
                    [cmake, "--build", build]):
        done = subprocess.run(command, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)
        if done.returncode != 0:
            fail("could not build the empty library:\n%s" % done.stdout)
    for root, _, names in os.walk(build):
        for name in names:
            if name.startswith("libbtbb_empty.") or name == "btbb_empty.lib":
                return os.path.join(root, name)
    fail("the empty library was built but not found under %s" % build)


def main():
    if len(sys.argv) != 5:
        fail("usage: test_btbb_check.py <cmake> <repository root> "
             "<include dir> <library>")
    cmake, repo_root, include_dir, library = sys.argv[1:]
    module = os.path.join(repo_root, "host", "cmake", "modules",
                          "FindBTBB.cmake")
    functions = required_functions(module)
    macros = required_macros(module)
    # The two ends of the list, so the test is not tied to any one function
    # and covers both prefixes it is sorted into.
    hidden = [functions[0], functions[-1]]

    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "project")
        os.mkdir(source)
        with open(os.path.join(source, "CMakeLists.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write(
                "cmake_minimum_required(VERSION 3.5...3.27)\n"
                "project(btbb_check_test C)\n"
                "set(CMAKE_MODULE_PATH \"%s\")\n"
                "find_package(BTBB REQUIRED)\n"
                % os.path.join(repo_root, "host", "cmake", "modules"))

        status, output = configure(cmake, source, os.path.join(work, "good"),
                                   include_dir, library)
        if status != 0:
            fail("the libbtbb this build uses was refused:\n%s" % output)
        if PROBE not in output:
            fail("configuring against the libbtbb this build uses never ran "
                 "the probe, so nothing here is checked:\n%s" % output)

        old_include = fake_header(include_dir, work, hidden)
        status, output = configure(cmake, source, os.path.join(work, "old"),
                                   old_include, library)
        if status == 0:
            fail("a libbtbb missing %s was accepted:\n%s"
                 % (" and ".join(hidden), output))
        for name in hidden:
            if name not in output:
                fail("the refusal does not say %s is missing:\n%s"
                     % (name, output))
        if old_include not in output:
            fail("the refusal does not say which libbtbb was found:\n%s"
                 % output)
        ref = pinned_ref(repo_root)
        if ref is None:
            print("no .github/workflows/build.yml here; not checking that the "
                  "refusal names the pinned commit")
        elif ref not in output:
            fail("the refusal does not name the pinned libbtbb commit %s, "
                 "which is what the reader has to check out:\n%s"
                 % (ref, output))

        # A missing macro on its own, with every function still there. The
        # tools act on what two of libbtbb's functions report through named
        # values, so a libbtbb without them is as unusable as one missing a
        # function -- and the probe cannot take the address of a macro, so
        # this is the half of it that a compile-time check has to catch.
        macro_include = fake_header(include_dir, work, [macros[0]],
                                    where="no-macro-include")
        status, output = configure(cmake, source,
                                   os.path.join(work, "no-macro"),
                                   macro_include, library)
        if status == 0:
            fail("a libbtbb without %s was accepted:\n%s"
                 % (macros[0], output))
        if macros[0] not in output:
            fail("the refusal does not say %s is missing:\n%s"
                 % (macros[0], output))

        # The library half, under the two optimisations that can erase it. The
        # header is this build's, so every function is declared and only the
        # definitions are absent. -flto is the harder of the two: it is what
        # drops a probe the compiler alone would have kept.
        empty = empty_library(cmake, work)
        for index, flags in enumerate(("-O2", "-O2 -flto")):
            if "-flto" in flags:
                # Against the good libbtbb first, so a compiler or linker that
                # cannot do link-time optimisation is told apart from a probe
                # that let the empty library through.
                status, output = configure(
                    cmake, source, os.path.join(work, "lto-support"),
                    include_dir, library, c_flags=flags)
                if status != 0:
                    print("this toolchain cannot build with %s; not checking "
                          "the probe under it" % flags)
                    continue
            status, output = configure(
                cmake, source, os.path.join(work, "empty-probe-%d" % index),
                include_dir, empty, c_flags=flags)
            if status == 0:
                fail("a library defining none of these functions was accepted "
                     "with %s, so the probe is passing on the header alone:"
                     "\n%s" % (flags, output))
            if "does not build" not in output:
                fail("the refusal with %s does not say the program failed to "
                     "link, so it was refused for the wrong reason:\n%s"
                     % (flags, output))

    print("PASS")


if __name__ == "__main__":
    main()
