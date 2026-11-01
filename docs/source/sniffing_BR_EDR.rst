Sniffing Classic Bluetooth
~~~~~~~~~~~~~~~~~~~~~~~~~~

Classic Bluetooth Basic Rate/Enhanced Data Rate (BR/EDR) sniffing works
differently from BLE. There is no
advertising channel to watch: a piconet hops across 79 channels, 1600 times a
second, in a sequence derived from the central's address and clock. Ubertooth
recovers those parameters from the air before it can follow a piconet.

Ubertooth's CC2400 radio supports Basic Rate using Gaussian Frequency-Shift
Keying (GFSK) at 1 Mbit/s only. Enhanced Data
Rate (EDR) payloads cannot be demodulated, and encrypted traffic cannot be
decrypted.


Bluetooth address parts
^^^^^^^^^^^^^^^^^^^^^^^

A Classic Bluetooth Device Address (BD_ADDR) such as ``22:44:66:88:AA:BB`` has three
parts:

.. list-table::
  :header-rows: 1

  * - Part
    - Bits
    - Example
    - How Ubertooth gets it
  * - Non-significant Address Part (NAP)
    - top 16
    - ``22:44``
    - Not transmitted in normal packets (see :ref:`finding-the-nap`)
  * - Upper Address Part (UAP)
    - next 8
    - ``66``
    - Calculated from captured packets (header error check / CRC)
  * - Lower Address Part (LAP)
    - low 24
    - ``88:AA:BB``
    - Read directly from every packet's access code

Normal packets in an established piconet do not carry the NAP. Their access
code is derived from the LAP; their hop sequence uses the LAP, the low bits of
the UAP, and the central's clock; and their header error check and CRC are
seeded with the UAP. Frequency Hop Synchronization packets used during inquiry
and paging are the exception: their payload can carry the full address.



Choosing the tool and firmware
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
  :header-rows: 1
  :widths: 25 20 55

  * - Tool
    - Firmware
    - Use it to
  * - ``ubertooth-rx``
    - ``bluetooth_rxtx``
    - Discover piconets by Lower Address Part (LAP) and Upper Address Part
      (UAP), recover the clock, and follow passively. Works on Linux and macOS.
  * - ``ubertooth-afh``
    - ``bluetooth_rxtx``
    - Detect the adaptive frequency hopping (AFH) channel map of a known piconet.
  * - ``ubertooth-scan``
    - ``bluetooth_rxtx``
    - Find undiscoverable devices with Ubertooth, then query them (name, version, features) with a normal BlueZ adapter. Linux only.
  * - ``ubertooth-follow``
    - ``bluetooth_rxtx``
    - Follow a piconet, given the target's address (``-t``), using a BlueZ
      adapter to help recover the clock. Linux only.
  * - ``ubertooth-btbr``
    - ``btbr``
    - Inquiry, paging, and *monitor* mode (capture a connection as it is set
      up, including the Frequency Hop Synchronization (FHS) packet). Experimental.

``bluetooth_rxtx`` is the standard firmware that every other tool, including
``ubertooth-btle``, needs. The ``btbr`` firmware is a separate image for
``ubertooth-btbr`` only. Flash it with
``ubertooth-dfu -d btbr/btbr.dfu -r`` (see :ref:`the-two-firmware-images`),
and flash ``bluetooth_rxtx.dfu`` again afterwards to use the other tools.
``ubertooth-util -V`` says which image a board is carrying, so it confirms
either switch:

	.. code-block:: sh

		$ ubertooth-util -U <serial> -V
		Host tools version: libubertooth <version> (git-26d4c11), libbtbb <version> (git-2fd0d39)
		libbtbb loaded from: /usr/local/lib/libbtbb.so.1
		Firmware version: btbr git-26d4c11 (API:<version>)
		Firmware build: ubertooth git-26d4c11 (builder@buildhost) Mon Oct 27 11:22:33 2025



Typical workflow
^^^^^^^^^^^^^^^^

#. **Survey** for nearby piconets. Survey mode records each Lower Address
   Part (LAP) it sees and calculates Upper Address Parts (UAPs) where it
   can. Devices only show up while they are actively communicating, so start
   some traffic (for example, play audio to a headset) during the survey:

	.. code-block:: sh

		ubertooth-rx -z -t 30

#. **Follow** a piconet. With only a Lower Address Part (LAP),
   ``ubertooth-rx`` calculates the Upper Address Part (UAP); with both, it
   skips straight to clock recovery. Write packets to a
   capture file for Wireshark:

	.. code-block:: sh

		ubertooth-rx -l 88aabb -u 66 -r capture.pcapng

   The clock search does not confirm a clock against a piconet that uses
   adapted frequency hopping (AFH), which current controllers do even when
   every channel is allowed, so following such a piconet does not start. The
   addresses and the packets the board hears are still recovered, and the
   capture above is still written. ``ubertooth-rx(1)``, THE CLOCK SEARCH AND
   ADAPTED HOPPING describes why, and what else reaches a clock.

#. **Check the Adaptive Frequency Hopping (AFH) map** if following is
   unreliable. Many piconets use AFH to
   avoid busy channels, which changes the hop sequence:

	.. code-block:: sh

		ubertooth-afh -l 88aabb -u 66

#. **Open the capture** in Wireshark. The Bluetooth Baseband (BTBB) and Basic
   Rate/Enhanced Data Rate (BR/EDR) dissectors are built
   into Wireshark 2.2 and newer; see the Wireshark plugins section of the
   :doc:`build_guide` for older versions. :doc:`bluetooth_captures_PCAP`
   describes the capture format.

See the man pages in ``host/doc/`` (`ubertooth-rx.md <https://github.com/SqueakyFloors/ubertooth/blob/main/host/doc/ubertooth-rx.md>`__, `ubertooth-afh.md <https://github.com/SqueakyFloors/ubertooth/blob/main/host/doc/ubertooth-afh.md>`__,
`ubertooth-scan.md <https://github.com/SqueakyFloors/ubertooth/blob/main/host/doc/ubertooth-scan.md>`__, `ubertooth-follow.md <https://github.com/SqueakyFloors/ubertooth/blob/main/host/doc/ubertooth-follow.md>`__) for all options.


.. _finding-the-nap:

Finding the Non-significant Address Part (NAP)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The Non-significant Address Part (NAP) is never sent in normal packets, so
``ubertooth-rx`` cannot recover it.
You need it wherever a tool asks for the whole address: ``ubertooth-follow -t``
and its ``-a`` both do. Some ways to get it:

- **Use a placeholder NAP to connect.** A page uses the paged device's LAP and
  UAP, 32 bits, and no more of its address: the LAP builds the access code that
  every packet of the exchange begins with, the LAP with the low four bits of
  the UAP selects the page hopping frequencies, and the whole 8-bit UAP seeds
  the header error check and the CRC of the FHS packet the pager sends
  (Bluetooth Core, Vol 2, Part B 1.2, 1.3, 6.3.3, 6.4.6, 7.1, and Table 2.3).
  The paged device's NAP is in no packet of the exchange; the only whole address
  that goes out is the pager's own, in that FHS packet (8.3.3.2).
  BlueZ's ``hci_create_connection()`` hands the controller all six bytes, and a
  page teaches the controller no NAP, so the address it reports back in its
  events is the one it was given. BlueZ's own address checks therefore match,
  and the reads that follow a page are keyed on the connection handle or on that
  same address. ``ubertooth-scan`` relies on this: it pages
  ``00:00:<UAP>:<LAP>`` through BlueZ to read the device's name, version, and
  features. It prints ``??:??`` where the NAP would be in its results, and
  ``00:00`` in the address line of the extended report, which is the address it
  really connected to.

  Three things a placeholder does not cover. The whole UAP is used, so a wrong
  UAP stops the page. Anything that pairs with the target or authenticates the
  link needs its real address; neither ``ubertooth-scan``'s reads nor
  ``ubertooth-follow -a``'s map read does. And a controller refuses to page its
  own address (8.3.2), so a placeholder that happened to match the local
  adapter's would fail. What a real controller makes of the guess has not been
  measured on this project's bench.
- **Narrow it by manufacturer.** The Non-significant Address Part (NAP) and
  Upper Address Part (UAP) together form the
  manufacturer's 24-bit Organizationally Unique Identifier (OUI)
  (``22:44:66`` above). Search the `Institute of Electrical and Electronics
  Engineers (IEEE) OUI registry
  <https://regauth.standards.ieee.org/standards-ra-web/pub/view.html#registries>`__
  for OUIs whose last byte equals the recovered UAP. Combined with the device's
  name or vendor, this often leaves a single candidate.
- **Capture a Frequency Hop Synchronization (FHS) packet.** The FHS packet
  carries the full Bluetooth Device Address (BD_ADDR), including
  the NAP (see below).
- **Read it from the device.** If you control a paired device, its Bluetooth
  settings show the full address.


Capturing Frequency Hop Synchronization (FHS) packets
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

FHS packets are brief discovery and connection-establishment traffic. Devices
send them as inquiry responses, and the central sends one to the peripheral
during paging. They contain the sender's full Bluetooth Device Address
(BD_ADDR) and clock. Because they are sent only during inquiry or paging,
passively following an established piconet with ``ubertooth-rx`` will not see
them.

``ubertooth-btbr`` monitor mode targets the paging phase. Flash the ``btbr``
firmware, then watch for pages to a peripheral whose address you know:

	.. code-block:: sh

		ubertooth-btbr
		> monitor 11:22:33:44:55:66

With more than one board attached, name the one carrying ``btbr`` with
``-U <index|serial>``, as the other tools take it; without it the tool lists
the attached boards and opens none.

Then connect to that peripheral from another device. If Ubertooth receives the
FHS before the real peripheral does, it synchronizes to the new piconet and
prints the FHS and the unencrypted Link Manager Protocol (LMP) traffic that
follows. If no FHS appears,
the peripheral responded first; try connecting again.

Limitations:

- This is a race and often takes several attempts.
- Packets sent after encryption is enabled are shown as raw data and cannot be decrypted.
- ``ubertooth-btbr`` needs the ``btbr`` image. On a board carrying
  ``bluetooth_rxtx`` it stops before its prompt with
  ``error: could not set up``, naming the board it got and the image to
  flash: that
  firmware never reads the endpoint the tool writes to, which is what the
  reports of the tool hanging shortly after start (upstream issue
  `#520 <https://github.com/greatscottgadgets/ubertooth/issues/520>`__) turned
  out to be.

`ubertooth-btbr.md <https://github.com/SqueakyFloors/ubertooth/blob/main/host/doc/ubertooth-btbr.md>`__ documents the other prompt commands
(``inquiry``, ``page``, ``inquiry_scan``, ``page_scan``, ``l2ping``,
``bdaddr``, ``idle``), the options, and the limitations.
