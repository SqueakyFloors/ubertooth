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
Ubertooth itself. Most tools also have a manual page under ``host/doc/``.

**BLE**

- ``ubertooth-btle`` — Passive BLE monitoring; follow connections / capture
  advertising with ``-f``. (Needs: ambient BLE traffic.) The most commonly used
  tool.
- ``btle-extcap.py`` — An external capture program that lets Wireshark capture
  BLE advertising from an Ubertooth directly, by running ``ubertooth-btle`` for
  it; see ``host/python/extcap/README.md``. (Needs: Wireshark.)

**Classic Bluetooth (BR/EDR)**

- ``ubertooth-rx`` — Passive Classic BT discovery/decode; finds piconets and
  their LAP/UAP. (Needs: a Classic BT peer.)
- ``ubertooth-afh`` — Passively recover a piconet's AFH channel map. (Needs: an
  active piconet.)
- ``ubertooth-follow`` — Active clock discovery and follow a piconet for a given
  UAP/LAP. (Needs: BlueZ and a target.)
- ``ubertooth-scan`` — Active inquiry scan using a BlueZ dongle plus Ubertooth.
  (Needs: a BlueZ Bluetooth dongle.)

**Signal / low-level**

- ``ubertooth-specan`` — Spectrum analyzer: a stream of signal strengths across
  2.4 GHz. (Needs: nothing — reads ambient RF.)
- ``ubertooth-specan-ui`` — Graphical live spectrum plot of 2.4 GHz, built on
  ``ubertooth-specan``; see :doc:`specan_ui`. (Needs: Python 3.8+ with numpy,
  qtpy and PySide6 or PyQt5.)
- ``ubertooth-dump`` — Raw stream of received bits/symbols off the radio.
- ``ubertooth-debug`` — Low-level register/debug access to the CC2400 radio.

**Device administration and firmware**

- ``ubertooth-util`` — Device admin: version flags (``-v`` firmware revision,
  ``-V`` firmware compile info, ``-H`` / ``--host-version`` host tools version),
  LEDs, reset, channel, DFU.
- ``ubertooth-dfu`` — Flash or read firmware over DFU.

**Special-purpose**

- ``ubertooth-ducky`` — Drive an Uberducky BLE HID-injection gadget. (Needs:
  Uberducky hardware.)
- ``ubertooth-ego`` — Sniff a Yuneec E-GO skateboard. (Niche.)
- ``ubertooth-tx`` — Retransmit symbols dumped with ``ubertooth-dump``.
  **Currently non-functional** (retained as a placeholder).



Developing Host Code
~~~~~~~~~~~~~~~~~~~~

Host code is the software running on a host computer to which an Ubertooth One is attached via USB.

The Ubertooth developers use GCC on Linux. We aim to have the host code work on OSX systems, but we cannot guarantee it; if you find a bug, `please let us know <https://github.com/greatscottgadgets/ubertooth/issues>`__. Theoretically, host code could be written on or for other platforms, but it hasn't been done yet.

Although source can be found in the `release downloads <https://github.com/greatscottgadgets/ubertooth/releases>`__, you will probably want fresh code from `git <https://github.com/greatscottgadgets/ubertooth>`__ if you plan to develop new code.