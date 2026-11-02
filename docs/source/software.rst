========
Software
========

Building and Running Ubertooth Software
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For information on how to download, build and install the Ubertooth software and dependencies, see the `build guide <https://ubertooth.readthedocs.io/en/latest/build_guide.html>`__.

For an introduction on the capabilities of Ubertooth and how to use it, see the `getting started <https://ubertooth.readthedocs.io/en/latest/getting_started.html>`__ page.



Command-line tools
~~~~~~~~~~~~~~~~~~

The host tools, grouped by purpose. "Needs" lists what is required beyond the
Ubertooth itself. Every tool has a manual page under ``host/doc/``, which is
the full reference for its options.

**BLE**

- ``ubertooth-btle`` — Passive BLE monitoring: follow connections (``-f``),
  capture advertising only (``-n``), or sniff a connection that is already up
  (``-p``). (Needs: ambient BLE traffic.) The most commonly used tool.
- ``btle-extcap.py`` — An external capture program that lets Wireshark capture
  BLE advertising from an Ubertooth directly, by running ``ubertooth-btle`` for
  it; see ``btle-extcap(1)`` (``host/doc/btle-extcap.md``). (Needs:
  Wireshark.)

**Classic Bluetooth (BR/EDR)**

- ``ubertooth-rx`` — Passive Classic BT discovery/decode; finds piconets and
  their LAP/UAP. Saves raw captures (``-d``) and replays them with their
  recorded times, no board attached (``-i``). (Needs: a Classic BT peer.)
- ``ubertooth-afh`` — Passively recover a piconet's AFH channel map. (Needs: an
  active piconet.)
- ``ubertooth-follow`` — Active clock discovery and follow a piconet for a given
  target address. **Linux only**: it drives an ordinary Bluetooth adapter
  through BlueZ. (Needs: BlueZ and a target.)
- ``ubertooth-scan`` — Active inquiry scan using a BlueZ dongle plus Ubertooth.
  **Linux only**, for the same reason. (Needs: a BlueZ Bluetooth dongle.)

- ``ubertooth-btbr`` — Take part in Classic BT: inquiry, paging, *monitor*
  mode, which captures a connection as it is set up, and transmitting chosen
  packet types on a connection. **Experimental**, and the
  only tool that needs the separate ``btbr`` firmware image rather than
  ``bluetooth_rxtx`` (see :ref:`the-two-firmware-images` and
  :doc:`sniffing_BR_EDR`). (Needs: the ``btbr`` image and a Classic BT peer.)

On macOS neither of those two BlueZ tools is built, and neither is planned:
the adapter operations they are built around — a clock read and the AFH map
for ``ubertooth-follow``, the remote version and features for
``ubertooth-scan -x`` — are not in macOS's public Bluetooth APIs. Use
``ubertooth-rx`` and ``ubertooth-afh`` there, which cover much of the same
ground by listening and need no adapter; ``COMPATIBILITY.md`` compares them
operation by operation.

**Signal / low-level**

- ``ubertooth-specan`` — Spectrum analyzer: a stream of signal strengths across
  2.4 GHz. (Needs: nothing — reads ambient RF.)
- ``ubertooth-specan-ui`` — Graphical live spectrum plot of 2.4 GHz, built on
  ``ubertooth-specan``; see :doc:`specan_ui`. (Needs: Python 3.8+ with numpy,
  qtpy, and PySide6 or PyQt5.)
- ``ubertooth-dump`` — Raw stream of received bits/symbols off the radio,
  demodulated as Bluetooth or, experimentally, with a configuration you wrote
  yourself (``-g``).
- ``ubertooth-debug`` — Low-level register reads on the CC2400 radio, and
  experimental register writes (``-w``).

**Device administration and firmware**

- ``ubertooth-util`` — Device admin: the version (``-V``: host tools,
  firmware version and firmware build; ``-v`` is the older, deprecated
  spelling of its firmware version line), LEDs, reset, channel, device count
  (``-N``), watchdog state and why the board last reset itself (``-w``).
  Reflashing is ``ubertooth-dfu``'s job.
- ``ubertooth-dfu`` — Flash or read firmware over DFU.

**Special-purpose**

- ``ubertooth-ducky`` — Drive an Uberducky BLE HID-injection gadget. (Needs:
  Uberducky hardware.)
- ``ubertooth-ego`` — Sniff a Yuneec E-GO skateboard. (Niche; needs: a Yuneec
  E-GO.)
- ``ubertooth-tx`` — Transmit one generic packet with ``-g``: a payload given
  in hex, framed by the radio with the sync word ``-a`` gives it, which a
  second board reads with ``ubertooth-dump -g``. (Experimental.) Retransmitting
  symbols dumped with ``ubertooth-dump`` is **still non-functional**.



Developing Host Code
~~~~~~~~~~~~~~~~~~~~

Host code is the software running on a host computer to which an Ubertooth One is attached via USB.

The host code is built with GCC or Clang on Linux and macOS, and this fork's
continuous integration builds and tests both on every change. ``COMPATIBILITY.md``
in the top of the tree records which platforms, compilers, and Python versions
have actually been exercised, and which are not supported. If you find a bug,
`please let us know
<https://github.com/SqueakyFloors/ubertooth/issues>`__.

Although source can be found in the `release downloads <https://github.com/SqueakyFloors/ubertooth/releases>`__, you will probably want fresh code from `git <https://github.com/SqueakyFloors/ubertooth>`__ if you plan to develop new code.