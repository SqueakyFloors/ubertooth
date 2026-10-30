=============
Ubertooth One
=============

Ubertooth One is the hardware platform of Project Ubertooth: a USB 2.4 GHz
radio that plugs into a host running the Ubertooth tools.

	.. image:: ../images/ubertooth-one.jpeg



Architecture
~~~~~~~~~~~~

* `RP-SMA <https://en.wikipedia.org/wiki/SMA_connector>`__ RF connector: connects to an antenna, test equipment, or a dummy load.

* `CC2591 <https://www.ti.com/product/CC2591>`__ RF front end.

* `CC2400 <https://www.ti.com/product/CC2400>`__ wireless transceiver.

* `LPC175x <https://www.nxp.com/products/processors-and-microcontrollers/arm-microcontrollers/general-purpose-mcus/lpc1700-arm-cortex-m3:MC_1403790745385>`__ ARM Cortex-M3 microcontroller with Full-Speed USB 2.0.

* USB A plug: connects to a host computer running the Ubertooth tools.


The processor is an LPC1754 or an LPC1756 depending on the build. The schematic
calls for an LPC1754, with 128 KB of flash; boards in the field are a mix, and
the LPC1756, with 256 KB, is the other part they are built with. The bootloader
reads the part identification number and sizes flash from it, so the same
firmware image serves either. Both parts have 32 KB of RAM in two 16 KB banks
that are not contiguous: the main SRAM at ``0x10000000``, which the firmware
runs from, and the AHB SRAM at ``0x2007C000``.



What the radio can and cannot do
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* Transmit and receive in the 2.4 GHz band, with transmit power and receive sensitivity comparable to a Class 1 Bluetooth device.

* One 1 MHz channel at a time. The receiver tunes a single channel, so following a hopping connection means retuning to each channel in turn rather than listening to the whole band at once.

* BLE on the 1M PHY only. The CC2400 cannot receive the Bluetooth 5 LE 2M or Coded (long range) PHYs, so it cannot capture extended advertising data, and it loses a connection that switches to 2M.

* Classic Bluetooth at Basic Rate only. The CC2400 radio demodulates only GFSK, so no firmware can capture EDR payloads, which use phase modulation and carry most audio and fast data; their access code and header are still seen.



Connectors and LEDs
~~~~~~~~~~~~~~~~~~~

.. image:: ../images/u1_roadmap.png

This diagram shows the LEDs and the pins of the expansion connector.

* RP-SMA antenna jack.

* Standard Arm Cortex Debug Connector (10-pin 50-mil JTAG).

* In-System Programming (ISP) serial connector.

* Expansion connector: intended for inter-Ubertooth communication or other future uses. Two of its pins are also where a jumper forces bootloader entry; see :doc:`programming`.

* Six LEDs. The firmware drives three of them (USR, RX and TX); the other three are power and status indicators it cannot control. :doc:`hardware_leds` says what each one means, which tool commands drive them, and how to read the fault codes a faulted board blinks.



Design
~~~~~~

Ubertooth One was designed in `KiCad <https://www.kicad.org/>`__, an open
source electronic design automation package, with surface mount components
suitable for reflow soldering. The design files are in this tree, under
``hardware/ubertooth-one``:
`ubertooth-one.sch <https://github.com/SqueakyFloors/ubertooth/blob/main/hardware/ubertooth-one/ubertooth-one.sch>`__
is the schematic and
`ubertooth-one.brd <https://github.com/SqueakyFloors/ubertooth/blob/main/hardware/ubertooth-one/ubertooth-one.brd>`__
the board layout, both in the file format KiCad used in 2011. Board makers
build Ubertooth One hardware from these files.

The board is a four-layer 1.6 mm PCB, copper order Front, Inner3, Inner2,
Back: 35 um copper, 0.35 mm dielectric, 18 um, 0.76 mm, 18 um, 0.35 mm, 35 um,
on DE104iML or an equivalent substrate (Er=4.42 at 2.4 GHz, TanD=0.016). Both
sides carry a black solder mask and a white silkscreen. Minimum trace width and
isolation are both 6 mil.



Power Usage
~~~~~~~~~~~

These measurements were taken using Ubertooth One running firmware revision 2014-02-R2. Zero_Chaos took these measurements using a Centech CT-USB-PW.

.. list-table :: 
  :header-rows: 1
  :widths: 1 1 1

  * - Command
    - Description
    - Power draw (amps)
  * -
    - Idle
    - 0.09A
  * - ``ubertooth-dump``
    - Receive
    - 0.13A
  * - ``ubertooth-util -t``
    - Transmit
    - 0.22A
  * - ``ubertooth-dfu -d``
    - Firmware upgrade
    - 0.10A


Demonstration
~~~~~~~~~~~~~

Michael Ossmann presented `Project Ubertooth: Building a Better Bluetooth Adapter <https://archive.org/details/sc2011_Project_Ubertooth>`__ at ShmooCon 2011. The recording is on the Internet Archive.
