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

U1_DFU_VENDORID = 0x1D50
U1_DFU_PRODUCTID = 0x6003
WILDCARD = 0xFFFF
SUFFIX_LENGTH = 16
# The firmware revision marker -r looks for in the image.
PAYLOAD = b"ubertooth git-testing (builder@host) 2026-01-01\x00" + bytes(64)


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
]

failures = []
with tempfile.TemporaryDirectory() as directory:
    for name, contents, expected, reaches_device in CASES:
        path = os.path.join(directory, f"{name}.dfu")
        with open(path, "wb") as handle:
            handle.write(contents)
        command = [BINARY, "-U", NO_SUCH_SERIAL, "-d", path, "-r"]
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=20, check=False, env=ENVIRONMENT)
        output = result.stdout + result.stderr
        print("$", " ".join(command))
        print(output, end="" if output.endswith("\n") else "\n")
        print(f"[exit {result.returncode}]")

        if result.returncode != 1:
            failures.append(f"{name}: exit {result.returncode}, expected 1")
        if expected is not None and expected not in output:
            failures.append(f"{name}: {expected!r} not in the output")
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
