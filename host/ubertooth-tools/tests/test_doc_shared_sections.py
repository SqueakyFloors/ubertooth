#!/usr/bin/env python3
"""A man page must carry the shared sections its page is meant to have.

Run as: test_doc_shared_sections.py <host source dir>

Several sections of host/doc/*.md are shared by design: the same heading, with
the same subject under it, on every page it belongs on. Nothing pinned that.
A failed edit took GAPS IN A CAPTURE out of ubertooth-dump.md while the other
gap-reporting pages kept it, and no test or CI check noticed; the section was
put back by hand.

SHARED_SECTIONS below names each such heading and the pages that carry it. The
set has to match the pages exactly, both ways:

  * a page in the list that has lost the heading fails here, which is the
    dropped section this test exists to catch;
  * a page that carries the heading and is not in the list fails too, so the
    list cannot fall behind the pages. Adding a tool that reports capture
    gaps, or one that transmits, means adding its page to the group here.

ALL_PAGES is checked against the directory for the same reason: a new page has
to be named here, which is the moment to decide which of the groups it is in.

The headings left out, and why. ARGUMENTS belongs to a tool that takes
positional arguments and DIAGNOSTICS, EXAMPLES and AUTHOR follow what a page
has to say rather than a shared design, so there is nothing to pin. The
sections a single page carries -- SQUELCH, WATCHDOG, MONITOR MODE and the rest
-- are that tool's own.

The pages themselves are read, not the generated roff in host/doc/man: the .md
is the source every page but ubertooth-debug.1 is rendered from, and
test_manpages.py already holds the rendering down.
"""

import glob
import os
import re
import sys

# Every page in host/doc, as a group each heading below is drawn from.
#
# ubertooth.md is the section 7 overview: it describes the project and the
# tools rather than a command, so it carries none of a tool page's sections.
OVERVIEW_PAGES = {"ubertooth.md"}

# The section 1 pages: one command each, which is what gives them a synopsis,
# options, and the rest.
TOOL_PAGES = {
    "btle-extcap.md",
    "ubertooth-afh.md",
    "ubertooth-btbr.md",
    "ubertooth-btle.md",
    "ubertooth-debug.md",
    "ubertooth-dfu.md",
    "ubertooth-ducky.md",
    "ubertooth-dump.md",
    "ubertooth-ego.md",
    "ubertooth-follow.md",
    "ubertooth-rx.md",
    "ubertooth-scan.md",
    "ubertooth-specan-ui.md",
    "ubertooth-specan.md",
    "ubertooth-tx.md",
    "ubertooth-util.md",
}

ALL_PAGES = TOOL_PAGES | OVERVIEW_PAGES

# The pages that give their exit statuses in a section of their own. The two
# spectrum analyzer pages describe each outcome where they describe what led
# to it -- a refused command line, a write that failed, a sweep the board
# would not stop -- and give the status there, so they have no section to
# carry.
EXIT_STATUS_PAGES = TOOL_PAGES - {"ubertooth-specan.md",
                                  "ubertooth-specan-ui.md"}

# The capture tools, whose page says what a gap in a capture is and how the
# tool reports one. ubertooth-btle is the one gap-reporting tool missing here:
# its page describes the same losses inside DIAGNOSTICS, among the other
# warnings a capture can print, rather than in a section of its own.
CAPTURE_PAGES = {
    "ubertooth-afh.md",
    "ubertooth-dump.md",
    "ubertooth-ego.md",
    "ubertooth-follow.md",
    "ubertooth-rx.md",
    "ubertooth-scan.md",
    "ubertooth-specan.md",
}

# The tools whose transmitting uses the CC2400 power level `ubertooth-util -a`
# sets, which is what their shared section describes. ubertooth-btbr transmits
# too, and its page talks about turning a level down for a bench rather than
# about the level itself.
TRANSMIT_PAGES = {
    "ubertooth-btle.md",
    "ubertooth-ducky.md",
    "ubertooth-ego.md",
    "ubertooth-tx.md",
}

# Each shared heading and the pages that carry it, with what it is for.
SHARED_SECTIONS = (
    ("SYNOPSIS", TOOL_PAGES, "how the command is called"),
    ("DESCRIPTION", TOOL_PAGES, "what the command does"),
    ("OPTIONS", TOOL_PAGES, "the options the command takes"),
    ("EXIT STATUS", EXIT_STATUS_PAGES, "what the command's exit status means"),
    ("SEE ALSO", TOOL_PAGES, "the other pages a reader wants"),
    ("COPYRIGHT", ALL_PAGES, "the license the page is under"),
    ("GAPS IN A CAPTURE", CAPTURE_PAGES,
     "what a gap in a capture is and how the tool reports one"),
    ("TRANSMIT POWER", TRANSMIT_PAGES,
     "the power level the tool transmits at"),
)

# The call every tool makes to report the capture losses it counted. A tool
# that makes it has gaps to describe, so its page belongs in CAPTURE_PAGES or
# among the exceptions named above it.
LOSS_REPORT_CALL = "ubertooth_report_capture_losses"

# The gap-reporting tools whose page carries no GAPS IN A CAPTURE section,
# with the reason. Kept apart from CAPTURE_PAGES so the cross-check below has
# something to compare against rather than a silent omission.
GAPS_ELSEWHERE = {
    # Described inside the page's DIAGNOSTICS, see CAPTURE_PAGES above.
    "ubertooth-btle.md",
}

HEADING = re.compile(r"^## (.+?)\s*$")


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def page_sections(doc):
    """The `## ` headings of each page in doc, by file name."""
    sections = {}
    paths = sorted(glob.glob(os.path.join(doc, "*.md")))
    if not paths:
        fail("no man page sources in %s" % doc)
    for path in paths:
        found = []
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                heading = HEADING.match(line)
                if heading:
                    found.append(heading.group(1))
        sections[os.path.basename(path)] = found
    return sections


def check_pages_named(sections):
    """Every page in host/doc has to be in one of the groups above."""
    unnamed = sorted(set(sections) - ALL_PAGES)
    if unnamed:
        fail("%s is not in ALL_PAGES, so no shared section is checked on it. "
             "Add it to TOOL_PAGES or OVERVIEW_PAGES, and to the groups whose "
             "sections it carries." % ", ".join(unnamed))
    gone = sorted(ALL_PAGES - set(sections))
    if gone:
        fail("%s is named here but is not in host/doc any more; take it out "
             "of the groups above" % ", ".join(gone))


def check_section(sections, heading, pages, what):
    """The pages carrying this heading have to be the pages listed for it."""
    carrying = set(name for name, found in sections.items()
                   if heading in found)
    missing = sorted(pages - carrying)
    if missing:
        fail("%s has no %s section, which every page listed for it carries "
             "(%s). Put the section back, or take the page out of the group "
             "in this test and say why."
             % (", ".join(missing), heading, what))
    extra = sorted(carrying - pages)
    if extra:
        fail("%s carries a %s section and is not listed for it here. Add it "
             "to the group, so the next page that loses the section fails "
             "this test." % (", ".join(extra), heading))
    return len(carrying)


def gap_reporting_pages(host_src):
    """The page of every tool that reports the losses of a capture."""
    pages = set()
    sources = sorted(glob.glob(os.path.join(host_src, "ubertooth-tools",
                                            "src", "*.c")))
    if not sources:
        fail("no tool sources under %s" % host_src)
    for path in sources:
        with open(path, encoding="utf-8", errors="replace") as handle:
            if LOSS_REPORT_CALL in handle.read():
                name = os.path.basename(path)
                pages.add(name[:-len(".c")] + ".md")
    if not pages:
        fail("no tool source calls %s, so the tools no longer report capture "
             "losses the way this check assumes" % LOSS_REPORT_CALL)
    return pages


def check_gap_reporting_tools(host_src):
    """A tool that reports gaps has to have a page that describes them.

    The pages are listed above by hand, and this is what keeps the list honest
    for a tool that is added later: a new capture tool calls the same report,
    so its page has to join CAPTURE_PAGES or be named in GAPS_ELSEWHERE with
    the reason.
    """
    reporting = gap_reporting_pages(host_src)
    unaccounted = sorted(reporting - CAPTURE_PAGES - GAPS_ELSEWHERE)
    if unaccounted:
        fail("%s reports the losses of a capture (it calls %s) and its page "
             "is in neither CAPTURE_PAGES nor GAPS_ELSEWHERE. Give the page a "
             "GAPS IN A CAPTURE section and list it, or say in GAPS_ELSEWHERE "
             "where it describes them instead."
             % (", ".join(unaccounted), LOSS_REPORT_CALL))
    silent = sorted((CAPTURE_PAGES | GAPS_ELSEWHERE) - reporting)
    if silent:
        fail("%s is listed as a page that describes capture gaps, but its "
             "tool does not call %s any more. Either the call went, in which "
             "case the section and the entry go with it, or the tool reports "
             "losses another way and this check needs to know about it."
             % (", ".join(silent), LOSS_REPORT_CALL))


def main():
    if len(sys.argv) != 2:
        fail("usage: test_doc_shared_sections.py <host source dir>")
    host_src = sys.argv[1]

    sections = page_sections(os.path.join(host_src, "doc"))
    check_pages_named(sections)
    checked = 0
    for heading, pages, what in SHARED_SECTIONS:
        if not pages:
            fail("%s is listed with no pages, so it is not checked" % heading)
        checked += check_section(sections, heading, pages, what)
    check_gap_reporting_tools(host_src)

    print("PASS: %d shared sections across %d man pages, %d headings"
          % (checked, len(sections), len(SHARED_SECTIONS)))


if __name__ == "__main__":
    main()
