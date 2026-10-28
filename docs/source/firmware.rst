========
Firmware
========

.. _the-two-firmware-images:

The two firmware images
~~~~~~~~~~~~~~~~~~~~~~~

A board carries one firmware image at a time, and there are two:

- ``bluetooth_rxtx`` — the standard image, needed by every host tool except
  one, ``ubertooth-btle`` and ``ubertooth-rx`` included. Built in
  ``firmware/bluetooth_rxtx``.
- ``btbr`` — needed by ``ubertooth-btbr`` and nothing else. It implements part
  of the Classic Bluetooth BR/EDR baseband, which the standard image does not.
  Built in ``firmware/btbr``; see :doc:`sniffing_BR_EDR`.

Switch a board from one to the other by flashing it:

	.. code-block:: sh

		ubertooth-dfu -d firmware/btbr/btbr.dfu -r                      # for ubertooth-btbr
		ubertooth-dfu -d firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r  # for everything else

Add ``-U <index|serial>`` to either command to pick one board of several; it
works from both images. A tool run against the wrong image fails rather than
waiting: ``ubertooth-btbr`` says it could not set the Ubertooth up, naming
``bluetooth_rxtx`` and the image to flash, and the other tools fail on the
first request the ``btbr`` image does not answer. Neither is refused over its
firmware API version, because both images report this tree's.

``ubertooth-util -V`` names the image a board is carrying, so it is how you
check which one is on there now, or that a switch worked. Its ``Firmware
version:`` line is the one to read; the host line above it says which tools
you are running, and the build line below it when that image was built:

	.. code-block:: sh

		$ ubertooth-util -U <serial> -V
		Host tools version: libubertooth <version> (git-26d4c11), libbtbb <version> (git-2fd0d39)
		Firmware version: bluetooth_rxtx git-26d4c11 (API:<version>)
		Firmware build: ubertooth git-26d4c11 (builder@buildhost) Mon Oct 27 11:22:33 2025
		$ ubertooth-util -U <serial> -V | grep '^Firmware version:'   # flashed with btbr
		Firmware version: btbr git-26d4c11 (API:<version>)

``ubertooth-dfu`` names what is in a firmware file before it writes anything,
so the ``Image:`` line says which image is about to be flashed, and a file that
is not an Ubertooth image is refused before the board is touched:

	.. code-block:: sh

		$ ubertooth-dfu -U <serial> -d firmware/btbr/btbr.dfu -r
		Checking firmware signature
		Image: btbr git-26d4c11 (API:<version>)

The ``Image:`` line takes its version from the file and ``-v`` reports the USB
``bcdDevice`` the running firmware answers with; both images carry the same
value, so the two agree. The samples write it as ``<version>``, because it
moves with the firmware; ``COMPATIBILITY.md`` names the value this tree
requires. See
:ref:`what-version-am-i-running` below, and ubertooth-util(1) and
ubertooth-dfu(1).

How To Update Firmware
~~~~~~~~~~~~~~~~~~~~~~

Flash the firmware built from this tree: the host tools require firmware of
their own API version, and ``ubertooth-dfu`` only writes an image carrying the
firmware info block described below, so images from an upstream release are
refused. ``./install.sh --flash-device <index|serial|all>`` builds and flashes
it; to do it by
hand, build the image with ``make -C firmware/bluetooth_rxtx`` and run the
``ubertooth-dfu`` command like so:

	.. code-block:: Sh

		$ ubertooth-dfu -d bluetooth_rxtx.dfu -r
		Checking firmware signature
		Image: bluetooth_rxtx git-ed30131d* (API:<version>)
		Switching selected device to DFU mode...
		........................................
		........................................
		................
		Detached
		Verified firmware revision: git-ed30131d*

All of that is progress and goes to standard error, except the last line: the
revision read back from the board is the result, and it is the only thing on
standard output. A script can read it on its own.

The device will automatically enter DFU mode and flash the firmware. During the
application-to-DFU transition, the tool checks the entry request, follows the
selected device by its physical USB path, and makes bounded retries if the
application returns without entering DFU. With multiple devices already in DFU,
it fails rather than choosing one ambiguously.

Every image built from this tree carries a firmware info block at a fixed
offset, naming the image and the revision it was built from. The tool reads it
out of the file before it writes anything, says what it is about to write
(``Image: bluetooth_rxtx git-ed30131d* (API:<version>)``), and refuses a file
that carries no such block, which includes every image built before the block
existed.

After flashing with ``-d ... -r``, the tool takes the expected image name and
revision from that block, waits for the application to return on the same
physical USB path, and requires the board to report both before returning
success. Both images built from one tree carry the same revision, so the name
is what tells them apart. If a programming transfer fails while the device
remains in DFU, it reopens that same path and retries the complete image once
from block zero.

An image whose block reports a firmware API version other than these tools'
own is not refused for that, only warned about.

The reset request itself often fails, because the bootloader disconnects as it
resets rather than acknowledging the request first. That is not a failure: the
tool looks for the DFU device on the same physical USB path again and reports
the board as detached once it has gone and stayed gone, which takes two
listings 1.5 s apart. A board that is back on that path by the second listing
reset into the bootloader rather than into its application, and the command
fails saying so. A line reading ``error: could not read the DFU state: the
board's firmware does not support it (stalled, libusb -9)`` means the
bootloader stalled a state query, and the command fails.
The command returns success only after the expected application image and
revision have been verified. If verification fails, use the reported stage and
error details or the documented manual bootloader recovery path.

Troubleshooting:

If you run into an error such as "error: Input/Output Error (libusb -1)" or the Ubertooth's 4 LEDs next to each other perform a distinctive chasing pattern, after extracting the archive of the latest Ubertooth release, change into the ``firmware`` directory. Then, run:

	.. code-block:: sh

		make clean all && make
		ubertooth-dfu -r -d bluetooth_rxtx/bluetooth_rxtx.dfu

If your Ubertooth One has no firmware or broken firmware and doesn't show up when issuing ``lsusb``, force it into DFU mode by bridging pins 1 and 3 on the EXPAND header (P4) while the device is unplugged, then plug it in with the bridge still connected:

	.. image:: ../images/u1_roadmap.png

Wait for the distinctive LED sequence or USB ID ``1d50:6003``, then remove the bridge and immediately run ``ubertooth-dfu``. The entry pin is sampled only during reset, so removing the bridge after DFU starts does not exit the bootloader. Remove it before the final reset; otherwise the next boot returns to DFU and ``-r`` fails with ``error: detach failed: the board reset and came back in the bootloader``, with the image written and the board in DFU rather than running it. If removing the live bridge is inconvenient, finish flashing, unplug the device, remove the bridge, and reconnect it; ``ubertooth-util -V`` then reports the new firmware.

On Linux, verify the bootloader USB ID before flashing:

	.. code-block:: sh

		$ lsusb -d 1d50:6003
		Bus 001 Device 007: ID 1d50:6003 OpenMoko, Inc. USB Bootloader

Then run the DFU command immediately.



.. _what-version-am-i-running:

What Version Am I Running? 
^^^^^^^^^^^^^^^^^^^^^^^^^^

In non-DFU mode, you can obtain firmware information with ``ubertooth-util -V``. Note that the version shown should match the one you just installed:

	.. code-block:: sh

		$ ubertooth-util -V
		Host tools version: libubertooth <version> (git-ed30131d*), libbtbb <version> (git-2fd0d39)
		Firmware version: bluetooth_rxtx git-ed30131d* (API:<version>)
		Firmware build: ubertooth git-ed30131d* (builder@host) Tue Aug  7 15:33:06 PDT 2018

Both firmware lines start with the image the board is running, read from its
firmware info block; a board running firmware from before the block shows the
revision alone. A firmware built from a git checkout reports its git revision
as above, with a trailing ``*`` if the working tree had uncommitted changes; a
release build reports the release name instead. The version is
generated into a ``version.h`` at build time and tracks the exact source it was
built from, so ``-V`` always reflects the firmware actually running.



Developing Firmware
^^^^^^^^^^^^^^^^^^^



You'll need a toolchain that supports ARM Cortex-M3. The Makefiles in the firmware directory are designed for GCC and a Linux-based toolchain, specifically ``arm-none-eabi-gcc and libnewlib-arm-none-eabi``. The bootloader also needs the ARM C++ runtime, because it is the one image written in C++; the plain firmware build leaves it out, so you only need that package to build the bootloader, with ``make -C firmware bootloader``. If you are running a Debian based distribution, you can run:

	.. code-block:: sh

		apt-get install gcc-arm-none-eabi libnewlib-arm-none-eabi
		# only for the bootloader
		apt-get install libstdc++-arm-none-eabi-newlib

Otherwise it can be downloaded from `https://launchpad.net/gcc-arm-embedded <https://launchpad.net/gcc-arm-embedded>`__, just unpack the archive and add the bin directory to your PATH.

To build the firmware, start from the directory where you cloned or unpacked the source and run the following:

	.. code-block:: sh

		cd firmware/bluetooth_rxtx/
		make

This will produce a file named bluetooth_rxtx.dfu which can be written to the Ubertooth using

	.. code-block:: sh

		ubertooth-dfu -d bluetooth_rxtx.dfu -r

The ``btbr`` image is built the same way, in ``firmware/btbr``, and produces
``btbr.dfu``.

Although firmware source and binary images can be found in the release downloads, you will probably want fresh code from git if you are planning to modify the firmware.