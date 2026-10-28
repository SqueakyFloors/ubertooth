---
inclusion: always
---
# Standard Output, Standard Error and Exit Status

Every host tool, library and script in this fork follows these rules. A
reader of the output, or a script piping it, must never have to guess which
stream a line is on.

## Standard output carries only what the user asked for

- The result of the command: requested reports (`ubertooth-util -v`,
  `-s`, `-b`), decoded packets and dumps, survey results, spectrum samples.
- Capture data when the file argument is `-` (`ubertooth-btle -q -`,
  `ubertooth-rx -q -` and `-d -`, `ubertooth-dump -d -`,
  `ubertooth-follow -q -` and `-d -`, `ubertooth-specan -d -`). In that case
  nothing else is written to standard output for the whole run, not even the
  result lines above: a line the user still needs goes to standard error
  instead (`ubertooth-rx -z` prints its survey results there). Only one
  capture can have that stream, so a tool whose two capture options are
  separate writers refuses the second (`ubertooth-rx -q - -d -`), and one
  that shares a single handle between them keeps ignoring the extra with a
  `warning: ` (`ubertooth-btle -q - -c -`). A destination whose writer seeks
  back over what it has written cannot be a pipe at all, so the PcapNG
  options and `ubertooth-dfu -u` refuse `-` and say what to use instead.
- The running commentary of an operation that *is* the command, rather than a
  note about the tool: `ubertooth-util -n` runs a range test and its
  `waiting for the repeater... (<n> s)` lines lead to the result they belong
  with, so both go to standard output. This is the one case; "Starting range
  test", which is the tool talking about itself, stays on standard error with
  the progress lines below. Do not read it as licence to move other progress.
- The usage text when the user asked for it with `-h`.

### `-h` beside another option that ends the command

`-h` and the options that answer from the host and then end the command
(`-V`, `-H`) all answer on standard output, so a command line with two of them
prints both, and the usage text comes last: it is the long one, and printing
it first would scroll the short answer off the screen. The order they were
written in makes no difference.

This is about the options that end the command. `ubertooth-util -N` answers
from the host too, but it prints its count and then runs the rest of the
command line, so `-h` beside it still ends the command and the count is not
printed.

    $ ubertooth-util -h -V      # the version, then the usage text, status 0
    $ ubertooth-util -V -h      # the same two, in the same order

## Standard error carries everything else

- Errors, prefixed `error: ` (lowercase), one line where possible, stating what
  was wrong and quoting the user's input as given.
- Warnings, prefixed `warning: `; notes, prefixed `note: `.
- Progress and status: "Resetting ...", "Stopping ...", DFU progress, device
  selection messages, "Ubertooth stopped", timing traces.
- The usage text, for a bare command line only: a tool that needs arguments
  and is run with none prints its one `error: ` line and then the usage text,
  both on standard error. A tool that has work to do with no options
  (`ubertooth-dump`, `ubertooth-rx`, `ubertooth-specan`) runs instead.
- No other refused command line carries the usage text. The `error: ` line
  describes the problem, so the usage text adds nothing; a reader who wants it
  runs `-h`. That covers an option the tool does not know, an option whose
  value is missing, a combination that cannot work, a value out of range, a
  stray word, and nothing to do on a command line that did say something. No
  tool is an exception.
- One wording for a command line the option parser will not take, whatever
  the language: `error: unrecognized option '-Z'`,
  `error: unrecognized option '--bogus'` and
  `error: option '-d' requires a value`. The C tools get it from
  `opt_report_bad_option()`; `ubertooth-btbr` and `btle-extcap.py` rewrite
  Python's `getopt` wording to the same lines. A tool that takes no long
  option adds `: the tools take single-letter options` to the second, which
  is why `btle-extcap.py`, whose long options are Wireshark's, does not.
- libubertooth and libbtbb never write to standard output. Their diagnostics
  go to standard error (or are returned to the caller).
- Firmware of another API version is an `error: ` in a release build, which
  refuses the command, and the same line as a `warning: ` in a development
  build, which runs it; a `note: ` then says what allowed that. One wording,
  one place: `ubertooth_report_api_mismatch()`.
- A request the board's firmware has not got is reported by
  `ubertooth_report_unsupported_request()` and nowhere else, so there is one
  wording for it: an `error: ` line naming the request, and, where a board's
  firmware API version has been read, a `note: ` putting it beside the one
  these tools need (ubertooth-dfu talks to the bootloader, which has no such
  version, so it gets the line alone). Do not write another wording for a
  stalled request, and do not hand one to `show_libusb_error()`.

The three cases, with `ubertooth-ego`:

    $ ubertooth-ego -h          # the usage text on standard output, status 0
    $ ubertooth-ego             # "error: nothing to do: choose a major mode
                                # (-f, -r or -i)" and the usage text, both on
                                # standard error, status 1
    $ ubertooth-ego -f -c 2408  # "error: -c cannot be combined with -f; only
                                # continuous receive (-r) uses a channel", that
                                # line alone on standard error, status 1

A lone `--` is not a bare command line. It ends the options and leaves
nothing behind it, so the user did write something: a tool that needs
arguments refuses it with that one `error: ` line and no usage text, and a
tool that has work to do with no options runs.

## A command line that says two things and means one

A command line that asks for two things where only one can happen is refused
with one `error: ` line, before any device is opened and before any file is
created. The tool never picks one of the two and drops the other without a
word. This covers:

- two options that cannot both run: two major modes, two modulations for one
  capture, two output formats for one sweep, the same setting written two
  ways (`ubertooth-util -c 2440 -C 24`);
- one option given a value twice (`-l 446688 -l aabbcc`): `error: option '-l'
  given more than once`. The C tools share one gate for it,
  `opt_check_once()`; `ubertooth-btbr` checks it in Python and says the same
  line. `btle-extcap.py` and `ubertooth-specan-ui` are the exceptions and keep
  the last value, because the program that calls them repeats an option as a
  matter of course.

Only a value counts as an answer. Repeating an option that takes no value
says the same thing twice and changes nothing, and an option whose value is
optional has two jobs: `ubertooth-util -c 2426 -c` sets the channel and then
reads it back, which is one value and one read.

A tool may still refuse a repeat of its own, where the second one is more
likely a mistyped command line than a request: `ubertooth-specan -v` takes no
value and is refused the second time, with a line of its own. A tool that does
that says so in its man page.

## A command that does two things and only one of them fails

Where one command line asks for two pieces of work and the first succeeds,
the work that was done stands and the exit status is `1`, with the
`error: ` line naming the half that failed. `ubertooth-dfu -s file.bin -d
image.dfu` checks both files first, so a bad one ends the command before
anything is written; then it signs, writing `file.dfu`; then it talks to the
board. A board that is missing, or that will not take the image, exits `1`
and leaves `file.dfu` in place, because the signed file is correct whether or
not the flash worked.

## Exit status

- `0`: the command did what was asked, including `-h`.
- `1`: a failure: a usage error, a command line with nothing to do, a device
  that is missing or failed, a partial failure.
- Other codes only for a distinct outcome a script needs to tell apart from
  failure, defined by that tool and listed in its man page's EXIT STATUS. Today
  there is one: `ubertooth-afh -t` exits `2` when no packet from the piconet
  arrived within the timeout. A usage error is never one of these; it is `1`.
- A command that did nothing exits `1`, never `0`, and opens no device.
- A call that answers a question did something, however short the answer:
  `btle-extcap.py` verifying a capture filter for Wireshark exits `0` whether
  the filter is good or not. What is wrong with the filter goes on standard
  output and nothing at all means it is good, because Wireshark reads that
  output as the answer and throws away the output of a call that exited
  non-zero.
- Check command-line problems before opening a device or creating an output
  file, so a refused command touches nothing.

## When changing code

- New output follows these rules. When touching a tool, fix any line in the
  code you change that breaks them; leave unrelated lines to a tracked issue.
- Tests pin the stream: assert which of standard output and standard error a
  message is on, and that standard output is empty on an error.
- Man pages describe the same split (an EXIT STATUS and, where a tool writes
  to standard error, a DIAGNOSTICS section or sentence).
