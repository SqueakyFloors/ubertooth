#!/bin/sh
# Fail when a branch claims a firmware API version main has already taken, or
# when a branch with no firmware change moves it at all.
#
# The rule is in .kiro/steering/branch-and-review.md, "Firmware API version".
# The host accepts firmware of exactly UBERTOOTH_API_VERSION, which the
# bluetooth_rxtx image reports as its USB bcdDevice, so a branch that changes
# that image's host-facing interface has to take a version main has not used.
# Two branches cut from the same main each picked the next value; one merged,
# and the other was then claiming a version whose firmware was already in the
# tree. Had it landed, tools built from it would have accepted a board still
# carrying the older image -- the one thing this version exists to prevent.
# Nothing noticed either time, because the rule was checked by hand.
#
# Run it from anywhere in a checkout:
#
#     tools/check-api-version.sh [main-ref]
#
# main-ref is the commit to treat as main's tip. It defaults to the remote
# this checkout tracks main from (branch.main.remote), then origin/main. CI
# runs this same script, so what fails there fails here.
#
# It reads the version at three commits:
#
#     base    at the merge base of main-ref and HEAD
#     cur     at HEAD
#     mainv   at main-ref
#
# and applies one rule:
#
#   - HEAD changes a firmware interface file: cur must be GREATER than mainv.
#     Greater than mainv, not than base, because main moves while a branch is
#     open. The collision described above is exactly the case where cur is
#     above base and equal to mainv, so comparing with the merge base is what
#     let it through.
#   - HEAD changes none of them: cur must EQUAL base. A host-only change has
#     no new firmware to pair with, so moving the version only refuses boards
#     that are fine.
#
# The value is binary-coded decimal, so comparing the raw 16-bit numbers
# orders it correctly and nothing here has to decode it to compare. The next
# value is not cur + 1, though: after 0x0119 comes 0x0120. A nibble above 9 is
# not a BCD digit at all and is refused, because 0x011a is the mistake that
# increment invites.
#
# What it cannot catch, so the next reader knows: two open branches claiming
# the same value, since neither is wrong until the other merges (the second
# goes red as soon as the first lands); and a firmware change that does not
# touch one of the files below. "Changes the interface" is approximated from
# paths because CI cannot judge behaviour.

set -u

header=host/libubertooth/src/ubertooth_interface.h

usage() {
    cat <<'EOF'
usage: tools/check-api-version.sh [main-ref]

Check UBERTOOTH_API_VERSION on this branch against main: a branch that changes
the firmware interface must take a version above main's, and a branch that
does not must leave it as the merge base had it.

main-ref is the commit to treat as main's tip, and defaults to the remote this
checkout tracks main from, then origin/main.
EOF
}

# The files whose change means the bluetooth_rxtx image's host-facing
# interface may have moved. Deliberately narrower than all of firmware/:
#
#   - firmware/bluetooth_rxtx is the image that reports this version, and
#     firmware/common is the shared code it links (its USB stack, clock and
#     radio helpers), so a change to either can change what the host sees.
#   - ubertooth_interface.h is the interface itself: the request numbers and
#     the structs both sides share, as well as the version.
#
# Left out on purpose, because none of it is in that image or that interface:
# firmware/tests (host-compiled unit tests), firmware/bootloader and
# firmware/btbr (separate images, each with a bcdDevice of its own that this
# version says nothing about), the development-only images (blinky,
# cc2400_test, clock_test, usb_test, assembly_test, tc13badge), and the
# firmware build files and README. Including them made the check fire on
# commits that could not have changed the interface, and a check that cries
# wolf gets ignored.
#
# The version lives in that header, so a change to the define itself does not
# count as a change to the interface. Counting it would let any branch certify
# its own bump: moving the version would be the firmware change that justifies
# moving the version, and the rule for a branch with no firmware change would
# never apply to anything.
watched_files() {
    git diff --name-only "$1" "$2" -- firmware/bluetooth_rxtx firmware/common
    # -U0 so only changed lines come back, not the context around them.
    if git diff -U0 "$1" "$2" -- "$header" |
        grep -E '^[-+]' |
        grep -vE '^(\+\+\+|---)' |
        grep -qvE '^[-+][[:space:]]*#[[:space:]]*define[[:space:]]+UBERTOOTH_API_VERSION[[:space:]]'; then
        printf '%s\n' "$header"
    fi
}

# The decimal number a four-digit BCD value spells, or failure when one of its
# nibbles is above 9 and so is not a decimal digit.
bcd_decimal() {
    total=0
    for place in 12 8 4 0; do
        nibble=$(( (0x$1 >> place) & 0xf ))
        [ "$nibble" -le 9 ] || return 1
        total=$(( total * 10 + nibble ))
    done
    printf '%s\n' "$total"
}

# "1.31 (0x0131)", from the four digits and the number they spell.
pretty() {
    printf '%d.%02d (0x%s)\n' "$(( $2 / 100 ))" "$(( $2 % 100 ))" "$1"
}

# The value after a BCD one: the number it spells plus one, re-encoded. So
# 0x0139 is followed by 0x0140 and 0x0199 by 0x0200.
next_bcd() {
    next=$(( $1 + 1 ))
    printf '0x%d%d%d%d\n' "$(( next / 1000 % 10 ))" "$(( next / 100 % 10 ))" \
        "$(( next / 10 % 10 ))" "$(( next % 10 ))"
}

# A commit named short enough to read in a message.
short() {
    git rev-parse --short "$1" 2> /dev/null || printf '%s\n' "$1"
}

# The four digits of UBERTOOTH_API_VERSION as one commit has it, on standard
# output. Every way of reading nothing is a failure here rather than an empty
# value: a check that had stopped finding the define would pass every branch.
read_version() {
    rev=$1
    label=$2
    if ! text=$(git show "$rev:$header" 2> /dev/null); then
        printf 'error: %s does not exist at %s (%s)\n' "$header" "$label" "$(short "$rev")" >&2
        return 1
    fi
    defines=$(printf '%s\n' "$text" |
        grep -cE '^[[:space:]]*#[[:space:]]*define[[:space:]]+UBERTOOTH_API_VERSION[[:space:]]')
    if [ "$defines" -ne 1 ]; then
        printf 'error: %s at %s (%s) defines UBERTOOTH_API_VERSION %s times, not once\n' \
            "$header" "$label" "$(short "$rev")" "$defines" >&2
        return 1
    fi
    # Whatever follows the value on the line, a comment included, is dropped.
    value=$(printf '%s\n' "$text" |
        sed -n 's/^[[:space:]]*#[[:space:]]*define[[:space:]][[:space:]]*UBERTOOTH_API_VERSION[[:space:]][[:space:]]*//p' |
        sed 's/[[:space:]].*//')
    if ! printf '%s\n' "$value" | grep -qE '^0x[0-9A-Fa-f]{4}$'; then
        printf "error: UBERTOOTH_API_VERSION at %s (%s) is '%s', which is not a\n" \
            "$label" "$(short "$rev")" "$value" >&2
        printf '       four-digit hex value such as 0x0131\n' >&2
        return 1
    fi
    printf '%s\n' "${value#0x}"
}

# The digits and the number they spell, for one commit, refusing anything that
# is not BCD. Sets $digits and $decimal.
load_version() {
    digits=$(read_version "$1" "$2") || return 1
    if ! decimal=$(bcd_decimal "$digits"); then
        printf 'error: UBERTOOTH_API_VERSION at %s (%s) is 0x%s, which is not\n' \
            "$2" "$(short "$1")" "$digits" >&2
        printf '       binary-coded decimal: every digit has to be 0-9, so the value\n' >&2
        printf '       after 0x0119 is 0x0120, not 0x011a\n' >&2
        return 1
    fi
}

main_ref=
while [ $# -gt 0 ]; do
    case $1 in
    -h | --help)
        usage
        exit 0
        ;;
    -*)
        printf "error: unknown option '%s'\n" "$1" >&2
        exit 1
        ;;
    *)
        if [ -n "$main_ref" ]; then
            printf "error: one main-ref at most, and '%s' is a second\n" "$1" >&2
            exit 1
        fi
        main_ref=$1
        ;;
    esac
    shift
done

top=$(git rev-parse --show-toplevel) || exit 1
if [ -z "$top" ]; then
    echo "error: not inside a git checkout" >&2
    exit 1
fi
cd "$top" || exit 1

if [ -z "$main_ref" ]; then
    remote=$(git config --get branch.main.remote 2> /dev/null) || remote=
    for candidate in "${remote:+$remote/main}" origin/main; do
        [ -n "$candidate" ] || continue
        if git rev-parse --verify --quiet "$candidate^{commit}" > /dev/null 2>&1; then
            main_ref=$candidate
            break
        fi
    done
fi
if [ -z "$main_ref" ]; then
    echo "error: no main to compare against: neither the remote this checkout" >&2
    echo "       tracks main from nor origin/main resolves. Fetch it, or name a" >&2
    echo "       commit: tools/check-api-version.sh <main-ref>" >&2
    exit 1
fi

main_commit=$(git rev-parse --verify --quiet "$main_ref^{commit}") || {
    printf "error: '%s' is not a commit in this checkout\n" "$main_ref" >&2
    exit 1
}
head_commit=$(git rev-parse --verify --quiet 'HEAD^{commit}') || {
    echo "error: HEAD is not a commit, so there is nothing to check" >&2
    exit 1
}
merge_base=$(git merge-base "$main_commit" "$head_commit" 2> /dev/null) || {
    printf "error: HEAD and '%s' share no commit, so there is no merge base to\n" "$main_ref" >&2
    echo "       compare against. A shallow clone is the usual reason: fetch the" >&2
    echo "       full history of both." >&2
    exit 1
}

load_version "$merge_base" "the merge base" || exit 1
base_digits=$digits
base_decimal=$decimal
load_version "$head_commit" "HEAD" || exit 1
cur_digits=$digits
cur_decimal=$decimal
load_version "$main_commit" "$main_ref" || exit 1
main_digits=$digits
main_decimal=$decimal

base_pretty=$(pretty "$base_digits" "$base_decimal")
cur_pretty=$(pretty "$cur_digits" "$cur_decimal")
main_pretty=$(pretty "$main_digits" "$main_decimal")
# 0x9999 is the last value BCD can spell. It is not reachable in practice, but
# a suggestion has to be a real value or it is worse than none.
if [ "$main_decimal" -ge 9999 ]; then
    free_value='a higher value (0x9999 is the last one BCD can spell)'
else
    free_value=$(next_bcd "$main_decimal")
fi

changed=$(watched_files "$merge_base" "$head_commit") || exit 1

# What was compared, for the record: a log that only says "passed" cannot be
# read later to see which main the answer was about.
compared=$(printf 'compared with %s (%s), merge base %s, which had %s' \
    "$main_ref" "$(git rev-parse --short "$main_commit")" \
    "$(git rev-parse --short "$merge_base")" "$base_pretty")

if [ -n "$changed" ]; then
    if [ "$(( 0x$cur_digits ))" -gt "$(( 0x$main_digits ))" ]; then
        echo "check-api-version: this branch changes the firmware interface and"
        echo "takes $cur_pretty, above main's $main_pretty."
        echo "$compared."
        exit 0
    fi
    {
        echo "error: this branch changes the firmware interface, so"
        echo "       UBERTOOTH_API_VERSION has to be above the one main carries."
        echo ""
        echo "         at HEAD:   $cur_pretty"
        echo "         on main:   $main_pretty"
        echo "         next free: $free_value"
        echo ""
        echo "       main here is $main_ref."
        echo ""
        echo "       Set it to $free_value in"
        echo "       $header, and move"
        echo "       CHANGELOG.md, COMPATIBILITY.md, TEST_PLAN.md and the pinned"
        echo "       strings in the tests with it."
        echo ""
        echo "       A board is accepted only if it reports exactly this version,"
        echo "       so reusing one main has already shipped lets tools built from"
        echo "       this branch accept a board carrying the older image."
        echo ""
        echo "       The firmware interface files this branch changes:"
        printf '%s\n' "$changed" | sed 's/^/         /'
        echo ""
        echo "       If none of them change what the host sees, say so on the pull"
        echo "       request; the list of watched files is in"
        echo "       tools/check-api-version.sh."
    } >&2
    exit 1
fi

if [ "$cur_digits" = "$base_digits" ]; then
    echo "check-api-version: no firmware interface file changed, and"
    echo "UBERTOOTH_API_VERSION is $cur_pretty, as the merge base had it."
    echo "$compared; main has $main_pretty."
    exit 0
fi
{
    echo "error: this branch changes no firmware interface file, so it has to"
    echo "       leave UBERTOOTH_API_VERSION as the merge base had it."
    echo ""
    echo "         at HEAD:       $cur_pretty"
    echo "         at merge base: $base_pretty"
    echo "         on main:       $main_pretty"
    echo "         next free, if this is a firmware change after all:"
    echo "                        $free_value"
    echo ""
    echo "       main here is $main_ref."
    echo ""
    echo "       Put it back to 0x$base_digits. A host-only change has no new"
    echo "       firmware to pair with, so moving the version only makes boards"
    echo "       that are fine be refused until they are reflashed."
    echo ""
    echo "       If this branch does change what the host sees, it changes it"
    echo "       somewhere tools/check-api-version.sh does not watch; say so on"
    echo "       the pull request."
} >&2
exit 1
