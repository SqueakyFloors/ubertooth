# Changelog

## 2020-12-R1+modern1

### New features

- Host tools built from a git checkout that is not on a tag warn about
  firmware of another API version and run the command anyway, instead of
  refusing it, so host tools and firmware from different commits can be used
  together without a reflash first. A release build still refuses it.
  `UBERTOOTH_ALLOW_API_MISMATCH=1` runs on whatever the build was, and `0`
  refuses whatever the build was.
  [#636](https://github.com/SqueakyFloors/ubertooth/issues/636)
- A request the board's firmware has not got is reported in one wording
  everywhere, naming the request and both firmware API versions: `error: could
  not set the channel: the board's firmware does not support it (stalled,
  libusb -9)` with `note: the board reports firmware API version <version>,
  libubertooth 1.1 requires <version>`. It replaces three different lines, one
  of which said only `control message unsupported`.
  [#636](https://github.com/SqueakyFloors/ubertooth/issues/636)
- **Script-visible change:** `ubertooth-util -n` now runs the range test
  instead of only starting it: progress while it waits, then the result block
  `-m` prints. Nothing answering within 10 seconds (`-n <seconds>` to choose
  that wait, `-n 0` to wait forever) stops the test and exits 1.
  [#649](https://github.com/SqueakyFloors/ubertooth/issues/649)
- `ubertooth-follow -t <BD_ADDR>` names the whole target address, so a follow
  reaches a target whose address does not begin `00:00`. It gives the LAP and
  UAP to follow as well.
  [#316](https://github.com/SqueakyFloors/ubertooth/issues/316)
- **Script-visible change:** `ubertooth-follow -a` needs `-t` instead of `-l`
  and `-u`, and the tool's progress moved from standard output to standard
  error.
  [#316](https://github.com/SqueakyFloors/ubertooth/issues/316)
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- **Script-visible change:** `ubertooth-follow -l` without `-u`, or `-u`
  without `-l`, is refused and names the missing half. It used to say "No
  address given" and follow the local adapter's own address instead of the
  piconet that was named.
  [#529](https://github.com/SqueakyFloors/ubertooth/issues/529)
- **Script-visible change:** every tool now refuses a command line that says
  two things and means one, with its one `error: ` line and before it opens a
  board: an option given a value twice (`-l 446688 -l aabbcc`), two major
  modes (`ubertooth-ego -f -r`), two modulations for one capture
  (`ubertooth-dump -c -l`), two output formats for one sweep
  (`ubertooth-specan -g -G`), and the same channel written two ways
  (`ubertooth-util -c 2440 -C 24`). One of the two used to win and the other
  was dropped silently. `ubertooth-util -c 2426 -c` still sets the channel and
  reads it back. `ubertooth-debug -r` takes the whole register list in one
  value, `-r %main,0x02-0x05`, where a second `-r` used to replace the first
  one's registers without a word. The tools that said it their own way now say
  that same line: `ubertooth-btbr` used to say `give -U once`, and
  `ubertooth-follow` `give -t once`, `give -l once` and `give -u once`.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- **Script-visible change:** `ubertooth-specan -d` now only chooses where the
  samples go, and `-g`/`-G` how they are written, so `-g -d out.dat` writes
  the gnuplot text into the file the man page always promised. `-d` used to
  create the file and leave it empty while a later `-g` took the samples to
  the screen.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- `-h` beside `-V`, `-H` or `--host-version` prints both answers, the usage
  text last, instead of only whichever was written first.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- `ubertooth-btle -i` on its own says `error: -i needs -f or -p`, the message
  its man page promises, rather than listing every major mode.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
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
- **Script-visible change:** every version line now says what it is. The host
  tools version (`ubertooth-util -H`, `-V` in `ubertooth-rx`, `-tx` and `-afh`)
  reads `Host tools version: libubertooth ...`, and the multi-board listing
  keeps each report's label with the board's index in it
  (`Firmware version (0): ...`) instead of a bare `0: ...`. A board that cannot
  be read says why where its value would be
  (`Firmware version (1): unavailable, in use by another program`).
  [#610](https://github.com/SqueakyFloors/ubertooth/issues/610)
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
- `ubertooth-specan -v` now prints what its page says it does, on standard
  error: the board, the sweep, where the samples go, and a summary once a
  second. It used to do nothing. A second `-v` is refused.
  [#510](https://github.com/SqueakyFloors/ubertooth/issues/510)
- `ubertooth-specan` reports a bad option itself, as the other tools do:
  `error: unrecognized option '-Z'` and `error: option '-l' requires a value`,
  instead of the option parser's own lines naming the whole path of the binary.
  [#338](https://github.com/SqueakyFloors/ubertooth/issues/338)
- Every tool now answers a mistyped command line the same way: the last four
  leaving it to the option parser, `ubertooth-debug`, `ubertooth-dfu`,
  `ubertooth-follow` and `ubertooth-scan`, report it themselves.
  `ubertooth-rx` and `ubertooth-dfu` also prefix the refusals they were
  missing one on.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- **Script-visible change:** the rest of `ubertooth-dfu`'s progress moved to
  standard error too: the DFU entry, the flash an image or a read-back covers,
  the dot per block, and `Detached`. Standard output now carries one line, the
  revision `-d <image> -r` verified after the reset, which moved the other way
  (it was on standard error), and every failure the tool reports itself begins
  `error: `.
  [#502](https://github.com/SqueakyFloors/ubertooth/issues/502)
- **Script-visible change:** `ubertooth-dfu` prints its `Checking firmware
  signature` progress line on standard error; it was on standard output. It
  also reports an image it will not write as one `error:` line, where it was
  two, so `Signature check failed, firmware will not be updated` and `Image
  too large for the attached part, firmware will not be updated` are gone.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
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
- `ubertooth-util -w` reports whether the board last came up from a watchdog
  reset, how many it has done since power-on, and which mode and which wait it
  was in when it stopped answering.
  [#509](https://github.com/SqueakyFloors/ubertooth/issues/509)
- `ubertooth-util -w` also reports how many of the firmware's waits on the
  radio gave up, one count per wait. Those waits are bounded so that a radio
  that never reports the state it was asked for cannot hold the board, and
  carrying on is silent: a count above zero is what tells a half-working radio
  from a quiet channel.
  [#488](https://github.com/SqueakyFloors/ubertooth/issues/488)
- `ubertooth-util -I` blinks the LEDs faster for easier
  identification, and now runs the blink on the board like every other mode, so
  the command returns at once and the board stays reachable; `-S` or any mode
  option stops it and leaves the LEDs off.
  [#167](https://github.com/SqueakyFloors/ubertooth/issues/167)
- **Script-visible change:** the `btbr` image reports the firmware API version
  of the tree it was built from, instead of a fixed 1.06 that never moved. A
  board carrying it is no longer refused over its version by every other tool;
  those tools still cannot drive that image and now fail on the first request
  it does not answer.
  [#556](https://github.com/SqueakyFloors/ubertooth/issues/556)
- **Reflash required:** the firmware API version is now 1.42, last moved by the
  CRC check a followed connection's data packets get, and the host tools
  require firmware of exactly their own API version, older or newer. Host and
  firmware come from the same tree: `ubertooth-dfu` reflashes a board whatever
  firmware it is running, but
  only with an image built from this tree, and `2020-12-R1` firmware is
  refused.
  [#4](https://github.com/SqueakyFloors/ubertooth/issues/4)
  [#21](https://github.com/SqueakyFloors/ubertooth/issues/21)
  [#167](https://github.com/SqueakyFloors/ubertooth/issues/167)
  [#174](https://github.com/SqueakyFloors/ubertooth/issues/174)
  [#240](https://github.com/SqueakyFloors/ubertooth/issues/240)
  [#261](https://github.com/SqueakyFloors/ubertooth/issues/261)
  [#433](https://github.com/SqueakyFloors/ubertooth/issues/433)
  [#434](https://github.com/SqueakyFloors/ubertooth/issues/434)
  [#464](https://github.com/SqueakyFloors/ubertooth/issues/464)
  [#473](https://github.com/SqueakyFloors/ubertooth/issues/473)
  [#475](https://github.com/SqueakyFloors/ubertooth/issues/475)
  [#488](https://github.com/SqueakyFloors/ubertooth/issues/488)
  [#508](https://github.com/SqueakyFloors/ubertooth/issues/508)
  [#570](https://github.com/SqueakyFloors/ubertooth/issues/570)
  [#571](https://github.com/SqueakyFloors/ubertooth/issues/571)
  [#505](https://github.com/SqueakyFloors/ubertooth/issues/505)
  [#658](https://github.com/SqueakyFloors/ubertooth/issues/658)
  [#657](https://github.com/SqueakyFloors/ubertooth/issues/657)
  [#660](https://github.com/SqueakyFloors/ubertooth/issues/660)
- Every firmware image now carries a block naming the image and the revision
  it was built from, read the same way off a board and out of a file.
  `ubertooth-dfu -d` says which image a file holds before it writes it
  (`Image: bluetooth_rxtx git-ed30131d* (API:<version>)`), and refuses a file
  that carries no such block before erasing anything, which includes every
  image built before this one. `ubertooth-util -v` and `-V` name the image a
  board is running, and still read a board whose firmware has no block, showing
  its revision alone.
  [#473](https://github.com/SqueakyFloors/ubertooth/issues/473)
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
- `ubertooth-btbr` takes `-U <index|serial>` to choose one of several attached
  boards, as every other tool does, and `-h` to print its usage text.
  [#642](https://github.com/SqueakyFloors/ubertooth/issues/642)
  [#410](https://github.com/SqueakyFloors/ubertooth/issues/410)
- **Script-visible change:** `ubertooth-btbr` exits 0 for `-h`, and reports a
  command line it will not take as one `error: ` line on standard error
  instead of printing its usage text on standard output.
  [#410](https://github.com/SqueakyFloors/ubertooth/issues/410)
- **Script-visible change:** `ubertooth-btbr` reports a warning or a failure as
  one `warning: ` or `error: ` line on standard error, as the other tools do,
  instead of a `HH:MM:SS | WARNING | ...` log line. The packets a command
  reports are still log lines.
  [#410](https://github.com/SqueakyFloors/ubertooth/issues/410)

### Build and compatibility

- The host tools build with a strict C standard (`-std=c17`, `-std=c2x`) on
  Linux, where that used to fail on `CLOCK_REALTIME`, `usleep()` and the
  byte-order helpers: the build asks glibc for the interfaces it uses, instead
  of leaving it to the compiler's default GNU standard.
  [#648](https://github.com/SqueakyFloors/ubertooth/issues/648)
- `install.sh` says it is checking Homebrew, and that it is working out which
  Ubertooth to flash, before those slow steps run; on macOS its first line
  used to be a couple of seconds late, which looked like a hang.
  [#640](https://github.com/SqueakyFloors/ubertooth/issues/640)
- Removed the unmaintained Dockerfile.
  [#324](https://github.com/SqueakyFloors/ubertooth/issues/324)
- Removed `Jenkinsfile` and `ci-scripts/`, which described a Jenkins build
  nobody runs here and switched two USB hubs that are not this project's. CI
  for this fork is the GitHub Actions workflows.
  [#349](https://github.com/SqueakyFloors/ubertooth/issues/349)
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
- The configure step refuses an installed libbtbb that is missing functions the
  host tools call, naming them and the commit to rebuild libbtbb at, instead of
  letting the build reach the compiler and fail with `call to undeclared
  function`.
  [#585](https://github.com/SqueakyFloors/ubertooth/issues/585)
- The host builds again on Ubuntu 20.04, this fork's declared minimum, where
  glibc still keeps the pthread calls out of libc: the libubertooth targets
  link pthread, so linking the library no longer leaves `pthread_create`
  undefined.
  [#315](https://github.com/SqueakyFloors/ubertooth/issues/315)
- The host builds on Ubuntu 20.04 and 22.04 again. One test asks the dynamic
  linker for the real `pthread_create`, which needs a GNU extension the older
  glibc headers hand out only when asked; the tests build by default, so the
  whole host build stopped there.
  [#725](https://github.com/SqueakyFloors/ubertooth/issues/725)
- Each firmware image now compiles the shared sources under `firmware/common`
  into its own directory. Building two images in one tree used to give the
  second one the first's objects, built with the wrong flags, so a release or
  CI build could ship an image nobody had built from clean.
  [#486](https://github.com/SqueakyFloors/ubertooth/issues/486)
- Editing a firmware compile flag, `OPT` or `DISABLE_TX` for example, rebuilds
  the objects. The build records the flags it used, so `make` no longer takes
  objects compiled with the old ones as up to date and links them into the
  image.
  [#544](https://github.com/SqueakyFloors/ubertooth/issues/544)

### Reliability fixes

- `ubertooth-util -r` recovers a board that answers nothing, which used to
  need a re-plug or a hub port power cycle. Such a board refuses the reset
  request like every other request, so `-r` resets its USB connection first --
  the re-plug done in software -- and then resets the board as usual, so it
  comes back idle. Select it by index: its serial cannot be read.
  [#731](https://github.com/SqueakyFloors/ubertooth/issues/731)
- `ubertooth-follow -w` compensates the delay it says it does. The option
  counts 625 us timeslots, and the clock the board hops on counts 312.5 us
  ticks; the delay was added tick for tick, so every value compensated half of
  what it said -- 1562.5 us of the default's 3125 us, leaving the board
  hopping on a clock more than two slots out, which on a hopping piconet is
  the wrong channel.
  [#672](https://github.com/SqueakyFloors/ubertooth/issues/672)
- `ubertooth-btbr` reports a board another program is using as that, in the
  words every other tool uses: `error: Ubertooth <serial> is in use by another
  program`, with the libusb code the system gives. It used to report it as a
  board that took none of the session's requests, with a note about firmware
  that had nothing to do with it.
  [#701](https://github.com/SqueakyFloors/ubertooth/issues/701)
- A spectrum analyzer run measures through the radio front end a freshly
  booted board has, whichever mode ran before it. `ubertooth-specan` and
  `ubertooth-util -q` do not set the radio's manual signal overrides
  themselves, and nothing put them back when a mode ended, so a sweep after a
  low-energy capture was taken through that capture's front end and a sweep
  after a reset was not. The write that was meant to set them at boot is gone
  as well: the radio reset a few microseconds later undid it, so it never
  reached a mode.
  [#471](https://github.com/SqueakyFloors/ubertooth/issues/471)
- `ubertooth-util -i` is reported as the success it is. The board used to go
  off the bus from inside the request handler, throwing away the answer the
  host was waiting for, so a board that had done exactly what was asked was
  reported as one that had timed out (`libusb -7`) and the command exited 1.
  Both images now answer the host first. The command also says, in a note of
  its own, that the board stays in its programmer and answers nothing else
  until its power is cut.
  [#508](https://github.com/SqueakyFloors/ubertooth/issues/508)
- `ubertooth-debug` prints each CC2400 field's whole value. Fourteen fields
  were read with a mask narrower than the field the dump draws beside them, so
  a field holding 64 read as 0, and two more took their value from a
  neighbour's bits: `FSMTC`'s `TC_TXEND2PAOFF` drew the field above it, and
  `FSTST3`'s `PD_DELAY` reported `CHP_DISABLE`. Register `0x15` also answers to
  `%MDMTST1` now; it carried `0x14`'s name, so no name of its own reached it.
  [#671](https://github.com/SqueakyFloors/ubertooth/issues/671)
- `ubertooth-btle -f` keeps up with a connection parameter update. The board
  kept the new transmit window in a field too narrow for it, so every window
  was cut to a fraction of its length -- 2.5 ms came out as 16.8 microseconds
  -- and the first connection event under the new parameters could end before a
  packet late in the window arrived.
  [#571](https://github.com/SqueakyFloors/ubertooth/issues/571)
- `ubertooth-btle -f -v1` keeps the data packets of a followed connection. The
  board reversed the connection's CRC starting value over a whole 32-bit word
  where the field is 24 bits wide, so every packet of the connection was
  checked against the wrong value, failed, and was dropped: the capture was
  empty for as long as the connection lasted. Advertising packets were
  unaffected.
  [#657](https://github.com/SqueakyFloors/ubertooth/issues/657)
- `ubertooth-btle -f` keeps following when a channel map update asks for no
  channels at all. Such an update -- which only a corrupted packet can carry --
  used to park the capture on one data channel, where it saw nothing until the
  connection's supervision timeout ran out. It is now refused and the
  connection carries on with the map it has.
  [#660](https://github.com/SqueakyFloors/ubertooth/issues/660)
- The average RSSI of a captured Bluetooth low-energy packet is now the mean
  of the samples taken while it arrived. The board computed it in unsigned
  arithmetic, so the value in the per-packet header of a PCAP written by
  `ubertooth-btle -c` was unrelated to the packet; the minimum and the maximum
  beside it were always right.
  [#570](https://github.com/SqueakyFloors/ubertooth/issues/570)
- `ubertooth-util -w` names three more places a board can stop answering in:
  the low-energy engine's two interrupt handlers and its symbol buffer. A
  reset caught in one of those handlers used to be reported as the mode's loop
  servicing USB, which is also what a reset caught in the loop itself reports.
  [#505](https://github.com/SqueakyFloors/ubertooth/issues/505)
- A Classic capture's noise figure and signal-to-noise ratio are what the radio
  heard, instead of a fixed -55 dBm in every record. The per-channel average
  behind them was cleared once per packet, so it never built up. A positive
  `ubertooth-util -z` finally means what it says, N dB above the measured noise
  floor: it was computed from an average pinned near the top of the radio's
  range, which made it a near-total squelch, and it now passes most of what the
  board hears.
  [#658](https://github.com/SqueakyFloors/ubertooth/issues/658)
- `ubertooth-btle -p` no longer latches a wrong connection interval and reports
  it for the rest of the run when a packet arrives just after the capture clock
  wraps.
  [#659](https://github.com/SqueakyFloors/ubertooth/issues/659)
- `ubertooth-follow -l <LAP> -u <UAP>` says what to do when its connection to
  the target fails: those four bytes leave the connection to be made to a
  guessed `00:00:<UAP>:<LAP>`, so the failure now asks for the whole address
  with `-t`.
  [#540](https://github.com/SqueakyFloors/ubertooth/issues/540)
- `ubertooth-specan-ui --wifi 14` places the channel on 2484 MHz, where it is,
  rather than 2482, so the band it sweeps for that channel, and where a signal
  in it lands on the plot, are no longer 2 MHz low.
  [#673](https://github.com/SqueakyFloors/ubertooth/issues/673)
- A selection that finds nothing says so when a board is in the DFU
  bootloader, which is what a board being flashed looks like. "no Ubertooth
  found" read the same as a board that had been unplugged.
  [#580](https://github.com/SqueakyFloors/ubertooth/issues/580)
- `ubertooth-btbr` receives a long Classic payload whole. Past the first 256
  bytes of the air buffer it decoded from 256 bytes earlier instead, so a DH5
  payload of 247 bytes or more, the largest a Classic packet carries, always
  failed its checksum.
  [#667](https://github.com/SqueakyFloors/ubertooth/issues/667)
- `ubertooth-btbr` no longer corrupts every Classic transmit of 77 bytes or
  more. The 2/3 FEC encoder ran half again as many groups as the payload had,
  writing the surplus over the air buffer, so everything after the first
  chunk went out corrupt. Some payload sizes are still corrupted by an older,
  narrower fault in the same encoder's last group.
  [#668](https://github.com/SqueakyFloors/ubertooth/issues/668)
  [#706](https://github.com/SqueakyFloors/ubertooth/issues/706)
- Page scan, inquiry scan and the paging monitor watch for a packet through
  nearly the whole 312.5-microsecond slot, as their own comment says, rather
  than the first 25 microseconds of it. The window was set in microseconds
  where the slot clock counts 100-nanosecond steps, so it closed before the
  moment a packet is expected and `ubertooth-btbr` missed most page and
  inquiry scan responses.
  [#669](https://github.com/SqueakyFloors/ubertooth/issues/669)
- Replaying a dump puts a low-energy packet that arrived as several records
  where the live capture of the same run put it, rather than up to a second
  later.
  [#643](https://github.com/SqueakyFloors/ubertooth/issues/643)
- The note about firmware the host tools were not built with names the board
  the message is about, instead of offering to reflash every board attached.
  [#652](https://github.com/SqueakyFloors/ubertooth/issues/652)
- `ubertooth-btbr` no longer starts a session on an arbitrary board. With
  several attached it used to open whichever one enumerated first, so a
  session could reach a board still carrying `bluetooth_rxtx` and report its
  write timeout as a failure of the board the user had just flashed with
  `btbr`. It now lists the attached boards and opens none until `-U` names
  one, and a failed setup names the board it was talking to.
  [#642](https://github.com/SqueakyFloors/ubertooth/issues/642)
- `ubertooth-util -I` says what every other tool says about firmware it was not
  built with, so it no longer tells you to flash older firmware over a board
  that is the newer of the two.
  [#635](https://github.com/SqueakyFloors/ubertooth/issues/635)
- `ubertooth-dfu -d <image> -r` says which of the post-reset outcomes it hit
  instead of one numeric line for all of them, so a board that took the whole
  image and then did not come back on USB is no longer reported as a failed
  flash and flashed again for nothing.
  [#579](https://github.com/SqueakyFloors/ubertooth/issues/579)
- **Script-visible change:** `ubertooth-dfu -r` no longer reports a board that
  reset straight back into the bootloader as detached: the DFU device has to
  be gone on two checks in a row, the second 1.5 s later, and a board that is
  back exits 1 saying why. Flashing with the expansion-header bridge still
  fitted now exits 1 where it could exit 0, and every reset that works spends
  about 1.5 s more.
  [#304](https://github.com/SqueakyFloors/ubertooth/issues/304)
- `ubertooth-dfu -d <image> -r` also checks which image came back, not only
  its revision, so a write of `btbr` over `bluetooth_rxtx` that did not take
  no longer verifies clean: both images of one tree carry the same revision.
  An image whose firmware info block reports a firmware API version other than
  these tools' own is written with a warning first.
  [#557](https://github.com/SqueakyFloors/ubertooth/issues/557)
- `ubertooth-btle -q -` and `-c -` no longer fill the terminal with the
  decoder's complaints about packets they do not print. A capture on standard
  output leaves no room for the per-packet dump, so that dump is not produced
  at all, and the complaints about fields in it go with it.
  [#602](https://github.com/SqueakyFloors/ubertooth/issues/602)
- `ubertooth-btle -f` and `-n` capture a Bluetooth low-energy packet longer
  than 50 bytes whole. The board used to cut every one of them to 50 bytes and
  the host then showed the first 46 bytes of the packet, so a Bluetooth 4.2
  Data Length Extension payload came out short on screen and in PCAP and
  PCAPNG files, with nothing to say it had been cut. A long packet now travels
  in several records and is put back together before it is decoded, and the
  paired libbtbb reads the payload length from all eight bits of the field, so
  a payload up to its 251-byte maximum arrives as it was sent. The promiscuous
  mode, `-p`, captures a long packet whole as well; its own capture path used
  to cut one at 50 bytes and report the payload as shorter than it was, or
  drop the packet for its length.
  [#4](https://github.com/SqueakyFloors/ubertooth/issues/4)
  [#546](https://github.com/SqueakyFloors/ubertooth/issues/546)
- `ubertooth-follow` stops when the board refuses the target's address or the
  clock reset, and says which of the two it was. It used to print a generic
  line that named neither, and follow anyway: without the address the board
  matched no packet, and without the reset it hopped on a clock the offset was
  not measured from.
  [#504](https://github.com/SqueakyFloors/ubertooth/issues/504)
- A board that stops answering the host usually resets itself within about
  three seconds and comes back idle, instead of staying on USB answering
  nothing until it is unplugged or its hub port is power-cycled. A capture sees
  the board disappear rather than hang. A board that hits one of the LED fault
  codes now reboots part-way through blinking it for the same reason, and
  `ubertooth-util -w` says which of the four faults it was, which counting
  blinks never gave you. That holds for a board running `btbr` too, where a
  fault used to read back as whatever the firmware was doing before it, "not in
  a wait" or "servicing USB"; a board that image stopped itself reads back as
  that.
  [#239](https://github.com/SqueakyFloors/ubertooth/issues/239)
  [#458](https://github.com/SqueakyFloors/ubertooth/issues/458)
  [#509](https://github.com/SqueakyFloors/ubertooth/issues/509)
  [#608](https://github.com/SqueakyFloors/ubertooth/issues/608)
- The `libusb -7` note names the fallback -- unplug the board or power-cycle its
  hub port -- as well as the self-reset, which a board whose loop keeps feeding
  the watchdog never gets.
  [#609](https://github.com/SqueakyFloors/ubertooth/issues/609)
- A board another program is using says so (`error: Ubertooth <serial> is in
  use by another program (libusb -3)`) instead of reporting a libusb code, and
  no longer has a stop sent to it on the way out; a board that answers nothing
  says that plainly too. See ubertooth(7), WHEN A BOARD CANNOT BE USED.
  [#232](https://github.com/SqueakyFloors/ubertooth/issues/232)
- A board no longer carries one run's hopping into the next. Starting a mode
  resets the hop mode and forgets the AFH map, so an `ubertooth-rx` after an
  `ubertooth-afh` -- including one that was killed and left the board streaming
  -- captures on the whole channel set instead of hopping adaptively on the old
  map. A channel set on its own, with `ubertooth-util -c`/`-C` and no mode
  running, is still kept, and the next mode starts on it unless that tool sets
  a channel of its own.
  [#261](https://github.com/SqueakyFloors/ubertooth/issues/261)
  [#312](https://github.com/SqueakyFloors/ubertooth/issues/312)
- **Script-visible change:** a capture that cannot start exits 1 in
  `ubertooth-rx`, `ubertooth-scan` and `ubertooth-follow`, instead of the
  negative libusb code a shell wrapped into 247 or 249; and both of
  `ubertooth-scan`'s BlueZ adapter failures go to standard error, one having
  gone to standard output. A refused capture request names itself as one
  `error: ` line, like the other requests that set a capture up.
  [#431](https://github.com/SqueakyFloors/ubertooth/issues/431)
  [#545](https://github.com/SqueakyFloors/ubertooth/issues/545)
- `ubertooth-rx` and `ubertooth-scan` sweep the whole band on a board that a
  killed `ubertooth-afh` or `ubertooth-follow` left capturing. They ask for the
  capture first and state the sweep after it, where the mode start used to
  forget a sweep stated before it: the capture sat on one channel with nothing
  said, and the AFH map `ubertooth-scan` printed meant nothing.
  [#490](https://github.com/SqueakyFloors/ubertooth/issues/490)
- `ubertooth-follow` hops with the piconet when it starts on a board that was
  still in another mode, such as a spectrum scan that was never stopped. It
  used to sit on one channel and follow nothing. The same went for a transmit
  started that way.
  [#261](https://github.com/SqueakyFloors/ubertooth/issues/261)
- `ubertooth-follow` timestamps are no longer hours out. It zeroed the board's
  clock after the capture had started, so a packet caught around the reset
  added about 34,700 s to every later packet time.
  [#494](https://github.com/SqueakyFloors/ubertooth/issues/494)
- A board running `btbr` can be reflashed with `-U <serial>`. It used to answer
  `ubertooth-dfu` with a stalled transfer and exit 1, because the image reset
  before the request asking it to enter DFU had been answered; it did enter the
  bootloader, which carries no serial number, so no `-U` could reach it again.
  [#475](https://github.com/SqueakyFloors/ubertooth/issues/475)
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
- A capture that goes quiet no longer writes junk records. The keep-alive the
  firmware sends when it has nothing to capture used to be published as a
  packet, so `ubertooth-dump -d` saved a record whose contents were never set,
  `ubertooth-btle` put the same record in its `-d` capture and a packet decoded
  from it in its PCAP output, and `ubertooth-specan -d` wrote samples read out
  of it. Replaying a capture file recorded before this skips such records.
  [#435](https://github.com/SqueakyFloors/ubertooth/issues/435)
- A streaming capture that loses packets to a truncated USB transfer says so
  once, on standard error, instead of dropping them in silence and looking like
  a quiet board. The keep-alive is the only short transfer a board sends on
  purpose.
  [#448](https://github.com/SqueakyFloors/ubertooth/issues/448)
- The two-board range test measures something at last: `ubertooth-util -n`
  against a `-e` repeater reports the power levels within a couple of seconds,
  in either direction, where it used to report nothing at all. Neither mode
  held the radio's receive gain amplifier out of reset, so both receivers reported
  a strong signal and never a sync word; the repeater answered before the
  sender had finished transmitting, because its delay was a loop the compiler
  deleted; it decoded requests only while the radio still held its reset
  value; and neither mode reached the radio's packet buffer in a way the radio
  accepts, so the packets carried no payload and the replies read back as
  zeros.
  [#26](https://github.com/SqueakyFloors/ubertooth/issues/26)
  [#483](https://github.com/SqueakyFloors/ubertooth/issues/483)
- An interrupt handler can no longer cut into a CC2400 register transfer and
  corrupt the register being read or written, or cost the radio a strobe and
  leave the board quiet. The longer FIFO transfers are still unguarded.
  [#175](https://github.com/SqueakyFloors/ubertooth/issues/175)
  [#429](https://github.com/SqueakyFloors/ubertooth/issues/429)
- `ubertooth-btbr` no longer leaves the board unreachable when the radio does
  not reach the state a slot is waiting for. The BR/EDR firmware waited on the
  radio's state machine forever, from the timer interrupt, so the board stopped
  answering anything until it was re-plugged.
  [#174](https://github.com/SqueakyFloors/ubertooth/issues/174)
- `ubertooth-btbr` no longer leaves the board unreachable when a packet stops
  part-way through. The BR/EDR firmware waited forever for the rest of the
  bytes, from the timer interrupt; it now gives up when they are overdue and
  reports the packet as received but undecodable, as it already did for a
  header it could not decode.
  [#495](https://github.com/SqueakyFloors/ubertooth/issues/495)
- **Script-visible change:** a capture whose USB stream fails stops there and
  exits 1, instead of waiting out its `-t` timer with nothing arriving and
  reporting success. `ubertooth-rx`, `ubertooth-dump`, `ubertooth-specan`,
  `ubertooth-scan`, `ubertooth-follow` and `ubertooth-afh` are affected.
  [#319](https://github.com/SqueakyFloors/ubertooth/issues/319)
- **Script-visible change:** a capture that never gets its receive transfer
  back exits 1 instead of 0, and the board is no longer sent a stop it may not
  be there to hear. The warning already says it may still be capturing;
  `ubertooth-util -S` stops one that is.
  [#454](https://github.com/SqueakyFloors/ubertooth/issues/454)
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
- `ubertooth-afh` stops when the board refuses the band sweep its map is
  watched through, instead of printing a map of the one channel the board was
  left on. With `-r` it now exits 1 when the receive could not be started at
  all, where it exited 0. `ubertooth-btle` names which of the requests that set
  a mode up the board refused, rather than only the mode, and a refused
  `-s` advertise request stops the board rather than possibly leaving it
  advertising, which is the state it stops answering in.
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
- `ubertooth-btbr` can be interrupted: Ctrl-C ends the running command and
  returns to the `bt>` prompt, `q` exits, and a board that will not answer is
  reported instead of hanging or ending in a traceback. The hang shortly after
  start was a board carrying `bluetooth_rxtx`, which never reads the endpoint
  this tool writes to; the tool now names that firmware and the image to flash.
  [#1](https://github.com/SqueakyFloors/ubertooth/issues/1)
  [#418](https://github.com/SqueakyFloors/ubertooth/issues/418)
- A Ctrl-C in the instant an `ubertooth-btbr` command starts returns to the
  prompt with the board free, instead of ending the tool in a traceback with
  the board still claimed.
  [#552](https://github.com/SqueakyFloors/ubertooth/issues/552)
- **Script-visible change:** `ubertooth-btbr` exits 1 when a session ends with
  the board still held by a command that would not stop, refuses a `-f` or
  `-b` value outside 0-65535 before opening a board, and reports a bad
  `bdaddr` and a malformed EIR on standard error rather than standard output.
  [#1](https://github.com/SqueakyFloors/ubertooth/issues/1)
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
- **Script-visible change:** `btle-extcap.py --extcap-interfaces` exits 1 when
  a board could not be opened, even if it named others; it used to exit 0 so
  that Wireshark kept the boards it could capture on. Wireshark discards a
  failed call's output, so one board another capture is already holding now
  leaves its capture list without the Ubertooths that were fine, until that
  board is freed. The reason is on standard error as before.
  [#386](https://github.com/SqueakyFloors/ubertooth/issues/386)
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
- `ubertooth-btle -n` and `-f` report the first two packets of a session. The
  board used to hand them to a receive endpoint these modes never read.
  [#434](https://github.com/SqueakyFloors/ubertooth/issues/434)
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
  board is reported as detached only once its DFU device has left the bus and
  stayed off it, whatever the request returned.
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
- The `btbr` image now carries the build string `ubertooth-dfu -r` reads the
  expected revision from, so flashing it no longer stops with "Unable to
  determine expected firmware revision from image" and then needs a power
  cycle; `ubertooth-util -V` reports that string from a board running `btbr`
  too. Flashing back off `btbr` still takes a second `ubertooth-dfu` run
  without `-U`, because a board in the bootloader reports no serial
  ([#475](https://github.com/SqueakyFloors/ubertooth/issues/475)).
  [#433](https://github.com/SqueakyFloors/ubertooth/issues/433)
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
- A capture whose first record reads ahead of the records after it is no longer
  anchored 327.68 s late. Between a capture's first two records, where a record
  left over from the mode before looks just like the capture clock starting
  over, the host's own clock now settles which it was, so a file that begins
  with such a record replays with its own times.
  [#465](https://github.com/SqueakyFloors/ubertooth/issues/465)
- A leftover first record no longer puts hours into every timestamp of a
  `ubertooth-rx` or `ubertooth-follow` capture. Such a record also carries the
  period count of the mode before, and the drop back to the capture's own count
  was read as time passing: 18,321 s, over five hours, where the mode before
  had run 200 periods.
  [#478](https://github.com/SqueakyFloors/ubertooth/issues/478)
- A live capture whose host fell behind no longer drops the gap between its
  first two packets from every later timestamp, and no longer loses a clock
  wrap there. How much time passed between two packets is now read as each one
  arrives rather than as the host gets round to it.
  [#492](https://github.com/SqueakyFloors/ubertooth/issues/492)
- A live capture whose host was slow to start now carries the wall-clock times
  its packets arrived at. Every timestamp used to be late by however long the
  host took to reach its first packet, seconds where the capture was written to
  a pipe nothing was reading yet or taken by a descheduled process.
  [#589](https://github.com/SqueakyFloors/ubertooth/issues/589)
- The `systime` of each packet line, and the host time a `-d` dump stores with
  each record, now come from the reading the record arrived with, the same one
  a capture's timestamps are anchored on. A stalled host's dump used to carry
  every time late, replay that late, and disagree with the capture written
  beside it in the same run.
  [#612](https://github.com/SqueakyFloors/ubertooth/issues/612)
- `ubertooth-afh -t` stops on the first Ctrl-C. An interrupt during the
  detection phase was dropped along with the timeout that normally ends it, so
  monitoring started anyway and had to be interrupted a second time.
  [#521](https://github.com/SqueakyFloors/ubertooth/issues/521)
- Closing the terminal on `ubertooth-btle`, `ubertooth-ego` or
  `ubertooth-ducky` takes the board out of its mode, as it already did for the
  other tools; the board used to be left capturing or advertising.
  [#515](https://github.com/SqueakyFloors/ubertooth/issues/515)

### Documentation

- `ubertooth-follow`'s man page describes what a board that refuses one of the
  follow's requests prints, including the note that says what to try, as
  ubertooth-rx(1) and ubertooth-scan(1) already did.
  [#617](https://github.com/SqueakyFloors/ubertooth/issues/617)
- `ubertooth-ego -h` and its man page no longer call `630f9ffe` the default
  access code. A run without `-a` sends none, so it uses whichever one the
  board was last given; `630f9ffe` is only the firmware's value after a reset.
  [#397](https://github.com/SqueakyFloors/ubertooth/issues/397)
- `ubertooth-ego -h` and its man page now say that `-r` without `-c` receives
  on 2418 MHz.
  [#413](https://github.com/SqueakyFloors/ubertooth/issues/413)
- `ubertooth-btle -h` no longer says what happens when no input file is given.
  The tool has no option that reads a capture back, so there was nothing to
  choose: it always captures from a board.
  [#534](https://github.com/SqueakyFloors/ubertooth/issues/534)
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
- `ubertooth-btbr` has a man page, a row in the README tool catalog, and a
  note saying which firmware image each tool needs and how to switch a board
  between `bluetooth_rxtx` and `btbr`.
  [#530](https://github.com/SqueakyFloors/ubertooth/issues/530)

### Under the hood

Fixes with no visible change in normal use.

- `ubertooth-dfu -d <image> -r` tells three things apart after the reset: a
  board that did not come back, a board that is back and will not open, and a
  host USB bus that could not be read at all. The last two both read as a
  board that would not open, which named a fault the board may not have had.
  [#676](https://github.com/SqueakyFloors/ubertooth/issues/676)
- A packet captured by `ubertooth-btle -p` is timestamped a fixed moment after
  the board synchronized on it, rather than somewhere in the middle of the
  packet, so its time no longer drifts later the longer the packet is. Times
  in a `-p` capture move earlier by up to about a third of a millisecond.
  [#546](https://github.com/SqueakyFloors/ubertooth/issues/546)
- A capture's ring buffer hands records between its two threads properly: a
  record and the time it arrived are both complete before the capture can see
  either, and a slot goes back to the board only once the capture has copied
  the record out of it. A capture far enough behind to fill the ring could
  read one record while the next arrival wrote over it.
  [#576](https://github.com/SqueakyFloors/ubertooth/issues/576)
- Editing a firmware linker script relinks the image. Nothing told `make` that
  the link depends on the script, so a change to a memory map or a section
  layout used to leave the previous image in place until `make clean`.
  [#568](https://github.com/SqueakyFloors/ubertooth/issues/568)
- A `ubertooth-btle -t` capture no longer matches an advertising packet too
  short to hold an address against the target. Such a packet used to be kept
  or dropped on three payload bytes and three bytes of CRC read as if they
  were an address.
  [#593](https://github.com/SqueakyFloors/ubertooth/issues/593)
- The low-energy engine's three drains of its symbol buffer are bounded. All
  three run in one of its interrupt handlers, and the loop that services USB
  runs beneath both of those, so a drain that never saw the buffer empty
  stopped the board answering anything.
  [#505](https://github.com/SqueakyFloors/ubertooth/issues/505)
- The low-energy engine's wait for the synthesizer to lock no longer holds most
  of the processor while the synthesizer is silent: it backs off once the lock
  is overdue, instead of looking again every three microseconds for as long as
  it takes.
  [#505](https://github.com/SqueakyFloors/ubertooth/issues/505)
- A control reply the USB controller would not take is offered again on the
  next service pass, instead of being dropped with nothing left to carry that
  transfer on.
  [#505](https://github.com/SqueakyFloors/ubertooth/issues/505)
- `ubertooth-follow` closes the local Bluetooth adapter it opens, on every path
  out, and once the clock reads are done rather than at the end of the run. It
  used to hold a handle on an adapter it had finished with for the whole
  capture, and never close it at all.
  [#506](https://github.com/SqueakyFloors/ubertooth/issues/506)
- `ubertooth-follow` gives the board back when it refuses the firmware version
  or the decoder settings, instead of leaving it claimed until the process
  exits.
  [#577](https://github.com/SqueakyFloors/ubertooth/issues/577)
  [#586](https://github.com/SqueakyFloors/ubertooth/issues/586)
- A capture whose collecting thread could not be started says so and ends,
  rather than waiting forever for packets nobody was there to deliver. The
  failure is reported as a positive number and was tested for a negative one,
  in every tool that starts a capture, in libubertooth, which is every
  `ubertooth-dump` and `ubertooth-afh` capture, and in the Kismet plugins,
  which did not look at it at all. A teardown that follows such a failure no
  longer waits on a thread that was never created.
  [#431](https://github.com/SqueakyFloors/ubertooth/issues/431)
  [#564](https://github.com/SqueakyFloors/ubertooth/issues/564)
- An `ubertooth-btbr` command the interpreter will not give a thread to says
  so on standard error and leaves the prompt up with the board free. The
  board was left claimed by a command nothing held, so every later command
  was refused as still running and the tool ended in a traceback.
  [#567](https://github.com/SqueakyFloors/ubertooth/issues/567)
- Every packet the firmware writes into the radio's 32-byte packet buffer used
  to carry one stray byte on the end, from eight clock pulses sent after the
  data. A receiver ignores the tail, so the modes that transmit with the
  radio's checksum turned off still got their data across; the range test,
  which has it on, could not work at all. The BLE transmit path fills the
  buffer in chunks, and there the stray byte also cost it one byte of room per
  chunk.
  [#483](https://github.com/SqueakyFloors/ubertooth/issues/483)
- A revision string too long for the one-byte length the firmware reports it
  with is cut to 255 characters, rather than counted modulo 256. The revisions
  this tree builds are twelve characters, so nothing reached that.
  [#464](https://github.com/SqueakyFloors/ubertooth/issues/464)
- Ctrl-C, SIGTERM, SIGHUP, SIGQUIT and the `-t` alarm now stop a capture
  through a flag a signal handler is allowed to set, in the tools that use
  libubertooth's handlers. The handler used to reach into the capture handle,
  which a handler may not do and which left no guarantee that the receive loop
  would see the stop.
  [#499](https://github.com/SqueakyFloors/ubertooth/issues/499)
- `ubertooth-btle`, `ubertooth-ego` and `ubertooth-ducky` stop through that
  same flag, instead of each keeping one of its own that a signal handler was
  not allowed to write. A replay of a saved capture can be stopped as well,
  which no tool asks for today.
  [#515](https://github.com/SqueakyFloors/ubertooth/issues/515)
- A BLE packet whose length field claims more than the board captured reads
  zeros past the capture, instead of whatever followed it in memory; the
  Kismet plugins' access code search stays inside its bank too.
  [#416](https://github.com/SqueakyFloors/ubertooth/issues/416)
- The firmware refuses a generic transmit request whose payload does not fit
  the packet, and no longer drops the last byte of one that does. No shipped
  tool sends it.
  [#384](https://github.com/SqueakyFloors/ubertooth/issues/384)
- The firmware refuses a multi-register write asking for more registers than
  the request can carry. Such a count used to be walked in a counter too
  narrow to reach it, so the loop never ended and wrote radio registers until
  the board was power-cycled. No shipped tool sends it.
  [#409](https://github.com/SqueakyFloors/ubertooth/issues/409)
- The reply to a read of every radio register claims all the bytes it carries,
  so the last register no longer goes missing from it. No shipped tool sends
  that request.
  [#450](https://github.com/SqueakyFloors/ubertooth/issues/450)
- A device status event the USB controller would not report is retried once
  per event, so a transient failure cannot lose a bus reset and leave the board
  answering on an address the host has taken away.
  [#359](https://github.com/SqueakyFloors/ubertooth/issues/359)
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
  power-cycled, and refuses a block of an image there rather than asking the
  part to program memory it has not got. It cannot undo the blocks already
  written, so an oversize image still leaves an application missing its tail;
  what it no longer does is write past the end of flash. A part its table
  cannot identify is bounded by the sectors it reports having, or by the old
  fixed bound if it reports nothing at all, and a block the part failed to
  program is reported rather than answered as written. It is reached only
  through ISP or JTAG, not `ubertooth-dfu`.
  [#256](https://github.com/SqueakyFloors/ubertooth/issues/256)
  [#423](https://github.com/SqueakyFloors/ubertooth/issues/423)
  [#424](https://github.com/SqueakyFloors/ubertooth/issues/424)
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
- Every tool gives back everything it allocates before it exits, and the ones
  that capture no longer take a 72 MB buffer before they have read the command
  line. Releasing a board twice is harmless now, so a tool no longer has to
  work out whether the library has already done it.
  [#366](https://github.com/SqueakyFloors/ubertooth/issues/366)
  [#381](https://github.com/SqueakyFloors/ubertooth/issues/381)
  [#387](https://github.com/SqueakyFloors/ubertooth/issues/387)
  [#388](https://github.com/SqueakyFloors/ubertooth/issues/388)
  [#399](https://github.com/SqueakyFloors/ubertooth/issues/399)
  [#407](https://github.com/SqueakyFloors/ubertooth/issues/407)
- The receive transfer the poll thread and the receive loop share is read and
  written under a lock.
  [#385](https://github.com/SqueakyFloors/ubertooth/issues/385)
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
