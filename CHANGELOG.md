# Changelog

## 2020-12-R1+modern1

### New features

- **Script-visible change:** `ubertooth-btle` refuses `-i`/`-I` without `-f` or
  `-p`, and `-a`/`-s` alongside `-f`, `-n` or `-p`, before it opens a device.
  Those command lines, and a mode the board refuses to start, now exit 1 where
  they used to exit 0, so a script that read that as success sees a failure.
  [#299](https://github.com/SqueakyFloors/ubertooth/issues/299)
  [#309](https://github.com/SqueakyFloors/ubertooth/issues/309)
- **Script-visible change:** `ubertooth-btle` exits 0 for `-h` and 1 when a
  capture stops because the board stopped answering; it answers `-a`, `-v` and
  `-t` on standard error when `-q -` or `-c -` has taken standard output, and
  reports a bad command line, a capture file it cannot create, and a failed
  request the way the converted tools do.
  [#308](https://github.com/SqueakyFloors/ubertooth/issues/308)
  [#330](https://github.com/SqueakyFloors/ubertooth/issues/330)
  [#346](https://github.com/SqueakyFloors/ubertooth/issues/346)
  [#347](https://github.com/SqueakyFloors/ubertooth/issues/347)
- **Script-visible change:** `ubertooth-afh -a` is gone. It never sent
  anything, and it silently swallowed the word after it.
  [#21](https://github.com/SqueakyFloors/ubertooth/issues/21)
- **Script-visible change:** `ubertooth-util -9` is gone, with the two
  developer scratch requests it sent, which the firmware no longer answers.
  [#21](https://github.com/SqueakyFloors/ubertooth/issues/21)
- **Script-visible change:** `ubertooth-util -f` is gone; use `ubertooth-dfu`
  to reflash a board.
  [#333](https://github.com/SqueakyFloors/ubertooth/issues/333)
- **Script-visible change:** `ubertooth-util -N` prints the device count and
  then runs the rest of the command, instead of dropping it.
  [#332](https://github.com/SqueakyFloors/ubertooth/issues/332)
- `ubertooth-btle` and `ubertooth-ego` list their `-j`/`-J` jam aliases in `-h`.
  [#21](https://github.com/SqueakyFloors/ubertooth/issues/21)
- `ubertooth-rx` lists `-s` in `-h` as the explicit form of its default channel
  sweep, and no longer calls it deprecated; the `-z` line no longer claims to
  imply it.
  [#21](https://github.com/SqueakyFloors/ubertooth/issues/21)
- **Script-visible change:** every tool now keeps standard output for its
  results and capture data; errors, warnings and progress go to standard
  error, `-h` exits 0, and a command line with nothing to do is refused. A
  file a tool cannot open is reported as `error: <file>: <reason>`.
  [#323](https://github.com/SqueakyFloors/ubertooth/issues/323)
- **Script-visible change:** `ubertooth-util` refuses a command line that asks
  for nothing, `-U 0` on its own included: it used to open the board, do
  nothing and exit 0.
  [#367](https://github.com/SqueakyFloors/ubertooth/issues/367)
- **Script-visible change:** `ubertooth-util -m` reports a range test whose
  packets came back corrupted as `error: invalid range test: mismatch on byte
  <n>` on standard error and exits 1; it was a line on standard output with
  exit 0.
  [#338](https://github.com/SqueakyFloors/ubertooth/issues/338)
- `ubertooth-specan` reports a bad option itself, as the other tools do:
  `error: unrecognized option '-Z'` and `error: option '-l' requires a value`,
  instead of the option parser's own lines naming the whole path of the binary.
  [#338](https://github.com/SqueakyFloors/ubertooth/issues/338)
- **Script-visible change:** after an error, tools print only the error; run
  `-h` for the usage text. A tool that needs arguments and is run with none
  still prints it, on standard error.
  [#323](https://github.com/SqueakyFloors/ubertooth/issues/323)
- ubertooth-btle can stream PCAP data to stdout with `-q -`
  or `-c -`, safely separating diagnostics for Wireshark and other consumers.
  Credit: Ryan Govostes ([@rgov](https://github.com/rgov)).
  [#7](https://github.com/SqueakyFloors/ubertooth/issues/7)
  [upstream #362](https://github.com/greatscottgadgets/ubertooth/pull/362)
- Every `-U` option accepts either a USB enumeration
  index or stable serial number, and multi-device messages make selectors clear
  without dumping full help text. With several devices and no `-U`,
  `ubertooth-util` read-only queries (`-s`, `-v`, `-V`, `-p`, `-b`, alone or
  combined) report every device and exit non-zero if a device cannot be opened
  or a query fails; `-U all` requests that listing explicitly, even for one
  device, and is refused for every other option but `-N`, saying which queries
  it needs when none was given. `-U all -N` alone just prints the count, as
  `-U 0 -N` does.
  [#6](https://github.com/SqueakyFloors/ubertooth/issues/6)
  [#27](https://github.com/SqueakyFloors/ubertooth/issues/27)
  [#28](https://github.com/SqueakyFloors/ubertooth/issues/28)
  [#396](https://github.com/SqueakyFloors/ubertooth/issues/396)
  [#401](https://github.com/SqueakyFloors/ubertooth/issues/401)
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
- `ubertooth-util -I` blinks the LEDs faster for easier
  identification, and now runs the blink on the board like every other mode, so
  the command returns at once and the board stays reachable; `-S` or any mode
  option stops it and leaves the LEDs off.
  [#167](https://github.com/SqueakyFloors/ubertooth/issues/167)
- **Reflash required:** the firmware API version is now 1.23, last moved by the
  control requests that no longer stall during a capture, and the host tools
  require firmware of exactly their own API version, older or newer
  (`ubertooth-util` still reads the version and reflashes any firmware). Host
  and firmware come from the same tree; `2020-12-R1` firmware is refused.
  [#21](https://github.com/SqueakyFloors/ubertooth/issues/21)
  [#167](https://github.com/SqueakyFloors/ubertooth/issues/167)
  [#240](https://github.com/SqueakyFloors/ubertooth/issues/240)
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

- Removed the unmaintained Dockerfile.
  [#324](https://github.com/SqueakyFloors/ubertooth/issues/324)
- A build no longer rewrites the committed man pages in `host/doc/man`, and
  installs the same pages with or without ronn. Refresh them after editing a
  `host/doc/*.md`: `cmake --build <build dir> --target refresh-manpages`; CI
  checks that the committed pages match. The installed pages now carry the
  text of their Markdown, which a few had drifted from.
  [#254](https://github.com/SqueakyFloors/ubertooth/issues/254)
  [#311](https://github.com/SqueakyFloors/ubertooth/issues/311)
- `man ubertooth-debug` is installed; its page was built but never installed.
  [#235](https://github.com/SqueakyFloors/ubertooth/issues/235)
- `man ubertooth-ducky`, `man ubertooth-follow` and `man ubertooth-tx` now
  work: those three pages are generated from their `host/doc/*.md` and
  installed with the rest, instead of shipping only as Markdown.
  [#313](https://github.com/SqueakyFloors/ubertooth/issues/313)
  [#327](https://github.com/SqueakyFloors/ubertooth/issues/327)
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
- libbtbb is built from this project's
  [fork](https://github.com/SqueakyFloors/libbtbb) at a pinned commit, needing
  no `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`; `install.sh --libbtbb-url`/`--libbtbb-ref`
  build a different one.
  [#320](https://github.com/SqueakyFloors/ubertooth/issues/320)
- The host builds again on Ubuntu 20.04, this fork's declared minimum, where
  glibc still keeps the pthread calls out of libc: the libubertooth targets
  link pthread, so linking the library no longer leaves `pthread_create`
  undefined.
  [#315](https://github.com/SqueakyFloors/ubertooth/issues/315)

### Reliability fixes

- A control request sent to a board that is receiving, transmitting or scanning
  no longer stalls. `ubertooth-util`, `ubertooth-debug` and anything else that
  reads the channel, a register or the clock during a capture used to fail
  roughly one call in twenty in Bluetooth follow mode, because the board stopped
  answering while it retuned.
  [#149](https://github.com/SqueakyFloors/ubertooth/issues/149)
  [#223](https://github.com/SqueakyFloors/ubertooth/issues/223)
- Three modes that could only be ended by unplugging the board now stop when
  asked: generic receive (`ubertooth-debug` after `UBERTOOTH_RX_GENERIC`), the
  range test and the repeater. Each waited for a packet without answering the
  host, so on a quiet channel the board was unreachable; the repeater never
  looked at the requested mode at all.
  [#223](https://github.com/SqueakyFloors/ubertooth/issues/223)
- `ubertooth-util -r` no longer reports an older range test's numbers as the
  current test's. A test that found no reply, or that the host stopped, now
  leaves no result, and the command says so.
- **Script-visible change:** a capture whose USB stream fails stops there and
  exits 1, instead of waiting out its `-t` timer with nothing arriving and
  reporting success. `ubertooth-rx`, `ubertooth-dump`, `ubertooth-specan`,
  `ubertooth-scan`, `ubertooth-follow` and `ubertooth-afh` are affected.
  [#319](https://github.com/SqueakyFloors/ubertooth/issues/319)
- The pinned libbtbb now follows the Core specification when AFH re-maps a hop
  channel, so host-side predictions agree with the channel the board tunes to.
  A clock and UAP tracked with an AFH map used to name channels the piconet was
  not on.
  [#143](https://github.com/SqueakyFloors/ubertooth/issues/143)
- `ubertooth-ducky -q` and `-b` name a request the board refused and exit 1,
  instead of waiting their two seconds and exiting 0 on a request the board
  refused.
  [#328](https://github.com/SqueakyFloors/ubertooth/issues/328)
- `ubertooth-ego` names a request the board refused while setting a capture up
  (channel, access code, capture length) and exits 1, instead of capturing with
  whatever settings the board was left on.
  [#379](https://github.com/SqueakyFloors/ubertooth/issues/379)
- `ubertooth-rx` and `ubertooth-scan` name a request the board refused while
  setting a capture up (the channel or band sweep, and `ubertooth-rx`'s
  Bluetooth address for `-l`/`-u`) and exit 1, instead of capturing on whatever
  channel the board was left on and reporting success. The one line says why,
  and a `note:` says what to try. A board that takes only part of a Bluetooth
  address is a failure too, so `ubertooth-tx` stops there rather than
  transmitting.
  [#379](https://github.com/SqueakyFloors/ubertooth/issues/379)
- `ubertooth-ego` refuses `-c` combined with `-f`, `-i` or `-j`, where it
  silently had no effect: those modes hop across the E-GO's own channel list.
  [#394](https://github.com/SqueakyFloors/ubertooth/issues/394)
- `ubertooth-ego -a` with an access code shorter than four bytes sets the code
  the user asked for, zero-filled, instead of padding it with leftovers of the
  previous request.
  [#402](https://github.com/SqueakyFloors/ubertooth/issues/402)
- Stopping a capture that has packets in hand (Ctrl-C, or an `ubertooth-afh` or
  `ubertooth-rx` timeout with traffic flowing) no longer releases a receive
  transfer the USB layer is still using, which could corrupt memory or crash
  the tool. A board that never gives the transfer back is now reported and
  left open rather than closed underneath it.
  [#370](https://github.com/SqueakyFloors/ubertooth/issues/370)
- A follow-mode control command the board refuses now names itself and says
  what went wrong, for example `error: start hopping failed: the board does
  not support this command (stalled)`. It used to read the completion status
  as a libusb error code and print `libUSB Error: Other error:  (4)`.
  [#275](https://github.com/SqueakyFloors/ubertooth/issues/275)
- `ubertooth-follow -a` needs `-l` and `-u`, and stops with an error instead of
  following an AFH map it could not read or that has fewer than 20 channels. A
  follow without a map clears any map an earlier run left on the board.
  [#44](https://github.com/SqueakyFloors/ubertooth/issues/44)
- `ubertooth-dfu -d` and `-s` check the file before they read it into memory:
  a directory, a FIFO or a device node is refused, and so is a file too large
  to be an image (507920 bytes for `-d`, 16 fewer for `-s`, which adds the
  suffix), instead of allocating from a size nothing had checked.
  [#246](https://github.com/SqueakyFloors/ubertooth/issues/246)
- A set and a read of `ubertooth-btle`'s CRC verify setting in one command both
  run, the set first, so `-v1 -v` reports the new value instead of dropping one
  of the two. A read works alongside `-f`, `-n` and `-p` as well, and a set the
  board does not answer exits 1 instead of being ignored.
  [#265](https://github.com/SqueakyFloors/ubertooth/issues/265)
- `btle-extcap.py` names each Wireshark interface by its board's serial, so
  re-plugging boards cannot move a capture to another board and two or more
  boards no longer list as `Ubertooth One 0:` repeated. The name changed with
  it, so an Ubertooth saved in a Wireshark profile has to be picked once more.
  [#269](https://github.com/SqueakyFloors/ubertooth/issues/269)
- `btle-extcap.py` takes the options Wireshark adds to its own calls, so an
  Ubertooth reaches Wireshark's capture list instead of the listing being
  refused, and a failed or refused capture exits non-zero.
  [#297](https://github.com/SqueakyFloors/ubertooth/issues/297)
  [#334](https://github.com/SqueakyFloors/ubertooth/issues/334)
- `btle-extcap.py --extcap-interfaces` no longer hides a failing
  `ubertooth-util` as an empty interface list: it passes the reason on and
  exits 1, and reports a tool missing from `PATH` instead of a traceback.
  [#362](https://github.com/SqueakyFloors/ubertooth/issues/362)
- `btle-extcap.py -h` names the options the script accepts, not
  `--list-interfaces`, `--list-dlts` and `--config`, which it rejects.
  [#268](https://github.com/SqueakyFloors/ubertooth/issues/268)
- A set and a read of the same `ubertooth-util` value in one command both run,
  the set first, so `-c2426 -c`, `-a5 -a`, `-l1 -l` and `-d1 -d` report the new
  value instead of dropping the set.
  [#228](https://github.com/SqueakyFloors/ubertooth/issues/228)
- A refused command line no longer creates or truncates an output file. Every
  file option (`ubertooth-btle -r`/`-q`/`-c`, `ubertooth-follow` and
  `ubertooth-rx -r`/`-q`/`-d`, `ubertooth-dump -d`, `ubertooth-specan -d`,
  `ubertooth-dfu -s`/`-u`) opens its file only once the whole command line has
  been accepted, and `Ignoring extra capture file` goes to standard error so it
  cannot land in a capture written to stdout.
  [#193](https://github.com/SqueakyFloors/ubertooth/issues/193)
- `ubertooth-debug -r` states the register range it accepts and quotes the
  item it refused, `error: register address must be 0x00-0xff (got 300 in
  0x02,300)`. A `%name` works anywhere in a list or range, not only last.
  [#235](https://github.com/SqueakyFloors/ubertooth/issues/235)
- `ubertooth-rx -u` without `-l`, `ubertooth-tx` without both, and more than
  one of `ubertooth-btle`'s `-f`, `-n` and `-p` are refused while the command
  line is read, before a device is opened, whatever boards are attached.
  [#236](https://github.com/SqueakyFloors/ubertooth/issues/236)
- A `ubertooth-btle` command line with no work in it (a bare run, or only `-A`,
  `-x`, `-i`/`-I` or a capture option) is refused with `error: nothing to do:
  ...` and exit 1, instead of opening a board and exiting 0.
  [#258](https://github.com/SqueakyFloors/ubertooth/issues/258)
- `ubertooth-specan -d` takes a bare `-` as standard output and every other
  value as a file name, including one starting with `-`, so `-d -x` writes the
  file `-x` instead of streaming binary samples to the terminal.
  [#231](https://github.com/SqueakyFloors/ubertooth/issues/231)
- `ubertooth-specan -d -` refuses a terminal, as `ubertooth-btle -q -` already
  did: `error: stdout is a tty, refusing to write binary samples to it`, exit
  status 1. A pipe or a redirect is unaffected, and btle's own refusal now
  reads the same way.
  [#259](https://github.com/SqueakyFloors/ubertooth/issues/259)
- `ubertooth-afh -t` now says when the initial detection phase received no
  packet from the piconet, does not start monitoring, and exits with status 2,
  instead of printing an all-zero map and carrying on monitoring until
  interrupted.
  [#187](https://github.com/SqueakyFloors/ubertooth/issues/187)
- `ubertooth-rx -i` now reports the host time stored with each record instead
  of the time of the replay: the `systime` of every packet line, and the
  timestamps of a PCAP or PcapNG written while replaying, come from the
  capture. [#202](https://github.com/SqueakyFloors/ubertooth/issues/202)
- PCAP and PcapNG timestamps now start at the time the capture started. They
  were offset by a fixed amount, up to a few minutes, that depended on the
  board's clock when the first packet arrived.
  [#202](https://github.com/SqueakyFloors/ubertooth/issues/202)
- `ubertooth-rx -i` reports a file it cannot read, such as a directory
  (`error: reading <file> failed: Is a directory`), and a last record cut
  short, and exits 1. Both used to end the replay silently with exit 0.
  [#203](https://github.com/SqueakyFloors/ubertooth/issues/203)
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
- Firmware no longer stops answering USB control
  requests after a BLE sniffing mode is stopped, so the
  board no longer needs a re-plug to be usable again.
  [#158](https://github.com/SqueakyFloors/ubertooth/issues/158)
- `ubertooth-dfu -s` refuses a binary that already carries a DFU suffix
  instead of adding a second one, and names the output
  file when it cannot create it. `-s` and `-d` now only read their input, so a
  firmware image in a read-only location can be signed or flashed. A `-u`
  read-back that fails part-way no longer empties an earlier file of the same
  name: it is written to a temporary file and renamed once complete. The
  documentation now says what `-u` writes (a raw dump of the application flash
  region, not a DFU file).
  [#200](https://github.com/SqueakyFloors/ubertooth/issues/200)
- `-U<index>` with an index that is not attached now opens nothing, instead of
  opening whichever USB device enumerated first and exiting 0: the tool prints
  `error: no Ubertooth at index 2 (2 attached)`, suggests selecting by serial
  (indexes shift when boards are added or removed), and exits non-zero. A nonzero
  index with a single board attached is refused the same way; use `-U0`. An
  index too large to be a device index (`-U4294967296`) is refused as given
  instead of being wrapped into a different index.
  [#188](https://github.com/SqueakyFloors/ubertooth/issues/188)
- `-U<serial>` takes the full serial or its first few characters, in either
  case, and refuses characters that start more than one attached board's serial
  (`error: '1c' matches 2 Ubertooth devices: ...`). An attached board that
  could not be opened or did not answer is reported with the result, instead of
  leaving a non-match looking like a serial that is not attached. The rules are
  documented in ubertooth-util(1), SELECTING A DEVICE, and ubertooth(7).
  [#210](https://github.com/SqueakyFloors/ubertooth/issues/210)
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
- `ubertooth-dfu -d` checks the whole image before the board leaves its
  application, so a corrupt, truncated, unsigned or empty file is refused with
  exit 1 and leaves the board running its firmware instead of stranded in the
  DFU bootloader. A failure once the board is already
  in DFU now detaches it anyway when `-r` was given, finding the DFU device
  again if the failure closed the handle. The image's DFU suffix must also name
  this device (`1d50:6003`, or the `0xffff` wildcard): firmware for another
  vendor or product used to be written.
  [#198](https://github.com/SqueakyFloors/ubertooth/issues/198)
  [#199](https://github.com/SqueakyFloors/ubertooth/issues/199)
- Control requests now answer reliably while the LED spectrum analyzer
  (`ubertooth-util -q`) runs: reads such as `-d`, `-C` and `-z` no longer fail
  every few samples with a stalled transfer, and `-S` stops the mode even if
  the radio stops reporting a synthesizer lock.
  [#213](https://github.com/SqueakyFloors/ubertooth/issues/213)
- Stopping a mode always resets the board before the next one starts, so two
  requests sent close together no longer carry one mode's LEDs, amplifier,
  channel and squelch into the next; and a mode request that arrives while a
  BLE mode shuts down is no longer dropped, which used to leave the board idle
  after the host had been told the mode started.
  [#209](https://github.com/SqueakyFloors/ubertooth/issues/209)
- A capture now holds only packets from the mode it started: stopping a mode
  drops what it had queued and resets its keep-alive timer, and a stream clears
  the board's receive endpoint before it reads. A basic-rate dump taken after a
  BLE capture no longer begins with two low-energy packets, or with an empty
  record from a stray keep-alive.
  [#179](https://github.com/SqueakyFloors/ubertooth/issues/179)
  [#318](https://github.com/SqueakyFloors/ubertooth/issues/318)
- `btbr` firmware: a host that stops reading no longer leaves the board
  answering nothing until it is power-cycled. A debug or fault message now
  waits about a tenth of a second for the host to read it and is then dropped,
  and the board keeps answering commands while it waits.
  [#282](https://github.com/SqueakyFloors/ubertooth/issues/282)
- `ubertooth-dfu -d <image> -r` no longer exits 1 after a flash that worked:
  the bootloader resets while the detach request is in flight, so the request
  fails on every macOS flash. The board is reported as still in DFU only when
  its DFU device really is still there.
  [#267](https://github.com/SqueakyFloors/ubertooth/issues/267)
- `ubertooth-dfu -r` no longer exits 0 with the board still in the bootloader
  when the bootloader acknowledges the reset request and then ignores it: the
  board is reported as detached only once its DFU device has left the bus,
  whatever the request returned.
  [#283](https://github.com/SqueakyFloors/ubertooth/issues/283)
- `ubertooth-dfu -s` writes `<file>.dfu` only once the whole signed image is
  ready, and checks a `-d` image given in the same command first, so a signing
  run that fails leaves an earlier `<file>.dfu` untouched and creates none. It
  now needs to be able to create a file in the output's directory, as `-u`
  does.
  [#305](https://github.com/SqueakyFloors/ubertooth/issues/305)
  [#286](https://github.com/SqueakyFloors/ubertooth/issues/286)
- `ubertooth-dfu -u <dump> -d <image>` no longer leaves a temporary dump file
  beside the target when the image is refused.
  [#241](https://github.com/SqueakyFloors/ubertooth/issues/241)
- `ubertooth-afh` now detects the channel map from the board's AFH hopping
  mode, which had never run: the firmware's AFH mode returned as soon as it was
  entered, and the capture that followed hopped only by leftover state, so the
  tool could end up parked on one channel and report only that one as used.
  Hopping across the channels the piconet is not yet known to use is now what
  the board is actually asked to do.
  [#238](https://github.com/SqueakyFloors/ubertooth/issues/238)
- `ubertooth-dfu -u` reads back only the flash the part has, instead of a
  fixed 240 KiB that faulted the bootloader on a 128 KiB part, and says how
  many bytes it is reading and where that figure came from.
  [#233](https://github.com/SqueakyFloors/ubertooth/issues/233)
- `ubertooth-dfu -d` refuses an image too large for the attached part, naming
  the part and the flash it has, while the board is still running its
  application; a part it cannot identify is reported as unchecked.
  [#281](https://github.com/SqueakyFloors/ubertooth/issues/281)
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
- Firmware RSSI averaging now rounds negative dBm correctly.
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
  fails (`-c`, `-a`, `-z`, `-l`, `-d`, `-m`); it names the failed read on
  standard error instead, as `-b`, `-p` and `-s` now also do.
  [#59](https://github.com/SqueakyFloors/ubertooth/issues/59)
  [#123](https://github.com/SqueakyFloors/ubertooth/issues/123)
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
  instead of a fixed 2 seconds, so a slow macOS re-enumeration no longer fails
  a successful reset. libubertooth adds
  `ubertooth_start_select_quiet()` for this silent, STOP-free polling, and
  `ubertooth_count()` no longer leaves libusb initialised.
  [#94](https://github.com/SqueakyFloors/ubertooth/issues/94)
- `ubertooth-util -r` reads the board's serial before the reset and waits for
  that serial, so later options in the same command (`-U0 -r -b`) stay on the
  board that was reset even if the index order changed across the reset. That
  read is quiet, through libubertooth's new `ubertooth_read_serial_string()`:
  a board that has stopped answering no longer prints a transfer error, or
  costs a second of timeout, on the command used to recover it.
  [#139](https://github.com/SqueakyFloors/ubertooth/issues/139)
  [#306](https://github.com/SqueakyFloors/ubertooth/issues/306)
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
- Firmware AFH hopping (`ubertooth-follow` with an AFH map) now follows the
  piconet's used channels instead of sitting on 2402 MHz, matching the
  Bluetooth spec's re-mapping and same-channel rules. A new AFH map now takes
  effect at once, including in sweep and `ubertooth-afh` modes.
  [#34](https://github.com/SqueakyFloors/ubertooth/issues/34)
- The `btbr` firmware image hops to the channels the Bluetooth spec selects.
  Under an AFH map it now keeps a used channel of the basic sequence instead of
  always selecting inside the reduced set, and falls back to basic hopping
  below the spec minimum of 20 used channels; without a map it no longer picks
  the wrong channel for some addresses.
  [#13](https://github.com/SqueakyFloors/ubertooth/issues/13)
- Correct Bluetooth Low Energy cyclic redundancy check filtering and perform
  verification in the physical-layer engine. Credit: oliver
  ([@Oliver0804](https://github.com/Oliver0804)).
  [upstream #550](https://github.com/greatscottgadgets/ubertooth/pull/550)
- Improve Classic Bluetooth paging speed and reliability. Credit: Hillel
  Chimowicz ([@hllhll](https://github.com/hllhll)) and
  [@shapaz](https://github.com/shapaz).
  [upstream #540](https://github.com/greatscottgadgets/ubertooth/pull/540)
- Bluetooth and BLE transmit and jamming
  (`ubertooth-tx`, `ubertooth-btle -s` and its interference modes,
  `ubertooth-ducky`, `ubertooth-ego -j`) now transmit at the power amplifier
  level set with `ubertooth-util -a`, instead of forcing a level of their own
  and leaving it changed afterwards. Note that this raises the default: without
  `-a` these modes now transmit at the power-on level 7 (0 dBm at the CC2400)
  rather than the level 3 (-7 dBm) they used to force. E-GO jamming already
  used 7, so it is unchanged.
  [#165](https://github.com/SqueakyFloors/ubertooth/issues/165)
- The Classic Bluetooth (`btbr`) firmware keeps the power amplifier level set
  with `ubertooth-util -a` instead of forcing level 3 when it brings the radio
  up, so its default rises to the power-on level 7 the same way. A generic
  transmit still sends at the level in its own request, but leaves the board's
  level as it found it.
  [#181](https://github.com/SqueakyFloors/ubertooth/issues/181)
- PCAP and PcapNG timestamps no longer jump 101.8 s every time the board's
  capture clock rolls over, which it does every 327.68 s, so packet times in a
  long capture are continuous again, live and on `ubertooth-rx -i` replay.
  [#218](https://github.com/SqueakyFloors/ubertooth/issues/218)
- PCAP and PcapNG timestamps also survive a quiet gap: a Classic capture
  counts the capture clock's periods from the packets, which carry the count,
  and where they do not, a lost one is reported on standard error.
  [#229](https://github.com/SqueakyFloors/ubertooth/issues/229)

### Documentation

- `ubertooth-ego -h` and its man page no longer call `630f9ffe` the default
  access code. A run without `-a` sends none, so it uses whichever one the
  board was last given; `630f9ffe` is only the firmware's value after a reset.
  [#397](https://github.com/SqueakyFloors/ubertooth/issues/397)
- `ubertooth-ego -h` and its man page now say that `-r` without `-c` receives
  on 2418 MHz.
  [#413](https://github.com/SqueakyFloors/ubertooth/issues/413)
- Help text, man pages and examples write option values as separate words
  (`-c 2440`), following the POSIX Utility Syntax Guidelines; the attached form
  (`-c2440`) still works. ubertooth(7), OPTION VALUES, explains the forms once.
  [#195](https://github.com/SqueakyFloors/ubertooth/issues/195)
- Every tool now describes its device selector as `-U <index|serial>`, in its
  help text and its man page, and points at ubertooth(7), SELECTING A DEVICE,
  for the rules. The old `-U<0-7>` was wrong twice over: the index is a
  position in the list of attached boards and is not capped at 7, and `-U`
  also takes a serial number.
  [#206](https://github.com/SqueakyFloors/ubertooth/issues/206)
- Added practical Classic Bluetooth Basic Rate/Enhanced Data Rate (BR/EDR)
  sniffing and Non-significant Address Part (NAP) / Frequency Hop
  Synchronization (FHS) recovery guidance.
  [#9](https://github.com/SqueakyFloors/ubertooth/issues/9)
  [#42](https://github.com/SqueakyFloors/ubertooth/issues/42)
- Expanded BLE connection-capture modes, targeting,
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

### Under the hood

Fixes with no visible change in normal use.

- A BLE packet whose length field claims more than the board captured reads
  zeros past the capture, instead of whatever followed it in memory; the
  Kismet plugins' access code search stays inside its bank too.
  [#416](https://github.com/SqueakyFloors/ubertooth/issues/416)
- The firmware refuses a generic transmit request whose payload does not fit
  the packet, and no longer drops the last byte of one that does. No shipped
  tool sends it.
  [#384](https://github.com/SqueakyFloors/ubertooth/issues/384)
- The firmware stalls a vendor request that carries fewer bytes than it reads,
  and a read-direction request that expects data, instead of acting on what
  the previous request left in the shared buffer.
  [#393](https://github.com/SqueakyFloors/ubertooth/issues/393)
- The firmware's waits on the USB controller and on the radio are bounded, so
  fewer ways are left for a board to stop answering until it is re-plugged.
  [#239](https://github.com/SqueakyFloors/ubertooth/issues/239)
- A BLE sniffing mode counts as running only once its first receive buffer and
  the radio are ready, and starts with no timer match left pending from the
  previous run.
  [#248](https://github.com/SqueakyFloors/ubertooth/issues/248)
- The firmware's generic receive mode picks between its packet-mode and
  symbol-stream receives by the radio's packet-mode bit, so the symbol stream
  is reachable at all.
  [#252](https://github.com/SqueakyFloors/ubertooth/issues/252)
- The bootloader refuses a read-back past the end of the flash the attached
  part has, instead of faulting and answering nothing until the board is
  power-cycled. It is reached only through ISP or JTAG, not `ubertooth-dfu`.
  [#256](https://github.com/SqueakyFloors/ubertooth/issues/256)
- `ubertooth-dfu` bounds every wait on the bootloader and retries a read-back
  that failed part-way, instead of waiting forever or reporting a truncated
  error code.
  [#208](https://github.com/SqueakyFloors/ubertooth/issues/208)
  [#216](https://github.com/SqueakyFloors/ubertooth/issues/216)
- `ubertooth-dfu` enters and recovers from DFU with bounded, device-specific
  steps, retries an interrupted transfer, and checks that the board comes back
  with the firmware revision it expects.
  [#29](https://github.com/SqueakyFloors/ubertooth/issues/29)
  [#31](https://github.com/SqueakyFloors/ubertooth/issues/31)
  [#35](https://github.com/SqueakyFloors/ubertooth/issues/35)
- Follow mode's asynchronous control commands give the USB layer a buffer that
  outlives the submit, and release the transfer when a submit fails.
  [#245](https://github.com/SqueakyFloors/ubertooth/issues/245)
- A tool that cannot allocate its capture ring buffer says so and exits
  instead of crashing.
  [#247](https://github.com/SqueakyFloors/ubertooth/issues/247)
- A capture gives its receive transfer back when it ends, and `ubertooth-rx`,
  `ubertooth-scan` and `ubertooth-follow` give the board back when they cannot
  start one.
  [#307](https://github.com/SqueakyFloors/ubertooth/issues/307)
  [#370](https://github.com/SqueakyFloors/ubertooth/issues/370)
- `ubertooth-afh`, `ubertooth-ducky` and `ubertooth-ego` give back everything
  they allocate before they exit.
  [#366](https://github.com/SqueakyFloors/ubertooth/issues/366)
  [#381](https://github.com/SqueakyFloors/ubertooth/issues/381)
  [#388](https://github.com/SqueakyFloors/ubertooth/issues/388)
- `ubertooth-scan` no longer leaks connection information after failed probes.
  [#3](https://github.com/SqueakyFloors/ubertooth/issues/3)
  [upstream #542](https://github.com/greatscottgadgets/ubertooth/issues/542)
- Host-library failures reach the caller and release the device, and a short
  read of a firmware image or a ducky payload is refused.
  [#2](https://github.com/SqueakyFloors/ubertooth/issues/2)
- AFH hopping no longer divides by zero in either baseband hop implementation.
  Credit: [@Kielukel](https://github.com/Kielukel) and Piotr Wasylów.
  https://github.com/Kielukel/ubertooth
- Corrected firmware signal-strength channel bounds. Credit: Gabriele
  Gristina ([@matrix](https://github.com/matrix)).
  [upstream #481](https://github.com/greatscottgadgets/ubertooth/pull/481)

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
