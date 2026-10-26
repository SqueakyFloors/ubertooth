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
- The usage text when the user asked for it with `-h`.

## Standard error carries everything else

- Errors, prefixed `error: ` (lowercase), one line where possible, stating what
  was wrong and quoting the user's input as given.
- Warnings, prefixed `warning: `; notes, prefixed `note: `.
- Progress and status: "Resetting ...", "Stopping ...", DFU progress, device
  selection messages, "Ubertooth stopped", timing traces.
- The usage text when it is shown because the tool could not take the command
  line at all, or it asked for nothing: an option the tool does not know, an
  option whose value is missing, no mode. Print the one-line `error: ` first,
  then the usage text, both on standard error.
- A command line the tool understood and will not run is the one `error: `
  line alone, with no usage text: a combination of options that cannot work
  (`ubertooth-util -z` with a mode option, `ubertooth-btle -f -n`,
  `ubertooth-ego -f -c`), an option value outside its range, a word that is
  not an option. The line already names what was wrong, so the usage text adds
  nothing.
- libubertooth and libbtbb never write to standard output. Their diagnostics
  go to standard error (or are returned to the caller).

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
