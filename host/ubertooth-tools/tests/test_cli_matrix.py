#!/usr/bin/env python3
"""One command-line contract, checked for every tool from one spec.

Run as: test_cli_matrix.py <path to cli_spec.json> <path to one tool>

The rules are the project's, not this test's:

  * .kiro/steering/cli-output.md -- which stream each kind of line is on, what
    the exit status means, and that the usage text follows only a bare command
    line (on standard error) or -h (on standard output);
  * host/doc/ubertooth.md, OPTION VALUES -- an option value is a separate word
    or attached, an optional value is taken from the next word only when it is
    a valid value, and a lone "-" as a file value means standard output;
  * host/doc/<tool>.md -- what each option does, and which combinations the
    tool refuses.

cli_spec.json writes those down per tool. This file turns them into runs of
the real binary and checks what comes back. The cases are generated, not
listed: a bare command line, -h, each option on its own, each option with a
missing, out-of-range or malformed value, the attached and the separate form
of every value, every pair of options, -h beside a bad option, a stray word,
"--", and a file option given "-".

Every run has UBERTOOTH_TEST_NO_DEVICES=1 set, so no board is ever opened,
whatever is attached: a case the tool accepts reaches device lookup and stops
there, and a case it refuses must not reach even that.

A case the tools do not satisfy yet is listed in the spec's
"expected_failures" with its finding id. It is then required to keep failing,
so the test stays green while the findings are open and a fix flips exactly
one case to passing -- at which point this test fails until the entry is
removed, which is how the finding gets closed.
"""

import atexit
import itertools
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile


SPEC_PATH = sys.argv[1]
# Absolute, because every run happens in a scratch directory of its own.
BINARY = os.path.abspath(sys.argv[2])
TOOL = os.path.basename(BINARY)

with open(SPEC_PATH) as handle:
    SPEC = json.load(handle)

if TOOL not in SPEC["tools"]:
    sys.exit("no CLI contract for %s in %s" % (TOOL, os.path.basename(SPEC_PATH)))

CONTRACT = SPEC["tools"][TOOL]

# A tool whose command line cannot be reached here (see "why_not" in the
# spec). Its man page and the spec's own ids are still checked: those need no
# run, and leaving them out would let a documentation finding go unheld for
# the one tool nothing else looks at.
TESTABLE = CONTRACT.get("testable", True)

# Case ids for the two checks that are about the spec and the documentation
# rather than about a command line, so a finding can claim them by id.
SPEC_CASE = TOOL + " (the option table)"
MAN_PAGE_CASE = TOOL + " (the man page)"
SPEC_IDS_CASE = "cli_spec.json (the ids)"
# The questions whose cases the generator makes rather than the spec listing
# them: a lone "--" and a doubled option are the same two command lines for
# every tool, so they are not written out per tool.
GENERATED_QUESTIONS = {"Q-6", "Q-7"}

# The role an option without one gets, and the roles this test acts on by
# name: a help or report option ends the command, a selector selects a board,
# and anything else only modifies the run. Every other role in the spec is a
# label the pair rules match on.
DEFAULT_ROLE = "modifier"
ACTED_ON_ROLES = {"help", "report", "selector", DEFAULT_ROLE}

ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")
# A Python tool runs under the interpreter CMake found, as every other Python
# test here does, rather than under whatever its shebang resolves to.
COMMAND = ([sys.executable, BINARY] if CONTRACT.get("kind") == "python"
           else [BINARY])

# A tool that finds boards through the Python usb1 module rather than through
# libubertooth, which is what UBERTOOTH_TEST_NO_DEVICES reaches. It gets a
# usb1 that finds nothing, so an accepted command line stops where the other
# tools stop, and no board is opened whatever is attached. The tool's own
# modules go on the path with it, because the wrapper under test is the one
# CMake configured into the build tree rather than an installed one.
USB1_STUB = '''"""A usb1 that finds no device: the matrix opens no board."""


class USBError(Exception):
    pass


class USBErrorTimeout(USBError):
    pass


class USBContext:
    def openByVendorIDAndProductID(self, vendor, product,
                                   skip_on_error=False):
        return None
'''

if CONTRACT.get("devices_via") == "usb1":
    STUB_DIRECTORY = tempfile.mkdtemp(prefix="cli-matrix-usb1-")
    # However the run ends, including the exits just below.
    atexit.register(shutil.rmtree, STUB_DIRECTORY, ignore_errors=True)
    with open(os.path.join(STUB_DIRECTORY, "usb1.py"), "w") as handle:
        handle.write(USB1_STUB)
    path = [STUB_DIRECTORY]
    module_dir = CONTRACT.get("module_dir")
    if module_dir is not None:
        path.append(os.path.normpath(os.path.join(
            os.path.dirname(os.path.abspath(SPEC_PATH)), module_dir)))
    ENVIRONMENT["PYTHONPATH"] = os.pathsep.join(path)
    # The tool puts its own module directory ahead of PYTHONPATH before it
    # imports usb1, so the check has to do the same thing: the question is
    # which usb1 wins where the tool itself looks, not where a plain
    # interpreter looks. Without this a machine with the real usb1 installed
    # could open a board.
    found = subprocess.run(
        [sys.executable, "-c",
         "import sys; sys.path.insert(0, %r); import usb1; "
         "print(usb1.__file__)" % (module_dir or "")],
        env=ENVIRONMENT, stdout=subprocess.PIPE, universal_newlines=True)
    if os.path.dirname(found.stdout.strip()) != STUB_DIRECTORY:
        sys.exit("%s would import %s and could open a board"
                 % (TOOL, found.stdout.strip() or "the real usb1"))
DEVICE_LOOKUP = tuple(message.lower() for message in SPEC["_device_lookup_messages"])
# An open finding, as the cases it breaks and the complaints it produces.
# Every case listed has to keep failing with all of them: a finding half
# fixed, or fixed for some of its cases, fails this test rather than passing
# quietly.
ENTRY_SHAPE = {"tool": str, "findings": list, "cases": list,
               "problem_contains": list}
for entry in SPEC["expected_failures"]:
    # The keys the test reads, checked once, so a typo in one of them is
    # named here rather than raising a KeyError somewhere further down. The
    # whole file is checked, not just this tool's entries, because an entry
    # for a tool that is not built here is the one nobody would look at.
    wrong = [key for key, kind in ENTRY_SHAPE.items()
             if not isinstance(entry.get(key), kind) or not entry[key]]
    if wrong:
        sys.exit("an expected_failures entry has no usable %s: %r"
                 % (", ".join(sorted(wrong)), entry))
    # A blank reason is inside every complaint, so it would hold the entry to
    # nothing. (A string instead of a list was caught above, where it would
    # otherwise have been read a character at a time.)
    reasons = entry["problem_contains"]
    if not all(isinstance(reason, str) and reason.strip()
               for reason in reasons):
        sys.exit("%s %s: problem_contains has to be a list of the complaints "
                 "the cases produce, one per complaint, none of them blank, "
                 "not %r"
                 % (", ".join(entry["findings"]), entry["cases"][0], reasons))
EXPECTED_FAILURES = [entry for entry in SPEC["expected_failures"]
                     if entry["tool"] == TOOL]

# A word no tool takes as an operand, and a serial no board has.
STRAY = "extra"
NO_SUCH_SERIAL = "ffffffffffffffffffffffffffffffff"

# Value placeholders in the spec stand for a path inside the run directory, so
# a case id does not carry a temporary path and is the same on every machine.
PLACEHOLDERS = ("CAPTURE.pcap", "CAPTURE2.pcap", "CAPTURE.pcapng", "DUMP.bin",
                "SAMPLES.bin", "REPLAY.bin", "MISSING.bin", "IMAGE.dfu",
                "OUT.bin", "PLAIN.bin")

# The placeholders the test creates before the run, because the case is about
# what the tool makes of a file it reads. MISSING.bin is deliberately absent.
INPUT_PLACEHOLDERS = ("IMAGE.dfu", "PLAIN.bin", "REPLAY.bin")


class Case(object):
    """One command line, the verdict it must produce, and why."""

    def __init__(self, arguments, verdict, error=None, contains=None,
                 names=None, source=None, creates=()):
        self.arguments = list(arguments)
        self.verdict = verdict
        # Files the run has to leave behind, for a command that reports
        # success by saying nothing.
        self.creates = list(creates)
        # The whole "error: " line, when the spec gives it; otherwise a
        # fragment of it, which is what a message that quotes the user's own
        # words can be pinned to.
        self.error = error
        self.contains = contains
        # An option the error line has to name, as the user spelled it.
        self.names = names
        self.source = source

    @property
    def identifier(self):
        return " ".join([TOOL] + self.arguments)


class Timeout(object):
    """Stands in for a result when the tool did not come back."""

    returncode = -1
    stdout = ""
    stderr = "the tool did not come back within %d seconds" % 20


class NotRun(object):
    """Stands in for a result where there was nothing to run.

    The option-table and man-page checks produce complaints without running
    the tool, and the spec can claim a case the matrix never generated.
    """

    returncode = None
    stdout = ""
    stderr = ""


# How the pages spell out a whole number of seconds. The prose drifts the
# same way the digits do, and a reader cannot tell which of the two is
# current, so {words:NAME} is checked as well as {define:NAME}. There is
# deliberately no rule for turning a number into English here: a new timeout
# has no entry, the check fails, and whoever changed it writes the word form
# in alongside.
SECOND_WORDS = {
    1: "one second",
    2: "two seconds",
    3: "three seconds",
    4: "four seconds",
    5: "five seconds",
}


def resolve_defines(phrase, tests_dir):
    """Fill {define:NAME} and {words:NAME} from the firmware interface header.

    A man page that quotes a number out of that header is only right until
    someone changes the header, and nothing otherwise connects the two. A
    spec phrase written with a placeholder is checked against the value
    the build actually uses. {define:NAME} gives the digits, {words:NAME}
    gives the same value in seconds spelled out.
    """
    header = os.path.join(tests_dir, "..", "..", "libubertooth", "src",
                          "ubertooth_interface.h")
    with open(header) as handle:
        text = handle.read()

    def digits_of(name):
        found = re.search(r"^#define\s+%s\s+(\S+)" % re.escape(name), text,
                          re.M)
        if found is None:
            raise AssertionError("%s does not define %s" % (header, name))
        return found.group(1)

    def value_of(match):
        return digits_of(match.group(1))

    def words_of(match):
        name = match.group(1)
        milliseconds = int(digits_of(name), 0)
        seconds, remainder = divmod(milliseconds, 1000)
        if remainder or seconds not in SECOND_WORDS:
            raise AssertionError(
                "%s is %d ms, which SECOND_WORDS in this file has no English "
                "for. Add it, and say the new figure in the pages that spell "
                "the timeout out." % (name, milliseconds))
        return SECOND_WORDS[seconds]

    phrase = re.sub(r"\{define:(\w+)\}", value_of, phrase)
    return re.sub(r"\{words:(\w+)\}", words_of, phrase)


def run(arguments, directory):
    """Run the tool in an empty directory, so a created file is visible."""
    resolved = [os.path.join(directory, argument)
                if argument in PLACEHOLDERS else argument
                for argument in arguments]
    try:
        result = subprocess.run(COMMAND + resolved, env=ENVIRONMENT,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                cwd=directory, universal_newlines=True,
                                errors="replace", timeout=20)
    except subprocess.TimeoutExpired:
        # No case here should wait on anything, so a run that does is a
        # failure to report rather than a crash in the middle of the suite.
        result = Timeout()
    created = sorted(os.listdir(directory))
    return result, created


USAGE_MARKER = CONTRACT.get("usage_marker", "-U <index|serial>")


def usage_text(text):
    """The usage text names the tool and carries the line only it carries."""
    return TOOL in text and USAGE_MARKER in text


def reached_device_lookup(output):
    """Did the run get past argument checking?

    Normally that shows as the device-lookup message. ubertooth-follow and
    ubertooth-scan look for the local Bluetooth adapter first, so on a machine
    without one they stop there instead, which is just as good a sign that the
    command line was accepted: the spec names those messages per tool.

    Both streams are searched, because this asks only whether argument
    checking passed, not which stream the message used. The stream rules are
    checked separately, so looking at both here lets nothing through.
    """
    lowered = output.lower()
    markers = DEVICE_LOOKUP + tuple(
        marker.lower() for marker in CONTRACT.get("accepted_markers", []))
    return any(message in lowered for message in markers)


def error_lines(stderr):
    return [line for line in stderr.splitlines() if line.startswith("error: ")]


def file_option_values(arguments):
    """The values the file options on this command line were given.

    A file option is one the spec gives a "stdout_value" for, whether or not
    "-" means standard output for it: a tool that does not take "-" that way
    has to refuse it, not write a file called "-".
    """
    options = CONTRACT.get("options", {})
    values = [value for name, value in zip(arguments, arguments[1:])
              if "stdout_value" in options.get(name, {})]
    # The attached spelling, where the value is glued to the option.
    for word in arguments:
        for name, option in options.items():
            if "stdout_value" in option and word.startswith(name) \
                    and len(word) > len(name):
                values.append(word[len(name):])
    return values


def stdout_allowed_on_failure(arguments):
    """The options whose own output is on standard output before a failure.

    ubertooth-util -N prints the board count and then runs the rest of the
    command; ubertooth-btle -q - and -c - put the capture itself there. Both
    are what the user asked for, so they are not a diagnostic leaking onto
    standard output.
    """
    written = " ".join(arguments)
    for allowed in CONTRACT.get("stdout_on_failure", []):
        if allowed in written:
            return True
    return False


# ---- the checks -------------------------------------------------------------

def check(case, result, created):
    """Return a list of complaints; empty means the case passed."""
    problems = []
    stdout, stderr = result.stdout, result.stderr
    status = result.returncode

    def complain(message):
        problems.append(message)

    # ---- universal ---------------------------------------------------------
    allowed_statuses = {0, 1} | {int(code) for code
                                 in CONTRACT.get("other_exit_codes", {})}
    if status not in allowed_statuses:
        complain("exit status %d is not one of %s"
                 % (status, sorted(allowed_statuses)))
    # Whether this rule already reported the write, so the verdicts below do
    # not report the same one again: one write, one complaint, so a finding
    # does not have to claim the same fault twice.
    said_standard_output = False
    if status != 0 and stdout and not stdout_allowed_on_failure(case.arguments):
        complain("standard output is not empty on a failure: %r" % stdout[:120])
        said_standard_output = True
    # The usage text belongs on standard output only when the user asked for
    # it. That is the "help" verdict, and also a line that carries -h without
    # the verdict being decided -- -h beside -V, where which one wins is an
    # open question.
    asked_for_it = case.verdict == "help" or any(name in case.arguments
                                                 for name in HELP_OPTIONS)
    if usage_text(stdout) and not asked_for_it:
        complain("the usage text is on standard output for a command line "
                 "that did not ask for it")
    # ubertooth(7), OPTION VALUES: a lone "-" as a file value is standard
    # output, so a run given one must not write a file called "-" instead.
    if "-" in file_option_values(case.arguments) and "-" in created:
        complain("wrote a file called '-' instead of writing to standard "
                 "output")

    # ---- per verdict -------------------------------------------------------
    if case.verdict == "help":
        if status != 0:
            complain("-h exited %d, not 0" % status)
        if not usage_text(stdout):
            complain("-h did not print the usage text on standard output")
        if stderr:
            complain("-h wrote to standard error: %r" % stderr[:160])

    elif case.verdict == "report":
        if status != 0:
            complain("exited %d, not 0" % status)
        if not stdout.strip():
            complain("printed nothing on standard output")
        if stderr:
            complain("wrote to standard error: %r" % stderr[:160])

    elif case.verdict == "usage":
        if status != 1:
            complain("exited %d, not 1" % status)
        # No exemption here, whatever the tool's stdout_on_failure says: a
        # command line the tool would not take writes nothing of its own.
        if stdout and not said_standard_output:
            complain("wrote to standard output: %r" % stdout[:120])
        first, _, rest = stderr.partition("\n")
        if case.error is not None and first != case.error:
            complain("first line is %r, not %r" % (first, case.error))
        if not usage_text(rest):
            complain("a bare command line did not print the usage text on "
                     "standard error")
        if reached_device_lookup(stdout + stderr):
            complain("reached device lookup")

    elif case.verdict == "refused":
        if status != 1:
            complain("exited %d, not 1" % status)
        # As for "usage": no exemption, and nothing said twice.
        if stdout and not said_standard_output:
            complain("wrote to standard output: %r" % stdout[:120])
        lines = error_lines(stderr)
        # A refusal is that one line and nothing else. Counting only the
        # prefixed lines would let a tool add the right line in front of the
        # unprefixed ones it already printed and read as fixed.
        # A warning or a note beside the error line is allowed: cli-output.md
        # puts all three on standard error. So is a progress line the spec
        # names in "progress_before_refusal", for a check that reports what it
        # is doing before it can know the answer. Anything else is a second
        # line saying what went wrong, which is what the one-line rule is
        # about.
        progress = CONTRACT.get("progress_before_refusal", [])
        said = [line for line in stderr.splitlines()
                if line.strip()
                and not line.startswith(("warning: ", "note: "))
                and line not in progress]
        if len(lines) != 1:
            complain("expected one 'error: ' line, got %d: %r"
                     % (len(lines), stderr[:240]))
        else:
            if said != lines:
                complain("said more than the one 'error: ' line: %r"
                         % [line for line in said if line not in lines][:4])
            if case.error is not None and lines[0] != case.error:
                complain("said %r, not %r" % (lines[0], case.error))
            if case.contains is not None and case.contains not in lines[0]:
                complain("said %r, which does not contain %r"
                         % (lines[0], case.contains))
            if case.names is not None and case.names not in lines[0]:
                complain("said %r, which does not name %s"
                         % (lines[0], case.names))
        if usage_text(stderr):
            complain("the usage text follows an error line that describes the "
                     "problem")
        if reached_device_lookup(stdout + stderr):
            complain("reached device lookup for a command line it refused")
        if created:
            complain("a refused command line created %s" % created)

    elif case.verdict == "silent_success":
        if status != 0:
            complain("exited %d, not 0" % status)
        if stdout:
            complain("wrote to standard output: %r" % stdout[:120])
        if stderr:
            complain("wrote to standard error: %r" % stderr[:160])
        # A command that says nothing has to have done the work, or the
        # verdict would pass for a tool that quietly did none of it.
        for expected_file in case.creates:
            if expected_file not in created:
                complain("did not write %s; the run directory holds %s"
                         % (expected_file, created))

    elif case.verdict == "device":
        if status != 1:
            complain("exited %d, not 1" % status)
        if not reached_device_lookup(stdout + stderr):
            complain("did not get past argument checking: %r" % stderr[:240])
        if usage_text(stderr):
            complain("printed the usage text")
        # A command line the tool accepted still ends in a failure here, and
        # that failure is the tool's own line. Where the spec names it, it has
        # to be among the "error: " lines, not an unprefixed line beside them:
        # that is how a tool whose device-side failures do not carry the
        # prefix is caught, which the refusal cases cannot reach.
        if case.contains is not None and \
                not any(case.contains in line for line in error_lines(stderr)):
            complain("no 'error: ' line contains %r: %r"
                     % (case.contains, stderr[:240]))

    elif case.verdict == "count_only":
        if status != 0:
            complain("exited %d, not 0" % status)
        if not stdout.strip().isdigit():
            complain("standard output is %r, not a count" % stdout[:120])
        if "could not open" in stderr.lower():
            complain("opened a device for a count that needs none")

    elif case.verdict == "count_then_device":
        # The count came out, and then the rest of the command failed to find
        # a board, so the status is the failure's: a run that answers part of
        # what was asked and then cannot do the rest did not do what was
        # asked.
        if status != 1:
            complain("exited %d, not 1" % status)
        if not stdout.split("\n")[0].strip().isdigit():
            complain("the count is not the first line of standard output: %r"
                     % stdout[:120])
        if not reached_device_lookup(stdout + stderr):
            complain("did not reach device lookup: %r" % stderr[:240])

    elif case.verdict == "undecided":
        # The man page does not decide this one, so only the universal rules
        # apply: whatever the tool does, it either refuses cleanly or goes on
        # to look for a device.
        if not reached_device_lookup(stdout + stderr):
            lines = error_lines(stderr)
            if status == 0:
                # Exit 0 without looking for a device means the command
                # answered from the host (-h, -V, a count, a file it wrote).
                # A silent exit 0 is a command that did nothing, which
                # cli-output.md gives status 1.
                if not stdout and not created:
                    complain("exited 0 having neither answered nor looked for "
                             "a device")
            elif len(lines) != 1:
                complain("neither reached device lookup nor refused with one "
                         "'error: ' line: %r" % stderr[:240])
            elif usage_text(stderr):
                complain("the usage text follows an error line")

    else:
        complain("unknown verdict %r in the spec" % case.verdict)

    return problems


# ---- generating the cases ---------------------------------------------------

OPTIONS = CONTRACT.get("options", {})


def check_the_spec_lists_every_option():
    """The spec's option table has to match the tool's own getopt string.

    Without this the matrix would quietly skip an option somebody added to a
    tool and forgot to write down here, and claim full coverage anyway. The
    getopt string is in the spec as well as in the source, so the two are
    compared and the source is read to confirm the spec copied it right.
    """
    here = os.path.dirname(os.path.abspath(SPEC_PATH))
    problems = []

    # The ids the spec uses have to exist, or an entry naming a tool, a
    # finding or a question by a name nothing else has is dropped in silence:
    # the cases it claims go unchecked and nothing says so. Every run checks
    # the whole file, because an entry for a tool that is not built here is
    # exactly the one that would never be looked at.
    findings = set(SPEC["_findings"]) - {"_about"}
    questions = set(SPEC["_questions"]) - {"_about"}
    for entry in SPEC["expected_failures"]:
        where = "%s %s" % (", ".join(entry["findings"]), entry["cases"][0])
        if entry["tool"] not in SPEC["tools"]:
            problems.append((SPEC_IDS_CASE,
                             "%s names the tool %s, which the spec does not "
                             "have" % (where, entry["tool"])))
        unknown = sorted(set(entry["findings"]) - findings)
        if unknown:
            problems.append((SPEC_IDS_CASE,
                             "%s names %s, which _findings does not have"
                             % (where, " ".join(unknown))))
    # A question id can be on a case, a stated combination or a pair rule,
    # per tool or across all of them, so the whole file is walked for them.
    asked_by_the_spec = {}
    def collect(node, where):
        if isinstance(node, dict):
            if "question" in node:
                asked_by_the_spec.setdefault(node["question"], where)
            for key, value in node.items():
                collect(value, "%s %s" % (where, key))
        elif isinstance(node, list):
            for value in node:
                collect(value, where)
    collect(SPEC, "cli_spec.json")

    for asked, where in sorted(asked_by_the_spec.items()):
        if asked not in questions:
            problems.append((SPEC_IDS_CASE,
                             "%s names %s, which _questions does not have"
                             % (where, asked)))
    # And the other way round: a question with no case behind it is one the
    # maintainer is asked about and the matrix never runs.
    for asked in sorted(questions - set(asked_by_the_spec)
                        - GENERATED_QUESTIONS):
        problems.append((SPEC_IDS_CASE,
                         "%s is in _questions, but no case names it" % asked))

    # The roles, the same way: a role is what this test and the pair rules
    # select options by, and a name nothing declares selects nothing. Spelling
    # "selector" any other way does not fail anywhere on its own -- the option
    # is quietly treated as a plain modifier instead -- so the names are
    # checked against _roles, and _roles against the options.
    declared = set(SPEC["_roles"]) - {"_about"}
    carried = set()
    for tool, contract in sorted(SPEC["tools"].items()):
        for name, option in sorted(contract.get("options", {}).items()):
            role = option.get("role", DEFAULT_ROLE)
            carried.add(role)
            if role not in declared:
                problems.append((SPEC_IDS_CASE,
                                 "%s %s has the role %r, which _roles does "
                                 "not have" % (tool, name, role)))
    for role in sorted(ACTED_ON_ROLES - declared):
        problems.append((SPEC_IDS_CASE,
                         "_roles does not have %r, which this test acts on"
                         % role))
    for role in sorted(declared - carried):
        problems.append((SPEC_IDS_CASE,
                         "_roles has %r, which no option carries" % role))
    # A pair rule names each side either by role or by the option itself, so
    # a side naming a role or an option that none of the options the rule
    # applies to has is a rule that can never match. A global rule applies to
    # every tool, a tool's own rule only to its own options, so each side is
    # checked against what it could actually select.
    every_option = {name for contract in SPEC["tools"].values()
                    for name in contract.get("options", {})}
    rules = [("_global_pair_rules", rule, carried, every_option)
             for rule in SPEC.get("_global_pair_rules", [])]
    for tool, contract in sorted(SPEC["tools"].items()):
        its_roles = {option.get("role", DEFAULT_ROLE)
                     for option in contract.get("options", {}).values()}
        rules += [(tool, rule, its_roles, set(contract.get("options", {})))
                  for rule in contract.get("pair_rules", [])]
    for where, rule, roles, options in rules:
        for side in (rule["a"], rule["b"]):
            if side == "any":
                continue
            if side.startswith("role:"):
                known = side[len("role:"):] in roles
            else:
                known = side in options
            if not known:
                problems.append((SPEC_IDS_CASE,
                                 "the %s rule %s beside %s names %s, which "
                                 "no option it applies to has"
                                 % (where, rule["a"], rule["b"], side)))

    # The man page the spec cites has to be there, and it has to list -h:
    # ten of them do not, which is finding F-09. It gets a case id of its own
    # so that finding can claim it like any other.
    page = CONTRACT.get("man")
    if page is not None and page.endswith(".md"):
        path = os.path.join(here, "..", "..", "doc", page)
        if not os.path.exists(path):
            problems.append((MAN_PAGE_CASE,
                             "%s cites %s, which is not there" % (TOOL, page)))
        else:
            with open(path) as handle:
                text = handle.read()
            # Only the OPTIONS section counts: a page that mentions -h under
            # DIAGNOSTICS still does not list it where a reader looks for it.
            section = re.search(r"\n## OPTIONS\n(.*?)(?=\n## |\Z)", text,
                                re.S)
            if section is None:
                problems.append((MAN_PAGE_CASE,
                                 "%s has no OPTIONS section" % page))
            elif " - `-h`" not in section.group(1):
                problems.append((MAN_PAGE_CASE,
                                 "%s does not list -h in its OPTIONS "
                                 "section" % page))
            # A page that promises behaviour the rules forbid, so that fixing
            # the sentence is what closes the finding. The spec names the
            # phrase, because only the page knows how it is worded. Both
            # sides have their whitespace collapsed, so reflowing the
            # paragraph does not read as the claim having been taken out.
            flowed = " ".join(text.split())
            for phrase, complaint in CONTRACT.get("man_must_not_say", []):
                if " ".join(phrase.split()) in flowed:
                    problems.append((MAN_PAGE_CASE,
                                     "%s says %r: %s" % (page, phrase,
                                                         complaint)))
            # A number the page quotes from the firmware interface, written
            # as a phrase with {define:NAME} where the value goes, or
            # {words:NAME} where the page spells it out in English. The page
            # and the header drift apart silently otherwise: a reader has no
            # way to tell that the timeout the page promises is last
            # release's.
            #
            # An entry is either a phrase, checked against this tool's own
            # page, or a [page, phrase] pair naming another page that quotes
            # the same value. ubertooth(7) describes the watchdog to users and
            # drifts just as easily, and no tool owns that page.
            for entry in CONTRACT.get("man_must_say", []):
                if isinstance(entry, str):
                    target, target_text = page, flowed
                else:
                    target, phrase_source = entry[0], entry[1]
                    other = os.path.join(here, "..", "..", "doc", target)
                    if not os.path.exists(other):
                        problems.append((MAN_PAGE_CASE,
                                         "the spec cites %s, which is not "
                                         "there" % target))
                        continue
                    with open(other) as handle:
                        target_text = " ".join(handle.read().split())
                    entry = phrase_source
                wanted = resolve_defines(entry, here)
                if " ".join(wanted.split()) not in target_text:
                    problems.append((MAN_PAGE_CASE,
                                     "%s does not say %r, which is what the "
                                     "header it quotes now says"
                                     % (target, wanted)))

    written = CONTRACT.get("optstring")
    if written is None:
        return problems

    # The source to read the real option string out of: a C tool's own file,
    # or the script itself for a Python one. Without this the spec's copy
    # would only be checked against the spec, which is what it is meant to
    # keep honest.
    source = os.path.join(here, "..", "src", TOOL + ".c")
    pattern = r'getopt\(argc,\s*argv,\s*"([^"]+)"\)'
    if not os.path.exists(source) and CONTRACT.get("kind") == "python":
        source = BINARY
        pattern = r'getopt\(sys\.argv\[1:\],\s*"([^"]+)"\)'
    # A source that is not there is a failure, not a check to skip: the spec's
    # copy of the option string would then be compared against nothing, and a
    # file somebody renames or moves would drop the one check that keeps the
    # two honest, silently and for as long as nobody noticed.
    if not os.path.exists(source):
        problems.append((SPEC_CASE,
                         "%s is not there, so the option string in the "
                         "spec is checked against nothing"
                         % os.path.relpath(source, here)))
    else:
        with open(source) as handle:
            text = handle.read()
        found = re.search(pattern, text)
        if found is None:
            problems.append((SPEC_CASE,
                         "could not find the getopt string in %s"
                            % os.path.relpath(source, here)))
        elif found.group(1) != written:
            problems.append((SPEC_CASE,
                         "the spec says the option string is %r, but %s "
                            "passes %r to getopt"
                            % (written, os.path.relpath(source, here),
                               found.group(1))))

    # The option string, read the way getopt reads it: a letter, then one
    # colon for a required value or two for an optional one. A leading colon
    # is not an option.
    from_optstring = {}
    letters = written[1:] if written.startswith(":") else written
    index = 0
    while index < len(letters):
        name = "-" + letters[index]
        index += 1
        colons = 0
        while index < len(letters) and letters[index] == ":":
            colons += 1
            index += 1
        from_optstring[name] = ("none", "required", "optional")[colons]

    missing = sorted(set(from_optstring) - set(OPTIONS))
    if missing:
        problems.append((SPEC_CASE,
                         "the option string has %s, which the spec does not "
                          "list" % " ".join(missing)))
    extra = sorted(set(OPTIONS) - set(from_optstring))
    if extra:
        problems.append((SPEC_CASE,
                         "the spec lists %s, which the option string does not "
                          "have" % " ".join(extra)))
    for name in sorted(set(from_optstring) & set(OPTIONS)):
        if from_optstring[name] != OPTIONS[name]["value"]:
            problems.append((SPEC_CASE,
                         "the spec says %s takes a %s value, the option "
                            "string says %s"
                            % (name, OPTIONS[name]["value"],
                               from_optstring[name])))
    return problems

ROLES = {name: option.get("role", DEFAULT_ROLE)
         for name, option in OPTIONS.items()}
HELP_OPTIONS = [name for name, role in ROLES.items() if role == "help"]
REPORT_OPTIONS = [name for name, role in ROLES.items() if role == "report"]
WORK_OPTIONS = CONTRACT.get("work_options")
REQUIRES_ALL = CONTRACT.get("requires_all")


def valid_value(name):
    option = OPTIONS[name]
    if option["value"] == "none":
        return None
    values = option.get("valid")
    return values[0] if values else None


def with_value(name):
    """The option as a command line: the option, plus a value when it needs one."""
    value = valid_value(name)
    if OPTIONS[name]["value"] == "required":
        if value is None:
            raise AssertionError("%s %s needs a valid value in the spec"
                                 % (TOOL, name))
        return [name, value]
    return [name]


def source_of(entry):
    """Where a spec entry's verdict comes from, for a failure message."""
    source = entry.get("source") or entry.get("why")
    if entry.get("question"):
        return "%s: %s" % (entry["question"], source) if source \
            else entry["question"]
    return source


def explicit(arguments):
    """A verdict the spec states outright for this exact command line."""
    for entry in CONTRACT.get("combinations", []):
        if entry["args"] == list(arguments):
            return entry
    return None


def verdict_for_line(arguments):
    """The verdict for a command line, from the tool's own shape.

    Three shapes cover every tool here: one that ends the command on its own
    (-h, -V, -H), one that needs a set of options before anything can run
    (requires_all), and one that needs at least one option that is work of its
    own (work_options). Anything else the tool accepts, so the run goes on to
    look for a device.
    """
    given = [word for word in arguments if word.startswith("-")
             and word in OPTIONS]
    for name in HELP_OPTIONS:
        if name in given:
            return Case(arguments, "help")
    for name in REPORT_OPTIONS:
        if name in given:
            return Case(arguments, "report")

    if REQUIRES_ALL is not None and not set(REQUIRES_ALL).issubset(given):
        return Case(arguments, "refused", error=CONTRACT["requires_all_error"],
                    source="%s requires %s" % (TOOL, " and ".join(REQUIRES_ALL)))

    # An option that needs a companion option, refused before any device is
    # opened when it is not there (ubertooth-rx -u without -l, for example).
    for name in given:
        needs = OPTIONS[name].get("requires")
        if needs and not set(needs).issubset(given):
            return Case(arguments, "refused",
                        error=OPTIONS[name].get("requires_error"),
                        contains=OPTIONS[name].get("requires_error_contains"),
                        source="%s %s needs %s" % (TOOL, name, " and ".join(needs)))
    if WORK_OPTIONS is not None and not set(WORK_OPTIONS) & set(given):
        selector_only = CONTRACT.get("selector_only_error")
        selectors = [name for name in given if ROLES.get(name) == "selector"]
        if selector_only is not None and selectors:
            if "%s" in selector_only:
                # The message quotes the selector back, so it depends on the
                # value this case used.
                position = arguments.index(selectors[0])
                selector_only = selector_only % arguments[position + 1]
            return Case(arguments, "refused", error=selector_only,
                        source="%s selector with nothing to do" % TOOL)
        return Case(arguments, "refused", error=CONTRACT["nothing_to_do"],
                    source="%s has nothing to do" % TOOL)
    return Case(arguments, "device")


def matches(rule_side, name):
    if rule_side == "any":
        return True
    if rule_side.startswith("role:"):
        return ROLES.get(name) == rule_side[len("role:"):]
    return rule_side == name


def pair_verdict(first, second, arguments):
    """The verdict for a pair, from the spec's rules, in the order written.

    The rules that hold for every tool come first, then the tool's own, then
    the tool's shape (see verdict_for_line).
    """
    # The rules that hold for every tool come first: an option that ends the
    # command outranks any combination rule, since the rest of the line never
    # runs.
    rules = SPEC.get("_global_pair_rules", []) + CONTRACT.get("pair_rules", [])
    for rule in rules:
        for a, b in ((first, second), (second, first)):
            if matches(rule["a"], a) and matches(rule["b"], b):
                other = b if matches(rule["a"], a) and rule["a"] != "any" else a
                source = rule.get("source") or rule.get("why")
                if rule.get("question"):
                    source = "%s: %s" % (rule["question"], source)
                return Case(arguments, rule["verdict"],
                            error=rule.get("error"),
                            contains=rule.get("error_contains"),
                            names=other if rule.get("names_the_other") else None,
                            source=source)
    return verdict_for_line(arguments)


def listed_cases():
    """The cases the spec writes out, for a tool with no short-option table.

    btle-extcap.py is the one: its command line is the extcap protocol's, all
    long options, with no man page to generate a table from.
    """
    cases = [Case([], "usage", error=CONTRACT["nothing_to_do"])]
    for entry in CONTRACT.get("cases", []):
        cases.append(Case(entry["args"], entry["verdict"],
                          error=entry.get("error"),
                          contains=entry.get("error_contains"),
                          source=source_of(entry),
                          creates=entry.get("creates", ())))
    return cases


def generated_cases():
    cases = []

    # A bare command line. A tool that needs arguments answers with its one
    # "error: " line and the usage text; one with work to do runs.
    if REQUIRES_ALL is not None:
        cases.append(Case([], "usage", error=CONTRACT["requires_all_error"]))
    elif WORK_OPTIONS is not None:
        cases.append(Case([], "usage", error=CONTRACT["nothing_to_do"]))
    else:
        cases.append(Case([], "device"))

    # -h, and -h alongside a command line the tool will not take: -h ends the
    # command only once the whole line has parsed. A tool without a -h at all
    # still gets the case: no tool is an exception to it, so the absence is a
    # finding rather than a reason to generate nothing.
    if not HELP_OPTIONS:
        cases.append(Case(["-h"], "help"))
    if HELP_OPTIONS:
        help_option = HELP_OPTIONS[0]
        cases.append(Case([help_option], "help"))
        cases.append(Case([help_option, STRAY], "refused",
                          error="error: unexpected argument '%s'" % STRAY))
        cases.append(Case([help_option, "-Z"], "refused",
                          error="error: unrecognized option '-Z'"))

    # A word that is not an option, including a bare "-", and "--".
    cases.append(Case([STRAY], "refused",
                      error="error: unexpected argument '%s'" % STRAY))
    cases.append(Case(["-"], "refused", error="error: unexpected argument '-'"))
    # "--" ends the options and leaves nothing behind it. A tool with work to
    # do runs; for one that needs arguments, whether "--" counts as a bare
    # command line (and gets the usage text) or as a line that said something
    # (and gets the error line alone) is Q-7, so it is left undecided rather
    # than pinned to what the tools happen to do.
    if verdict_for_line([]).verdict == "device":
        cases.append(Case(["--"], "device"))
    else:
        cases.append(Case(["--"], "undecided", source="Q-7"))

    # An option the tool does not know, written short and long, and every
    # option it used to have. The rule is the same for every tool; the tools
    # that have not been converted to report it the shared way are findings,
    # not exceptions, so the cases are generated for all of them.
    cases.append(Case(["-Z"], "refused",
                      error="error: unrecognized option '-Z'"))
    cases.append(Case(["--bogus"], "refused",
                      contains="error: unrecognized option '--bogus'"))
    for removed in CONTRACT.get("removed_options", []):
        cases.append(Case([removed], "refused",
                          error="error: unrecognized option '%s'" % removed))

    for name, option in sorted(OPTIONS.items()):
        arguments = with_value(name)

        # The option on its own.
        case = explicit(arguments)
        if case is not None:
            cases.append(Case(arguments, case["verdict"], error=case.get("error"),
                              contains=case.get("error_contains"),
                              source=source_of(case),
                              creates=case.get("creates", ())))
        else:
            cases.append(verdict_for_line(arguments))

        # An option whose value is missing.
        if option["value"] == "required":
            cases.append(Case([name], "refused",
                              error="error: option '%s' requires a value" % name))

        # A value out of range or malformed, as a separate word and attached.
        for bad, message in option.get("invalid", []):
            cases.append(Case([name, bad], "refused", contains=message))
            cases.append(Case([name + bad], "refused", contains=message))

        # The attached form of a value the tool takes: the same command line
        # written the other way must do the same thing. A value that starts
        # with "-" is included, because attached is the only way to write one
        # (ubertooth(7) uses "-z -90" as its example of the separate form and
        # the squelch is where it matters). A placeholder is left out: it
        # stands for a path the run directory decides, which cannot be glued
        # to the option in a stable case id.
        value = valid_value(name)
        if value is not None and value not in PLACEHOLDERS:
            attached = [name + value]
            reference = explicit(arguments)
            if reference is not None:
                cases.append(Case(attached, reference["verdict"],
                                  error=reference.get("error"),
                                  contains=reference.get("error_contains"),
                                  source=source_of(reference),
                                  creates=reference.get("creates", ())))
            else:
                separate = verdict_for_line(arguments)
                cases.append(Case(attached, separate.verdict,
                                  error=separate.error,
                                  contains=separate.contains,
                                  names=separate.names,
                                  source=separate.source,
                                  creates=separate.creates))

        # An optional value written as the next word is taken as the value
        # when it is a valid one, which a trailing stray word proves by being
        # the word the tool complains about.
        if option["value"] == "optional" and value is not None:
            cases.append(Case([name, value, STRAY], "refused",
                              error="error: unexpected argument '%s'" % STRAY))

        # A file option given "-". ubertooth(7) says a lone "-" is standard
        # output; a tool that writes a file called "-" instead is a finding,
        # not a different rule. The verdict comes from the spec where it says
        # so outright (a tool that needs work beside the capture names the
        # whole command line there), otherwise from the tool's shape.
        if option.get("stdout_value") is not None:
            dash = explicit([name, "-"])
            for written in ([name, "-"], [name + "-"]):
                if dash is not None:
                    cases.append(Case(written, dash["verdict"],
                                      error=dash.get("error"),
                                      contains=dash.get("error_contains"),
                                      source=source_of(dash),
                                      creates=dash.get("creates", ())))
                else:
                    shape = verdict_for_line([name, "-"])
                    cases.append(Case(written, shape.verdict,
                                      error=shape.error,
                                      contains=shape.contains,
                                      names=shape.names,
                                      source=shape.source,
                                      creates=shape.creates))

    # The command lines the spec states outright, including the ones with more
    # than two options.
    for entry in CONTRACT.get("combinations", []):
        cases.append(Case(entry["args"], entry["verdict"],
                          error=entry.get("error"),
                          contains=entry.get("error_contains"),
                          source=source_of(entry),
                          creates=entry.get("creates", ())))

    # Every pair of options, in both orders, with a valid value where one is
    # needed. This is where mode against mode lands, and where an option that
    # needs a companion meets it. Both orders, because the order can decide
    # the outcome: with -h beside -V the first one written wins.
    for first, second in itertools.permutations(sorted(OPTIONS), 2):
        arguments = with_value(first) + with_value(second)
        case = explicit(arguments)
        if case is not None:
            cases.append(Case(arguments, case["verdict"], error=case.get("error"),
                              contains=case.get("error_contains"),
                              source=source_of(case),
                              creates=case.get("creates", ())))
        else:
            cases.append(pair_verdict(first, second, arguments))

    # The cases the spec lists on its own (the Python tools, and anything a
    # tool's page decides that the shapes above do not reach).
    for entry in CONTRACT.get("cases", []):
        cases.append(Case(entry["args"], entry["verdict"],
                          error=entry.get("error"),
                          contains=entry.get("error_contains"),
                          source=source_of(entry),
                          creates=entry.get("creates", ())))

    # A selector that names no board, with nothing asked of it: the refusal
    # has to be the command line's, not the fault of whichever boards are
    # attached, which "-U 0" cannot show on a machine that has a board at
    # index 0.
    selectors = [name for name, role in ROLES.items() if role == "selector"]
    if selectors and (WORK_OPTIONS is not None or REQUIRES_ALL is not None):
        name = selectors[0]
        expected = CONTRACT.get("selector_only_error")
        if expected is None:
            expected = (CONTRACT["requires_all_error"] if REQUIRES_ALL is not None
                        else CONTRACT["nothing_to_do"])
        elif "%s" in expected:
            expected = expected % NO_SUCH_SERIAL
        cases.append(Case([name, NO_SUCH_SERIAL], "refused", error=expected))

    # An option given twice. Two tools refuse it, as their man pages say:
    # ubertooth-follow for the three options that name the target ("give -t
    # once"), and ubertooth-specan's -v, which may be given only once. The
    # other man pages do not decide what the second one does, and those tools
    # keep the last and say nothing.
    for name, option in sorted(OPTIONS.items()):
        if option["value"] == "none" or ROLES.get(name) in ("help", "report"):
            continue
        values = option.get("valid") or []
        if len(values) < 2 or any(value in PLACEHOLDERS for value in values):
            continue
        doubled = [name, values[0], name, values[1]]
        if REQUIRES_ALL is not None:
            for other in REQUIRES_ALL:
                if other != name:
                    doubled += with_value(other)
        elif WORK_OPTIONS is not None and name not in WORK_OPTIONS:
            doubled = with_value(WORK_OPTIONS[0]) + doubled
        if explicit(doubled) is None:
            cases.append(Case(doubled, "undecided", source="Q-6"))

    return cases


# ---- running ---------------------------------------------------------------

def open_findings_for(identifier):
    """The findings that claim this case, by its exact command line."""
    return [entry for entry in EXPECTED_FAILURES
            if identifier in entry["cases"]]


def findings_of(claimed):
    """The finding ids of the entries that claim a case, for a message."""
    return ", ".join(sorted({finding for entry in claimed
                             for finding in entry["findings"]}))


def pair_reasons_with_complaints(reasons, complaints):
    """Pair each reason with one complaint it is inside.

    Returns the reasons no pairing could place, by their position, and the
    complaints left unpaired. The pairing is one to one because two reasons
    that both fit the same complaint are two complaints the cases have to
    produce, not one counted twice.

    Pairing is searched rather than taken first-come: a loose reason would
    otherwise take the only complaint a precise one fits, and the precise
    one would be reported as gone while it is still there. Each reason tries
    every complaint it fits, and a complaint already paired gives its reason
    a chance to move elsewhere -- the standard augmenting search, which on a
    handful of each costs nothing.
    """
    paired = {}                        # complaint position -> reason position

    def claim(which, tried):
        for spot, complaint in enumerate(complaints):
            if reasons[which] in complaint and spot not in tried:
                tried.add(spot)
                if spot not in paired or claim(paired[spot], tried):
                    paired[spot] = which
                    return True
        return False

    gone = [which for which in range(len(reasons))
            if not claim(which, set())]
    left = [complaint for spot, complaint in enumerate(complaints)
            if spot not in paired]
    return gone, left


def no_local_adapter(directory):
    """Does this machine lack the local Bluetooth adapter the tool opens?

    Only ubertooth-follow and ubertooth-scan open one, and only before they
    look for an Ubertooth, so where an accepted command line stops depends on
    the answer. One bare run settles it: the tool's own "no adapter" message
    means there is none.
    """
    if not CONTRACT.get("accepted_markers"):
        return True
    result, _ = run([], directory)
    output = (result.stdout + result.stderr).lower()
    return any(marker.lower() in output
               for marker in CONTRACT["accepted_markers"])


def main():
    if not TESTABLE:
        cases = []
        print("%s: %s" % (TOOL, " ".join(CONTRACT["why_not"])))
    elif OPTIONS:
        cases = generated_cases()
    else:
        cases = listed_cases()
    seen = {}
    for case in cases:
        # A generated case can repeat one the spec also states; keep the first.
        seen.setdefault(case.identifier, case)

    directory = tempfile.mkdtemp(prefix="cli-matrix-")
    failures = []
    fixed = []
    seen_cases = set()
    matched_findings = set()
    checked = 0
    undecided = 0
    known_cases = 0

    # The spec's option table against the tool's own getopt string, and the
    # man page it cites. Neither is a command line, so each gets a case id of
    # its own and then goes through the same expected-failure machinery as a
    # run: a mismatch in the table means the matrix is not covering what it
    # claims, and a page with no -h is finding F-09.
    spec_problems = {}
    for identifier, problem in check_the_spec_lists_every_option():
        spec_problems.setdefault(identifier, []).append(problem)

    def classify(identifier, case, problems, result):
        """File one case's complaints: new failure, open finding, or fixed."""
        nonlocal known_cases
        claimed = [entry for entry in open_findings_for(identifier)
                   if bare_adapter_run
                   or not entry.get("only_without_local_adapter")]
        if not claimed:
            if problems:
                failures.append((identifier, case, problems, result))
            return

        # The case is one an open finding breaks. It has to still break, in
        # the way that finding describes, and nothing else about it may have
        # gone wrong since.
        seen_cases.add(identifier)
        if not problems:
            fixed.append("%s: %s now passes"
                         % (identifier, findings_of(claimed)))
            return
        known_cases += 1
        matched_findings.update(findings_of(claimed).split(", "))
        # An entry names one reason per complaint its cases have to produce,
        # and every one of them has to be there: a single reason would stand
        # for whatever it happened to match, so a tool that broke the rule in
        # one more way, or stopped breaking it in one of them, would read as
        # unchanged.
        wanted = [(entry, reason) for entry in claimed
                  for reason in entry["problem_contains"]]
        gone, left = pair_reasons_with_complaints(
            [reason for _, reason in wanted], problems)
        for which in gone:
            entry, reason = wanted[which]
            fixed.append("%s: %s no longer complains %r; it now says %s"
                         % (identifier, ", ".join(entry["findings"]),
                            reason, problems))
        if left:
            failures.append((identifier, case, left, result))

    try:
        bare_adapter_run = no_local_adapter(directory)
        skipped = 0
        for identifier, case in sorted(seen.items()):
            # A case the tool accepts would go on to talk to the local
            # Bluetooth adapter on a machine that has one, which is not what
            # this test is about.
            if case.verdict in ("device", "undecided") and not bare_adapter_run:
                skipped += 1
                continue
            for entry in os.listdir(directory):
                path = os.path.join(directory, entry)
                shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
            # A tool that reads a file needs one to read; the spec's
            # placeholders name it, and the run directory is where it goes.
            for argument in case.arguments:
                if argument in INPUT_PLACEHOLDERS:
                    with open(os.path.join(directory, argument), "wb") as handle:
                        handle.write(b"\0" * 64)

            result, created = run(case.arguments, directory)
            # A file the test put there for the tool to read is not something
            # the run created.
            created = [name for name in created
                       if name not in INPUT_PLACEHOLDERS]
            problems = check(case, result, created)
            checked += 1
            if case.verdict == "undecided":
                undecided += 1

            classify(identifier, case, problems, result)

        # The two checks that are about the spec and the man page rather than
        # about a command line go through the same machinery.
        for identifier, problems in sorted(spec_problems.items()):
            classify(identifier, Case([], "refused", source="the spec"),
                     problems, NotRun())
    finally:
        shutil.rmtree(directory, ignore_errors=True)

    # A case an open finding claims that the matrix never generated: the
    # finding's own record of what it breaks has drifted, which hides the case
    # rather than checking it.
    for entry in EXPECTED_FAILURES:
        if entry.get("only_without_local_adapter") and not bare_adapter_run:
            continue
        for identifier in entry["cases"]:
            if identifier not in seen_cases:
                failures.append((identifier, Case([], "refused"),
                                 ["%s lists this case, but the matrix does "
                                  "not generate it"
                                  % ", ".join(entry["findings"])],
                                 NotRun()))

    for identifier, case, problems, result in failures:
        print("FAIL %s" % identifier)
        print("     expected: %s%s" % (case.verdict,
                                       " (%s)" % case.source if case.source else ""))
        for problem in problems:
            print("     %s" % problem)
        if result.returncode is not None:
            print("     status %d, stdout %r, stderr %r"
                  % (result.returncode, result.stdout[:200],
                     result.stderr[:300]))

    spec_name = os.path.basename(SPEC_PATH)
    for line in fixed:
        print("FIXED %s; update expected_failures in %s" % (line, spec_name))

    findings = sorted(matched_findings
                      | {finding for entry in EXPECTED_FAILURES
                         for finding in entry["findings"]})
    print("%s: %d cases, %d undecided, %d known failing%s"
          % (TOOL, checked, undecided, known_cases,
             " (%s)" % ", ".join(findings) if findings else ""))
    if skipped:
        print("%s: %d cases skipped: this machine has the local Bluetooth "
              "adapter the tool opens, so an accepted command line would go "
              "on to talk to it" % (TOOL, skipped))

    if failures or fixed:
        sys.exit(1)


main()
