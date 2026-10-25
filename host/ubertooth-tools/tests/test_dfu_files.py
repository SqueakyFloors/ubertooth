#!/usr/bin/env python3
"""ubertooth-dfu file handling: -s, and the files -d and -u touch.

Run as: test_dfu_files.py <path to ubertooth-dfu>

No board is needed. The cases that would otherwise reach a device use a serial
number no board has, and the tool runs with UBERTOOTH_TEST_NO_DEVICES=1, so
device selection finds nothing without opening anything whatever is attached.
The read-only cases are skipped when running as root, because root ignores
the permissions they rely on.
"""

import os
import shutil
import stat
import subprocess
import sys
import tempfile
import zlib

BINARY = sys.argv[1]
NO_SUCH_SERIAL = "ffffffffffffffffffffffffffffffff"
NOT_FOUND = f"no ubertooth found with serial matching '{NO_SUCH_SERIAL}'"
# Hide attached boards from the tool: matching a serial otherwise opens every
# one of them to read its serial, which stalls on a board another process is
# using.
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")
SUFFIX_LENGTH = 16
# The largest image the tool reads: the flash above the 16 KiB bootloader on
# the largest part it knows (512 KiB), plus the suffix. The tool derives this
# from its part table; the cases below only need the same number.
MAX_IMAGE = 512 * 1024 - 16 * 1024 + SUFFIX_LENGTH
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


def expected_signed(payload):
    """The whole image -s must write for payload, suffix and CRC included.

    The suffix is the 16 bytes the tool appends: bcdDevice 0, idProduct 0x6003,
    idVendor 0x1d50, bcdDFU 0x0100, the UFD signature, the suffix length, and
    the CRC over everything before it. The CRC starts at 0xffffffff with no
    final inversion, which is zlib's inverted.
    """
    head = (b"\x00\x00"
            + (0x6003).to_bytes(2, "little")
            + (0x1D50).to_bytes(2, "little")
            + (0x0100).to_bytes(2, "little")
            + b"\x55\x46\x44"
            + bytes([SUFFIX_LENGTH]))
    crc = zlib.crc32(payload + head) ^ 0xFFFFFFFF
    return payload + head + crc.to_bytes(4, "little")

with tempfile.TemporaryDirectory() as directory:
    readonly_dir = os.path.join(directory, "ro")

    # 1. -s on a plain .bin writes <name>.dfu: the binary plus a 16-byte suffix
    #    naming 1d50:6003.
    print("\n### -s signs a .bin")
    binary = os.path.join(directory, "image.bin")
    signed = os.path.join(directory, "image.dfu")
    write(binary, FIRMWARE)
    before_sign = sorted(os.listdir(directory))
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
        # The whole image, CRC included: signing through a temporary file
        # renamed into place must leave exactly the bytes it used to write
        # directly.
        check(data == expected_signed(FIRMWARE),
              "-s wrote a different signed image than expected")
        # ...and nothing beside it: the temporary file is renamed, never left.
        leftovers = sorted(set(os.listdir(directory)) - set(before_sign) -
                           {"image.dfu"})
        check(not leftovers, f"-s left files beside its output: {leftovers}")
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
    # file, so that exit has to clean up like the rest.
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

    # The -s limit is the image limit less the suffix signing adds, so what
    # -s writes at the limit is a file -d still accepts.
    print("\n### -s stops one byte before a .dfu that -d would refuse")
    at_limit = os.path.join(directory, "at-limit.bin")
    with open(at_limit, "wb") as handle:
        handle.write(FIRMWARE)  # the revision marker -r needs
        handle.truncate(MAX_IMAGE - SUFFIX_LENGTH)  # the rest is sparse
    code, output = run("-s", at_limit)
    check(code == 0, f"-s on a .bin at the limit exited {code}: {output.strip()}")
    at_limit_dfu = os.path.join(directory, "at-limit.dfu")
    if os.path.exists(at_limit_dfu):
        check(os.path.getsize(at_limit_dfu) == MAX_IMAGE,
              "-s at the limit wrote an unexpected size")
        code, output = run("-U", NO_SUCH_SERIAL, "-d", at_limit_dfu, "-r")
        check(NOT_FOUND in output.lower(),
              "-d refused the image -s wrote at the limit")
    else:
        check(False, "-s wrote no .dfu for a .bin at the limit")

    over_limit = os.path.join(directory, "over-limit.bin")
    with open(over_limit, "wb") as handle:
        handle.truncate(MAX_IMAGE - SUFFIX_LENGTH + 1)
    code, output = run("-s", over_limit)
    check(code == 1, f"-s one byte over the limit exited {code}")
    check(not os.path.exists(os.path.join(directory, "over-limit.dfu")),
          "-s wrote a .dfu one byte over the limit")

    # A directory, and a FIFO: neither has a length that says what a read
    # would return. The FIFO is opened read-write here so it has a reader of
    # its own and the tool's open does not block. -s gets a FIFO named .bin,
    # so it has to pass the extension check before the guard sees it.
    fifo = os.path.join(directory, "image.fifo")
    fifo_bin = os.path.join(directory, "image-fifo.bin")
    os.mkfifo(fifo)
    os.mkfifo(fifo_bin)
    fifo_end = os.open(fifo, os.O_RDWR)
    fifo_bin_end = os.open(fifo_bin, os.O_RDWR)
    try:
        for label, path in (("a directory", directory), ("a FIFO", fifo)):
            code, output = run("-U", NO_SUCH_SERIAL, "-d", path, "-r")
            check(code == 1, f"-d on {label} exited {code}")
            check(os.path.basename(path) in output or path in output,
                  f"-d did not name {label} it refused")
            check(NOT_FOUND not in output.lower(),
                  f"-d opened a device for {label}")

        code, output = run("-s", fifo_bin)
        check(code == 1, f"-s on a FIFO exited {code}")
        check("not a regular file" in output,
              "-s did not say its input is not a regular file")
        check(not os.path.exists(os.path.join(directory, "image-fifo.dfu")),
              "-s wrote a .dfu for a FIFO")
    finally:
        os.close(fifo_end)
        os.close(fifo_bin_end)
        os.unlink(fifo)
        os.unlink(fifo_bin)

    # A normal image of the same shape still gets through to the board.
    code, output = run("-U", NO_SUCH_SERIAL, "-d", signed, "-r")
    check(code == 1, f"-d on a normal image exited {code}")
    check(NOT_FOUND in output.lower(),
          "-d refused a normal image after the size check")

    # 8. A failed -s leaves no .dfu: not a partial one, not an empty one, and
    #    not a replaced one. The output used to be created and truncated
    #    before the input had been read and before the -d image had been
    #    checked, so both of those failures destroyed an earlier .dfu of the
    #    same name, or left a new one behind. The signed file is now written
    #    through a temporary file renamed over the target once the whole image
    #    is there, and every failure removes that temporary file.
    #
    #    What is left of the truncate-before-read window cannot be reproduced
    #    from a test: the input is measured and refused before it is read, so
    #    the remaining ways signing can fail after the output exists are a
    #    short read of a file something else is shrinking, an allocation
    #    failure, and a write that does not fit. None of those can be asked
    #    for portably. The cases below pin what can be: the refusals that come
    #    before the output, and the cleanup of the temporary file.
    print("\n### a failed -s keeps the earlier .dfu and creates none")
    sign_dir = os.path.join(directory, "sign")
    os.mkdir(sign_dir)
    good_bin = os.path.join(sign_dir, "fw.bin")
    good_dfu = os.path.join(sign_dir, "fw.dfu")
    write(good_bin, FIRMWARE)
    # A directory as the input: the shape the issue reported, now refused by
    # the regular-file guard before anything is opened for writing. Its .dfu
    # name has no input of its own, so each case below says which .bin it
    # signs.
    refused_input = os.path.join(sign_dir, "unreadable.bin")
    os.mkdir(refused_input)
    bad_image = os.path.join(sign_dir, "not-an-image.dfu")
    write(bad_image, b"this is not firmware, it is a note to self\n")
    EARLIER = b"an earlier signed image\n" * 10

    def sign_failed(label, arguments, written):
        """Run a -s that must fail, with and without an earlier output."""
        for existing in (None, EARLIER):
            if existing is None:
                if os.path.exists(written):
                    os.unlink(written)
            else:
                write(written, existing)
            before = sorted(os.listdir(sign_dir))
            code, output = run(*arguments)
            check(code == 1, f"{label} exited {code}")
            if existing is None:
                check(not os.path.exists(written),
                      f"{label} created {os.path.basename(written)}")
            else:
                check(os.path.exists(written) and read(written) == existing,
                      f"{label} did not leave {os.path.basename(written)} as it was")
            # No temporary file beside the output either, and nothing else new.
            check(sorted(os.listdir(sign_dir)) == before,
                  f"{label} changed the directory: {sorted(os.listdir(sign_dir))}")
            check(NOT_FOUND not in output.lower(),
                  f"{label} went looking for a board anyway")
        # Leave the directory as the next case expects to find it.
        if os.path.exists(written):
            os.unlink(written)
        return output

    # An input the tool will not read whole.
    output = sign_failed("-s on a refused input",
                         ("-s", refused_input),
                         os.path.join(sign_dir, "unreadable.dfu"))
    check("unreadable.bin" in output,
          "-s did not name the input it could not read")

    # With -u in the same command, that input is still what is refused, and
    # the read-back's temporary file is never created: the -s checks come
    # before either output, not just before its own.
    readback = os.path.join(sign_dir, "readback.bin")
    write(readback, EARLIER)
    before = sorted(os.listdir(sign_dir))
    code, output = run("-u", readback, "-s", refused_input,
                       "-U", NO_SUCH_SERIAL, "-r")
    check(code == 1, f"-u with a refused -s input exited {code}")
    check("unreadable.bin" in output and "not a regular file" in output,
          "-u with a refused -s input did not report the input")
    check(read(readback) == EARLIER,
          "-u with a refused -s input destroyed the earlier read-back")
    check(sorted(os.listdir(sign_dir)) == before,
          f"-u with a refused -s input changed the directory: "
          f"{sorted(os.listdir(sign_dir))}")
    check(NOT_FOUND not in output.lower(),
          "-u with a refused -s input went looking for a board anyway")
    os.unlink(readback)

    # Which of the two is reported says which came first: a -u output that
    # cannot be created at all does not get in front of a refused -s input.
    code, output = run("-u", os.path.join(sign_dir, "no-such-dir", "out.bin"),
                       "-s", refused_input, "-U", NO_SUCH_SERIAL, "-r")
    check(code == 1, f"-u with nowhere to write and a refused -s input exited {code}")
    check("not a regular file" in output,
          "the -s input was not checked before the -u output was created")

    # An output that cannot be replaced: the rename over the target fails, so
    # the temporary file beside it has to go too.
    blocked_bin = os.path.join(sign_dir, "blocked.bin")
    blocked_dfu = os.path.join(sign_dir, "blocked.dfu")
    write(blocked_bin, FIRMWARE)
    os.mkdir(blocked_dfu)
    write(os.path.join(blocked_dfu, "keep"), b"not the signed image\n")
    code, output = run("-s", blocked_bin)
    check(code == 1, f"-s with an unreplaceable output exited {code}")
    check("blocked.dfu" in output,
          "-s did not name the output it could not write")
    check(sorted(os.listdir(blocked_dfu)) == ["keep"],
          "-s wrote into the directory in its output's place")
    leftovers = sorted(name for name in os.listdir(sign_dir)
                       if name.startswith("blocked.dfu."))
    check(not leftovers, f"-s left a temporary file behind: {leftovers}")
    shutil.rmtree(blocked_dfu)
    os.unlink(blocked_bin)

    # Renaming a file into place needs write access to the directory, which
    # overwriting one in place did not: -s in a read-only directory now fails
    # even when the .dfu it would replace is writable. It is what -u has
    # always done, and the man page says so.
    print("\n### -s needs to be able to write in the output's directory")
    if AS_ROOT:
        skipped.append("read-only output directory with a target (running as root)")
    else:
        fixed_dir = os.path.join(directory, "fixed")
        os.mkdir(fixed_dir)
        fixed_bin = os.path.join(fixed_dir, "fw.bin")
        fixed_dfu = os.path.join(fixed_dir, "fw.dfu")
        write(fixed_bin, FIRMWARE)
        write(fixed_dfu, EARLIER)
        os.chmod(fixed_dir, stat.S_IRUSR | stat.S_IXUSR)
        code, output = run("-s", fixed_bin)
        check(code == 1, f"-s in a read-only directory exited {code}")
        check("fw.dfu" in output,
              "-s did not name the output it could not replace")
        os.chmod(fixed_dir, stat.S_IRWXU)
        check(read(fixed_dfu) == EARLIER,
              "-s in a read-only directory changed the earlier .dfu")
        check(sorted(os.listdir(fixed_dir)) == ["fw.bin", "fw.dfu"],
              f"-s left files behind: {sorted(os.listdir(fixed_dir))}")

    # The signed image is put in the target's place rather than written
    # through it, so a .dfu that is a symlink, or one with another hard link
    # to it, is replaced instead of having the image land in what it shares.
    # The man page says so; this pins it.
    print("\n### -s replaces its output rather than writing through a link")
    link_dir = os.path.join(directory, "links")
    os.mkdir(link_dir)
    for kind in ("symlink", "hardlink"):
        source = os.path.join(link_dir, f"{kind}.bin")
        target = os.path.join(link_dir, f"{kind}.dfu")
        other = os.path.join(link_dir, f"{kind}.other")
        write(source, FIRMWARE)
        write(other, EARLIER)
        if kind == "symlink":
            os.symlink(other, target)
        else:
            os.link(other, target)
        code, output = run("-s", source)
        check(code == 0, f"-s on a {kind} output exited {code}: {output.strip()}")
        check(read(target) == expected_signed(FIRMWARE),
              f"-s did not write the signed image over the {kind}")
        check(not os.path.islink(target), f"-s kept the {kind} in place")
        check(read(other) == EARLIER,
              f"-s wrote through the {kind} into the file it shared")

    # A -d image the tool refuses, with -s in the same command: the refusal
    # comes before the signed file exists, in either option order.
    for label, arguments in (
        ("-s with a refused -d image",
         ("-s", good_bin, "-d", bad_image, "-U", NO_SUCH_SERIAL, "-r")),
        ("-s with a refused -d image, -d first",
         ("-d", bad_image, "-s", good_bin, "-U", NO_SUCH_SERIAL, "-r")),
    ):
        output = sign_failed(label, arguments, good_dfu)
        check("firmware will not be updated" in output,
              f"{label} did not refuse the image")

    # The same command with an image the tool accepts signs the .bin and then
    # goes looking for the board, so the reordering did not cost -s its work.
    code, output = run("-s", good_bin, "-d", signed, "-U", NO_SUCH_SERIAL, "-r")
    check(code == 1, f"-s with an accepted -d image exited {code}")
    check(NOT_FOUND in output.lower(),
          "-s with an accepted -d image never looked for a board")
    check(os.path.exists(good_dfu) and read(good_dfu) == expected_signed(FIRMWARE),
          "-s with an accepted -d image wrote no signed file")
    leftovers = sorted(set(os.listdir(sign_dir)) -
                       {"fw.bin", "fw.dfu", "unreadable.bin", "not-an-image.dfu"})
    check(not leftovers, f"-s left files behind: {leftovers}")

for note in skipped:
    print("SKIP", note)
if failures:
    for failure in failures:
        print("FAIL", failure)
    sys.exit(1)
print("DFU file handling cases ok")
