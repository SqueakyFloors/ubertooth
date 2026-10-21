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
| Python 3 + setuptools | specan_ui / ubtbr Python tools | Python tools not installed | `python3 python3-setuptools` (3.12+ **needs** setuptools, PEP 632) |
| ronn (ronn-ng) | regenerate man pages from `host/doc/*.md` | committed `.1` man pages are installed as-is | `gem install ronn-ng` |

See `COMPATIBILITY.md` for supported OS / toolchain / Python **versions**.

## Firmware

| Dependency | Purpose | Debian/Ubuntu | macOS |
|---|---|---|---|
| arm-none-eabi-gcc | cross-compile firmware | `gcc-arm-none-eabi` | `brew install --cask gcc-arm-embedded` |
| dfu-util | flash firmware over DFU | `dfu-util` | `brew install dfu-util` |
| ubertooth-dfu (host tool) | the `.dfu` packaging step | built with the host tools | same |

## Quick install (Ubuntu 22.04 / 24.04)

```sh
sudo apt-get install cmake build-essential pkg-config \
     libusb-1.0-0-dev libbluetooth-dev libfftw3-dev \
     python3 python3-setuptools dfu-util gcc-arm-none-eabi
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
