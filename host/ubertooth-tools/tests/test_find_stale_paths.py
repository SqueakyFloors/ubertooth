#!/usr/bin/env python3
"""A remembered libbtbb, libubertooth or libbluetooth path must be re-checked.

FindBTBB.cmake, FindUBERTOOTH.cmake and FindBLUETOOTH.cmake remember the
header directory and the library they found, so a later configure does not
search again. A package upgrade moves them -- Homebrew puts each version in a
directory of its own -- and an uninstall or a rebuild into another prefix
takes them away, so the remembered paths name files that have gone. Taking
them on trust reported the library found and left the build to fail on the
missing header. FindBTBB went further: its function check ran against a header
and a library that were not there, so it failed the configure over a libbtbb
that was installed and fine.

Each module is configured against an install of its own in three shapes, with
what it remembers removed each time:

  * the whole directory gone, which is the upgrade;
  * the two files gone and the directories that held them left behind, which
    is an uninstall or a half-cleaned prefix, so what has to be looked for is
    the header itself and not the directory it sat in;
  * the paths handed to the module as ordinary variables rather than cache
    entries, which a find_path() does not search past either.

After each removal the module must report paths that are there, or report
nothing found. libbtbb is installed wherever this test runs, because the tools
need it, so FindBTBB is asked for it with REQUIRED and has to arrive back at
the installed one, function check and all. libubertooth and libbluetooth may
be absent, so an honest "not found" passes for those two.

Usage: test_find_stale_paths.py <cmake> <repository root>
                                <libbtbb include dir> <libbtbb library>
"""

import os
import shutil
import subprocess
import sys
import tempfile

# The project each case configures. Nothing is compiled except FindBTBB's own
# function check, so the two modules that have no check get a project with no
# languages in it and cost a fraction of a second each.
#
# NORMAL_INCLUDE_DIR hands the module a path as an ordinary variable rather
# than a cache entry, which is the other way a stale path reaches it: a
# find_path() whose result variable already holds a path does not search, so
# the module has to drop both kinds.
PROJECT = """\
cmake_minimum_required(VERSION 3.5...3.27)
project(find_stale_paths_test %(language)s)
set(CMAKE_MODULE_PATH [[%(modules)s]])
if(NORMAL_INCLUDE_DIR)
  set(%(prefix)s_INCLUDE_DIR "${NORMAL_INCLUDE_DIR}")
  set(%(prefix)s_LIBRARIES "${NORMAL_LIBRARY}")
endif()
find_package(%(package)s %(required)s)
file(WRITE "${CMAKE_BINARY_DIR}/found.txt" "${%(prefix)s_FOUND}")
file(WRITE "${CMAKE_BINARY_DIR}/include-dir.txt" "${%(prefix)s_INCLUDE_DIR}")
file(WRITE "${CMAKE_BINARY_DIR}/library.txt" "${%(prefix)s_LIBRARIES}")
"""

# The three modules: the find_package() name, the variable prefix they report
# through, and the header they are known by.
#
# libbtbb's install is a copy of the real one, because FindBTBB's function
# check has to pass on the first configure of each case; for the other two an
# empty file is enough, since nothing reads or links them.
MODULES = (
    {"package": "BTBB", "prefix": "LIBBTBB", "header": "btbb.h",
     "library": "libbtbb", "required": "REQUIRED", "language": "C"},
    {"package": "UBERTOOTH", "prefix": "LIBUBERTOOTH",
     "header": "ubertooth.h", "library": "libubertooth",
     "required": "", "language": "NONE"},
    {"package": "BLUETOOTH", "prefix": "LIBBLUETOOTH", "header": "hci.h",
     "library": "libbluetooth", "required": "", "language": "NONE"},
)


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def configure(cmake, source, build, defines):
    """Configure the project, returning (status, output on one line)."""
    done = subprocess.run([cmake, "-S", source, "-B", build] + defines,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    # CMake wraps and indents a message(FATAL_ERROR), so what is reported here
    # is one line.
    return done.returncode, " ".join(done.stdout.split())


def reported(build):
    """What the module reported: (found, include directory, library list)."""
    answers = []
    for name in ("found.txt", "include-dir.txt", "library.txt"):
        with open(os.path.join(build, name), encoding="utf-8") as handle:
            answers.append(handle.read().strip())
    return answers[0] == "TRUE", answers[1], answers[2]


def install_paths(module, root, real):
    """Where make_install() puts the header directory and the library.

    libbtbb's copy keeps the real library's file name, so the linker sees the
    extension it expects for this platform.
    """
    if module["package"] == "BTBB":
        name = os.path.basename(real[1][0])
    else:
        name = module["library"] + ".so"
    return os.path.join(root, "include"), os.path.join(root, "lib", name)


def make_install(module, root, real):
    """A header and a library under root, standing for the install a package
    upgrade is about to replace.

    real is the libbtbb this build found, as (include directory, libraries);
    libbtbb's header and library are copied from it so FindBTBB's function
    check passes against the copy. The other two modules check nothing, so an
    empty file of each stands in.
    """
    include, library = install_paths(module, root, real)
    os.makedirs(include)
    os.makedirs(os.path.dirname(library))
    if module["package"] == "BTBB":
        shutil.copy(os.path.join(real[0], module["header"]), include)
        # The first entry: LIBBTBB_LIBRARIES holds one library, and a second
        # would have to be copied under its own name to be linked.
        shutil.copy(real[1][0], library)
    else:
        for path in (os.path.join(include, module["header"]), library):
            open(path, "w", encoding="utf-8").close()
    return include, library


def check_recovered(module, build, case, forbidden):
    """The module must report an install that is there, not one that has gone.

    forbidden are paths that have been removed: neither the include directory
    nor a library may be one of them or sit under one. A module that reports
    nothing found passes, which is the honest answer where the library is not
    installed at all; what it may not do is report a path that has gone.
    """
    where = "%s: %s" % (module["package"], case)
    found, include, libraries = reported(build)
    entries = [include] + libraries.split(";")
    gone = [entry for entry in entries if entry
            for root in forbidden
            if entry == root or entry.startswith(root + os.sep)]
    if gone:
        fail("%s: Find%s.cmake still reports %s, which has been removed"
             % (where, module["package"], ", ".join(gone)))
    if not found:
        return
    if not os.path.exists(os.path.join(include, module["header"])):
        fail("%s: Find%s.cmake reports %s found with its header in %r, where "
             "there is no %s" % (where, module["package"], module["prefix"],
                                 include, module["header"]))
    for entry in libraries.split(";"):
        if entry and os.path.isabs(entry) and not os.path.exists(entry):
            fail("%s: Find%s.cmake reports the library as %r, which is not "
                 "there" % (where, module["package"], entry))


def check_module(cmake, work, modules, module, real):
    """Run the three stale-path shapes against one module."""
    package = module["package"]
    source = os.path.join(work, package.lower())
    os.mkdir(source)
    with open(os.path.join(source, "CMakeLists.txt"), "w",
              encoding="utf-8") as handle:
        handle.write(PROJECT % dict(module, modules=modules))

    def configure_or_fail(build, defines, case, where):
        status, output = configure(cmake, source, build, defines)
        if status != 0:
            fail("%s: %s: configuring %s failed, so the module used a path it "
                 "remembered instead of searching for %s again:\n%s"
                 % (package, case, where, module["prefix"], output))

    def take_install(tag, case):
        """Configure against an install of this module's own, and check the
        module took it: a case whose first configure found something else
        would show nothing when the install is removed."""
        root = os.path.join(work, "%s-%s" % (package.lower(), tag))
        include, library = make_install(module, root, real)
        build = os.path.join(work, "%s-%s-build" % (package.lower(), tag))
        configure_or_fail(build, ["-D%s_INCLUDE_DIR=%s"
                                  % (module["prefix"], include),
                                  "-D%s_LIBRARIES=%s"
                                  % (module["prefix"], library)],
                          case, "against an install of its own")
        if reported(build)[1] != include:
            fail("%s: %s: Find%s.cmake did not take the install it was given "
                 "in %s but reported %s, so this case shows nothing"
                 % (package, case, package, include, reported(build)[1]))
        return root, include, library, build

    # The upgrade: the directory the last configure found the library in has
    # gone.
    case = "the whole directory removed"
    root, _, _, build = take_install("upgraded", case)
    shutil.rmtree(root)
    configure_or_fail(build, [], case, "again after the removal")
    check_recovered(module, build, case, [root])

    # The two files gone and the directories that held them left behind.
    case = "the files removed, their directories left"
    _, include, library, build = take_install("files-removed", case)
    header = os.path.join(include, module["header"])
    os.remove(header)
    os.remove(library)
    configure_or_fail(build, [], case, "again after the removal")
    check_recovered(module, build, case, [header, library])

    # The same paths as ordinary variables rather than cache entries.
    case = "paths given as ordinary variables"
    root = os.path.join(work, "%s-never-there" % package.lower())
    include, library = install_paths(module, root, real)
    build = os.path.join(work, "%s-ordinary-variables" % package.lower())
    configure_or_fail(build, ["-DNORMAL_INCLUDE_DIR=" + include,
                              "-DNORMAL_LIBRARY=" + library],
                      case, "with the paths set as ordinary variables")
    check_recovered(module, build, case, [root])


def main():
    if len(sys.argv) != 5:
        fail("usage: test_find_stale_paths.py <cmake> <repository root> "
             "<libbtbb include dir> <libbtbb library>")
    cmake, repo_root, btbb_include, btbb_libraries = sys.argv[1:]
    real = (btbb_include, [entry for entry in btbb_libraries.split(";")
                           if entry])
    if not real[1]:
        fail("no libbtbb library was passed, so FindBTBB cannot be configured "
             "against a copy of the one this build uses")
    modules = os.path.join(repo_root, "host", "cmake", "modules")

    with tempfile.TemporaryDirectory() as work:
        for module in MODULES:
            check_module(cmake, work, modules, module, real)

    print("PASS")


if __name__ == "__main__":
    main()
