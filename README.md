# Ubertooth

Project Ubertooth is an open source wireless development platform
suitable for Bluetooth experimentation. Ubertooth ships with a capable
BLE (Bluetooth Smart) sniffer and can sniff some data from Basic Rate
(BR) Bluetooth Classic connections.

The latest release is [2020-12-R1](https://github.com/greatscottgadgets/ubertooth/releases/tag/2020-12-R1).
The latest firmware build can be found on the release page.

This release is paired with [libbtbb 2020-12-R1](https://github.com/greatscottgadgets/libbtbb/releases/tag/2020-12-R1).

Instructions for flashing the firmware can be found [on the corresponding readthedocs.io firmware page](https://ubertooth.readthedocs.io/en/latest/firmware.html).
Instructions for building libbtbb can be found [on the corresponding readthedocs.io build_guide page](https://ubertooth.readthedocs.io/en/latest/build_guide.html).

Build dependencies (required vs optional, and what each enables) are listed in [REQUIREMENTS.md](REQUIREMENTS.md).

## Quick install (Debian/Ubuntu)

From a checkout of this repository, [`install.sh`](install.sh) does the whole
from-source setup in one command — system dependencies, libbtbb, the host
tools, specan_ui Python deps, and udev rules:

```sh
./install.sh                              # host tools only
./install.sh --venv ~/ubertooth-venv      # isolate the Python tools in a venv
./install.sh --flash-device <index|serial|all>   # also build + flash firmware
```

Run `./install.sh --help` for all options. This is the from-source path; it is
not a replacement for distro packaging (apt/dnf/brew).

--------------------

# Tools

Ubertooth ships a set of host command-line tools. Each links to its manual page.
"Needs" indicates what a test/use requires beyond the Ubertooth itself.

### Bluetooth Low Energy (BLE)
| Tool | What it does | Needs |
|---|---|---|
| [ubertooth-btle](host/doc/ubertooth-btle.md) | Passive BLE monitoring; follow connections (`-f`), capture advertising (`-n`), or stream PCAP to stdout (`-q -`/`-c -`) | ambient BLE |

### Classic Bluetooth (BR/EDR)
| Tool | What it does | Needs |
|---|---|---|
| [ubertooth-rx](host/doc/ubertooth-rx.md) | Passive Classic BT discovery/decode (finds piconets, LAP/UAP) | a Classic BT peer |
| [ubertooth-afh](host/doc/ubertooth-afh.md) | Passively recover a piconet's AFH channel map | an active piconet |
| [ubertooth-follow](host/doc/ubertooth-follow.md) | Active clock discovery + follow a piconet for a UAP/LAP | BlueZ + target |
| [ubertooth-scan](host/doc/ubertooth-scan.md) | Active inquiry scan (uses a BlueZ dongle + Ubertooth) | BlueZ dongle |

### Signal / low-level
| Tool | What it does | Needs |
|---|---|---|
| [ubertooth-specan](host/doc/ubertooth-specan.md) | Spectrum analyzer: stream of 2.4GHz signal strengths | — |
| [ubertooth-dump](host/doc/ubertooth-dump.md) | Raw stream of received bits/symbols | — |
| [ubertooth-debug](host/doc/ubertooth-debug.md) | Low-level CC2400 radio register/debug access | — |

### Device administration & firmware
| Tool | What it does | Needs |
|---|---|---|
| [ubertooth-util](host/doc/ubertooth-util.md) | Device admin: versions (`-v`/`-V`/`-H`), LEDs, reset, channel, DFU | — |
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

--------------------

# Getting Help

Before asking for help with Ubertooth, check to see if your question is listed in the [FAQ](https://ubertooth.readthedocs.io/en/latest/faq.html).

For assistance with Ubertooth general use or development, please look at the [issues on the GitHub project](https://github.com/greatscottgadgets/ubertooth/issues). This is the preferred place to ask questions or open issues so that others may locate the answer to your question in the future.

We invite you to join our community discussions on [Discord](https://discord.gg/rsfMw3rsU8). Note that while technical support requests are welcome here, we do not have support staff on duty at all times. Be sure to also submit an issue on GitHub if you’ve found a bug or if you want to ensure that your request will be tracked and not overlooked.

GitHub issues on this repository that are labelled "technical support" by Great Scott Gadgets employees can expect a response time of two weeks. We currently do not have expected response times for other GitHub issues or pull requests for this repository. 
