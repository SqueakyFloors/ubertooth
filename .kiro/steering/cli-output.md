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
  `ubertooth-specan -d -`). In that case nothing else is written to standard
  output for the whole run, not even the result lines above.
- The running commentary of an operation that *is* the command, rather than a
  note about the tool: `ubertooth-util -n` runs a range test and its
  `waiting for the repeater... (<n> s)` lines lead to the result they belong
  with, so both go to standard output. This is the one case; "Starting range
  test", which is the tool talking about itself, stays on standard error with
  the progress lines below. Do not read it as licence to move other progress.
- The usage text when the user asked for it with `-h`.

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
- libubertooth and libbtbb never write to standard output. Their diagnostics
  go to standard error (or are returned to the caller).
- Firmware of another API version is an `error: ` in a release build, which
  refuses the command, and the same line as a `warning: ` in a development
  build, which runs it; a `note: ` then says what allowed that. One wording,
  one place: `ubertooth_report_api_mismatch()`.
- A request the board's firmware has not got is reported by
  `ubertooth_report_unsupported_request()` and nowhere else, so there is one
  wording for it: an `error: ` line naming the request, and a `note: ` with
  the board's firmware API version beside the one these tools need. Do not
  write a new line for a stalled request.

The three cases, with `ubertooth-ego`:

    $ ubertooth-ego -h          # the usage text on standard output, status 0
    $ ubertooth-ego             # "error: nothing to do: choose a major mode
                                # (-f, -r or -i)" and the usage text, both on
                                # standard error, status 1
    $ ubertooth-ego -f -c 2408  # "error: -c cannot be combined with -f; only
                                # continuous receive (-r) uses a channel", that
                                # line alone on standard error, status 1

## Exit status

- `0`: the command did what was asked, including `-h`.
- `1`: a failure: a usage error, a command line with nothing to do, a device
  that is missing or failed, a partial failure.
- Other codes only for a distinct outcome a script needs to tell apart from
  failure, defined by that tool and listed in its man page's EXIT STATUS. Today
  there is one: `ubertooth-afh -t` exits `2` when no packet from the piconet
  arrived within the timeout. A usage error is never one of these; it is `1`.
- A command that did nothing exits `1`, never `0`, and opens no device.
- Check command-line problems before opening a device or creating an output
  file, so a refused command touches nothing.

## When changing code

- New output follows these rules. When touching a tool, fix any line in the
  code you change that breaks them; leave unrelated lines to a tracked issue.
- Tests pin the stream: assert which of standard output and standard error a
  message is on, and that standard output is empty on an error.
- Man pages describe the same split (an EXIT STATUS and, where a tool writes
  to standard error, a DIAGNOSTICS section or sentence).
