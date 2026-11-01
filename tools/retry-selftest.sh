#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Check that tools/retry.sh still does the three things CI relies on it for.
#
#     tools/retry-selftest.sh
#
# CI wraps its package downloads in retry.sh so that a distribution mirror
# which has stopped sending bytes becomes a named failure in a few minutes
# instead of a job cancelled at its own timeout with nothing to say why. That
# only helps if retry.sh really limits an attempt, really retries, and really
# passes an ordinary failure's exit status through, so each of those is
# checked here rather than assumed.
#
# The interesting case is a host with no timeout command. retry.sh then runs
# the attempts without a limit and says so, which is what happens on every
# macOS runner, and until this script existed that path had never been run.
# So the checks below branch on whether a limiter is present and assert the
# behaviour of whichever path this host takes.
#
# Prints each case with its exit status, how long it took and what it wrote to
# standard error, so a failing run says what happened without a rerun. Exits
# 0 when every case holds, 1 otherwise.

set -u

here=$(dirname "$0")
retry="$here/retry.sh"

if [ ! -x "$retry" ]; then
    printf '%s\n' "error: $retry is missing or not executable" >&2
    exit 1
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

failures=0
case_name=

# retry.sh looks for timeout, then gtimeout (Homebrew's name for the GNU
# tools on macOS), and falls back to running the attempts unbounded. Decide
# the same way, so the expectations below match the path it will take.
if command -v timeout > /dev/null 2>&1 || command -v gtimeout > /dev/null 2>&1; then
    limited=yes
else
    limited=no
fi
printf '%s\n' "attempts are time-limited on this host: $limited"

fail() {
    printf '%s\n' "FAIL: $case_name: $*" >&2
    failures=$((failures + 1))
}

# Run retry.sh and keep its exit status, its two output streams and how long
# it took. Nothing here uses set -e, because every case below is about a
# command that is meant to fail.
run() {
    case_name=$1
    shift
    printf '\n%s\n' "case: $case_name"
    printf '%s\n' "  tools/retry.sh $*"
    start=$(date +%s)
    "$retry" "$@" > "$work/out" 2> "$work/err"
    status=$?
    elapsed=$(($(date +%s) - start))
    printf '%s\n' "  exit status $status after ${elapsed}s"
    sed 's/^/  stderr: /' "$work/err"
    sed 's/^/  stdout: /' "$work/out"
}

want_status() {
    [ "$status" -eq "$1" ] || fail "exit status $status, expected $1"
}

want_stderr() {
    grep -qF -- "$1" "$work/err" || fail "standard error does not mention '$1'"
}

want_stdout() {
    grep -qF -- "$1" "$work/out" || fail "standard output does not mention '$1'"
}

want_empty_stdout() {
    [ ! -s "$work/out" ] || fail "wrote to standard output and should not have"
}

want_elapsed() {  # minimum maximum
    if [ "$elapsed" -lt "$1" ] || [ "$elapsed" -gt "$2" ]; then
        fail "took ${elapsed}s, expected between $1 and $2"
    fi
}

# A command that succeeds is run once and its status passed on, with nothing
# added to either stream. On a host with no limiter the note about that is the
# one thing allowed on standard error.
run "a command that succeeds" 5 true
want_status 0
want_empty_stdout
if [ "$limited" = yes ]; then
    [ ! -s "$work/err" ] || fail "wrote to standard error and should not have"
else
    want_stderr "no timeout command here"
fi

# An ordinary failure comes back with the command's own status, not with 1.
# CI depends on this: a package that genuinely does not exist has to look
# different from a mirror that stalled.
run "a command that fails" 5 sh -c 'exit 7'
want_status 7
want_stderr "failed with status 7 (attempt 1 of 2)"
want_stderr "failed 2 times, last with status 7"

# The retry is real: a command that fails once and succeeds next time comes
# back as a success. The marker file makes the second attempt behave
# differently from the first.
marker="$work/first-attempt-done"
# The $1 below belongs to the inner sh, which is handed the marker path as an
# argument, so it must not expand here.
# shellcheck disable=SC2016
run "a command that fails once" 5 \
    sh -c '[ -e "$1" ] && exit 0; : > "$1"; exit 1' retried "$marker"
want_status 0
want_stderr "failed with status 1 (attempt 1 of 2)"
want_stderr "retrying in"

# The case retry.sh exists for: a command that never finishes on its own.
if [ "$limited" = yes ]; then
    # Two attempts of 2 seconds with 10 seconds between them is about 14,
    # and anything near the 60 the command asked for would mean the limit
    # never landed.
    run "a command that never finishes" 2 sleep 60
    want_status 1
    want_elapsed 12 45
    want_stderr "did not finish within 2s (attempt 1 of 2)"
    want_stderr "never finished, in 2 attempts of 2s each."
    want_stderr "The package mirror is too slow or has stalled."
else
    # No limiter, so the attempt is not time-limited and the command runs to
    # completion past the limit it was given. That is the fallback working as
    # intended, and the note is what tells the reader the limit is not there.
    # The workflow step's timeout-minutes is the only bound on this path.
    run "a command that outlasts its limit, unbounded" 1 sleep 3
    want_status 0
    want_elapsed 3 30
    want_stderr "no timeout command here, so attempts are not time-limited"
fi

# The command line is checked before anything is run, and a bad one is
# refused by the rules in .kiro/steering/cli-output.md: usage on standard
# output for -h, the error line alone on standard error otherwise.
run "no arguments at all"
want_status 1
want_empty_stdout
want_stderr "error: a per-attempt limit in seconds and a command are both needed"
want_stderr "usage: tools/retry.sh"

run "a limit with no command" 5
want_status 1
want_stderr "error: no command given after the per-attempt limit '5'"

run "a limit that is not a number" abc true
want_status 1
want_stderr "error: the per-attempt limit must be whole seconds, not 'abc'"

# timeout 0 means no limit at all, which is the one thing retry.sh exists to
# prevent, so it is refused rather than passed on.
run "a limit of zero" 0 true
want_status 1
want_stderr "error: the per-attempt limit must not be 0, which means no limit"

run "the help option" -h
want_status 0
want_stdout "usage: tools/retry.sh"
[ ! -s "$work/err" ] || fail "-h wrote to standard error"

printf '\n'
if [ "$failures" -ne 0 ]; then
    printf '%s\n' "error: $failures of retry.sh's checks did not hold" >&2
    exit 1
fi
printf '%s\n' "retry.sh bounds an attempt, retries, and passes a status through."
