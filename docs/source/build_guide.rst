===========
Build Guide
===========



Release 2020-12-R1
~~~~~~~~~~~~~~~~~~

`for Release 2018-12-R1 see here <https://ubertooth.readthedocs.io/en/latest/Release_2018-12-R1.html>`__



Prerequisites
^^^^^^^^^^^^^

There are some prerequisites that need to be installed before building libbtbb and the Ubertooth tools. Many of these are available from your operating system's package repositories, for example:



Debian 10 / Ubuntu 20.04 / Kali
+++++++++++++++++++++++++++++++

	.. code-block:: sh

		sudo apt install cmake libusb-1.0-0-dev make gcc g++ libbluetooth-dev wget \
		pkg-config python3-numpy python3-qtpy python3-usb1



Fedora 33
+++++++++

	.. code-block:: sh

		sudo dnf install libusb1-devel make gcc gcc-c++ cmake wget tar bluez-libs-devel
		# libbtbb installs to /usr/local/lib64 on this system, the Ubertooth library to /usr/local/lib:
		printf '/usr/local/lib\n/usr/local/lib64\n' | sudo tee /etc/ld.so.conf.d/libc.conf



RHEL 8
++++++

	.. code-block:: sh

		sudo subscription-manager repos --enable=codeready-builder-for-rhel-8-x86_64-rpms
		sudo dnf install libusb1-devel make gcc gcc-c++ wget tar bluez-libs-devel python36 python36-devel
		# libbtbb installs to /usr/local/lib64 on this system, the Ubertooth library to /usr/local/lib:
		printf '/usr/local/lib\n/usr/local/lib64\n' | sudo tee /etc/ld.so.conf.d/libc.conf
		# Note: you will also need to install CMake 3.12.0 or greater. See https://cmake.org/install/



CentOS 8
++++++++

	.. code-block:: sh

		sudo dnf config-manager --enable powertools
		sudo dnf install libusb1-devel make gcc gcc-c++ wget tar bluez-libs-devel python36 python36-devel
		# libbtbb installs to /usr/local/lib64 on this system, the Ubertooth library to /usr/local/lib:
		printf '/usr/local/lib\n/usr/local/lib64\n' | sudo tee /etc/ld.so.conf.d/libc.conf
		# Note: you will also need to install CMake 3.12.0 or greater. See https://cmake.org/install/



macOS (Homebrew)
++++++++++++++++

macOS 14 (Sonoma) or newer is supported; see `COMPATIBILITY.md <https://github.com/SqueakyFloors/ubertooth/blob/main/COMPATIBILITY.md>`__ for the
tested versions and `REQUIREMENTS.md <https://github.com/SqueakyFloors/ubertooth/blob/main/REQUIREMENTS.md>`__ for what each dependency enables.
Install the Xcode command line tools (``xcode-select --install``) and
`Homebrew <https://brew.sh>`__, then:

	.. code-block:: sh

		brew install cmake pkg-config libusb wget fftw
		# firmware build and flashing (optional):
		brew install --cask gcc-arm-embedded
		brew install dfu-util

For the spectrum analyzer user interface's (UI) Python dependencies
(installed in a venv because Homebrew's Python rejects system-wide
``pip install``), see
:doc:`specan_ui`.

``ubertooth-scan`` and ``ubertooth-follow`` need BlueZ (Linux only), so they are
not built on macOS. The host tools in this repository need no CMake options of
their own, and neither does the pinned libbtbb. The archived host source below
needs two: ``-DCMAKE_POLICY_VERSION_MINIMUM=3.5`` under CMake 4, which current
Homebrew installs, and ``-DCMAKE_C_FLAGS="-I$(brew --prefix)/include"`` on any
Homebrew install, because it looks for libusb's headers in one place only and
that is not where the compiler searches.



FreeBSD users can install the host tools and library directly from the ports and package system:
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

	.. code-block:: sh

		sudo pkg install ubertooth



libbtbb
^^^^^^^

Next the Bluetooth baseband library (libbtbb) needs to be built for the Ubertooth tools to decode Bluetooth packets. These tools build against `this project's libbtbb fork <https://github.com/SqueakyFloors/libbtbb>`__, pinned to one commit so that a push to libbtbb cannot change an Ubertooth build. ``ENABLE_PYTHON=OFF`` skips libbtbb's own Python tools, which the Ubertooth tools do not use:

	.. code-block:: sh

		git clone https://github.com/SqueakyFloors/libbtbb.git
		cd libbtbb
		git checkout 4b7f8d737ba7d24d4e1f24e9eaf4693fea9fe66d
		mkdir build
		cd build
		cmake -DENABLE_PYTHON=OFF ..
		make
		sudo make install
		sudo ldconfig     # Linux only

libbtbb installs its library to the directory the system keeps libraries in: ``lib`` under most prefixes, ``lib64`` on a 64-bit Fedora, RHEL or SUSE, and ``lib/<triplet>`` where the prefix is ``/usr`` on Debian or Ubuntu. The Ubertooth build asks pkg-config and CMake where it is rather than assuming ``lib``, so nothing here changes with it; `libbtbb's README <https://github.com/SqueakyFloors/libbtbb#where-it-installs>`__ says how to choose the directory, and ``-DCMAKE_INSTALL_LIBDIR=`` is the way.

That is the commit CI tests against and `install.sh <https://github.com/SqueakyFloors/ubertooth/blob/main/install.sh>`__ builds, and a CI step fails if this page falls behind it. ``./install.sh`` does all of the above for you, and ``./install.sh --libbtbb-url``/``--libbtbb-ref`` build a different libbtbb, which has to carry the entry points the tools call: a libbtbb without them, upstream's included, is refused by the configure step described next.

The Ubertooth tools' own configure step checks the libbtbb it finds for the functions they call, and refuses one that is missing any of them, naming them and the commit above. So if you built the tools once and come back to them later, you may need to rebuild libbtbb at the commit the tree now asks for before the tools will configure.



Ubertooth Tools
^^^^^^^^^^^^^^^

The Ubertooth repository contains host code for sniffing Bluetooth packets, configuring the Ubertooth and updating firmware. All three are built and installed by default using the following method:

	.. code-block:: sh

		wget https://github.com/greatscottgadgets/ubertooth/releases/download/2020-12-R1/ubertooth-2020-12-R1.tar.xz
		tar -xf ubertooth-2020-12-R1.tar.xz
		cd ubertooth-2020-12-R1/host
		mkdir build
		cd build
		cmake ..          # Linux / CMake before 4
		# macOS with CMake 4 (use instead of the command above):
		cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		  -DCMAKE_C_FLAGS="-I$(brew --prefix)/include" ..
		make
		sudo make install
		sudo ldconfig     # Linux only



Wireshark plugins
^^^^^^^^^^^^^^^^^

Users of Wireshark version 2.2+ do not need to build any plugins at all and may skip this section (see `this comment <https://github.com/greatscottgadgets/libbtbb/issues/50#issuecomment-284128258>`__). This includes users of Debian 10+, Ubuntu 20.04+, Fedora 33+, RHEL 8.3+, and most other Linux distributions. You can check your version by clicking on Help --> About Wireshark.

Wireshark version 1.12 and newer includes the Ubertooth BLE plugin by
default. It is also possible to `capture BLE from Ubertooth
directly into Wireshark
<https://ubertooth.readthedocs.io/en/latest/capturing_BLE_Wireshark.html>`__
with a little work.

The Wireshark Bluetooth Baseband (BTBB) and Basic Rate/Enhanced Data Rate
(BR/EDR) plugins allow Bluetooth baseband traffic captured with Kismet to be
analyzed and dissected in the Wireshark graphical user interface (GUI). They
are built separately from the rest of the Ubertooth and libbtbb software.

The directory passed to cmake as ``MAKE_INSTALL_LIBDIR`` varies from system to system, but it should be the location of existing Wireshark plugins, such as ``asn1.so`` and ``ethercat.so``. On macOS this is likely ``/Applications/Wireshark.app/Contents/PlugIns/wireshark/``.

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

There are a number of pieces of `third party software <https://ubertooth.readthedocs.io/en/latest/third_party_software.html>`__ that support the Ubertooth. Some support Ubertooth out of the box, while others require plugins to be built.



Firmware
~~~~~~~~

This completes the install of the Ubertooth tools, the next step is to look at the getting started guide. You should always `update the firmware <https://ubertooth.readthedocs.io/en/latest/firmware.html>`__ on the Ubertooth device to match the software release version that you are using.
