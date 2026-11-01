=================
Building from git
=================

Prerequisites
~~~~~~~~~~~~~

This page is the :doc:`build guide <build_guide>` starting from a git clone
rather than a release archive; that page carries the longer notes on each
prerequisite, on where libbtbb installs its library, and on the Wireshark
plugins. ``./install.sh`` in the root of a clone does the whole build for you,
and the steps below are what it does by hand.

There are some prerequisites that need to be installed before building libbtbb and the Ubertooth tools. Many of these are available from your operating system's package repositories, for example:

Debian/Ubuntu
^^^^^^^^^^^^^

	.. code-block:: sh

		sudo apt-get install git cmake libusb-1.0-0-dev make gcc g++ libbluetooth-dev \
		libfftw3-dev pkg-config python3 python3-numpy python3-qtpy python3-pyqt5 \
		python3-usb1



Fedora / Red Hat
^^^^^^^^^^^^^^^^

	.. code-block:: sh

		sudo dnf install git libusb1-devel make gcc gcc-c++ cmake pkgconf-pkg-config \
		wget tar bluez-libs-devel fftw-devel



macOS (Homebrew)
^^^^^^^^^^^^^^^^

Install the Xcode command line tools (``xcode-select --install``) and
`Homebrew <https://brew.sh>`__, then:

	.. code-block:: sh

		brew install git cmake pkg-config libusb wget fftw



libbtbb
~~~~~~~

Next the Bluetooth baseband library (libbtbb) needs to be built for the Ubertooth tools to decode Bluetooth packets:

	.. code-block:: sh

		git clone https://github.com/SqueakyFloors/libbtbb.git
		cd libbtbb
		git checkout 017676965a77cd7b00bfede343b7800dee6aa6a5
		mkdir build
		cd build
		cmake -DENABLE_PYTHON=OFF ..
		make
		sudo make install

This is this project's libbtbb fork, at the commit the Ubertooth tools are built and tested against; see :doc:`build_guide`. The tools' configure step refuses a libbtbb that is missing a function they call, and names the commit to rebuild it at, so an older one fails there rather than part way through the build.

**Linux users**: if you are installing for the first time, or you receive errors about finding the library, you should run:

	.. code-block:: sh

		sudo ldconfig



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

On macOS the install goes into the Homebrew prefix, which you own, so ``make install`` needs no ``sudo`` there.

**Linux users**: if you are installing for the first time, or you receive errors about finding the library, you should run:

	.. code-block:: sh

		sudo ldconfig



Wireshark
~~~~~~~~~

Wireshark version 1.12 and newer includes the Ubertooth BLE plugin by default. It is also possible to :doc:`capture BLE from Ubertooth directly into Wireshark <capturing_BLE_Wireshark>`.

Users of Wireshark 2.2 and newer do not need to build these plugins at all and may skip this section.

The Wireshark BTBB and BR/EDR plugins allow Bluetooth baseband traffic that has been captured using Kismet to be analyzed and dissected within the Wireshark GUI. They are built separately from the rest of the Ubertooth and libbtbb software. The directory passed to cmake as ``CMAKE_INSTALL_LIBDIR`` varies from system to system, but it should be the location of existing Wireshark plugins, such as ``asn1.so`` and ``ethercat.so``; the path in the commands below is one Wireshark 2.x layout, not a value to copy blindly.

	.. code-block:: sh

		sudo apt-get install wireshark wireshark-dev libwireshark-dev cmake
		cd libbtbb/wireshark/plugins/btbb
		mkdir build
		cd build
		cmake -DCMAKE_INSTALL_LIBDIR=/usr/lib/x86_64-linux-gnu/wireshark/libwireshark3/plugins ..
		make
		sudo make install

Then repeat for the BT BR/EDR plugin:

	.. code-block:: sh

		sudo apt-get install wireshark wireshark-dev libwireshark-dev cmake
		cd libbtbb/wireshark/plugins/btbredr
		mkdir build
		cd build
		cmake -DCMAKE_INSTALL_LIBDIR=/usr/lib/x86_64-linux-gnu/wireshark/libwireshark3/plugins ..
		make
		sudo make install	



Third Party Software
~~~~~~~~~~~~~~~~~~~~

There are a number of pieces of :doc:`third party software <third_party_software>` that support the Ubertooth. Some support Ubertooth out of the box, while others require plugins to be built.



Firmware
~~~~~~~~

This completes the install of the Ubertooth tools, and the next step is :doc:`firmware`. The host tools require firmware of their own API version, so flash the firmware built from the same checkout before you use them; a board carrying any other version is refused. ``./install.sh --flash-device <index|serial|all>`` builds and writes it for you.
