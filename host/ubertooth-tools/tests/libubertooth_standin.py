"""A stand-in libubertooth for the tests that configure FindUBERTOOTH.cmake.

The module asks the libubertooth it finds for a few of the functions the host
tools call, so a test that points it at an install of its own has to give it
something those functions are in. This builds one: a header that declares them
and a library that defines them, from the names the module itself lists, so
neither test can fall behind the list.

Which names go where is the caller's to choose, because that is what the cases
in test_ubertooth_check.py are: a header with a function missing is what an
older libubertooth looks like, and a complete header over a library without
the symbols is the other half of the probe.

Used by test_ubertooth_check.py and test_find_stale_paths.py; not a test
itself, so ctest does not run it.
"""

import glob
import os
import re
import subprocess
import sys


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def required_functions(modules):
    """The names in LIBUBERTOOTH_REQUIRED_FUNCTIONS, as the module lists them.

    modules is the directory holding FindUBERTOOTH.cmake.
    """
    path = os.path.join(modules, "FindUBERTOOTH.cmake")
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    block = re.search(r"^set\(LIBUBERTOOTH_REQUIRED_FUNCTIONS\n(.*?)^\)$",
                      text, re.S | re.M)
    if not block:
        fail("no set(LIBUBERTOOTH_REQUIRED_FUNCTIONS ...) block in %s" % path)
    # However far the names are indented, so re-indenting the block in the
    # module cannot quietly drop one from a stand-in and leave a test failing
    # for a reason that has nothing to do with it.
    names = re.findall(r"^[ \t]+([a-z_][a-z0-9_]*)", block.group(1), re.M)
    if len(names) < 2:
        fail("%s lists %d functions; the tests that use this need at least "
             "two" % (path, len(names)))
    return names


def build(cmake, work, where, declare, define):
    """A header directory and a library under work/where.

    declare are the functions the header declares and define are the ones the
    library defines; either may be shorter than the module's list, which is
    what makes an install the module has to refuse.

    Returns (header directory, library).
    """
    source = os.path.join(work, where)
    build_dir = os.path.join(work, where + "-build")
    os.makedirs(source)
    with open(os.path.join(source, "ubertooth.h"), "w",
              encoding="utf-8") as handle:
        handle.write("/* Stands in for libubertooth's header. */\n")
        for name in declare:
            handle.write("int %s(void);\n" % name)
    with open(os.path.join(source, "standin.c"), "w",
              encoding="utf-8") as handle:
        # Not the stand-in header: a function the header does not declare
        # still has to be defined where the case asks for it.
        for name in define:
            handle.write("int %s(void);\n" % name)
            handle.write("int %s(void) { return 0; }\n" % name)
        # A library with nothing in it at all is not something every linker
        # will make, so there is always one symbol.
        handle.write("int ubertooth_standin_present(void) { return 0; }\n")
    with open(os.path.join(source, "CMakeLists.txt"), "w",
              encoding="utf-8") as handle:
        handle.write("cmake_minimum_required(VERSION 3.5...3.27)\n"
                     "project(libubertooth_standin C)\n"
                     "add_library(ubertooth SHARED standin.c)\n")
    for command, what in (([cmake, "-S", source, "-B", build_dir],
                           "configure"),
                          ([cmake, "--build", build_dir], "build")):
        done = subprocess.run(command, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)
        if done.returncode != 0:
            fail("could not %s the libubertooth stand-in in %s:\n%s"
                 % (what, source, done.stdout))
    # Under the build directory rather than in it: a multi-config generator
    # puts the library in a subdirectory named after the configuration.
    built = [path for path in glob.glob(os.path.join(build_dir, "**",
                                                     "libubertooth.*"),
                                        recursive=True)
             if not path.endswith(".h")]
    if len(built) != 1:
        fail("the libubertooth stand-in in %s built %s, expected one library"
             % (source, sorted(built) or "nothing"))
    return source, built[0]
