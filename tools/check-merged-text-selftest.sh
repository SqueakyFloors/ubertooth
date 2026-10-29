#!/bin/sh
# Check that tools/check-merged-text.sh answers each shape of branch the way
# it is meant to.
#
#     tools/check-merged-text-selftest.sh
#
# The check reports a paragraph a branch removes that main had just gained.
# Everything it must NOT report looks much the same in a diff: a paragraph
# rewritten in place, a paragraph moved to another page, a page renamed, a
# page deleted and merged into another, a revert, a generated roff page
# following its source. So most of the cases below are shapes that must pass,
# and each one is a way the check could start crying wolf that no other case
# would catch.
#
# Each case builds a throwaway repository with a main branch and a branch off
# it, because that is what the check reads: the branch's diff against its merge
# base, and main's own history behind it.
#
# Two kinds of case would catch the check losing its grip on the files. The
# dropped-paragraph cases must fail, and the passing cases assert the counts in
# the success line: a pathspec or a marker that stopped matching reports zero
# files compared and zero blocks read, and would otherwise pass everything.
#
# Prints each case with its exit status and both output streams, so a failing
# run says what happened without a rerun. Exits 0 when every case holds, 1
# otherwise.

# shellcheck disable=SC2329 # the para_* functions are called by name, through
# write_page, so shellcheck cannot see that they are used.

set -u

here=$(cd "$(dirname "$0")" && pwd) || exit 1
check="$here/check-merged-text.sh"

if [ ! -x "$check" ]; then
    printf '%s\n' "error: $check is missing or not executable" >&2
    exit 1
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

failures=0
case_name=
repos=0
repo=
status=0
out=
err=
adder=

fail() {
    printf '%s\n' "FAIL: $case_name: $*" >&2
    failures=$((failures + 1))
}

# The paragraphs the cases move around. Each is long enough to clear the
# check's word floor, and distinctive enough that a grep for one line of it
# says which paragraph reached the output.
para_level() {
    cat <<'EOF'
The level printed beside each packet is that packet's own reading in decibels,
taken from the strongest sample the radio reports while the packet is arriving,
so that a dump and a capture of the same run agree about every packet in it.
EOF
}

para_level_reworded() {
    cat <<'EOF'
Each packet's level is its own reading in decibels. The radio reports several
samples while a packet arrives and the strongest of them is the one printed,
which is also the one a capture of that run carries, so the two agree.
EOF
}

para_level_lightly_edited() {
    cat <<'EOF'
The level printed beside each packet is that packet's own reading in decibels,
taken from the strongest sample the radio reports while the packet is arriving,
so that a dump and a capture of one run agree about every packet in it.
EOF
}

para_timeout() {
    cat <<'EOF'
A board that stops answering is reported rather than waited on forever: the
tool says which request went unanswered, leaves the capture file as it stands,
and exits with a failure, so a wedged board cannot look like a quiet night.
EOF
}

para_hopping() {
    cat <<'EOF'
Hopping follows the sequence the connection agreed, and the tool prints the
channel each packet arrived on next to its timestamp, which is enough to tell a
missed hop from a packet the radio simply did not hear at all.
EOF
}

para_options() {
    cat <<'EOF'
An option given twice is refused rather than quietly taken the second time, and
a value that is not a number is named in the error, so a command line that does
something other than what it looks like cannot run.
EOF
}

para_examples() {
    cat <<'EOF'
The example below writes a capture to a file and prints nothing else, which is
the shape every tool here follows: results on standard output, and anything
about how the run went on standard error instead.
EOF
}

git_in() {
    git -C "$repo" "$@"
}

commit() {
    git_in add -A || exit 1
    git_in commit -q -m "$1" || exit 1
}

# Write a page from the paragraphs named, each under a heading of its own, so
# that a change at the top of the page and a change at the bottom land in
# separate hunks the way they do in a real man page.
write_page() {
    page=$1
    shift
    {
        printf '%% UBERTOOTH-PAGE(1)\n'
        for paragraph in "$@"; do
            # The heading is the paragraph's own name, not its position, so
            # that inserting a paragraph does not renumber the headings below
            # it: that would put an added line next to every removal and
            # nothing in these pages would ever be a plain deletion.
            printf '\n## %s\n\n' \
                "$(printf '%s' "${paragraph#para_}" | tr 'a-z_' 'A-Z ')"
            "$paragraph"
        done
    } > "$page"
}

# A repository with a documentation page, a roff page beside it, and a source
# file, all on a main branch.
new_repo() {
    repos=$((repos + 1))
    repo="$work/repo$repos"
    mkdir -p "$repo/host/doc/man" "$repo/src" || exit 1
    git_in init -q || exit 1
    git_in symbolic-ref HEAD refs/heads/main || exit 1
    git_in config user.email selftest@example.invalid || exit 1
    git_in config user.name "Self test" || exit 1
    write_page "$repo/host/doc/page.md" para_hopping para_options para_examples
    printf 'int main(void) { return 0; }\n' > "$repo/src/tool.c"
    commit "the page"
}

# The paragraph main is about to lose, added as a merge of its own.
add_level_paragraph() {
    {
        printf '\n## LEVEL\n\n'
        para_level
    } >> "$repo/host/doc/page.md"
    commit "the level beside a packet is the packet's own reading"
    adder=$(git_in rev-parse --short HEAD)
}

# Merges on main that do not touch the page, to put distance between the
# paragraph and a branch's merge base.
pad_main() {
    i=0
    while [ "$i" -lt "$1" ]; do
        printf 'int filler%s(void) { return %s; }\n' "$i" "$i" \
            > "$repo/src/filler$i.c"
        commit "filler $i"
        i=$((i + 1))
    done
}

branch_off() {
    git_in checkout -q -b branch main || exit 1
}

# Run the check inside the repository and report its status and streams.
run_check() {
    out="$work/out"
    err="$work/err"
    ( cd "$repo" && "$check" "$@" ) > "$out" 2> "$err"
    status=$?
    printf '\ncase: %s\n' "$case_name"
    printf '  exit status %s\n' "$status"
    sed 's/^/  stdout: /' "$out"
    sed 's/^/  stderr: /' "$err"
}

# Assert that a passing run really read the files.
assert_read() {
    grep -q "$1 file(s) compared" "$out" ||
        fail "the success line does not say $1 file(s) were compared"
    case $(sed -n 's/.*, \([0-9]*\) removed block(s) read.*/\1/p' "$out") in
    0|'')
        fail "no removed block was read, so nothing was compared"
        ;;
    esac
}

# ---- the loss the check exists for -------------------------------------
# A branch whose rebase resolution kept its own side of the page: its new
# paragraph is there, the paragraph main gained while it was being written is
# gone.

case_name="a paragraph main just gained, deleted by a branch, is reported"
new_repo
add_level_paragraph
branch_off
write_page "$repo/host/doc/page.md" \
    para_hopping para_timeout para_options para_examples
commit "say what a board that stops answering does"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "host/doc/page.md:" "$err" || fail "the file is not named"
grep -q "strongest sample the radio reports" "$err" ||
    fail "the lost paragraph itself is not quoted"
grep -q "$adder" "$err" ||
    fail "the commit that gave main the paragraph is not named"
grep -q "the level beside a packet" "$err" ||
    fail "that commit's subject is not named"
grep -q "Drops-merged-text" "$err" ||
    fail "the way out for a deliberate removal is not named"
[ -s "$out" ] && fail "wrote to standard output on a failure"

case_name="the same removal passes with a Drops-merged-text trailer"
new_repo
add_level_paragraph
branch_off
write_page "$repo/host/doc/page.md" \
    para_hopping para_timeout para_options para_examples
commit "take the level paragraph back out

Drops-merged-text: host/doc/page.md"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "excused by a Drops-merged-text trailer" "$out" ||
    fail "the success line does not say the removal was excused"

# ---- shapes that must not be reported ----------------------------------

case_name="a paragraph rewritten where it stood is not reported"
new_repo
add_level_paragraph
branch_off
write_page "$repo/host/doc/page.md" \
    para_hopping para_options para_examples para_level_reworded
commit "say the level more plainly"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
assert_read 1

case_name="a paragraph lightly edited and moved up the page is not reported"
new_repo
add_level_paragraph
branch_off
write_page "$repo/host/doc/page.md" \
    para_level_lightly_edited para_hopping para_options para_examples
commit "put the level paragraph first"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
assert_read 1

case_name="a paragraph moved to another page is not reported"
new_repo
add_level_paragraph
branch_off
write_page "$repo/host/doc/page.md" para_hopping para_options para_examples
write_page "$repo/host/doc/other.md" para_level
commit "the level belongs on the other page"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
assert_read 2

case_name="a renamed page carrying its text is not reported"
new_repo
add_level_paragraph
branch_off
git_in mv host/doc/page.md host/doc/renamed.md || exit 1
commit "rename the page"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"

case_name="a page deleted into another page is not reported"
new_repo
write_page "$repo/host/doc/old.md" para_timeout
commit "a page of its own for a board that stops answering"
branch_off
git_in rm -q host/doc/old.md || exit 1
write_page "$repo/host/doc/page.md" \
    para_hopping para_options para_examples para_timeout
commit "fold the old page into the main one"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
assert_read 2

case_name="a revert of text main gained long ago is not reported"
new_repo
add_level_paragraph
pad_main 7
branch_off
write_page "$repo/host/doc/page.md" para_hopping para_options para_examples
commit "take the level paragraph back out"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "^note:" "$err" ||
    fail "the removal outside the window is not mentioned on standard error"

case_name="a window that reaches further back reports the same removal"
run_check --window 20
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "7 merges before" "$err" ||
    fail "the distance back to the commit that added it is not reported"

# ---- the generated roff page -------------------------------------------
# host/doc/man/*.1 is generated from host/doc/*.md. A branch whose roff page is
# behind its source has lost nothing; a branch that drops a paragraph from the
# source has, even though a copy of the words elsewhere in the tree -- a stale
# roff page, or the same paragraph on another page -- is still there.

case_name="a generated roff page losing text is not reported"
new_repo
add_level_paragraph
{
    printf '.TH UBERTOOTH\\-PAGE 1\n.SH LEVEL\n'
    para_level
} > "$repo/host/doc/man/page.1"
commit "the roff page"
branch_off
printf '.TH UBERTOOTH\\-PAGE 1\n' > "$repo/host/doc/man/page.1"
commit "regenerate the roff page"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "no .md, .rst or .txt file changed" "$out" ||
    fail "the roff page was read as prose"

case_name="the source page losing text a copy still has is reported"
new_repo
add_level_paragraph
{
    printf '.TH UBERTOOTH\\-PAGE 1\n.SH LEVEL\n'
    para_level
} > "$repo/host/doc/man/page.1"
commit "the roff page"
branch_off
write_page "$repo/host/doc/page.md" \
    para_hopping para_timeout para_options para_examples
commit "say what a board that stops answering does"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "host/doc/page.md:" "$err" || fail "the source page is not named"

# ---- branches with nothing to report -----------------------------------

case_name="a branch that only adds prose is clean"
new_repo
add_level_paragraph
branch_off
{
    printf '\n## TIMEOUT\n\n'
    para_timeout
} >> "$repo/host/doc/page.md"
commit "say what a board that stops answering does"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "0 removed block(s) read" "$out" ||
    fail "the success line does not say no block was removed"
[ -s "$err" ] && fail "wrote to standard error on a clean branch"

case_name="a branch that changes no prose says so"
new_repo
branch_off
printf 'int added(void) { return 1; }\n' > "$repo/src/added.c"
commit "a source change"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "no .md, .rst or .txt file changed" "$out" ||
    fail "a branch with no prose change is not reported as such"

case_name="a checkout sitting on main has no branch to read"
new_repo
add_level_paragraph
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "no branch to read" "$out" || fail "it did not say there is no branch"

# ---- the command line --------------------------------------------------

case_name="an unknown argument is refused"
new_repo
run_check --wobble
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "error: unknown argument" "$err" || fail "no error line"
[ -s "$out" ] && fail "wrote to standard output on a usage error"

case_name="a window that is not a number is refused"
run_check --window soon
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "error: --window takes a count of merges" "$err" || fail "no error line"

case_name="a ref that does not exist is refused"
run_check --main not-a-ref
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "is not a commit in this checkout" "$err" || fail "no error line"

case_name="-h prints the usage on standard output"
run_check -h
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "^usage: " "$out" || fail "the usage text is not on standard output"
[ -s "$err" ] && fail "wrote to standard error on -h"

printf '\n'
if [ "$failures" -eq 0 ]; then
    printf '%s\n' "check-merged-text-selftest: every case holds."
    exit 0
fi
printf '%s\n' "error: $failures case(s) failed" >&2
exit 1
