#!/bin/sh
# Regenerate ubertooth-one-schematic.pdf from ubertooth-one.sch.
#
# ubertooth-one.sch is in the format KiCad used in 2011, which keeps symbol
# graphics in separate .lib libraries and names each symbol without saying which
# library it came from. Current KiCad plots that file with every wire, label,
# reference and value in place but no symbol bodies at all, because the step
# that matches those bare symbol names to a library is part of the schematic
# editor's one-time migration and is not available from kicad-cli. Supplying the
# libraries through a project or global symbol library table does not help: the
# plot comes out byte for byte identical.
#
# So this script converts instead of plotting directly. It reads the project's
# own cache library, which holds a copy of every symbol the schematic uses,
# converts it to the current .kicad_sym format, and writes a .kicad_sch that
# embeds those symbol graphics alongside the converted wires, labels, junctions
# and text. kicad-cli then plots that, with the symbols drawn.
#
# The conversion is checked, not assumed: the script exports a netlist from the
# converted schematic and compares it against ubertooth-one.net, the netlist the
# original design produced. Component references, pin numbers, every net and
# every deliberately unconnected pin must match, or the script fails and writes
# no PDF.
#
# Nothing under hardware/ubertooth-one is modified. The design files are read
# only; everything else happens in a temporary directory.
#
# Usage: ./make-schematic-pdf.sh [-o output.pdf] [-k path/to/kicad-cli]
#
# Needs KiCad 9 or later (for kicad-cli) and Python 3. The PDF records its own
# creation time, so a rerun produces an equivalent but not byte-identical file.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="$here/ubertooth-one-schematic.pdf"
kicad=

while [ $# -gt 0 ]; do
	case $1 in
	-o)
		[ $# -ge 2 ] || { echo "error: -o needs a file name" >&2; exit 1; }
		out=$2
		shift 2
		;;
	-k)
		[ $# -ge 2 ] || { echo "error: -k needs a file name" >&2; exit 1; }
		kicad=$2
		shift 2
		;;
	-h)
		cat <<'USAGE'
Usage: make-schematic-pdf.sh [-o output.pdf] [-k path/to/kicad-cli]

Regenerate ubertooth-one-schematic.pdf from ubertooth-one.sch. Needs KiCad 9
or later and Python 3. The design files are read only. The conversion is
checked against ubertooth-one.net; nothing is written if the check fails.
Comments at the top of this script explain why the conversion is needed.
USAGE
		exit 0
		;;
	*)
		echo "error: unknown argument '$1'" >&2
		exit 1
		;;
	esac
done

if [ -z "$kicad" ]; then
	for candidate in \
		kicad-cli \
		/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli \
		/Volumes/KiCad/KiCad/KiCad.app/Contents/MacOS/kicad-cli \
		/usr/bin/kicad-cli \
		/usr/local/bin/kicad-cli
	do
		if command -v "$candidate" >/dev/null 2>&1; then
			kicad=$candidate
			break
		fi
	done
fi
if [ -z "$kicad" ]; then
	echo "error: kicad-cli not found; install KiCad 9 or later, or pass -k <path>" >&2
	exit 1
fi
command -v python3 >/dev/null 2>&1 || { echo "error: python3 not found" >&2; exit 1; }

for f in ubertooth-one.sch ubertooth-one-cache.lib ubertooth-one.net; do
	[ -r "$here/$f" ] || { echo "error: cannot read $here/$f" >&2; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "converting the cache library to the current symbol format" >&2
cp "$here/ubertooth-one-cache.lib" "$work/cache.lib"
"$kicad" sym upgrade "$work/cache.lib" -o "$work/cache.kicad_sym" >/dev/null

cat > "$work/convert.py" <<'PYTHON_END'
"""Convert a 2011-format KiCad schematic to the current .kicad_sch format.

Usage: convert.py <legacy.sch> <cache.kicad_sym> <legacy-cache.lib> <out.kicad_sch>
"""
import re
import sys
import uuid as uuidmod

MIL = 0.0254  # one mil in millimetres

# Legacy files store a symbol's placement as a 2x2 matrix mapping symbol
# coordinates (y upwards) to sheet coordinates (y downwards). These are the four
# matrices this schematic uses, with the rotation angle each one corresponds to
# in the current format. The angles were measured, not guessed: a probe
# schematic placed a known symbol at each angle and the exported netlist showed
# where its pins landed.
ANGLES = {
    (1, 0, 0, -1): 0,
    (-1, 0, 0, 1): 180,
    (0, 1, 1, 0): 270,
    (0, -1, -1, 0): 90,
}

FIELD_RE = re.compile(
    r'^F\s+(\d+)\s+"((?:[^"\\]|\\.)*)"\s+([HV])\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+'
    r'(\d+)\s+([LRC])\s+([LRCTB])([NI])([NB])(?:\s+"((?:[^"\\]|\\.)*)")?\s*$'
)
FIELD_NAMES = {0: "Reference", 1: "Value", 2: "Footprint", 3: "Datasheet"}

# The 1V8 power symbol is an alias of +1.8V with identical graphics, so point it
# at the parent and the embedded library needs no derived symbols.
ALIASES = {"1V8": "+1.8V"}


def mm(v):
    s = f"{v * MIL:.4f}".rstrip("0").rstrip(".")
    return s or "0"


def q(s):
    # A legacy text item encodes a line break as the two characters
    # backslash-n. Reduce it to a real newline, escape what has to be escaped,
    # then write the line break back as the escape the current reader expects.
    s = s.replace("\\n", "\n")
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return '"' + s + '"'


def stable_uuid(*parts):
    """A uuid derived from the item's own data, so reruns produce the same file."""
    return str(uuidmod.uuid5(uuidmod.NAMESPACE_URL,
                             "ubertooth-one/" + "/".join(map(str, parts))))


def lib_id_of(name):
    return "cache:" + ALIASES.get(name, name)


def sexpr_at(text, start):
    """Return the balanced parenthesised expression beginning at start."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise SystemExit("error: unbalanced s-expression in the symbol library")


def top_level_symbols(text):
    """{name: s-expression} for each top-level (symbol "name" ...) in a .kicad_sym."""
    out = {}
    pos = 0
    pattern = re.compile(r'\(symbol\s+"((?:[^"\\]|\\.)*)"')
    while True:
        m = pattern.search(text, pos)
        if not m:
            return out
        body = sexpr_at(text, m.start())
        out[m.group(1)] = body
        # Skip the whole symbol, so its nested per-unit (symbol ...) blocks are
        # not mistaken for top-level ones.
        pos = m.start() + len(body)


def legacy_pins(lib_text):
    """{symbol name: [(pin number, x, y)]} connection points, in symbol coordinates."""
    pins = {}
    name = None
    for line in lib_text.splitlines():
        if line.startswith("DEF "):
            name = line.split()[1].lstrip("~")
            pins.setdefault(name, [])
        elif line.startswith("ALIAS ") and name:
            for a in line.split()[1:]:
                pins[a] = pins[name]
        elif line.startswith("X ") and name is not None:
            f = line.split()
            pins[name].append((f[2], int(f[3]), int(f[4])))
        elif line.startswith("ENDDEF"):
            name = None
    return pins


class Comp:
    __slots__ = ("name", "ref", "unit", "stamp", "x", "y", "mat", "fields")


def parse_sch(text):
    lines = text.splitlines()
    descr, comps, wires, notes = {}, [], [], []
    juncs, nocons, labels, texts = [], [], [], []
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("$Descr"):
            f = line.split()
            descr["paper"] = (f[1], int(f[2]), int(f[3]))
            i += 1
            while not lines[i].startswith("$EndDescr"):
                m = re.match(r'^(\w+)\s+"((?:[^"\\]|\\.)*)"\s*$', lines[i])
                if m:
                    descr[m.group(1)] = m.group(2)
                i += 1
        elif line.startswith("$Comp"):
            c = Comp()
            c.fields = []
            c.unit, c.stamp, c.mat = 1, "", (1, 0, 0, -1)
            i += 1
            while not lines[i].startswith("$EndComp"):
                s = lines[i]
                if s.startswith("L "):
                    _, name, ref = s.split(None, 2)
                    c.name, c.ref = name.strip(), ref.strip()
                elif s.startswith("U "):
                    f = s.split()
                    c.unit, c.stamp = int(f[1]), f[3]
                elif s.startswith("P "):
                    f = s.split()
                    c.x, c.y = int(f[1]), int(f[2])
                elif s.startswith("F "):
                    m = FIELD_RE.match(s)
                    if not m:
                        raise SystemExit("error: cannot parse field line: " + s)
                    c.fields.append(m.groups())
                elif re.match(r'^\s+-?\d+\s+-?\d+\s+-?\d+\s+-?\d+\s*$', s):
                    c.mat = tuple(int(v) for v in s.split())
                i += 1
            comps.append(c)
        elif line.startswith("Wire Wire Line"):
            i += 1
            wires.append([int(v) for v in lines[i].split()])
        elif line.startswith("Wire Notes Line"):
            i += 1
            notes.append([int(v) for v in lines[i].split()])
        elif line.startswith("Connection "):
            f = line.split()
            juncs.append((int(f[2]), int(f[3])))
        elif line.startswith("NoConn "):
            f = line.split()
            nocons.append((int(f[2]), int(f[3])))
        elif line.startswith("Text Label ") or line.startswith("Text Notes "):
            f = line.split()
            kind = f[1]
            item = (lines[i + 1], int(f[2]), int(f[3]), int(f[4]), int(f[5]))
            i += 1
            (labels if kind == "Label" else texts).append(item)
        i += 1
    return descr, comps, wires, notes, juncs, nocons, labels, texts


def pin_positions(comps, pins):
    """[(ref, pin number, sheet x, sheet y)] for every pin of every component."""
    out = []
    for c in comps:
        a, b, cc, d = c.mat
        for num, px, py in pins.get(c.name, []):
            out.append((c.ref, num, c.x + a * px + b * py, c.y + cc * px + d * py))
    return out


def split_wires(wires, nodes):
    """Split each wire at every node lying strictly inside it.

    A legacy schematic lets a pin, or another wire, meet the middle of a wire
    with nothing but a junction dot to mark it. The current format joins wires
    at their end points, so each wire has to be cut into segments at those
    nodes or those connections are lost.
    """
    out = []
    for x1, y1, x2, y2 in wires:
        dx, dy = x2 - x1, y2 - y1
        inner = []
        for nx, ny in nodes:
            if (nx, ny) in ((x1, y1), (x2, y2)):
                continue
            if dx * (ny - y1) != dy * (nx - x1):
                continue  # not on the line through the wire
            if not (min(x1, x2) <= nx <= max(x1, x2)
                    and min(y1, y2) <= ny <= max(y1, y2)):
                continue  # on the line but beyond the wire's ends
            inner.append((nx, ny))
        if not inner:
            out.append((x1, y1, x2, y2))
            continue
        inner.sort(key=lambda p: (p[0] - x1) * dx + (p[1] - y1) * dy)
        prev = (x1, y1)
        for pt in inner:
            out.append((prev[0], prev[1], pt[0], pt[1]))
            prev = pt
        out.append((prev[0], prev[1], x2, y2))
    return out


def emit(descr, comps, wires, notes, juncs, nocons, labels, texts, libsyms):
    root = stable_uuid("root")
    o = []
    w = o.append
    w("(kicad_sch")
    w("\t(version 20231120)")
    w('\t(generator "make-schematic-pdf")')
    w(f"\t(uuid {q(root)})")
    paper, pw, ph = descr["paper"]
    w(f"\t(paper {q(paper)} {mm(pw)} {mm(ph)})")
    w("\t(title_block")
    for key, tok in (("Title", "title"), ("Date", "date"),
                     ("Rev", "rev"), ("Comp", "company")):
        if descr.get(key):
            w(f"\t\t({tok} {q(descr[key])})")
    for n in range(1, 5):
        if descr.get(f"Comment{n}"):
            w(f"\t\t(comment {n} {q(descr[f'Comment{n}'])})")
    w("\t)")

    w("\t(lib_symbols")
    for lib_id in sorted({lib_id_of(c.name) for c in comps}):
        bare = lib_id.split(":", 1)[1]
        if bare not in libsyms:
            raise SystemExit(f"error: {bare} is not in the cache library")
        # Rename the library's bare symbol name to the nickname-qualified id
        # the placements refer to.
        body = re.sub(r'^\(symbol\s+"(?:[^"\\]|\\.)*"',
                      lambda _m: "(symbol " + q(lib_id), libsyms[bare], count=1)
        w("\n".join("\t\t" + ln for ln in body.splitlines()))
    w("\t)")

    for n, (x1, y1, x2, y2) in enumerate(wires):
        w(f"\t(wire (pts (xy {mm(x1)} {mm(y1)}) (xy {mm(x2)} {mm(y2)}))")
        w("\t\t(stroke (width 0) (type default))")
        w(f'\t\t(uuid {q(stable_uuid("wire", n, x1, y1, x2, y2))})')
        w("\t)")

    for n, (x1, y1, x2, y2) in enumerate(notes):
        w(f"\t(polyline (pts (xy {mm(x1)} {mm(y1)}) (xy {mm(x2)} {mm(y2)}))")
        w("\t\t(stroke (width 0) (type dash))")
        w(f'\t\t(uuid {q(stable_uuid("note", n, x1, y1, x2, y2))})')
        w("\t)")

    for n, (x, y) in enumerate(juncs):
        w(f"\t(junction (at {mm(x)} {mm(y)}) (diameter 0) (color 0 0 0 0)")
        w(f'\t\t(uuid {q(stable_uuid("junction", n, x, y))})')
        w("\t)")

    for n, (x, y) in enumerate(nocons):
        w(f"\t(no_connect (at {mm(x)} {mm(y)})"
          f' (uuid {q(stable_uuid("noconn", n, x, y))}))')

    for n, (body, x, y, orient, size) in enumerate(labels):
        w(f"\t(label {q(body)} (at {mm(x)} {mm(y)} {orient * 90})")
        w(f"\t\t(effects (font (size {mm(size)} {mm(size)})) (justify left))")
        w(f'\t\t(uuid {q(stable_uuid("label", n, body, x, y))})')
        w("\t)")

    for n, (body, x, y, orient, size) in enumerate(texts):
        w(f"\t(text {q(body)} (at {mm(x)} {mm(y)} {orient * 90})")
        w(f"\t\t(effects (font (size {mm(size)} {mm(size)})) (justify left bottom))")
        w(f'\t\t(uuid {q(stable_uuid("text", n, body, x, y))})')
        w("\t)")

    for c in comps:
        if c.mat not in ANGLES:
            raise SystemExit(f"error: {c.ref} has an unhandled transform {c.mat}")
        w("\t(symbol")
        w(f"\t\t(lib_id {q(lib_id_of(c.name))})")
        w(f"\t\t(at {mm(c.x)} {mm(c.y)} {ANGLES[c.mat]})")
        w(f"\t\t(unit {c.unit})")
        w("\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)")
        w(f'\t\t(uuid {q(stable_uuid("sym", c.stamp, c.ref, c.x, c.y))})')
        named = set()
        for g in c.fields:
            idx = int(g[0])
            name = FIELD_NAMES.get(idx, g[11] or f"Field{idx}")
            named.add(name)
            value, orient, fx, fy, fsize = g[1], g[2], int(g[3]), int(g[4]), int(g[5])
            font = f"(size {mm(fsize)} {mm(fsize)})"
            if g[9] == "I":
                font += " (italic yes)"
            if g[10] == "B":
                font += " (bold yes)"
            eff = f"(font {font})"
            just = " ".join(t for t in ({"L": "left", "R": "right", "C": ""}[g[7]],
                                        {"T": "top", "B": "bottom",
                                         "C": "", "L": "", "R": ""}[g[8]]) if t)
            if just:
                eff += f" (justify {just})"
            if int(g[6]) & 1:
                eff += " (hide yes)"
            w(f"\t\t(property {q(name)} {q(value)} "
              f"(at {mm(fx)} {mm(fy)} {90 if orient == 'V' else 0})")
            w(f"\t\t\t(effects {eff})")
            w("\t\t)")
        # The current format expects these four to be present on every symbol.
        for idx in (0, 1, 2, 3):
            if FIELD_NAMES[idx] not in named:
                w(f'\t\t(property "{FIELD_NAMES[idx]}" "" '
                  f"(at {mm(c.x)} {mm(c.y)} 0)")
                w("\t\t\t(effects (font (size 1.27 1.27)) (hide yes))")
                w("\t\t)")
        # The instance path has to name the root sheet's own uuid, or KiCad
        # reads every symbol as unannotated and leaves unnamed nets nameless.
        w("\t\t(instances")
        w('\t\t\t(project ""')
        w(f'\t\t\t\t(path "/{root}" (reference {q(c.ref)}) (unit {c.unit}))')
        w("\t\t\t)")
        w("\t\t)")
        w("\t)")

    w("\t(sheet_instances")
    w('\t\t(path "/" (page "1"))')
    w("\t)")
    w(")")
    return "\n".join(o) + "\n"


def main():
    sch_path, sym_path, lib_path, out_path = sys.argv[1:5]
    sch = open(sch_path, encoding="utf-8", errors="replace").read()
    libsyms = top_level_symbols(open(sym_path, encoding="utf-8",
                                     errors="replace").read())
    pins = legacy_pins(open(lib_path, encoding="utf-8", errors="replace").read())

    descr, comps, wires, notes, juncs, nocons, labels, texts = parse_sch(sch)
    positions = pin_positions(comps, pins)

    nodes = set(juncs) | set(nocons) | {(x, y) for _r, _n, x, y in positions}
    nodes |= {(x, y) for _b, x, y, _o, _s in labels}
    for x1, y1, x2, y2 in wires:
        nodes.add((x1, y1))
        nodes.add((x2, y2))
    segments = split_wires(wires, nodes)

    open(out_path, "w", encoding="utf-8").write(
        emit(descr, comps, segments, notes, juncs, nocons, labels, texts, libsyms))

    sys.stderr.write(
        f"converted {len(comps)} components, {len(wires)} wires "
        f"({len(segments)} segments), {len(juncs)} junctions, "
        f"{len(labels)} labels, {len(texts)} notes, "
        f"{len(nocons)} no-connects\n")


if __name__ == "__main__":
    main()
PYTHON_END

cat > "$work/checknet.py" <<'PYTHON_END'
"""Compare a netlist exported from the converted schematic against the original.

Usage: checknet.py <original.net> <converted.net>

Net names are not compared: an unnamed net is named after one of its own pins,
which differs between the two. What is compared is which pins share a net, which
component references exist, which pins each has, and which pins are unconnected.
"""
import re
import sys

HEADER = re.compile(r'^\s*\(\s*/\S+\s+(?:\S+)\s+(\S+)\s')
PIN = re.compile(r'^\s*\(\s*(\S+)\s+(\S+)\s*\)\s*$')
UNCONNECTED = ("?", "/<NO_NET>", "unconnected-")


def parse(path):
    comps = {}
    ref = None
    for line in open(path, errors="replace"):
        m = HEADER.match(line)
        if m:
            ref = m.group(1)
            comps.setdefault(ref, {})
            continue
        m = PIN.match(line)
        if m and ref:
            comps[ref][m.group(1)] = m.group(2)
    return comps


def unconnected(net):
    return net.startswith(UNCONNECTED) or net in UNCONNECTED


def nets(comps):
    out = {}
    for ref, pins in comps.items():
        for pin, net in pins.items():
            if unconnected(net):
                continue
            out.setdefault(net, set()).add((ref, pin))
    return frozenset(frozenset(v) for v in out.values() if len(v) > 1)


def floating(comps):
    return {(r, p) for r, pins in comps.items()
            for p, n in pins.items() if unconnected(n)}


def main():
    want, got = parse(sys.argv[1]), parse(sys.argv[2])
    problems = []

    if set(want) != set(got):
        problems.append(f"component references differ: "
                        f"missing {sorted(set(want) - set(got))}, "
                        f"extra {sorted(set(got) - set(want))}")
    for ref in sorted(set(want) & set(got)):
        if set(want[ref]) != set(got[ref]):
            problems.append(f"{ref} pin numbers differ")

    a, b = nets(want), nets(got)
    if a != b:
        problems.append(f"{len(a - b)} of {len(a)} nets are not reproduced, "
                        f"and {len(b - a)} nets are not in the original")
        for net in sorted(a - b, key=len)[:5]:
            problems.append("  original net not reproduced: " + str(sorted(net)))
        for net in sorted(b - a, key=len)[:5]:
            problems.append("  net not in the original: " + str(sorted(net)))

    fa, fb = floating(want), floating(got)
    if fa != fb:
        problems.append(f"unconnected pins differ: "
                        f"{sorted(fa - fb)[:5]} only in the original, "
                        f"{sorted(fb - fa)[:5]} only in the conversion")

    if problems:
        sys.stderr.write("error: the converted schematic is not electrically "
                         "identical to the original\n")
        for p in problems:
            sys.stderr.write("  " + p + "\n")
        return 1

    sys.stderr.write(f"netlist check passed: {len(want)} components, "
                     f"{sum(len(v) for v in want.values())} pins, "
                     f"{len(a)} nets, {len(fa)} unconnected pins, all identical\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
PYTHON_END

echo "converting the schematic" >&2
python3 "$work/convert.py" \
	"$here/ubertooth-one.sch" \
	"$work/cache.kicad_sym" \
	"$here/ubertooth-one-cache.lib" \
	"$work/ubertooth-one.kicad_sch"

echo "checking the conversion against ubertooth-one.net" >&2
"$kicad" sch export netlist --format orcadpcb2 \
	-o "$work/converted.net" "$work/ubertooth-one.kicad_sch" >/dev/null
python3 "$work/checknet.py" "$here/ubertooth-one.net" "$work/converted.net"

echo "plotting $out" >&2
"$kicad" sch export pdf -o "$work/schematic.pdf" "$work/ubertooth-one.kicad_sch" >/dev/null
cp "$work/schematic.pdf" "$out"
