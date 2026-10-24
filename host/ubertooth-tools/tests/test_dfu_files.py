#!/usr/bin/env python3
"""ubertooth-dfu file handling: -s, and the files -d and -u touch (#200).

Run as: test_dfu_files.py <path to ubertooth-dfu>

No board is needed. The cases that would otherwise reach a device use a serial
number no board has, so the tool stops at device selection; nothing is opened,
claimed or changed. The read-only cases are skipped when running as root,
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
                            timeout=20, check=False)
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

    # 6. A failed -u leaves an earlier read-back of the same name alone, and no
    #    temporary file behind.
    print("\n### a failed -u keeps the earlier file")
    upload_dir = os.path.join(directory, "upload")
    os.mkdir(upload_dir)
    keep = os.path.join(upload_dir, "keep.bin")
    write(keep, b"an earlier read-back\n" * 100)
    before = read(keep)
    code, output = run("-U", NO_SUCH_SERIAL, "-u", keep, "-r")
    check(code == 1, f"-u with no board exited {code}")
    check(read(keep) == before, "a failed -u destroyed the earlier file")
    leftovers = sorted(set(os.listdir(upload_dir)) - {"keep.bin"})
    check(not leftovers, f"-u left files behind: {leftovers}")

for note in skipped:
    print("SKIP", note)
if failures:
    for failure in failures:
        print("FAIL", failure)
    sys.exit(1)
print("DFU file handling cases ok")
