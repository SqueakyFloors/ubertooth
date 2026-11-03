#!/usr/bin/env python3
"""Each basic-rate transmit format is described twice, and the two must agree.

Run as: test_tx_format_tables.py <repository root>

A `tx` command names a format, a data length and a repeat count, and both ends
bound those from a description of the format they keep separately:

  * the host, from TX_FORMATS in host/python/ubtbr/ubtbr/btctl.py, which is
    what ubertooth-btbr's `tx` checks the length and the count against before
    anything leaves the machine;
  * the device, from bbcodec_tx_formats and the layouts it points at in
    firmware/btbr/src/codec/codec.c, which is what btctl_handle_tx_pkt_req()
    checks the same two against before it puts a packet on the air.

Nothing compared them, so a format whose slot count or data length changed on
one side only would leave the host accepting a `tx` the device then refuses on
its console, where the user cannot see it. This reads both descriptions out of
the source and checks they say the same thing about every format: its number,
how many air slots one packet fills, how many data bytes it carries, and the
payload header and voice field the host fills in. Every field of TX_FORMATS is
compared, so a field added to it has to be given a meaning here rather than
going unchecked.

The count a `tx` may ask for is BTCTL_TX_PKT_MAX_SLOTS divided by the format's
slots, so that figure is compared as well, and the one line each side divides
on is pinned: the two tables agreeing is no use if one side stops dividing the
same way.

What this does not check. It compares the two descriptions, not the code that
reads them: a side that stopped checking a length or a count against its own
table, or that compared it the wrong way round, would pass here. Those checks
are each other's tests, in test_ubtbr.py and in firmware/tests.

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

def strip_comments(text):
    """The text with /* ... */ and // ... comments replaced by spaces.

    Line structure is kept, so a pinned function body still reads as one.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                fail("unterminated block comment in a scanned source file")
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


def braced(text, start):
    """The text inside the first { ... } at or after start."""
    try:
        open_at = text.index("{", start)
    except ValueError:
        fail("no initializer or body found where one was expected")
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
                    fail("a scanned table or body carries a preprocessor "
                         "directive, so what the build uses is not what this "
                         "test would read: %s"
                         % [l.strip() for l in region.split("\n")
                            if "#" in l][0])
                return region
    fail("unbalanced braces in a scanned source file")


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
    return braced(text, found[0].end())


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
    "if (t->payload_header_bytes != 0) { *min_data = 0; } "
    "else if (t->has_crc) { *min_data = 1; } "
    "else { *min_data = *max_data; }"
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


def device_limits(layout):
    """The data bytes a format accepts, as the device decides it."""
    biggest = layout["payload_bytes"]
    if layout["payload_header_bytes"] != 0:
        return 0, biggest
    if layout["has_crc"]:
        return 1, biggest
    return biggest, biggest


def device_formats(root):
    """Each format the device can be asked for, by name, from the firmware."""
    codec_c = strip_comments(
        read(os.path.join(root, "firmware/btbr/src/codec/codec.c")))
    codec_h = strip_comments(
        read(os.path.join(root, "firmware/btbr/include/ubtbr/codec.h")))
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
        values = split_top(braced(body, 0) if body.lstrip().startswith("{")
                           else body)
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
        parts = split_top(braced(entry, 0))
        if len(parts) != 2:
            fail("bbcodec_tx_formats[%s] is not an air TYPE and a layout: %s"
                 % (name, entry))
        if parts[1] == "NULL":
            # bbcodec_tx_format() refuses a format number this table leaves
            # empty, which is a format the device cannot be asked for at all.
            holes.add(name)
            continue
        air = layout(parts[1])
        for field in ("nslots", "payload_bytes", "payload_header_bytes",
                      "has_crc", "voice_bytes"):
            if not isinstance(air.get(field), int):
                fail("bbcodec_tx_formats[%s] gives %s as %r, not a number"
                     % (name, field, air.get(field)))
        least, most = device_limits(air)
        formats[name] = {
            "slots": air["nslots"],
            "min": least,
            "max": most,
            "hlen": air["payload_header_bytes"],
            "voice": air["voice_bytes"],
        }
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


def check_divisions(root):
    for path, expression in DIVISIONS:
        text = read(os.path.join(root, path))
        if path.endswith(".c"):
            text = strip_comments(text)
        if expression not in [line.strip() for line in text.split("\n")]:
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
        read(os.path.join(root, "host/libubertooth/src/ubertooth_interface.h")))
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

    print("PASS: %d formats, %d fields each, agree between TX_FORMATS and "
          "the firmware, with the same slot bound divided the same way"
          % (len(host), len(COMPARED)))


if __name__ == "__main__":
    main()
