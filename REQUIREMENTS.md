# Build Requirements

Dependencies for building the Ubertooth host tools and firmware. The build does
**not** fail when an *optional* dependency is missing — it quietly builds fewer
tools. The CMake configure step prints a **build summary** showing which tools
are enabled/disabled; pass `-DREQUIRE_ALL=ON` to turn a missing optional
dependency into a hard error (recommended for CI and release builds).

## Host tools

### Required
| Dependency | Purpose | Debian/Ubuntu | Fedora | macOS (Homebrew) |
|---|---|---|---|---|
| CMake ≥ 3.5 | build system | `cmake` | `cmake` | `cmake` |
| C/C++ compiler and make | build | `build-essential` (or `gcc g++ make`) | `gcc gcc-c++ make` | Xcode CLT |
| pkg-config | dep discovery | `pkg-config` | `pkgconf-pkg-config` | `pkg-config` |
| libusb-1.0 | USB device access | `libusb-1.0-0-dev` | `libusb1-devel` | `libusb` |
| libbtbb | Bluetooth baseband decode | build from source (the pinned fork commit, see [the build guide](docs/source/build_guide.rst)) | source | source |

Missing a **required** dependency fails the build.

### Optional — absence silently drops tools unless `-DREQUIRE_ALL=ON`
| Dependency | Enables | If missing | Install |
|---|---|---|---|
| libbluetooth (BlueZ) | `ubertooth-scan`, `ubertooth-follow` | those two tools are **not built** | `libbluetooth-dev` / `bluez-libs-devel` (Linux only; on macOS there is nothing to install and those two tools are never built — [why, and what to use instead](COMPATIBILITY.md#macos-ubertooth-scan-and-ubertooth-follow-are-linux-only)) |
| FFTW3 | specan_ui spectrum GUI deps | GUI may not function | `libfftw3-dev` / `fftw-devel` / `brew install fftw` |
| Python 3 + runtime modules | specan_ui / ubtbr Python tools | Python tools install, but fail at runtime if deps are missing | `python3` (`python3-venv` on Debian/Ubuntu for `--venv`, `python3-pip` on Fedora); specan_ui: `numpy qtpy` plus `pyside6` (pip) or the distro packages (`python3-numpy python3-qtpy python3-pyqt5` on apt, `python3-numpy python3-QtPy python3-pyside6` on dnf); ubtbr: `libusb1` / distro `python3-usb1` (apt) or `python3-libusb1` (dnf) |
| ronn-ng **0.10.1** | the `refresh-manpages` target, which regenerates the committed man pages from `host/doc/*.md` | nothing: the committed `.1` pages are the ones installed, and a build never regenerates them ([host/doc/man/README.md](host/doc/man/README.md)) | `gem install ronn-ng -v 0.10.1` |

Only that one ronn version is supported: another renders the same Markdown
differently, so the committed pages would no longer match a refresh, and
`refresh-manpages` refuses to run. `RONN_VERSION` in
`host/doc/man/CMakeLists.txt` is where it is decided, and the man-page CI job
reads that line; the job also pins kramdown, which does the Markdown parse.

See [COMPATIBILITY.md](COMPATIBILITY.md) for supported OS / toolchain / Python **versions**.

## Firmware

| Dependency | Purpose | Debian/Ubuntu | Fedora | macOS |
|---|---|---|---|---|
| GNU make | firmware build | `build-essential` | `make` | Xcode CLT (stock `make` 3.81 is fine) |
| arm-none-eabi-gcc + its C library | cross-compile firmware | `gcc-arm-none-eabi libnewlib-arm-none-eabi` | `arm-none-eabi-gcc-cs arm-none-eabi-newlib` | `brew install --cask gcc-arm-embedded` |
| ARM C++ runtime | only `firmware/bootloader`, the one image written in C++ | `libstdc++-arm-none-eabi-newlib` | `arm-none-eabi-gcc-cs-c++` | carried by the cask above |
| perl | only `make -C firmware test`, to generate the ToorCon badge's register header | pre-installed | pre-installed | pre-installed |
| dfu-util | flash firmware over DFU | `dfu-util` | `dfu-util` | `brew install dfu-util` |
| ubertooth-dfu (host tool) | the `.dfu` packaging step | built with the host tools | same | same |

## Quick install (Ubuntu 22.04 / 24.04, Fedora, macOS with Homebrew)

The easiest path is the bootstrap script, which installs all of the below and
builds/installs libbtbb and the host tools for you. The one exception is the
ARM C++ runtime: nothing the script builds needs it, so install it by hand if
you are going to build the bootloader (`make -C firmware bootloader`).

```sh
./install.sh            # see ./install.sh --help for options (venv, flash, ...)
```

It picks the package manager from the machine: `apt-get` on Debian/Ubuntu,
`dnf` on Fedora, Homebrew on macOS. On macOS it installs `cmake pkg-config
libusb fftw` (and, with `--with-firmware`, `dfu-util` and the
`gcc-arm-embedded` cask), then installs into the Homebrew prefix without
`sudo`.

To do it by hand instead, on Debian/Ubuntu:

```sh
sudo apt-get install git cmake build-essential pkg-config \
     libusb-1.0-0-dev libbluetooth-dev libfftw3-dev \
     python3 python3-venv python3-numpy python3-qtpy python3-pyqt5 python3-usb1 \
     dfu-util gcc-arm-none-eabi libnewlib-arm-none-eabi
# then build libbtbb from source (the pinned fork commit), then the host tools
```

or on Fedora:

```sh
sudo dnf install git cmake gcc gcc-c++ make pkgconf-pkg-config \
     libusb1-devel bluez-libs-devel fftw-devel \
     python3 python3-pip python3-libusb1 python3-numpy python3-QtPy python3-pyside6 \
     dfu-util arm-none-eabi-gcc-cs arm-none-eabi-newlib
# then build libbtbb from source (the pinned fork commit), then the host tools
```

On Fedora libbtbb and libubertooth both install to `lib64`, the directory the
system keeps libraries in; the build asks pkg-config and CMake where that is
rather than assuming, and `install.sh` sets the installed tools' run path to
it, so a hand-built install may need `/usr/local/lib64` in
`/etc/ld.so.conf.d/` where the script needs nothing.

## Verifying what got built

After `cmake`, read the **build summary** it prints. Example when BlueZ is
missing:

```
Ubertooth host tools — build summary
  always built: ubertooth-rx tx dump util btle dfu specan ego afh ducky debug
  bluez tools:  DISABLED (ubertooth-scan, ubertooth-follow) — libbluetooth NOT found
                install libbluetooth-dev / bluez-libs-devel, or set -DREQUIRE_ALL=ON to make this an error
```

For a build that must include everything, configure with `-DREQUIRE_ALL=ON` so
a missing optional dependency fails loudly instead of silently dropping tools.
On macOS that one cannot be met — BlueZ is Linux only, so `ubertooth-scan` and
`ubertooth-follow` are never built there and the summary says so instead of
naming a package to install. Leave `REQUIRE_ALL` off on a Mac, as
`install.sh` does.
