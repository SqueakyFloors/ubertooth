Capturing BLE in Wireshark
~~~~~~~~~~~~~~~~~~~~~~~~~~

You can capture BLE in Wireshark with standard Wireshark builds. This guide assumes Linux.

    #. Run the command: ``mkfifo /tmp/pipe``
    #. Open Wireshark
    #. Click Capture -> Options
    #. Click "Manage Interfaces" button on the right side of the window
    #. Click the "New" button
    #. In the "Pipe" text box, type "/tmp/pipe"
    #. Click Save, then click Close
    #. Click "Start"

In a terminal, run ``ubertooth-btle``:

	.. code-block:: sh

		ubertooth-btle -f -q /tmp/pipe

In the Wireshark window you should see packets scrolling by.

``-q`` writes ``LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR``, which Wireshark dissects
itself, so nothing needs configuring first; it is also what the Wireshark
plugin in :doc:`using_ubertooth_with_wireshark` uses. ``-c`` writes the older
PPI layout instead, for tools that expect it, and ``-r`` writes PcapNG, which
cannot go into a pipe because its writer seeks back over what it has written.
:doc:`bluetooth_captures_PCAP` describes what each format records.

Note: a capture whose link type Wireshark has no dissector mapped for gives a
Data Link Type (DLT) error such as
``User encapsulation not handled: DLT=147, check your Preferences->Protocols->DLT_USER``
(see `issue #61 <https://github.com/greatscottgadgets/ubertooth/issues/61>`__).
The ``-q`` and ``-c`` captures above are not in that position, but if you meet
the error on an older capture, follow these steps:

    #. Click Edit -> Preferences
    #. Click Protocols -> DLT_USER
    #. Click Edit (Encapsulations Table)
    #. Click New
    #. Under DLT, select "User 0 (DLT=147)" (adjust this selection as appropriate if the error message showed a different DLT number than 147)
    #. Under Payload Protocol, enter: btle
    #. Click OK
    #. Click OK



Capturing BLE in Scapy
^^^^^^^^^^^^^^^^^^^^^^
#. Run ubertooth-btle to capture data

  .. code-block:: sh

    ubertooth-btle -n -c /tmp/bluesCap

#. Open python and run:

  .. code-block:: py

    import time
    from scapy.all import *
    snarf = PcapReader('/tmp/bluesCap')
    pkts = [i for i in snarf]

    ## Sleep for 3 seconds and then grab from the generator
    time.sleep(3)
    pkts = pkts + [i for i in snarf]



Sniffing connection data
^^^^^^^^^^^^^^^^^^^^^^^^

What Ubertooth can capture
""""""""""""""""""""""""""

BLE uses 3 advertising channels (37, 38, 39) and
37 data channels. Devices
advertise on the advertising channels; once a central connects, the two devices
hop across the data channels using parameters agreed in the connection request
(``CONNECT_IND``, called ``CONNECT_REQ`` in older specs). Ubertooth has a
single radio, so it can listen on only one channel at a time.

``ubertooth-btle`` has three sniffing modes; use one at a time:

.. list-table::
  :header-rows: 1
  :widths: 10 90

  * - Flag
    - Behavior
  * - ``-f``
    - **Follow mode (recommended).** Listen on one advertising channel and log advertisements. When a ``CONNECT_IND`` is seen, follow that connection across the data channels and log its packets, then return to advertising when it ends.
  * - ``-n``
    - **No-follow.** Log advertising packets only; never follow connections.
  * - ``-p``
    - **Promiscuous (experimental).** Try to recover and follow a connection that is *already* established, by inferring its parameters from data-channel traffic. Slower and less reliable than follow mode, and limited to connections that hop with Channel Selection Algorithm 1: a connection between Bluetooth 5 devices normally uses Algorithm 2 and cannot be recovered this way.

Why you only see advertisements
"""""""""""""""""""""""""""""""

Follow mode can only follow a connection whose setup it observes. To follow
a connection, Ubertooth must receive the ``CONNECT_IND`` packet, which holds
the access address, hop increment, timing, and CRC
seed for that connection.
Common reasons for seeing only advertising traffic:

- **The connection already existed** when capture started. Ubertooth cannot join it in follow mode. Disconnect and reconnect the devices while ``ubertooth-btle -f`` is running. Promiscuous mode, ``-p``, recovers a connection that is already running, but only one that hops with Channel Selection Algorithm 1, so it does not help with a connection between Bluetooth 5 devices.
- **The connection request was sent on a different advertising channel.** Ubertooth listens on one channel (37 by default, selectable with ``-A 37|38|39``), and the central sends ``CONNECT_IND`` on whichever channel it heard the advertisement. Each connection attempt has roughly a one-in-three chance of happening on the channel being monitored, so you may need to reconnect several times.
- **Weak signal.** Move Ubertooth closer to the devices; a missed ``CONNECT_IND`` or missed data packets cause the follow to fail.
- **Unsupported link features.** Ubertooth implements the Bluetooth Low
  Energy 1M physical layer (PHY) only. A connection that negotiates the 2M
  or Coded PHY modulates differently and will not be followed. Follow mode
  hops with either channel selection algorithm, including Algorithm 2, which
  Bluetooth 5 devices normally use.

Encrypted connections are captured, but their payloads are encrypted. The
`crackle <https://github.com/mikeryan/crackle>`__ tool can recover keys only
for Bluetooth Low Energy Legacy pairing captured in full, not Bluetooth Low
Energy Secure Connections.

Following a specific device
"""""""""""""""""""""""""""

By default, follow mode follows *any* connection it sees. To follow only
connections involving one device, first identify its address from the
advertisements (``ubertooth-btle -n``), then set a target filter and start
follow mode:

	.. code-block:: sh

		ubertooth-btle -t aa:bb:cc:dd:ee:ff     # set the filter (stored on the device)
		ubertooth-btle -f -q /tmp/pipe          # follow; only matching connections are followed

``-t`` can be combined with ``-f`` in one command and accepts a mask length.
For example, ``-t 00:1A:7D:00:00:00/24`` matches an Organizationally Unique
Identifier (OUI). The filter persists on
the Ubertooth until it is cleared or the device is reset. While a filter is
set, advertisements from other devices are dropped too. Clear it with:

	.. code-block:: sh

		ubertooth-btle -t none

See the ``ubertooth-btle`` man page (`host/doc/ubertooth-btle.md <https://github.com/SqueakyFloors/ubertooth/blob/main/host/doc/ubertooth-btle.md>`__) for all options.



Capturing from a remote host
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

You can use `sshdump <https://www.wireshark.org/docs/man-pages/sshdump.html>`__
over Secure Shell (SSH) to capture packets remotely from an Ubertooth attached
to another host or virtual machine. In the Wireshark user interface (UI), this
may appear as an interface named "SSH remote capture: ``sshdump``". Configure it
with the following "remote capture command":

	.. code-block:: sh

		killall ubertooth-btle; unlink /tmp/btlepipe; mkfifo /tmp/btlepipe; ubertooth-btle -f -q /tmp/btlepipe &>/dev/null & cat /tmp/btlepipe

The "remote interface" option is ignored and can be set to any value.



Useful display filters
^^^^^^^^^^^^^^^^^^^^^^

Only connection requests and non-zero data packets:

	.. code-block:: sh

		btle.data_header.length > 0 || btle.advertising_header.pdu_type == 0x05

Only attribute read responses, write requests, and notifications:

	.. code-block:: sh

		btatt.opcode in { 0x0b 0x12 0x1b }
