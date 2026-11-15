#!/usr/bin/env python3
"""install.sh installs the dependencies its own platform calls them by.

Run as: test_install_packages.py <path to install.sh> [REQUIREMENTS.md]

Each run here installs nothing and touches no device. The platform, the
package managers, dpkg-query, the Python interpreter and git are faked, and
--no-sudo prints the privileged steps instead of running them, so what the
script would have installed is read off its standard error. Every run ends at
the faked libbtbb clone, which cannot succeed.

What is checked:

  * a Linux machine with apt-get is installed with apt, and one with dnf and
    no apt-get is installed with dnf, under Fedora's names for the same
    dependencies;
  * a machine with both is treated as the Debian machine it is;
  * a Linux machine with neither is refused, in one "error: " line that names
    both package managers;
  * --with-firmware adds the ARM toolchain for whichever one it is, and
    --update-bootloader one package more;
  * which apt commands a Debian run executes, counted: a machine that has
    every package gets none at all, a machine missing some gets one index
    refresh however many steps want packages, and each install asks for the
    missing packages and no others. --skip-deps asks dpkg nothing, --force
    refreshes nothing, and a dpkg-query that cannot answer sends the whole
    list to apt rather than skipping an install the machine needs;
  * an apt command that fails ends the run, with apt's own words and before
    anything is built;
  * every package name the script asks for appears in REQUIREMENTS.md, so a
    dependency added to the script cannot be left out of the document. The
    other direction is not checked: the search is over the whole file, so a
    name that has gone from the script still has somewhere to match.
"""

import atexit
import os
import re
import shutil
import subprocess
import sys
import tempfile


SCRIPT = sys.argv[1]
SCRIPT_DIR = os.path.dirname(os.path.abspath(SCRIPT))
REQUIREMENTS = sys.argv[2] if len(sys.argv) > 2 else None

# The install commands each platform is expected to print, word for word as
# the script would have run them. The apt lines are what it has always run,
# DEBIAN_FRONTEND and all; the dnf lines are the same dependencies under
# Fedora's names.
APT_PACKAGES = ("cmake build-essential pkg-config libusb-1.0-0-dev"
                " libbluetooth-dev libfftw3-dev python3 python3-venv"
                " python3-usb1 git")
APT_INSTALL = "env DEBIAN_FRONTEND=noninteractive apt-get install -y"
APT_UPDATE = "apt-get update -qq"
APT_DEPENDENCIES = APT_INSTALL + " " + APT_PACKAGES
APT_FIRMWARE_PACKAGES = "gcc-arm-none-eabi libnewlib-arm-none-eabi dfu-util make"
APT_FIRMWARE = APT_INSTALL + " " + APT_FIRMWARE_PACKAGES
DNF_DEPENDENCIES = (
    "dnf install -y cmake gcc gcc-c++ make pkgconf-pkg-config libusb1-devel"
    " bluez-libs-devel fftw-devel python3 python3-pip python3-libusb1 git")
DNF_FIRMWARE = ("dnf install -y arm-none-eabi-gcc-cs arm-none-eabi-newlib"
                " dfu-util make")
# --update-bootloader adds the ARM C++ runtime, and nothing else does: it
# builds bl_update, which carries the bootloader's binary, and the bootloader
# is the one image written in C++.
APT_BOOTLOADER_PACKAGE = "libstdc++-arm-none-eabi-newlib"
APT_BOOTLOADER = APT_FIRMWARE + " " + APT_BOOTLOADER_PACKAGE
DNF_BOOTLOADER = DNF_FIRMWARE + " arm-none-eabi-gcc-cs-c++"

# Faking the platform, the interpreter and git puts a Linux run of the script
# in reach on any machine and ends it before anything is built: the clone
# fails, and the pip installs fail, which is a warning rather than a stop. The
# package managers and dpkg-query are added per case, because which of them
# exists, and what dpkg says, is what each case is about.
FAKES = {
    # The script asks for "uname -s" and nothing else.
    "uname": 'echo Linux',
    "nproc": 'echo 1',
    # The version check passes, pip fails: a warning, not a stop.
    "python3": 'case "$*" in *pip*) exit 1 ;; *) exit 0 ;; esac',
    "git": 'exit 1',
}
PACKAGE_MANAGERS = {
    # Neither is ever run under --no-sudo: the privileged steps are printed
    # instead. They are here to be found by "command -v", which is how the
    # script decides which platform it is on. The run that does let apt
    # execute (the failure case at the end) passes a fake of its own.
    "apt-get": 'exit 0',
    "dnf": 'exit 0',
}

# What dpkg-query answers, in the forms that matter. The script calls it as
# "dpkg-query -W -f=${Status} <package>", so the package is the last argument:
# exit 1 with nothing printed is dpkg's own answer for a name it has never
# heard of, and "install ok installed" is the one status that counts as
# present. Each fake records the package it was asked about in QUERY_LOG, so a
# case can check that every package was asked about exactly once.
QUERY_PREAMBLE = """
for argument in "$@"; do package="$argument"; done
if [ -n "${QUERY_LOG:-}" ]; then echo "$package" >> "$QUERY_LOG"; fi
"""
NOTHING_INSTALLED = QUERY_PREAMBLE + "exit 1"
ALL_INSTALLED = QUERY_PREAMBLE + "printf 'install ok installed'"
# A package name the case names is missing; every other one is installed.
MISSING_ONE = QUERY_PREAMBLE + """
case "$package" in
	%s) exit 1 ;;
	*) printf 'install ok installed' ;;
esac
"""
# Exit 2 is dpkg-query's "something went wrong", as opposed to the 1 that
# means it knows nothing about the package.
QUERY_BROKEN = QUERY_PREAMBLE + "exit 2"
# A package that was removed and left its configuration behind: known to dpkg,
# so the query succeeds, but not a package apt is finished with.
CONFIG_FILES_ONLY = QUERY_PREAMBLE + "printf 'deinstall ok config-files'"


# The system directories the runs below take their real tools from, and the
# names that must not come from them. Each run's PATH is one directory of
# links to those tools, with the names below left out, rather than a list of
# the handful of tools the script uses: a tool it reaches for later would
# otherwise go missing here and fail a case for the wrong reason.
#
# What is left out is what the script looks for to decide what to do, and each
# case says which of those it has. The package managers decide which platform
# it takes itself to be, and on a Debian or Ubuntu machine the real apt-get is
# in one of these directories. dpkg-query is left out for the same reason: the
# real one on a Debian machine would answer about the packages that machine
# happens to have, so which apt commands a case expects would depend on the
# machine running the test. The Ubertooth tools decide whether a
# --with-firmware run builds the firmware before it installs anything: a
# machine with the distribution's ubertooth package would send those cases
# down that path, where they would build firmware inside the checkout and
# never reach the dependency step they are about.
SYSTEM_DIRECTORIES = ("/usr/bin", "/bin", "/usr/sbin", "/sbin")
HIDDEN = ("apt", "apt-get", "dnf", "dnf5", "microdnf", "yum", "zypper",
          "pacman", "brew", "dpkg", "dpkg-query",
          "ubertooth-dfu", "ubertooth-util")


def link_system_tools():
    """One directory of links to the system tools, minus the hidden names.

    Made once and shared by every run. It is over a thousand links on a
    normal machine: a quarter of a second each time on an idle one, but much
    more on a loaded one, where building it per run took the test most of the
    way to its timeout.
    """
    farm = tempfile.mkdtemp(prefix="install-sh-tools.")
    for system in SYSTEM_DIRECTORIES:
        if not os.path.isdir(system):
            continue
        for name in os.listdir(system):
            link = os.path.join(farm, name)
            if name in HIDDEN or name in FAKES or name in PACKAGE_MANAGERS \
                    or os.path.lexists(link):
                continue
            os.symlink(os.path.join(system, name), link)
    return farm


TOOLS = link_system_tools()
# Removed however the test ends, a failed case included.
atexit.register(shutil.rmtree, TOOLS, ignore_errors=True)


class Run(object):
    """What one run of the script did."""

    def __init__(self, status, stderr, stdout, queries, apt_log):
        self.status = status
        self.stderr = stderr
        self.stdout = stdout
        # The packages dpkg-query was asked about, in order.
        self.queries = queries
        # What a fake apt-get was really asked to do, in order, for the runs
        # that let the privileged steps execute. Empty for the rest, which
        # print those steps instead: apt_commands() reads them off.
        self.apt_log = apt_log

    def __str__(self):
        return "status %d\n%s" % (self.status, self.stderr)

    def would_run(self):
        """The privileged commands the run printed instead of running."""
        marker = "[would run as root] "
        return [line.split(marker, 1)[1] for line in self.stderr.splitlines()
                if marker in line]

    def apt_commands(self):
        """The apt commands of those, which is what is being counted."""
        return [line for line in self.would_run() if "apt-get" in line]


def run_with_fakes(arguments, package_managers, dpkg_query=NOTHING_INSTALLED,
                   extra_fakes=None, no_sudo=True):
    """Run install.sh with those fakes in place; return a Run.

    dpkg_query is the body of the dpkg-query fake, or None for a machine
    without dpkg-query at all. no_sudo=False drops --no-sudo, so the
    privileged steps really are executed: with a fake sudo and a fake apt-get
    in extra_fakes, that is how a failing apt command is tested.
    """
    directory = tempfile.mkdtemp(prefix="install-sh-packages.")
    try:
        binaries = os.path.join(directory, "bin")
        os.mkdir(binaries)
        query_log = os.path.join(directory, "dpkg-query.log")
        apt_log = os.path.join(directory, "apt.log")
        fakes = dict(FAKES)
        for name in package_managers:
            fakes[name] = PACKAGE_MANAGERS[name]
        if dpkg_query is not None:
            fakes["dpkg-query"] = dpkg_query
        fakes.update(extra_fakes or {})
        for name, body in fakes.items():
            path = os.path.join(binaries, name)
            with open(path, "w") as fake:
                fake.write("#!/bin/sh\n%s\n" % body)
            os.chmod(path, 0o755)
        # The fakes, then the system tools, and nothing else: a package
        # manager or an Ubertooth install on this machine must not be
        # reachable behind them.
        environment = dict(os.environ)
        environment["PATH"] = os.pathsep.join([binaries, TOOLS])
        environment["QUERY_LOG"] = query_log
        environment["APT_LOG"] = apt_log
        command = ["bash", SCRIPT,
                   "--prefix", os.path.join(directory, "prefix")]
        if no_sudo:
            command.append("--no-sudo")
        # The script is bash (its shebang, "set -o pipefail"), so it is run as
        # bash rather than through /bin/sh, which is dash on Debian and Ubuntu.
        completed = subprocess.run(
            command + list(arguments),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True, timeout=60,
            env=environment, cwd=SCRIPT_DIR)
        queries = []
        if os.path.exists(query_log):
            with open(query_log) as log:
                queries = log.read().split()
        executed = []
        if os.path.exists(apt_log):
            with open(apt_log) as log:
                executed = log.read().splitlines()
        return Run(completed.returncode, completed.stderr, completed.stdout,
                   queries, executed)
    finally:
        shutil.rmtree(directory, ignore_errors=True)


# --- which package manager, and the names it is asked for ----------------

# Fedora: dnf, and the package names Fedora uses. The run gets as far as the
# clone, which fails, so the status only says that; the printed command is
# what this case is about.
run = run_with_fakes([], ["dnf"])
assert run.status != 0, run
assert run.stdout == "", run.stdout
assert "==> Installing system dependencies (dnf)" in run.stderr, run
assert DNF_DEPENDENCIES in run.would_run(), run
# Fedora is installed by dnf, which is asked for the whole list every run: the
# package counting below is apt's, and dpkg has no part in a Fedora run.
assert run.queries == [], run.queries

# Debian/Ubuntu: apt-get, and the names it has always used.
run = run_with_fakes([], ["apt-get"])
assert run.status != 0, run
assert run.stdout == "", run.stdout
assert "==> Installing system dependencies (apt)" in run.stderr, run
assert APT_DEPENDENCIES in run.would_run(), run

# A machine with both is a Debian or Ubuntu machine that also has dnf
# installed, and is installed with apt.
run = run_with_fakes([], ["apt-get", "dnf"])
assert run.status != 0, run
assert APT_DEPENDENCIES in run.would_run(), run
assert not [line for line in run.would_run() if line.startswith("dnf ")], run

# Neither: refused in one "error: " line that names both, with nothing on
# standard output, before anything is installed or built.
run = run_with_fakes([], [])
assert run.status == 1, run
assert run.stdout == "", run.stdout
assert run.stderr == ("error: on Linux this script supports Debian/Ubuntu "
                      "(apt-get) and Fedora (dnf) only; see REQUIREMENTS.md "
                      "for other platforms\n"), run

# --with-firmware adds the ARM cross-compiler and dfu-util, named for
# whichever package manager is in use, and installs them before the longer
# dependency step.
for manager, firmware, dependencies in (
        ("dnf", DNF_FIRMWARE, DNF_DEPENDENCIES),
        ("apt-get", APT_FIRMWARE, APT_DEPENDENCIES)):
    run = run_with_fakes(["--with-firmware"], [manager])
    assert run.status != 0, run
    # No Ubertooth tool is reachable (see HIDDEN), so the script installs the
    # toolchain and leaves the firmware to the end, where this run never
    # arrives. A run that did build it would do so inside the checkout, and
    # the two commands this case compares would not both be there to compare.
    # Checked before stdout, which a firmware build would also have written
    # to: this is the message that says what went wrong.
    assert "==> Building firmware" not in run.stderr, (manager, run)
    assert run.stdout == "", run.stdout
    printed = run.would_run()
    assert firmware in printed, (manager, firmware, run)
    assert printed.index(firmware) < printed.index(dependencies), \
        (manager, printed)

# --update-bootloader asks for one package more, the ARM C++ runtime, because
# the bootloader bl_update carries is the one image written in C++. The run
# above must not have asked for it: a firmware build that does not touch the
# bootloader has no use for it, and it is a large package.
for manager, firmware, bootloader in (("dnf", DNF_FIRMWARE, DNF_BOOTLOADER),
                                      ("apt-get", APT_FIRMWARE,
                                       APT_BOOTLOADER)):
    run = run_with_fakes(["--with-firmware"], [manager])
    assert firmware in run.would_run(), (manager, run)
    assert bootloader not in run.would_run(), (manager, run)
    run = run_with_fakes(
        ["--update-bootloader", "--flash-device", "ab" * 16], [manager])
    assert run.status != 0, run
    assert "==> Building firmware" not in run.stderr, (manager, run)
    assert run.stdout == "", run.stdout
    assert bootloader in run.would_run(), (manager, bootloader, run)


# --- how many apt commands a Debian run executes -------------------------

# Nothing installed: one index refresh, then the one install, and that is
# every apt command the run executes. The refresh comes first, because an
# install from indexes that were never fetched can only fail.
run = run_with_fakes([], ["apt-get"])
assert run.apt_commands() == [APT_UPDATE, APT_DEPENDENCIES], run
# Each package was asked about once, and no package outside the list was.
assert run.queries == APT_PACKAGES.split(), run.queries

# Everything installed, which is what a re-run normally is: no apt command at
# all, not even the index refresh, and a line saying so. This is the whole
# point of the change: an index refresh and an install with nothing to do each
# cost seconds of network and disk on a machine that needs neither.
run = run_with_fakes([], ["apt-get"], ALL_INSTALLED)
assert run.apt_commands() == [], run
assert "==> System dependencies already installed (apt); skipping" \
    in run.stderr, run
assert run.queries == APT_PACKAGES.split(), run.queries

# --with-firmware, nothing installed: two steps want packages and there is
# still one refresh, shared between them, with the firmware toolchain
# installed before the longer dependency list.
run = run_with_fakes(["--with-firmware"], ["apt-get"])
assert run.apt_commands() == [APT_UPDATE, APT_FIRMWARE, APT_DEPENDENCIES], run

# --update-bootloader, everything installed: the step that reaches both
# dependency lists executes nothing either, and says so twice.
run = run_with_fakes(["--update-bootloader", "--flash-device", "ab" * 16],
                     ["apt-get"], ALL_INSTALLED)
assert run.apt_commands() == [], run
assert "==> Firmware toolchain already installed (apt); skipping" \
    in run.stderr, run
assert "==> System dependencies already installed (apt); skipping" \
    in run.stderr, run
assert run.queries == (APT_FIRMWARE_PACKAGES.split()
                       + [APT_BOOTLOADER_PACKAGE] + APT_PACKAGES.split()), \
    run.queries

# One package missing out of the firmware toolchain's list, which is the
# --update-bootloader case a machine set up for firmware builds really hits:
# the C++ runtime alone is installed, the rest of that list is not asked for
# again, and the dependency step still executes nothing.
run = run_with_fakes(["--update-bootloader", "--flash-device", "ab" * 16],
                     ["apt-get"], MISSING_ONE % APT_BOOTLOADER_PACKAGE)
assert run.apt_commands() == [
    APT_UPDATE, APT_INSTALL + " " + APT_BOOTLOADER_PACKAGE], run

# One package missing out of the host dependencies: that package is installed
# and the other nine are left alone, so a user who removed one dependency
# does not wait for the whole list to be reconsidered.
run = run_with_fakes([], ["apt-get"], MISSING_ONE % "libfftw3-dev")
assert run.apt_commands() == [APT_UPDATE, APT_INSTALL + " libfftw3-dev"], run
assert "==> Installing system dependencies (apt): libfftw3-dev" \
    in run.stderr, run

# A package removed with its configuration left behind is one apt still has
# work to do on, so it counts as missing however much dpkg still knows about
# it. dpkg-query succeeds for it, which is why the status is read rather than
# the exit code alone.
run = run_with_fakes([], ["apt-get"], CONFIG_FILES_ONLY)
assert run.apt_commands() == [APT_UPDATE, APT_DEPENDENCIES], run

# --skip-deps installs no packages and asks dpkg nothing: the user has said
# the packages are there, and a query would only slow the run down.
run = run_with_fakes(["--skip-deps", "--with-firmware"], ["apt-get"])
assert run.apt_commands() == [], run
assert run.queries == [], run.queries
assert "==> Skipping system dependencies (--skip-deps)" in run.stderr, run

# --force rebuilds this tree; it is not a reason to refresh package indexes.
# A package that is installed is as current as apt can make it, and a user who
# wants newer packages upgrades them with apt itself.
run = run_with_fakes(["--force"], ["apt-get"], ALL_INSTALLED)
assert run.apt_commands() == [], run

# A dpkg-query that cannot answer, and a machine with no dpkg-query at all:
# nothing is known about any package, so the whole list goes to apt with a
# warning. The opposite guess would skip an install the machine needs, which
# is the one mistake this must not make.
for query, warning in (
        (QUERY_BROKEN,
         "warning: dpkg-query could not say which packages are installed;"
         " installing them all"),
        (None,
         "warning: dpkg-query is not here, so which packages are installed is"
         " unknown; installing them all")):
    run = run_with_fakes([], ["apt-get"], query)
    assert run.apt_commands() == [APT_UPDATE, APT_DEPENDENCIES], run
    assert warning in run.stderr, (warning, run)


# --- an apt command that fails -------------------------------------------

# The runs above print the privileged commands instead of running them. These
# two let them run, through a fake sudo, against an apt-get that fails: a
# missing dependency must stop the run and say so in apt's own words, not be
# skipped quietly. Each case is also a check that the run gets no further: the
# Python dependency step is the next thing the script would print.
FAILING_SUDO = {"sudo": 'exec "$@"'}
# Logged so the failure cases count commands as the ones above do: apt really
# runs here, so there is no "[would run as root] " line to read.
APT_LOGGING = """
echo "$*" >> "$APT_LOG"
"""
APT_INSTALL_FAILS = APT_LOGGING + """
case "$1" in
	update) exit 0 ;;
	*) echo "E: Unable to locate package" >&2; exit 100 ;;
esac
"""
APT_UPDATE_FAILS = APT_LOGGING + """
case "$1" in
	update) echo "E: Could not get lists" >&2; exit 100 ;;
	*) exit 0 ;;
esac
"""
for body, message, executed in (
        (APT_INSTALL_FAILS, "E: Unable to locate package",
         ["update -qq", "install -y " + APT_PACKAGES]),
        # A refresh that failed stops the run there: installing from indexes
        # that could not be fetched is the next thing that would go wrong.
        (APT_UPDATE_FAILS, "E: Could not get lists", ["update -qq"])):
    fakes = dict(FAILING_SUDO)
    fakes["apt-get"] = body
    run = run_with_fakes([], ["apt-get"], NOTHING_INSTALLED, fakes,
                         no_sudo=False)
    assert run.status != 0, run
    assert message in run.stderr, (message, run)
    assert run.apt_log == executed, (run.apt_log, run)
    assert "==> Installing Python runtime deps" not in run.stderr, run
    assert run.stdout == "", run.stdout

# And the other way round: with every package installed, an apt-get that fails
# whatever it is asked is never reached, so the run passes the dependency
# steps and stops where every run here stops, at the clone.
fakes = dict(FAILING_SUDO)
fakes["apt-get"] = APT_LOGGING + 'echo "E: apt should not have run" >&2; exit 100'
run = run_with_fakes([], ["apt-get"], ALL_INSTALLED, fakes, no_sudo=False)
assert run.apt_log == [], run.apt_log
assert "E: apt should not have run" not in run.stderr, run
assert "==> Installing Python runtime deps" in run.stderr, run


# --- the document a user installing by hand reads ------------------------

# Every package the script asks for is named in the table a user installing by
# hand reads, so a dependency added here has to be added there too. A name
# left in the table after the script stopped asking for it is not caught: the
# search is over the whole document, prose included.
if REQUIREMENTS:
    with open(REQUIREMENTS) as table:
        listed = table.read()
    for command in (APT_DEPENDENCIES, APT_FIRMWARE, APT_BOOTLOADER,
                    DNF_DEPENDENCIES, DNF_FIRMWARE, DNF_BOOTLOADER):
        # Everything the command installs is what follows its -y.
        for package in command.split(" -y ", 1)[1].split():
            # As a whole name, not as a substring: "make" is inside "cmake"
            # and "gcc" inside "gcc-c++", so a plain search would find a
            # wrong name in the right one's place. A package name's own
            # characters are letters, digits, and ".+-", so a name is this
            # one only where none of those sits beside it.
            found = re.search(r"(?<![\w.+-])%s(?![\w.+-])"
                              % re.escape(package), listed)
            assert found, (package, REQUIREMENTS)

print("install.sh: apt on Debian/Ubuntu, dnf on Fedora, each with the "
      "package names REQUIREMENTS.md lists; neither is refused; and a Debian "
      "run whose packages are all installed executes no apt command")
