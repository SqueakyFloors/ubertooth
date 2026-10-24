# Changelog

## 2020-12-R1+modern1

### New features

- ubertooth-btle can stream packet capture (PCAP) data to stdout with `-q -`
  or `-c -`, safely separating diagnostics for Wireshark and other consumers.
  Credit: Ryan Govostes ([@rgov](https://github.com/rgov)).
  [#7](https://github.com/SqueakyFloors/ubertooth/issues/7)
  [upstream #362](https://github.com/greatscottgadgets/ubertooth/pull/362)
- Every `-U` option accepts either a Universal Serial Bus (USB) enumeration
  index or stable serial number, and multi-device messages make selectors clear
  without dumping full help text. With several devices and no `-U`,
  `ubertooth-util` read-only queries (`-s`, `-v`, `-V`, `-p`, `-b`, alone or
  combined) report every device and exit non-zero if a device cannot be opened
  or a query fails; `-U all` requests that listing explicitly, even for one
  device, and is refused for every other option.
  [#6](https://github.com/SqueakyFloors/ubertooth/issues/6)
  [#27](https://github.com/SqueakyFloors/ubertooth/issues/27)
  [#28](https://github.com/SqueakyFloors/ubertooth/issues/28)
- `install.sh` provides one-command source installation on Debian/Ubuntu and on
  macOS with Homebrew, with optional virtual environments, a custom `--prefix`,
  `--skip-deps` for already-installed packages, udev setup on Linux, firmware
  builds, and explicitly selected flashing through
  `--flash-device <index|serial|all>`, which now recognises several attached
  boards and refuses to skip one it cannot read.
  [#23](https://github.com/SqueakyFloors/ubertooth/issues/23)
  [#25](https://github.com/SqueakyFloors/ubertooth/issues/25)
- `install.sh` stops with a clear error, before installing or building
  anything, when Python is older than 3.8. [#154](https://github.com/SqueakyFloors/ubertooth/issues/154)
- `install.sh` installs `libnewlib-arm-none-eabi` explicitly, and its apt hint
  names `python3-pyqt5` (Ubuntu has no `python3-pyside6` before 24.10).
  [#152](https://github.com/SqueakyFloors/ubertooth/issues/152)
  [#154](https://github.com/SqueakyFloors/ubertooth/issues/154)
- `ubertooth-util -H`/`--host-version` reports the installed host-tools
  version without a device.
- `ubertooth-util -I` blinks the light-emitting diodes (LEDs) faster for easier
  identification, and now runs the blink on the board like every other mode, so
  the command returns at once and the board stays reachable; `-S` or any mode
  option stops it and leaves the LEDs off.
  [#167](https://github.com/SqueakyFloors/ubertooth/issues/167)
- **Reflash required:** the firmware API version is now 1.11, and the host
  tools require firmware of exactly their own API version, older or newer
  (`ubertooth-util` still reads the version and reflashes any firmware). Host
  and firmware come from the same tree; `2020-12-R1` firmware is refused.
  [#167](https://github.com/SqueakyFloors/ubertooth/issues/167)
- `ubertooth-specan-ui` draws a slowly smoothed average of the max-hold trace
  as a cyan dotted line labelled "Avg: N dBm", shown with the other markers.
  Credit: Sirio Balmelli ([@siriobalmelli](https://github.com/siriobalmelli)).
  [#75](https://github.com/SqueakyFloors/ubertooth/issues/75)
  https://github.com/siriobalmelli-foss/ubertooth/commit/01c3bf9e
- `ubertooth-specan-ui` shows its keys on screen (press `h`; a hint is always
  visible), labels frequencies in whole MHz, no longer leaves ghost marker
  labels behind, draws the plot slightly wider than the scanned range, and has
  a man page and usage guide.
  [#125](https://github.com/SqueakyFloors/ubertooth/issues/125)
  [#126](https://github.com/SqueakyFloors/ubertooth/issues/126)

### Build and compatibility

- Python tools are installed natively under the chosen CMake prefix, so
  custom prefixes and staged installs work without modifying the source tree
  or requiring `PYTHONPATH`.
  [#33](https://github.com/SqueakyFloors/ubertooth/issues/33)
- CMake skips the Python tools with a warning when Python is older than 3.8
  (an error with `-DREQUIRE_ALL=ON`) and names the interpreter it uses.
  [#91](https://github.com/SqueakyFloors/ubertooth/issues/91)
- Modern CMake and macOS builds are supported, including CMake 4 policy
  compatibility, Homebrew libusb headers, corrected cache/error declarations,
  and protection against system `MAX`/`MIN` macro collisions.
  [upstream #551](https://github.com/greatscottgadgets/ubertooth/issues/551)
- Python 3.12 and current setuptools releases are supported. Credit: Arnaud
  Rebillout ([@elboulangero](https://github.com/elboulangero)) and Rhys Perry
  ([@rhysperry111](https://github.com/rhysperry111)).
  [upstream #524](https://github.com/greatscottgadgets/ubertooth/pull/524)
- CMake reports which optional host tools are enabled; `REQUIRE_ALL=ON` makes
  missing optional dependencies fatal, with requirements documented by tool.
- Correct the firmware-build `ubertooth-dfu` variable. Credit: Michael Ossmann
  ([@mossmann](https://github.com/mossmann)).
  [upstream #485](https://github.com/greatscottgadgets/ubertooth/pull/485)
- Replace deprecated Scapy packet-reading usage with `PcapReader`. Credit:
  [@stryngs](https://github.com/stryngs).
  [upstream #519](https://github.com/greatscottgadgets/ubertooth/pull/519)

### Reliability fixes

- `ubertooth-afh -t` now says when the initial detection phase received no
  packet from the piconet, does not start monitoring, and exits with status 2,
  instead of printing an all-zero map and carrying on monitoring until
  interrupted.
  [#187](https://github.com/SqueakyFloors/ubertooth/issues/187)
- `ubertooth-afh` exits 1 when the selected board is not found, instead of
  crashing with a segmentation fault; `ubertooth-follow` now stops there too.
  [#185](https://github.com/SqueakyFloors/ubertooth/issues/185)
- `ubertooth-util -m` says `no range test result yet` when no range test has
  completed since the board started, instead of `invalid range test result`.
  [#190](https://github.com/SqueakyFloors/ubertooth/issues/190)
- Every `ubertooth-*` tool now refuses a word that is not an option, including
  a bare `-`, before opening a device (`error: unexpected argument '<word>'`),
  so a typo such as `- v` is no longer dropped while the rest of the command
  runs. `-h` and `-V` now end the command only after the whole command line
  has parsed, so a mistake written alongside them is still reported.
  `ubertooth-btle -q -`, `-c -` and `-r -` still mean stdin/stdout.
  [#155](https://github.com/SqueakyFloors/ubertooth/issues/155)
- Option values are now parsed the same way by every tool: a whole decimal
  number in range, a lower address part (LAP) of exactly 6 hex digits, an
  upper address part (UAP) of 2, an access address of 1 to 8, and a Bluetooth
  address as `xx:xx:xx:xx:xx:xx`. Values that used to be read as 0 or
  truncated, such as `ubertooth-rx -c abc` or `-l zz3344`, are refused before
  the device is opened. `ubertooth-btle -v` and `ubertooth-ducky -q` also take
  their value as the next word.
  [#155](https://github.com/SqueakyFloors/ubertooth/issues/155)
- Firmware no longer stops answering Universal Serial Bus (USB) control
  requests after a Bluetooth Low Energy (BLE) sniffing mode is stopped, so the
  board no longer needs a re-plug to be usable again.
  [#158](https://github.com/SqueakyFloors/ubertooth/issues/158)
- `-U<index>` with an index that is not attached now opens nothing, instead of
  opening whichever USB device enumerated first and exiting 0: the tool prints
  `error: no Ubertooth at index 2 (2 attached)`, suggests selecting by serial
  (indexes shift when boards are added or removed), and exits non-zero. A nonzero
  index with a single board attached is refused the same way; use `-U0`. An
  index too large to be a device index (`-U4294967296`) is refused as given
  instead of being wrapped into a different index.
  [#188](https://github.com/SqueakyFloors/ubertooth/issues/188)
- `ubertooth-util -q <N>` (LED spectrum analyzer, threshold -N dBm) takes
  20-120, the radio's RSSI range, and bare `-q` uses -85 dBm. The old default
  (225) and `-q183` to `-q225` lit no LED at all; the firmware refuses values
  outside 20-120.
  [#189](https://github.com/SqueakyFloors/ubertooth/issues/189)
- Stopping the LED spectrum analyzer (`ubertooth-util -q`), `ubertooth-specan`
  or BLE faux slave mode (`ubertooth-btle -s <address>`) now resets the board
  like the other modes: LEDs off, amplifier off, channel 2441 MHz and squelch
  -120 dBm. Stopping `-q` used to leave the LEDs lit.
  [#191](https://github.com/SqueakyFloors/ubertooth/issues/191)

- `ubertooth-dfu` now performs bounded, device-specific Device Firmware
  Upgrade (DFU) entry and recovery, retries interrupted transfers, and verifies
  that the selected device returns with the expected firmware revision.
  [#29](https://github.com/SqueakyFloors/ubertooth/issues/29)
  [#31](https://github.com/SqueakyFloors/ubertooth/issues/31)
  [#35](https://github.com/SqueakyFloors/ubertooth/issues/35)
- `ubertooth-ducky -U` now selects devices by index or serial number.
  [#36](https://github.com/SqueakyFloors/ubertooth/issues/36)
- `ubertooth-specan -l`/`-u` now require valid, ordered in-band frequencies
  before opening the device.
  [#37](https://github.com/SqueakyFloors/ubertooth/issues/37)
- `ubertooth-btle -t none` now reliably clears the persistent target filter.
  [#38](https://github.com/SqueakyFloors/ubertooth/issues/38)
- `ubertooth-ego` rejects malformed and out-of-range packet lengths before
  opening the device.
  [#39](https://github.com/SqueakyFloors/ubertooth/issues/39)
- Firmware received signal strength indicator (RSSI) averaging now rounds
  negative decibels relative to one milliwatt (dBm) correctly.
  [#15](https://github.com/SqueakyFloors/ubertooth/issues/15)
- Firmware version strings and header dependencies now update reliably after
  source or revision changes, including GNU Make 3.81 and source archives.
  [#30](https://github.com/SqueakyFloors/ubertooth/issues/30)
- `ubertooth-util` stops a previous operation before starting a new mode and
  validates both frequency and channel-number arguments.
  [#22](https://github.com/SqueakyFloors/ubertooth/issues/22)
  [#5](https://github.com/SqueakyFloors/ubertooth/issues/5)
  [upstream #448](https://github.com/greatscottgadgets/ubertooth/issues/448)
- `ubertooth-util` exits 0 after a successful query such as `-b`, `-c`, `-a`,
  or `-z`, and an earlier failure is no longer hidden by a later success.
  [#40](https://github.com/SqueakyFloors/ubertooth/issues/40)
- `ubertooth-util` no longer prints error codes as values when a query read
  fails (`-c`, `-a`, `-z`, `-l`, `-d`, `-m`, `-9`); it names the failed read on
  standard error instead, as `-b`, `-p` and `-s` now also do. A failed `-9`
  test command now exits non-zero.
  [#59](https://github.com/SqueakyFloors/ubertooth/issues/59)
  [#123](https://github.com/SqueakyFloors/ubertooth/issues/123)
- Host-library failures now propagate correctly and release device resources;
  firmware-image and ducky operations reject short reads.
  [#2](https://github.com/SqueakyFloors/ubertooth/issues/2)
- `ubertooth-util -p` exits non-zero when the device reports a failed part ID
  read instead of printing `Part ID: 0`. In libubertooth, `cmd_get_partnum()`
  now returns a negative error for that case (it returned 0), and the new
  `cmd_get_part_id()` returns the full unsigned 32-bit ID.
  [#52](https://github.com/SqueakyFloors/ubertooth/issues/52)
- `ubertooth-util -v` and `-V` now exit with an error when the firmware
  version can't be read, instead of printing `error: <n>`.
  [#51](https://github.com/SqueakyFloors/ubertooth/issues/51)
- `ubertooth-util` now releases the device when it finishes a command,
  without stopping a running mode.
  [#117](https://github.com/SqueakyFloors/ubertooth/issues/117)
- `ubertooth-util -r` waits up to 10 seconds for the device to re-enumerate
  (by serial with `-U <serial>`) instead of a fixed 2 seconds, so a slow macOS
  re-enumeration no longer fails a successful reset. libubertooth adds
  `ubertooth_start_select_quiet()` for this silent, STOP-free polling, and
  `ubertooth_count()` no longer leaves libusb initialised.
  [#94](https://github.com/SqueakyFloors/ubertooth/issues/94)
- `ubertooth-debug` exits 0 only when every register read succeeds (it used to
  exit with the last register's value) and names each register that fails.
  [#164](https://github.com/SqueakyFloors/ubertooth/issues/164)
- `ubertooth-util` refuses option values that are not a whole number in range
  (`-l`, `-d`, `-c`, `-C`, `-q`, `-z`); `-Cabc` used to set 2402 MHz, and
  `-q0` (once "the default") is now an error: use bare `-q`.
  [#172](https://github.com/SqueakyFloors/ubertooth/issues/172)
- `ubertooth-btle -a` refuses a value that is not 1-8 hex digits (`-azzzz` used
  to set an uninitialized address), refuses get and set together, no longer
  skips `-v` after a get, and reports a failed set. The address can be the
  next word (`-a 8e89bed6`).
  [#171](https://github.com/SqueakyFloors/ubertooth/issues/171)
- `ubertooth-specan` stops any mode left running before it starts and drops
  samples outside `-l`/`-u` left over from an earlier run; tools also stop the
  board cleanly on SIGHUP and SIGALRM (and exit 0, as for SIGINT/SIGTERM).
  [#173](https://github.com/SqueakyFloors/ubertooth/issues/173)
- `ubertooth-util -a0` sets PA level 0 instead of reading the level, and a
  level outside 0-7 is refused before opening the device.
  [#161](https://github.com/SqueakyFloors/ubertooth/issues/161)
- `ubertooth-util` refuses `-z` combined with a mode option, where it silently
  had no effect: a mode stops the device, which resets the squelch.
  [#159](https://github.com/SqueakyFloors/ubertooth/issues/159)
- libubertooth reports a short reply to any fixed-size read as an error instead
  of a value made from stale bytes (such as channel 0 right after hopping).
  [#149](https://github.com/SqueakyFloors/ubertooth/issues/149)
- `ubertooth-util` refuses a word that is not an option, such as `- V`, with an
  error and exit 1 before opening the device, instead of ignoring it and
  running the rest of the command. Option values may now be the next word:
  `-c 2440` sets the channel like `-c2440` (it used to read the channel).
  [#146](https://github.com/SqueakyFloors/ubertooth/issues/146)
- `ubertooth-util` refuses two or more mode options (`-q`, `-n`, `-e`, `-t`,
  `-x`, `-I`, `-i`, `-f`) with an error and exit 1 before opening the device,
  instead of running only one of them and exiting 0.
  [#82](https://github.com/SqueakyFloors/ubertooth/issues/82)
- `ubertooth-specan-ui` plots again: it passed `-U 0` to `ubertooth-specan` as a
  single argument, which was read as a serial, and asked for 2400-2483 MHz,
  which `ubertooth-specan` rejects. Its `-U` now takes an index or a serial like
  the other tools, and it no longer crashes when `C` is pressed before data
  arrives.
  [#124](https://github.com/SqueakyFloors/ubertooth/issues/124)
- `ubertooth-scan` no longer leaks connection information after failed probes.
  [#3](https://github.com/SqueakyFloors/ubertooth/issues/3)
  [upstream #542](https://github.com/greatscottgadgets/ubertooth/issues/542)
- Prevent Adaptive Frequency Hopping (AFH) divide-by-zero failures in both
  baseband hop implementations. Credit: [@Kielukel](https://github.com/Kielukel) and Piotr Wasylów.
  https://github.com/Kielukel/ubertooth
- Firmware AFH hopping (`ubertooth-follow` with an AFH map) now follows the
  piconet's used channels instead of sitting on 2402 MHz, matching the
  Bluetooth spec's re-mapping and same-channel rules. A new AFH map now takes
  effect at once, including in sweep and `ubertooth-afh` modes.
  [#34](https://github.com/SqueakyFloors/ubertooth/issues/34)
- Correct Bluetooth Low Energy cyclic redundancy check filtering and perform
  verification in the physical-layer engine. Credit: oliver
  ([@Oliver0804](https://github.com/Oliver0804)).
  [upstream #550](https://github.com/greatscottgadgets/ubertooth/pull/550)
- Correct firmware signal-strength channel bounds. Credit: Gabriele
  Gristina ([@matrix](https://github.com/matrix)).
  [upstream #481](https://github.com/greatscottgadgets/ubertooth/pull/481)
- Improve Classic Bluetooth paging speed and reliability. Credit: Hillel
  Chimowicz ([@hllhll](https://github.com/hllhll)).
  [upstream #540](https://github.com/greatscottgadgets/ubertooth/pull/540)
- Bluetooth and Bluetooth Low Energy (BLE) transmit and jamming
  (`ubertooth-tx`, `ubertooth-btle -s` and its interference modes,
  `ubertooth-ducky`, `ubertooth-ego -j`) now transmit at the power amplifier
  level set with `ubertooth-util -a`, instead of forcing a level of their own
  and leaving it changed afterwards. Note that this raises the default: without
  `-a` these modes now transmit at the power-on level 7 (0 dBm at the CC2400)
  rather than the level 3 (-7 dBm) they used to force. E-GO jamming already
  used 7, so it is unchanged.
  [#165](https://github.com/SqueakyFloors/ubertooth/issues/165)

### Documentation

- Added practical Classic Bluetooth Basic Rate/Enhanced Data Rate (BR/EDR)
  sniffing and Non-significant Address Part (NAP) / Frequency Hop
  Synchronization (FHS) recovery guidance.
  [#9](https://github.com/SqueakyFloors/ubertooth/issues/9)
  [#42](https://github.com/SqueakyFloors/ubertooth/issues/42)
- Expanded Bluetooth Low Energy (BLE) connection-capture modes, targeting,
  and limitations.
  [#8](https://github.com/SqueakyFloors/ubertooth/issues/8)
- Updated macOS/Homebrew dependencies and CMake 4 build flags.
  [#10](https://github.com/SqueakyFloors/ubertooth/issues/10)
- Added spectrum-analyzer user interface installation and import-error
  troubleshooting, replacing its obsolete README.
  [#11](https://github.com/SqueakyFloors/ubertooth/issues/11)
- Expanded the tool catalog, device-selection guidance, and man pages,
  including previously undocumented flags and corrected command descriptions.
  [#21](https://github.com/SqueakyFloors/ubertooth/issues/21)
- Added the supported platform/toolchain matrix, missing per-tool pages, and
  an archived 2020-12-R1 release guide.
  [#12](https://github.com/SqueakyFloors/ubertooth/issues/12)

_community fork, Fri, 23 Oct 2026 01:38:18 -0700_

## 2020-12-R1

- Bluetooth classic sniffing and link layer support (see btbr)
- Upgraded to Python 3
- Christmas lights!
- Bugfix: ubertooth-btle will no longer freeze in high traffic environments
- Bugfix: time discontinuities in PCAP files
- Bugfix: BLE faux slave can now transmit more than 32 bytes
- USB API version bumped to 1.07

_Mike Ryan &lt;mikeryan@lacklustre.net&gt;, 25 Dec 2020 13:30:00 -0800_

## 2018-12-R1

- Trigger an Uberducky with ubertooth-ducky
- Bugfixes in ubertooth-btle slave mode
- Improvements to spectrum analyzer UI
- USB API version bumped to 1.06

_Mike Ryan &lt;mikeryan@lacklustre.net&gt;, 04 Dec 2018 21:50:00 -0800_

## 2018-08-R1

- Support for partial channel map updates
- LE no-follow mode (advertising packets only)
- USB API version bumped to 1.05

_Mike Ryan &lt;mikeryan@lacklustre.net&gt;, 07 Aug 2018 15:20:00 -0700_

## 2018-06-R1

- Brand new LE sniffing engine
- Support for partial channel maps and channel map updates during connection
- Support for connection updates has been lost, but will be re-added in a
  future release
- Packet filtering with ubertooth-btle -t will filter advertising packets in
  addition to connections
- USB API version bumped to 1.03

_Mike Ryan &lt;mikeryan@lacklustre.net&gt;, 26 Jun 2018 13:05:00 -0700_

## 2017-03-R2

- USB API versioning - We have added USB API versioning to improve the
  experience of failures due to mismatched firmware and host code. You may
  find that Ubertooth tools complain that your firmware is out of date.
  Updating firmware is simple and reliable, please follow the instructions
  on the wiki. A release archive with precompiled firmware is available.
- Basic Rate PCAP output - the pcap file produced by the Ubertooth tools
  should all be read by the latest Wireshark versions without the need to
  install plugins.
- Kismet plugins - Kismet is currently under heavy development and it has
  not been possible to continue tracking the master branch. The legacy
  Kismet plugins should continue to work, but we will wait to track git
  master until development has stabalised.

_Dominic Spill &lt;dominicgs@gmail.com&gt;, 01 Mar 2017 10:39:00 -0700   _

 ubertooth (2015-10-R1) unstable; urgency=low
- This is a minor bugfix and update release
- Update ubertooth-scan to provide more consistent output
- It is now possible to build receive only firmware
- Host code versioning is more reliable
- Python tools now support Python 3. Thanks Hannes
  ([@el-han](https://github.com/el-han))!

_Dominic Spill &lt;dominicgs@gmail.com&gt;, 12 Oct 2015 03:10:00 -0700   _

 ubertooth (2015-09-R2) unstable; urgency=low
- Basic Rate Bluetooth Improvements
  - Stability has improved in performance constrained environments
  - The "uh oh, buffer not emptied" issue no longer causes the tools to loop
        indefinitely
  - AFH support has greatly improved, UAP and CLK values are now found more
        quickly when AFH is used
- Basic Rate PCAP support
  - ubertooth-rx and ubertooth-follow support a new PCAP file format
  - The new PCAP format is also supported with a new Wireshark plugin: btbredr
  - The PCAP format is still in the early stages, but we hope to have the plugin
        included in Wireshark later this year
  - The older btbb format is still produced by the Kismet plugins, they will
        be migrated over to the new format in the next release
- Bluetooth Smart (BLE) Improvements
  - Improved packet reception
  - Promiscuous sniffing mode is more reliable
  - Promiscuous sniffing reports the recovered parameters to the host
- Skateboard Hacking
  - Support for interfering with connections of BLE enabled skateboards
  - ubertooth-ego targets Yuneec E-go (not Bluetooth)
  - Always hack safely and responsibly!

_Dominic Spill &lt;dominicgs@gmail.com&gt;, 05 Sep 2015 05:30:00 -0700   _

## 2014-02-R2

- Bluetooth Smart (Low Energy) Support
  - Promiscuous and follow modes
  - Pcap format packet logging
  - Pairing / encryption support when paired with crackle
  - Credit for BLE features goes to Mike Ryan ([@mikeryan](https://github.com/mikeryan))
- Unified host tool for monitoring Basic Rate
  - ubertooth-rx replaces -lap, -uap, -hop tools
  - Once UAP is discovered, ubertooth-rx automatically tries to find clock
        values and begin hopping
  - Thanks to Will Code ([@willcode](https://github.com/willcode)) for working on this
- Survey tool - ubertooth-scan
  - Combining both Ubertooth and a standard Bluetooth dongle
  - Ubertooth scans for non-discoverable master devices
  - Dongle probes devices for piconet information and features
- Cmake now used for the build system
  - Improves support for non-Linux operating systems
  - More sensible handling of dependencies
  - New build instructions
- Packaging (Experimental)
  - Early stage support for packaging systems
  - libbtbb in Homebrew repository, Ubertooth coming soon
  - MacPorts availability is under test
  - Release already available in Pentoo
- GitHub migration
  - libbtbb, Ubertooth and gr-bluetooth all hosted on GitHub
  - Allows for more open development and collaboration model
  - Already seeing an increase in issue reporting and pull requests

_Dominic Spill &lt;dominicgs@gmail.com&gt;, 20 Feb 2014 06:06:00 -0700   _
