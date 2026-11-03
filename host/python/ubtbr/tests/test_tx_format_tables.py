#!/usr/bin/env python3
"""Each basic-rate transmit format is described three times, and all must agree.

Run as: test_tx_format_tables.py <repository root>

A `tx` command names a format, a data length and a repeat count, and each of
these says what the format allows:

  * the host, from TX_FORMATS in host/python/ubtbr/ubtbr/btctl.py, which is
    what ubertooth-btbr's `tx` checks the length and the count against before
    anything leaves the machine;
  * the device, from bbcodec_tx_formats and the layouts it points at in
    firmware/btbr/src/codec/codec.c, which is what btctl_handle_tx_pkt_req()
    checks the same two against before it puts a packet on the air;
  * the man page, host/doc/ubertooth-btbr.md, which is the copy the user
    reads and acts on.

Nothing compared them, so a format whose slot count or data length changed in
one place would leave the host accepting a `tx` the device refuses on its
console, where the user cannot see it, or the page promising a length both
ends refuse. This reads all three out of the source and checks they say the
same thing about every format: its number, how many air slots one packet
fills, how many data bytes it carries, and the payload header and voice field
the host fills in. Every field of TX_FORMATS is compared, so a field added to
it has to be given a meaning here rather than going unchecked.

The count a `tx` may ask for is BTCTL_TX_PKT_MAX_SLOTS divided by the format's
slots, so that figure is compared as well, and the one line each side divides
on is pinned: the two tables agreeing is no use if one side stops dividing the
same way.

What this does not check. It compares the descriptions, not the code that
reads them: a side that stopped checking a length or a count against its own
table, or that compared it the wrong way round, would pass here. Those checks
are each other's tests, in test_ubtbr.py and in firmware/tests.

What the man page is read for: the list of formats it introduces, each item
and its own prose, the sentence that gives the counts, and the worked `tx`
commands. A figure it states anywhere else is on whoever writes it.

This is a host test that reads firmware source and one man page, so the
firmware directories it reads and that page are both named in the `changes`
job of .github/workflows/build.yml, which is what runs the host jobs for a
change to them. Reading another file outside what is named there means adding
it in the same change, or a branch that touched only that file would skip
this test.

The device's table holds two things this does not compare, because the host
never sends either and has no copy to drift from: the air TYPE code a format
goes out with, which EV3 and HV3 deliberately share, and the error correction
over the data field.

The two sides state the data length differently, which is why this is not a
field-for-field comparison. The device's table holds the longest payload the
format carries, and bbcodec_tx_format_limits() turns that into the range the
caller may ask for: a format with a payload header takes anything down to
nothing, an enhanced synchronous one needs at least the byte its CRC covers,
and a voice one has the single length its type always has. TX_FORMATS states
the resulting range outright. So the rule is applied here, and the function it
comes from is pinned below: a change to it fails here and names what to bring
across, rather than leaving this mirror quietly out of date.

The format sets have to match both ways. A format one side knows and the
other does not is a failure, so adding one means adding it to both tables.

How the sources are read. btctl.py is parsed as Python, so its table is read
exactly. The C is read the way a careful `grep` would: comments are stripped,
then the initializer lists are split on commas outside brackets. String
literals are not understood, so a comment opener inside one would be taken for
the start of a comment; no file read here has one. A reformat of one of these
tables fails this test rather than passing it on nothing, which is the right
direction: it says what it could not find.
"""

import ast
import os
import re
import sys


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        fail("cannot read %s, so the table in it is not checked: %s"
             % (path, e.strerror))


# ---------------------------------------------------------------- reading C

def strip_comments(text, where="a scanned source file"):
    """The text with /* ... */ and // ... comments replaced by spaces.

    Line structure is kept, so a pinned function body still reads as one.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                fail("unterminated block comment in %s" % where)
            out.append(" " if "\n" not in text[i:end] else
                       text[i:end].count("\n") * "\n")
            i = end + 2
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def braced(text, start, what):
    """The text inside the first { ... } at or after start."""
    try:
        open_at = text.index("{", start)
    except ValueError:
        fail("no initializer or body found for %s" % what)
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                region = text[open_at + 1:i]
                if "#" in region:
                    # A preprocessor directive inside a table or a function
                    # body means the source says two different things and the
                    # build picks one. Reading C as text cannot tell which, so
                    # a scan that went on would be checking a branch that may
                    # not be the one compiled.
                    fail("%s carries a preprocessor directive, so what the "
                         "build uses is not what this test would read: %s"
                         % (what, [l.strip() for l in region.split("\n")
                                   if "#" in l][0]))
                return region
    fail("unbalanced braces in %s" % what)


def split_top(text):
    """text split on the commas outside any brace, bracket or parenthesis."""
    items, depth, item = [], 0, []
    for ch in text:
        if ch in "{[(":
            depth += 1
        elif ch in "}])":
            depth -= 1
        if ch == "," and depth == 0:
            items.append("".join(item))
            item = []
        else:
            item.append(ch)
    items.append("".join(item))
    return [i.strip() for i in items if i.strip()]


def definition(text, pattern, what):
    """The initializer or body of the one definition pattern matches."""
    found = list(re.finditer(pattern, text))
    if len(found) != 1:
        fail("found %d definitions of %s, expected one" % (len(found), what))
    return braced(text, found[0].end(), what)


def designated(body, what):
    """{designator: initializer} for a [NAME] = ... initializer list."""
    entries = {}
    for item in split_top(body):
        m = re.match(r"\[\s*([A-Za-z_][A-Za-z0-9_]*)\s*\]\s*=\s*(.*)$",
                     item, re.S)
        if not m:
            fail("%s has an entry this test cannot read, so the table it "
                 "describes is not checked: %s" % (what, item.split("\n")[0]))
        entries[m.group(1)] = m.group(2).strip()
    return entries


def struct_fields(text, tag):
    """The fields of a struct, in the order a positional initializer fills."""
    body = definition(text, r"typedef\s+struct\s+%s\s*(?=\{)" % tag, tag)
    fields = []
    for decl in body.split(";"):
        if decl.strip():
            fields.append(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", decl)[-1])
    return fields


def enum_members(text, tag):
    """The members of an enum, as {name: value}, in order."""
    body = definition(text, r"typedef\s+enum\s+%s\s*(?=\{)" % tag, tag)
    members, nxt = {}, 0
    for item in split_top(body):
        m = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:=\s*(-?\d+))?$", item)
        if not m:
            fail("%s has a member this test cannot read: %s" % (tag, item))
        nxt = int(m.group(2)) if m.group(2) is not None else nxt
        members[m.group(1)] = nxt
        nxt += 1
    return members


def define_number(text, name, where):
    """The value of a #define of a plain decimal number."""
    m = re.search(r"^#define\s+%s\s+(\d+)\s*$" % name, text, re.M)
    if not m:
        fail("no #define %s with a plain number in %s" % (name, where))
    return int(m.group(1))


# ------------------------------------------------- the device's description

# What bbcodec_tx_format_limits() does, which is mirrored by device_limits()
# below. Pinned as the text of its body with comments and run of whitespace
# taken out: a change to the rule fails here and says to bring it across.
LIMITS_RULE = (
    "*max_data = t->payload_bytes; "
    "switch (t->length_kind) { "
    "case BB_LEN_HEADER: *min_data = 0; break; "
    "case BB_LEN_NEGOTIATED: *min_data = 1; break; "
    "case BB_LEN_FIXED: default: *min_data = *max_data; break; }"
)


def check_limits_rule(codec_c):
    # The return type is part of the pattern so that a call to the function
    # is not taken for a second definition of it.
    body = definition(codec_c,
                      r"\bvoid\s+bbcodec_tx_format_limits\s*\([^)]*\)\s*",
                      "bbcodec_tx_format_limits()")
    found = " ".join(body.split())
    if found != LIMITS_RULE:
        fail("bbcodec_tx_format_limits() no longer does what this test "
             "mirrors in device_limits(). Bring the change across, then put "
             "the new body in LIMITS_RULE.\n  found:    %s\n  expected: %s"
             % (found, LIMITS_RULE))


def tx_layout_reference(name, entry):
    """What a bbcodec_tx_formats entry gives as its air layout.

    An entry is an air TYPE code and a layout, written either in order or with
    the field names. None means the entry gives no layout at all, which is a
    format number left empty.
    """
    parts, named = [], {}
    for part in split_top(braced(entry, 0, "bbcodec_tx_formats[%s]" % name)):
        m = re.match(r"\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", part, re.S)
        if m:
            named[m.group(1)] = m.group(2).strip()
        else:
            parts.append(part)
    if named and parts:
        fail("bbcodec_tx_formats[%s] names some fields and not others, so "
             "which is the layout is not clear: %s" % (name, entry))
    if named:
        return named.get("t")
    if len(parts) == 1:
        return None
    if len(parts) != 2:
        fail("bbcodec_tx_formats[%s] is not an air TYPE and a layout: %s"
             % (name, entry))
    return parts[1]


def device_limits(layout, name):
    """The data bytes a format accepts, as the device decides it.

    The format states where its data length comes from, and that is what
    decides the range: the length is in the payload header, or agreed when the
    link was set up, or the one the type always has.
    """
    biggest = layout["payload_bytes"]
    kind = layout["length_kind"]
    if kind == "BB_LEN_HEADER":
        return 0, biggest
    if kind == "BB_LEN_NEGOTIATED":
        return 1, biggest
    if kind == "BB_LEN_FIXED":
        return biggest, biggest
    fail("bbcodec_tx_formats[%s] gives its length_kind as %r, which is not "
         "one of bbcodec_length_t's members, so what the device allows "
         "cannot be worked out" % (name, kind))


def device_formats(root):
    """Each format the device can be asked for, by name, from the firmware."""
    codec_c = strip_comments(
        read(os.path.join(root, "firmware/btbr/src/codec/codec.c")),
        "codec.c")
    codec_h = strip_comments(
        read(os.path.join(root, "firmware/btbr/include/ubtbr/codec.h")),
        "codec.h")
    check_limits_rule(codec_c)

    fields = struct_fields(codec_h, "bbcodec_types_s")
    acl = designated(
        definition(codec_c, r"\bbbcodec_acl_types\s*\[[^\]]*\]\s*=\s*",
                   "bbcodec_acl_types"),
        "bbcodec_acl_types")

    def layout(reference):
        """The air layout a bbcodec_tx_formats entry points at."""
        m = re.match(r"&\s*bbcodec_acl_types\s*\[\s*([A-Za-z_][A-Za-z0-9_]*)"
                     r"\s*\]$", reference)
        if m:
            if m.group(1) not in acl:
                fail("bbcodec_tx_formats points at bbcodec_acl_types[%s], "
                     "which that table has no entry for" % m.group(1))
            body = acl[m.group(1)]
        else:
            m = re.match(r"&\s*([A-Za-z_][A-Za-z0-9_]*)$", reference)
            if not m:
                fail("bbcodec_tx_formats has an air layout this test cannot "
                     "follow: %s" % reference)
            body = definition(
                codec_c,
                r"\bbbcodec_types_t\s+%s\s*=\s*" % m.group(1), m.group(1))
        values = split_top(
            braced(body, 0, "the air layout %s" % reference)
            if body.lstrip().startswith("{") else body)
        if len(values) != len(fields):
            fail("an air layout gives %d of bbcodec_types_t's %d fields, so "
                 "this test cannot tell which is which: %s"
                 % (len(values), len(fields), " ".join(values)))
        return dict(zip(fields, (int(v) if v.isdigit() else v
                                 for v in values)))

    formats, holes = {}, set()
    tx = designated(
        definition(codec_c, r"\bbbcodec_tx_formats\s*\[[^\]]*\]\s*=\s*",
                   "bbcodec_tx_formats"),
        "bbcodec_tx_formats")
    for name, entry in tx.items():
        reference = tx_layout_reference(name, entry)
        if reference in ("NULL", "0", None):
            # bbcodec_tx_format() refuses a format number this table leaves
            # empty, which is a format the device cannot be asked for at all.
            holes.add(name)
            continue
        air = layout(reference)
        for field in ("nslots", "payload_bytes", "payload_header_bytes",
                      "has_crc", "voice_bytes"):
            if not isinstance(air.get(field), int):
                fail("bbcodec_tx_formats[%s] gives %s as %r, not a number"
                     % (name, field, air.get(field)))
        least, most = device_limits(air, name)
        formats[name] = {
            "slots": air["nslots"],
            "min": least,
            "max": most,
            "hlen": air["payload_header_bytes"],
            "voice": air["voice_bytes"],
        }

    # The buffer the transmit side whitens a voice field in is sized from
    # this, so it is the longest voice field any format has and not merely
    # room enough for it.
    longest = max([f["voice"] for f in formats.values()] or [0])
    ceiling = define_number(codec_h, "BB_VOICE_BYTES_MAX", "codec.h")
    if ceiling != longest:
        fail("BB_VOICE_BYTES_MAX is %d and the longest voice field any "
             "format carries is %d. Its comment in codec.h calls it the "
             "longest field rather than room for it, so the fix is whichever "
             "of the two is now wrong." % (ceiling, longest))
    return formats, holes


# --------------------------------------------------- the host's description

def host_formats(path):
    """TX_FORMATS, by format name, read out of btctl.py."""
    assignments = {}
    for node in ast.parse(read(path)).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                and isinstance(node.targets[0], ast.Name):
            assignments[node.targets[0].id] = node.value

    def named(name):
        if name not in assignments:
            fail("btctl.py has no %s" % name)
        return assignments[name]

    call = named("TxFormat")
    if not isinstance(call, ast.Call) or len(call.args) != 2 \
            or not isinstance(call.args[1], ast.Constant):
        fail("TxFormat in btctl.py is not a namedtuple of named fields")
    fields = call.args[1].value.replace(",", " ").split()

    table = named("TX_FORMATS")
    if not isinstance(table, ast.Dict):
        fail("TX_FORMATS in btctl.py is not a dict of formats")
    formats = {}
    for key, value in zip(table.keys, table.values):
        if not isinstance(key, ast.Constant) or not isinstance(key.value, str):
            fail("TX_FORMATS has an entry whose name is not a plain string")
        if not isinstance(value, ast.Call) \
                or getattr(value.func, "id", None) != "TxFormat" \
                or len(value.args) != len(fields) \
                or not all(isinstance(a, ast.Constant) and
                           isinstance(a.value, int) and
                           not isinstance(a.value, bool)
                           for a in value.args):
            fail("TX_FORMATS[%r] is not a TxFormat of %d numbers"
                 % (key.value, len(fields)))
        formats[key.value] = dict(zip(fields,
                                      (a.value for a in value.args)))
    return fields, formats


# ------------------------------------------------------------- the checking

# What each field of TX_FORMATS means on the device's side, and how to say so
# when the two disagree. Every field of TxFormat is named here, which is
# checked below: a field added to it has to be given its device-side meaning
# rather than joining the table uncompared.
COMPARED = (
    ("code", "code", "the format number the host sends"),
    ("slots", "slots", "the air slots one packet fills, which bounds the "
        "repeat count"),
    ("min", "min", "the fewest data bytes the format carries"),
    ("max", "max", "the most data bytes the format carries"),
    ("hlen", "hlen", "the payload header the host writes"),
    ("voice", "voice", "the voice bytes the host fills"),
)

# The one line each side divides BTCTL_TX_PKT_MAX_SLOTS by the format's slots
# on, which is what turns the two tables into the count a `tx` may ask for.
# Pinned as text: comparing the tables is no use if one side stops dividing
# the way the other does. Matched as a whole line, so neither a longer
# expression that starts the same way nor a copy left in a comment stands in
# for the line the code runs.
DIVISIONS = (
    ("firmware/btbr/src/btctl/btctl.c",
     "max_slots = BTCTL_TX_PKT_MAX_SLOTS / t->nslots;"),
    ("host/python/ubtbr/ubertooth-btbr.in",
     "most = BTCTL_TX_PKT_MAX_SLOTS // fmt.slots"),
)


# How the man page spells the slot counts it works the allowed counts out
# from.
SLOT_WORDS = {"one": 1, "two": 2, "three": 3, "four": 4, "five": 5}


MAN_PAGE = "host/doc/ubertooth-btbr.md"

# The sentence the list of formats and their lengths follows. The list is read
# from here rather than from the whole page, so an unrelated list elsewhere on
# it is not taken for a format.
MAN_PAGE_LIST_OPENS = "The formats, with the data bytes each carries"


def man_page_list(text):
    """The lines of the page's list of formats: its items and their prose.

    The list starts at the sentence that introduces it and ends at the first
    line that is not part of an item, so a later list on the page cannot add
    a format to it.
    """
    lines, reached, started = [], False, False
    for line in text.split("\n"):
        if not reached:
            reached = MAN_PAGE_LIST_OPENS in line
            continue
        item = re.match(r" - (.*) :$", line)
        if item:
            started = True
            lines.append(item.group(1))
            continue
        if started:
            if not line.startswith("   "):
                break
            lines.append(line.strip())
    if not reached:
        fail("%s no longer opens its list of formats with %r, so the lengths "
             "it states are not checked" % (MAN_PAGE, MAN_PAGE_LIST_OPENS))
    if not started:
        fail("%s opens a list of formats and states no lengths in it, so "
             "what it promises the user is not checked" % MAN_PAGE)
    return lines


def man_page_lengths(lines):
    """The data bytes the page's list states, by format name.

    Read from the whole of each item, its own prose included, so a figure put
    in the explanation is compared rather than passed over.
    """
    lengths = {}
    for line in lines:
        for name, least, most in re.findall(
                r"`([a-z0-9]+)`\s+(\d+)(?:-(\d+))?\b", line):
            if name in lengths:
                fail("%s states what %s carries twice, so one of the two is "
                     "stale and whichever comes last is what a reader of "
                     "this test would compare" % (MAN_PAGE, name))
            lengths[name] = (int(least), int(most) if most else int(least))
    return lengths


def man_page_voices(lines):
    """The voice bytes the page's list states, by format name.

    The figure is in an item's own prose, so it belongs to the formats that
    item names: "`dv` 0-9" and then "10 bytes of voice".
    """
    voices, named = {}, []
    for line in lines:
        item_names = re.findall(r"`([a-z0-9]+)`\s+\d+", line)
        if item_names:
            named = item_names
        for stated in re.findall(r"(\d+) bytes of voice", line):
            for name in named:
                if name in voices:
                    fail("%s states %s's voice field twice, so one of the "
                         "two is stale" % (MAN_PAGE, name))
                voices[name] = int(stated)
    return voices


def man_page_counts(text):
    """The counts the page allows, as {slots: (count, the formats named)}.

    From the "400 of a one-slot format such as `dm1` or `ev3`" phrases, each
    reaching to the next of them or to the end of its sentence.
    """
    # Prose, so a line break can fall anywhere in a phrase.
    text = " ".join(text.split())
    counts = {}
    phrases = list(re.finditer(r"(\d+) of a ([a-z]+)-slot", text))
    for i, phrase in enumerate(phrases):
        stop = phrases[i + 1].start() if i + 1 < len(phrases) else len(text)
        sentence_ends = text.find(".", phrase.end())
        if 0 <= sentence_ends < stop:
            stop = sentence_ends
        word = phrase.group(2)
        if word not in SLOT_WORDS:
            fail("%s says %s of a %s-slot format, and this test does not "
                 "know how many slots that is"
                 % (MAN_PAGE, phrase.group(1), word))
        if SLOT_WORDS[word] in counts:
            fail("%s states the count for a %d-slot format twice, so one of "
                 "the two is stale" % (MAN_PAGE, SLOT_WORDS[word]))
        named = re.findall(r"`([a-z0-9]+)`", text[phrase.end():stop])
        if not named:
            fail("%s allows %s transmissions of a %s-slot format and names "
                 "no format that is one, so nothing says which formats the "
                 "count is for" % (MAN_PAGE, phrase.group(1), word))
        counts[SLOT_WORDS[word]] = (int(phrase.group(1)), named)
    if not counts:
        fail("%s no longer states the counts it allows as \"400 of a "
             "one-slot\", so what it promises the user is not checked"
             % MAN_PAGE)
    return counts


def check_man_page_examples(text, host, slots):
    """The page's worked `tx` commands against what the tool would accept.

    A command the page shows and the tool refuses is the same divergence read
    from the other end, and these lines state a length and a count of their
    own: `tx dh5 339 5` is the longest DH5 repeated five times.
    """
    shown = 0
    for line in text.split("\n"):
        command = re.match(r"\s+bt> tx\s+(\S+)(?:\s+(\d+))?(?:\s+(\d+))?",
                           line)
        if not command:
            continue
        shown += 1
        name, length, count = command.groups()
        if name not in host:
            fail("%s shows `tx %s` and TX_FORMATS has no such format"
                 % (MAN_PAGE, name))
        fmt = host[name]
        if length is not None and not (fmt["min"] <= int(length)
                                       <= fmt["max"]):
            fail("%s shows `tx %s %s` and %s carries %d to %d data bytes"
                 % (MAN_PAGE, name, length, name, fmt["min"], fmt["max"]))
        allowed = slots // fmt["slots"]
        if count is not None and not (1 <= int(count) <= allowed):
            fail("%s shows `tx %s %s %s` and 1 to %d transmissions are "
                 "allowed for %s" % (MAN_PAGE, name, length, count, allowed,
                                     name))
    if not shown:
        fail("%s shows no `bt> tx` command, so the commands it offers the "
             "user are not checked" % MAN_PAGE)


def check_man_page(root, host, slots):
    """The man page's figures against the table the host bounds a tx with."""
    text = read(os.path.join(root, MAN_PAGE))
    lines = man_page_list(text)
    lengths = man_page_lengths(lines)
    counts = man_page_counts(text)

    for name in sorted(set(lengths) | set(host)):
        if name not in lengths:
            fail("%s does not state what %s carries, so a format the tool "
                 "takes is undocumented" % (MAN_PAGE, name))
        if name not in host:
            fail("%s states what %s carries and TX_FORMATS has no such "
                 "format, so the page offers one the tool refuses"
                 % (MAN_PAGE, name))
        stated, real = lengths[name], (host[name]["min"], host[name]["max"])
        if stated != real:
            fail("%s says %s carries %d to %d data bytes and TX_FORMATS has "
                 "%d to %d" % ((MAN_PAGE, name) + stated + real))

    for packet_slots, (count, named) in sorted(counts.items()):
        allowed = slots // packet_slots
        if count != allowed:
            fail("%s allows %d transmissions of a %d-slot format and the "
                 "bound of %d slots allows %d"
                 % (MAN_PAGE, count, packet_slots, slots, allowed))
        for name in named:
            if name not in host:
                fail("%s gives %s as a %d-slot format and TX_FORMATS has no "
                     "such format" % (MAN_PAGE, name, packet_slots))
            if host[name]["slots"] != packet_slots:
                fail("%s gives %s as a %d-slot format and TX_FORMATS has it "
                     "filling %d" % (MAN_PAGE, name, packet_slots,
                                     host[name]["slots"]))
    for name in sorted(host):
        if host[name]["slots"] not in counts:
            fail("%s states no count for a %d-slot format, which %s is"
                 % (MAN_PAGE, host[name]["slots"], name))

    # A voice field is a figure of its own in the item's prose.
    stated = man_page_voices(lines)
    for name in sorted(host):
        carries = host[name]["voice"]
        if carries and name not in stated:
            fail("%s does not say %s carries %d bytes of voice, which it does"
                 % (MAN_PAGE, name, carries))
        if name in stated and stated[name] != carries:
            fail("%s says %s carries %d bytes of voice and TX_FORMATS has %d"
                 % (MAN_PAGE, name, stated[name], carries))
    for name in sorted(set(stated) - set(host)):
        fail("%s states a voice field for %s and TX_FORMATS has no such "
             "format" % (MAN_PAGE, name))

    check_man_page_examples(text, host, slots)


def check_divisions(root):
    for path, expression in DIVISIONS:
        text = read(os.path.join(root, path))
        if path.endswith(".c"):
            text = strip_comments(text, path)
            lines = [line.strip() for line in text.split("\n")]
        else:
            # Python: a comment after the expression is still that line.
            lines = [line.split("#")[0].strip()
                     for line in text.split("\n")]
        if expression not in lines:
            fail("%s no longer works the count out on a line of its own as "
                 "%r, so the two sides may allow different counts from "
                 "tables that agree. Check the other side divides the same "
                 "way, then put the new line in DIVISIONS."
                 % (path, expression))


def main():
    if len(sys.argv) != 2:
        fail("usage: test_tx_format_tables.py <repository root>")
    root = sys.argv[1]

    header = strip_comments(
        read(os.path.join(root,
                          "host/libubertooth/src/ubertooth_interface.h")),
        "ubertooth_interface.h")
    numbers = enum_members(header, "btctl_tx_format_e")
    if numbers.pop("BTCTL_FMT_COUNT", None) is None:
        fail("btctl_tx_format_e has no BTCTL_FMT_COUNT")

    device, holes = device_formats(root)
    btctl_py = os.path.join(root, "host/python/ubtbr/ubtbr/btctl.py")
    fields, host = host_formats(btctl_py)

    uncompared = sorted(set(fields) - set(f for f, _, _ in COMPARED))
    unknown = sorted(set(f for f, _, _ in COMPARED) - set(fields))
    if uncompared or unknown:
        fail("TxFormat's fields are %s and this test compares %s. A field of "
             "TX_FORMATS that is not in COMPARED is not checked against the "
             "firmware at all, so each one belongs there with what it means "
             "on the device's side."
             % (" ".join(fields), " ".join(f for f, _, _ in COMPARED)))

    # A format number the device's table leaves empty is one it refuses, so
    # the host has nothing to offer for it and must not name it.
    offered = sorted(holes & set("BTCTL_FMT_" + n.upper() for n in host))
    if offered:
        fail("TX_FORMATS offers %s, which bbcodec_tx_formats leaves empty, so "
             "the device refuses it" % ", ".join(offered))
    unnumbered = sorted(holes - set(numbers))
    if unnumbered:
        fail("bbcodec_tx_formats leaves %s empty and btctl_tx_format_e has no "
             "such format, so it is a name nothing sends"
             % ", ".join(unnumbered))

    # A `tx` reads its format name in lower case, so a name with a capital in
    # it is one the command line cannot reach.
    shouting = sorted(n for n in host if n != n.lower())
    if shouting:
        fail("TX_FORMATS names %s, and `tx` looks a format up in lower case, "
             "so that format cannot be asked for" % ", ".join(shouting))

    # The three lists of formats: the numbers they are sent as, the device's
    # table of layouts, and the host's. Each has to name exactly the same
    # formats, so a format added to one of them cannot be left out of another.
    named = {"btctl_tx_format_e": set(numbers) - holes,
             "bbcodec_tx_formats in codec.c": set(device),
             "TX_FORMATS in btctl.py":
                 set("BTCTL_FMT_" + n.upper() for n in host)}
    for this, mine in named.items():
        for other, theirs in named.items():
            missing = sorted(mine - theirs)
            if missing:
                fail("%s names %s, which %s does not. Every format belongs "
                     "in all three: the host and the device each bound a tx "
                     "command from their own table. A format number the "
                     "device is meant to refuse belongs in codec.c as an "
                     "entry with no air layout, written out as NULL, so that "
                     "it is a decision rather than an omission."
                     % (this, ", ".join(missing), other))

    for name in sorted(host):
        enum = "BTCTL_FMT_" + name.upper()
        theirs = dict(device[enum], code=numbers[enum])
        for field, wanted, what in COMPARED:
            if host[name][field] != theirs[wanted]:
                fail("%s: %s is %d in TX_FORMATS and %d in the firmware (%s)"
                     % (name, field, host[name][field], theirs[wanted], what))

    # The repeat count each side allows is this figure divided by the
    # format's slots, so a difference here is a difference in every count.
    slots = define_number(header, "BTCTL_TX_PKT_MAX_SLOTS",
                          "ubertooth_interface.h")
    module = {n.targets[0].id: n.value
              for n in ast.parse(read(btctl_py)).body
              if isinstance(n, ast.Assign) and len(n.targets) == 1
              and isinstance(n.targets[0], ast.Name)}
    mirrored = module.get("BTCTL_TX_PKT_MAX_SLOTS")
    if not isinstance(mirrored, ast.Constant) \
            or not isinstance(mirrored.value, int):
        fail("btctl.py has no BTCTL_TX_PKT_MAX_SLOTS of its own")
    if mirrored.value != slots:
        fail("BTCTL_TX_PKT_MAX_SLOTS is %d in btctl.py and %d in "
             "ubertooth_interface.h" % (mirrored.value, slots))
    check_divisions(root)
    check_man_page(root, host, slots)

    print("PASS: %d formats, %d fields each, agree between TX_FORMATS, the "
          "firmware and the man page, with the same slot bound divided the "
          "same way" % (len(host), len(COMPARED)))


if __name__ == "__main__":
    main()
