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
# "Changes a firmware interface file" is an approximation, because CI cannot
# judge behaviour: it is a change to the content of one of the files listed
# below, comments and blank lines not counted. So the next reader knows which
# way each half of it is wrong:
#
#   - It misses a firmware change the host relies on that is made somewhere
#     else, firmware/common.mk above all (see watched_pattern below).
#   - It calls changes interface changes that are not. Three kinds, in
#     rough order of how often they turn up: a change to the BTBR and BTCTL
#     half of ubertooth_interface.h, which belongs to the btbr image and its
#     own bcdDevice; a change to one of the image's sources that the host
#     never reads, the LED patterns, the debug UART and tinyprintf among
#     them; and code moved verbatim between two watched files. Each is a
#     false red on a gating job, and the way out is the maintainer merging
#     it red, not a version nobody needs; the message below says so.
#   - It cannot see two open branches claiming the same value, since neither
#     is wrong until the other merges. The second goes red as soon as the
#     first lands, and the push-to-main leg of the job catches it even if the
#     pull request was merged without re-running.

set -u

# Byte order, everywhere. The lists of watched files are built with sort and
# then compared with comm, and comm refuses input it finds out of order: left
# to the environment's collation the two can disagree about two paths, which
# comm reports as a failure and some locales would make a daily occurrence.
# It also keeps the path matching below about bytes rather than about what the
# runner thinks a letter is.
LC_ALL=C
export LC_ALL

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

--list-watched prints the files a change to which asks for a new version, and
reads no commit, so it takes no main-ref.
EOF
}

# Run a git command over what goes into the bluetooth_rxtx image: the sources
# it compiles, the shared code it links (firmware/common, lpcusb included),
# the linker script it is laid out with, and its own Makefile, which is
# nothing but the list of those sources. One list, so the check and
# --list-watched cannot drift apart.
#
# Only files that are compiled or linked, or that say which files are: a
# README under one of those directories cannot change what the host sees, and
# counting it would let a branch authorise its own bump by appending a line to
# one.
#
# The two linker scripts are named rather than matched, because firmware/common
# holds four and this image uses two of them: common.mk's default,
# LPC17xx_Linker_Script_with_bootloader.ld, and the sections.ld it INCLUDEs.
# The other two belong to the bootloader and to assembly_test.
#
# Left out because it is not in this image: firmware/tests (host-compiled unit
# tests), firmware/bootloader and firmware/btbr (separate images, each with a
# bcdDevice of its own that this version says nothing about), the
# development-only images (blinky, cc2400_test, clock_test, usb_test,
# assembly_test, tc13badge), firmware/Makefile (which picks which images to
# build), and documentation.
#
# Left out although it is the one gap that can hide a real interface change:
# firmware/common.mk, the shared build file every image includes. It carries
# the compiler flags, -DTX_ENABLE and the commented-out -fpack-struct among
# them, so a change there can alter what the host sees with no source change
# at all. It also now holds part of the image's source list -- fw_info.c and
# the FW_IMAGE_NAME the firmware info block bakes in -- so dropping that
# source, which ubertooth-dfu requires, would trip nothing here. The
# firmware/ctags.mk it includes in turn defines only the ctags and cscope
# targets and cannot reach a compile or a link, so that one is no gap at all.
#
# Watching common.mk means accepting its false reds -- the two changes to it
# in the last 35 merges were an object-directory move and the info block --
# because the alternative, narrowing to the lines that set flags, needs a
# hand-kept list of flag spellings. Which of those costs less is a call for
# the maintainer rather than a decision for this script, and it is tracked
# separately; this check lands with the narrow list that is certainly right.
#
# Written as one pattern over whole paths, matched against a plain listing of
# the tree, rather than as git pathspecs. Pathspec globbing is not the same
# command to command -- the wildcards that select these files for `git diff`
# select nothing for `git ls-tree` -- and a list that silently comes back
# empty is the answer that asks for no bump.
#
# Both source arms reach any depth, so neither stops at a directory
# separator: firmware/common/ has to reach lpcusb/target/, where the shared
# USB sources live, and firmware/bluetooth_rxtx/ holds no subdirectory today
# but the btbr image already keeps its sources in src/ and include/. Were
# this image to grow one, matching only its direct children would see the
# Makefile edit that introduced the subdirectory and nothing in it ever
# after, which is a silent pass rather than a visible red. The README and the
# flash helper beside the sources are left out by the list of extensions, not
# by the depth, so reaching further costs nothing.
watched_pattern='^(firmware/bluetooth_rxtx/(.*\.(c|h|S|s|ld)|Makefile)|firmware/common/.*\.(c|h|S|s)|firmware/common/(LPC17xx_Linker_Script_with_bootloader|sections)\.ld|host/libubertooth/src/ubertooth_interface\.h)$'

# One watched file with everything that is not interface taken out: whole-line
# comments, blank lines, trailing space and the version define. What is left
# is what can change the firmware the host talks to, so two commits whose
# filtered files all agree have no interface change between them, and a branch
# whose only change is the bump itself, a whole-line comment or appended prose
# cannot buy itself a version.
#
# Only a whole-line comment, though. A comment that follows code, a comment
# body whose /* opened after code on an earlier line, and a re-indentation all
# read as content, so each counts as the firmware change that would justify a
# bump. The cost runs both ways: a reworded trailing comment asks for a bump
# nobody needed, and a branch that wanted one could take it by editing a
# trailing comment in any watched file. Reading further would mean lexing C,
# and a /* inside a string literal -- never alone on its line -- would then be
# mistaken for the start of a comment and swallow real code until the next */.
# Ignoring whitespace would retire the re-indentation half, but one of the
# watched files is the image's Makefile, where a leading tab is what makes a
# line a recipe, so whitespace there is content.
#
# What this does not weaken is the rule's point. The branch that has to be
# refused is the one that changed the firmware and kept the version, and for
# that the filter only has to notice real code changes, which it does.
strip_noninterface() {
    awk '
    # Inside a block comment that began on an earlier line.
    in_block {
        pos = index($0, "*/")
        if (pos == 0)
            next
        in_block = 0
        # A line that closes the comment and then carries code is kept whole:
        # filtering it further would mean lexing C.
        rest = substr($0, pos + 2)
        if (rest !~ /^[ \t]*$/)
            print_line($0)
        next
    }
    {
        line = $0
        sub(/^[ \t]+/, "", line)
        # The version define is not the interface; moving it is what the rest
        # of this script decides about.
        if (line ~ /^#[ \t]*define[ \t]+UBERTOOTH_API_VERSION[ \t]/)
            next
        if (line ~ /^\/\//)
            next
        if (line ~ /^\/\*/) {
            # A one-line /* ... */, or the start of a block.
            if (line ~ /\*\/[ \t]*$/)
                next
            if (index(line, "*/") == 0) {
                in_block = 1
                next
            }
        }
        print_line($0)
    }
    function print_line(s) {
        sub(/[ \t]+$/, "", s)
        if (s != "")
            print s
    }
    '
}

# That filtered text for one file at one commit, empty when the commit does
# not have the file, so adding or removing one shows up as a difference. A
# git show that fails is a failure rather than an empty answer: empty is the
# answer that asks for no bump.
interface_text() {
    git cat-file -e "$1:$2" 2> /dev/null || return 0
    text=$(git show "$1:$2") || return 1
    printf '%s\n' "$text" | strip_noninterface
}

# Every file the check watches at one commit, as "<blob hash> <tab> <path>":
# the image's sources, and the interface header the version lives in. The
# hashes are what let the comparison below read only the few files that could
# have changed, instead of filtering all forty-odd at two commits each.
#
# ls-tree's default output is parsed rather than its --format, which needs a
# git newer than some supported distributions ship. Its shape -- mode, type,
# hash, tab, path -- has been fixed for the life of git.
watched_at() {
    rev=$1
    out=$2
    git --no-pager ls-tree -r "$rev" > "$work/raw" || return 1
    # mode, type, hash, tab, path -> hash, tab, path, watched paths only.
    #
    # The pattern reaches awk through the environment rather than through -v,
    # because -v processes escape sequences in the value: it turns the \. that
    # means a literal dot into a plain . that matches any character, so this
    # reading of the list and --list-watched's grep would quietly disagree.
    # ENVIRON hands the string over untouched, which also keeps gawk from
    # warning about the escape on standard error.
    WATCHED_PATTERN="$watched_pattern" awk -F'\t' \
        'NF > 1 && $2 ~ ENVIRON["WATCHED_PATTERN"] { split($1, f, " "); print f[3] "\t" $2 }' \
        "$work/raw" > "$work/named" || return 1
    sort "$work/named" > "$out" || return 1
    rm -f "$work/raw" "$work/named"
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
            if [ "$list_watched" = yes ]; then
                echo "error: --list-watched twice" >&2
                exit 1
            fi
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

# --list-watched reads no commit, so a ref with it is a command line that
# cannot mean one thing; refusing it beats quietly doing one of the two.
if [ "$list_watched" = yes ] && [ -n "$main_ref" ]; then
    printf "error: --list-watched reads no commit, so '%s' cannot be checked\n" \
        "$main_ref" >&2
    printf '       at the same time\n' >&2
    exit 1
fi

top=$(git rev-parse --show-toplevel 2> /dev/null) || {
    echo "error: not inside a git checkout, so there is no branch to check" >&2
    exit 1
}
cd "$top" || exit 1

# The files a change to which asks for a bump, so a reader -- and the
# self-test -- can see what the check is watching rather than trust that the
# pathspecs above still match anything.
if [ "$list_watched" = yes ]; then
    tracked=$(git --no-pager ls-files) || exit 1
    printf '%s\n' "$tracked" | grep -E "$watched_pattern" | sort
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

# Which watched files this branch really changes. Every one is compared by
# content with the comments and blank lines taken out, rather than by whether
# the branch touched its path, so a comment appended to a firmware source does
# not read as the firmware change that would justify a bump.
#
# Comparing files this way rather than reading `git diff` output is also what
# keeps the answer independent of this checkout's configuration. A diff is a
# display format: color.ui = always colorises it even when nothing is a
# terminal, and diff.external replaces it altogether, either of which would
# leave a line-matching filter finding no changes and reporting the permissive
# answer. There is no format to get wrong here.
#
# Everything in this script fails rather than guesses, this included: a git
# command that went wrong must not come back as an empty list.
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

watched_at "$merge_base" "$work/base.list" || {
    echo "error: could not list the firmware sources at the merge base" >&2
    exit 1
}
watched_at "$head_commit" "$work/head.list" || {
    echo "error: could not list the firmware sources at HEAD" >&2
    exit 1
}

# The files worth filtering: those the two commits do not store as the same
# blob. A file with the same hash at both cannot have changed, comments or
# not, so there is nothing for the filter to decide about it. A file added or
# removed appears on one side only and is read with an empty text for the
# other, so it counts as a change unless it is all comments.
#
# comm writes to a file and its status is read, rather than the two being one
# pipeline ending in a tolerated failure. A pipeline's status is its last
# command's, so a comm or sort that went wrong would have come back as an
# empty list of changed files, which is the answer that asks for no bump --
# the one shape of wrongness this script is meant not to have. comm exits
# non-zero on input it finds out of order, which is a real possibility rather
# than a theoretical one: that is what LC_ALL above is for, and if the two
# ever disagree again this says so instead of passing the branch.
comm -3 "$work/base.list" "$work/head.list" > "$work/differing" || {
    echo "error: could not compare the firmware sources of the two commits" >&2
    exit 1
}
# comm indents a line that is in the second list only, so the leading blank is
# dropped before the path is cut out of "<hash> <tab> <path>".
: > "$work/paths"
if [ -s "$work/differing" ]; then
    sed -e 's/^[[:space:]]*//' -e '/^$/d' "$work/differing" | cut -f2 |
        sort -u > "$work/paths" || {
        echo "error: could not read which firmware sources differ" >&2
        exit 1
    }
fi

# Fed from a file rather than a pipe, so a failure below ends the script
# instead of a subshell it cannot report from.
: > "$work/changed"
while IFS= read -r path; do
    interface_text "$merge_base" "$path" > "$work/a" || {
        printf 'error: could not read %s at the merge base\n' "$path" >&2
        exit 1
    }
    interface_text "$head_commit" "$path" > "$work/b" || {
        printf 'error: could not read %s at HEAD\n' "$path" >&2
        exit 1
    }
    cmp -s "$work/a" "$work/b" || printf '%s\n' "$path" >> "$work/changed"
done < "$work/paths"
changed=$(cat "$work/changed")

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
        echo "       If none of them change what the host sees, this is a false"
        echo "       red. The usual reasons: the file is the interface header but"
        echo "       the change is to its BTBR or BTCTL half, which belongs to the"
        echo "       separate btbr image; the file is compiled into the image but"
        echo "       the host never reads it, such as the LED patterns or the"
        echo "       debug UART; or the code was moved verbatim between two of"
        echo "       these files. Say so on the pull request and let the"
        echo "       maintainer merge with this job red, rather than taking a"
        echo "       version nobody needs."
        echo "       tools/check-api-version.sh --list-watched prints the list."
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
# pull request would be refused by a job it cannot satisfy.
#
# It stays narrow, because a stray 0x0200 on an ordinary branch would refuse
# every user's board until it was reflashed, which is the thing the host-only
# half of the rule exists to prevent. So: a single major up from the merge
# base, the minor digits at 00, above main rather than merely not below it,
# and the three documents that record the version moving with it, which a
# release changes and a slip of the finger does not.
#
# Above main, because a value equal to main's is the collision this whole
# check is about, and the release shape must not be a door around it: a
# release branch that was cut before another release landed, or re-cut after
# one, would otherwise claim a version whose firmware is already in the tree.
# The push-to-main leg would not catch it either, since there the merge base
# is main and an equal value leaves through the "unchanged" exit above. A
# release rebased on main has its merge base at main's tip, so the value it
# takes is above main's and this still passes.
release_doc_list=$(git --no-pager diff --name-only "$merge_base" "$head_commit" -- \
    CHANGELOG.md COMPATIBILITY.md TEST_PLAN.md) || {
    echo "error: could not tell whether this branch is a release" >&2
    exit 1
}
# grep -c exits 1 on a count of 0, so its status is not read here.
release_docs=$(printf '%s\n' "$release_doc_list" | sort -u | grep -c .)
if [ "$(( cur_decimal % 100 ))" -eq 0 ] &&
    [ "$(( cur_decimal / 100 ))" -eq "$(( base_decimal / 100 + 1 ))" ] &&
    [ "$cur_decimal" -gt "$main_decimal" ] &&
    [ "$release_docs" -eq 3 ]; then
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
    echo "       (minor digits 00) with CHANGELOG.md, COMPATIBILITY.md and"
    echo "       TEST_PLAN.md moving with it."
    echo ""
    echo "       If this branch does change what the host sees, it changes it"
    echo "       somewhere tools/check-api-version.sh does not watch; say so on"
    echo "       the pull request."
} >&2
exit 1
