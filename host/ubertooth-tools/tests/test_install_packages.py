#!/usr/bin/env python3
"""install.sh installs the dependencies its own platform calls them by.

Run as: test_install_packages.py <path to install.sh> [REQUIREMENTS.md]

Each run here installs nothing and touches no device. The platform, the
package managers, the Python interpreter and git are faked, and --no-sudo
prints the privileged steps instead of running them, so what the script would
have installed is read off its standard error. Every run ends at the faked
libbtbb clone, which cannot succeed.

What is checked:

  * a Linux machine with apt-get is installed with apt, and one with dnf and
    no apt-get is installed with dnf, under Fedora's names for the same
    dependencies;
  * a machine with both is treated as the Debian machine it is;
  * a Linux machine with neither is refused, in one "error: " line that names
    both package managers;
  * --with-firmware adds the ARM toolchain for whichever one it is;
  * the package names the script asks for are the ones REQUIREMENTS.md lists,
    so the table and the script cannot drift apart.
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
APT_DEPENDENCIES = (
    "env DEBIAN_FRONTEND=noninteractive apt-get install -y"
    " cmake build-essential pkg-config libusb-1.0-0-dev"
    " libbluetooth-dev libfftw3-dev python3 python3-venv python3-usb1 git")
APT_FIRMWARE = (
    "env DEBIAN_FRONTEND=noninteractive apt-get install -y"
    " gcc-arm-none-eabi libnewlib-arm-none-eabi dfu-util make")
DNF_DEPENDENCIES = (
    "dnf install -y cmake gcc gcc-c++ make pkgconf-pkg-config libusb1-devel"
    " bluez-libs-devel fftw-devel python3 python3-pip python3-libusb1 git")
DNF_FIRMWARE = ("dnf install -y arm-none-eabi-gcc-cs arm-none-eabi-newlib"
                " dfu-util make")

# Faking the platform, the interpreter and git puts a Linux run of the script
# in reach on any machine and ends it before anything is built: the clone
# fails, and the pip installs fail, which is a warning rather than a stop. The
# package managers are added per case, because which of them exists is what
# each case is about.
FAKES = {
    # The script asks for "uname -s" and nothing else.
    "uname": 'echo Linux',
    "nproc": 'echo 1',
    # The version check passes, pip fails: a warning, not a stop.
    "python3": 'case "$*" in *pip*) exit 1 ;; *) exit 0 ;; esac',
    "git": 'exit 1',
}
PACKAGE_MANAGERS = {
    # Neither is ever run: --no-sudo prints the privileged steps instead. They
    # are here to be found by "command -v", which is how the script decides
    # which platform it is on.
    "apt-get": 'exit 0',
    "dnf": 'exit 0',
}


# The system directories the runs below take their real tools from, and the
# names that must not come from them. Each run's PATH is one directory of
# links to those tools, with the names below left out, rather than a list of
# the handful of tools the script uses: a tool it reaches for later would
# otherwise go missing here and fail a case for the wrong reason.
#
# What is left out is what the script looks for to decide what to do, and each
# case says which of those it has. The package managers decide which platform
# it takes itself to be, and on a Debian or Ubuntu machine the real apt-get is
# in one of these directories. The Ubertooth tools decide whether a
# --with-firmware run flashes before it installs anything: a machine with the
# distribution's ubertooth package would send those cases down the flashing
# path, where they would build firmware inside the checkout and never reach
# the dependency step they are about.
SYSTEM_DIRECTORIES = ("/usr/bin", "/bin", "/usr/sbin", "/sbin")
HIDDEN = ("apt", "apt-get", "dnf", "dnf5", "microdnf", "yum", "zypper",
          "pacman", "brew",
          "ubertooth-dfu", "ubertooth-util")


def link_system_tools():
    """One directory of links to the system tools, minus the hidden names.

    Made once and shared by every run: it is a thousand links on a normal
    machine, and rebuilding it per run cost more than the runs themselves.
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


def run_with_fakes(arguments, package_managers):
    """Run install.sh with those package managers present; return it.

    Returns (exit status, standard error, standard output).
    """
    directory = tempfile.mkdtemp(prefix="install-sh-packages.")
    try:
        binaries = os.path.join(directory, "bin")
        os.mkdir(binaries)
        fakes = dict(FAKES)
        for name in package_managers:
            fakes[name] = PACKAGE_MANAGERS[name]
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
        # The script is bash (its shebang, "set -o pipefail"), so it is run as
        # bash rather than through /bin/sh, which is dash on Debian and Ubuntu.
        completed = subprocess.run(
            ["bash", SCRIPT, "--no-sudo",
             "--prefix", os.path.join(directory, "prefix")] + list(arguments),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True, timeout=60,
            env=environment, cwd=SCRIPT_DIR)
        return completed.returncode, completed.stderr, completed.stdout
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def would_run(stderr):
    """The privileged commands the run printed instead of running."""
    marker = "[would run as root] "
    return [line.split(marker, 1)[1] for line in stderr.splitlines()
            if marker in line]


# Fedora: dnf, and the package names Fedora uses. The run gets as far as the
# clone, which fails, so the status only says that; the printed command is
# what this case is about.
status, stderr, stdout = run_with_fakes([], ["dnf"])
assert status != 0, stderr
assert stdout == "", stdout
assert "==> Installing system dependencies (dnf)" in stderr, stderr
assert DNF_DEPENDENCIES in would_run(stderr), (DNF_DEPENDENCIES, stderr)

# Debian/Ubuntu: apt-get, and the names it has always used.
status, stderr, stdout = run_with_fakes([], ["apt-get"])
assert status != 0, stderr
assert stdout == "", stdout
assert "==> Installing system dependencies (apt)" in stderr, stderr
assert APT_DEPENDENCIES in would_run(stderr), (APT_DEPENDENCIES, stderr)

# A machine with both is a Debian or Ubuntu machine that also has dnf
# installed, and is installed with apt.
status, stderr, stdout = run_with_fakes([], ["apt-get", "dnf"])
assert status != 0, stderr
assert APT_DEPENDENCIES in would_run(stderr), (APT_DEPENDENCIES, stderr)
assert not [line for line in would_run(stderr) if line.startswith("dnf ")], \
    stderr

# Neither: refused in one "error: " line that names both, with nothing on
# standard output, before anything is installed or built.
status, stderr, stdout = run_with_fakes([], [])
assert status == 1, (status, stderr)
assert stdout == "", stdout
assert stderr == ("error: on Linux this script supports Debian/Ubuntu "
                  "(apt-get) and Fedora (dnf) only; see REQUIREMENTS.md for "
                  "other platforms\n"), stderr

# --with-firmware adds the ARM cross-compiler and dfu-util, named for
# whichever package manager is in use, and installs them before the longer
# dependency step.
for manager, firmware, dependencies in (
        ("dnf", DNF_FIRMWARE, DNF_DEPENDENCIES),
        ("apt-get", APT_FIRMWARE, APT_DEPENDENCIES)):
    status, stderr, stdout = run_with_fakes(["--with-firmware"], [manager])
    assert status != 0, stderr
    assert stdout == "", stdout
    # No Ubertooth tool is reachable (see HIDDEN), so the script installs the
    # toolchain and leaves the firmware to the end, where this run never
    # arrives. A run that did build it would do so inside the checkout, and
    # the two commands this case compares would not both be there to compare.
    assert "==> Building firmware" not in stderr, (manager, stderr)
    printed = would_run(stderr)
    assert firmware in printed, (manager, firmware, stderr)
    assert printed.index(firmware) < printed.index(dependencies), \
        (manager, printed)

# Every package the script asks for is in the table a user installing by hand
# reads, so a name added or corrected in one of them has to be done in both.
if REQUIREMENTS:
    with open(REQUIREMENTS) as table:
        listed = table.read()
    for command in (APT_DEPENDENCIES, APT_FIRMWARE,
                    DNF_DEPENDENCIES, DNF_FIRMWARE):
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
      "package names REQUIREMENTS.md lists, and neither is refused")
