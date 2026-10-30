==================================
Using Ubertooth with Wireshark
==================================

``btle-extcap.py`` is an external capture plugin for Wireshark. With it
installed, every attached Ubertooth appears in Wireshark's own capture list as
"Ubertooth One ``<serial>``", and starting a capture on one shows BLE
advertising packets in the packet window as they arrive. There is no fifo to
create, no second terminal running ``ubertooth-btle``, and no capture file in
between: Wireshark runs ``ubertooth-btle`` for you and reads the PCAP data it
writes.

The named-pipe method in :doc:`capturing_BLE_Wireshark` still works and is
worth knowing, since it takes the full range of ``ubertooth-btle`` options.
The plugin trades those for one click.

Installing it
~~~~~~~~~~~~~

1. **Install the tools and flash the board.** From your clone of this
   repository:

   .. code-block:: sh

      ./install.sh --flash-device all

   Add ``--skip-deps`` if the build dependencies are already installed.
   ``ubertooth-util -V`` should then show the same commit for the host tools
   and the firmware.

2. **Copy the plugin into Wireshark's Personal Extcap path.** Wireshark names
   the path itself, under **Help → About Wireshark → Folders**; it differs
   between versions and platforms, and Wireshark 4.4 and later look in a
   ``wireshark`` subdirectory of it.

   .. code-block:: sh

      mkdir -p <personal extcap path>
      cp host/python/extcap/btle-extcap.py <personal extcap path>/
      chmod +x <personal extcap path>/btle-extcap.py

   ``ubertooth-util`` and ``ubertooth-btle`` must be on the ``PATH``
   Wireshark starts the script with.

3. **Restart Wireshark**, so it runs the plugin and learns the interfaces.

Capturing
~~~~~~~~~

Pick the Ubertooth interface in the capture list and start the capture as you
would for any other interface. Advertising packets appear decoded as Bluetooth
LE Link Layer, and the stop button ends the capture with the packets still in
the window.

The capture runs ``ubertooth-btle`` in follow mode, so a connection whose setup
the board hears is followed across the data channels and its packets arrive in
the same window. :doc:`capturing_BLE_Wireshark` explains what that mode can and
cannot follow.

The gear icon next to the interface opens the options dialog, which offers
one setting: **Advertising Channel**, 37, 38 or 39. Ubertooth has a single
radio and listens on one channel at a time, so a device advertising on the
other two is heard only when it reaches the channel you chose. Channel 37 is
the default. A capture started again with another channel needs no restart of
Wireshark.

Capture filters are not supported
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``ubertooth-btle`` captures every advertising packet it hears and cannot apply
a capture filter. Type one into the capture-filter box for this interface and
the box turns red with "this interface does not apply capture filters; use a
display filter". Filter the captured packets instead, with a display filter,
for example:

.. code-block:: text

   btle.advertising_header.pdu_type == 0x00
   btle.advertising_address == aa:bb:cc:dd:ee:ff

:doc:`capturing_BLE_Wireshark` has more display filters worth keeping.

From tshark
~~~~~~~~~~~

``tshark`` uses the same plugin. ``tshark -D`` lists the Ubertooth interfaces
by name, and the extcap option can be given on the command line:

.. code-block:: sh

   tshark -D
   tshark -i ubertooth<serial> --channel 38 -w adverts.pcapng

Stop it with Ctrl-C. The interface name holds the board's serial, so it keeps
working across a replug or a reboot.

More detail
~~~~~~~~~~~

The ``btle-extcap(1)`` man page
(`host/doc/btle-extcap.md <https://github.com/SqueakyFloors/ubertooth/blob/main/host/doc/btle-extcap.md>`__)
covers every call Wireshark makes, the exit statuses, and the diagnostics to
read when a board does not appear in the capture list. Running the script by
hand is the first thing to try there:

.. code-block:: sh

   btle-extcap.py --extcap-interfaces
