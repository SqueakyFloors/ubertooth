#!/usr/bin/env python3
"""ubertooth-dfu file handling: -s, and the files -d and -u touch (#200, #241).

Run as: test_dfu_files.py <path to ubertooth-dfu>

No board is needed. The cases that would otherwise reach a device use a serial
number no board has, and the tool runs with UBERTOOTH_TEST_NO_DEVICES=1, so
device selection finds nothing without opening anything whatever is attached
(#219). The read-only cases are skipped when running as root,
because root ignores the permissions they rely on.
"""

import os
import shutil
import stat
import subprocess
import sys
import tempfile

BINARY = sys.argv[1]
NO_SUCH_SERIAL = "ffffffffffffffffffffffffffffffff"
NOT_FOUND = f"no ubertooth found with serial matching '{NO_SUCH_SERIAL}'"
# Hide attached boards from the tool: matching a serial otherwise opens every
# one of them to read its serial, which stalls on a board another process is
# using (#219).
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")
SUFFIX_LENGTH = 16
AS_ROOT = hasattr(os, "geteuid") and os.geteuid() == 0

failures = []
skipped = []


def check(condition, message):
    if not condition:
        failures.append(message)


def run(*arguments):
    command = [BINARY, *arguments]
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=20, check=False, env=ENVIRONMENT)
    output = result.stdout + result.stderr
    print("$", " ".join(command))
    if output:
        print(output, end="" if output.endswith("\n") else "\n")
    print(f"[exit {result.returncode}]")
    return result.returncode, output


def write(path, data):
    with open(path, "wb") as handle:
        handle.write(data)


def read(path):
    with open(path, "rb") as handle:
        return handle.read()


FIRMWARE = b"ubertooth git-testing (builder@host) 2026-01-01\x00" + bytes(200)

with tempfile.TemporaryDirectory() as directory:
    readonly_dir = os.path.join(directory, "ro")

    # 1. -s on a plain .bin writes <name>.dfu: the binary plus a 16-byte suffix
    #    naming 1d50:6003.
    print("\n### -s signs a .bin")
    binary = os.path.join(directory, "image.bin")
    signed = os.path.join(directory, "image.dfu")
    write(binary, FIRMWARE)
    code, _ = run("-s", binary)
    check(code == 0, f"-s on a .bin exited {code}")
    if os.path.exists(signed):
        data = read(signed)
        check(data[:len(FIRMWARE)] == FIRMWARE, "-s changed the firmware bytes")
        check(len(data) == len(FIRMWARE) + SUFFIX_LENGTH,
              f"-s wrote {len(data)} bytes, expected {len(FIRMWARE) + SUFFIX_LENGTH}")
        suffix = data[-SUFFIX_LENGTH:]
        check(suffix[8:11] == b"\x55\x46\x44", "-s wrote no UFD signature")
        check(suffix[11] == SUFFIX_LENGTH, "-s wrote a wrong suffix length")
        check(suffix[2:4] == (0x6003).to_bytes(2, "little"), "-s wrote a wrong idProduct")
        check(suffix[4:6] == (0x1D50).to_bytes(2, "little"), "-s wrote a wrong idVendor")
    else:
        check(False, "-s created no .dfu")

    # 2. -s refuses a .bin that already carries a suffix, and writes nothing.
    print("\n### -s refuses an already-suffixed .bin")
    suffixed = os.path.join(directory, "suffixed.bin")
    shutil.copyfile(signed, suffixed)
    before = read(suffixed)
    code, output = run("-s", suffixed)
    check(code != 0, "-s accepted an already-suffixed .bin")
    check(not os.path.exists(os.path.join(directory, "suffixed.dfu")),
          "-s wrote a second suffix")
    check(read(suffixed) == before, "-s changed its input")
    check("already has a DFU suffix" in output,
          "-s did not say the input already has a suffix")

    # 3. -s only reads its input, so a read-only binary signs fine.
    print("\n### -s signs a read-only .bin")
    if AS_ROOT:
        skipped.append("read-only input (running as root)")
    else:
        readonly = os.path.join(directory, "readonly.bin")
        write(readonly, FIRMWARE)
        os.chmod(readonly, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
        code, output = run("-s", readonly)
        check(code == 0, f"-s on a read-only .bin exited {code}: {output.strip()}")
        check(os.path.exists(os.path.join(directory, "readonly.dfu")),
              "-s wrote no .dfu for a read-only input")

    # 4. When the output cannot be created, the message names the output.
    print("\n### -s names the output it cannot create")
    if AS_ROOT:
        skipped.append("unwritable output directory (running as root)")
    else:
        os.mkdir(readonly_dir)
        write(os.path.join(readonly_dir, "r.bin"), FIRMWARE)
        os.chmod(readonly_dir, stat.S_IRUSR | stat.S_IXUSR)
        code, output = run("-s", os.path.join(readonly_dir, "r.bin"))
        check(code != 0, "-s succeeded with an unwritable output directory")
        check("r.dfu" in output, "-s did not name the output file it could not create")
        os.chmod(readonly_dir, stat.S_IRWXU)

    # 5. -d only reads its image, so a read-only image reaches device selection.
    print("\n### -d reads a read-only image")
    if AS_ROOT:
        skipped.append("read-only download image (running as root)")
    else:
        readonly_image = os.path.join(directory, "image-readonly.dfu")
        shutil.copyfile(signed, readonly_image)
        os.chmod(readonly_image, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
        code, output = run("-U", NO_SUCH_SERIAL, "-d", readonly_image, "-r")
        check(code == 1, f"-d on a read-only image exited {code}")
        check("Permission denied" not in output,
              "-d still needs write access to its image")
        check(NOT_FOUND in output.lower(),
              "-d did not get as far as looking for the board")

    # 6. A failed -u leaves an earlier read-back of the same name alone, with
    #    its permissions, and no temporary file behind. The same must hold for
    #    every way the command can end before the upload: -u creates its
    #    temporary file while the options are still being read.
    print("\n### a failed -u keeps the earlier file")
    upload_dir = os.path.join(directory, "upload")
    os.mkdir(upload_dir)
    keep = os.path.join(upload_dir, "keep.bin")
    write(keep, b"an earlier read-back\n" * 100)
    os.chmod(keep, stat.S_IRUSR | stat.S_IWUSR)
    before = read(keep)
    before_mode = stat.S_IMODE(os.stat(keep).st_mode)

    def keep_survived(label):
        check(read(keep) == before, f"{label} destroyed the earlier file")
        check(stat.S_IMODE(os.stat(keep).st_mode) == before_mode,
              f"{label} changed the earlier file's permissions")
        leftovers = sorted(set(os.listdir(upload_dir)) - {"keep.bin"})
        check(not leftovers, f"{label} left files behind: {leftovers}")
        # Remove them, so one failing case does not report the next one's
        # leftovers as well.
        for leftover in leftovers:
            os.unlink(os.path.join(upload_dir, leftover))

    code, _ = run("-U", NO_SUCH_SERIAL, "-u", keep, "-r")
    check(code == 1, f"-u with no board exited {code}")
    keep_survived("a failed -u")

    # The refused -d image is checked after -u has already made its temporary
    # file, so that exit has to clean up like the rest (#241).
    refused = os.path.join(directory, "not-a-dfu-file")
    write(refused, b"this is not firmware, it is a note to self\n")

    for label, arguments in (
        ("-u with -h", ("-u", keep, "-h")),
        ("-u with a stray word", ("-u", keep, "extra")),
        ("-u with an unknown option", ("-u", keep, "-Z")),
        ("-u twice", ("-u", keep, "-u", keep, "-U", NO_SUCH_SERIAL, "-r")),
        ("-u with a refused -d image",
         ("-u", keep, "-d", refused, "-U", NO_SUCH_SERIAL, "-r")),
        ("-u with a refused -d image, -d first",
         ("-d", refused, "-u", keep, "-U", NO_SUCH_SERIAL, "-r")),
    ):
        code, output = run(*arguments)
        check(code == 1, f"{label} exited {code}")
        keep_survived(label)
        if "refused -d" in label:
            check("firmware will not be updated" in output,
                  f"{label} did not refuse the image")
            check(NOT_FOUND not in output.lower(),
                  f"{label} went looking for a board anyway")

    # 7. A file the tool would read into memory whole is measured first: a
    #    huge or sparse file, or one that is not a regular file at all, is
    #    refused before any allocation and before any device is opened (#246).
    print("\n### -d and -s refuse a file they cannot read whole")
    oversized = os.path.join(directory, "oversized.dfu")
    with open(oversized, "wb") as handle:
        handle.truncate(4 * 1024 * 1024)  # sparse: no disk used
    code, output = run("-U", NO_SUCH_SERIAL, "-d", oversized, "-r")
    check(code == 1, f"-d on an oversized image exited {code}")
    check("an Ubertooth image is at most" in output,
          "-d did not say how large an image may be")
    check(NOT_FOUND not in output.lower(),
          "-d opened a device for an oversized image")

    oversized_bin = os.path.join(directory, "oversized-input.bin")
    with open(oversized_bin, "wb") as handle:
        handle.truncate(4 * 1024 * 1024)
    code, output = run("-s", oversized_bin)
    check(code == 1, f"-s on an oversized .bin exited {code}")
    check("an Ubertooth image is at most" in output,
          "-s did not say how large an image may be")
    check(not os.path.exists(os.path.join(directory, "oversized-input.dfu")),
          "-s wrote a .dfu for an oversized input")

    # A directory, and a FIFO: neither has a length that says what a read
    # would return. The FIFO is opened read-write here so it has a reader of
    # its own and the tool's open does not block.
    fifo = os.path.join(directory, "image.fifo")
    os.mkfifo(fifo)
    fifo_end = os.open(fifo, os.O_RDWR)
    try:
        for label, path in (("a directory", directory), ("a FIFO", fifo)):
            code, output = run("-U", NO_SUCH_SERIAL, "-d", path, "-r")
            check(code == 1, f"-d on {label} exited {code}")
            check(os.path.basename(path) in output or path in output,
                  f"-d did not name {label} it refused")
            check(NOT_FOUND not in output.lower(),
                  f"-d opened a device for {label}")
    finally:
        os.close(fifo_end)
        os.unlink(fifo)

    # A normal image of the same shape still gets through to the board.
    code, output = run("-U", NO_SUCH_SERIAL, "-d", signed, "-r")
    check(code == 1, f"-d on a normal image exited {code}")
    check(NOT_FOUND in output.lower(),
          "-d refused a normal image after the size check")

for note in skipped:
    print("SKIP", note)
if failures:
    for failure in failures:
        print("FAIL", failure)
    sys.exit(1)
print("DFU file handling cases ok")
