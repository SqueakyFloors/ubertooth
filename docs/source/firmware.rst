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
works from both images. A tool run against the wrong image is refused rather
than left waiting: the other tools report a firmware API version they do not
accept, and ``ubertooth-btbr`` says it could not set the Ubertooth up.

``ubertooth-util -v`` names the image a board is carrying, so it is how you
check which one is on there now, or that a switch worked:

	.. code-block:: sh

		$ ubertooth-util -U <serial> -v
		Firmware version: bluetooth_rxtx git-26d4c11 (API:1.33)
		$ ubertooth-util -U <serial> -v     # the same board, flashed with btbr
		Firmware version: btbr git-26d4c11 (API:1.06)

``ubertooth-dfu`` names what is in a firmware file before it writes anything,
so the ``Image:`` line says which image is about to be flashed, and a file that
is not an Ubertooth image is refused before the board is touched:

	.. code-block:: sh

		$ ubertooth-dfu -U <serial> -d firmware/btbr/btbr.dfu -r
		Checking firmware signature
		Image: btbr git-26d4c11 (API:1.33)

The two API versions above are different numbers: the ``Image:`` line takes
its version from the file, while ``-v`` reports the USB ``bcdDevice`` the
running firmware answers with, which ``btbr`` fixes at 1.06. See
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
		Image: bluetooth_rxtx git-ed30131d* (API:1.33)
		Switching selected device to DFU mode...
		........................................
		........................................
		................
		Detached
		Verified firmware revision: git-2671234

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
(``Image: bluetooth_rxtx git-ed30131d* (API:1.33)``), and refuses a file that
carries no such block, which includes every image built before the block
existed.

After flashing with ``-d ... -r``, the tool takes the expected revision from
that block, waits for the application to return on the same physical USB path,
and requires the revision the board reports to match before returning success.
If a programming transfer fails while the device remains in DFU, it reopens
that same path and retries the complete image once from block zero.

The reset request itself often fails, because the bootloader disconnects as it
resets rather than acknowledging the request first. That is not a failure: the
tool looks for the DFU device on the same physical USB path again, carries on
when it has gone, and reports the board as still in DFU only when it is still
there. An ``error: control message unsupported`` line now comes only from the
DFU state queries, where it means the bootloader did not answer and the
command fails.
The command returns success only after the expected application revision has
been verified. If verification fails, use the reported stage and error details
or the documented manual bootloader recovery path.

Troubleshooting:

If you run into an error such as "libUSB Error: Command Error: (-1)" or the Ubertooth's 4 LEDs next to each other perform a distinctive chasing pattern, after extracting the archive of the latest Ubertooth release, change into the ``firmware`` directory. Then, run:

	.. code-block:: sh

		make clean all && make
		ubertooth-dfu -r -d bluetooth_rxtx/bluetooth_rxtx.dfu

If your Ubertooth One has no firmware or broken firmware and doesn't show up when issuing ``lsusb``, force it into DFU mode by bridging pins 1 and 3 on the EXPAND header (P4) while the device is unplugged, then plug it in with the bridge still connected:

	.. image:: ../images/u1_roadmap.png

Wait for the distinctive LED sequence or USB ID ``1d50:6003``, then remove the bridge and immediately run ``ubertooth-dfu``. The entry pin is sampled only during reset, so removing the bridge after DFU starts does not exit the bootloader. Remove it before the final reset; otherwise the next boot returns to DFU. If removing the live bridge is inconvenient, finish flashing, unplug the device, remove the bridge, and reconnect it.

On Linux, verify the bootloader USB ID before flashing:

	.. code-block:: sh

		$ lsusb -d 1d50:6003
		Bus 001 Device 007: ID 1d50:6003 OpenMoko, Inc. USB Bootloader

Then run the DFU command immediately.



.. _what-version-am-i-running:

What Version Am I Running? 
^^^^^^^^^^^^^^^^^^^^^^^^^^

In non-DFU mode, you can obtain firmware information with ``ubertooth-util -v``. Note that the version shown should match the one you just installed:

	.. code-block:: sh

		$ ubertooth-util -v
		Firmware version: bluetooth_rxtx git-ed30131d* (API:1.33)
		$ ubertooth-util -V
		bluetooth_rxtx: ubertooth git-ed30131d* (builder@host) Tue Aug  7 15:33:06 PDT 2018

Both lines start with the image the board is running, read from its firmware
info block; a board running firmware from before the block shows the revision
alone. A firmware built from a git checkout reports its git revision as
above, with a trailing ``*`` if the working tree had uncommitted changes; a
release build reports the release name instead. The version is
generated into a ``version.h`` at build time and tracks the exact source it was
built from, so ``-v`` always reflects the firmware actually running.



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