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
       tools/check-api-version.sh --list-watched

Check UBERTOOTH_API_VERSION on this branch against main: a branch that changes
the firmware interface must take a version above main's, and a branch that
does not must leave it as the merge base had it. A release is the one
exception: it takes the next major version on its own.

main-ref is the commit to treat as main's tip, and defaults to the remote this
checkout tracks main from, then origin/main.

--list-watched prints the files a change to which asks for a new version.
EOF
}

# Run a git command over what goes into the bluetooth_rxtx image: the sources
# it compiles, the shared code it links (firmware/common, lpcusb included) and
# its own Makefile, which is nothing but the list of those sources. One list,
# so the check and --list-watched cannot drift apart.
#
# Only files that are compiled, or that say which files are: a README under
# one of those directories cannot change what the host sees, and counting it
# would let a branch authorise its own bump by appending a line to one. A
# pathspec wildcard here matches across directories, so firmware/common/*.c
# reaches lpcusb too.
#
# Left out because it is not in this image: firmware/tests (host-compiled unit
# tests), firmware/bootloader and firmware/btbr (separate images, each with a
# bcdDevice of its own that this version says nothing about), the
# development-only images (blinky, cc2400_test, clock_test, usb_test,
# assembly_test, tc13badge), firmware/Makefile and ctags.mk (which pick which
# images to build), and documentation.
#
# Left out although it is a real gap: firmware/common.mk, the shared build file
# every image includes. It carries the compiler flags, -DTX_ENABLE and the
# commented-out -fpack-struct among them, so a change there can alter what the
# host sees with no source change at all. It is not watched because in the last
# 34 merges its only change was where object files are written, which no host
# can observe: watching it would have reded a pull request that could not have
# moved the interface, and the only ways out of a red gating job are a version
# bump that costs every user a reflash or an edit to this script. A check that
# cries wolf gets ignored, which is worse than no check. Narrowing it to the
# lines that set flags needs a hand-kept list of flag spellings, which is a
# call for the maintainer rather than a decision for this script to make.
#
# The interface header is not here: it is handled separately below, because
# the version lives in it.
over_image_sources() {
    git --no-pager "$@" -- \
        'firmware/bluetooth_rxtx/*.c' 'firmware/bluetooth_rxtx/*.h' \
        'firmware/bluetooth_rxtx/*.S' 'firmware/bluetooth_rxtx/*.s' \
        'firmware/bluetooth_rxtx/*.ld' 'firmware/bluetooth_rxtx/Makefile' \
        'firmware/common/*.c' 'firmware/common/*.h' \
        'firmware/common/*.S' 'firmware/common/*.s' 'firmware/common/*.ld'
}

# The interface header with everything that is not interface taken out: the
# version define, comment lines and blank lines. Two commits whose headers
# differ after that have a real interface change between them -- a request
# added or renumbered, a shared struct altered -- and two whose headers agree
# have only moved the version or reworded a comment.
#
# Comparing the file this way rather than reading `git diff` output is
# deliberate. A diff is a display format: color.ui = always colorises it even
# when nothing is a terminal, and diff.external replaces it altogether, either
# of which would leave a line-matching filter finding no changes and reporting
# the permissive answer. There is no format to get wrong here.
#
# The comment lines it recognises are the ones this header uses: a line whose
# first character is / or *. A line inside a block comment that starts with
# neither is read as code, which costs a bump that was not needed rather than
# missing one that was.
normalized_header() {
    git show "$1:$header" | sed \
        -e '/^[[:space:]]*#[[:space:]]*define[[:space:]][[:space:]]*UBERTOOTH_API_VERSION[[:space:]]/d' \
        -e '/^[[:space:]]*\/\{1,2\}/d' \
        -e '/^[[:space:]]*\*/d' \
        -e 's/[[:space:]]*$//' \
        -e '/^$/d'
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
    if ! printf '%s\n' "$value" | grep -qE '^0[xX][0-9A-Fa-f]{4}$'; then
        printf "error: UBERTOOTH_API_VERSION at %s (%s) is '%s', which is not a\n" \
            "$label" "$(short "$rev")" "$value" >&2
        printf '       four-digit hex value such as 0x0131\n' >&2
        return 1
    fi
    # The four digits alone. Stripped with sed rather than with a shell prefix
    # expansion, because the pattern that expansion needs reads as an issue
    # reference to tools/check-issue-references.sh.
    printf '%s\n' "$value" | sed 's/^0[xX]//'
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
list_watched=no
end_of_options=no
while [ $# -gt 0 ]; do
    if [ "$end_of_options" = no ]; then
        case $1 in
        -h | --help)
            usage
            exit 0
            ;;
        --list-watched)
            list_watched=yes
            shift
            continue
            ;;
        --)
            # So a ref whose name begins with a dash can still be named.
            end_of_options=yes
            shift
            continue
            ;;
        -*)
            printf "error: unknown option '%s'\n" "$1" >&2
            exit 1
            ;;
        esac
    fi
    if [ -n "$main_ref" ]; then
        printf "error: one main-ref at most, and '%s' is a second\n" "$1" >&2
        exit 1
    fi
    main_ref=$1
    shift
done

top=$(git rev-parse --show-toplevel) || exit 1
if [ -z "$top" ]; then
    echo "error: not inside a git checkout" >&2
    exit 1
fi
cd "$top" || exit 1

# The files a change to which asks for a bump, so a reader -- and the
# self-test -- can see what the check is watching rather than trust that the
# pathspecs above still match anything.
if [ "$list_watched" = yes ]; then
    over_image_sources ls-files || exit 1
    printf '%s\n' "$header"
    exit 0
fi

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

# Everything in this script fails rather than guesses, this included: a git
# command that went wrong must not come back as an empty list, which is the
# answer that asks for no bump.
changed=$(over_image_sources diff --no-color --name-only "$merge_base" "$head_commit") || {
    echo "error: could not list the firmware sources this branch changes" >&2
    exit 1
}
base_header=$(normalized_header "$merge_base") || exit 1
head_header=$(normalized_header "$head_commit") || exit 1
if [ "$base_header" != "$head_header" ]; then
    changed=$(printf '%s\n%s\n' "$changed" "$header" | grep .)
fi

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

# The one move allowed with no firmware change: a release takes the next major
# version, with the minor digits at 00, to mark that its host tools and
# firmware only work with each other -- whether or not the cycle changed the
# firmware (docs/source/release_procedure.rst). Without this the release's own
# pull request would be refused by a job it cannot satisfy. It stays narrow: a
# single major up from the merge base, nothing below main, and no other value.
if [ "$(( cur_decimal % 100 ))" -eq 0 ] &&
    [ "$(( cur_decimal / 100 ))" -eq "$(( base_decimal / 100 + 1 ))" ] &&
    [ "$cur_decimal" -ge "$main_decimal" ]; then
    echo "check-api-version: no firmware interface file changed, and"
    echo "UBERTOOTH_API_VERSION moves to $cur_pretty, the next major version,"
    echo "which is a release's own bump."
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
    echo "       that are fine be refused until they are reflashed. The one"
    echo "       exception is a release, which takes the next major version"
    echo "       (minor digits 00) on its own."
    echo ""
    echo "       If this branch does change what the host sees, it changes it"
    echo "       somewhere tools/check-api-version.sh does not watch; say so on"
    echo "       the pull request."
} >&2
exit 1
