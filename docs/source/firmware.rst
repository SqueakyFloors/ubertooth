========
Firmware
========

How To Update Firmware
~~~~~~~~~~~~~~~~~~~~~~

First, grab the `latest Ubertooth release <https://github.com/greatscottgadgets/ubertooth/releases/latest>`__. Then, extract the archive and change into directory ``ubertooth-one-firmware-bin``.

You may then run the ``ubertooth-dfu`` command like so:

	.. code-block:: Sh

		$ ubertooth-dfu -d bluetooth_rxtx.dfu -r
		Switching selected device to DFU mode...
		Checking firmware signature
		........................................
		........................................
		................

The device will automatically enter DFU mode and flash the firmware. During the
application-to-DFU transition, the tool checks the entry request, follows the
selected device by its physical USB path, and makes bounded retries if the
application returns without entering DFU. With multiple devices already in DFU,
it fails rather than choosing one ambiguously.

After flashing with ``-d ... -r``, the tool extracts the expected revision from
the image, waits for the application to return on the same physical USB path,
and requires the reported revision to match before returning success. If a
programming transfer fails while the device remains in DFU, it reopens that same
path and retries the complete image once from block zero. Project firmware
images include the compile-information marker used for this verification.

The reset request itself often fails, because the bootloader disconnects as it
resets rather than acknowledging the request first. That is not a failure: the
tool looks for the DFU device on the same physical USB path again, carries on
when it has gone, and reports the board as still in DFU only when it is still
there. A ``control message unsupported`` line now comes only from the DFU state
queries, where it means the bootloader did not answer and the command fails.
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



What Version Am I Running? 
^^^^^^^^^^^^^^^^^^^^^^^^^^

In non-DFU mode, you can obtain firmware information with ``ubertooth-util -v``. Note that the version shown should match the one you just installed:

	.. code-block:: sh

		$ ubertooth-util -v
		Firmware version: 2018-12-R1 (API:1.05)
		$ ubertooth-util -V
		ubertooth 2018-12-R1 (mikeryan@steel) Tue Aug  7 15:33:06 PDT 2018

A firmware built from a git checkout reports its git revision instead, e.g.
``git-1a2b3c4`` (with a trailing ``*`` if the working tree had uncommitted
changes); release archives report the release name as above. The version is
generated into a ``version.h`` at build time and tracks the exact source it was
built from, so ``-v`` always reflects the firmware actually running.



Developing Firmware
^^^^^^^^^^^^^^^^^^^



You'll need a toolchain that supports ARM Cortex-M3. The Makefiles in the firmware directory are designed for GCC and a Linux-based toolchain, specifically ``arm-none-eabi-gcc and libnewlib-arm-none-eabi``. If you are running a Debian based distribution, you can run:

	.. code-block:: sh

		apt-get install gcc-arm-none-eabi libnewlib-arm-none-eabi

Otherwise it can be downloaded from `https://launchpad.net/gcc-arm-embedded <https://launchpad.net/gcc-arm-embedded>`__, just unpack the archive and add the bin directory to your PATH.

To build the firmware, start from the directory where you cloned or unpacked the source and run the following:

	.. code-block:: sh

		cd firmware/bluetooth_rxtx/
		make

This will produce a file named bluetooth_rxtx.dfu which can be written to the Ubertooth using

	.. code-block:: sh

		ubertooth-dfu -d bluetooth_rxtx.dfu -r

Although firmware source and binary images can be found in the release downloads, you will probably want fresh code from git if you are planning to modify the firmware.