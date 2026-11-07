# Changelog

## 2020-12-R1+modern1

Contents:

- [Highlights](#highlights) · [Stability and lockups](#stability-and-lockups) · [Across all tools](#across-all-tools)
- Tools: [ubertooth-util](#ubertooth-util) · [ubertooth-dfu](#ubertooth-dfu) · [ubertooth-btle](#ubertooth-btle) · [btle-extcap.py](#btle-extcappy) · [ubertooth-rx](#ubertooth-rx) · [ubertooth-afh](#ubertooth-afh) · [ubertooth-follow](#ubertooth-follow) · [ubertooth-scan](#ubertooth-scan) · [ubertooth-btbr](#ubertooth-btbr) · [ubertooth-specan](#ubertooth-specan) · [ubertooth-specan-ui](#ubertooth-specan-ui) · [ubertooth-dump](#ubertooth-dump) · [ubertooth-debug](#ubertooth-debug) · [ubertooth-ducky](#ubertooth-ducky) · [ubertooth-ego](#ubertooth-ego)
- [Firmware](#firmware) · [Libraries](#libraries) · [Build and compatibility](#build-and-compatibility) · [Documentation](#documentation) · [Under the hood](#under-the-hood)

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
- **Firmware you can install, identify, and get back from.** Images carry an info block, so `ubertooth-dfu` names what it will write and refuses a bad file before the board leaves its firmware.
  ([#1064](https://github.com/SqueakyFloors/ubertooth/issues/1064), [#1062](https://github.com/SqueakyFloors/ubertooth/issues/1062))
- **The bootloader updates over USB, no programmer needed.** `ubertooth-dfu --update-bootloader` (or `install.sh --update-bootloader`) installs this release's bootloader, which recovers a board from an interrupted firmware download, refuses to start a half-written image, and reports its own version to `ubertooth-util -B`. A board that keeps its old bootloader runs everything else in this release.
  ([#1056](https://github.com/SqueakyFloors/ubertooth/issues/1056), [#1123](https://github.com/SqueakyFloors/ubertooth/issues/1123), [#242](https://github.com/SqueakyFloors/ubertooth/issues/242))
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

### ubertooth-util

#### New features

- `ubertooth-util -B` reports the bootloader's version and revision, or, for
  a board as shipped, a hash that tells such bootloaders apart.
- **The two-board range test works.** `ubertooth-util -n` against a second
  board running `ubertooth-util -e` reports the power levels each way within a
  couple of seconds (before, it reported nothing). It runs to a result
  instead of only starting the test, gives up after 10 seconds (`-n
  <seconds>` to change, `-n 0` to wait forever), ignores other boards'
  traffic, stops on Ctrl-C, and keeps its progress on standard error.
- **Faster identify blink.** `ubertooth-util -I` blinks the LEDs faster, and
  stops on `-S`.
- **Script-visible change.** `ubertooth-util -f` (use `ubertooth-dfu`) and
  `-9` are removed. `ubertooth-util -V` prints the standard version lines and
  ends the command; `-v` still prints the `Firmware version:` line alone, and
  is deprecated in favor of `-V`.
- **Script-visible change.** `ubertooth-util` refuses, before opening a board,
  a word that is not an option, two mode options, `-z` with a mode, `-N` with
  `-U`, a command line that asks for nothing, and out-of-range values (for
  example `-a` outside 0-7, or `-q` outside 20-120; bare `-q` uses -85 dBm).
  `ubertooth-util -N` prints the count and then runs the rest of the command.

([#1056](https://github.com/SqueakyFloors/ubertooth/issues/1056), [#649](https://github.com/SqueakyFloors/ubertooth/issues/649), [#26](https://github.com/SqueakyFloors/ubertooth/issues/26), [#483](https://github.com/SqueakyFloors/ubertooth/issues/483), [#686](https://github.com/SqueakyFloors/ubertooth/issues/686), [#784](https://github.com/SqueakyFloors/ubertooth/issues/784), [#694](https://github.com/SqueakyFloors/ubertooth/issues/694), [#1026](https://github.com/SqueakyFloors/ubertooth/issues/1026), [#190](https://github.com/SqueakyFloors/ubertooth/issues/190), [#338](https://github.com/SqueakyFloors/ubertooth/issues/338), [#167](https://github.com/SqueakyFloors/ubertooth/issues/167), [#635](https://github.com/SqueakyFloors/ubertooth/issues/635), [#333](https://github.com/SqueakyFloors/ubertooth/issues/333), [#21](https://github.com/SqueakyFloors/ubertooth/issues/21), [#542](https://github.com/SqueakyFloors/ubertooth/issues/542), [#146](https://github.com/SqueakyFloors/ubertooth/issues/146), [#82](https://github.com/SqueakyFloors/ubertooth/issues/82), [#159](https://github.com/SqueakyFloors/ubertooth/issues/159), [#481](https://github.com/SqueakyFloors/ubertooth/issues/481), [#367](https://github.com/SqueakyFloors/ubertooth/issues/367), [#172](https://github.com/SqueakyFloors/ubertooth/issues/172), [#161](https://github.com/SqueakyFloors/ubertooth/issues/161), [#189](https://github.com/SqueakyFloors/ubertooth/issues/189), [#332](https://github.com/SqueakyFloors/ubertooth/issues/332))

#### Fixes

- **Wrong or hidden results.** A failed read printed an error code as the
  value (or `Part ID: 0`) and could exit 0; it now names the failed read and
  exits 1, and a later success no longer hides an earlier failure. A set and
  a read of one value in a command (`ubertooth-util -c 2426 -c`) both run, and
  `ubertooth-util -m` no longer reports an earlier range test's numbers as the
  latest.
- **Reset and programmer mode.** `ubertooth-util -r` waits up to 10 seconds
  for the board to come back (macOS can be slow), and later options in the
  same command stay on the board that was reset. `ubertooth-util -i` reports
  success instead of a timeout, and says the board stays in its programmer
  until its power is cut.
- **Leftover state.** `ubertooth-util` stops a previous operation before
  starting a new mode, checks frequency and channel values, and releases the
  board when it is done without stopping a running mode.

([#59](https://github.com/SqueakyFloors/ubertooth/issues/59), [#123](https://github.com/SqueakyFloors/ubertooth/issues/123), [#52](https://github.com/SqueakyFloors/ubertooth/issues/52), [#51](https://github.com/SqueakyFloors/ubertooth/issues/51), [#40](https://github.com/SqueakyFloors/ubertooth/issues/40), [#228](https://github.com/SqueakyFloors/ubertooth/issues/228), [#94](https://github.com/SqueakyFloors/ubertooth/issues/94), [#139](https://github.com/SqueakyFloors/ubertooth/issues/139), [#306](https://github.com/SqueakyFloors/ubertooth/issues/306), [#508](https://github.com/SqueakyFloors/ubertooth/issues/508), [#22](https://github.com/SqueakyFloors/ubertooth/issues/22), [#5](https://github.com/SqueakyFloors/ubertooth/issues/5), [upstream #448](https://github.com/greatscottgadgets/ubertooth/issues/448), [#117](https://github.com/SqueakyFloors/ubertooth/issues/117))

### ubertooth-dfu

#### New features

- **Install the bootloader over USB.** `ubertooth-dfu --update-bootloader -d
  bl_update.dfu` installs this release's bootloader with no serial programmer.
  It writes nothing when the board already has that bootloader (`--force`
  writes anyway), then reads it back and reports what the board holds. Keep a
  serial adapter to hand: an interrupted bootloader write needs one.
- **Script-visible change.** All of `ubertooth-dfu`'s progress goes to
  standard error. Standard output carries only the revision `ubertooth-dfu -d
  <image> -r` verified after the reset, and every failure is one `error:`
  line.

([#1123](https://github.com/SqueakyFloors/ubertooth/issues/1123), [#1128](https://github.com/SqueakyFloors/ubertooth/issues/1128), [#502](https://github.com/SqueakyFloors/ubertooth/issues/502), [#481](https://github.com/SqueakyFloors/ubertooth/issues/481))

#### Fixes

- **Interrupted downloads are recoverable.** If a download stops part way (a
  cable pulled, the board drops off the bus, the host dies), the board waits
  in DFU for the next `ubertooth-dfu -d` instead of needing a bench repair.
  This needs this release's bootloader (see `--update-bootloader` above).
- **Bad images refused before flashing.** `ubertooth-dfu -d` checks the whole
  image while the board is still running its firmware. A corrupt, truncated,
  unsigned, empty, or oversized image, one for another device, or a path that
  is not a file is refused with exit 1, and the board keeps running. An image
  built for a different firmware API version gets a warning.
- **Honest flash results.** `ubertooth-dfu -d <image> -r` checks that the
  right image and revision came back and says which outcome it hit. A good
  flash no longer fails on macOS, and a board that stayed in the bootloader
  is no longer reported as detached (confirming a reset takes about 1.5
  seconds longer).
- **Boards that could not be flashed.** A board with a bootloader built from
  this tree could not be flashed over USB, a board running `btbr` could not be
  reached with `-U <serial>`, and flashing `btbr` stopped for want of a
  revision. All three now work.
- **Files and notes.** `ubertooth-dfu -s` refuses an already-signed file and
  writes `<file>.dfu` only once it is complete, and `-s` and `-d` only read
  their input. `ubertooth-dfu -u` reads back only the flash the part has,
  writes to a file (not standard output), and never empties an earlier file
  on failure. A board left in the bootloader gets a note naming why and what
  to run.

([#242](https://github.com/SqueakyFloors/ubertooth/issues/242), [#1039](https://github.com/SqueakyFloors/ubertooth/issues/1039), [#1041](https://github.com/SqueakyFloors/ubertooth/issues/1041), [#198](https://github.com/SqueakyFloors/ubertooth/issues/198), [#199](https://github.com/SqueakyFloors/ubertooth/issues/199), [#246](https://github.com/SqueakyFloors/ubertooth/issues/246), [#281](https://github.com/SqueakyFloors/ubertooth/issues/281), [#557](https://github.com/SqueakyFloors/ubertooth/issues/557), [#579](https://github.com/SqueakyFloors/ubertooth/issues/579), [#267](https://github.com/SqueakyFloors/ubertooth/issues/267), [#283](https://github.com/SqueakyFloors/ubertooth/issues/283), [#304](https://github.com/SqueakyFloors/ubertooth/issues/304), [#491](https://github.com/SqueakyFloors/ubertooth/issues/491), [#475](https://github.com/SqueakyFloors/ubertooth/issues/475), [#433](https://github.com/SqueakyFloors/ubertooth/issues/433), [#200](https://github.com/SqueakyFloors/ubertooth/issues/200), [#305](https://github.com/SqueakyFloors/ubertooth/issues/305), [#286](https://github.com/SqueakyFloors/ubertooth/issues/286), [#241](https://github.com/SqueakyFloors/ubertooth/issues/241), [#233](https://github.com/SqueakyFloors/ubertooth/issues/233), [#1170](https://github.com/SqueakyFloors/ubertooth/issues/1170), [#1133](https://github.com/SqueakyFloors/ubertooth/issues/1133))

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

- **Send chosen packet types.** With a connection up, the `ubertooth-btbr`
  command `tx <format> [<length>] [<count>] [<hex>]` sends one packet of a
  chosen type (AUX1, DV, HV, EV, or DM and DH at any length), for testing a
  Classic decoder against packets real stacks rarely send. See
  ubertooth-btbr(1), TRANSMITTING CHOSEN PACKET TYPES.
- **Choose a board.** `ubertooth-btbr -U <index|serial>` picks one of several
  boards. With several attached and no `-U`, it lists them and opens none
  (before, it opened whichever came first). `ubertooth-btbr -h` prints usage.
- **More of a session is visible.** `ubertooth-btbr` prints the LMP packets a
  connection decodes, says why a peer hung up (`Peer detached: reason 0x13,
  the peer's user ended the connection`), and names the LMP power-control
  packets correctly.
- **Script-visible change.** A session's output is a clean packet log:
  packets and inquiry responses go to standard output, one line each with the
  time in front (`13:54:02 RX LMP ...`). The prompt, help, progress, and
  plain `warning:` and `error:` lines go to standard error, with no color
  codes when redirected.
- **Script-visible change.** `ubertooth-btbr -h` exits 0, and unknown prompt
  words, short addresses, and out-of-range `-f` (0-63) and `-b` (0-3) values
  are refused. A session exits 1 when it loses the board, leaves it held, or
  loses its standard output.

([#736](https://github.com/SqueakyFloors/ubertooth/issues/736), [#642](https://github.com/SqueakyFloors/ubertooth/issues/642), [#410](https://github.com/SqueakyFloors/ubertooth/issues/410), [#735](https://github.com/SqueakyFloors/ubertooth/issues/735), [#951](https://github.com/SqueakyFloors/ubertooth/issues/951), [#950](https://github.com/SqueakyFloors/ubertooth/issues/950), [#800](https://github.com/SqueakyFloors/ubertooth/issues/800), [#874](https://github.com/SqueakyFloors/ubertooth/issues/874), [#943](https://github.com/SqueakyFloors/ubertooth/issues/943), [#908](https://github.com/SqueakyFloors/ubertooth/issues/908), [#1075](https://github.com/SqueakyFloors/ubertooth/issues/1075), [#1142](https://github.com/SqueakyFloors/ubertooth/issues/1142), [#737](https://github.com/SqueakyFloors/ubertooth/issues/737), [#920](https://github.com/SqueakyFloors/ubertooth/issues/920), [#959](https://github.com/SqueakyFloors/ubertooth/issues/959))

#### Fixes

- **Connections to real devices dropped.** A connection was cut after exactly
  30 seconds, a request `ubertooth-btbr` did not handle or a refused opening
  request left the link to time out, and a declined connection was held until
  link supervision ended it.
- **Two Ubertooths could not connect to each other.** A board in `page_scan` missed about
  half of all pages and gave up after one lost page response, `page` and
  `page_scan` reported connections that were never made, and a dropped
  transmit slot cost the link a repeat or put its hopping out of step. Two
  boards now page each other, and `l2ping` between them gets a reply.
- **Corrupt packets.** Received payloads of 247 bytes or more failed their
  checksum, transmits of 77 bytes or more went out corrupt, and headers from
  other piconets were accepted.
- **Crashes and hangs.** Short or malformed LMP packets, unreadable board
  messages, cut-off inquiry responses, and non-ASCII device names ended the
  session. Ctrl-C now returns to the `bt>` prompt, a board still running
  `bluetooth_rxtx` is named instead of hanging, a board in use by another
  program is reported as such, and `-V` and `-h` work without the Python
  `libusb1` package.

([#942](https://github.com/SqueakyFloors/ubertooth/issues/942), [#949](https://github.com/SqueakyFloors/ubertooth/issues/949), [#957](https://github.com/SqueakyFloors/ubertooth/issues/957), [#1074](https://github.com/SqueakyFloors/ubertooth/issues/1074), [#807](https://github.com/SqueakyFloors/ubertooth/issues/807), [#1156](https://github.com/SqueakyFloors/ubertooth/issues/1156), [#1168](https://github.com/SqueakyFloors/ubertooth/issues/1168), [#1166](https://github.com/SqueakyFloors/ubertooth/issues/1166), [#1191](https://github.com/SqueakyFloors/ubertooth/issues/1191), [#667](https://github.com/SqueakyFloors/ubertooth/issues/667), [#668](https://github.com/SqueakyFloors/ubertooth/issues/668), [#706](https://github.com/SqueakyFloors/ubertooth/issues/706), [#1036](https://github.com/SqueakyFloors/ubertooth/issues/1036), [#960](https://github.com/SqueakyFloors/ubertooth/issues/960), [#971](https://github.com/SqueakyFloors/ubertooth/issues/971), [#978](https://github.com/SqueakyFloors/ubertooth/issues/978), [#988](https://github.com/SqueakyFloors/ubertooth/issues/988), [#1](https://github.com/SqueakyFloors/ubertooth/issues/1), [#418](https://github.com/SqueakyFloors/ubertooth/issues/418), [#552](https://github.com/SqueakyFloors/ubertooth/issues/552), [#898](https://github.com/SqueakyFloors/ubertooth/issues/898), [#701](https://github.com/SqueakyFloors/ubertooth/issues/701))

### ubertooth-specan

- **Script-visible change.** `ubertooth-specan -d` chooses only where the
  samples go, and `-g` or `-G` how they are written, so `ubertooth-specan -g
  -d out.dat` writes gnuplot text to the file (before, the file stayed empty).
  `-d -` streams to standard output but refuses a terminal, and any other
  value, even one starting with `-`, is a file name.
- **`ubertooth-specan -v` works.** It prints the board, the sweep, where the
  samples go, and a summary once a second, on standard error (before, it did
  nothing).
- **Fixed: sweeps measured through the wrong settings.** A sweep after
  another mode used that mode's radio front-end settings and could include
  samples left over from an earlier run; it now starts from a clean board.
  `ubertooth-specan -l` and `-u` must be valid, ordered frequencies in the
  band.
- **Fixed: unclear errors.** A `-d` file that cannot be written stops the
  sweep and names the failure (for example, no space left on device), and a
  bad option gets the same error wording as every other tool. Every tool also
  stops the board cleanly on SIGHUP and SIGALRM.

([#481](https://github.com/SqueakyFloors/ubertooth/issues/481), [#231](https://github.com/SqueakyFloors/ubertooth/issues/231), [#259](https://github.com/SqueakyFloors/ubertooth/issues/259), [#510](https://github.com/SqueakyFloors/ubertooth/issues/510), [#471](https://github.com/SqueakyFloors/ubertooth/issues/471), [#173](https://github.com/SqueakyFloors/ubertooth/issues/173), [#37](https://github.com/SqueakyFloors/ubertooth/issues/37), [#655](https://github.com/SqueakyFloors/ubertooth/issues/655), [#338](https://github.com/SqueakyFloors/ubertooth/issues/338))

### ubertooth-specan-ui

- **BLE channel labels.** Press `n` in `ubertooth-specan-ui` to label the axis
  and markers with BLE channel numbers, with the advertising channels 37, 38,
  and 39 always named, and `n` again for megahertz. Credit: Emmanuel Blot
  ([@eblot](https://github.com/eblot)).
- **Average trace.** A slowly smoothed average of the max-hold trace is drawn
  as a cyan dotted line labeled "Avg: N dBm". Credit: Sirio Balmelli
  ([@siriobalmelli](https://github.com/siriobalmelli)).
- **Easier to use.** Keys are shown on screen (press `h`), frequencies are
  labeled in whole MHz, marker labels no longer linger, the plot is slightly
  wider than the scanned range, and there is a man page.
- **Script-visible change.** `ubertooth-specan-ui` refuses an unknown option
  or stray argument with exit 1 (before, it ignored it and swept). A missing
  `numpy` or Qt binding, or a bad `-l` or `-u` value, gets one `error:` line
  instead of a traceback.
- **Fixed: nothing plotted.** `ubertooth-specan-ui` passed arguments
  `ubertooth-specan` rejected, so it drew nothing. Its `-U` now takes an index
  or serial, pressing `C` before data arrives no longer crashes it, and
  `ubertooth-specan-ui --wifi 14` sits at 2484 MHz (before, 2482).

([#76](https://github.com/SqueakyFloors/ubertooth/issues/76), [#745](https://github.com/SqueakyFloors/ubertooth/issues/745), [#75](https://github.com/SqueakyFloors/ubertooth/issues/75), [#125](https://github.com/SqueakyFloors/ubertooth/issues/125), [#126](https://github.com/SqueakyFloors/ubertooth/issues/126), [#934](https://github.com/SqueakyFloors/ubertooth/issues/934), [#124](https://github.com/SqueakyFloors/ubertooth/issues/124), [#673](https://github.com/SqueakyFloors/ubertooth/issues/673))

### ubertooth-dump

- **Script-visible change.** `ubertooth-dump` refuses to write its symbol
  stream to a terminal, with or without `-d -`, and the refusal names what it
  would write (binary data, or the `-b` bitstream).
- **Fixed: dumps in the wrong modulation.** When the board refused the
  modulation `ubertooth-dump -c` or `-l` asked for, the dump ran anyway in
  whatever modulation the board was left in and exited 0. It now stops with an
  error and exit 1.

([#481](https://github.com/SqueakyFloors/ubertooth/issues/481), [#945](https://github.com/SqueakyFloors/ubertooth/issues/945))

### ubertooth-debug

- **Fixed: wrong register values.** `ubertooth-debug` read fourteen CC2400
  fields with too narrow a mask and two from a neighbor's bits; every field
  now shows its whole value, and register `0x15` answers to its own name,
  `%MDMTST1`.
- **Fixed: unclear failures.** `ubertooth-debug` names a board running the
  `btbr` image instead of failing on the first read, names each register read
  that fails, and exits 0 only when every read succeeds (before, it exited
  with the last register's value). `ubertooth-debug -r` states the range it
  accepts and quotes the item it refused, and a `%name` works anywhere in a
  list or range.

([#671](https://github.com/SqueakyFloors/ubertooth/issues/671), [#808](https://github.com/SqueakyFloors/ubertooth/issues/808), [#164](https://github.com/SqueakyFloors/ubertooth/issues/164), [#235](https://github.com/SqueakyFloors/ubertooth/issues/235))

### ubertooth-ducky

- `ubertooth-ducky -U` selects a board by index or serial number.
- **Fixed: refused requests reported as success.** `ubertooth-ducky -q` and
  `-b` now name a request the board refused and exit 1 (before, exit 0).

([#36](https://github.com/SqueakyFloors/ubertooth/issues/36), [#328](https://github.com/SqueakyFloors/ubertooth/issues/328))

### ubertooth-ego

- **Script-visible change.** `ubertooth-ego` refuses options that do nothing
  in the chosen mode: `-l` with `-i` or `-j`, and `-c` with `-f`, `-i`, or
  `-j`. Malformed or out-of-range lengths are refused before a board is
  opened.
- **Fixed: settings carried over from an earlier run.** After an interrupted
  run, `ubertooth-ego` could capture in the previous mode, on the previous
  channel, or with the previous access code. Each run now starts in the mode
  and settings it asks for, a short `-a` code is zero-filled, and a setting
  the board refuses stops the run with exit 1.
- **Fixed: damaged records and gaps.** Every record ended with two bytes of
  its own timestamp, and `delta_t` could be short by whole clock periods
  after a quiet spell.

([#411](https://github.com/SqueakyFloors/ubertooth/issues/411), [#394](https://github.com/SqueakyFloors/ubertooth/issues/394), [#39](https://github.com/SqueakyFloors/ubertooth/issues/39), [#757](https://github.com/SqueakyFloors/ubertooth/issues/757), [#397](https://github.com/SqueakyFloors/ubertooth/issues/397), [#402](https://github.com/SqueakyFloors/ubertooth/issues/402), [#379](https://github.com/SqueakyFloors/ubertooth/issues/379), [#749](https://github.com/SqueakyFloors/ubertooth/issues/749), [#981](https://github.com/SqueakyFloors/ubertooth/issues/981))

### Firmware

#### bluetooth_rxtx

- The receive-only build (`DISABLE_TX=1`) names itself `bluetooth_rx`, so
  `ubertooth-util -v` and `ubertooth-dfu` tell it from the full image. A build
  from an unpacked source release still reports `bluetooth_rxtx`.
- **Fixed: Ubertooth Zero and ToorCon badge.** The firmware read the radio's
  status pin on the wrong port, so on the Zero a packet longer than 32 bytes
  went out cut short. The Ubertooth One was unaffected.
- **Fixed: lost or stale USB replies.** A control request the board was
  briefly slow to report went unanswered, which could end a `ubertooth-btle`
  capture, and a request could be answered with the previous one's leftovers.
- **Fixed: signal and CRC handling.** RSSI averaging rounds negative dBm
  correctly, and BLE CRC filtering and verification run in the physical-layer
  engine. Credit: oliver ([@Oliver0804](https://github.com/Oliver0804))
  ([upstream #550](https://github.com/greatscottgadgets/ubertooth/pull/550)).

([#1006](https://github.com/SqueakyFloors/ubertooth/issues/1006), [#1007](https://github.com/SqueakyFloors/ubertooth/issues/1007), [#714](https://github.com/SqueakyFloors/ubertooth/issues/714), [#462](https://github.com/SqueakyFloors/ubertooth/issues/462), [#15](https://github.com/SqueakyFloors/ubertooth/issues/15))

#### btbr

- **Script-visible change.** The `btbr` image reports the firmware API
  version of the tree it was built from (before, a fixed 1.06), so other
  tools no longer refuse it over its version.
- **Paging and scanning that meet.** Page scan and inquiry scan hold the one
  frequency the specification gives for its full 1.28 seconds, watch through
  nearly the whole slot (before, its first 25 microseconds), and hop on the
  channels the specification selects, AFH included. Paging is faster and more
  reliable. Credit: Hillel Chimowicz ([@hllhll](https://github.com/hllhll))
  and [@shapaz](https://github.com/shapaz)
  ([upstream #540](https://github.com/greatscottgadgets/ubertooth/pull/540)).
- **Fixed: sessions that went silent forever.** A full scheduler slot, a
  radio not tuned in time, or an unexpected packet could leave a connection,
  page, inquiry, scan, or `monitor` waiting forever, or stop the firmware.
  Each now reports the dropped packet or the ended session and carries on.
- **Fixed: packets and settings.** DM1, DM3, and DM5 packets whose payload is
  not a multiple of five bytes went out a few bits short, so some payload
  lengths failed their checksum at the receiver. **A board running an older
  `btbr` build will not interoperate with this one for those lengths, and
  nothing warns about a mixed pair: reflash both boards.** The image also keeps the power level set
  with `ubertooth-util -a`, prints addresses with their leading zeros, and is
  not built for boards whose radio pin cannot raise its interrupt (the
  Ubertooth Zero and the ToorCon badge).

([#556](https://github.com/SqueakyFloors/ubertooth/issues/556), [#811](https://github.com/SqueakyFloors/ubertooth/issues/811), [#669](https://github.com/SqueakyFloors/ubertooth/issues/669), [#13](https://github.com/SqueakyFloors/ubertooth/issues/13), [#1139](https://github.com/SqueakyFloors/ubertooth/issues/1139), [#1151](https://github.com/SqueakyFloors/ubertooth/issues/1151), [#1174](https://github.com/SqueakyFloors/ubertooth/issues/1174), [#1167](https://github.com/SqueakyFloors/ubertooth/issues/1167), [#745](https://github.com/SqueakyFloors/ubertooth/issues/745), [#706](https://github.com/SqueakyFloors/ubertooth/issues/706), [#181](https://github.com/SqueakyFloors/ubertooth/issues/181), [#1011](https://github.com/SqueakyFloors/ubertooth/issues/1011))

#### The bootloader

- **Install the bootloader over USB.** `make -C firmware bl_update` builds an
  updater image, `ubertooth-dfu --update-bootloader -d bl_update.dfu`
  installs it, and `ubertooth-util -B` confirms it. Keep a serial adapter to
  hand: an interrupted bootloader write needs one. See ubertooth-dfu(1),
  UPDATING THE BOOTLOADER.
- **Updating is optional.** This release runs on whatever bootloader a board
  has. With this release's bootloader, a board also recovers over USB from an
  interrupted download, refuses to start a half-written or corrupt image,
  keeps downloads and read-backs within the flash it has, and reports its own
  version to `ubertooth-util -B`.

([#1056](https://github.com/SqueakyFloors/ubertooth/issues/1056), [#242](https://github.com/SqueakyFloors/ubertooth/issues/242), [#354](https://github.com/SqueakyFloors/ubertooth/issues/354), [#439](https://github.com/SqueakyFloors/ubertooth/issues/439))

### Libraries

#### libubertooth

- **Choose where results go.** A program sets the `result_stream` field of
  `ubertooth_t` (read with `ubertooth_result_stream()`) to say where a
  capture's results are written; unset, they go to standard output as before.
  `ubertooth_print_afh_map()` prints an AFH map to a chosen stream.
- **Deprecated.** `register_cleanup_handler()` (use
  `ubertooth_catch_stop_signals()`; it is removed in the next release) and
  `cmd_get_access_address()` (use `cmd_read_access_address()`). Both still
  work; GCC and Clang warn, which stops a `-Werror` build.
- **Fixed: silent failures.** `cmd_le_set_adv_data()` refuses more than the
  31 bytes the board can advertise (before, it took up to 255 and the board
  dropped them), and `LE_ADV_MAX_LEN` gives way to `ADV_IND_MAX_DATA`. A short
  reply to a fixed-size read is an error instead of a value made from stale
  bytes.

([#745](https://github.com/SqueakyFloors/ubertooth/issues/745), [#752](https://github.com/SqueakyFloors/ubertooth/issues/752), [#149](https://github.com/SqueakyFloors/ubertooth/issues/149))

#### libbtbb

- **Built from this project's fork.** libbtbb comes from the
  [fork](https://github.com/SqueakyFloors/libbtbb) at a pinned commit, with no
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`; `install.sh --libbtbb-url` and
  `--libbtbb-ref` build another. A libbtbb missing what the tools use is
  refused at configure time, by name. What it is tested against is in its
  [TESTING.md](https://github.com/SqueakyFloors/libbtbb/blob/master/TESTING.md).
- **Fixed: decoding.** A 255-byte advertising packet lost its last three
  bytes, AUX1, DV, EV3, EV4, and EV5 payloads did not decode, and an
  unreadable payload was reported with the previous packet's bytes. All now
  decode correctly, and AFH hop re-mapping follows the Core specification.
- **Fixed: the Classic clock search stalled.** It kept searching for a UAP it
  already had, whether given as `0x00` or worked out from `ubertooth-rx -l
  <LAP>` alone, and worked through tens of thousands of candidates with no
  packets seen ([libbtbb issue 134](https://github.com/SqueakyFloors/libbtbb/issues/134), [libbtbb issue 136](https://github.com/SqueakyFloors/libbtbb/issues/136), [libbtbb issue 131](https://github.com/SqueakyFloors/libbtbb/issues/131)).
- **Captures and wording.** A BLE capture records each connection's
  parameters, a Classic record carries the header and payload of the packet
  that settled the piconet's address, the library's notes go to standard
  error, and it reports its release the way these tools do. Bluetooth roles
  read Central and Peripheral (for example `LL_PERIPHERAL_FEATURE_REQ`), in the
  Wireshark plugins too; display-filter field names are unchanged.

([#320](https://github.com/SqueakyFloors/ubertooth/issues/320), [#736](https://github.com/SqueakyFloors/ubertooth/issues/736), [#143](https://github.com/SqueakyFloors/ubertooth/issues/143))

### Build and compatibility

- **One-command install.** `install.sh` installs from source on Debian and
  Ubuntu, Fedora, and macOS with Homebrew: virtual environments, `--prefix`,
  `--skip-deps`, udev rules, firmware builds, flashing chosen boards with
  `--flash-device <index|serial|all>`, and installing the bootloader with
  `--update-bootloader` (a board that already has it is left alone).
  `install.sh --uninstall` removes what it installed, keeping libbtbb unless
  asked. It refuses Python older than 3.8 up front, rejects bad options in the
  tools' wording, and `install.sh --help` prints only the usage.
- **Current platforms.** macOS 15 (Sequoia) is the oldest macOS tested
  (macOS 14 should still build). Ubuntu 20.04 and 22.04 build again, CMake 4
  and current Homebrew work, and the host tools build with a strict C
  standard (`-std=c17`, `-std=c2x`). Python 3.12 and current setuptools are
  supported. Credit: Arnaud Rebillout
  ([@elboulangero](https://github.com/elboulangero)) and Rhys Perry
  ([@rhysperry111](https://github.com/rhysperry111)) ([upstream #524](https://github.com/greatscottgadgets/ubertooth/pull/524)).
  `ubertooth-scan` and `ubertooth-follow` stay Linux only; configuring on
  macOS says so and points at `ubertooth-rx` and `ubertooth-afh`.
- **libubertooth in your own code.** libubertooth installs where your system
  keeps libraries, beside libbtbb (`lib64` and multiarch included), with
  `libubertooth.pc` for pkg-config and `ubertoothConfig.cmake` for
  `find_package(ubertooth)` (see host/README.cmake.md). The tools build on
  their own against an installed libubertooth, and an outdated one is refused
  in one line. The Python tools install under the chosen prefix, with no
  `PYTHONPATH` needed.
- **Clear configure checks.** The configure step names the libbtbb it found
  and warns when it is not the pinned commit, refuses one that lacks
  functions the tools call (naming the commit to rebuild at), finds libraries
  again after they move (Homebrew upgrades included), lists the optional
  tools it enables (`-DREQUIRE_ALL=ON` makes a missing one fatal), and skips
  the Python tools with a warning when Python is older than 3.8.
- **Reliable firmware builds.** Each firmware image builds the shared
  sources on its own, editing a compile flag rebuilds the objects, and
  version strings update reliably. A release ships the `btbr` image beside
  `bluetooth_rxtx`, and images report the release rather than `unknown`. The
  firmware build's `ubertooth-dfu` path is corrected. Credit: Michael Ossmann
  ([@mossmann](https://github.com/mossmann)) ([upstream #485](https://github.com/greatscottgadgets/ubertooth/pull/485)).
- **Man pages and housekeeping.** Every man page is installed (the
  `ubertooth-debug`, `-ducky`, `-follow`, and `-tx` pages were missing), with
  or without ronn, and a build no longer rewrites the committed pages. The
  Dockerfile, `Jenkinsfile`, and `ci-scripts/` are removed; CI is the GitHub
  Actions workflows. Scripts read captures with Scapy's `PcapReader`. Credit:
  [@stryngs](https://github.com/stryngs) ([upstream #519](https://github.com/greatscottgadgets/ubertooth/pull/519)).

([#23](https://github.com/SqueakyFloors/ubertooth/issues/23), [#25](https://github.com/SqueakyFloors/ubertooth/issues/25), [#1229](https://github.com/SqueakyFloors/ubertooth/issues/1229), [#24](https://github.com/SqueakyFloors/ubertooth/issues/24), [#337](https://github.com/SqueakyFloors/ubertooth/issues/337), [#154](https://github.com/SqueakyFloors/ubertooth/issues/154), [#152](https://github.com/SqueakyFloors/ubertooth/issues/152), [#640](https://github.com/SqueakyFloors/ubertooth/issues/640), [#323](https://github.com/SqueakyFloors/ubertooth/issues/323), [#1205](https://github.com/SqueakyFloors/ubertooth/issues/1205), [#1242](https://github.com/SqueakyFloors/ubertooth/issues/1242), [#315](https://github.com/SqueakyFloors/ubertooth/issues/315), [#725](https://github.com/SqueakyFloors/ubertooth/issues/725), [#648](https://github.com/SqueakyFloors/ubertooth/issues/648), [#715](https://github.com/SqueakyFloors/ubertooth/issues/715), [#728](https://github.com/SqueakyFloors/ubertooth/issues/728), [#47](https://github.com/SqueakyFloors/ubertooth/issues/47), [#745](https://github.com/SqueakyFloors/ubertooth/issues/745), [#886](https://github.com/SqueakyFloors/ubertooth/issues/886), [#798](https://github.com/SqueakyFloors/ubertooth/issues/798), [#33](https://github.com/SqueakyFloors/ubertooth/issues/33), [libbtbb issue 88](https://github.com/SqueakyFloors/libbtbb/issues/88), [#1118](https://github.com/SqueakyFloors/ubertooth/issues/1118), [#585](https://github.com/SqueakyFloors/ubertooth/issues/585), [#913](https://github.com/SqueakyFloors/ubertooth/issues/913), [#905](https://github.com/SqueakyFloors/ubertooth/issues/905), [#91](https://github.com/SqueakyFloors/ubertooth/issues/91), [#486](https://github.com/SqueakyFloors/ubertooth/issues/486), [#544](https://github.com/SqueakyFloors/ubertooth/issues/544), [#30](https://github.com/SqueakyFloors/ubertooth/issues/30), [#235](https://github.com/SqueakyFloors/ubertooth/issues/235), [#313](https://github.com/SqueakyFloors/ubertooth/issues/313), [#327](https://github.com/SqueakyFloors/ubertooth/issues/327), [#254](https://github.com/SqueakyFloors/ubertooth/issues/254), [#311](https://github.com/SqueakyFloors/ubertooth/issues/311), [#324](https://github.com/SqueakyFloors/ubertooth/issues/324), [#349](https://github.com/SqueakyFloors/ubertooth/issues/349), [upstream #551](https://github.com/greatscottgadgets/ubertooth/issues/551))

### Documentation

- **A man page for every tool,** `btle-extcap(1)` and `ubertooth-btbr(1)`
  included. Every page and help text lists `-h`, writes option values as
  separate words (`-c 2440`; the attached form still works), and describes the
  board selector as `-U <index|serial>`; ubertooth(7) explains both once.
- **Guides.** "Using Ubertooth with Wireshark" covers `btle-extcap.py` end to
  end, the sniffing guide walks a Classic follow from an unknown target to a
  capture in Wireshark, and there is guidance for BLE connection capture, the
  spectrum analyzer UI, and the supported platforms (`COMPATIBILITY.md`). The
  README opens with a Hardware section, and the hardware page describes the
  Ubertooth One as it is today.
- **What the tools can and cannot do.** The man pages explain the
  `ubertooth-rx` clock search and why a run can end without a clock, the
  `ubertooth-afh -m` trade-off, which channels `ubertooth-btle -p` listens on,
  what an `ubertooth-dump` record holds, what a board's refusals print, and
  that a capture cannot hold EDR payloads or an encrypted link's contents.
- **Releases and contributing.** The README says what a release tag means
  (`yyyy-mm-DEVn` development, `yyyy-mm-RCn` candidate, `yyyy-mm-Rn` release)
  and that host tools and firmware from one tag belong together.
  `ubertooth-util(1)` says when the firmware API version moves, and
  `CONTRIBUTING.md` lists the tests and CI checks a change needs.
- **Inclusive terms.** Bluetooth connection roles are now called Central and
  Peripheral, the Bluetooth SIG's current names, replacing the older terms in
  every tool's output, help text, and man page; the SPI select line is now
  called chip select. For example, `ubertooth-btle -s` is now faux Peripheral
  mode, its error reads `error: faux peripheral address must be
  xx:xx:xx:xx:xx:xx`, and `ubertooth-util -w` reports the mode as `BLE
  peripheral`. Option letters, request numbers, and mode numbers are
  unchanged, so every host tool works with every board as before.

([#481](https://github.com/SqueakyFloors/ubertooth/issues/481), [#195](https://github.com/SqueakyFloors/ubertooth/issues/195), [#206](https://github.com/SqueakyFloors/ubertooth/issues/206), [#530](https://github.com/SqueakyFloors/ubertooth/issues/530), [#21](https://github.com/SqueakyFloors/ubertooth/issues/21), [#12](https://github.com/SqueakyFloors/ubertooth/issues/12), [#518](https://github.com/SqueakyFloors/ubertooth/issues/518), [#9](https://github.com/SqueakyFloors/ubertooth/issues/9), [#42](https://github.com/SqueakyFloors/ubertooth/issues/42), [#8](https://github.com/SqueakyFloors/ubertooth/issues/8), [#11](https://github.com/SqueakyFloors/ubertooth/issues/11), [#10](https://github.com/SqueakyFloors/ubertooth/issues/10), [#968](https://github.com/SqueakyFloors/ubertooth/issues/968), [#194](https://github.com/SqueakyFloors/ubertooth/issues/194), [#1025](https://github.com/SqueakyFloors/ubertooth/issues/1025), [#945](https://github.com/SqueakyFloors/ubertooth/issues/945), [#699](https://github.com/SqueakyFloors/ubertooth/issues/699), [#617](https://github.com/SqueakyFloors/ubertooth/issues/617), [#413](https://github.com/SqueakyFloors/ubertooth/issues/413), [#397](https://github.com/SqueakyFloors/ubertooth/issues/397), [#534](https://github.com/SqueakyFloors/ubertooth/issues/534), [#905](https://github.com/SqueakyFloors/ubertooth/issues/905), [#700](https://github.com/SqueakyFloors/ubertooth/issues/700))

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
  program is reported rather than answered as written. It takes effect once
  the board carries this release's bootloader.
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
