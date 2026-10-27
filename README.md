<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/ubertooth_wordmark_dark.png">
    <img src="docs/images/ubertooth_wordmark.png" alt="Ubertooth" width="320">
  </picture>
</p>

# Ubertooth

Project Ubertooth is an open source wireless development platform
suitable for Bluetooth experimentation. Ubertooth ships with a capable
BLE (Bluetooth Smart) sniffer and can sniff some data from Basic Rate
(BR) Bluetooth Classic connections.

## What it is good for

Ubertooth is a sniffer. Unlike a phone app such as nRF Connect or
LightBlue, which sees only its own connection after the operating system
has handled it, Ubertooth listens to the air and hears other devices'
traffic.

- **Passive BLE capture** of advertising and of connections between
  other devices: link-layer packets, connection setup, channels and
  timing, saved as PCAP for Wireshark.
- **Spectrum analysis** of the 2.4 GHz band, to see what else is
  transmitting and where.
- **Bluetooth Classic discovery**: finding nearby piconets by address,
  including devices that are not discoverable.
- **Scriptable**: every feature is a command-line tool with defined output
  and exit codes, so it fits test rigs and automation.
- **Open**: the firmware and host tools can be changed for custom 2.4 GHz
  radio experiments.

## What it cannot do

- **BLE on the 1M PHY only.** The CC2400 radio cannot receive the
  Bluetooth 5 LE 2M or Coded (long-range) PHYs, so it cannot capture
  extended-advertising data, and it loses a connection that switches to
  2M. Most current phones, computers and peripherals switch to 2M right
  after connecting.
- **Classic Bluetooth: Basic Rate only.** EDR payloads, which carry most
  audio and fast data, are not captured, and following a piconet is slow
  and partial.
- **No decryption** of encrypted links without their keys.

To sniff Bluetooth 5 traffic, use a sniffer built on a Bluetooth 5 radio,
for example Nordic's nRF Sniffer for Bluetooth LE on an nRF52840 dongle.

The latest release is [2020-12-R1](https://github.com/greatscottgadgets/ubertooth/releases/tag/2020-12-R1).
The latest firmware build can be found on the release page.

That release is paired with [libbtbb 2020-12-R1](https://github.com/greatscottgadgets/libbtbb/releases/tag/2020-12-R1).
The current tree instead builds against this project's
[libbtbb fork](https://github.com/SqueakyFloors/libbtbb), pinned to one commit
that `install.sh`, CI and the build guide all use, so a push to libbtbb cannot
change an Ubertooth build. The fork builds on current CMake and Python tooling
without the workarounds upstream needs.

Instructions for flashing the firmware can be found [on the corresponding readthedocs.io firmware page](https://ubertooth.readthedocs.io/en/latest/firmware.html).

The host tools require firmware built from the same source tree (the same
firmware API version), so flash the firmware whenever you update the tools:
`./install.sh --flash-device <index|serial|all>`. The API version is bumped
whenever a firmware change affects the host, so a board left on older firmware
is normally refused with a version error; reflash from the commit you built
the tools from.

Instructions for building libbtbb by hand, with the pin, are in this tree's
[build guide](docs/source/build_guide.rst); the published
[readthedocs.io build_guide page](https://ubertooth.readthedocs.io/en/latest/build_guide.html)
documents upstream, not this fork.

Build dependencies (required vs optional, and what each enables) are listed in [REQUIREMENTS.md](REQUIREMENTS.md).

## Quick install (Debian/Ubuntu and macOS)

From a checkout of this repository, [`install.sh`](install.sh) does the whole
from-source setup in one command — system dependencies, libbtbb, the host
tools, specan_ui Python deps, and (on Linux) udev rules:

```sh
./install.sh                              # host tools only
./install.sh --venv ~/ubertooth-venv      # isolate the Python tools in a venv
./install.sh --flash-device <index|serial|all>   # also build + flash firmware
```

On Debian/Ubuntu it uses `apt` and installs into `/usr/local` with `sudo`. On
macOS it needs [Homebrew](https://brew.sh), installs into the Homebrew prefix
without `sudo`, and skips `ubertooth-scan`/`ubertooth-follow` (no BlueZ on
macOS); use `--venv` there so the Python packages stay out of the system
Python. Run `./install.sh --help` for all options, including `--prefix`,
`--skip-deps` (use already-installed system packages), and
`--libbtbb-url`/`--libbtbb-ref`, which build a libbtbb other than the pinned
fork commit (`--help` prints the pin). This is the from-source path; it is not
a replacement for distro packaging (apt/dnf/brew).

--------------------

# Tools

Ubertooth ships a set of host command-line tools. Each links to its manual page.
"Needs" indicates what a test/use requires beyond the Ubertooth itself.

### BLE
| Tool | What it does | Needs |
|---|---|---|
| [ubertooth-btle](host/doc/ubertooth-btle.md) | Passive BLE monitoring; follow connections (`-f`), capture advertising (`-n`), or stream PCAP to stdout (`-q -`/`-c -`) | ambient BLE |
| [btle-extcap.py](host/python/extcap/README.md) | Lets Wireshark capture BLE advertising from an Ubertooth directly, by running `ubertooth-btle` for it | Wireshark |

### Classic Bluetooth (BR/EDR)
| Tool | What it does | Needs |
|---|---|---|
| [ubertooth-rx](host/doc/ubertooth-rx.md) | Passive Classic BT discovery/decode (finds piconets, LAP/UAP); saves raw captures (`-d`) and replays them without a device (`-i`) | a Classic BT peer |
| [ubertooth-afh](host/doc/ubertooth-afh.md) | Passively recover a piconet's AFH channel map | an active piconet |
| [ubertooth-follow](host/doc/ubertooth-follow.md) | Active clock discovery + follow a piconet for a UAP/LAP | BlueZ + target |
| [ubertooth-scan](host/doc/ubertooth-scan.md) | Active inquiry scan (uses a BlueZ dongle + Ubertooth) | BlueZ dongle |

### Signal / low-level
| Tool | What it does | Needs |
|---|---|---|
| [ubertooth-specan](host/doc/ubertooth-specan.md) | Spectrum analyzer: stream of 2.4GHz signal strengths | — |
| [ubertooth-specan-ui](host/doc/ubertooth-specan-ui.md) | Graphical live spectrum plot of 2.4GHz ([guide](docs/source/specan_ui.rst)) | Python 3.8+: numpy, qtpy, PySide6 or PyQt5 |
| [ubertooth-dump](host/doc/ubertooth-dump.md) | Raw stream of received bits/symbols | — |
| [ubertooth-debug](host/doc/ubertooth-debug.md) | Low-level CC2400 radio register/debug access | — |

### Device administration & firmware
| Tool | What it does | Needs |
|---|---|---|
| [ubertooth-util](host/doc/ubertooth-util.md) | Device admin: versions (`-v`/`-V`/`-H`), LEDs, reset, channel, device count | — |
| [ubertooth-dfu](host/doc/ubertooth-dfu.md) | Flash/read firmware over DFU | — |

### Special-purpose
| Tool | What it does | Needs |
|---|---|---|
| [ubertooth-ducky](host/doc/ubertooth-ducky.md) | Drive an Uberducky BLE HID-injection gadget | Uberducky hardware |
| [ubertooth-ego](host/doc/ubertooth-ego.md) | Sniff a Yuneec E-GO skateboard (niche) | Yuneec E-GO |
| [ubertooth-tx](host/doc/ubertooth-tx.md) | Retransmit dumped symbols — **currently non-functional** | — |

See also the [Ubertooth overview man page](host/doc/ubertooth.md).

--------------------

# Documentation

Documentation for Ubertooth can be viewed on [Read the Docs](https://ubertooth.readthedocs.io/en/latest/). The raw documenation files for Ubertooth are in the [docs folder](https://github.com/greatscottgadgets/ubertooth/tree/master/docs) in this repository. Documentation changes can be submitted through pull request and suggestions can be made as GitHub issues. 

After editing a man page source in `host/doc/`, regenerate its committed roff
page and commit both, or the `Man pages` CI job fails:

```sh
cmake -S host -B build
cmake --build build --target refresh-manpages
```

--------------------

# Getting Help

Before asking for help with Ubertooth, check to see if your question is listed in the [FAQ](https://ubertooth.readthedocs.io/en/latest/faq.html).

For assistance with Ubertooth general use or development, please look at the [issues on the GitHub project](https://github.com/greatscottgadgets/ubertooth/issues). This is the preferred place to ask questions or open issues so that others may locate the answer to your question in the future.

We invite you to join our community discussions on [Discord](https://discord.gg/rsfMw3rsU8). Note that while technical support requests are welcome here, we do not have support staff on duty at all times. Be sure to also submit an issue on GitHub if you’ve found a bug or if you want to ensure that your request will be tracked and not overlooked.

GitHub issues on this repository that are labelled "technical support" by Great Scott Gadgets employees can expect a response time of two weeks. We currently do not have expected response times for other GitHub issues or pull requests for this repository. 

--------------------

# Credits

Ubertooth was created by Great Scott Gadgets, with Michael Ossmann
([@mossmann](https://github.com/mossmann)), Dominic Spill
([@dominicgs](https://github.com/dominicgs)), and Mike Ryan
([@mikeryan](https://github.com/mikeryan)) as its principal authors. This fork
builds on their work and on contributions from the wider community. See
[CREDITS.md](CREDITS.md) for the original authors, contributors whose work is
incorporated here, and every upstream contributor.
