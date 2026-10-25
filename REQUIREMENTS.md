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
| C/C++ compiler | build | `gcc g++` | `gcc gcc-c++` | Xcode CLT |
| pkg-config | dep discovery | `pkg-config` | `pkgconf-pkg-config` | `pkg-config` |
| libusb-1.0 | USB device access | `libusb-1.0-0-dev` | `libusb1-devel` | `libusb` |
| libbtbb | Bluetooth baseband decode | build from source (match release) | source | source |

Missing a **required** dependency fails the build.

### Optional — absence silently drops tools unless `-DREQUIRE_ALL=ON`
| Dependency | Enables | If missing | Install |
|---|---|---|---|
| libbluetooth (BlueZ) | `ubertooth-scan`, `ubertooth-follow` | those two tools are **not built** | `libbluetooth-dev` / `bluez-libs-devel` (Linux only; not on macOS) |
| FFTW3 | specan_ui spectrum GUI deps | GUI may not function | `libfftw3-dev` / `fftw-devel` / `brew install fftw` |
| Python 3 + runtime modules | specan_ui / ubtbr Python tools | Python tools install, but fail at runtime if deps are missing | `python3`; specan_ui: `numpy qtpy` plus `pyside6` (pip) or `pyqt5` (apt: `python3-pyqt5`); ubtbr: `libusb1` / distro `python3-usb1` |
| ronn-ng **0.10.1** | the `refresh-manpages` target, which regenerates the committed man pages from `host/doc/*.md` | nothing: the committed `.1` pages are the ones installed, and a build never regenerates them ([host/doc/man/README.md](host/doc/man/README.md)) | `gem install ronn-ng -v 0.10.1` |

Only that one ronn version is supported: another renders the same Markdown
differently, so the committed pages would no longer match a refresh. It is the
version the man-page CI check installs.

See [COMPATIBILITY.md](COMPATIBILITY.md) for supported OS / toolchain / Python **versions**.

## Firmware

| Dependency | Purpose | Debian/Ubuntu | macOS |
|---|---|---|---|
| GNU make | firmware build | `build-essential` | Xcode CLT (stock `make` 3.81 is fine) |
| arm-none-eabi-gcc | cross-compile firmware | `gcc-arm-none-eabi` | `brew install --cask gcc-arm-embedded` |
| dfu-util | flash firmware over DFU | `dfu-util` | `brew install dfu-util` |
| ubertooth-dfu (host tool) | the `.dfu` packaging step | built with the host tools | same |

## Quick install (Ubuntu 22.04 / 24.04, macOS with Homebrew)

The easiest path is the bootstrap script, which installs all of the below and
builds/installs libbtbb and the host tools for you:

```sh
./install.sh            # see ./install.sh --help for options (venv, flash, ...)
```

On macOS it installs `cmake pkg-config libusb fftw` (and, with
`--with-firmware`, `dfu-util` and the `gcc-arm-embedded` cask) through Homebrew,
then installs into the Homebrew prefix without `sudo`.

To do it by hand instead:

```sh
sudo apt-get install cmake build-essential pkg-config \
     libusb-1.0-0-dev libbluetooth-dev libfftw3-dev \
     python3 python3-numpy python3-qtpy python3-pyqt5 python3-usb1 \
     dfu-util gcc-arm-none-eabi libnewlib-arm-none-eabi
# then build libbtbb from source (matching the release), then the host tools
```

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
