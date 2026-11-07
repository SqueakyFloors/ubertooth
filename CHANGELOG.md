# Changelog

## 2020-12-R1+modern1

### Highlights

The first release from this community fork, building on Great Scott Gadgets'
2020-12-R1. The sections below list every change; these are the ones worth
knowing about.

- **`ubertooth-btle -f` follows the connections today's devices make.** It now reads both ends' channel-selection bits and hops with algorithm 2 as well as 1, catching 13,676 packets across 23 connections where the same run caught 280 across 1.
  ([#836](https://github.com/SqueakyFloors/ubertooth/issues/836))
- **Promiscuous mode locks onto a connection already running, and captures replay.** `-p` recovers the access address, CRC start, and hop of a connection nobody saw set up (algorithm 2 included), and `-d`/`-R` save and re-decode the records with no board.
  ([#1025](https://github.com/SqueakyFloors/ubertooth/issues/1025), [#1129](https://github.com/SqueakyFloors/ubertooth/pull/1129))
- **Classic sniffing copes with a piconet that adapts its hopping.** `ubertooth-rx -l -u` scores clock candidates by the hops they explain, so it locks onto a piconet that moves off busy channels (1.8 to 25.4 packets a second once locked), `ubertooth-afh` reports the real channel map, and one run can read up to eight boards at once.
  ([#968](https://github.com/SqueakyFloors/ubertooth/issues/968), [#963](https://github.com/SqueakyFloors/ubertooth/issues/963), [#1063](https://github.com/SqueakyFloors/ubertooth/pull/1063))
- **A capture file says what the board really heard.** Each record carries the packet's own signal level and a background estimate (or says it has none), the real Classic header and recovered address part, and a full-length low-energy payload; dropped packets are counted.
  ([#909](https://github.com/SqueakyFloors/ubertooth/issues/909), [#284](https://github.com/SqueakyFloors/ubertooth/issues/284), [libbtbb issue 28](https://github.com/SqueakyFloors/libbtbb/issues/28))
- **The `btbr` image and `ubertooth-btbr` are usable.** The tool starts and names its firmware instead of hanging, a Classic connection to a real device outlives its first 30 seconds, and two Ubertooths page each other and carry traffic.
  ([#623](https://github.com/SqueakyFloors/ubertooth/issues/623), [#948](https://github.com/SqueakyFloors/ubertooth/issues/948), [#807](https://github.com/SqueakyFloors/ubertooth/issues/807))
- **Options the pages describe now do what they say.** The two-board range test reports the lowest power that got through each way, `ubertooth-specan -v` summarizes once a second, `ubertooth-btle -s`/`-t` keep separate addresses, and a capture uses the settings it was asked for rather than an interrupted run's leftovers.
  ([#649](https://github.com/SqueakyFloors/ubertooth/issues/649), [#510](https://github.com/SqueakyFloors/ubertooth/issues/510), [#872](https://github.com/SqueakyFloors/ubertooth/issues/872))
- **A board keeps answering, and recovers itself when it does not.** Opening by serial no longer kills about one capture in twenty, radio and USB waits are bounded, a stuck board usually resets within about three seconds (or `ubertooth-util -r` recovers it from the host), and the firmware now has room for its stack.
  ([#505](https://github.com/SqueakyFloors/ubertooth/issues/505), [#509](https://github.com/SqueakyFloors/ubertooth/issues/509), [#1073](https://github.com/SqueakyFloors/ubertooth/pull/1073))
- **Firmware you can install, identify, and get back from.** Images carry an info block, so `ubertooth-dfu` names what it will write and refuses a bad file, an interrupted download leaves the board recoverable in DFU, and a new bootloader installs over USB with `--update-bootloader`.
  ([#242](https://github.com/SqueakyFloors/ubertooth/issues/242), [#1064](https://github.com/SqueakyFloors/ubertooth/issues/1064), [#1062](https://github.com/SqueakyFloors/ubertooth/issues/1062))
- **One command line across every tool.** Results go to standard output and everything else to standard error, a failure is one `error:` line and a non-zero exit, a lone `-` streams the capture (PcapNG excepted, with advice), and `-V` answers the same everywhere.
  ([#782](https://github.com/SqueakyFloors/ubertooth/issues/782), [#813](https://github.com/SqueakyFloors/ubertooth/issues/813), [#636](https://github.com/SqueakyFloors/ubertooth/issues/636))
- **Documentation that matches the code, including what does not work.** Every tool has a man page, there is a README catalog and a "how sniffing works" guide, a Wireshark walkthrough, and `COMPATIBILITY.md`; an option nothing acts on says so and is listed as known-broken.
  ([#785](https://github.com/SqueakyFloors/ubertooth/issues/785), [#895](https://github.com/SqueakyFloors/ubertooth/issues/895), [#891](https://github.com/SqueakyFloors/ubertooth/issues/891))
- **Tests, with a board in the loop.** 64 firmware test programs and 140 host `ctest` tests, among them a contract matrix pinning each tool's streams, messages, and exit status, hop selection checked against the Core specification's sample data, fuzzed decoders, and CI across Linux and macOS.
  ([#482](https://github.com/SqueakyFloors/ubertooth/issues/482), [#809](https://github.com/SqueakyFloors/ubertooth/issues/809), [libbtbb issue 93](https://github.com/SqueakyFloors/libbtbb/issues/93))
- **It builds and installs on current systems.** Current toolchains and Python 3, `install.sh` for Debian and Ubuntu, Fedora, and macOS (with `--uninstall`), and libubertooth installs with a pkg-config file and a CMake package config.
  ([#852](https://github.com/SqueakyFloors/ubertooth/issues/852), [#1044](https://github.com/SqueakyFloors/ubertooth/issues/1044), [#1108](https://github.com/SqueakyFloors/ubertooth/pull/1108))

### Stability and lockups

A board or a tool that stopped answering, wedged, reset itself, or crashed,
and the reporting that makes those visible.

- **Boards recover on their own.** A board that stops answering resets itself
  within about three seconds, and `ubertooth-util -r` recovers one that
  answers nothing, without a re-plug. A failed request's error suggests
  `ubertooth-util -r` before unplugging.
- **Fixed: boards that hung until re-plugged.** Control requests during a
  capture, stopping a mode, starting another tool with `-U` during a capture,
  or a stalled radio could leave the board unresponsive, in both
  `bluetooth_rxtx` and `btbr`.
- **Fixed: memory corruption and crashes.** The firmware stack grows from
  about 730 bytes to 11 KB, and an overflow faults at once instead of
  overwriting capture data. Interrupt races in the radio and capture code are
  closed, and the host tools no longer crash stopping a capture or when a
  board is missing.
- **`ubertooth-util -w` says why a board stopped:** resets, the mode and wait
  it stopped in, processor faults and their address, timed-out waits, failed
  transfers, and stack use. The firmware API version moved with it; reflash
  older boards.

([#904](https://github.com/SqueakyFloors/ubertooth/issues/904), [#764](https://github.com/SqueakyFloors/ubertooth/issues/764), [#239](https://github.com/SqueakyFloors/ubertooth/issues/239), [#505](https://github.com/SqueakyFloors/ubertooth/issues/505), [#509](https://github.com/SqueakyFloors/ubertooth/issues/509), [#488](https://github.com/SqueakyFloors/ubertooth/issues/488), [#953](https://github.com/SqueakyFloors/ubertooth/issues/953), [#823](https://github.com/SqueakyFloors/ubertooth/issues/823), [#731](https://github.com/SqueakyFloors/ubertooth/issues/731), [#723](https://github.com/SqueakyFloors/ubertooth/issues/723), [#730](https://github.com/SqueakyFloors/ubertooth/issues/730), [#458](https://github.com/SqueakyFloors/ubertooth/issues/458), [#608](https://github.com/SqueakyFloors/ubertooth/issues/608), [#609](https://github.com/SqueakyFloors/ubertooth/issues/609), [#149](https://github.com/SqueakyFloors/ubertooth/issues/149), [#223](https://github.com/SqueakyFloors/ubertooth/issues/223), [#175](https://github.com/SqueakyFloors/ubertooth/issues/175), [#429](https://github.com/SqueakyFloors/ubertooth/issues/429), [#174](https://github.com/SqueakyFloors/ubertooth/issues/174), [#495](https://github.com/SqueakyFloors/ubertooth/issues/495), [#370](https://github.com/SqueakyFloors/ubertooth/issues/370), [#185](https://github.com/SqueakyFloors/ubertooth/issues/185), [#158](https://github.com/SqueakyFloors/ubertooth/issues/158), [#213](https://github.com/SqueakyFloors/ubertooth/issues/213), [#282](https://github.com/SqueakyFloors/ubertooth/issues/282), [#877](https://github.com/SqueakyFloors/ubertooth/issues/877))

### Across all tools

Behavior the tools share.

#### The command line and option values

- Command-line handling revamped so every tool behaves consistently: invalid
  arguments and combinations are rejected before a board is opened. An option
  and its value may now be separated by a space.

([#155](https://github.com/SqueakyFloors/ubertooth/issues/155), [#193](https://github.com/SqueakyFloors/ubertooth/issues/193), [#236](https://github.com/SqueakyFloors/ubertooth/issues/236), [#323](https://github.com/SqueakyFloors/ubertooth/issues/323), [#481](https://github.com/SqueakyFloors/ubertooth/issues/481), [#501](https://github.com/SqueakyFloors/ubertooth/issues/501), [#541](https://github.com/SqueakyFloors/ubertooth/issues/541), [#790](https://github.com/SqueakyFloors/ubertooth/issues/790))

#### Output streams, messages, and exit status

- **Script-visible change.** In every tool, results and capture data go to
  standard output; errors, warnings, and progress go to standard error; help
  (`-h`) exits 0; and a command line with nothing to do is refused. A request the firmware lacks is
  named in the error instead of a raw libusb code, and the Python tools no
  longer end in a traceback when a stream closes.
- **Script-visible change.** Every tool's `-V` option, the Python tools
  included, prints the host and firmware versions the same way. Versions name
  a release rather than a commit, and the same source builds a byte-identical
  image. For example:

      $ ubertooth-util -V
      Host tools version: 2026-11-R1+22 (libbtbb 2026-11-R1+22)
      Firmware version: bluetooth_rxtx 2026-11-R1+22 (API:1.58)

([#21](https://github.com/SqueakyFloors/ubertooth/issues/21), [#275](https://github.com/SqueakyFloors/ubertooth/issues/275), [#323](https://github.com/SqueakyFloors/ubertooth/issues/323), [#337](https://github.com/SqueakyFloors/ubertooth/issues/337), [#481](https://github.com/SqueakyFloors/ubertooth/issues/481), [#542](https://github.com/SqueakyFloors/ubertooth/issues/542), [#610](https://github.com/SqueakyFloors/ubertooth/issues/610), [#636](https://github.com/SqueakyFloors/ubertooth/issues/636), [#745](https://github.com/SqueakyFloors/ubertooth/issues/745), [#812](https://github.com/SqueakyFloors/ubertooth/issues/812), [#911](https://github.com/SqueakyFloors/ubertooth/issues/911), [#920](https://github.com/SqueakyFloors/ubertooth/issues/920), [#930](https://github.com/SqueakyFloors/ubertooth/issues/930), [#1230](https://github.com/SqueakyFloors/ubertooth/issues/1230))

#### Choosing a board, and what it is running

- **Reflash required.** The firmware API version is now 1.58. A release build
  refuses firmware of any other version; a development build warns.
  `ubertooth-dfu` reflashes any board, but only with an image from this tree
  (`2020-12-R1` firmware is refused). Each image carries an info block that
  `ubertooth-dfu -d` and `ubertooth-util -v`/`-V` read.
- Every tool that uses a board selects it the same way, with `-U` followed by
  a USB index or the board's serial number (for example `ubertooth-btle -f -U
  1`), and gives the same messages for no board, several boards, or an
  unreadable serial.
- **Fixed:** a mode setting (jamming, modulation, channel, AFH map) leaked into
  the next run on the same board.

([#4](https://github.com/SqueakyFloors/ubertooth/issues/4), [#6](https://github.com/SqueakyFloors/ubertooth/issues/6), [#21](https://github.com/SqueakyFloors/ubertooth/issues/21), [#27](https://github.com/SqueakyFloors/ubertooth/issues/27), [#28](https://github.com/SqueakyFloors/ubertooth/issues/28), [#77](https://github.com/SqueakyFloors/ubertooth/issues/77), [#165](https://github.com/SqueakyFloors/ubertooth/issues/165), [#167](https://github.com/SqueakyFloors/ubertooth/issues/167), [#174](https://github.com/SqueakyFloors/ubertooth/issues/174), [#179](https://github.com/SqueakyFloors/ubertooth/issues/179), [#188](https://github.com/SqueakyFloors/ubertooth/issues/188), [#191](https://github.com/SqueakyFloors/ubertooth/issues/191), [#209](https://github.com/SqueakyFloors/ubertooth/issues/209), [#210](https://github.com/SqueakyFloors/ubertooth/issues/210), [#232](https://github.com/SqueakyFloors/ubertooth/issues/232), [#240](https://github.com/SqueakyFloors/ubertooth/issues/240), [#261](https://github.com/SqueakyFloors/ubertooth/issues/261), [#312](https://github.com/SqueakyFloors/ubertooth/issues/312), [#318](https://github.com/SqueakyFloors/ubertooth/issues/318), [#323](https://github.com/SqueakyFloors/ubertooth/issues/323), [#352](https://github.com/SqueakyFloors/ubertooth/issues/352), [#395](https://github.com/SqueakyFloors/ubertooth/issues/395), [#396](https://github.com/SqueakyFloors/ubertooth/issues/396), [#401](https://github.com/SqueakyFloors/ubertooth/issues/401), [#429](https://github.com/SqueakyFloors/ubertooth/issues/429), [#433](https://github.com/SqueakyFloors/ubertooth/issues/433), [#434](https://github.com/SqueakyFloors/ubertooth/issues/434), [#440](https://github.com/SqueakyFloors/ubertooth/issues/440), [#464](https://github.com/SqueakyFloors/ubertooth/issues/464), [#473](https://github.com/SqueakyFloors/ubertooth/issues/473), [#475](https://github.com/SqueakyFloors/ubertooth/issues/475), [#488](https://github.com/SqueakyFloors/ubertooth/issues/488), [#490](https://github.com/SqueakyFloors/ubertooth/issues/490), [#505](https://github.com/SqueakyFloors/ubertooth/issues/505), [#508](https://github.com/SqueakyFloors/ubertooth/issues/508), [#543](https://github.com/SqueakyFloors/ubertooth/issues/543), [#547](https://github.com/SqueakyFloors/ubertooth/issues/547), [#570](https://github.com/SqueakyFloors/ubertooth/issues/570), [#571](https://github.com/SqueakyFloors/ubertooth/issues/571), [#580](https://github.com/SqueakyFloors/ubertooth/issues/580), [#636](https://github.com/SqueakyFloors/ubertooth/issues/636), [#652](https://github.com/SqueakyFloors/ubertooth/issues/652), [#657](https://github.com/SqueakyFloors/ubertooth/issues/657), [#658](https://github.com/SqueakyFloors/ubertooth/issues/658), [#660](https://github.com/SqueakyFloors/ubertooth/issues/660), [#686](https://github.com/SqueakyFloors/ubertooth/issues/686), [#718](https://github.com/SqueakyFloors/ubertooth/issues/718), [#757](https://github.com/SqueakyFloors/ubertooth/issues/757), [#764](https://github.com/SqueakyFloors/ubertooth/issues/764), [#805](https://github.com/SqueakyFloors/ubertooth/issues/805), [#828](https://github.com/SqueakyFloors/ubertooth/issues/828), [#872](https://github.com/SqueakyFloors/ubertooth/issues/872))

#### Captures: destinations, files, and what a capture reports

- Capture destinations are consistent across tools: a file name of `-` (or any
  path to standard output) streams the capture with the report on standard error, PcapNG to standard
  output is refused with advice, and a refused run leaves no empty file.
- **Fixed:** two capture options naming one file, or a replay writing into its
  own dump, silently destroyed the capture (or never ended). Both are refused
  before anything is written; a replay piped into its own dump warns.
- Each record carries the packet's signal power, a background estimate, and
  SNR (or `none`). A run warns, with an end-of-run total, on receiver
  saturation, dropped packets, lost air time, or write failures. Fixes to the
  access-code search, early signal levels, clock trim, and the noise-valid
  flag make the figures accurate.
- Updated libbtbb names each record's logical transport (SCO, eSCO, or ACL),
  removes its unfinished capture file on error, and survives a failed
  allocation. Credit: Hannes Ellinger
  ([@el-han](https://github.com/el-han)) for the per-packet signal figure, and
  [@orgua](https://github.com/orgua) for noting that dBm readings must be
  averaged in power
  ([upstream #240](https://github.com/greatscottgadgets/ubertooth/pull/240)).

([#77](https://github.com/SqueakyFloors/ubertooth/issues/77), [#118](https://github.com/SqueakyFloors/ubertooth/issues/118), [#435](https://github.com/SqueakyFloors/ubertooth/issues/435), [#448](https://github.com/SqueakyFloors/ubertooth/issues/448), [#481](https://github.com/SqueakyFloors/ubertooth/issues/481), [#637](https://github.com/SqueakyFloors/ubertooth/issues/637), [#643](https://github.com/SqueakyFloors/ubertooth/issues/643), [#658](https://github.com/SqueakyFloors/ubertooth/issues/658), [#663](https://github.com/SqueakyFloors/ubertooth/issues/663), [#665](https://github.com/SqueakyFloors/ubertooth/issues/665), [#736](https://github.com/SqueakyFloors/ubertooth/issues/736), [#745](https://github.com/SqueakyFloors/ubertooth/issues/745), [#771](https://github.com/SqueakyFloors/ubertooth/issues/771), [#786](https://github.com/SqueakyFloors/ubertooth/issues/786), [#792](https://github.com/SqueakyFloors/ubertooth/issues/792), [#803](https://github.com/SqueakyFloors/ubertooth/issues/803), [#849](https://github.com/SqueakyFloors/ubertooth/issues/849), [#869](https://github.com/SqueakyFloors/ubertooth/issues/869), [#873](https://github.com/SqueakyFloors/ubertooth/issues/873), [#1040](https://github.com/SqueakyFloors/ubertooth/issues/1040), [#1130](https://github.com/SqueakyFloors/ubertooth/issues/1130), [#1147](https://github.com/SqueakyFloors/ubertooth/issues/1147), [#1149](https://github.com/SqueakyFloors/ubertooth/issues/1149), [#1157](https://github.com/SqueakyFloors/ubertooth/issues/1157), [#1165](https://github.com/SqueakyFloors/ubertooth/issues/1165), [#1146](https://github.com/SqueakyFloors/ubertooth/issues/1146), [#1148](https://github.com/SqueakyFloors/ubertooth/issues/1148))

#### Capture timestamps

- **Fixed:** timestamps jumped at clock rollovers, leapt by hours after a mode
  change, or showed when the host read a packet instead of when it arrived.
  They now run correctly live and on replay, and the per-packet `systime`
  matches the host time in an `ubertooth-rx -d` or `ubertooth-btle -d` dump.

([#202](https://github.com/SqueakyFloors/ubertooth/issues/202), [#218](https://github.com/SqueakyFloors/ubertooth/issues/218), [#229](https://github.com/SqueakyFloors/ubertooth/issues/229), [#465](https://github.com/SqueakyFloors/ubertooth/issues/465), [#478](https://github.com/SqueakyFloors/ubertooth/issues/478), [#492](https://github.com/SqueakyFloors/ubertooth/issues/492), [#589](https://github.com/SqueakyFloors/ubertooth/issues/589), [#612](https://github.com/SqueakyFloors/ubertooth/issues/612))

#### Starting and stopping a capture

- **Script-visible change.** A capture that cannot start, loses its USB
  stream, or cannot end its mode exits 1 and says so, instead of a false 0 or
  a board left unusable. One Ctrl-C ends a run, even during startup; closing
  the terminal ends the board's mode; stops are prompt; and captured data is
  still written.

([#319](https://github.com/SqueakyFloors/ubertooth/issues/319), [#379](https://github.com/SqueakyFloors/ubertooth/issues/379), [#431](https://github.com/SqueakyFloors/ubertooth/issues/431), [#454](https://github.com/SqueakyFloors/ubertooth/issues/454), [#515](https://github.com/SqueakyFloors/ubertooth/issues/515), [#545](https://github.com/SqueakyFloors/ubertooth/issues/545), [#584](https://github.com/SqueakyFloors/ubertooth/issues/584), [#741](https://github.com/SqueakyFloors/ubertooth/issues/741), [#802](https://github.com/SqueakyFloors/ubertooth/issues/802), [#844](https://github.com/SqueakyFloors/ubertooth/issues/844))

#### Generic radio mode

- **Experimental:** generic radio mode captures and sends non-Bluetooth
  2.4 GHz protocols without new firmware. `ubertooth-debug -w` writes CC2400
  registers, `ubertooth-dump -g` captures raw symbols or sync-word-framed
  packets with CRC status, and `ubertooth-tx -g` transmits; see ubertooth(7),
  GENERIC RADIO MODE. Options and output may change.

([#352](https://github.com/SqueakyFloors/ubertooth/issues/352))

### ubertooth-btle

#### New features

- **Save and replay.** `ubertooth-btle -d <file>` saves the board's raw
  records (the `ubertooth-rx -d` format), and `ubertooth-btle -R <file>`
  decodes them again with no board. It is the only way to keep the connection
  parameters promiscuous mode works out.
- **Promiscuous mode finds Bluetooth 5 connections.** `ubertooth-btle -p`
  recovers connections that hop with channel selection algorithm 2 and reports
  the `Algorithm 2 event counter`. It does not learn the channel map, so a
  connection using fewer than 37 channels is captured with gaps.
- **Stream to Wireshark.** `ubertooth-btle -q -` and `ubertooth-btle -c -`
  stream PCAP to standard output with diagnostics kept off it. Credit: Ryan
  Govostes ([@rgov](https://github.com/rgov))
  ([upstream #362](https://github.com/greatscottgadgets/ubertooth/pull/362)).
- **Script-visible change.** The CRC verify setting moves from `-v` to `-k`
  (`ubertooth-btle -k` reads it, `-k 0` and `-k 1` set it), and `-v` is now
  verbose with no value, as in every other tool. `ubertooth-btle -s A -t B`
  advertises from A and follows B (before, the two shared one address), and `-s`
  takes only a full address.
- **Script-visible change.** `ubertooth-btle` refuses, before opening a board
  and with exit 1, two capture modes at once, `-i`/`-I` without `-f` or `-p`,
  `-A` or `-t` with `-p`, `-a` or `-s` with a capture mode, and a command line
  with nothing to do. A capture whose board stops answering exits 1.

([#745](https://github.com/SqueakyFloors/ubertooth/issues/745), [#1025](https://github.com/SqueakyFloors/ubertooth/issues/1025), [#7](https://github.com/SqueakyFloors/ubertooth/issues/7), [#542](https://github.com/SqueakyFloors/ubertooth/issues/542), [#804](https://github.com/SqueakyFloors/ubertooth/issues/804), [#1017](https://github.com/SqueakyFloors/ubertooth/issues/1017), [#383](https://github.com/SqueakyFloors/ubertooth/issues/383), [#779](https://github.com/SqueakyFloors/ubertooth/issues/779), [#481](https://github.com/SqueakyFloors/ubertooth/issues/481), [#237](https://github.com/SqueakyFloors/ubertooth/issues/237), [#299](https://github.com/SqueakyFloors/ubertooth/issues/299), [#309](https://github.com/SqueakyFloors/ubertooth/issues/309), [#308](https://github.com/SqueakyFloors/ubertooth/issues/308), [#330](https://github.com/SqueakyFloors/ubertooth/issues/330), [#346](https://github.com/SqueakyFloors/ubertooth/issues/346), [#347](https://github.com/SqueakyFloors/ubertooth/issues/347), [#258](https://github.com/SqueakyFloors/ubertooth/issues/258))

#### Fixes

- **Follow mode lost connections.** `ubertooth-btle -f` hopped only
  with channel selection algorithm 1, so it caught about one event in ten of a
  Bluetooth 5 connection. It also mistimed connection updates and transmit
  windows, parked on one channel after a missed first event or an empty
  channel map, jumped to another pair's connection, and stayed on a closed
  connection until its timeout. It now captures about 2.5 times as many
  packets.
- **Promiscuous mode stopped short or reported wrong figures.**
  `ubertooth-btle -p` often stalled at the access address on a busy
  connection, never reported the access address it found, reported
  connection intervals that were wrong (it now reports none rather than a
  wrong one), locked onto the advertising address after an earlier `-t`, sent
  its findings into a capture on standard output, and stopped capturing when a
  packet was cut off part-way.
- **Missing packets.** Packets over 50 bytes were cut short (payloads
  up to 251 bytes now arrive whole), the first two packets of a session were
  lost, `ubertooth-btle -f -k 1` dropped every data packet, a single failed
  transfer or late USB poll ended a capture, dropped packets went unreported,
  some advertising packets were recorded under a connection's address, and
  `ubertooth-btle -a` or `-t none` left a filter set for later runs.
- **Wrong signal and timing figures.** The `rssi=` figure is now the
  packet's own level and matches the capture file (before, it could be off by 90 dB),
  and reads `rssi=none` when the packet is too strong to measure. The PCAP
  average RSSI, packet timestamps (before, 8 microseconds late), and `delta_t`
  (before, short by whole clock periods) are now correct.
- **Misleading messages.** A good Wireshark capture no longer ends in
  `Error from extcap pipe`: undecodable packets are counted in one note, with
  detail under `-v`, and no longer flood the terminal. `ubertooth-btle` warns
  that `-i`, `-I`, `-j`, and `-J` do nothing yet, warns when a long PcapNG
  capture runs out of room for connection parameters, validates `-a`, and
  refuses a replay that would write into its own dump.

([#836](https://github.com/SqueakyFloors/ubertooth/issues/836), [#571](https://github.com/SqueakyFloors/ubertooth/issues/571), [#625](https://github.com/SqueakyFloors/ubertooth/issues/625), [#774](https://github.com/SqueakyFloors/ubertooth/issues/774), [#660](https://github.com/SqueakyFloors/ubertooth/issues/660), [#933](https://github.com/SqueakyFloors/ubertooth/issues/933), [#1005](https://github.com/SqueakyFloors/ubertooth/issues/1005), [#1015](https://github.com/SqueakyFloors/ubertooth/issues/1015), [#824](https://github.com/SqueakyFloors/ubertooth/issues/824), [#825](https://github.com/SqueakyFloors/ubertooth/issues/825), [#713](https://github.com/SqueakyFloors/ubertooth/issues/713), [#659](https://github.com/SqueakyFloors/ubertooth/issues/659), [#614](https://github.com/SqueakyFloors/ubertooth/issues/614), [#819](https://github.com/SqueakyFloors/ubertooth/issues/819), [#4](https://github.com/SqueakyFloors/ubertooth/issues/4), [#546](https://github.com/SqueakyFloors/ubertooth/issues/546), [#434](https://github.com/SqueakyFloors/ubertooth/issues/434), [#657](https://github.com/SqueakyFloors/ubertooth/issues/657), [#823](https://github.com/SqueakyFloors/ubertooth/issues/823), [#838](https://github.com/SqueakyFloors/ubertooth/issues/838), [#710](https://github.com/SqueakyFloors/ubertooth/issues/710), [#547](https://github.com/SqueakyFloors/ubertooth/issues/547), [#38](https://github.com/SqueakyFloors/ubertooth/issues/38), [#859](https://github.com/SqueakyFloors/ubertooth/issues/859), [#1194](https://github.com/SqueakyFloors/ubertooth/issues/1194), [#570](https://github.com/SqueakyFloors/ubertooth/issues/570), [#662](https://github.com/SqueakyFloors/ubertooth/issues/662), [#974](https://github.com/SqueakyFloors/ubertooth/issues/974), [#602](https://github.com/SqueakyFloors/ubertooth/issues/602), [#1141](https://github.com/SqueakyFloors/ubertooth/issues/1141), [#771](https://github.com/SqueakyFloors/ubertooth/issues/771), [#871](https://github.com/SqueakyFloors/ubertooth/issues/871), [#171](https://github.com/SqueakyFloors/ubertooth/issues/171), [#265](https://github.com/SqueakyFloors/ubertooth/issues/265))

### btle-extcap.py

- **Each board is its own Wireshark interface.** Interfaces are named by the
  board's serial, so re-plugging cannot move a capture to another board, and
  several boards no longer list as `Ubertooth One 0:` repeated. A Wireshark
  profile that saved an Ubertooth has to pick it again.
- **Fixed: Ubertooth missing from Wireshark.** `btle-extcap.py` refused the
  options Wireshark adds to its calls, so no Ubertooth appeared in the capture
  list. `btle-extcap.py -h` also lists the options the script really takes.
- **Fixed: failures reported as success.** A failing `ubertooth-util`, a
  missing tool, or a closed standard output used to give an empty interface
  list, exit 0, or a traceback. Each is now one `error:` line and exit 1.
- **Fixed: capture filters silently ignored.** A capture filter typed for an
  Ubertooth interface now turns Wireshark's filter box red and says to use a
  display filter (before, the capture ran unfiltered).
- **Script-visible change.** `btle-extcap.py --capture` exits only 0 or 1, and
  stopping it with Ctrl-C, from Wireshark or `tshark`, exits 0.
  `btle-extcap.py --extcap-interfaces` exits 1 if any board could not be
  opened, so Wireshark lists no Ubertooths until that board is free. Unknown
  options and calls with nothing to do get the same error wording as every
  other tool.

([#269](https://github.com/SqueakyFloors/ubertooth/issues/269), [#297](https://github.com/SqueakyFloors/ubertooth/issues/297), [#334](https://github.com/SqueakyFloors/ubertooth/issues/334), [#268](https://github.com/SqueakyFloors/ubertooth/issues/268), [#362](https://github.com/SqueakyFloors/ubertooth/issues/362), [#926](https://github.com/SqueakyFloors/ubertooth/issues/926), [#929](https://github.com/SqueakyFloors/ubertooth/issues/929), [#343](https://github.com/SqueakyFloors/ubertooth/issues/343), [#386](https://github.com/SqueakyFloors/ubertooth/issues/386), [#481](https://github.com/SqueakyFloors/ubertooth/issues/481), [#323](https://github.com/SqueakyFloors/ubertooth/issues/323))

### ubertooth-rx

#### New features

- **Several boards at once.** `ubertooth-rx` reads up to eight boards in one
  run, optionally each on its own channel (for example `ubertooth-rx -U
  0,1,2,3 -c 10,30,50,70 -l e9c5ba`), so it hears as many packets as it has
  boards. The boards pool the channels the piconet is heard on, and pass on
  its UAP once two of them agree; each recovers the clock itself.
- **Script-visible change.** `ubertooth-rx -z` prints its `Survey Results`
  heading on standard error, so a saved survey holds only addresses. A replay
  (`ubertooth-rx -i`) refuses `-c` and `-t` instead of ignoring them, and
  `ubertooth-rx -h` lists `-s` as the default channel sweep rather than as
  deprecated.

([#783](https://github.com/SqueakyFloors/ubertooth/issues/783), [#21](https://github.com/SqueakyFloors/ubertooth/issues/21), [#745](https://github.com/SqueakyFloors/ubertooth/issues/745))

#### Fixes

- **Piconets that adapt their hopping could not be followed.**
  `ubertooth-rx -l <lap> -u <uap>` never locked onto the clock of such a
  piconet, which is what current controllers do. With the updated libbtbb it
  finds the clock and follows.
- **Stuck on a wrong clock.** A follow on a wrong clock used to decode
  nothing for the rest of the run. `ubertooth-rx` now warns and searches
  again when the clock proves wrong or nothing is heard for a minute, keeping
  the address part it found and any UAP given with `-u`.
- **Long packets crashed or decoded garbage.** A packet longer than
  one 400-microsecond bank was decoded from zeros, and `ubertooth-rx -l` crashed with
  `double free or corruption` after about 36 packets. A capture at a high `-e`
  now reports somewhat fewer packets, all of them real.
- **Wrong data in capture records.** Records now carry the packet's
  real header (before, 18 zero bits until the follow began), the recovered
  UAP (including on the packet that settled it and in `ubertooth-rx -z`
  surveys), and the right clock (before, some packets were placed about seven
  minutes ahead). Per-packet times track the run (before, a 15-second follow
  could span 191 seconds), and a packet too strong to measure reads `s=none
  snr=none`. `ubertooth-scan` no longer places packets a slot late.
- **Replays and empty files.** `ubertooth-rx -i` reports the host time
  stored in the capture rather than the replay's, and exits 1 with an error
  for an unreadable file or a cut-off last record (before, exit 0). A run
  refused before it captures anything leaves no empty capture file behind.

([#968](https://github.com/SqueakyFloors/ubertooth/issues/968), [#771](https://github.com/SqueakyFloors/ubertooth/issues/771), [#903](https://github.com/SqueakyFloors/ubertooth/issues/903), [#666](https://github.com/SqueakyFloors/ubertooth/issues/666), [#792](https://github.com/SqueakyFloors/ubertooth/issues/792), [#778](https://github.com/SqueakyFloors/ubertooth/issues/778), [#767](https://github.com/SqueakyFloors/ubertooth/issues/767), [#842](https://github.com/SqueakyFloors/ubertooth/issues/842), [#664](https://github.com/SqueakyFloors/ubertooth/issues/664), [#1182](https://github.com/SqueakyFloors/ubertooth/issues/1182), [#1162](https://github.com/SqueakyFloors/ubertooth/issues/1162), [#202](https://github.com/SqueakyFloors/ubertooth/issues/202), [#203](https://github.com/SqueakyFloors/ubertooth/issues/203), [#993](https://github.com/SqueakyFloors/ubertooth/issues/993), [#1013](https://github.com/SqueakyFloors/ubertooth/issues/1013))

### ubertooth-afh

- **Script-visible change.** `ubertooth-afh -a` is removed.
- **Script-visible change.** The removal threshold `ubertooth-afh -m` now
  defaults to 800 packets (before, 5), so a full-band map holds at 79 of 79;
  a channel the piconet gives up takes 800 packets to leave the map.
- **Fixed: wrong channel maps.** `ubertooth-afh` reported only a handful of
  the channels a full-band piconet uses, marked the two band-edge channels
  used without hearing them, and could park on one channel and report only
  that.
- **Fixed: failures that looked like results.** When the board refuses the
  band sweep, `ubertooth-afh` exits 1 instead of printing a one-channel map,
  and `ubertooth-btle` likewise names the setup request the board refused.
  `ubertooth-afh -t` exits 2 when detection hears nothing, instead of
  monitoring an all-zero map, and stops on the first Ctrl-C.

([#21](https://github.com/SqueakyFloors/ubertooth/issues/21), [#194](https://github.com/SqueakyFloors/ubertooth/issues/194), [#756](https://github.com/SqueakyFloors/ubertooth/issues/756), [#238](https://github.com/SqueakyFloors/ubertooth/issues/238), [#379](https://github.com/SqueakyFloors/ubertooth/issues/379), [#187](https://github.com/SqueakyFloors/ubertooth/issues/187), [#521](https://github.com/SqueakyFloors/ubertooth/issues/521))

### ubertooth-follow

- **Name the target by its full address.** `ubertooth-follow -t <BD_ADDR>`
  follows any target, not only one whose address begins `00:00`, and gives
  the LAP and UAP to follow. A failed connection after `-l` and `-u` asks for
  `-t` instead.
- **Script-visible change.** `ubertooth-follow -a` (follow with an AFH map)
  needs `-t`, and `-l` without `-u`, or `-u` without `-l`, is refused (before,
  it followed the local adapter instead). Progress goes to standard error.
- **Fixed: hopping on the wrong channels.** AFH hopping sat on 2402 MHz; a
  board left in another mode, or by a killed tool, stayed on one channel or
  hopped all 79; and `ubertooth-follow -w` compensated half the delay it
  named. A new AFH map takes effect at once, an unreadable map or one with
  fewer than 20 channels is refused, and a follow without a map clears an
  earlier run's.
- **Fixed: following without what was asked for.** When the board refused the
  target's address or the clock reset, `ubertooth-follow` followed anyway; it
  now stops and says which. A UAP you gave is kept after a wrong clock instead
  of being worked out again from the air.
- **Fixed: timestamps and empty files.** Packet times could be hours out
  after a packet caught around the clock reset, and a run refused before it
  captured anything left an empty capture file.

([#316](https://github.com/SqueakyFloors/ubertooth/issues/316), [#481](https://github.com/SqueakyFloors/ubertooth/issues/481), [#529](https://github.com/SqueakyFloors/ubertooth/issues/529), [#540](https://github.com/SqueakyFloors/ubertooth/issues/540), [#34](https://github.com/SqueakyFloors/ubertooth/issues/34), [#44](https://github.com/SqueakyFloors/ubertooth/issues/44), [#261](https://github.com/SqueakyFloors/ubertooth/issues/261), [#788](https://github.com/SqueakyFloors/ubertooth/issues/788), [#672](https://github.com/SqueakyFloors/ubertooth/issues/672), [#504](https://github.com/SqueakyFloors/ubertooth/issues/504), [#745](https://github.com/SqueakyFloors/ubertooth/issues/745), [#494](https://github.com/SqueakyFloors/ubertooth/issues/494), [#995](https://github.com/SqueakyFloors/ubertooth/issues/995))

### ubertooth-scan

- **Script-visible change.** `ubertooth-scan` keeps its headings, `-x`
  progress, and failures on standard error, so saved or piped output holds
  only results. A refused inquiry, or a device that will not complete the
  `ubertooth-scan -x` report, is named and exits 1 (before, exit 0).
- **Fixed: wrong or stuck device names.** `ubertooth-scan` could print one
  device's name against another's address, and a device that answered and
  then went silent held the scan forever. Unreadable names now print as
  `[unknown]` and the scan carries on.
- **Fixed: newer manufacturers unnamed.** `ubertooth-scan -x` reported a
  device from a recently registered company as `Manufacturer: not assigned`;
  it now knows four times as many company identifiers.

([#323](https://github.com/SqueakyFloors/ubertooth/issues/323), [#1172](https://github.com/SqueakyFloors/ubertooth/issues/1172), [#1090](https://github.com/SqueakyFloors/ubertooth/issues/1090), [#745](https://github.com/SqueakyFloors/ubertooth/issues/745))

### ubertooth-btbr

#### New features

- `ubertooth-btbr` can put packet types on the air that a real stack almost
  never sends, which is what a Classic decoder needs to be tested against.
  With a connection up, `tx <format> [<length>] [<count>] [<hex>]` transmits
  one packet built to that format: AUX1, DV, the HV voice packets, the EV
  synchronous ones, and DM or DH at any length the format carries, including
  the odd and boundary lengths. The data is a counted pattern unless you give
  the bytes, and the line the command prints is what went on the air. A DV
  packet's two fields get patterns that start at different bytes, `a0` for the
  voice field and `00` for the data field, so a decoder can see where the
  voice field ends without knowing how long either field is.
  ubertooth-btbr(1), TRANSMITTING CHOSEN PACKET TYPES has the formats and
  their lengths.
  [#736](https://github.com/SqueakyFloors/ubertooth/issues/736)
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
- **Script-visible change:** `ubertooth-btbr` keeps its prompt and its
  `Type ? to list commands` line out of a capture, so every line of one can
  be read by the time in front of it. The prompt carries no newline of its
  own, so the first packet after each one used to land on its end
  (`bt> 12:14:53 RX FHS: ...`). A session at a terminal is unchanged: the
  terminal's line editor still draws the prompt.
  [#800](https://github.com/SqueakyFloors/ubertooth/issues/800)
- **Script-visible change:** `ubertooth-btbr` prints what `?` and
  `? <command>` list on standard error, with the prompt, so a capture holds
  packets alone whatever the user types during it. Typing `?` with the capture
  piped somewhere used to put untimed lines into it. A `? <command>` naming no
  command is now the `error: no help on '<word>'` line the prompt's other
  refusals read like, in place of `*** No help on <word>`.
  [#874](https://github.com/SqueakyFloors/ubertooth/issues/874)
- **Script-visible change:** `ubertooth-btbr` refuses a word its prompt does
  not know, `error: no such command '<word>'` on standard error with the
  prompt still up, where such a word used to be taken silently. `x` and `q`
  with a word after them leave, as `exit <word>` already did.
  [#908](https://github.com/SqueakyFloors/ubertooth/issues/908)

#### Fixes

- `ubertooth-btbr` no longer reports standard output gone when the reader
  left before any packet was printed: nothing had been written to that
  stream, so nothing asked for was lost and the session ends as it otherwise
  would. The prompt used to be written there, which is what noticed.
  [#800](https://github.com/SqueakyFloors/ubertooth/issues/800)
- Fixed: `ubertooth-btbr`'s `page_scan` reported a connection it never made.
  Where the board could not put its answer to the page on the air, it still
  printed the pager's address and `Peripheral started` and told the host it
  was connected, while the pager had heard nothing and was still paging. The
  board now says the answer was not sent and goes on scanning, so it is
  listening for the pager's next attempt. `inquiry_scan` had a smaller
  version of it: it sent the extended inquiry response after an inquiry
  response that never went out, and now goes back to scanning.
  [#1156](https://github.com/SqueakyFloors/ubertooth/issues/1156)
- Fixed: `ubertooth-btbr` took any `-f` or `-b` value up to 65535 and the
  board then used the low bits of it, so `-f 64` set the same offset as
  `-f 0` and `-b 4` allowed no access-code bit errors at all, the opposite of
  raising the tolerance. Each is now refused with its real range,
  `error: -f must be 0-63 (got 64)` and `error: -b must be 0-3 (got 4)`,
  before a board is opened.
  [#1142](https://github.com/SqueakyFloors/ubertooth/issues/1142)
- Fixed: `ubertooth-btbr`'s `page` and `l2ping` held a connection the peer had
  already declined, until link supervision ended it a second later. A peer
  that refuses the connection request now gets a detach, and the command says
  so and ends straight away.
  [#1074](https://github.com/SqueakyFloors/ubertooth/issues/1074)
- `ubertooth-btbr` carries on when a device name cannot be written in the
  session's character encoding. A name arrives as whatever bytes the air
  carried, and one that is not ASCII ended the command in
  `BTCtlInquiryCmd stopped: UnicodeEncodeError` on a session not running under
  UTF-8. That line is dropped now, with one warning saying why and naming
  `PYTHONIOENCODING=utf-8`, and the packets after it are printed as usual. A
  diagnostic the encoding could not take used to silence standard error for
  the rest of the session.
- Fixed: `ubertooth-btbr`'s `l2ping` never reached its L2CAP packet when the
  device it paged was another Ubertooth. The two boards connected and then sat
  there, because each side sent `LMP_setup_complete` only on receiving one, so
  neither sent the first and the link setup never finished. The central now
  sends its own as soon as its connection request is accepted, and a board in
  `page_scan`, which `discoverable` also runs, answers an L2CAP echo request
  small enough to reply to in the packets this link sends, so `l2ping` between
  two boards sees a reply. A request larger than that is not
  answered; ubertooth-btbr(1), LIMITATIONS says how large it may be.
  [#807](https://github.com/SqueakyFloors/ubertooth/issues/807)
- `ubertooth-btbr`'s `page`, `l2ping`, `monitor`, and `bdaddr` refuse an
  address that is not six bytes, where a short one used to be acted on as a
  different one: `bdaddr 11:22:33` gave the board `00:00:11:22:33` and `page
  11:22:33` paged that device, with nothing said. An inquiry's `bdaddr=` keeps
  its leading zeros too, so the address it prints is one the prompt takes.
  [#1075](https://github.com/SqueakyFloors/ubertooth/issues/1075)
- Fixed: a `ubertooth-btbr` `page`, `l2ping`, or `page_scan` session gave up
  one second after a connection that came to nothing, with most of the time
  the user asked for unused. A pager cannot tell the ID packet that
  acknowledges its page response from the one that answers a page message --
  both are a bare access code -- so anything else answering a page nearby puts
  it in a connection with no peer on it; and a board that has answered a page
  response the pager did not hear sits in a connection the pager has already
  left. Either way link supervision ended that connection a second later and
  the command stopped, 44 seconds into a 45-second page attempt or a few
  seconds into a minute of scanning. Both sides now go back to the page train
  or the page scan they came from and keep the deadline of the attempt that
  was running, so a page that is answered by the wrong board, or a page
  response whose acknowledgment is lost, costs a second rather than the whole
  attempt. The board says which it did, as `Paging resumed` or `Page scan
  resumed`.
  [#807](https://github.com/SqueakyFloors/ubertooth/issues/807)
- `ubertooth-btbr` writes plain text when its standard error is redirected,
  where it used to send ANSI color escapes whichever way the stream went, so a
  saved session was full of `^[[32;1m`. Its finer-grained progress lines
  (`Send idle`, `send bb ...`) no longer appear in a normal session, which
  leaves standard error carrying progress only, and the LMP packet lines space
  their bytes (`02 00 00`) like the ACL and inquiry lines beside them.
- `ubertooth-btbr` no longer reports a packet whose header came from a piconet
  with a different UAP. All eight bits of a header's error check now have to
  match, where seven did, and the dropped bit cost real coverage: the check is
  computed from the UAP, so every header of one particular other UAP that got
  past the access-code match was accepted, and a header read out of noise
  passed one time in 128 instead of one in 256. Either was printed as a packet
  with whatever type, logical transport address and flags it carried.
  [#1036](https://github.com/SqueakyFloors/ubertooth/issues/1036)
- A `ubertooth-btbr` connection to a real Classic device is no longer cut
  short after exactly 30 seconds. The peer asks how many slots its packets
  may use, and a request like that has to be answered; getting nothing back,
  it waited out its 30-second LMP response timeout and detached the link with
  `LMP_DETACH | 0f22`. The answer holds the link to single-slot packets,
  which is where a connection starts anyway, and the feature mask the tool
  sends no longer offers 3-slot and 5-slot packets it goes on to refuse.
  [#942](https://github.com/SqueakyFloors/ubertooth/issues/942)
- A `ubertooth-btbr` connection says why a peer hung up -- `Peer detached:
  reason 0x13, the peer's user ended the connection` -- and ends the link
  there. As the central it used to call the detach an unhandled opcode and
  hold the connection open until link supervision ended it.
  [#951](https://github.com/SqueakyFloors/ubertooth/issues/951)
- A `ubertooth-btbr` connection answers an LMP request it has no handler for,
  rather than leaving the peer to wait out a 30-second response timeout and
  drop the link. A PDU that expects no reply is ignored instead, so the
  answers go only where one is due: against a peer that asked for less
  transmit power five times a second, that was 302 refusals in a connection
  carrying 18987 packets, and the link was no shorter for them. The
  `warning: LMP opcode ... is not handled` line is now said once per opcode
  for the life of a connection, not once per PDU.
  [#949](https://github.com/SqueakyFloors/ubertooth/issues/949)
- A `ubertooth-btbr` capture names LMP opcodes 31 and 32 as the current
  specification does, `LMP_INCR_POWER_REQ` and `LMP_DECR_POWER_REQ`; the
  extended pair 127/31 and 127/32 carries `LMP_POWER_CONTROL_REQ` and
  `LMP_POWER_CONTROL_RES`. Each table held the other's names, so a peer asking
  for less transmit power read as a response to something else. An opcode no
  table names is now its number rather than `None`.
  [#950](https://github.com/SqueakyFloors/ubertooth/issues/950)
- A `ubertooth-btbr` session carries on through an LMP PDU too short to carry
  an opcode, reporting it in a `warning:` line. One used to end the session in
  `error: BTCtlPagingCmd stopped: IndexError: index out of range` and take the
  capture with it.
  [#960](https://github.com/SqueakyFloors/ubertooth/issues/960)
- A `ubertooth-btbr` session answers an LMP PDU that holds its opcode but ends
  before its parameters: the name request, the extended features request, and
  the AFH set are refused with invalid LMP parameters, and a short slot offset
  is traced and let go. `monitor` now says it is keeping the channel map it
  has rather than hopping over one the central never sent. The shortest of
  these PDUs used to end the session in `error: BTCtlPagingCmd stopped:
  IndexError: index out of range`, and the rest were read as though the
  missing bytes were there.
  [#971](https://github.com/SqueakyFloors/ubertooth/issues/971)
- Fixed: a message from the board that `ubertooth-btbr` could not read, or an
  inquiry response the air cut short, ended the thread reading the board or
  the running command in `IndexError: index out of range`, and with it the
  rest of the session. Such a message is now dropped with a `warning:` line
  naming what arrived, said once per reason, and the session keeps capturing.
  [#978](https://github.com/SqueakyFloors/ubertooth/issues/978)
- Fixed: an inquiry response whose extended inquiry response was padded with
  zeros after its last field was reported as a malformed EIR. A zero length
  byte is where the significant part ends, so `ubertooth-btbr` now takes the
  padding for the end of the response and says nothing.
  [#988](https://github.com/SqueakyFloors/ubertooth/issues/988)
- A `ubertooth-btbr` connection goes on when the peer refuses one of the five
  requests it opens with: the refusal is reported with the reason the peer
  gave, and the next request goes out. A refusal used to leave the session
  asking nothing further, so the link sat there until link supervision ended
  it with no line saying why.
  [#957](https://github.com/SqueakyFloors/ubertooth/issues/957)
- `ubertooth-btbr` refuses a session, and `-V`, whose standard output was not
  opened to be written: one line, no board opened, exit 1. On some builds of
  Python a whole session used to run with its capture going into a descriptor
  that was not its standard output.
  [#959](https://github.com/SqueakyFloors/ubertooth/issues/959)
- A `ubertooth-btbr` session is text. It used to print one NUL per progress
  line, the C string terminator the board's console puts on the end of each
  one, which `grep` on a saved session answers with `Binary file matches`
  instead of the lines asked for.
  [#943](https://github.com/SqueakyFloors/ubertooth/issues/943)
- A `ubertooth-btbr` session whose standard output is a descriptor that
  cannot be written, which is what `ubertooth-btbr 1<&0` leaves, reports the
  lost capture in the one `error: standard output has gone away` line. The
  first packet used to end the command with a second line naming the errno,
  as though the command itself had failed.
  [#920](https://github.com/SqueakyFloors/ubertooth/issues/920)
- A board running `ubertooth-btbr`'s `page_scan` completes the connection for
  a page it hears in either half of the pager's transmit slot. A pager sends
  two page messages per slot and its answering FHS packet comes a fixed time
  after the first of them, so a board that answered the second one waited for
  that packet 312.5 us too late and the page went nowhere: about half of all
  pages were answered and then led to nothing, in runs lasting a whole
  1.28-second page interval. An empty window is now re-read as a page heard
  second, and the board waits for the pager's next FHS 1.25 ms later. This is
  what `page`, `page_scan` and `l2ping` between two boards needed.
  [#807](https://github.com/SqueakyFloors/ubertooth/issues/807)
- A board in `ubertooth-btbr`'s `page_scan` gave up on the pager's page
  response after two windows, so one lost packet ended the page and no
  connection was made. It now waits through pagerespTO, which gives a page
  response that goes missing once a second chance instead of none.
  [#807](https://github.com/SqueakyFloors/ubertooth/issues/807)
- `ubertooth-btbr -V` and `-h` answer on a host where the Python `libusb1`
  package is not installed, instead of ending in a `ModuleNotFoundError`
  traceback: the command line is now read before the `usb1` module is
  imported. `-V` is how a user says which tools they have, so it is the call
  that has to work when something is missing. A call that opens a board still
  needs the package and says so in one line.
  [#898](https://github.com/SqueakyFloors/ubertooth/issues/898)
- **Script-visible change:** `ubertooth-btbr` exits 1 when the thread reading
  the board stopped on a failure, for example because the board's hub port was
  power-cycled during an `inquiry`. Nothing restarts that thread, so no
  message arrives for the rest of the session; it reported every step
  correctly and then exited 0, so a script could not tell such a run from one
  where every command worked. A single command the board refused still leaves
  the prompt up and does not change the status.
  [#737](https://github.com/SqueakyFloors/ubertooth/issues/737)
- `ubertooth-btbr` reports a board another program is using as that, in the
  words every other tool uses: `error: Ubertooth <serial> is in use by another
  program`, with the libusb code the system gives. It used to report it as a
  board that took none of the session's requests, with a note about firmware
  that had nothing to do with it.
  [#701](https://github.com/SqueakyFloors/ubertooth/issues/701)
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
- `ubertooth-btbr` no longer starts a session on an arbitrary board. With
  several attached it used to open whichever one enumerated first, so a
  session could reach a board still carrying `bluetooth_rxtx` and report its
  write timeout as a failure of the board the user had just flashed with
  `btbr`. It now lists the attached boards and opens none until `-U` names
  one, and a failed setup names the board it was talking to.
  [#642](https://github.com/SqueakyFloors/ubertooth/issues/642)
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

- Fixed: `ubertooth-btbr`'s `page` reported a connection when its own page
  response never went on the air. The packet that acknowledges a page response
  is a bare access code with nothing in it to say who sent it, so the only
  thing that makes one an answer to this board is that the page response went
  out. Where the board could not put it there, any other board answering a
  page nearby was taken for the paged device, and `page` printed `ID(3)`,
  `Central started`, and told the host it was connected to a device that had
  heard nothing, until link supervision ended the connection a second later.
  The board now repeats the page response instead, for as long as the page
  response timeout allows, and then goes back to paging. This is the pager's
  half of the `page_scan` fix above.
  [#1168](https://github.com/SqueakyFloors/ubertooth/issues/1168)

- Fixed: a `btbr` transmit slot that put nothing on the air cost the link a
  repeat and an acknowledgment. The slot is composed before the radio is asked
  for it, so a `tx` asked for with a count of N went out N-1 times when one of
  its slots was dropped, and the peer, left unacknowledged for the packet it
  had just sent, repeated that packet and cost the link another slot. The
  transmit task now reports whether the slot reached the air, and both the
  repeat and the acknowledgment go back to the link for the next transmit slot
  to carry.
  [#1166](https://github.com/SqueakyFloors/ubertooth/issues/1166)

- Fixed: a `btbr` `page` could run on with nothing to show for it. A transmit
  slot the board had no room left to compose at all did not move its hopping
  on, while the device being paged moved its own on for the window it had
  opened. The two stayed one frequency apart for the rest of the attempt, so
  every packet after that went out where the paged device had just stopped
  listening, and the page found nothing until the train started again. A
  dropped slot now moves the hopping on as a slot that went out does, in an
  `inquiry` sweep as well as in a page.
  [#1191](https://github.com/SqueakyFloors/ubertooth/issues/1191)

- A `page` the board has to give up on because it has no slot left to listen
  for the acknowledgment now says so once its page response has gone out,
  rather than while the radio still holds it. A new `page` asked for in that
  gap, up to a couple of slots of it, would have stopped the board.
  [#1168](https://github.com/SqueakyFloors/ubertooth/issues/1168)


### ubertooth-specan

#### New features

- **Script-visible change:** `ubertooth-specan -d` now only chooses where the
  samples go, and `-g`/`-G` how they are written, so `-g -d out.dat` writes
  the gnuplot text into the file the man page always promised. `-d` used to
  create the file and leave it empty while a later `-g` took the samples to
  the screen.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- `ubertooth-specan -v` now prints what its page says it does, on standard
  error: the board, the sweep, where the samples go, and a summary once a
  second. It used to do nothing. A second `-v` is refused.
  [#510](https://github.com/SqueakyFloors/ubertooth/issues/510)
- `ubertooth-specan` reports a bad option itself, as the other tools do:
  `error: unrecognized option '-Z'` and `error: option '-l' requires a value`,
  instead of the option parser's own lines naming the whole path of the binary.
  [#338](https://github.com/SqueakyFloors/ubertooth/issues/338)

#### Fixes

- `ubertooth-specan` checks every write of its text formats, so a sweep into
  a `-d` file that cannot be written ends where the writing failed and names
  it: `error: writing samples failed: could not write the sample (No space
  left on device)`. Only the raw records were checked, so a text sweep ran to
  the end and reported the vaguer `could not flush the samples`.
  [#655](https://github.com/SqueakyFloors/ubertooth/issues/655)
- A spectrum analyzer run measures through the radio front end a freshly
  booted board has, whichever mode ran before it. `ubertooth-specan` and
  `ubertooth-util -q` do not set the radio's manual signal overrides
  themselves, and nothing put them back when a mode ended, so a sweep after a
  low-energy capture was taken through that capture's front end and a sweep
  after a reset was not. The write that was meant to set them at boot is gone
  as well: the radio reset a few microseconds later undid it, so it never
  reached a mode.
  [#471](https://github.com/SqueakyFloors/ubertooth/issues/471)
- `ubertooth-specan -d` takes a bare `-` as standard output and every other
  value as a file name, including one starting with `-`, so `-d -x` writes the
  file `-x` instead of streaming binary samples to the terminal.
  [#231](https://github.com/SqueakyFloors/ubertooth/issues/231)
- `ubertooth-specan -d -` refuses a terminal, as `ubertooth-btle -q -` already
  did: `error: stdout is a tty, refusing to write binary samples to it`, exit
  status 1. A pipe or a redirect is unaffected, and btle's own refusal now
  reads the same way.
  [#259](https://github.com/SqueakyFloors/ubertooth/issues/259)
- `ubertooth-specan -l`/`-u` now require valid, ordered in-band frequencies
  before opening the device.
  [#37](https://github.com/SqueakyFloors/ubertooth/issues/37)
- `ubertooth-specan` stops any mode left running before it starts and drops
  samples outside `-l`/`-u` left over from an earlier run; tools also stop the
  board cleanly on SIGHUP and SIGALRM (and exit 0, as for SIGINT/SIGTERM).
  [#173](https://github.com/SqueakyFloors/ubertooth/issues/173)

### ubertooth-specan-ui

#### New features

- `ubertooth-specan-ui` reports a missing `numpy` or Qt binding as one
  `error: ` line with a note saying what to install, rather than ending in a
  traceback, which is what `ubertooth-btbr` has done since its own fix. It also
  reads `-l` and `-u` itself, so a value that is not a number and one outside
  the band get the line `ubertooth-specan` gives for both -- `error: lower
  frequency must be 2402-2480 MHz (got abc)` -- in place of the argument
  parser's `invalid int value`.
- **Script-visible change:** `ubertooth-specan-ui` refuses a command line it
  cannot read instead of running it, as `error: unrecognized option
  '--bogus'` or `error: unexpected argument 'extra'` on standard error with
  exit 1. A mistyped option used to be dropped without a word and the window
  opened and swept; a bad value ended the command in 2 with the usage text.
  [#934](https://github.com/SqueakyFloors/ubertooth/issues/934)
- `ubertooth-specan-ui` draws a slowly smoothed average of the max-hold trace
  as a cyan dotted line labeled "Avg: N dBm", shown with the other markers.
  Credit: Sirio Balmelli ([@siriobalmelli](https://github.com/siriobalmelli)).
  [#75](https://github.com/SqueakyFloors/ubertooth/issues/75)
  https://github.com/siriobalmelli-foss/ubertooth/commit/01c3bf9e
- `ubertooth-specan-ui` shows its keys on screen (press `h`; a hint is always
  visible), labels frequencies in whole MHz, no longer leaves ghost marker
  labels behind, draws the plot slightly wider than the scanned range, and has
  a man page and usage guide.
  [#125](https://github.com/SqueakyFloors/ubertooth/issues/125)
  [#126](https://github.com/SqueakyFloors/ubertooth/issues/126)
- `ubertooth-specan-ui` can label its axis and its markers with BLE channel
  numbers instead of megahertz. Press `n` in the window for the channel
  numbers -- the advertising channels 37, 38, and 39, and the data channels 0
  to 36 -- and `n` again for the megahertz the window still starts in. In
  channel mode the axis is marked on the channels themselves, so a line on the
  graph lines up with the number under it, the three advertising channels are
  always named when they are in the plot, and a marker names the channel
  nearest the point it sits on. The difference between two markers is in
  megahertz in both modes, and says so. The right end of the axis reads `MHz`
  or `BLE ch`, since the tick labels themselves are bare numbers either way.
  Credit: Emmanuel Blot ([@eblot](https://github.com/eblot)) for the channel
  labels and the mapping behind them.
  [#76](https://github.com/SqueakyFloors/ubertooth/issues/76),
  [#745](https://github.com/SqueakyFloors/ubertooth/issues/745)

#### Fixes

- `ubertooth-specan-ui --wifi 14` places the channel on 2484 MHz, where it is,
  rather than 2482, so the band it sweeps for that channel, and where a signal
  in it lands on the plot, are no longer 2 MHz low.
  [#673](https://github.com/SqueakyFloors/ubertooth/issues/673)
- `ubertooth-specan-ui` plots again: it passed `-U 0` to `ubertooth-specan` as a
  single argument, which was read as a serial, and asked for 2400-2483 MHz,
  which `ubertooth-specan` rejects. Its `-U` now takes an index or a serial like
  the other tools, and it no longer crashes when `C` is pressed before data
  arrives.
  [#124](https://github.com/SqueakyFloors/ubertooth/issues/124)

### ubertooth-dump

#### New features

- **Script-visible change:** `ubertooth-dump` refuses a terminal on standard
  output, with or without `-d -`, instead of filling it with the binary symbol
  stream: `error: stdout is a tty, refusing to write binary dump data to it`.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)

#### Fixes

- `ubertooth-dump -b` refuses a terminal for what it really writes: `error:
  stdout is a tty, refusing to write the bitstream dump to it`. That stream
  is a `0` or a `1` per symbol, a line per captured block, and the refusal
  it shares with the binary captures used to call it binary.
- `ubertooth-dump` ends the run when the board will not take the modulation
  `-c` or `-l` asked for: `error: modulation set failed` and exit 1, with no
  capture started. The request's result used to be dropped, so a board left in
  another modulation was dumped as it stood -- `-l` on a board still set to
  Classic gave Classic symbols and exit 0, with nothing in the output to say
  which modulation produced it.
  [#945](https://github.com/SqueakyFloors/ubertooth/issues/945)

### ubertooth-debug

#### Fixes

- `ubertooth-debug` checks the firmware image too, so a board carrying
  `btbr` is refused by name before a register is read, with the same
  `note:` line that flashes `bluetooth_rxtx`. That image answers no register
  read, so the command used to fail on the read itself and report a refused
  request instead of the firmware. The firmware version is still not checked:
  reading CC2400 registers is debugging work on whatever revision a board
  carries.
  [#808](https://github.com/SqueakyFloors/ubertooth/issues/808)
- `ubertooth-debug` prints each CC2400 field's whole value. Fourteen fields
  were read with a mask narrower than the field the dump draws beside them, so
  a field holding 64 read as 0, and two more took their value from a
  neighbour's bits: `FSMTC`'s `TC_TXEND2PAOFF` drew the field above it, and
  `FSTST3`'s `PD_DELAY` reported `CHP_DISABLE`. Register `0x15` also answers to
  `%MDMTST1` now; it carried `0x14`'s name, so no name of its own reached it.
  [#671](https://github.com/SqueakyFloors/ubertooth/issues/671)
- `ubertooth-debug -r` states the register range it accepts and quotes the
  item it refused, `error: register address must be 0x00-0xff (got 300 in
  0x02,300)`. A `%name` works anywhere in a list or range, not only last.
  [#235](https://github.com/SqueakyFloors/ubertooth/issues/235)
- `ubertooth-debug` exits 0 only when every register read succeeds (it used to
  exit with the last register's value) and names each register that fails.
  [#164](https://github.com/SqueakyFloors/ubertooth/issues/164)

### ubertooth-util

#### New features

- `ubertooth-util -B` says which bootloader a board has, which nothing used to
  report. One built from this tree reports its version and revision; every
  board as shipped reports no version block, and a hash of the whole
  bootloader, which is what tells two such bootloaders apart.
  [#1056](https://github.com/SqueakyFloors/ubertooth/issues/1056)
- **Script-visible change:** `ubertooth-util -n` now runs the range test
  instead of only starting it: progress while it waits, then the result block
  `-m` prints. Nothing answering within 10 seconds (`-n <seconds>` to choose
  that wait, `-n 0` to wait forever) stops the test and exits 1.
  [#649](https://github.com/SqueakyFloors/ubertooth/issues/649)
- **Script-visible change:** `ubertooth-util -U <index|serial> -N` and
  `-U all -N` are refused: `-N` counts the boards on the USB bus and asks
  nothing of the board a selector names. Both used to print the count.
  [#481](https://github.com/SqueakyFloors/ubertooth/issues/481)
- **Script-visible change:** `ubertooth-util -9` is gone, with the two
  developer scratch requests it sent, which the firmware no longer answers.
  [#21](https://github.com/SqueakyFloors/ubertooth/issues/21)
- **Script-visible change:** `ubertooth-util -f` is gone; use `ubertooth-dfu`
  to reflash a board.
  [#333](https://github.com/SqueakyFloors/ubertooth/issues/333)
- **Script-visible change:** `ubertooth-util -N` prints the device count and
  then runs the rest of the command, instead of dropping it.
  [#332](https://github.com/SqueakyFloors/ubertooth/issues/332)
- **Script-visible change:** `ubertooth-util` refuses a command line that asks
  for nothing, `-U 0` on its own included: it used to open the board, do
  nothing and exit 0.
  [#367](https://github.com/SqueakyFloors/ubertooth/issues/367)
- **Script-visible change:** `ubertooth-util -m` reports a range test whose
  packets came back corrupted as `error: invalid range test: mismatch on byte
  <n>` on standard error and exits 1; it was a line on standard output with
  exit 0.
  [#338](https://github.com/SqueakyFloors/ubertooth/issues/338)
- `ubertooth-util -I` blinks the LEDs faster for easier
  identification, and now runs the blink on the board like every other mode, so
  the command returns at once and the board stays reachable; `-S` or any mode
  option stops it and leaves the LEDs off.
  [#167](https://github.com/SqueakyFloors/ubertooth/issues/167)
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
- `ubertooth-util -v` is deprecated in favor of `-V`. It keeps printing the
  `Firmware version:` line alone, unchanged and with no warning, so scripts
  that parse it are undisturbed.
  [#542](https://github.com/SqueakyFloors/ubertooth/issues/542)

#### Fixes

- Fixed: `ubertooth-util -n` printed its `waiting for the repeater... (<n> s)`
  progress on standard output, so a script reading `-n` for the range test
  result was handed those lines along with the result whenever the repeater
  took more than a second to answer. The progress now goes to standard error,
  where `Starting range test` and the run's error lines already were, and
  standard output carries the four result lines alone.
  [#1026](https://github.com/SqueakyFloors/ubertooth/issues/1026)
- The two-board range test (`ubertooth-util -n` against a `-e` repeater)
  ignores traffic that is not its own exchange instead of ending the run on
  it. The sender took any packet the radio's CRC accepted as the repeater's
  reply, so a second pair testing on the same channel, or a stray packet,
  ended the run as `error: invalid range test: mismatch on byte <n>`. The
  sender now keeps listening and names what it discarded once per run on
  standard error, `warning: range test: a packet that is not this board's
  reply was ignored (first difference at byte <n>)`, and the repeater answers
  only requests, so two repeaters cannot answer each other. A reply that is
  this board's but arrived corrupted is still reported as a mismatch.
  [#784](https://github.com/SqueakyFloors/ubertooth/issues/784)
- **Script-visible change:** `ubertooth-util -n` interrupted with Ctrl-C stops
  the range test, like every other ending of the wait, and exits 1 with
  `error: interrupted before the range test finished; the range test has been
  stopped`. A signal used to leave the board listening for a reply forever,
  until someone ran `-S`.
  [#694](https://github.com/SqueakyFloors/ubertooth/issues/694)
- The two-board range test reports a result every run, instead of
  `invalid range test: mismatch on byte 0` every five to ten runs. Both boards
  started reading a packet out of the radio the moment it reported the sync
  word, which is before the radio has written the first byte, so byte 0 came
  back wrong -- and only byte 0, because the read trails the air from there on.
  Each now waits for the radio to have the start of the packet.
  [#686](https://github.com/SqueakyFloors/ubertooth/issues/686)
- `ubertooth-util -i` is reported as the success it is. The board used to go
  off the bus from inside the request handler, throwing away the answer the
  host was waiting for, so a board that had done exactly what was asked was
  reported as one that had timed out (`libusb -7`) and the command exited 1.
  Both images now answer the host first. The command also says, in a note of
  its own, that the board stays in its programmer and answers nothing else
  until its power is cut.
  [#508](https://github.com/SqueakyFloors/ubertooth/issues/508)
- `ubertooth-util -I` says what every other tool says about firmware it was not
  built with, so it no longer tells you to flash older firmware over a board
  that is the newer of the two.
  [#635](https://github.com/SqueakyFloors/ubertooth/issues/635)
- `ubertooth-util -r` no longer reports an older range test's numbers as the
  current test's. A test that found no reply, or that the host stopped, now
  leaves no result, and the command says so.
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
- A set and a read of the same `ubertooth-util` value in one command both run,
  the set first, so `-c2426 -c`, `-a5 -a`, `-l1 -l` and `-d1 -d` report the new
  value instead of dropping the set.
  [#228](https://github.com/SqueakyFloors/ubertooth/issues/228)
- `ubertooth-util -m` says `no range test result yet` when no range test has
  completed since the board started, instead of `invalid range test result`.
  [#190](https://github.com/SqueakyFloors/ubertooth/issues/190)
- `ubertooth-util -q <N>` (LED spectrum analyzer, threshold -N dBm) takes
  20-120, the radio's RSSI range, and bare `-q` uses -85 dBm. The old default
  (225) and `-q183` to `-q225` lit no LED at all; the firmware refuses values
  outside 20-120.
  [#189](https://github.com/SqueakyFloors/ubertooth/issues/189)
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
  `ubertooth_count()` no longer leaves libusb initialized.
  [#94](https://github.com/SqueakyFloors/ubertooth/issues/94)
- `ubertooth-util -r` reads the board's serial before the reset and waits for
  that serial, so later options in the same command (`-U0 -r -b`) stay on the
  board that was reset even if the index order changed across the reset. That
  read is quiet, through libubertooth's new `ubertooth_read_serial_string()`:
  a board that has stopped answering no longer prints a transfer error, or
  costs a second of timeout, on the command used to recover it.
  [#139](https://github.com/SqueakyFloors/ubertooth/issues/139)
  [#306](https://github.com/SqueakyFloors/ubertooth/issues/306)
- `ubertooth-util` refuses option values that are not a whole number in range
  (`-l`, `-d`, `-c`, `-C`, `-q`, `-z`); `-Cabc` used to set 2402 MHz, and
  `-q0` (once "the default") is now an error: use bare `-q`.
  [#172](https://github.com/SqueakyFloors/ubertooth/issues/172)
- `ubertooth-util -a0` sets PA level 0 instead of reading the level, and a
  level outside 0-7 is refused before opening the device.
  [#161](https://github.com/SqueakyFloors/ubertooth/issues/161)
- `ubertooth-util` refuses `-z` combined with a mode option, where it silently
  had no effect: a mode stops the device, which resets the squelch.
  [#159](https://github.com/SqueakyFloors/ubertooth/issues/159)
- `ubertooth-util` refuses a word that is not an option, such as `- V`, with an
  error and exit 1 before opening the device, instead of ignoring it and
  running the rest of the command. Option values may now be the next word:
  `-c 2440` sets the channel like `-c2440` (it used to read the channel).
  [#146](https://github.com/SqueakyFloors/ubertooth/issues/146)
- `ubertooth-util` refuses two or more mode options (`-q`, `-n`, `-e`, `-t`,
  `-x`, `-I`, `-i`, `-f`) with an error and exit 1 before opening the device,
  instead of running only one of them and exiting 0.
  [#82](https://github.com/SqueakyFloors/ubertooth/issues/82)

### ubertooth-dfu

#### New features

- The note about a board in the DFU bootloader now names what reaches it,
  `ubertooth-dfu` with no `-U`: `-d <image> -r` writes an image and asks the
  board to start it, and `-r` alone asks without writing. The note no longer
  says that no tool can open such a board, since that one can.
  [#242](https://github.com/SqueakyFloors/ubertooth/issues/242)
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
- `ubertooth-dfu --update-bootloader` now leaves a board that already has the
  bootloader the image carries alone. It asks the running firmware for the hash
  `ubertooth-util -B` reports and writes nothing when it matches the image's,
  so the board keeps the firmware the updater image would have replaced.
  `--force` writes without asking.
  [#1123](https://github.com/SqueakyFloors/ubertooth/issues/1123)

#### Fixes

- Fixed: `ubertooth-dfu -u /dev/stdout` answered with an errno about a file,
  `error: /dev/stdout: Operation not permitted`, because only the lone `-`
  counted as standard output. The names of that stream are now refused while
  the command line is read, with the line the lone `-` already got, which says
  to give a file name instead.
  [#1170](https://github.com/SqueakyFloors/ubertooth/issues/1170)
- Fixed: `ubertooth-dfu --update-bootloader` reported success whenever the
  board came back in DFU, which is also what a board whose bootloader entry
  pin is bridged does without ever starting the updater, so a run that
  installed nothing read like one that worked. The command now reads the
  bootloader region before the write and again afterwards, and says what the
  board holds: an installed bootloader is reported with the hash
  `ubertooth-util -B` will give, a region that already held it says nothing was
  written, and a region that provably is not the one asked for is an error and
  exit 1, whether it did not change at all or changed into neither bootloader.
  Where the reads cannot settle it, the run says so and claims nothing either
  way.
  [#1128](https://github.com/SqueakyFloors/ubertooth/issues/1128)
- Fixed: when `ubertooth-dfu -r` left a board in the bootloader, the notes
  saying why named two of the three reasons the bootloader runs. All three are
  now named, and a run that wrote no image is told to write a whole one, which
  is what a board left half-written by an interrupted download needs.
  [#1133](https://github.com/SqueakyFloors/ubertooth/issues/1133)
- Fixed: a board carrying a bootloader built from this tree could not be
  flashed over USB at all. `ubertooth-dfu` failed on the first firmware block
  and left the board in the bootloader with no firmware. The boot ROM routine
  that copies RAM into flash refuses a source address off a word boundary, and
  the buffer each download block is programmed out of had moved off one; it is
  now aligned, and such a board flashes and verifies. A board carrying the
  original bootloader was never affected. Installing this bootloader still
  needs an ISP session, since the bootloader cannot replace itself over USB.
  [#491](https://github.com/SqueakyFloors/ubertooth/issues/491)
- A firmware download that stops part way through -- a cable pulled, a board
  that leaves the bus, a host that dies -- no longer leaves an image a board
  will start. `ubertooth-dfu -d` writes the vector table last, leaving erased
  flash in its place until then, and the bootloader refuses to start an
  application that is not there: it waits in DFU instead, so the next
  `ubertooth-dfu -d` recovers the board rather than someone at the bench with
  the entry pin bridged. The tool fills in the checksum the part reserves over
  that vector table and the bootloader checks it, so a board that lost power
  part way through programming that one block is not started either. It also
  records how many bytes it wrote and a checksum over all of them, in 32 bytes
  of the image reserved for it, and the bootloader adds the whole image up
  before it starts any of it: a download interrupted in its last 4 KiB, which
  used to leave a board for the bench, now leaves one waiting in DFU as well.
  The four bytes of the vector checksum and fourteen of that record are the
  only differences between an image file and the flash written from it. The
  bootloader half takes effect only on a board carrying a bootloader built
  from this tree, which is installed with ISP or JTAG. A read of the image
  file that fails part way through now fails the download as well, where it
  used to end the transfer and report success.
  [#242](https://github.com/SqueakyFloors/ubertooth/issues/242),
  [#1039](https://github.com/SqueakyFloors/ubertooth/issues/1039),
  [#1041](https://github.com/SqueakyFloors/ubertooth/issues/1041)
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
- A board running `btbr` can be reflashed with `-U <serial>`. It used to answer
  `ubertooth-dfu` with a stalled transfer and exit 1, because the image reset
  before the request asking it to enter DFU had been answered; it did enter the
  bootloader, which carries no serial number, so no `-U` could reach it again.
  [#475](https://github.com/SqueakyFloors/ubertooth/issues/475)
- `ubertooth-dfu -d` and `-s` check the file before they read it into memory:
  a directory, a FIFO or a device node is refused, and so is a file too large
  to be an image (507920 bytes for `-d`, 16 fewer for `-s`, which adds the
  suffix), instead of allocating from a size nothing had checked.
  [#246](https://github.com/SqueakyFloors/ubertooth/issues/246)
- `ubertooth-dfu -s` refuses a binary that already carries a DFU suffix
  instead of adding a second one, and names the output
  file when it cannot create it. `-s` and `-d` now only read their input, so a
  firmware image in a read-only location can be signed or flashed. A `-u`
  read-back that fails part-way no longer empties an earlier file of the same
  name: it is written to a temporary file and renamed once complete. The
  documentation now says what `-u` writes (a raw dump of the application flash
  region, not a DFU file).
  [#200](https://github.com/SqueakyFloors/ubertooth/issues/200)
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
- `ubertooth-dfu -u` reads back only the flash the part has, instead of a
  fixed 240 KiB that faulted the bootloader on a 128 KiB part, and says how
  many bytes it is reading and where that figure came from.
  [#233](https://github.com/SqueakyFloors/ubertooth/issues/233)
- `ubertooth-dfu -d` refuses an image too large for the attached part, naming
  the part and the flash it has, while the board is still running its
  application; a part it cannot identify is reported as unchecked.
  [#281](https://github.com/SqueakyFloors/ubertooth/issues/281)

### ubertooth-ducky

#### Fixes

- `ubertooth-ducky -q` and `-b` name a request the board refused and exit 1,
  instead of waiting their two seconds and exiting 0 on a request the board
  refused.
  [#328](https://github.com/SqueakyFloors/ubertooth/issues/328)
- `ubertooth-ducky -U` now selects devices by index or serial number.
  [#36](https://github.com/SqueakyFloors/ubertooth/issues/36)

### ubertooth-ego

#### New features

- **Script-visible change:** `ubertooth-ego -l` with interference (`-i`, `-j`)
  is refused, naming the mode as it was spelled: only the modes that read a
  packet read a capture length, so it used to set a firmware value the mode
  never looks at and said nothing.
  [#411](https://github.com/SqueakyFloors/ubertooth/issues/411)

#### Fixes

- `ubertooth-ego`'s mode clash lists `-j`, so a user who typed it is not given
  a list without it: `error: -j cannot be combined with -i; choose one of -f,
  -r, -i, or -j`.
- `ubertooth-ego` no longer prints a `delta_t` short by whole 327.68 s periods
  of the clock after a quiet spell. The gap now comes from the same capture
  times the other tools use, which count every wrap, so the figure holds the
  whole of the gap. The first packet of a capture reports no gap rather than
  its own clock reading.
  [#981](https://github.com/SqueakyFloors/ubertooth/issues/981)
- `ubertooth-ego` captures in the mode its command line asks for, on a board
  left in another E-GO mode by a run that was interrupted without stopping it:
  `ubertooth-ego -r -c 2418` after a killed `-f` used to go on hopping across
  the E-GO channel list, and its capture looked like a continuous receive that
  found nothing. A second run of the same mode starts over too, so it receives
  on the channel it was given rather than the one before it. `ubertooth-ego
  -r` and `-i` after any other interrupted capture, such as a killed
  `ubertooth-specan`, also used to start in follow mode.
  [#757](https://github.com/SqueakyFloors/ubertooth/issues/757)
- Every e-go record carried two bytes of its own timestamp at the end of its
  data, because the receive buffer was two bytes shorter than the record it
  goes out in. The buffer is now the record's whole data field, and the
  receive loop stays inside it whatever capture length the board was given.
  [#749](https://github.com/SqueakyFloors/ubertooth/issues/749)
- `ubertooth-ego` captures with the access code the run asked for.
  `-a <code>` used to stay on the board until it was reset or reflashed, so the
  next run captured with the previous run's code while `-h` and the man page
  called `630f9ffe` the default. A mode start now puts the documented default
  back, and `-a` is stated for the run that asks for it.
  [#397](https://github.com/SqueakyFloors/ubertooth/issues/397)
- `ubertooth-ego` names a request the board refused while setting a capture up
  (channel, access code, capture length) and exits 1, instead of capturing with
  whatever settings the board was left on.
  [#379](https://github.com/SqueakyFloors/ubertooth/issues/379)
- `ubertooth-ego` refuses `-c` combined with `-f`, `-i` or `-j`, where it
  silently had no effect: those modes hop across the E-GO's own channel list.
  [#394](https://github.com/SqueakyFloors/ubertooth/issues/394)
- `ubertooth-ego -a` with an access code shorter than four bytes sets the code
  the user asked for, zero-filled, instead of padding it with leftovers of the
  previous request.
  [#402](https://github.com/SqueakyFloors/ubertooth/issues/402)
- `ubertooth-ego` rejects malformed and out-of-range packet lengths before
  opening the device.
  [#39](https://github.com/SqueakyFloors/ubertooth/issues/39)

### Firmware

#### bluetooth_rxtx

- The release's receive-only firmware names itself `bluetooth_rx` instead of
  `bluetooth_rxtx`, so `ubertooth-util -v` and ubertooth-dfu(1) tell a board
  running it from one running the full image. A `DISABLE_TX=1` build from a
  git checkout reports the same name; one from an unpacked source release
  keeps the shipped version.h and so reports `bluetooth_rxtx`, which
  firmware/README now says. Every host tool accepts either name.
- Fixed: on the Ubertooth Zero and the ToorCon badge the firmware looked for
  the radio's GIO6 status pin on the wrong GPIO port. That pin is P1.15 on the
  Zero and P1.10 on the badge, and the firmware asked port 2 for all three
  boards, so the signals it took for the radio's came from somewhere else: on
  the Zero from bit 15 of port 2, which the part does not have, so every read
  answered 0 and the wait for the radio's transmit buffer to drain ran to its
  bound, which ends the packet, so a packet longer than the 32 bytes that
  buffer holds went out as its first 32 bytes; on the badge from P2.10, a pin
  that exists and carries no radio signal, so the wait answered whatever that
  pin happened to read. Reads now follow the board.
  Carrier sense armed an interrupt on the same wrong port; the part raises a
  GPIO interrupt on ports 0 and 2 only, so on those two boards it now arms
  nothing rather than an unrelated pin. No board loses a carrier-sense trigger
  it was getting, because the pin that was armed is not one the radio drives,
  and the receive loop marks a packet from its own RSSI comparison as before.
  The Ubertooth One, whose GIO6 is P2.2, is unaffected.
  [#1006](https://github.com/SqueakyFloors/ubertooth/issues/1006)
  [#1007](https://github.com/SqueakyFloors/ubertooth/issues/1007)
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
- Firmware RSSI averaging now rounds negative dBm correctly.
  [#15](https://github.com/SqueakyFloors/ubertooth/issues/15)
- Correct Bluetooth Low Energy cyclic redundancy check filtering and perform
  verification in the physical-layer engine. Credit: oliver
  ([@Oliver0804](https://github.com/Oliver0804)).
  [upstream #550](https://github.com/greatscottgadgets/ubertooth/pull/550)

#### btbr

- **Script-visible change:** the `btbr` image reports the firmware API version
  of the tree it was built from, instead of a fixed 1.06 that never moved. A
  board carrying it is no longer refused over its version by every other tool;
  those tools still cannot drive that image and now fail on the first request
  it does not answer.
  [#556](https://github.com/SqueakyFloors/ubertooth/issues/556)
- Fixed: the `btbr` BR/EDR image asked port 2 for the radio's GIO6 interrupt
  whatever the board, and GIO6 is on port 1 on the Ubertooth Zero and the
  ToorCon badge. Its PHY refills the radio's 32-byte transmit buffer from that
  interrupt, which is how a packet longer than the buffer reaches the air at
  all, and the part raises a GPIO interrupt on ports 0 and 2 only: on those
  boards nothing could arm it, and every page, inquiry, and answer to a scan
  would have gone out as its first 28 bytes. The image now says so when it is
  built rather than on the air. It does not build for a board whose GIO6
  cannot raise that interrupt, and a build of all the images for such a board
  leaves it out with a note instead of failing, so the rest build as before.
  On the badge that replaces a link failure, because `btbr` and the firmware
  it shares with the other images both claimed the handler for that interrupt.
  The Ubertooth One builds `btbr` as before.
  [#1011](https://github.com/SqueakyFloors/ubertooth/issues/1011)
- Fixed: a `btbr` connection or page could stop forever. When the slot that
  would have ended a transmission was already full, the board said only
  `tdma_schedule bucket overflow` and carried on: the radio was left
  transmitting, the packet the link had handed over stayed held, and nothing
  told the link the slot was over, so it scheduled neither its next receive nor
  its next transmit and went silent. On the page response it stopped the
  firmware instead, because there the next packet is scheduled from a receive
  that still arrives. It now says the packet was not sent, stops the radio,
  gives the packet back, and reports the failed transmit, so the link carries
  on.
  [#1139](https://github.com/SqueakyFloors/ubertooth/issues/1139)
- Fixed: a `btbr` connection or page could also stop forever when the slot the
  transmit itself needed was already full. The board said only
  `tdma_schedule bucket overflow`, no part of the transmit ran, and nothing
  told the link the slot was over, so it went silent and the next packet it
  handed over stopped the firmware with `txs: already got pkt`. It now says
  the packet was not sent, gives the packet back, and reports the failed
  transmit. A transmit that lost only the slot it would have started in is
  reported as a dropped packet too, where it used to be dropped in silence.
  [#1151](https://github.com/SqueakyFloors/ubertooth/issues/1151)
- Fixed: every `btbr` mode that receives could go quiet the same way, because
  the receive task asked the scheduler for its slots without reading the
  answer. A connection, a page, an inquiry, a page scan, or a `monitor` follow
  whose next receive was refused had nothing left to run, said only
  `tdma_schedule bucket overflow`, and waited forever; a multi-slot packet
  whose decode could not be continued was worse, because it also kept the
  buffer it was being decoded into and the next receive stopped the firmware
  with `rx_schedule: rx_msg!=0`. A refused receive is now reported to the mode
  that asked for it, which goes back to listening where it can and otherwise
  reports the session over, and a decode that cannot be continued ends the
  receive as one that produced nothing. A `page` or an `inquiry` the scheduler
  has no room to start at all is refused instead of being reported as running.
  [#1174](https://github.com/SqueakyFloors/ubertooth/issues/1174)
- Fixed: a `btbr` `page_scan`, `inquiry_scan`, or `monitor` could go quiet and
  never come back. Each of them listens by asking the scheduler for the next
  slot, and a slot already holding four tasks refuses it; the board said only
  `tdma_schedule bucket overflow` and then had nothing left to run at all.
  Even the scan's own one-minute deadline was never reached, because that is
  checked when the next window is opened and no window was being opened, so
  the session never ended and the user had to stop it. The board now says the
  scan could not be armed and reports the session over. `monitor` had a second
  version of it, on the slot that starts following a connection it has just
  seen being made: it kept the address it decoded but lost the follow in
  silence, and now reports that too.
  [#1167](https://github.com/SqueakyFloors/ubertooth/issues/1167)
- Fixed: when the `btbr` image's radio had not tuned in time for a transmit
  slot, the board said only `TX: RF not FS_ON`, and then spent the next slot
  waiting for an end of transmission that could not come, counting a radio
  that never answered in `ubertooth-util -w`. It also left the dropped
  packet's count of unsent bytes standing, so a page that missed a slot could
  stop the firmware when it sent its next ID packet. It now says the packet
  was not sent, waits for nothing, and gives the byte count back.
  [#745](https://github.com/SqueakyFloors/ubertooth/issues/745)
- Fixed: the addresses the `btbr` firmware printed lost their leading zeros,
  so they were not the twelve hex digits the prompt takes back.
  `bdaddr 00:11:22:33:44:55` was echoed as `Set bdaddr 1122334455`, and the
  `fhs: ba=` line of a `page_scan` or `monitor` session dropped them the same
  way from the address it read off the air. Both now print twelve digits.
  [#745](https://github.com/SqueakyFloors/ubertooth/issues/745)
- A board in `btbr`'s `page_scan` or `inquiry_scan` now listens on the one
  frequency the specification gives a scanning device, and holds it for the
  1.28 seconds that frequency lasts, instead of moving every few slots. A
  pager sweeps all 32 possible frequencies in 20 ms and an inquirer in 40 ms,
  so either now meets a scanning board regularly, where before both ends were
  moving and had to coincide. The legs that answer a page or an inquiry hop on
  their own rules, as the specification separates them.
  [#811](https://github.com/SqueakyFloors/ubertooth/issues/811)
- A board scanning for a page goes back to scanning when the packet it was
  waiting for turns out to be something else, instead of waiting in page scan
  forever with nothing left to run and its 60-second timeout out of reach. The
  same in `btbr`'s monitor mode.
  [#811](https://github.com/SqueakyFloors/ubertooth/issues/811)
- Page scan, inquiry scan and the paging monitor watch for a packet through
  nearly the whole 312.5-microsecond slot, as their own comment says, rather
  than the first 25 microseconds of it. The window was set in microseconds
  where the slot clock counts 100-nanosecond steps, so it closed before the
  moment a packet is expected and `ubertooth-btbr` missed most page and
  inquiry scan responses.
  [#669](https://github.com/SqueakyFloors/ubertooth/issues/669)
- The `btbr` firmware image hops to the channels the Bluetooth spec selects.
  Under an AFH map it now keeps a used channel of the basic sequence instead of
  always selecting inside the reduced set, and falls back to basic hopping
  below the spec minimum of 20 used channels; without a map it no longer picks
  the wrong channel for some addresses.
  [#13](https://github.com/SqueakyFloors/ubertooth/issues/13)
- Improve Classic Bluetooth paging speed and reliability. Credit: Hillel
  Chimowicz ([@hllhll](https://github.com/hllhll)) and
  [@shapaz](https://github.com/shapaz).
  [upstream #540](https://github.com/greatscottgadgets/ubertooth/pull/540)
- The Classic Bluetooth (`btbr`) firmware keeps the power amplifier level set
  with `ubertooth-util -a` instead of forcing level 3 when it brings the radio
  up, so its default rises to the power-on level 7 the same way. A generic
  transmit still sends at the level in its own request, but leaves the board's
  level as it found it.
  [#181](https://github.com/SqueakyFloors/ubertooth/issues/181)
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

#### The bootloader

- A bootloader can be installed over USB. In earlier releases a new bootloader
  reached a board only over the serial programming interface or JTAG, because a
  bootloader refuses USB writes to its own region, so the bootloader fixes in
  this release reached only boards someone had reflashed that way.
  `make -C firmware bl_update` builds an image that carries a bootloader, and
  `ubertooth-dfu --update-bootloader -d bl_update.dfu` installs it: the host
  prints the hash the new bootloader will report, the image programs the region
  and reads it back, and the board returns to DFU for your firmware, which
  `ubertooth-util -B` then confirms. An interrupted write leaves the board
  waiting in the part's own serial programmer, so keep a serial adapter to
  hand; ubertooth-dfu(1), UPDATING THE BOOTLOADER has the procedure, the
  refusals, and the one window that needs the programming entry pin too.
  [#1056](https://github.com/SqueakyFloors/ubertooth/issues/1056)
- The release improves the bootloader and the flashing path, and a board runs
  this firmware on the bootloader it already carries: nothing here asks for a
  new bootloader, and an image is written over USB the same way either way.
  A bootloader built from this tree is installed with
  `ubertooth-dfu --update-bootloader`, as above, or with a 3.3 V serial
  programmer on the ISP connector or a JTAG probe, as
  docs/source/programming.rst describes; what follows is what a board gains
  once it carries one. Everything else in the release works whatever
  bootloader a board carries; without one from this tree these are
  unavailable:
  - recovery over USB from an interrupted or incomplete firmware download. An
    older bootloader jumps straight into whatever is at the start of the
    application region, so a board that lost a download part way through
    faults on every power-up until someone bridges the entry pin at the
    bench; a board on this bootloader waits in DFU for the next
    `ubertooth-dfu -d`.
  - refusing to start an application whose first block was programmed only in
    part, and one whose flash no longer adds up over the length the host
    wrote: the vector checksum and the whole-image check. Both are read on the
    board before it jumps.
  - bounding a download, and a `-u` read-back, by the flash the part in front
    of the bootloader actually has. A block with no flash behind it is refused
    with `ERRADDRESS` rather than asking the part to program memory it has not
    got, or faulting on the read and answering nothing until the board is
    power-cycled; a part the built-in table cannot identify is bounded by the
    sectors it reports having.
  - the bootloader's own version and revision in `ubertooth-util -B`, where a
    board as shipped reports only a hash of its bootloader.
  [#242](https://github.com/SqueakyFloors/ubertooth/issues/242),
  [#354](https://github.com/SqueakyFloors/ubertooth/issues/354),
  [#439](https://github.com/SqueakyFloors/ubertooth/issues/439),
  [#1056](https://github.com/SqueakyFloors/ubertooth/issues/1056)

### Libraries

#### libubertooth

- libubertooth lets the calling program say where a capture's results go,
  instead of writing them to standard output itself: the `result_stream`
  field of `ubertooth_t`, read with the new
  `ubertooth_result_stream()`. A handle that sets none writes to standard
  output as before, so every tool here, and a program of your own, behaves
  exactly as it did. `ubertooth_print_afh_map()` writes an AFH channel map on
  a stream you choose, in the line libbtbb's `btbb_print_afh_map()` prints on
  standard output.
  [#745](https://github.com/SqueakyFloors/ubertooth/issues/745)
- libubertooth's `register_cleanup_handler()` is deprecated in favor of
  `ubertooth_catch_stop_signals()`, which installs the same stop handlers
  without needing a device handle. It is still exported, so a program of your
  own that calls it builds and links against this release; GCC and Clang now
  name the replacement in a warning, which a build with `-Werror` will stop
  on. It is removed in the release after this one.
  [#745](https://github.com/SqueakyFloors/ubertooth/issues/745)
- libubertooth's `cmd_get_access_address()` is deprecated in favor of
  `cmd_read_access_address()`, which reports an error separately from the
  address instead of returning both as one `u32`. The header said it was
  deprecated already; GCC and Clang now name the replacement in a warning
  too, which a build with `-Werror` will stop on. It is still exported and
  still works as it did.
  [#745](https://github.com/SqueakyFloors/ubertooth/issues/745)
- A program that sets more advertising data than the board can transmit is
  told so, where it used to be taken and then dropped: `cmd_le_set_adv_data()`
  and the board's `UBERTOOTH_LE_SET_ADV_DATA` both refuse anything longer than
  the 31 bytes a legacy ADV_IND carries, with `error: LE advertising data too
  long`. Up to 255 bytes used to be accepted, and the advertising mode asked
  for afterwards then went back to idle without a word. `LE_ADV_MAX_LEN` is
  gone from `ubertooth_interface.h` with it: a program that bounded a call by
  it wants `ADV_IND_MAX_DATA`. No shipped tool sets more than 18 bytes.
  [#752](https://github.com/SqueakyFloors/ubertooth/issues/752)
- libubertooth reports a short reply to any fixed-size read as an error instead
  of a value made from stale bytes (such as channel 0 right after hopping).
  [#149](https://github.com/SqueakyFloors/ubertooth/issues/149)

#### libbtbb

- The tools are built against a newer libbtbb
  ([`bf878df`](https://github.com/SqueakyFloors/libbtbb/commit/bf878df27e131dd8ab56bb3497918eb9a83f607b)),
  which brings its own run of fixes to the Classic and BLE decoders and to the
  capture file writers: a 255-octet advertising packet keeps its last three
  octets instead of losing them to the CRC field, AUX1, DV, EV3, EV4 and EV5
  payloads decode, a packet whose payload the decoder could not read is no
  longer reported with the previous packet's bytes, a BLE capture records each
  connection's parameters, and the library's own notes go to standard error
  rather than into a capture on standard output. A Classic capture record also
  carries more of the packet that settled the piconet's address: its header
  and its payload, both checked and both recomputable from the reference
  address in the same record, where that record used to carry one of them at
  best. What that library is now tested against is in its
  [TESTING.md](https://github.com/SqueakyFloors/libbtbb/blob/master/TESTING.md).
  `install.sh` and the build guides check out the new commit; a libbtbb
  without what the tools use is still refused at configure time, by name.
  [#736](https://github.com/SqueakyFloors/ubertooth/issues/736)
- The pin moves on to the current libbtbb
  ([`50bbc76`](https://github.com/SqueakyFloors/libbtbb/commit/50bbc76d383e4038ee6dadccad55f0161c1cb029)),
  which names the Bluetooth connection roles Central and Peripheral wherever it
  says them. A printed LE packet reads `LL_PERIPHERAL_FEATURE_REQ` where it read
  `LL_SLAVE_FEATURE_REQ`, an advertising data type reads `(Peripheral
  Connection Interval Range)`, and the Wireshark plugins built from the same
  clone label a packet's direction `Central to Peripheral` and word the LMP and
  eSCO fields to match. Only the wording moves: every field abbreviation a
  display filter or `tshark -e` names is as it was, the installed header and the
  library's exported symbols are byte for byte what the old pin installed, and
  a capture record is unchanged down to the option codes. The library's other
  two commits are its own documentation.
  [#736](https://github.com/SqueakyFloors/ubertooth/issues/736)
- The pin moves on again, to libbtbb
  ([`5c4e535`](https://github.com/SqueakyFloors/libbtbb/commit/5c4e535968b58db2f28e2f3bce0f02ca1c02b03e)),
  which brings two more fixes to the Classic clock search. Fixed: a piconet
  whose address part (UAP) was given as `0x00`, as `ubertooth-rx -u 00` gives
  it, went on searching for an address part it had already been handed, so
  CLK1-27 never arrived; it now starts the clock search
  ([libbtbb issue 134](https://github.com/SqueakyFloors/libbtbb/issues/134)).
  Fixed: a clock search on a piconet that had observed no packets chose tens
  of thousands of candidates and worked through them; it now chooses none
  ([libbtbb issue 131](https://github.com/SqueakyFloors/libbtbb/issues/131)).
  The tools call no new library function, so a libbtbb without what they use
  is still refused at configure time, by name. `install.sh` and the build
  guides check out the new commit.
  [#736](https://github.com/SqueakyFloors/ubertooth/issues/736)
- The pin moves on again, to libbtbb
  ([`d356e9d`](https://github.com/SqueakyFloors/libbtbb/commit/d356e9da7fb03969371aa14d603a8aad2f0ef198)),
  where the release a build from a source archive reports comes from git
  rather than a version kept by hand, as it now does here. A build with no
  release to read reports `unknown`; the commit libbtbb records beside it is
  what identifies such a build.
- The pin moved on before that, to libbtbb
  ([`c70de76`](https://github.com/SqueakyFloors/libbtbb/commit/c70de76afbe13f3740bcd906952a36c849383810)),
  where libbtbb reports its release the way these tools report theirs: the
  release it was built from, or the nearest release and how many commits are
  past it. It records the commit separately, which is what the configure step
  reads to say which libbtbb a build found.
- The pin moved on once more before that, to libbtbb
  ([`7e1cdf0`](https://github.com/SqueakyFloors/libbtbb/commit/7e1cdf0336857a714da1af8c7496fe32453ac6d0)),
  which finishes the clock search off. Fixed: a capture given only a LAP
  worked the address part out for itself and then kept searching for it, so
  `ubertooth-rx -l <LAP>` with no `-u` read the piconet's packets and never
  reached follow mode; the clock search now starts
  ([libbtbb issue 136](https://github.com/SqueakyFloors/libbtbb/issues/136)).
  [#736](https://github.com/SqueakyFloors/ubertooth/issues/736)
- libbtbb is built from this project's
  [fork](https://github.com/SqueakyFloors/libbtbb) at a pinned commit, needing
  no `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`; `install.sh --libbtbb-url`/`--libbtbb-ref`
  build a different one.
  [#320](https://github.com/SqueakyFloors/ubertooth/issues/320)
- The pinned libbtbb now follows the Core specification when AFH re-maps a hop
  channel, so host-side predictions agree with the channel the board tunes to.
  A clock and UAP tracked with an AFH map used to name channels the piconet was
  not on.
  [#143](https://github.com/SqueakyFloors/ubertooth/issues/143)

### Build and compatibility

- `install.sh` provides one-command source installation on Debian/Ubuntu and on
  macOS with Homebrew, with optional virtual environments, a custom `--prefix`,
  `--skip-deps` for already-installed packages, udev setup on Linux, firmware
  builds, and explicitly selected flashing through
  `--flash-device <index|serial|all>`, which now recognizes several attached
  boards and refuses to skip one it cannot read.
  [#23](https://github.com/SqueakyFloors/ubertooth/issues/23)
  [#25](https://github.com/SqueakyFloors/ubertooth/issues/25)
- `install.sh --update-bootloader`, alongside `--flash-device`, installs the
  bootloader this tree builds on the board named and puts `bluetooth_rxtx` back
  afterwards, which used to be four steps by hand. A board that already
  has that bootloader is left alone, boards are done one at a time, and the
  result is read off each board. Keep a serial programmer to hand: an
  interrupted bootloader write leaves a board needing one.
  [#1229](https://github.com/SqueakyFloors/ubertooth/issues/1229)
- The supported macOS minimum is 15 (Sequoia), the oldest version CI builds
  now that GitHub has retired the macOS 14 runner image. macOS 14 should still
  build, but it is no longer tested.
  [#1242](https://github.com/SqueakyFloors/ubertooth/issues/1242)
- `install.sh` stops with a clear error, before installing or building
  anything, when Python is older than 3.8. [#154](https://github.com/SqueakyFloors/ubertooth/issues/154)
- `install.sh` installs `libnewlib-arm-none-eabi` explicitly, and its apt hint
  names `python3-pyqt5` (Ubuntu has no `python3-pyside6` before 24.10).
  [#152](https://github.com/SqueakyFloors/ubertooth/issues/152)
  [#154](https://github.com/SqueakyFloors/ubertooth/issues/154)
- The configure step says which libbtbb it found, the commit that library was
  built from, and the commit these tools are built against, and warns when the
  two commits differ. A libbtbb older than the pin used to be accepted
  without a word as long as it had every function the tools call, so the tools
  ran with that library's behavior, and a capture measured against it looked
  like a change in the tools themselves. It is a warning and not a refusal: a
  libbtbb from a distribution package records no commit, and the line says
  that rather than guessing.
  [#1118](https://github.com/SqueakyFloors/ubertooth/issues/1118)
- libubertooth installs to the directory your system keeps libraries in,
  which is where libbtbb already went: `lib64` on a 64-bit Fedora, RHEL or
  SUSE, `lib/<triplet>` where the prefix is `/usr` on a multiarch Debian or
  Ubuntu, and `lib` elsewhere. The two used to land in different directories
  on such a system, so the loader had to be told about both.
  `-DCMAKE_INSTALL_LIBDIR=` chooses the directory, `-DCMAKE_INSTALL_INCLUDEDIR=`
  the headers', and `LIB_SUFFIX` still decides where a build command passes it.
  `libubertooth.pc` follows, so `pkg-config --libs libubertooth` names the
  directory the library is really in, and `./install.sh` puts that directory
  first on the installed tools' run path and says so if an earlier install
  left a libubertooth in `lib`.
  [#745](https://github.com/SqueakyFloors/ubertooth/issues/745)
- `./install.sh` installs on Fedora. It used to stop on any Linux without
  `apt-get`; it now finds `dnf` and asks it for the same dependencies under
  Fedora's names, the ARM toolchain for `--with-firmware` included, and the
  rest of the run is what it has always been on Linux.
  [#24](https://github.com/SqueakyFloors/ubertooth/issues/24)
- The install writes `libubertooth.pc`, so a program of your own built against
  the installed library takes its flags from `pkg-config --cflags --libs
  libubertooth` instead of being given them by hand, including the include
  directory `<libusb-1.0/libusb.h>` resolves against. It reads its prefix back
  from where it is installed, so a staged or relocated install describes the
  directories it really landed in.
  [#886](https://github.com/SqueakyFloors/ubertooth/issues/886)
- The install also writes `ubertoothConfig.cmake`, so a CMake project of your
  own says `find_package(ubertooth REQUIRED)` and links
  `ubertooth::ubertooth`. The target carries the installed library, its header
  directory, and libbtbb and libusb, whose headers the Ubertooth headers
  include, so nothing else has to be found or named by hand; a version can be
  asked for, and any 1.x satisfies a request for 1.0. It reads its prefix back
  from where it is installed, as `libubertooth.pc` does.
  host/README.cmake.md has the three lines a consumer needs.
  [#745](https://github.com/SqueakyFloors/ubertooth/issues/745)
- Configuring on macOS no longer tells you to install a Linux package to get
  `ubertooth-scan` and `ubertooth-follow`. It says BlueZ is Linux only and
  points at `ubertooth-rx` and `ubertooth-afh`, which cover much of the same
  ground by listening and need no adapter. The docs now state that these two
  tools stay Linux only, and why.
  [#47](https://github.com/SqueakyFloors/ubertooth/issues/47)
- A reconfigure after libbtbb, libubertooth or libbluetooth moves finds it
  where it is now, the check libusb already had. A moved libbtbb was the worst
  of the three: the configure failed with instructions to rebuild a libbtbb
  that was installed and fine, because the check for the functions the tools
  call ran against the header that had gone.
  [#913](https://github.com/SqueakyFloors/ubertooth/issues/913)
- A reconfigure after a Homebrew upgrade finds libusb where it is now. The
  build remembered where it found libusb, and Homebrew puts each version in a
  directory of its own, so after an upgrade the remembered one had gone: the
  configure reported libusb found and the build then failed on the missing
  header. It now checks the remembered header and library are still there and
  searches again if either has moved.
  [#905](https://github.com/SqueakyFloors/ubertooth/issues/905)
- A release now carries the btbr image (`btbr.dfu`, `.bin` and `.hex`)
  alongside bluetooth_rxtx, so `ubertooth-btbr` has firmware to flash without
  building it. Published images report the release version rather than
  `unknown`, and no image records the name of whoever built it.
- `cmake -S host/ubertooth-tools -B build` configures: the tools can be built
  on their own against an installed libubertooth, which is what
  `FindUBERTOOTH.cmake` is for. CMake 4 refused the directory outright,
  because nothing there said which CMake versions it was written for, and
  before that the first `find_package` failed because nothing pointed at the
  Find modules beside it. The tools built that way now ask glibc for the
  interfaces they use, as a build of the whole tree does, so a strict C
  standard works there too.
  [#798](https://github.com/SqueakyFloors/ubertooth/issues/798)
- Configuring the tools against a libubertooth installed from an older commit
  is refused in one line that names what is missing and how to install a
  current one. Such an install used to configure without a word and then fail
  in the compiler with dozens of errors naming symbols its header has not
  got: `FindUBERTOOTH.cmake` now asks the libubertooth it found for a few of
  the functions the tools call, as the libbtbb check already did.
  [#745](https://github.com/SqueakyFloors/ubertooth/issues/745)
- The host tools build with a strict C standard (`-std=c17`, `-std=c2x`) on
  Linux, where that used to fail on `CLOCK_REALTIME`, `usleep()` and the
  byte-order helpers: the build asks glibc for the interfaces it uses, instead
  of leaving it to the compiler's default GNU standard.
  [#648](https://github.com/SqueakyFloors/ubertooth/issues/648)
- `install.sh` finds the libbtbb it just built wherever libbtbb installed it,
  so the tools run on a system whose libraries live in `lib64` or a multiarch
  `lib/<triplet>`. It built them for a libbtbb in `lib` under the prefix, and
  on such a system the installed `ubertooth-util` would not start at all:
  `Library not loaded: @rpath/libbtbb.1.dylib`, or the equivalent on Linux.
  The build guide and READMEs say where libbtbb installs.
  [#88](https://github.com/SqueakyFloors/libbtbb/issues/88)
- `install.sh --uninstall` takes the install back off again: the files the
  install recorded, the udev rule with them, and nothing else. The libbtbb it
  installed is kept unless you ask for it, because other builds link against it.
  [#337](https://github.com/SqueakyFloors/ubertooth/issues/337)
- `install.sh` says it is checking Homebrew, and that it is working out which
  Ubertooth to flash, before those slow steps run; on macOS its first line
  used to be a couple of seconds late, which looked like a hang.
  [#640](https://github.com/SqueakyFloors/ubertooth/issues/640)
- `install.sh` refuses a command line in the same words as the tools:
  `error: unrecognized option '--bogus'` where it said
  `error: unknown option: --bogus (use --help)`, and
  `error: unexpected argument 'extra'` for a word or a lone `-`, which it used
  to call an unknown option too. A lone `--` now ends the options and the
  install goes ahead with the defaults, and `-h` answers once the whole
  command line has parsed, so `install.sh -h --bogus` reports the bad option
  instead of printing the help and exiting 0.
  [#323](https://github.com/SqueakyFloors/ubertooth/issues/323)
- Fixed: `install.sh --help` printed its usage text and then the whole script;
  it now prints the usage text and stops.
  [#1205](https://github.com/SqueakyFloors/ubertooth/issues/1205)
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
- Firmware version strings and header dependencies now update reliably after
  source or revision changes, including GNU Make 3.81 and source archives.
  [#30](https://github.com/SqueakyFloors/ubertooth/issues/30)

### Documentation

- `ubertooth-btle(1)`, the tool's `-h` text, and the capture guide say that
  promiscuous mode, `-p`, recovers connections that hop with channel selection
  algorithm 1: on an algorithm-2 connection, which is what Bluetooth 5.0
  devices normally use, it reports an access address and a CRC init value and
  goes no further, and such a connection is captured from its setup with `-f`.
  The man page also says which channels `-p` listens on as it works a
  connection out, where it used to name only the one the board was tuned to,
  and the capture guide and
  the concepts chapter no longer say Ubertooth implements algorithm 1 only,
  which stopped being true of follow mode when it learned algorithm 2.
  [#1025](https://github.com/SqueakyFloors/ubertooth/issues/1025)
- `ubertooth-rx(1)` explains the clock search: what it needs, and why a run
  can end without a clock.
  [#968](https://github.com/SqueakyFloors/ubertooth/issues/968)
- `ubertooth-follow(1)` and the sniffing guide walk a follow through from not
  knowing the target to a capture file in Wireshark, including what a follow
  that has not locked on looks like and why. Both now say plainly what a
  capture cannot hold: enhanced data rate payloads, which the radio cannot
  demodulate, and the contents of an encrypted link.
  [#518](https://github.com/SqueakyFloors/ubertooth/issues/518)
- The usage text of `ubertooth-util`, `ubertooth-tx` and `ubertooth-dfu` lists
  `-h`, which all three accept and every other tool's usage text lists, and
  `ubertooth-util`'s `-t` line spells "initiate".
- Renamed the Bluetooth connection roles from master/slave to
  Central/Peripheral, and SPI slave-select to chip-select. `ubertooth-btle
  -s` is now faux peripheral mode, its errors read `error: faux peripheral
  address must be xx:xx:xx:xx:xx:xx`, and `ubertooth-util -w` reports the mode
  as `BLE peripheral`. The option letters, the request numbers, and the mode
  numbers are unchanged, so no host tool and board pairing is affected.
- The sniffing guide, `ubertooth-rx(1)`, `ubertooth-follow(1)`, and
  `ubertooth-scan(1)` agree about the placeholder `00:00` these tools page a
  sniffed piconet on, where one claimed it works and the others called it
  unverified. A page does not carry the paged device's NAP, so the placeholder
  is not what decides whether a page reaches the target; a wrong UAP still
  does, the specification sections are cited, and what a real controller makes
  of the guess is still unmeasured here.
  [#699](https://github.com/SqueakyFloors/ubertooth/issues/699)
- `ubertooth-debug(1)` is titled "read CC2400 radio registers", and
  `ubertooth(7)` lists it as "Peeking at registers on the CC2400". Both used
  to offer writing as well, which no option of any tool does, so a reader went
  looking for one that is not there.
  [#945](https://github.com/SqueakyFloors/ubertooth/issues/945)
- `ubertooth-dump(1)` says what a dump holds, which it did not say at all:
  68-byte records of the host arrival time, as Unix epoch seconds, and the
  whole USB receive packet, or with `-b` one 401-byte line of ASCII `0` and
  `1` per block. Its `-h` text names `-d`'s value and says what `-` means, and
  both it and the man page stop giving the dump's binary-ness as the reason a
  terminal on standard output is refused: `-b` is text, and is refused too.
  [#945](https://github.com/SqueakyFloors/ubertooth/issues/945)
- `ubertooth-util(1)` says when the firmware API version moves: during
  development only for a change that breaks older host tools, not for every
  firmware change the host sees. It is what `COMPATIBILITY.md` already
  describes.
  [#905](https://github.com/SqueakyFloors/ubertooth/issues/905)
- The README and the release procedure say what a release tag means:
  `yyyy-mm-DEVn` is a development pre-release, `yyyy-mm-RCn` a release
  candidate and `yyyy-mm-Rn` the release, the first two published as GitHub
  pre-releases, and host tools and firmware from one tag belong together.
- The README opens with a short Hardware section: what an Ubertooth One is,
  the board photo, and links on to the detail. The hardware page it links to
  is current, and says which LPC part a board carries and how much flash and
  RAM it has, what the radio can and cannot do, the connectors and the LEDs as
  the firmware drives them today, and the KiCad design files the board is
  built from. The Ubertooth Zero page is gone: the fork's documentation covers
  Ubertooth One.
- A one-page guide, "Using Ubertooth with Wireshark", covers the
  `btle-extcap.py` plugin end to end: what it gives you over a named pipe,
  the three install steps, the options dialog, why a capture filter is
  refused, and running it from `tshark`.
- `btle-extcap.py` has a man page of its own, `btle-extcap(1)`, installed with
  the others: where to put the script so Wireshark finds it, every call
  Wireshark makes, what it answers a capture filter with, its two exit
  statuses and its diagnostics. It was the one tool in the README catalog with
  no page.
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
  time, so what a threshold means depends on how many channels the piconet is
  using: a small one notices a dropped channel quickly and prunes channels
  still in use, a large one settles and is slow to notice.
  [#194](https://github.com/SqueakyFloors/ubertooth/issues/194)

### Under the hood

Fixes with no visible change in normal use.

- `ubertooth-follow` no longer borrows an earlier call's error number when it
  reports a failed adapter or clock read.
  [#44](https://github.com/SqueakyFloors/ubertooth/issues/44)
- The pinned library no longer races when more than one thread decodes at the
  same time.
  [#783](https://github.com/SqueakyFloors/ubertooth/issues/783)
- The paged board's `LMP_setup_complete` carries the transaction ID the
  specification gives a transaction the peripheral started, where it carried
  the one for the central's. Two Ubertooths are lenient about it; a standard
  controller may not be.
  [#1074](https://github.com/SqueakyFloors/ubertooth/issues/1074)
- `l2ping` sends nothing on the signaling channel before the link setup has
  finished or after this end has hung up, which is what the board in
  `page_scan` already did.
  [#1074](https://github.com/SqueakyFloors/ubertooth/issues/1074)
- `ubertooth-btle -p` times a connection's returns to the channel it is sitting
  on more closely. It stamps each return at the moment the packet began to
  arrive rather than after the whole packet had been read and checked, which on
  a connection carrying full-length packets is 2 ms later; and it treats a run
  of packets less than one connection interval apart as the single connection
  event it is, however long that event runs. A busy event of full-length
  packets is longer than the shortest interval, so part way through it used to
  be taken for the start of the next one.
- Giving up a followed low-energy connection takes one path rather than two.
  The host's request to stop following and the end of the connection itself
  did the same three things in blocks of their own, and a request that arrived
  in the same connection event as the end of the connection was left set
  behind the follow it had asked to stop.
- The range test's repeater lets its last reply finish on the air before it
  switches the radio off, as the sender already did, so the reply burst is
  sixteen whole packets at the top amplifier level and not fifteen.
- An E-GO capture length set by one run no longer stays on the board for the
  next one, which the access code already did not. The tool states a length on
  every run, so this shows only to another program talking to the board.
- A low-energy capture's dump holds the four records promiscuous mode works a
  connection's parameters out from -- the access address, CRC init, hop
  interval, and hop increment -- which the low-energy callback used to return
  on before it reached the write. They are the only record of which connection
  a promiscuous capture belongs to, so a replay of such a dump had the packets
  and no way to say what they belonged to. No tool pairs a dump with the
  low-energy callback today, so nothing a user runs changes.
- `ubertooth-dfu` reads a board that left the bus between being listed and
  being opened as a board that is not in the bootloader, which is what it is,
  rather than ending the command with the USB code for it. A board still
  re-enumerating is looked for again, as the matching search for a board
  running its application already did.
- The search for an access code in a bank of captured symbols tells the library
  how many symbols the bank holds, so the library checks the search bound
  rather than taking the caller's word for it.
- The clock a Classic capture is following, the trim it sends the board to hold
  that clock, and how long its clock search has run belong to the board being
  read rather than to the process, and go back to where a capture starts when
  one ends. One board behaves as it did; a capture reading several would have
  measured each board's correction against another board's history.
  libubertooth's ubertooth_follow_quiet_ms() and ubertooth_clock_search_ms()
  take the handle to ask about as a result, and
  ubertooth_bredr_follow_reset() is new beside them.
  [#783](https://github.com/SqueakyFloors/ubertooth/issues/783)
- The LED demo mode (`ubertooth-util -x`) arms its blink timer from a clean
  start and disarms it on the way out, so a second run of it cannot toggle an
  LED before its own first blink is due.
  [#884](https://github.com/SqueakyFloors/ubertooth/issues/884)
- A capture started against a board a killed run had left streaming no longer
  opens with that run's packets: the tools that read the stream endpoint stop
  the board and wait for it to go idle first, and say on standard error that
  they did.
  [#438](https://github.com/SqueakyFloors/ubertooth/issues/438)
- A low-energy connection request that names the advertising access address as
  the connection's own, which the specification does not allow, is refused
  rather than followed. Following one left the board tuned to the advertising
  channels instead of the connection's data channels.
  [#933](https://github.com/SqueakyFloors/ubertooth/issues/933)
- `btbr` clears the radio's pending interrupt by writing the pin's bit, rather
  than reading a register that clears whichever bits are written to it and
  writing the answer back.
  [#1011](https://github.com/SqueakyFloors/ubertooth/issues/1011)
- A `ubertooth-btbr` session refuses a peer's role switch instead of passing
  it over in silence, answers a request to pair under the extended opcode it
  really arrives with, and sends each answer under the request's own
  transaction number so the peer can match it.
  [#942](https://github.com/SqueakyFloors/ubertooth/issues/942)
- The LED that marks a packet in a low-energy capture is timed by a counter of
  its own rather than by the timer the radio's own deadlines live on, so the
  capture loop no longer writes that timer to light an LED. The blink is the
  same length as before.
  [#505](https://github.com/SqueakyFloors/ubertooth/issues/505)
- A followed connection keeps its event grid when a packet arrives at the very
  end of a connection event, which the specification allows. Such a packet was
  counted into the event that followed it: that armed a 300 microsecond timeout
  over the next event's own start, and left the connection's event counter one
  ahead of the peer's, so every later connection update and channel map change
  was applied an event early.
  [#884](https://github.com/SqueakyFloors/ubertooth/issues/884)
- A channel map or connection parameter change is marked as waiting for its
  instant only once all of it has been read, so the event that applies it
  cannot pick up a half-read map or a mixture of two updates. A map applied
  half-read left the receiver tuned where the peer was not for the rest of the
  connection.
  [#884](https://github.com/SqueakyFloors/ubertooth/issues/884)
- A request that carries more data to the board than the buffer its handler
  registered is refused, instead of having the rest of it written over whatever
  follows that buffer. The length a request may declare is sixteen bits and the
  largest of those buffers holds 258 bytes, so the reach was about 65 KiB. It
  covers every kind of request the firmware answers, vendor, standard, class
  and the bootloader's; no shipped tool sends such a request.
  [#391](https://github.com/SqueakyFloors/ubertooth/issues/391)
- A generic transmit puts the radio registers it used back, so a receive
  entered afterwards runs the mode the host asked for rather than the one the
  transmit left set, on the channel the host chose. No shipped tool asks for
  the mode.
  [#352](https://github.com/SqueakyFloors/ubertooth/issues/352)
- The generic receive lets the radio write the first byte of a packet before it
  starts reading the packet out, so byte 0 of a record is the byte that
  arrived. The same wait fixed the range test's wrong byte 0.
  [#352](https://github.com/SqueakyFloors/ubertooth/issues/352)
- The range test's wait for that first byte is no longer cut short by a clock
  period the drift correction has lengthened. The wait measured itself against
  a period of 312.5 us, the usual one, so a reading taken just after a
  lengthened period reset was read as up to 167 us of waiting that had not
  happened, which left byte 0 of that reply read before the radio had written
  it.
  [#870](https://github.com/SqueakyFloors/ubertooth/issues/870)
- Both of those waits now take the time that has passed from the direction the
  clock counter moved, so neither can be cut short by a clock period of any
  length. Measuring against a period of 312.5 us covered the two lengths the
  drift correction produces, but a period the host had set to a length of its
  own could still be read as a whole lead of waiting that had not happened.
  The host has to have moved the period, and the wait has to fall inside it.
  [#887](https://github.com/SqueakyFloors/ubertooth/issues/887)
- A capture keeps a packet whose transfer brought all 64 bytes and came back
  with the timed-out status rather than the completed one. It used to be
  dropped, and the warning about a transfer cut short passes over a full one,
  so a packet the host already held was lost with nothing said.
  [#638](https://github.com/SqueakyFloors/ubertooth/issues/638)
- Low-energy capture dewhitens a captured packet four bytes at a time. The
  first 48 bytes used to come from a table of whitening words and the rest a
  bit at a time, so a Data Length Extension payload cost about 1700 lookups
  between two packets on the air; it is now one path costing about sixty word
  operations, and the image is 2.1 KB smaller.
  [#600](https://github.com/SqueakyFloors/ubertooth/issues/600)
- A connection update the promiscuous mode follows has to carry the fields it
  is read for. A shorter control packet claiming to be one had them read from
  whatever the receive buffer held before it, and the connection followed from
  there.
- The connection-follow callback reads its payload length as the whole byte a
  low-energy header holds, and keeps the read of the packet's CRC inside the
  bytes it decoded. Nothing reaches the callback today; it is the half of
  `ubertooth-btle -p` that follows a connection, waiting for the work that
  would call it again.
  [#597](https://github.com/SqueakyFloors/ubertooth/issues/597)
- The thread that completes a capture's transfers treats a wait a signal cut
  short as a wait to take again. It used to end the thread, which left the
  capture with nobody to complete its transfers and no line to say why; the
  same wait during a capture's teardown now finishes as well, instead of
  leaving the transfer behind for the next capture on that handle to refuse.
  [#584](https://github.com/SqueakyFloors/ubertooth/issues/584)
- The board refuses a raw request to set a modulation no capture mode
  configures, and a raw request for a frequency the radio cannot tune. The
  frequency used to be pulled up to the lowest the radio can reach and reported
  as a success, so the capture came back from somewhere nobody had asked for.
  The band sweep and the out-of-band frequencies the radio can reach still
  work, and no host tool ever sent either refused value.
  [#818](https://github.com/SqueakyFloors/ubertooth/issues/818)
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
- `ubertooth-rx -l` and `ubertooth-follow` crashed instead of saying anything
  when the piconet they follow could not be allocated. Both now report
  `error: out of memory` and exit 1, as `ubertooth-afh` already did, and
  `ubertooth-follow` allocates the piconet once the command line has been
  accepted, so `-h` and `-V` still answer.
  [#987](https://github.com/SqueakyFloors/ubertooth/issues/987)
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
- Every field of the low-energy capture state is accounted for where a mode
  start clears it. One of them was cleared twice over, which read as a line
  meant for another field; none was in fact left behind, and a test now fails
  if a field is added to the state and left out of the clear.
  [#820](https://github.com/SqueakyFloors/ubertooth/issues/820)
- A low-energy packet the board finished receiving as a capture transfer
  faulted reaches the host with the fault marked on it, as the board's other
  capture loops have always marked their own. The packet is kept either way,
  and no shipped tool reads the mark yet, so nothing in a capture looks
  different today.
  [#849](https://github.com/SqueakyFloors/ubertooth/issues/849)
- A promiscuous capture starts following the connection it has worked out
  without a stray retune. The board set the follow up in two steps, and a
  clock tick landing between them could ask for a hop before there was a
  connection to time it, which cost the capture its place and left it waiting
  for the search to run again.
  [#745](https://github.com/SqueakyFloors/ubertooth/issues/745)
- A bootloader update whose read-back the part refuses erases the sector
  holding the vector table again, so the board comes up in the part's serial
  programmer, which a serial adapter recovers, instead of starting a
  bootloader the part declined to confirm. A part that refuses that erase as
  well is left as it was.
  [#1127](https://github.com/SqueakyFloors/ubertooth/issues/1127)

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
