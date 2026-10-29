#!/usr/bin/env python3
"""ubertooth-dfu -d checks the image before it goes near a device.

Run as: test_dfu_images.py <path to ubertooth-dfu>

Every run uses a serial number no board has, so the tool would fail at device
selection if it got that far. A rejected image must be refused with exit 1 and
its own message, without that device-selection line: nothing was opened, so
nothing was switched into the DFU bootloader. An acceptable image must get
past the check and reach device selection instead. The tool runs with
UBERTOOTH_TEST_NO_DEVICES=1, so selection opens nothing whatever is attached.
"""

import os
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
# How long one ubertooth-dfu run is given. The whole test takes about 0.3 s on
# an idle machine and 1.2 s on one busy with parallel builds, so twenty seconds
# is many times what any single run needs. The CTest timeout on this test is
# larger again, so this deadline is the one that fires and names the run that
# timed out.
RUN_TIMEOUT_S = 20

U1_DFU_VENDORID = 0x1D50
U1_DFU_PRODUCTID = 0x6003
WILDCARD = 0xFFFF
SUFFIX_LENGTH = 16

# The firmware info block every image from this tree carries, at the fixed
# offset -d reads the image name and revision from
# (host/libubertooth/src/ubertooth_interface.h).
FW_INFO_OFFSET = 0x100
FW_INFO_LENGTH = 64
FW_INFO_MAGIC = b"UBTFINFO"
FW_INFO_FORMAT = 1
FW_INFO_API = 0x0133
FW_IMAGE_NAME = b"bluetooth_rxtx"
FW_REVISION = b"git-testing"


def info_block(magic=FW_INFO_MAGIC, fmt=FW_INFO_FORMAT,
               image_name=FW_IMAGE_NAME, revision=FW_REVISION):
    block = bytearray(FW_INFO_LENGTH)
    block[0:8] = magic
    block[8:10] = fmt.to_bytes(2, "little")
    block[10:12] = FW_INFO_API.to_bytes(2, "little")
    block[16:16 + len(image_name)] = image_name
    block[32:32 + len(revision)] = revision
    return bytes(block)


def firmware(block=None):
    """A firmware payload: the build string, then the block where it lives."""
    body = bytearray(b"\xff" * (FW_INFO_OFFSET + FW_INFO_LENGTH + 64))
    marker = b"ubertooth git-testing (builder@host) 2026-01-01\x00"
    body[0:len(marker)] = marker
    if block is not None:
        body[FW_INFO_OFFSET:FW_INFO_OFFSET + FW_INFO_LENGTH] = block
    return bytes(body)


PAYLOAD = firmware(info_block())


def dfu_crc(data):
    """The DFU suffix CRC32: the usual CRC-32 without the final inversion."""
    return zlib.crc32(data) ^ 0xFFFFFFFF


def image(payload=PAYLOAD, vendor=U1_DFU_VENDORID, product=U1_DFU_PRODUCTID,
          bcd_dfu=0x0100, signature=b"\x55\x46\x44", length=SUFFIX_LENGTH,
          corrupt_crc=False):
    body = bytearray(payload)
    body += (0).to_bytes(2, "little")          # bcdDevice
    body += product.to_bytes(2, "little")
    body += vendor.to_bytes(2, "little")
    body += bcd_dfu.to_bytes(2, "little")
    body += signature
    body += bytes([length])
    crc = dfu_crc(bytes(body))
    if corrupt_crc:
        crc ^= 0xFFFFFFFF
    body += crc.to_bytes(4, "little")
    return bytes(body)


# (name, file contents, expected substring, reaches device selection)
CASES = [
    ("good", image(), None, True),
    ("wildcard-ids", image(vendor=WILDCARD, product=WILDCARD), None, True),
    ("corrupt-crc", image(corrupt_crc=True), "CRC mismatch", False),
    ("truncated", image()[:-20], "Unknown DFU suffix length", False),
    ("no-suffix", bytes(len(PAYLOAD)), "Unknown DFU suffix length: 0", False),
    ("text-file", b"this is not firmware, it is a note to self\n",
     "Unknown DFU suffix length", False),
    ("suffix-only", image(payload=b""), "no firmware", False),
    ("bad-signature", image(signature=b"\x00\x46\x44"),
     "DFU Signature mismatch", False),
    ("unknown-dfu-version", image(bcd_dfu=0x0200), "Unknown DFU version: 0200",
     False),
    ("wrong-vendor", image(vendor=0x1234), "vendor 0x1234", False),
    ("wrong-product", image(product=0x6004), "product 0x6004", False),
    # A properly signed file for this device that is not an image from this
    # tree: no firmware info block where every image carries one.
    ("no-info-block", image(payload=firmware()),
     "there is no firmware info block here at offset 0x100", False),
    ("info-block-elsewhere",
     image(payload=firmware()[:0x80] + info_block()
           + firmware()[0x80 + FW_INFO_LENGTH:]),
     "there is no firmware info block here at offset 0x100", False),
    ("info-block-unknown-format", image(payload=firmware(info_block(fmt=99))),
     "format this host does not know", False),
    ("info-block-empty-image-name",
     image(payload=firmware(info_block(image_name=b""))),
     "no readable image name or revision", False),
]

failures = []
with tempfile.TemporaryDirectory() as directory:
    for name, contents, expected, reaches_device in CASES:
        path = os.path.join(directory, f"{name}.dfu")
        with open(path, "wb") as handle:
            handle.write(contents)
        command = [BINARY, "-U", NO_SUCH_SERIAL, "-d", path, "-r"]
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=RUN_TIMEOUT_S, check=False,
                                env=ENVIRONMENT)
        output = result.stdout + result.stderr
        print("$", " ".join(command))
        print(output, end="" if output.endswith("\n") else "\n")
        print(f"[exit {result.returncode}]")

        if result.returncode != 1:
            failures.append(f"{name}: exit {result.returncode}, expected 1")
        if expected is not None and expected not in output:
            failures.append(f"{name}: {expected!r} not in the output")
        # Nothing of this reaches standard output: the check's own progress
        # line is progress, and the refusal is an error.
        if result.stdout != "":
            failures.append(f"{name}: wrote {result.stdout!r} to standard output")
        if "Checking firmware signature" not in result.stderr:
            failures.append(f"{name}: the progress line is not on standard error")
        if not reaches_device:
            # One line, prefixed, naming the file it is about: the path is
            # the one the user wrote, so a script's own name for the image
            # comes back to it.
            lines = [line for line in result.stderr.splitlines()
                     if line.startswith("error: ")]
            if len(lines) != 1:
                failures.append(f"{name}: {len(lines)} 'error: ' lines, "
                                f"expected 1: {result.stderr!r}")
            elif not lines[0].startswith(f"error: {path}: "):
                failures.append(f"{name}: the refusal {lines[0]!r} does not "
                                "name the image")
        selected = NOT_FOUND in output.lower()
        if reaches_device and not selected:
            failures.append(f"{name}: image was not accepted for writing")
        if not reaches_device and selected:
            failures.append(f"{name}: a device was opened for a bad image")

if failures:
    for failure in failures:
        print("FAIL", failure)
    sys.exit(1)
print(f"{len(CASES)} DFU image cases ok")
