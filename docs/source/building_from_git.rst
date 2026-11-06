=================
Building from git
=================

Prerequisites
~~~~~~~~~~~~~

This page is the :doc:`build guide <build_guide>` starting from a git clone
rather than a release archive. ``./install.sh`` in the root of a clone does the
whole build for you, and the steps below are what it does by hand.

You need git, CMake, make, a C and C++ compiler, pkg-config, libusb-1.0, and,
on Linux, libbluetooth for ``ubertooth-scan`` and ``ubertooth-follow``, plus
Python 3 and the spectrum analyzer UI's modules. The build guide gives the
package names per system, and carries the longer notes on each prerequisite,
on where libbtbb installs its library, and on the Wireshark plugins.

libbtbb
~~~~~~~

Next the Bluetooth baseband library (libbtbb) needs to be built for the Ubertooth tools to decode Bluetooth packets:

	.. code-block:: sh

		git clone https://github.com/SqueakyFloors/libbtbb.git
		cd libbtbb
		git checkout c70de76afbe13f3740bcd906952a36c849383810
		mkdir build
		cd build
		cmake -DENABLE_PYTHON=OFF ..
		make
		sudo make install
		sudo ldconfig     # Linux only, and only on a first install

This is this project's libbtbb fork, at the commit the Ubertooth tools are built and tested against; see :doc:`build_guide`. The tools' configure step refuses a libbtbb that is missing a function they call, and names the commit to rebuild it at, so an older one fails there rather than part way through the build. It also prints one line naming the libbtbb it found and the commit that library was built from, and warns when that is not the commit above.

Ubertooth tools
~~~~~~~~~~~~~~~

The Ubertooth repository contains host code for sniffing Bluetooth packets, configuring the Ubertooth and updating firmware. All three are built and installed by default using the following method:

	.. code-block:: sh

		git clone https://github.com/SqueakyFloors/ubertooth.git
		cd ubertooth/host
		mkdir build
		cd build
		cmake ..
		make
		sudo make install
		sudo ldconfig     # Linux only, and only on a first install

On macOS the install goes into the Homebrew prefix, which you own, so ``make install`` needs no ``sudo`` there.


Wireshark and third party software
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Whether Wireshark needs a plugin built by hand, how to build the BTBB and
BR/EDR ones, and what other software supports the Ubertooth are all in the
:doc:`build guide <build_guide>`. You can also :doc:`capture BLE from Ubertooth
directly into Wireshark <capturing_BLE_Wireshark>`.



Firmware
~~~~~~~~

This completes the install of the Ubertooth tools, and the next step is :doc:`firmware`. The host tools require firmware of their own API version, so flash the firmware built from the same commit before you use them; the :doc:`build guide <build_guide>` says which builds refuse a board that reports another and which only warn. ``./install.sh --flash-device <index|serial|all>`` builds and writes it for you.
