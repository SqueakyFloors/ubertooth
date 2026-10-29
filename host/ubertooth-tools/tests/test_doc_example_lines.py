#!/usr/bin/env python3
"""The example output lines in a man page are lines a host test pins.

Run as: test_doc_example_lines.py <host source dir>

The man pages show what a tool prints, and the host tests pin those same lines
against literals of their own. Nothing compared the two, so a change that
reworded a message and updated its test, and left the page alone, passed
everywhere: the page went on promising a line no tool prints. This reads the
pages and the tests and matches one against the other, so a page left behind
fails here.

Which lines: the ones in an indented example block of host/doc/*.md that a
tool writes rather than a user types -- a diagnostic (`error:`, `warning:` or
`note:`) or a report line of the form `Label: value`. Command lines, diagrams
and prose are left alone, and so are capture records, which the tests that
write them already pin.

How a line is matched. Neither side holds a whole line of real output, so both
are read as patterns:

  * what varies from run to run becomes a wildcard on both sides: a
    placeholder (`<serial>`, `<version>`), a board serial, a `git-` revision,
    an address, a number;
  * a test's literal also has a wildcard wherever it leaves a field open -- a
    macro parameter, a printf conversion, an adjacent literal it is joined to.

A documented line is covered when a pinned pattern can produce it: the fixed
text has to agree character for character, and the page may only have a
placeholder where the test leaves a field open. So `Serial No (0): 1cc0...` is
covered by `"Serial No (" i "): " FAKE_SERIAL "\\n"` whatever index and serial
each shows, and stops being covered as soon as the label or the punctuation
between them changes on one side only.

A diagnostic also counts as covered when a test pins it without its
`error: ` / `warning: ` / `note: ` prefix, because several of these messages
are built by a library that leaves the prefix to the tool that prints them.

What this does not catch. Where a test pins only part of a line, the rest of
that line is tied to nothing, so a reworded tail can still get through;
pinning the whole line in the test closes that. And every number is a
wildcard, so a figure a page shows is never checked, even one that does not
vary.

NOT_PINNED below holds the lines no test pins today. It is checked for being
exact -- an entry that no longer appears on its page, or that a test has since
pinned, fails here -- so it is a list that can only shrink, and editing a line
it names means saying so here.
"""

import glob
import os
import re
import sys
from functools import lru_cache

# Stands for "anything here" in both kinds of pattern.
WILD = "\x00"

# A C or Python token: a string literal, an identifier, or one character.
# Python's single-quoted literals count, and so does the `{...}` of an
# f-string, which is a field the format fills in.
TOKEN = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\''
                   r'|[A-Za-z_][A-Za-z_0-9]*|.', re.S)
FIELD = re.compile(r'\{[^{}]*\}')

# What this reads past rather than into: a string literal keeps whatever is
# inside it, a comment goes. Matched in one pass so that a `#` or a `//`
# inside a literal is not read as the start of a comment.
C_PROSE = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\''
                     r'|(/\*(?:.|\n)*?\*/)|(//[^\n]*)')
PY_PROSE = re.compile(r'"""(?:.|\n)*?"""|\'\'\'(?:.|\n)*?\'\'\''
                      r'|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\''
                      r'|(#[^\n]*)')

# A printf conversion, filled in when the line is printed. `r` is Python's,
# for a value written as its repr. The flag characters are spelled with the
# digits before the hash, because a hash followed by a digit reads as an
# issue number to tools/check-issue-references.sh.
CONVERSION = re.compile(r'%[-+ 0-9#.*lhzjt]*[diouxXeEfgGaAcrspn%]')

# What a line does not promise: a placeholder, a board serial, a firmware
# revision, an address, a number.
VOLATILE = re.compile(
    r'<[^<>]*>'
    r'|[0-9a-f]{32}'
    r'|git-[0-9A-Za-z_.*-]+'
    r'|\b[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){4,5}\b'
    r"|'[0-9a-f]{2,32}'"          # a serial, or the start of one, in quotes
    r'|0x[0-9A-Fa-f]+'
    r'|\b\d+(?:\.\d+)*\b')

# A line a tool prints: a diagnostic, or a report line with a label. The
# wildcard form is for a test that holds the level in a conversion.
DIAGNOSTIC = re.compile(r'^(?:error|warning|note|' + WILD + r'): ')
DOCUMENTED = re.compile(r"^(?:error|warning|note): |^[A-Z][A-Za-z0-9 '/()-]{2,40}: \S")

# An indented example block line, and the list item whose prose is indented
# the same way.
INDENTED = re.compile(r'^ {4,}\S')
LIST_ITEM = re.compile(r'^ *[-*] \S')

# A pinned pattern needs a run of fixed text this long to say anything: ": "
# with a wildcard each side would otherwise match any labelled line.
LEAST_FIXED_RUN = 6

# And it has to account for this much of the line it covers, so that a short
# head such as `error: -` does not stand in for the whole of a long line.
LEAST_SHARE = 0.6

# A floor on how many example lines this finds, so a change to the patterns
# above cannot leave the check passing having looked at nothing.
AT_LEAST = 60

# The documented lines no host test pins, with why. Each entry is the page and
# the line as it appears there.
NOT_PINNED = [
    # The note the AFH detection prints as it finds a channel. Nothing runs
    # that part of ubertooth_callback.c under a test yet.
    ("ubertooth-afh.md",
     "note: activating additional channel 37"),
    # ubtbr's own diagnostics quote the Python exception that ended a run, and
    # its tests pin the phrase rather than the whole line.
    ("ubertooth-btbr.md",
     "error: the receive thread stopped: USBErrorPipe: LIBUSB_ERROR_PIPE [-9]"),
    ("ubertooth-btbr.md",
     "warning: BTCtlInquiryCmd: could not ask the device to go idle: "
     "LIBUSB_ERROR_NO_DEVICE [-4]"),
    # ubertooth-dfu names the image file it was given, and its tests use a
    # temporary path, so the name on the page is not a name any test prints.
    ("ubertooth-dfu.md",
     "error: other-device.dfu: there is no firmware info block here at "
     "offset 0x100; firmware will not be updated"),
    ("ubertooth-dfu.md",
     "error: not-an-image.dfu: Unknown DFU suffix length: 115; firmware will "
     "not be updated"),
    ("ubertooth-dfu.md",
     "error: too-big.dfu needs 114944 bytes of flash; part 0x25011722 "
     "(LPC1754) has 114688 above the bootloader; firmware will not be "
     "updated"),
    # The API version mismatch report. The page writes the version as one
    # placeholder; the test builds it from its two halves with the dot
    # between them spelled out, so the two do not line up field for field.
    ("ubertooth.md",
     "error: Ubertooth firmware API version <version> found, libubertooth "
     "1.1 requires <version>."),
    ("ubertooth.md",
     "warning: Ubertooth firmware API version <version> found, libubertooth "
     "1.1 requires <version>."),
    ("ubertooth.md",
     "note: the board reports firmware API version <version>, libubertooth "
     "1.1 requires <version>"),
    ("ubertooth-dfu.md",
     "warning: older.dfu: this image reports API version 1.07 in its "
     "firmware info block, and these tools report <version>; the tools and "
     "the board have to report the same one"),
    # The signature report's own lines. The tests read the values rather than
    # the lines the tool prints around them.
    ("ubertooth-dfu.md",
     "Image: bluetooth_rxtx git-ed30131d* (API:<version>)"),
    ("ubertooth-dfu.md",
     "Image: bluetooth_rxtx git-ed30131d (API:<version>)"),
    ("ubertooth-dfu.md",
     "DFU expected revision: git-ed30131d"),
    # The refusal of two options that cannot be combined. The tool builds it
    # from the options it was given and the list it allows, and the test
    # leaves both to the run, so the page's own spelling of them is not
    # checked either way.
    ("ubertooth-util.md",
     "error: options -q -n cannot be combined; give only one of -q -n -e -t "
     "-x -I -i"),
    # The reason libusb gave for a board that could not be opened. The only
    # test that pins this text pins it as an error line of another tool, and
    # which of the three levels a line is said with is part of what a page
    # promises, so that does not cover a note.
    ("ubertooth-util.md",
     "note: Access denied (insufficient permissions) (libusb -3)"),
    # ubertooth-specan's write failures: the tests pin the line up to the
    # reason the C library gave, which the page shows as a placeholder.
    ("ubertooth-specan.md",
     "error: writing samples failed: wrote 0 of 3 bytes (<reason>)"),
    ("ubertooth-specan.md",
     "error: writing samples failed: could not write the sample (<reason>)"),
    ("ubertooth-specan.md",
     "error: writing samples failed: could not flush the samples (<reason>)"),
    # The version report, whose libubertooth and libbtbb halves the tests
    # replace with a fake of their own, so no test prints this shape.
    ("ubertooth-util.md",
     "Host tools version: libubertooth <version> (git-ed30131d*), libbtbb "
     "<version> (git-2fd0d39)"),
    ("ubertooth.md",
     "Host tools version: libubertooth <version> (<revision>), libbtbb "
     "<version> (<revision>)"),
    # The build string is whatever the board reports; the faked board reports
    # a short one of its own.
    ("ubertooth-util.md",
     "Firmware build: ubertooth git-ed30131d* (builder@buildhost) Mon Oct 27 "
     "11:22:33 2025"),
    ("ubertooth.md",
     "Firmware build: <build string the board reports>"),
]


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def unescape(text):
    return (text.replace('\\n', '\n').replace('\\t', '\t')
                .replace('\\"', '"').replace("\\'", "'")
                .replace('\\\\', '\\'))


def usable(pattern):
    """Whether a pinned pattern says enough to tie a line to.

    Open at both ends, a pattern only asks for its fixed text to appear
    somewhere in the line, which a phrase common to many lines would satisfy;
    `"%s requires %s"` would cover any line with " requires " in it. So one
    end has to be fixed text, which is where a line's label or its full stop
    is, and some run of that text has to be long enough to mean something.
    """
    if pattern[:1] == WILD and pattern[-1:] == WILD:
        return False
    return any(len(run) >= LEAST_FIXED_RUN for run in pattern.split(WILD))


def fixed_length(pattern):
    return len(pattern.replace(WILD, ""))


def without_prose(text, python):
    """The source with its comments gone, so prose is not read as a literal.

    A Python docstring matters most: its own quotes tokenize as an empty
    literal, and the sentences inside it would then be read as lines a test
    pins. A sentence about output is not output.

    Literals are matched in the same pass and kept, so a `#` or a `//` inside
    one does not cut the literal short.
    """
    def replace(match):
        # A group matched only for a comment; a literal matches group 0 alone.
        return ' ' if match.lastindex else match.group(0)

    text = C_PROSE.sub(replace, text)
    if python:
        text = PY_PROSE.sub(replace, text)
    return text


def pinned_patterns(text, python=False):
    """What a test source pins, one pattern per run of adjacent literals.

    The compiler joins literals that sit side by side, so `"a: " NAME "\\n"`
    is one line with the macro's value in the middle. That value is a wildcard
    here, since this reads the source and not the macro. A token that is
    neither a literal nor an identifier ends the run.
    """
    text = without_prose(text, python)
    text = text.replace('\\\n', ' ')                     # continued lines
    patterns = []
    run = []
    gap = False
    at = 0
    while at < len(text):
        token = TOKEN.match(text, at)
        value = token.group(0)
        at = token.end()
        if value.startswith('"') or value.startswith("'"):
            if run and gap:
                run.append(WILD)
            run.append(FIELD.sub(WILD, unescape(value[1:-1])))
            gap = False
        elif value.isspace():
            continue
        elif value[0].isalpha() or value[0] == '_':
            gap = True                      # a macro or a variable in between
        else:
            if run:
                patterns.append("".join(run))
            run = []
            gap = False
    if run:
        patterns.append("".join(run))
    return [CONVERSION.sub(WILD, p) for p in patterns]


def pattern_of(line):
    """A line as a pattern: what it does not promise becomes a wildcard."""
    pattern = VOLATILE.sub(WILD, line)
    return re.sub(WILD + '+', WILD, pattern).strip()


def documented_forms(line):
    """A documented line as a pattern, and without its diagnostic prefix.

    The second form is for the messages a library builds and leaves the
    prefix to the tool that prints them, so a test pins the message alone.
    Only the documented side is stripped: a pinned `error: X` must not come
    to cover a page that shows `note: X`, because which of the three a line
    is said with is part of what the page promises.
    """
    pattern = pattern_of(line)
    prefix = DIAGNOSTIC.match(pattern)
    if prefix:
        return [pattern, pattern[prefix.end():]]
    return [pattern]


def pinned_lines(host_src):
    """Every line the host tests pin a tool to print, as a pattern."""
    sources = []
    for where, suffix in ((("ubertooth-tools", "tests"), "*.c"),
                          (("ubertooth-tools", "tests"), "*.py"),
                          (("ubertooth-tools", "tests"), "*.json"),
                          (("python", "*", "tests"), "*.py")):
        sources += sorted(glob.glob(os.path.join(host_src, *where, suffix)))
    # Not this file: the lines quoted below are not a tool's output, and a
    # list of lines nothing pins must not end up pinning them.
    here = os.path.basename(__file__)
    sources = [path for path in sources
               if os.path.basename(path) != here]
    if not sources:
        fail("no host test sources found under %s" % host_src)
    lines = []
    for path in sources:
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        for pattern in pinned_patterns(text, path.endswith(".py")):
            for line in pattern.split("\n"):
                form = pattern_of(line)
                if usable(form):
                    lines.append(form)
    return list(dict.fromkeys(lines))


def documented_lines(doc):
    """The example output lines the man pages show, with their place.

    An indented run of lines is an example block unless the line above it
    starts a list item, in which case the indented lines are that item's
    prose -- the description of an option, which no tool prints. A couple of
    those descriptions are indented four spaces where their neighbours use
    three, so indentation alone does not tell them apart.
    """
    found = []
    for path in sorted(glob.glob(os.path.join(doc, "*.md"))):
        with open(path, encoding="utf-8", errors="replace") as handle:
            in_list_item = False
            for number, raw in enumerate(handle, 1):
                if not INDENTED.match(raw):
                    in_list_item = LIST_ITEM.match(raw) is not None
                    continue
                if in_list_item:
                    continue
                line = raw.strip()
                if DOCUMENTED.match(line):
                    found.append((os.path.basename(path), number, line))
    return found


def can_produce(pinned, documented):
    """Can the pinned pattern produce the documented line?

    A wildcard in the pinned pattern stands for any run of characters, which
    is where the page spells out what the test left open. Everywhere else the
    two have to agree character for character -- including the page's own
    wildcards, which have to sit where the test leaves a field open.
    """
    @lru_cache(maxsize=None)
    def match(i, j):
        if i == len(documented) and j == len(pinned):
            return True
        if j < len(pinned) and pinned[j] == WILD:
            return (match(i, j + 1)
                    or (i < len(documented) and match(i + 1, j)))
        if (i < len(documented) and j < len(pinned)
                and documented[i] == pinned[j] and documented[i] != WILD):
            return match(i + 1, j + 1)
        return False

    return match(0, 0)


def covered_by(pinned, line):
    """The pinned pattern that ties this documented line down, if any.

    As well as being able to produce the line, the pattern has to account for
    most of it. Without that, a pinned `error: -` -- the head of a line a
    test asserted the start of -- would stand in for every long `error: -U
    ...` line a page shows, tail and all.
    """
    for form in documented_forms(line):
        needed = LEAST_SHARE * fixed_length(form)
        for candidate in pinned:
            if (fixed_length(candidate) >= needed
                    and can_produce(candidate, form)):
                return candidate
    return None


def variants(line):
    """The line with two letters pushed into it, in a few places.

    The places are inside the runs the page does promise, the three longest
    of them, a quarter and three quarters of the way along each, so a change
    lands on text rather than inside a placeholder. A pinned pattern that
    covers one of these as happily as the line itself is matching something
    the line has in common with other lines rather than the line. One of them
    breaking is enough to tell, because another may land in a field the test
    leaves open: there the page spells out what the test does not promise,
    and the page's words are nobody's to check.
    """
    runs = [run for run in pattern_of(line).split(WILD) if len(run) >= 4]
    made = []
    for run in sorted(runs, key=len, reverse=True)[:3]:
        for share in (0.25, 0.75):
            at = line.find(run) + int(len(run) * share)
            made.append(line[:at] + "zq" + line[at:])
    return made


def main():
    if len(sys.argv) != 2:
        fail("usage: test_doc_example_lines.py <host source dir>")
    host_src = sys.argv[1]
    # The match below recurses once per character it gets through, over the
    # two patterns together, and the longest line on a page is about 250
    # characters. The default limit of 1000 is close enough to that to be
    # worth raising, and this is still far below where the C stack runs out.
    sys.setrecursionlimit(4000)

    pinned = pinned_lines(host_src)
    documented = documented_lines(os.path.join(host_src, "doc"))
    if len(documented) < AT_LEAST:
        fail("found only %d example output lines in the man pages, expected "
             "at least %d: the patterns in this test no longer recognize them"
             % (len(documented), AT_LEAST))

    known = set(NOT_PINNED)
    if len(known) != len(NOT_PINNED):
        fail("NOT_PINNED names the same line of the same page twice")
    stale = []
    pinned_after_all = []
    held = 0
    for page, number, line in documented:
        # Tied down only if a pattern covers the line and stops covering it
        # once a word of the line changes. A pattern that covers both is
        # matching something the line has in common with other lines.
        tied = covered_by(pinned, line) is not None
        if tied:
            changed = variants(line)
            tied = any(covered_by(pinned, one) is None for one in changed)
        if tied and (page, line) in known:
            pinned_after_all.append((page, number, line))
        elif not tied and (page, line) not in known:
            stale.append((page, number, line))
        elif tied:
            held += 1

    # Reported first: an entry for a line that has been edited, or for a page
    # that no longer shows it, would otherwise be hidden by the failures
    # below, and it is usually the cause of them.
    gone = known - set((page, line) for page, _, line in documented)
    if gone:
        print("NOT_PINNED names lines that no page shows any more. A line it "
              "names that has been edited counts: say in the entry what the "
              "page says now, or take the entry out if a test pins it.")
        for page, line in sorted(gone):
            print("  %s: %s" % (page, line))
        fail("%d NOT_PINNED entry (entries) are out of date" % len(gone))

    if pinned_after_all:
        print("A test now pins these lines, so NOT_PINNED should lose them:")
        for page, number, line in pinned_after_all:
            print("  %s:%d: %s" % (page, number, line))
        fail("NOT_PINNED has %d entry (entries) a test has caught up with"
             % len(pinned_after_all))

    if stale:
        print("No host test pins these lines, so the pages promise output "
              "nothing is known to print. Either the line is out of date, or "
              "the test that covers it pins too little of it to tell.")
        for page, number, line in stale:
            print("  %s:%d: %s" % (page, number, line))
        fail("%d of %d documented output lines are pinned by no test"
             % (len(stale), len(documented)))

    print("PASS: %d of %d documented output lines are pinned by a host test, "
          "%d recorded as pinned by none (%d pinned lines read)"
          % (held, len(documented), len(documented) - held, len(pinned)))


if __name__ == "__main__":
    main()
