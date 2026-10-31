===============
Getting Started
===============

There are three major components of Project Ubertooth:

    * hardware: The hardware design of Ubertooth One is quite stable. You can :doc:`build one <assembling_hardware>` or `buy one <http://greatscottgadgets.com/ubertoothone>`__.
    * firmware: This is software that executes on the ARM processor on the Ubertooth One itself. This page assumes that you have the USB bootloader plus bluetooth_rxtx firmware installed on your board. The host tools require firmware built from the same tree, so flash this tree's firmware before you start: see :doc:`firmware`.
    * host code: This is software running on a general purpose computer connected to the Ubertooth One via USB. If you have not yet built the host code, please follow the :doc:`build guide <build_guide>`.

Ubertooth One is a development platform. The true power of the device is best realized when you start writing your own software and adapting it to your needs. If you are just getting to know the board, however, it can be helpful to try out open source code that others have made available. This guide will help you get started with your Ubertooth One by introducing you to some useful host code from the Ubertooth software repository.



Spectrum Analysis
~~~~~~~~~~~~~~~~~

	.. image:: ../images/specan_ui.png

The first thing you should try with a new Ubertooth One is real-time spectrum analysis. Take a look at Jared's `demonstration video <http://www.sharebrained.com/2011/05/24/ubertooth-spectrum-analysis-fun>`__ for a preview.

Connect an antenna to your Ubertooth One and plug it into your computer. **Never operate your Ubertooth One without an antenna connected.** You should see the RST and 1V8 LEDs illuminate. This indicates that the LPC175x microcontroller is running (RST) and that power is being supplied to the CC2400 wireless transceiver IC (1V8). The USB LED may also light up if your computer's operating system has enumerated and configured the device (typical on Linux). Now you need some host code to tell the Ubertooth One what to do.

``ubertooth-specan-ui`` is installed with the host tools, so once you have
followed the :doc:`build guide <build_guide>` you can run it from anywhere and
watch the 2.4 GHz activity the Ubertooth One detects. It needs a few Python
packages of its own; :doc:`specan_ui` lists them and describes the display, the
options and the keys.

One possible thing that could go wrong at this point is that your operating system does not grant you permission to communicate with the USB device. On Linux this is a udev rule, and installing the host tools writes one: ``make install`` and ``./install.sh`` put ``40-ubertooth.rules`` in ``/etc/udev/rules.d/`` and set it to the first of the ``usb`` and ``plugdev`` groups that exists on the system. ``-DUDEV_RULES_GROUP=<group>`` chooses another group, and ``-DINSTALL_UDEV_RULES=OFF`` skips the rule.

To install the rule by hand instead, copy it out of your build directory (``misc/udev/40-ubertooth.rules`` under it, so ``host/build/misc/udev/`` if you built in ``host/build``) and reload the rules as root:

	.. code-block:: sh

		# cp host/build/misc/udev/40-ubertooth.rules /etc/udev/rules.d/
		# udevadm control --reload-rules

Make sure you are a member of the group the rule names. After adding the udev rule, unplug the Ubertooth One and plug it in again.

During operation of ubertooth-specan-ui the RX LED should illuminate, and the USR LED should be dimly lit. ``ubertooth-util -S`` stops whatever mode a board is in, and ``ubertooth-util -r`` resets it, so you do not have to unplug it between runs.



LAP Sniffing
~~~~~~~~~~~~

	.. image:: ../images/ubertooth-lap.png

Bluetooth packets start with a code that is based on the Lower Address Part (LAP) of a particular Bluetooth Device Address (BD_ADDR). The BD_ADDR is a 48 bit MAC address, just like the MAC address of an Ethernet device. The LAP consists of the lower 24 bits of the BD_ADDR and is the only part of the address that is transmitted with every packet.

The most important passive Bluetooth monitoring function is simply capturing the LAP from each packet transmitted on a channel. LAP sniffing allows you to identify Bluetooth devices operating in your vicinity.

In order to sniff LAPs, you'll have to compile the tools in ``host/ubertooth-tools``. These are command line programs intended to work with the bluetooth_rxtx firmware installed on your Ubertooth One. Follow the :doc:`build guide <build_guide>` to install the prerequisite libbtbb, a library for Bluetooth baseband functions; these tools are built against `this project's fork of it <https://github.com/SqueakyFloors/libbtbb>`__ at the commit that guide names.

``./install.sh`` in the root of a checkout does libbtbb and the tools together. To build the tools on their own, once libbtbb is installed, type:

	.. code-block:: sh

		mkdir build
		cd build/
		cmake ..
		make
		sudo make install

in the ``host`` directory. Then make sure your Ubertooth One is plugged in and execute:

	.. code-block:: sh

		$ ubertooth-rx

You should see various random LAPs detected. Due to uncertainties in identifying Bluetooth packets without prior knowledge of an address, it is normal for this process to identify false positives. Error correction mitigates this problem, but a small number of false positives may still be seen. When you see the same LAP detected more than once, that is very likely an actual Bluetooth transmission.

Generate some Bluetooth traffic and enjoy the show. I like to use a mobile phone or other Bluetooth device to perform an inquiry (usually called "find new Bluetooth devices" or something similar) to make sure that everything is working properly. An inquiry should produce lots of packets with the LAP 0x9e8b33.

Once you have seen a LAP multiple times, you can be confident that it is a genuine Bluetooth piconet. To find the next byte of the address, the UAP, we can use:

	.. code-block:: sh

		$ ubertooth-rx -l <LAP>

In this mode ubertooth-rx only detects packets from the given piconet and uses them to determine the next byte of the address and some of the internal clock value. ``ubertooth-rx -z`` does both steps at once: it surveys the band and lists each piconet it finds with the UAP it has worked out, and ``-t <seconds>`` gives it a deadline. A survey that has heard only a few dozen packets may print a UAP that is wrong, so give it half a minute or more before you trust one.

For more information on this process, and the challenges involved in monitoring Bluetooth connections, please read this `blog post <http://ubertooth.blogspot.co.uk/2013/02/motivating-problem.html>`__.



Kismet
~~~~~~

	.. image:: ../images/kismet.png

A plugin for `Kismet <http://www.kismetwireless.net/>`__, the venerable 802.11 monitoring tool, ships in ``host/kismet``. It is built against a Kismet source tree matching the installed version, and the tree it was written for is the 2013 era one that :doc:`third_party_software` names, so it needs that Kismet rather than a current release. The instructions in ``host/kismet/plugin-ubertooth/README`` say how to install and use it.

Kismet-Ubertooth identifies not only the LAP but also the 8 bit Upper Address Part (UAP) of detected devices as it is able, by analyzing the timing and other characteristics of multiple packets over time, and it dumps complete decoded packets to a pcapbtbb file that can be read with a Wireshark plugin distributed with `libbtbb <https://github.com/SqueakyFloors/libbtbb>`__. Full packet decoding is only possible when the packet's UAP has been determined. ``ubertooth-rx`` does the same two jobs on its own now: ``-z`` lists piconets with their UAPs, and ``-r`` or ``-q`` writes the decoded packets to a PcapNG or PCAP file that Wireshark reads.



Where to Go from Here
~~~~~~~~~~~~~~~~~~~~~

I hope you have found this guide helpful in getting to know your Ubertooth One. If you are interested in contributing to the project, or if you need help or would just like to chat about Project Ubertooth, join the `#ubertooth channel on Discord <https://discord.gg/rsfMw3rsU8>`__. Happy hacking!
