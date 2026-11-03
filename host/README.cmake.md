CMake Settings
==============
The following are flags that may be of use when configuing this project.

 * INSTALL_UDEV_RULES
  * A boolean flag for installing udev rules to control access to
    Ubertooth hardware.

 * UDEV_RULES_GROUP
  * A variable to specify group name to be used in the udev rules,
    if not set CMake will attempt to determine in usb or plugdev
    are used on the host system.

 * ENABLE_PYTHON
  * Build tools that require python - now only ubertooth-specan-ui

Using the installed library from CMake
======================================
The install writes `ubertoothConfig.cmake` beside the library, in
`<libdir>/cmake/ubertooth`, so a CMake project of its own asks for the package
by name and links one target:

```cmake
find_package(ubertooth REQUIRED)
target_link_libraries(my-program PRIVATE ubertooth::ubertooth)
```

`ubertooth::ubertooth` carries the installed library, the directory its
headers are in, and libbtbb and libusb, whose headers those headers include.
Nothing else has to be found or named.

An install outside the directories CMake searches is pointed at with
`-DCMAKE_PREFIX_PATH=<prefix>`, and the version can be asked for:
`find_package(ubertooth 1.0 REQUIRED)` is satisfied by any 1.x, because that
is what the library's `SOVERSION` promises.

The config file reads its prefix back from where it is installed, so an
install staged or copied somewhere else describes the directories it really
landed in. `libubertooth.pc` describes the same install to pkg-config, for a
program whose build system is not CMake.
