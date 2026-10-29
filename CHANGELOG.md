# Changelog

## 2020-12-R1+modern1

### New features

- **Script-visible change:** `ubertooth-btle -s` and `-t` each have their own
  address, so `-s A -t B` advertises from A and follows B, which is what it
  reads like. They shared one address, so whichever option was written last
  set both, and `-s <address> -t none` advertised from `00:00:00:00:00:00`
  without a word.
  [#383](https://github.com/SqueakyFloors/ubertooth/issues/383)
- **Script-visible change:** `ubertooth-btle -s` takes one whole address and
  nothing else. The `/<bits>` mask and `none` that `-t` takes are refused with
  `error: faux slave address must be xx:xx:xx:xx:xx:xx (got none)` and exit 1,
  where they used to be parsed, dropped and the command run anyway: a faux
  slave advertises from a single address, so there is nothing for a mask to
  narrow or for `none` to clear. A script that gave either one now gets a
  failure instead of exit 0.
  [#779](https://github.com/SqueakyFloors/ubertooth/issues/779)
- **Script-visible change:** `ubertooth-ego -l` with interference (`-i`, `-j`)
  is refused, naming the mode as it was spelled: only the modes that read a
  packet read a capture length, so it used to set a firmware value the mode
  never looks at and said nothing.
  [#411](https://github.com/SqueakyFloors/ubertooth/issues/411)
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
- **Script-visible change:** a lone `-` is standard output for every capture
  option that can stream, so `ubertooth-rx -q -` and `-d -`,
  `ubertooth-dump -d -` and `ubertooth-follow -q -` and `-d -` pipe the
  capture to another program, as `ubertooth-btle -q -` already did. Each used
  to create a file called `-` in the working directory instead. The capture
  owns standard output for the whole run, so the per-packet report is not
  printed, and `-q -` with `-d -` is refused. A PcapNG capture cannot be
  piped, because its writer seeks back over what it has written, so
  `ubertooth-rx -r -` and `ubertooth-follow -r -` are refused with
  `error: PCAPNG capture to stdout is not supported; use -q -` instead of
  writing a file called `-` and capturing into it. `ubertooth-dfu -u -` is
  refused for the same reason, since the read-back is written through a
  temporary file and rewound if the read has to be retried.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- **Script-visible change:** `ubertooth-util -U <index|serial> -N` and
  `-U all -N` are refused: `-N` counts the boards on the USB bus and asks
  nothing of the board a selector names. Both used to print the count.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- `ubertooth-rx` and `ubertooth-follow` name a capture file they cannot create
  in one `error: ` line: `error: /no/such/dir/capture.pcapng: No such file or
  directory`, where they used to print
  `ubertooth-rx: create_bredr_capture_file: : File exists`, naming an internal
  function and no file. A `-q` file's line carries no reason, because the PCAP
  writer prints its own and leaves none that can be trusted.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- **Script-visible change:** `ubertooth-dump` refuses a terminal on standard
  output, with or without `-d -`, instead of filling it with the binary symbol
  stream: `error: stdout is a tty, refusing to write binary dump data to it`.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- `btle-extcap.py` reports an option it does not know and an option whose
  value is missing in the wording every other tool uses:
  `error: unrecognized option '--bogus'` and
  `error: option '--fifo' requires a value`, instead of passing on Python's
  `option --bogus not recognized`.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- `ubertooth-btle` refuses two major modes with `error: choose one of -f, -n
  or -p`; the line no longer addresses the reader.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- `-h` beside `-V` prints both answers, the usage text last, instead of only
  whichever was written first.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- `ubertooth-btle -i` on its own says `error: -i needs -f or -p`, the message
  its man page promises, rather than listing every major mode.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- `ubertooth-util -w` on a board that reset itself says which part of its USB
  work it stopped in: handing a capture packet over, reading a request,
  running one, writing the reply, or one of the handshakes with the USB
  controller. It used to report all of those as "servicing USB".
  [#239](https://github.com/SqueakyFloors/ubertooth/issues/239)
  [#505](https://github.com/SqueakyFloors/ubertooth/issues/505)
- **Script-visible change:** `ubertooth-btle -A` is refused alongside `-p` and
  exits 1. Promiscuous mode listens on the channel the board is holding and
  sets none of its own, so the advertising channel index was taken and then
  never sent.
  [#237](https://github.com/SqueakyFloors/ubertooth/issues/237)
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
  tools version (`-V`) reads `Host tools version: libubertooth ...`, and the
  multi-board listing keeps each report's label with the board's index in it
  (`Firmware version (0): ...`) instead of a bare `0: ...`. A board that
  cannot be read says why where its value would be
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
- **Script-visible change:** a `ubertooth-scan` run collected in a file or
  piped into another program no longer picks up its three headings, the
  progress of `-x` or its failures: those are on standard error now. An
  inquiry the adapter refused, or a device the inquiry found that would not
  complete the report `-x` asks for, is named and exits 1; the inquiry used to
  print the name of a BlueZ function and exit 0, and three of the report's
  failures ended the whole run with the board still open.
  [#323](https://github.com/SqueakyFloors/ubertooth/issues/323)
- **Script-visible change:** the lines every tool prints while choosing a
  board now read like the rest. With none attached they say `error: no
  Ubertooth devices found`, in place of `could not open Ubertooth device`;
  with several attached and no `-U`, `error: multiple Ubertooth devices
  found; select one with -U <index|serial>` and then the boards, in place of
  `multiple Ubertooth devices found! Use '-U' to specify device number`,
  which did not say that `-U` takes a serial as well. Each board on that list
  is now `  Device <n>: <serial>`, without the `Serial No: ` the old line
  carried, and a board whose serial could not be read is listed as
  `  Device <n>: serial unreadable` instead of being left out. A refused index
  or serial no longer gets a second line after it that named no cause of its
  own, and a serial that matches nothing says `error: no Ubertooth found with
  serial matching '<string>'`. A libusb failure that cannot be put in words
  reads `error: Input/Output Error (libusb -1)`, in place of `libUSB Error:
  Input/Output Error:  (-1)`.
  [#323](https://github.com/SqueakyFloors/ubertooth/issues/323)
- A bus the host could not list is reported as that, rather than as a bench
  with no board on it: the tools say `error: could not list USB devices`, and
  `ubertooth-util -N` answers non-zero instead of counting 0.
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
  it needs when none was given. A `-U` selector with nothing but `-N` beside
  it is refused, because `-N` counts the boards on the bus and asks nothing of
  the one it names.
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
- **Reflash required:** the firmware API version is now 1.53, last moved by
  promiscuous mode reporting the access address of the connection it follows,
  before that by the
  board reporting a low-energy packet it had to drop for want of queue room and
  by a channel set reaching the board while it follows a connection, before
  that by the
  E-GO mode request starting the mode it names on a board already running one,
  by the AFH map a mode start keeps, the bound on the clock drift
  the board accumulates and the generic receive's FIFO flush, by
  the channel a mode start keeps and the E-GO access code
  it resets, by the finer breadcrumb sites inside USB service that
  `ubertooth-util -w` reads, by the breadcrumb gaining two more places a board
  can stop, by two requests refusing a value they used to accept, and by a
  record naming no channel when the board is tuned outside the band. A release
  build of the host
  tools refuses firmware of any other API version, older or newer, while a
  development build warns about it. Host and firmware come from the same tree:
  `ubertooth-dfu` reflashes a board whatever firmware it is running, but only
  with an image built from this tree, and `2020-12-R1` firmware is refused.
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
  [#395](https://github.com/SqueakyFloors/ubertooth/issues/395)
  [#429](https://github.com/SqueakyFloors/ubertooth/issues/429)
  [#686](https://github.com/SqueakyFloors/ubertooth/issues/686)
  [#757](https://github.com/SqueakyFloors/ubertooth/issues/757)
  [#440](https://github.com/SqueakyFloors/ubertooth/issues/440)
  [#547](https://github.com/SqueakyFloors/ubertooth/issues/547)
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
  instead of a `HH:MM:SS | WARNING | ...` log line.
  [#410](https://github.com/SqueakyFloors/ubertooth/issues/410)
- The host tools version (`-V`) says which libbtbb the run loaded, on a second
  line: `libbtbb loaded from: /usr/local/lib/libbtbb.so.1`. A machine
  with a distribution or Homebrew libbtbb beside this fork's reports two
  versions that may be the same number, and the path is what tells them apart.
  `install.sh` ends by printing the host lines the tools it installed report,
  and warns if they load a libbtbb from somewhere else.
  [#337](https://github.com/SqueakyFloors/ubertooth/issues/337)
- **Script-visible change:** `ubertooth-btbr` prints the packets a command
  decodes, and the inquiry responses with their EIR, on standard output, one
  line each with the time in front of it (`13:54:02 RX LMP ...`), so a session
  can be piped or saved; they used to be `HH:MM:SS | INFO | ...` log lines on
  standard error, where nothing reading the tool's output could see them. The
  progress a command makes stays on standard error, now as plain lines with
  neither the time nor the level.
  [#735](https://github.com/SqueakyFloors/ubertooth/issues/735)
- `ubertooth-btbr` reports the LMP packets a connection decodes (`page`,
  `l2ping`, `page_scan`, `discoverable`), which it printed nowhere a reader
  could see before. Packet lines carry no terminal color any more, and
  `monitor` prints the payload of a packet that failed its CRC as plain hex
  rather than a Python `b'...'` repr.
  [#735](https://github.com/SqueakyFloors/ubertooth/issues/735)
- **Script-visible change:** `ubertooth-btbr` says `error: standard output has
  gone away` and exits 1 when whatever was reading it stops, as
  `ubertooth-btbr | head -5` does. It used to end in status 120 and a message
  from the interpreter.
  [#735](https://github.com/SqueakyFloors/ubertooth/issues/735)
- `-V` is the version command in every tool, and prints the same labelled
  lines whichever tool you ask: the two host lines above, then
  `Firmware version: ...` and `Firmware build: ...` for the board `-U` names,
  or for each attached board, indexed as the other per-board lines are. With
  no Ubertooth attached it prints the host lines and `Firmware version: no
  Ubertooth attached` and still exits 0, so it answers "which tools am I
  running?" on its own; a board that is attached but cannot be read says why
  on its line and the command exits 1. It was only in `ubertooth-rx`,
  `ubertooth-afh` and `ubertooth-tx`, where it printed the host line alone.
  [#542](https://github.com/SqueakyFloors/ubertooth/issues/542)
- **Script-visible change:** `ubertooth-util -V` now prints that same
  three-line answer. It used to print the firmware build string alone, which
  is the third line of it now, and `-U all -V` no longer lists that string per
  board: `-V` reports every attached board itself. The build string is no
  longer prefixed with the image name it came from (`bluetooth_rxtx: ubertooth
  git-...`): the line above it names the image, and the `Firmware build: `
  label names the line. `-V` also ends the command, as `-h` does, so
  `ubertooth-util -V -r` prints the version and does not reset the board; it
  used to be a query that ran alongside the rest of the command line.
  [#542](https://github.com/SqueakyFloors/ubertooth/issues/542)
- **Script-visible change:** `ubertooth-util -H` and `--host-version` are
  removed. `-V` prints the host lines, with no board needed, so nothing is
  lost; they were added in this fork and have never been in a release.
  [#542](https://github.com/SqueakyFloors/ubertooth/issues/542)
- `ubertooth-util -v` is deprecated in favour of `-V`. It keeps printing the
  `Firmware version:` line alone, unchanged and with no warning, so scripts
  that parse it are undisturbed.
  [#542](https://github.com/SqueakyFloors/ubertooth/issues/542)
- **Script-visible change:** `ubertooth-btle`'s CRC verify setting moves from
  `-v` to `-k`: `-k` reads it, `-k0` and `-k1` set it. `-v`, `-v0` and `-v1`
  keep working for this release and print `warning: -v for the CRC verify
  setting is deprecated and will be removed; use -k`; after it, `-v` means
  verbose there as in every other tool. Giving both spellings in one command
  is refused.
  [#542](https://github.com/SqueakyFloors/ubertooth/issues/542)

### Build and compatibility

- The host tools build with a strict C standard (`-std=c17`, `-std=c2x`) on
  Linux, where that used to fail on `CLOCK_REALTIME`, `usleep()` and the
  byte-order helpers: the build asks glibc for the interfaces it uses, instead
  of leaving it to the compiler's default GNU standard.
  [#648](https://github.com/SqueakyFloors/ubertooth/issues/648)
- `install.sh --uninstall` takes the install back off again: the files the
  install recorded, the udev rule with them, and nothing else. The libbtbb it
  installed is kept unless you ask for it, because other builds link against it.
  [#337](https://github.com/SqueakyFloors/ubertooth/issues/337)
- `install.sh` says it is checking Homebrew, and that it is working out which
  Ubertooth to flash, before those slow steps run; on macOS its first line
  used to be a couple of seconds late, which looked like a hang.
  [#640](https://github.com/SqueakyFloors/ubertooth/issues/640)
- On macOS, `cmake -S host -B build && cmake --build build` builds the whole
  tree. It used to stop on one test unless the configure was given
  `-DCMAKE_C_FLAGS="-I$(brew --prefix)/include"`, because Homebrew keeps
  libusb's header off the compiler's search path; the build now takes that
  directory from libusb, so no include flag of your own is needed and
  `install.sh` passes none. `install.sh` now also hands your `CFLAGS` to every
  configure, on Linux as well as macOS, so a second run with different
  `CFLAGS` builds with them rather than with the first run's.
  [#715](https://github.com/SqueakyFloors/ubertooth/issues/715)
  [#728](https://github.com/SqueakyFloors/ubertooth/issues/728)
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

- `ubertooth-btle -p` discovers the access address of the connection it finds,
  instead of inheriting one from an earlier command. A `-t` target set for
  follow mode used to count on the board as "the host gave us an access
  address", so promiscuous mode skipped the half of itself that discovers one
  and synced on the advertising address `8e89bed6`, capturing nothing -- and
  the setting stayed on the board, so every later `-p` did the same until the
  board was reset. The board now keeps the two apart, and a connection
  timeout during `-p` puts discovery back rather than falling to `8e89bed6`.
  **Script-visible change:** `-t` with `-p` is now refused, `-t none`
  included: `error: -t does not go with -p: promiscuous mode has no device
  address to match a target against`, with exit 1 and before a board is
  opened.
  [#825](https://github.com/SqueakyFloors/ubertooth/issues/825)
- `ubertooth-btle -a <address>` no longer narrows what a later `-f` or `-n`
  run logs. Setting an access address used to switch the board's advertising
  filter on with an empty target, which passes the six advertising packet
  types it knows and drops every other, extended advertising included.
  [#825](https://github.com/SqueakyFloors/ubertooth/issues/825)
- `ubertooth-ego` captures in the mode its command line asks for, on a board
  left in another E-GO mode by a run that was interrupted without stopping it:
  `ubertooth-ego -r -c 2418` after a killed `-f` used to go on hopping across
  the E-GO channel list, and its capture looked like a continuous receive that
  found nothing. A second run of the same mode starts over too, so it receives
  on the channel it was given rather than the one before it. `ubertooth-ego
  -r` and `-i` after any other interrupted capture, such as a killed
  `ubertooth-specan`, also used to start in follow mode.
  [#757](https://github.com/SqueakyFloors/ubertooth/issues/757)
- One Ctrl-C ends a capture even when it arrives while the board is still
  being opened. The tools installed their signal handlers after the board was
  open, and a tool started as a background job by a script is started with
  SIGINT ignored, so a signal arriving in that window was discarded: the first
  Ctrl-C did nothing and a second was needed. Selecting a board by serial
  number reads the serial of every attached board, and one that does not
  answer costs a second, which is long enough for a signal to land there. The
  board selection now gives up as soon as the stop arrives, and a run stopped
  before it opened a board exits 0 rather than reporting a failure to find one.
  [#584](https://github.com/SqueakyFloors/ubertooth/issues/584)
- `ubertooth-follow` hops on the piconet's channels after a run that captured
  nothing. The board now keeps an AFH channel map stated after the mode
  request, as it already kept a channel: on a board left in a mode that
  captures no packets -- a killed `ubertooth-specan`, `-btle` or `-ego` -- the
  map that arrived next used to be thrown away by the mode start's reset, and
  the follow hopped all 79 channels instead.
  [#788](https://github.com/SqueakyFloors/ubertooth/issues/788)
- A capture that is losing packets says so, in every mode that captures:
  `warning: the board's packet queue filled up and it dropped at least one
  packet; this capture has a gap` as it happens, and `warning: the capture has
  3 gaps where the board's queue filled up and it dropped packets` when the
  capture ends. A capture the board could not keep up with used to look exactly
  like a quiet band. The count is of gaps, not of lost packets: the board sends
  no number.
  [#786](https://github.com/SqueakyFloors/ubertooth/issues/786)
- **Script-visible change:** every tool that ends a capture mode exits 1 when
  the board refused the request that ends it. Such a board is left running
  that mode and no other tool can open it until it is reset or re-plugged, so
  a run that reported success used to be followed by a board nothing could
  open. `ubertooth-afh`, `-btle`, `-ducky`, `-dump`, `-ego`, `-follow`, `-rx`,
  `-scan`, `-specan` and `-tx` all do this; whatever they captured is still
  written out.
  [#741](https://github.com/SqueakyFloors/ubertooth/issues/741)
  [#802](https://github.com/SqueakyFloors/ubertooth/issues/802)
- **Script-visible change:** `ubertooth-btbr` exits 1 when the thread reading
  the board stopped on a failure, for example because the board's hub port was
  power-cycled during an `inquiry`. Nothing restarts that thread, so no
  message arrives for the rest of the session; it reported every step
  correctly and then exited 0, so a script could not tell such a run from one
  where every command worked. A single command the board refused still leaves
  the prompt up and does not change the status.
  [#737](https://github.com/SqueakyFloors/ubertooth/issues/737)
- A run refused for its firmware leaves no empty capture file behind.
  `ubertooth-btle`, `-dump`, `-follow`, `-rx` and `-specan` create the file
  `-q`, `-c`, `-r` or `-d` names once the board is open and its firmware
  accepted, not before, so there is nothing to tell apart from a capture that
  caught nothing.
  [#803](https://github.com/SqueakyFloors/ubertooth/issues/803)
- A board that has left the USB bus is reported as that rather than as
  firmware too old: `error: could not stop the running mode: the board is no
  longer attached`, with no note about firmware API versions. Such a board can
  stall a request on its way out, which sent the reader off to reflash a board
  that was never the problem.
  [#805](https://github.com/SqueakyFloors/ubertooth/issues/805)
- `ubertooth-btle -f` keeps up with the connection it is following instead of
  parking on one data channel. A first connection event that received nothing
  left the next one armed at a moment the board's clock had gone past, which it
  only reaches when that clock wraps 429 seconds later; and a connection that
  then stopped delivering was chased forever rather than letting the capture go
  back to the advertising channel for the next one. Against a bench peer
  opening a connection every 4 seconds, the same 240 seconds now captures 20647
  packets across 16 connections where it captured 8038 across 11.
  [#774](https://github.com/SqueakyFloors/ubertooth/issues/774)
- A packet in a low-energy capture carries the access address it was received
  with. A connection request set the connection's address as soon as it was
  read, while the radio went on listening for the advertising one until the
  next hop, so packets received in between were recorded under the
  connection's: other devices' advertising packets turned up in the capture as
  the opening packets of the connection just followed.
  [#774](https://github.com/SqueakyFloors/ubertooth/issues/774)
- `ubertooth-btle -p` keeps capturing after a failed capture transfer. One such
  error used to end the capture: the board went on answering the host and
  recording nothing until the tool was killed.
  [#823](https://github.com/SqueakyFloors/ubertooth/issues/823)
- `ubertooth-util -w` reports how many capture transfers have failed since
  power-on, so a capture losing packets to the hardware tells itself apart from
  a quiet channel, and what the board's last processor fault was: the fault
  status register and the address of the refused access.
  [#823](https://github.com/SqueakyFloors/ubertooth/issues/823)
  [#764](https://github.com/SqueakyFloors/ubertooth/issues/764)
- Every Classic capture finds the access codes that start at the last position
  of a bank of symbols. The search was given one position fewer than the bank
  holds, so an access code beginning 64 symbols before the bank's end -- with
  all 64 of its symbols captured -- was never found: one in 337 positions
  lost, in every bank, in `ubertooth-rx` and its `-z` survey, `ubertooth-scan`
  and the three AFH detection paths.
  [#792](https://github.com/SqueakyFloors/ubertooth/issues/792)
- `ubertooth-afh` no longer reports the two channels at the edges of the band
  as used without having seen a packet on them. Detection fills a single
  unused channel between used ones, and off the end of the band -- Basic Rate
  has channels 0 to 78 -- it read a channel that does not exist as used, so a
  packet on channel 1 marked channel 0 used and one on channel 77 marked
  channel 78. Both went into the printed map and into the map sent to the
  board.
  [#756](https://github.com/SqueakyFloors/ubertooth/issues/756)
- A low-energy capture is one of the captures that says what it lost. The
  board had no way to report a low-energy packet it dropped for want of queue
  room, and `ubertooth-btle` did not ask, so such a capture looked like a quiet
  channel; the board now marks the next packet through and the tool reports the
  gaps when the capture ends. It matters most for long packets, which take
  several of the queue's slots and are dropped whole, so a busy capture could
  lose exactly the packets that were wanted.
  [#547](https://github.com/SqueakyFloors/ubertooth/issues/547)
- A low-energy capture survives a poll the board did not answer in time. One
  unlucky poll used to end the capture with `error: USB poll failed`, however
  long it had been running; it is now retried, as a stalled poll already was,
  up to three attempts. A board that has gone still ends the capture at once.
  `ubertooth-ego` reads packets the same way and gets the same.
  [#710](https://github.com/SqueakyFloors/ubertooth/issues/710)
- **Script-visible change:** `ubertooth-util -n` interrupted with Ctrl-C stops
  the range test, like every other ending of the wait, and exits 1 with
  `error: interrupted before the range test finished; the range test has been
  stopped`. A signal used to leave the board listening for a reply forever,
  until someone ran `-S`.
  [#694](https://github.com/SqueakyFloors/ubertooth/issues/694)
- A tool that checks the firmware API version now checks the firmware image
  too, and one run against a board carrying this tree's other image is refused
  by name as the board is opened: `error: the Ubertooth is running btbr; this
  tool needs bluetooth_rxtx`, with a `note:` giving the `ubertooth-dfu` line
  that flashes it. Both images report the same API version, so nothing refused
  such a board and the tool failed instead on the first request that image
  does not answer, reported as a libusb error. `ubertooth-util` and
  `ubertooth-dfu` make neither check.
  [#718](https://github.com/SqueakyFloors/ubertooth/issues/718)
- `ubertooth-debug` checks the firmware image too, so a board carrying
  `btbr` is refused by name before a register is read, with the same
  `note:` line that flashes `bluetooth_rxtx`. That image answers no register
  read, so the command used to fail on the read itself and report a refused
  request instead of the firmware. The firmware version is still not checked:
  reading CC2400 registers is debugging work on whatever revision a board
  carries.
  [#808](https://github.com/SqueakyFloors/ubertooth/issues/808)
- `ubertooth-util -r` recovers a board that answers nothing, which used to
  need a re-plug or a hub port power cycle. Such a board refuses the reset
  request like every other request, so `-r` resets its USB connection first --
  the re-plug done in software -- and then resets the board as usual, so it
  comes back idle. Select it by index: its serial cannot be read.
  [#731](https://github.com/SqueakyFloors/ubertooth/issues/731)
- A stray dash after an option is named with the word it was written in:
  `ubertooth-util -v-` reads `error: unrecognized option '-' in '-v-'`. It
  used to be blamed on `--`, which nobody typed, or on the next word, so
  `ubertooth-util -v- -h` complained about `-h`. Every tool reported it that
  way.
  [#501](https://github.com/SqueakyFloors/ubertooth/issues/501)
- A command line with a dash in more than one word names the word that is
  actually wrong. `ubertooth-util -v- -Ufoo-` read `error: unrecognized option
  '-' in '-Ufoo-'`, a word with nothing wrong in it -- the dash there is part
  of `-U`'s value -- and now reads `'-v-'`, the word the tools stopped on.
  [#790](https://github.com/SqueakyFloors/ubertooth/issues/790)
- A value that is not a Bluetooth address reads the same way from every option
  that takes one, naming the address it sets: `error: target address must be
  xx:xx:xx:xx:xx:xx (got <word>)`. Three tools worded it three ways, and
  `ubertooth-btle` had a second wording for a bad hex digit and called the
  mask of `-t` and `-s` a `MAC mask` beside it.
  [#541](https://github.com/SqueakyFloors/ubertooth/issues/541)
- `ubertooth-specan` checks every write of its text formats, so a sweep into
  a `-d` file that cannot be written ends where the writing failed and names
  it: `error: writing samples failed: could not write the sample (No space
  left on device)`. Only the raw records were checked, so a text sweep ran to
  the end and reported the vaguer `could not flush the samples`.
  [#655](https://github.com/SqueakyFloors/ubertooth/issues/655)
- A control request the board's USB controller was briefly too slow to report
  is now answered microseconds later instead of going unanswered. The firmware
  read the controller's answer before it could tell whether one had arrived,
  and a reply that came late was dropped along with the request that was
  waiting -- a one-second wait on the host, and the end of a `ubertooth-btle`
  capture, which reads every captured packet with a control request.
  [#714](https://github.com/SqueakyFloors/ubertooth/issues/714)
- A control request can no longer be answered with the leftovers of the one
  before it. Every reply used to be followed by a zero-length packet that was
  offered to the host and left on the endpoint, because a reply shorter than
  64 bytes has already ended its own data stage and nothing collects it; a
  later request's read took it instead and came back with no data, no error
  and no stall.
  [#462](https://github.com/SqueakyFloors/ubertooth/issues/462)
- Every e-go record carried two bytes of its own timestamp at the end of its
  data, because the receive buffer was two bytes shorter than the record it
  goes out in. The buffer is now the record's whole data field, and the
  receive loop stays inside it whatever capture length the board was given.
  [#749](https://github.com/SqueakyFloors/ubertooth/issues/749)
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
- A board that is advertising (`ubertooth-btle -s`, `ubertooth-ducky`) keeps
  answering the host if its radio stops sending. The wait for the radio to
  empty its transmit buffer had no end, and that wait sits in the loop that
  services USB, so a radio that stopped draining left the board answering
  nothing until it was unplugged and plugged back in.
- The two-board range test reports a result every run, instead of
  `invalid range test: mismatch on byte 0` every five to ten runs. Both boards
  started reading a packet out of the radio the moment it reported the sync
  word, which is before the radio has written the first byte, so byte 0 came
  back wrong -- and only byte 0, because the read trails the air from there on.
  Each now waits for the radio to have the start of the packet.
  [#686](https://github.com/SqueakyFloors/ubertooth/issues/686)
- A Classic packet longer than one 400 µs bank is reported as what was
  captured, and a piconet follow runs to the end instead of dying partway.
  `ubertooth-rx` told the decoder it had 4000 symbols where 400 had
  arrived, so a DH5's payload and its CRC verdict were decoded out of about
  2700 bytes of zeros, on screen and in the payload written to a capture file,
  with nothing marking it as incomplete. Reading past the symbols also walked
  off the end of the buffer they sit in: `ubertooth-rx -l` aborted with
  `double free or corruption` on about the thirty-sixth packet of a piconet,
  live or replayed from a capture. Its search for an access code now stops
  where the other capture paths do, at the last position whose whole access
  code was captured, so a capture run at a high `-e` reports somewhat fewer
  packets: the positions that are gone could only ever have matched symbols
  that never arrived.
  [#666](https://github.com/SqueakyFloors/ubertooth/issues/666)
  [#792](https://github.com/SqueakyFloors/ubertooth/issues/792)
- A Classic packet's signal level is its own from the first packet on a
  channel. The table of recent maxima started with one of its 790 entries
  filled, and the level is the strongest entry of the packet's channel, so
  every channel reported at least about -55 dBm for its first ten packets: a
  packet that should have read -91 dBm read -55 dBm, in the signal field of
  the record written to a capture file as well as on screen, and the
  signal-to-noise ratio printed beside it was out by the same 36 dB.
  [#665](https://github.com/SqueakyFloors/ubertooth/issues/665)
- The clock a Classic packet is placed at is the clock its access code
  arrived on. `ubertooth-rx` put a packet captured in the first 400 µs of a
  mode, or in the 400 µs after the clock starts over every 327.68 s, about
  seven minutes ahead of the clock, and `ubertooth-scan` placed every packet a
  whole 625 µs slot late, which is what its survey mode builds its clock
  candidates out of.
  [#664](https://github.com/SqueakyFloors/ubertooth/issues/664)
- A piconet follow's clock trim moves the board's clock towards the piconet
  rather than away from it. A correction that should have advanced the clock
  was sent as a large positive delay, which stretched one tick to 6.85 ms and
  left the clock 5.91 ms behind where 15 µs forward was wanted, and the drift
  it reported alongside was computed unsigned: a real 15 PPM came out as
  1342162, was printed as that, and left the board being slowed by a further
  320 PPM, the same direction as the error, for the rest of the run. So
  `ubertooth-rx -l`/`-u` and `ubertooth-follow` keep the hop sequence they
  have found.
  [#663](https://github.com/SqueakyFloors/ubertooth/issues/663)
- A capture gets the channel, or the whole-band sweep, its command line asked
  for, whatever the board was doing before. A tool states its channel just
  after the request that starts its mode, and a board left in a mode that
  captures nothing -- a killed `ubertooth-specan`, `-btle` or `-ego` -- threw
  that channel away as it left the old mode, a moment later. Every packet then
  came back on 2441 MHz while the user had asked for the band, an
  `ubertooth-scan` AFH map was built from one channel, and `ubertooth-ducky`
  advertised on 2441 MHz, which is not an advertising channel at all, whatever
  `-A` asked for. The board now keeps a channel or sweep stated after the mode
  request, and `ubertooth-btle`, `-ducky` and `-ego` state theirs there.
  [#543](https://github.com/SqueakyFloors/ubertooth/issues/543)
- `ubertooth-ego` captures with the access code the run asked for.
  `-a <code>` used to stay on the board until it was reset or reflashed, so the
  next run captured with the previous run's code while `-h` and the man page
  called `630f9ffe` the default. A mode start now puts the documented default
  back, and `-a` is stated for the run that asks for it.
  [#397](https://github.com/SqueakyFloors/ubertooth/issues/397)
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
- `ubertooth-btle -f` waits out a connection's whole transmit window before
  giving up on its first connection event. A connection request names that
  window, and the master's first packet may arrive anywhere inside it, but the
  board allowed the time of one packet, so a window longer than the 1.25 ms
  minimum could expire unwatched. A separate fault still loses the opening
  events of most connections
  ([#774](https://github.com/SqueakyFloors/ubertooth/issues/774)), so this is
  not yet the whole of "the capture starts where the connection does".
  [#625](https://github.com/SqueakyFloors/ubertooth/issues/625)
- Every packet in a `ubertooth-btle` capture is
  timestamped 8 microseconds earlier, which is where its preamble really
  started. The board back-dated the reading to the preamble but left out the
  header byte that had also arrived by then, so air times measured from a
  capture, and comparisons against a capture taken by other hardware, were off
  by that much.
  [#662](https://github.com/SqueakyFloors/ubertooth/issues/662)
- `ubertooth-btle -p` still reports the connection it found when the capture
  is written to a file. The access address, CRC init value, hop interval and
  hop increment went to standard output, which `-q -` or `-c -` has taken for
  the capture, so a user had no way to learn the access address their capture
  was of. They follow the tool's other answer lines to standard error.
  [#614](https://github.com/SqueakyFloors/ubertooth/issues/614)
- `ubertooth-btle` and `ubertooth-ego` say when a capture is losing packets.
  Both read their packets one at a time, and a reply holding part of a record
  was dropped without a word, so a capture losing packets looked exactly like
  a quiet board. They now report it once per run, as a streaming capture
  already did.
  [#637](https://github.com/SqueakyFloors/ubertooth/issues/637)
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
- `ubertooth-util -w` names two more: an interrupt the firmware does not
  handle, and a failed debug UART transfer. Both stop the board in a loop that
  marked nothing, so the report named whatever the board had been doing before
  it got there.
  [#723](https://github.com/SqueakyFloors/ubertooth/issues/723)
- `ubertooth-util -w`'s line for what the board is doing now no longer names a
  wait the board has already left. A wait reached from inside an interrupt
  handler used to claim that line on its way out, and the crystal-oscillator
  wait is reached that way on every BLE channel hop.
  [#730](https://github.com/SqueakyFloors/ubertooth/issues/730)
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
- `ubertooth-btle -p` reports the access address of the connection it is
  following. It is the first of the four parameters the mode works out and the
  only one that says which connection a capture is of, and it never reached the
  host: the board queued it and then emptied the queue on its way into the
  half of the mode that follows the connection. The CRC init value, hop
  interval and hop increment were unaffected.
  [#237](https://github.com/SqueakyFloors/ubertooth/issues/237)
- `ubertooth-btle -p` no longer measures its first connection interval against
  a clock reading left by an earlier attempt. The board times how long the
  connection takes to come back to the channel it is listening on and keeps the
  smallest gap, but it kept the last sighting where nothing cleared it, and the
  capture clock is stopped and zeroed between attempts: the first gap was
  measured from a reading of a clock that had since restarted, so it was an
  arbitrary number, and whenever it came out small it was the interval reported
  for the rest of the attempt. The stamp the hop increment is counted from had
  the same fault.
  [#713](https://github.com/SqueakyFloors/ubertooth/issues/713)
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
  host, so on a quiet channel the board was unreachable; the repeater did not
  look at the requested mode.
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
- A capture filter typed for an Ubertooth interface in Wireshark now turns the
  filter box red with `this interface does not apply capture filters; use a
  display filter`. The box used to stay green and the capture ran unfiltered,
  with the notice on an error output Wireshark shows only for a capture that
  failed. An empty filter box is still accepted.
  [#343](https://github.com/SqueakyFloors/ubertooth/issues/343)
- **Script-visible change:** `btle-extcap.py --capture` exits 0 or 1 and
  nothing else. It used to pass `ubertooth-btle`'s own exit status through, or
  report 128 plus the number of the signal that killed it. It also handles
  Ctrl-C, which `tshark` sends to the whole process group: the script used to
  die of it and report nothing, where now a capture that was asked to stop
  exits 0 as it does under Wireshark.
  [#343](https://github.com/SqueakyFloors/ubertooth/issues/343)
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
  the device is opened. `ubertooth-btle -k` and `ubertooth-ducky -q` also take
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
- A live capture that goes quiet for longer than its capture clock's period no
  longer comes out 429.50 s wrong from there on, which `ubertooth-btle -f` and
  `-n` could do on any quiet channel. How many times the board's clock started
  over during a gap now comes from the time the host's own clock saw pass.
  [#465](https://github.com/SqueakyFloors/ubertooth/issues/465)
- Two records out of place in a live capture cost it less. A first record
  reading below the records after it no longer puts every later time up to a
  period into the future, which nothing reported, and a record out of place
  further in no longer adds a period to the records after it where its reading
  dropped by less time than the host saw pass.
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
- A Classic capture records the reference UAP on the packet that settles it.
  `ubertooth-rx` read the piconet's UAP before handing the packet to the
  decoder, so the one packet whose CRC settled that UAP went into the capture
  as if nothing were known, and a reader could not check that packet's CRC -
  the packet the UAP was derived from being the one record that most needs it.
  [#767](https://github.com/SqueakyFloors/ubertooth/issues/767)
- A Classic packet whose 2/3 FEC payload is not a multiple of five bytes long
  now puts its last code group on the air whole, padded with zeros as the
  Bluetooth specification asks. The `btbr` image counted that group as 12 bits
  per byte rather than the 15 bits it occupies, so the transmitter stopped up
  to 9 bits early and the receiver looked for the rest of the payload in the
  wrong place: about one payload size in sixteen failed its checksum, and the
  bits cut off the air were filled from whatever was on the stack rather than
  with zeros. **A board running an older build of the `btbr` image will not
  interoperate with a board running this one, for the DM1, DM3 and DM5 sizes
  this changes**, and the firmware API version does not move for it, so nothing
  warns you about a mixed pair: reflash both boards. Paging is unaffected, its
  FHS packet being 20 bytes, which was already counted right.
  [#706](https://github.com/SqueakyFloors/ubertooth/issues/706)
- A Classic capture record carries the packet's own header once the followed
  piconet's clock bits are known. It went in as 18 zero bits until the follow
  began, which a reader cannot tell from a NULL packet that was never sent.
  Records written before that still carry none. See WHAT A CAPTURE RECORD
  HOLDS in ubertooth-rx(1).
  [#778](https://github.com/SqueakyFloors/ubertooth/issues/778)

### Documentation

- Every man page lists `-h` in its OPTIONS section, where a reader looks for
  it: eleven of them only mentioned it elsewhere. ubertooth-specan(1) no
  longer promises the usage text after a bad option, which no tool prints.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- `CONTRIBUTING.md` says what a change to this fork needs: the tests to run,
  the CI checks a pull request has to pass and what each is for, and that
  nothing in the repository enforces them, so a reviewer has to read a change
  to `.github/workflows/` as carefully as the code.
  [#700](https://github.com/SqueakyFloors/ubertooth/issues/700)
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
  for the rules. The earlier `-U<0-7>` described neither half: the index is a
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
- ubertooth-afh(1) says what `-m` trades off. It counts packets rather than
  time, so the default of 5 cannot settle the map of a piconet using the whole
  band, and a value that does settle it is in the hundreds.
  [#194](https://github.com/SqueakyFloors/ubertooth/issues/194)

### Under the hood

Fixes with no visible change in normal use.

- The thread that completes a capture's transfers treats a wait a signal cut
  short as a wait to take again. It used to end the thread, which left the
  capture with nobody to complete its transfers and no line to say why; the
  same wait during a capture's teardown now finishes as well, instead of
  leaving the transfer behind for the next capture on that handle to refuse.
  [#584](https://github.com/SqueakyFloors/ubertooth/issues/584)
- The generic receive clears the radio's FIFO between packets. A packet longer
  than the FIFO left bytes in it, and those bytes came out in front of the next
  packet: the record after a long one arrived shifted. No shipped tool asks for
  the mode.
  [#775](https://github.com/SqueakyFloors/ubertooth/issues/775)
- The clock drift the board accumulates stays within what it can correct. A run
  of drift requests used to walk past what the 16-bit figure holds and come
  back with the other sign, which would run the clock the opposite way to the
  one that was asked for.
  [#768](https://github.com/SqueakyFloors/ubertooth/issues/768)
- A followed connection's events keep their place in the count the connection
  keeps, whatever the connection asked for: an event is never given longer to
  run than the interval between events, so no two intervals are counted as one.
  The count is what every later parameter change is timed against, and one
  short step in it timed all of them a little early.
  [#774](https://github.com/SqueakyFloors/ubertooth/issues/774)
- A followed connection whose parameters change keeps its timing from the
  window the change named, rather than carrying on from where the old
  parameters had it.
  [#774](https://github.com/SqueakyFloors/ubertooth/issues/774)
- The board's record of why it last reset itself says which firmware wrote it,
  so a firmware that keeps more there than the one before it starts the counts
  again instead of reading its own fields out of what it finds. Flashing this
  one over an earlier one therefore reports no unrequested resets, and this
  boot as the board's first.
  [#764](https://github.com/SqueakyFloors/ubertooth/issues/764)
- The `*` on a version string, which says the checkout had uncommitted
  changes, is about the checkout the tools were built from. It used to be
  about whichever tree you happened to be standing in when you ran `cmake`.
  [#743](https://github.com/SqueakyFloors/ubertooth/issues/743)
- `ubertooth-debug` prints the radio's modulation offset as the signed number
  of steps it is, so a negative offset no longer reads as a large positive
  frequency. The firmware leaves that field at zero, so a board running this
  tree always read it right.
  [#732](https://github.com/SqueakyFloors/ubertooth/issues/732)
- The ToorCon 13 badge's range test listens long enough to hear the reply.
  Its window was about 90 ms of the second the Ubertooth repeater holds the
  reply for, so the badge gave up before the reply was sent and the test could
  not pass at all. The window is sized from that delay now. Untested: nobody
  here has a badge.
  [#691](https://github.com/SqueakyFloors/ubertooth/issues/691)
- The ToorCon 13 badge's range test puts its packets in the radio and reads
  them back, so it can pass. It reached the radio's buffer a byte at a time,
  which the radio does not take as data at all: it transmitted empty packets
  and read zeros back, so the reply never matched what was sent and the test
  could not pass however close the two boards were. Untested: nobody here has
  a badge, and no build makes this image.
  [#705](https://github.com/SqueakyFloors/ubertooth/issues/705)
- A board selection that ran out of memory exited 1 with no line at all,
  whether it was asked for by index or by serial.
- The `ubertooth-btle` that `btle-extcap.py` runs for a Wireshark capture no
  longer writes its text decode of every packet, some 560 kB for 8 seconds of
  advertising, onto the script's own output: the capture goes to Wireshark's
  pipe, and the decode is dropped. Wireshark discarded it anyway, and keeping
  it off the error output leaves room there for the reason a failed capture
  gives.
  [#343](https://github.com/SqueakyFloors/ubertooth/issues/343)
- A channel set on a board that is following a low-energy connection is kept
  instead of being accepted and thrown away. The board went on reporting its
  old channel and tuned the next mode to it, so a tool that set a channel on a
  board left in that mode by a capture that did not stop cleanly then received
  on whichever channel was there before, with nothing to say so. The connection
  being followed is unaffected either way: that mode hops on the connection's
  own channel map.
  [#440](https://github.com/SqueakyFloors/ubertooth/issues/440)
- A USB packet the board's controller would not take, or would not release
  once read, is reported to the firmware as such instead of as a packet that
  went through. The firmware could act on a received packet the hardware still
  held and might hand over again, which on a control request means running the
  request twice.
  [#358](https://github.com/SqueakyFloors/ubertooth/issues/358)
- `ubertooth-dfu -d <image> -r` tells three things apart after the reset: a
  board that did not come back, a board that is back and will not open, and a
  host USB bus that could not be read at all. The last two both read as a
  board that would not open, which named a fault the board may not have had.
  [#676](https://github.com/SqueakyFloors/ubertooth/issues/676)
- A firmware without transmit refuses a request to interfere with an E-GO
  connection without storing the mode it refused, so the board is left as it
  was. No shipped tool can reach it: `ubertooth-ego -i` stops on the refusal.
  [#397](https://github.com/SqueakyFloors/ubertooth/issues/397)
- A packet captured by `ubertooth-btle -p` is timestamped a fixed moment after
  the board synchronized on it, rather than somewhere in the middle of the
  packet, so its time no longer drifts later the longer the packet is. Times
  in a `-p` capture move earlier by up to about a third of a millisecond.
  [#546](https://github.com/SqueakyFloors/ubertooth/issues/546)
- A connection update is checked against the ranges the specification allows,
  the way a connection request already was, and one outside them is ignored
  instead of being taken on trust: the capture keeps following the connection
  on the parameters it has. A window size of 255 used to put 319 ms into the
  wait for the first event under the new parameters, and a supervision timeout
  of hours was then all that ended the chase.
  [#624](https://github.com/SqueakyFloors/ubertooth/issues/624)
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
- The BR/EDR image's per-slot waits on the radio's state machine give up at a
  point in the slot rather than after a fixed number of tries. A radio that
  was late used to hold the timer interrupt for about nine milliseconds, and
  the piconet clock lost every slot it spent there, around 28 of them; now it
  loses at most the slot it is in.
  [#496](https://github.com/SqueakyFloors/ubertooth/issues/496)
- The ToorCon badge's transfers to the radio give up rather than waiting
  forever for the part's UART. Every read and write the badge makes goes
  through that wait, so a UART left switched off froze the badge underneath all
  of its bounded waits on the radio.
  [#681](https://github.com/SqueakyFloors/ubertooth/issues/681)
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
  which did not check it. A teardown that follows such a failure no
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
  which has it on, did not get a result. The BLE transmit path fills the
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
- A record the board sends no longer ends in bytes of whatever was on its
  stack: the generic radio receive built its record in a buffer shorter than
  the fixed 50 bytes that are sent, and the spectrum analyzer left its last
  two bytes unwritten.
  [#661](https://github.com/SqueakyFloors/ubertooth/issues/661)
- A board running `btbr` that hits a processor fault blinks a code that can be
  read: the pause between blinks did not pause, two of the four faults shared
  a count, and no code finished before the board reset itself. See the LEDs
  doc for what each one means.
  [#670](https://github.com/SqueakyFloors/ubertooth/issues/670)
- Corrected firmware signal-strength channel bounds. Credit: Gabriele
  Gristina ([@matrix](https://github.com/matrix)).
  [upstream #481](https://github.com/greatscottgadgets/ubertooth/pull/481)
- The radio's data FIFO is written and read with interrupts masked, as its
  registers already were. A control request arriving from the host while the
  board was writing a packet into the radio could take over the bus part-way
  through and corrupt both transfers.
  [#429](https://github.com/SqueakyFloors/ubertooth/issues/429)
- A generic transmit packet longer than the radio's 32-byte FIFO goes in a
  piece at a time, waiting for the radio to send each one. The whole packet
  used to be written at once, so a packet of 33 to 50 bytes was pushed past
  the end of the FIFO. How the radio frames a packet that long is a separate
  question, still open.
  [#395](https://github.com/SqueakyFloors/ubertooth/issues/395)
- Sixteen buffers, bounds, timeouts and byte-order conversions that no shipped
  tool can reach now hold on their own, so a later change cannot make one of
  them bite. No shipped tool behaves differently; what moved
  is what a board does with values no tool sends.
  [#656](https://github.com/SqueakyFloors/ubertooth/issues/656)

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
