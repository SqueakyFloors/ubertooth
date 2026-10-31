===========
Programming
===========

This page describes how to load compiled firmware onto an Ubertooth One. Other pages describe how to build and write the :doc:`firmware` or :doc:`host code <software>` for the platform.

There are three ways to program an Ubertooth board: the USB bootloader, the ISP bootloader, and JTAG.

Please note that it is not possible to replace the board's USB bootloader via ubertooth-dfu, because the bootloader accepts writes only from address 0x4000, where the application region starts, and reads back only from there as well. Installing a bootloader needs ISP or JTAG.



USB bootloader
~~~~~~~~~~~~~~

This is the recommended method of loading code onto an Ubertooth Zero or Ubertooth One provided that the USB bootloader is already installed (at the time of manufacture, for example). If you need to install the bootloader itself, you will have to use either ISP or JTAG.

The bootloader executes every time the device starts up from reset or power cycle. Normally it just gets out of the way and passes control to the application firmware very quickly. Alternatively it can enter DFU mode which permits firmware upload and download over USB. There are two ways to tell the bootloader that you want it to enter DFU mode:

    #. soft bootloader entry: By setting a flag in RAM, the application firmware can instruct the bootloader to enter DFU mode following a reset (without loss of power). Both firmware images this tree builds, ``bluetooth_rxtx`` and ``btbr``, answer that request, and ``ubertooth-dfu`` triggers the reset itself as part of a write (``-d``) or a read (``-u``), so no separate step is needed. Soft entry only works on Ubertooth One, not Ubertooth Zero.

    #. hard bootloader entry (also known as pin entry): By connecting two pins on the expansion header with a jumper during reset (either soft or hard boot), you can force the bootloader into DFU mode. When using pin entry, the bootloader enters DFU mode for only a few seconds; if no DFU activity has started in that time it resets the board rather than starting the application. The entry pin is sampled again after that reset, so a jumper left in place gives a DFU opportunity on every reset, and the board stays in the bootloader until the jumper is removed. Remove it before the final reset if you want the application firmware to boot. This method works on Ubertooth One but not Ubertooth Zero.

The bootloader indicates DFU mode by flashing the LEDs in a distinctive pattern. It also identifies itself as "usb_bootloader" on USB.

During DFU mode, ``ubertooth-dfu -d <file>.dfu`` writes an image to the board and ``ubertooth-dfu -u <file>`` reads the application flash region back to a raw file. The read-back carries no DFU suffix, so it is not a file ``-d`` can write back.

It is possible that soft bootloader entry may be broken by installing a faulty application or an application that does not provide a method of triggering soft entry. In this case, pin entry must be used (e.g. by holding a paper clip in the expansion header while plugging in the device) to "unbrick" the unit.

For Ubertooth One, the pins to connect are pins 1 and 3 of the expansion header (P4). For Ubertooth Zero, the pins to connect are pins 1 and 13 of the expansion header (J1).



ISP bootloader
~~~~~~~~~~~~~~

The LPC175x features an In-System Programming (ISP) bootloader that allows code to be loaded over a serial interface.

If the board's application firmware still runs, ``ubertooth-util -i`` hands the chip to its on-chip programmer over USB, which saves forcing ISP entry from the programmer side. The board answers nothing else after that until its power is cut, so unplug it or power-cycle its hub port when you are done. A board whose application no longer runs has to be put into ISP from the programmer, as the modifications below describe.

Either way you will need `lpc21isp <http://sourceforge.net/projects/lpc21isp>`__ and a 3.3V serial programming device such as one of the following:



Pogoprog
^^^^^^^^

The official Ubertooth ISP programmer is Pogoprog, an open source board that can be assembled using a process similar to Ubertooth One assembly.



An FTDI Basic Breakout - 3.3V
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

You can use SparkFun's `FTDI Basic Breakout - 3.3V <http://www.sparkfun.com/products/9873>`__ to program an Ubertooth. Adafruit's `FTDI Friend <http://www.adafruit.com/index.php?main_page=product_info&products_id=284>`__ is a similar board that should work as well. To allow lpc21isp to automatically activate the LPC175x's ISP bootloader, you must modify the board in one of two ways.



Method one: the easy way
++++++++++++++++++++++++

Just short the CTS pin to GND. This will force the microcontroller into ISP mode every time it resets while the programmer is connected. In order to perform a normal reset to run the newly loaded code, you will have to first disconnect the programmer.



Method two: the better way
++++++++++++++++++++++++++

Connect the CTS pin to the RTS pin (pin 3) on the FT232RL using fine gauge wire such as wire wrap wire. This is trickier to solder, but it has the advantage that the pin will be fully controlled by lpc21isp. This means that you can leave the programmer connected to the Ubertooth board throughout multiple program/test cycles.



Using ISP
+++++++++

.. image:: ../images/pogoprog-placement.jpeg

Warning: Code loaded via ISP will blow away the bootloader. It is generally recommended to use this method only for installing the bootloader itself.

Connect your serial programmer to the Ubertooth board and run ``make program`` in the directory of the image you are installing — ``firmware/bootloader`` for the bootloader. The target is per-image, so the top level ``firmware`` directory does not have it. It calls lpc21isp on that image's ``.hex`` file and writes to ``/dev/ttyUSB0``; ``make program PROGDEV=/dev/ttyUSB1`` picks another serial port, and ``LPCISP=`` another lpc21isp.

If you have a precompiled binary in .hex format, you can invoke lpc21isp directly with ``lpc21isp -control firmware.hex /dev/ttyUSB0 230400 4000``, replacing ``firmware.hex`` with the firmware filename and ``/dev/ttyUSB0`` with the device file of your serial programmer.



JTAG
^^^^

Warning: Code loaded via JTAG will blow away the bootloader. It is generally recommended to use this method only for installing the bootloader itself.

Connect your ARM Cortex-M3 JTAG debugger (such as one supported by `OpenOCD <https://openocd.org>`__) to the standard Cortex Debug Connector on the Ubertooth One or the non-standard JTAG connector on the Ubertooth Zero. You know what to do.
