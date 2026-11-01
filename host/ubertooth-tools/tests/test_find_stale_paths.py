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

FindUBERTOOTH has a function check of its own, so the libubertooth each of its
cases is pointed at is a stand-in built here: a header declaring the functions
the module asks for and a library defining them, which is enough for the probe
to pass and keeps this test to the paths it is about. What the search then
arrives at depends on the machine, and only one of the three answers is the
full check: where this tree's libubertooth is installed the module finds it
and reports it, which is the case the host-installed-libubertooth CI job
exercises; where none is installed it reports nothing found; and where an
older one is installed it finds and refuses that, and what is checked is that
its refusal names no path that has gone.

A fourth shape is the move itself, in check_move() below: the install goes
from one prefix to another, both named by nothing but their pkg-config files,
and the module has to report the second.

Usage: test_find_stale_paths.py <cmake> <repository root>
                                <libbtbb include dir> <libbtbb library>
"""

import os
import shutil
import subprocess
import sys
import tempfile

import libubertooth_standin

# The project each case configures. Nothing is compiled except the function
# checks of FindBTBB and FindUBERTOOTH, so FindBLUETOOTH, which has none, gets
# a project with no languages in it and costs a fraction of a second a case.
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
# libbtbb's install is a copy of the real one and libubertooth's is the
# stand-in built below, because the function checks of those two modules have
# to pass on the first configure of each case; FindBLUETOOTH checks nothing,
# so an empty file stands in for it.
#
# "module" is the pkg-config module each one asks for, used by the move case
# below: FindBLUETOOTH asks for bluez rather than for a name of its own.
MODULES = (
    {"package": "BTBB", "prefix": "LIBBTBB", "header": "btbb.h",
     "library": "libbtbb", "module": "libbtbb", "required": "REQUIRED",
     "language": "C"},
    {"package": "UBERTOOTH", "prefix": "LIBUBERTOOTH",
     "header": "ubertooth.h", "library": "libubertooth",
     "module": "libubertooth", "required": "", "language": "C"},
    {"package": "BLUETOOTH", "prefix": "LIBBLUETOOTH", "header": "hci.h",
     "library": "libbluetooth", "module": "bluez", "required": "",
     "language": "NONE"},
)

# A pkg-config file for the install make_install() has just written, so the
# move case can put an install somewhere the modules' own directory lists do
# not name and have them find it anyway.
PC_FILE = """\
prefix=%(root)s
exec_prefix=${prefix}
includedir=${prefix}/include
libdir=${prefix}/lib

Name: %(module)s
Description: stands in for %(module)s while the modules are tested
Version: 1.0
Cflags: -I${includedir}
Libs: -L${libdir} -l%(link)s
"""


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def refused_as_old(output):
    """Whether a configure failed on FindUBERTOOTH's function check.

    The module asks the libubertooth it found for the functions these tools
    call, so on a machine with an older one installed a search that reaches
    outside this test's own prefixes ends in that refusal. The probe's result
    variable says so: it is in the output whichever way the probe failed.
    """
    return "LIBUBERTOOTH_HAS_REQUIRED_FUNCTIONS - Failed" in output


def configure(cmake, source, build, defines, pkg_config_path=None):
    """Configure the project, returning (status, output on one line)."""
    environment = dict(os.environ)
    if pkg_config_path is not None:
        environment["PKG_CONFIG_PATH"] = pkg_config_path
    done = subprocess.run([cmake, "-S", source, "-B", build] + defines,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True,
                          env=environment)
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


def install_paths(module, root, material):
    """Where make_install() puts the header directory and the library.

    A copy keeps the file name of the library it was copied from, so the
    linker sees the extension it expects for this platform.
    """
    source = material.get(module["package"])
    if source:
        name = os.path.basename(source[1])
    else:
        name = module["library"] + ".so"
    return os.path.join(root, "include"), os.path.join(root, "lib", name)


def make_install(module, root, material):
    """A header and a library under root, standing for the install a package
    upgrade is about to replace.

    material says what each module's install is copied from, as
    (header directory, library): the libbtbb this build found for FindBTBB,
    and the stand-in built by standin_libubertooth() for FindUBERTOOTH, so
    that the function check of each passes against the copy. FindBLUETOOTH
    checks nothing and has no entry, so an empty file of each stands in.
    """
    include, library = install_paths(module, root, material)
    os.makedirs(include)
    os.makedirs(os.path.dirname(library))
    source = material.get(module["package"])
    if source:
        shutil.copy(os.path.join(source[0], module["header"]), include)
        shutil.copy(source[1], library)
    else:
        for path in (os.path.join(include, module["header"]), library):
            open(path, "w", encoding="utf-8").close()
    pkgconfig = os.path.join(root, "lib", "pkgconfig")
    os.makedirs(pkgconfig)
    with open(os.path.join(pkgconfig, module["module"] + ".pc"), "w",
              encoding="utf-8") as handle:
        handle.write(PC_FILE % {"root": root, "module": module["module"],
                                "link": module["library"][len("lib"):]})
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


def check_module(cmake, work, modules, module, material):
    """Run the three stale-path shapes against one module."""
    package = module["package"]
    source = os.path.join(work, package.lower())
    os.mkdir(source)
    with open(os.path.join(source, "CMakeLists.txt"), "w",
              encoding="utf-8") as handle:
        handle.write(PROJECT % dict(module, modules=modules))

    def configure_or_fail(build, defines, case, where, pkg_config_path=None):
        status, output = configure(cmake, source, build, defines,
                                   pkg_config_path)
        if status != 0:
            fail("%s: %s: configuring %s failed, so the module used a path it "
                 "remembered instead of searching for %s again:\n%s"
                 % (package, case, where, module["prefix"], output))

    def searched_again(build, defines, case, where, forbidden):
        """Configure after the install was removed, and say whether the module
        reported an answer to check.

        The module has to look again rather than hand back what it remembered.
        It can also look again, find a real install of the library on this
        machine, and refuse that one: FindUBERTOOTH asks the libubertooth it
        finds for the functions these tools call, and a developer machine can
        have an older one installed. That is the module searching, which is
        what this case is about, so what is checked then is that the refusal
        does not name a path that has gone; there is nothing else to read,
        because a refused configure writes no answer.
        """
        status, output = configure(cmake, source, build, defines)
        if status == 0:
            return True
        if not refused_as_old(output):
            fail("%s: %s: configuring %s failed, so the module used a path it "
                 "remembered instead of searching for %s again:\n%s"
                 % (package, case, where, module["prefix"], output))
        # The refusal itself, not the whole output: the status lines above it
        # name the paths that have gone, which is the module saying it is
        # searching for them again. The whole output where the error cannot
        # be found, rather than the tail end of it, so a message this does
        # not recognize is examined instead of skipped.
        at = output.find("CMake Error")
        refusal = output[at:] if at >= 0 else output
        named = [path for path in forbidden if path in refusal]
        if named:
            fail("%s: %s: Find%s.cmake refused a library at %s, which has "
                 "been removed, so it did not search again:\n%s"
                 % (package, case, package, ", ".join(named), output))
        print("%s: %s: the module searched again and refused the library "
              "installed on this machine, which is older than these tools; "
              "the paths it remembered are gone from its message"
              % (package, case))
        return False

    def take_install(tag, case):
        """Configure against an install of this module's own, and check the
        module took it: a case whose first configure found something else
        would show nothing when the install is removed."""
        root = os.path.join(work, "%s-%s" % (package.lower(), tag))
        include, library = make_install(module, root, material)
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
    if searched_again(build, [], case, "again after the removal", [root]):
        check_recovered(module, build, case, [root])

    # The two files gone and the directories that held them left behind.
    case = "the files removed, their directories left"
    _, include, library, build = take_install("files-removed", case)
    header = os.path.join(include, module["header"])
    os.remove(header)
    os.remove(library)
    if searched_again(build, [], case, "again after the removal",
                      [header, library]):
        check_recovered(module, build, case, [header, library])

    # The same paths as ordinary variables rather than cache entries.
    case = "paths given as ordinary variables"
    root = os.path.join(work, "%s-never-there" % package.lower())
    include, library = install_paths(module, root, material)
    build = os.path.join(work, "%s-ordinary-variables" % package.lower())
    if searched_again(build, ["-DNORMAL_INCLUDE_DIR=" + include,
                              "-DNORMAL_LIBRARY=" + library],
                      case, "with the paths set as ordinary variables",
                      [root]):
        check_recovered(module, build, case, [root])

    check_move(cmake, work, module, material, source, configure_or_fail)


def pkgconfig_dir(root):
    """Where make_install() put the pkg-config file under root."""
    return os.path.join(root, "lib", "pkgconfig")


def check_move(cmake, work, module, material, source, configure_or_fail):
    """A library that moves to another prefix must be found where it is now.

    The two prefixes here are named by nothing but their pkg-config files,
    which is the case a package upgrade makes: Homebrew's pkg-config file
    points into the version's own directory under Cellar. pkg-config's own
    answer is remembered in the build directory too, so dropping the paths is
    not enough on its own -- the module has to ask pkg-config again, or it
    reports the library missing although it is installed.

    Skipped where the first configure finds something other than the prefix it
    was pointed at, which is a machine with no pkg-config on it, or where it
    finds an older install of the library and refuses it, which is the same
    machine seen through FindUBERTOOTH's function check.
    """
    package = module["package"]
    case = "moved to another prefix"
    first = os.path.join(work, "%s-prefix-one" % package.lower())
    include, _ = make_install(module, first, material)
    build = os.path.join(work, "%s-moved-build" % package.lower())
    status, output = configure(cmake, source, build, [], pkgconfig_dir(first))
    if status != 0:
        if not refused_as_old(output):
            fail("%s: %s: configuring against an install in a prefix of its "
                 "own failed:\n%s" % (package, case, output))
        print("%s: %s: the module did not take the install in %s but the "
              "library this machine has, which is older than these tools, so "
              "the move is not checked here" % (package, case, first))
        return
    if reported(build)[1] != include:
        print("%s: %s: pkg-config did not lead the module to %s, so the move "
              "is not checked here" % (package, case, include))
        return

    second = os.path.join(work, "%s-prefix-two" % package.lower())
    moved, library = make_install(module, second, material)
    shutil.rmtree(first)
    configure_or_fail(build, [], case, "again after the move",
                      pkgconfig_dir(second))
    found, where, libraries = reported(build)
    if not found or where != moved or library not in libraries.split(";"):
        fail("%s: %s: Find%s.cmake reports %s, %s after the install moved to "
             "%s, where it is now: dropping the paths it remembered is not "
             "enough if pkg-config is not asked again"
             % (package, case, package, where or "nothing",
                libraries or "nothing", second))


def standin_libubertooth(cmake, modules, work):
    """A header and a library standing in for an installed libubertooth.

    FindUBERTOOTH.cmake asks the libubertooth it finds for a few of the
    functions the tools call, so an empty file no longer passes for one in the
    cases above. A stand-in rather than a copy of the real library, because
    nothing here needs libubertooth's own code and the real header would drag
    libbtbb and libusb into every probe. The names come from the module
    itself, through the helper test_ubertooth_check.py builds its own
    stand-ins with.

    Returns (header directory, library).
    """
    names = libubertooth_standin.required_functions(modules)
    return libubertooth_standin.build(cmake, work, "standin", names, names)


def main():
    if len(sys.argv) != 5:
        fail("usage: test_find_stale_paths.py <cmake> <repository root> "
             "<libbtbb include dir> <libbtbb library>")
    cmake, repo_root, btbb_include, btbb_libraries = sys.argv[1:]
    btbb = [entry for entry in btbb_libraries.split(";") if entry]
    if not btbb:
        fail("no libbtbb library was passed, so FindBTBB cannot be configured "
             "against a copy of the one this build uses")
    modules = os.path.join(repo_root, "host", "cmake", "modules")

    with tempfile.TemporaryDirectory() as work:
        # The first entry of LIBBTBB_LIBRARIES: it holds one library, and a
        # second would have to be copied under its own name to be linked.
        material = {"BTBB": (btbb_include, btbb[0]),
                    "UBERTOOTH": standin_libubertooth(cmake, modules, work)}
        for module in MODULES:
            check_module(cmake, work, modules, module, material)

    print("PASS")


if __name__ == "__main__":
    main()
