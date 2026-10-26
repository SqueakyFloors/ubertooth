#!/bin/sh
# Run a command with a time limit, retrying it a few times.
#
#     tools/retry.sh <seconds> <command> [argument ...]
#
# <seconds> is the limit for one attempt. There are three attempts, with 5 then
# 10 seconds between them, and a command that ignores the limit is given 10
# more seconds before it is killed. So the longest this can take is
#
#     3 * (<seconds> + 10) + 15
#
# which is what a workflow step's timeout-minutes has to leave room for, or
# GitHub cancels the step before the message below is printed.
#
# CI uses this for its package installs. A distribution mirror that has
# stopped sending bytes is the problem being solved: the download then neither
# finishes nor fails, so the step sits there until the job's own timeout
# cancels it many minutes later, with nothing after the "Get:" line to say
# why. That reads as a hung build, and it has cost contributors hours of
# looking at the wrong thing. A limit per attempt turns it into a named
# failure in a few minutes, and the retry recovers the common case where the
# mirror is only having a bad minute.
#
# Every failure is retried, not only a timeout, because a mirror having a bad
# minute surfaces as whatever the package manager made of it -- a 404 from a
# half-written index, a hash mismatch, apt's own 100 -- and telling those from
# a genuine "no such package" by their status is guesswork. The waiting costs
# 15 seconds, so retrying the deterministic failures too is the cheaper
# mistake; their own error is printed on every attempt.
#
# The command's own output is left alone; the notes and errors here go to
# standard error. Exit status is the command's, or 1 when it never completed.

set -u

attempts=3
backoff=5
grace=10

usage="usage: tools/retry.sh <seconds> <command> [argument ...]"

if [ $# -ge 1 ] && { [ "$1" = -h ] || [ "$1" = --help ]; }; then
    printf '%s\n' "$usage"
    exit 0
fi

# Run with nothing at all, print the usage text; refuse any other bad command
# line with the error line alone, as .kiro/steering/cli-output.md asks.
if [ $# -eq 0 ]; then
    printf '%s\n' "error: a per-attempt limit in seconds and a command are both needed" >&2
    printf '%s\n' "$usage" >&2
    exit 1
fi

if [ $# -lt 2 ]; then
    printf '%s\n' "error: no command given after the per-attempt limit '$1'" >&2
    exit 1
fi

limit=$1
shift

case $limit in
    '' | *[!0-9]*)
        printf '%s\n' "error: the per-attempt limit must be whole seconds, not '$limit'" >&2
        exit 1
        ;;
esac

# 0 is rejected rather than passed on, because `timeout 0` means no limit at
# all, which is the one thing this script exists to prevent.
if [ "$limit" -eq 0 ]; then
    printf '%s\n' "error: the per-attempt limit must not be 0, which means no limit" >&2
    exit 1
fi

# GNU coreutils names it `timeout` on Linux and `gtimeout` on macOS, where
# Homebrew prefixes the GNU tools. GitHub's macOS images ship neither, so the
# attempts there are retried but not time-limited, and the note below says so
# rather than leaving the next reader to assume a limit that is not there. The
# timeout-minutes on the workflow step is the backstop.
if command -v timeout > /dev/null 2>&1; then
    limiter=timeout
elif command -v gtimeout > /dev/null 2>&1; then
    limiter=gtimeout
else
    limiter=
    printf '%s\n' "note: no timeout command here, so attempts are not time-limited" >&2
fi

attempt=1
timed_out=no
while :; do
    if [ -n "$limiter" ]; then
        # SIGTERM at the limit, SIGKILL $grace seconds later if it is ignored.
        # Killing a command part way through is only safe if the command has
        # no state to leave half-written, which is why the workflow wraps the
        # download of a package and not its installation: a kill during
        # unpacking would leave dpkg needing `dpkg --configure -a`, which no
        # retry here can do. A fetch has nothing to undo, and the package
        # manager's lock goes with the process.
        "$limiter" --kill-after="${grace}s" "$limit" "$@"
        status=$?
    else
        "$@"
        status=$?
    fi

    if [ "$status" -eq 0 ]; then
        exit 0
    fi

    # 124 is timeout's own code for the limit being reached, 137 the status it
    # reports for a command it had to kill. A command killed for running the
    # machine out of memory arrives as 137 too, so a stall is the likely
    # reading of it here rather than a certain one.
    if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
        timed_out=yes
        printf '%s\n' "note: '$*' did not finish within ${limit}s (attempt $attempt of $attempts)" >&2
    else
        timed_out=no
        printf '%s\n' "note: '$*' failed with status $status (attempt $attempt of $attempts)" >&2
    fi

    if [ "$attempt" -ge "$attempts" ]; then
        break
    fi
    attempt=$((attempt + 1))
    printf '%s\n' "note: retrying in ${backoff}s" >&2
    sleep "$backoff"
    backoff=$((backoff * 2))
done

if [ "$timed_out" = yes ]; then
    printf '%s\n' "error: '$*' never finished, in $attempts attempts of ${limit}s each." >&2
    printf '%s\n' "       The package mirror is too slow or has stalled. This is not a" >&2
    printf '%s\n' "       build or test failure; re-run the job." >&2
    exit 1
fi

printf '%s\n' "error: '$*' failed $attempts times, last with status $status." >&2
exit "$status"
