Ubertooth Host
==============
This is host code for use with bluetooth_rxtx firmware.


Required Software
-----------------
These tools link to libbtbb, and are built and tested against this project's
fork of it (https://github.com/SqueakyFloors/libbtbb) at one pinned commit.
The [build guide](../docs/source/build_guide.rst) names that commit; the
simplest way to get it is [install.sh](../install.sh), which clones and builds
it for you. By hand:
```
$ git clone https://github.com/SqueakyFloors/libbtbb.git
$ cd libbtbb/
$ git checkout <the commit named in the build guide>
$ mkdir build
$ cd build
$ cmake -DENABLE_PYTHON=OFF ..
$ make
$ sudo make install
```

libbtbb installs its library to the directory the system keeps libraries in
(`lib`, or `lib64` and `lib/<triplet>` on the systems that use those), and the
build below asks pkg-config and CMake where it is rather than assuming `lib`.
libubertooth goes into that same directory, which `-DCMAKE_INSTALL_LIBDIR=`
chooses.

This software also requires libusb 1.0 or higher, which can be found at
http://www.libusb.org or installed from your OS's package repository.

An optional, but recommended, dependency is libPcap, which is available from
http://www.tcpdump.org or can be found in your OS's package repository.

Building
--------
Build the library and tools using the following steps:
```
$ mkdir build
$ cd build
$ cmake ..
$ make
$ make install (may require root privileges)
```

If previous versions of libbtbb or Ubertooth tools are installed they should be
removed before building the latest release.  A script to do so can be found in
the libbtbb archive and run as follows:
```
$ sudo libbtbb/cmake/cleanup.sh -d
```
Running the script without the '-d' option will print the files to be removed.

The Tools
---------
The catalog in the top-level [README](../README.md#tools) lists every tool,
groups them by purpose, and says what each needs beyond the Ubertooth itself.
Each tool's manual page in [host/doc](doc) is the full reference for its
options: `man ubertooth` for the overview, `man ubertooth-rx` and so on for a
tool. `ubertooth-follow` and `ubertooth-scan` are built only where BlueZ is,
and `btle-extcap.py` is not installed by the build: it is copied into
Wireshark's extcap directory by hand.

For example:
```
ubertooth-specan -G | feedgnuplot --stream 0.5 --domain --3d
```
draws a realtime animated 3D plot of the frequency spectrum, and
`ubertooth-dump -d - | dd of=/dev/null` reports the symbol stream's transfer
rate, which should be about 1 MB/s.


Privledge Reduction
-------------------
If you desire to run any program which accesses the ubertooth hardware as a user
you may do so by copying libubertooth/40-ubertooth.rules to wherever your distro
keeps udev rules, typically /lib/udev/rules.d

This action will allow any user in the "usb" group to access the ubertooth
hardware. If you want to give access to a different group you can easily edit
the rules file and change usb to whatever group you prefer.

Troubleshooting
---------------

If trying to run any of the host tools results in error message like 
```
ubertooth-specan: error while loading shared libraries: libubertooth.so.1: cannot open shared object file: No such file or directory
```
there could be a problem with your library path configuration.

On Arch Linux and Fedora it could be neccessary to edit the `/etc/ld.so.conf` (or create a corresponding file with the path in `/etc/ld.so.conf.d/`) file to include the directory the install put the library in: `/usr/local/lib`, and `/usr/local/lib64` on a 64-bit Fedora, which keeps its libraries there. The configure step says which one it is ("Installing the library to ..."), and `install.sh` needs none of this, because it gives the tools it installs a run path.
