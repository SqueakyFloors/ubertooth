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

set -u

here=$(cd "$(dirname "$0")" && pwd) || exit 1
check="$here/check-merged-text.sh"

if [ ! -x "$check" ]; then
    printf '%s\n' "error: $check is missing or not executable" >&2
    exit 1
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

tab=$(printf '\t')
esc=$(printf '\033')

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
para_level="\
The level printed beside each packet is that packet's own reading in decibels,
taken from the strongest sample the radio reports while the packet is arriving,
so that a dump and a capture of the same run agree about every packet in it."

para_level_reworded="\
Each packet's level is its own reading in decibels. The radio reports several
samples while a packet arrives and the strongest of them is the one printed,
which is also the one a capture of that run carries, so the two agree."

para_level_lightly_edited="\
The level printed beside each packet is that packet's own reading in decibels,
taken from the strongest sample the radio reports while the packet is arriving,
so that a dump and a capture of one run agree about every packet in it."

para_timeout="\
A board that stops answering is reported rather than waited on forever: the
tool says which request went unanswered, leaves the capture file as it stands,
and exits with a failure, so a wedged board cannot look like a quiet night."

para_hopping="\
Hopping follows the sequence the connection agreed, and the tool prints the
channel each packet arrived on next to its timestamp, which is enough to tell a
missed hop from a packet the radio simply did not hear at all."

para_options="\
An option given twice is refused rather than quietly taken the second time, and
a value that is not a number is named in the error, so a command line that does
something other than what it looks like cannot run."

para_bullets="\
- The capture tools give the board back when they cannot start a capture, so
  that a second run finds a free board instead of one still claimed by the run
  that failed before it.
- A capture gives its receive transfer back when it ends, so a run with more
  than one phase no longer leaves one transfer outstanding for every phase
  after the first."

# The same two entries said shorter. Each keeps runs of six or seven words of
# the entry it replaces and nothing longer, which is what a condensed entry
# looks like: recognizable in short runs, nothing left in long ones.
para_bullets_condensed="\
- The capture tools give the board back, and a second run finds a free board.
- A capture gives its receive transfer back when it ends, so nothing is left
  outstanding after a run of several phases."

para_examples="\
The example below writes a capture to a file and prints nothing else, which is
the shape every tool here follows: results on standard output, and anything
about how the run went on standard error instead."

git_in() {
    git -C "$repo" "$@"
}

commit() {
    git_in add -A || exit 1
    git_in commit -q -m "$1" || exit 1
}

# Write a page from heading-and-paragraph pairs, so that a change at the top of
# the page and a change at the bottom land in separate hunks the way they do in
# a real man page.
#
#     write_page <path> <HEADING> <paragraph> [<HEADING> <paragraph>]...
write_page() {
    page=$1
    shift
    {
        printf '%% UBERTOOTH-PAGE(1)\n'
        # Each paragraph goes under a heading of its own name, not its
        # position, so that inserting a paragraph does not renumber the
        # headings below it: that would put an added line next to every
        # removal and nothing in these pages would ever be a plain deletion.
        while [ "$#" -ge 2 ]; do
            printf '\n## %s\n\n%s\n' "$1" "$2"
            shift 2
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
    write_page "$repo/host/doc/page.md" \
        HOPPING "$para_hopping" \
        OPTIONS "$para_options" \
        EXAMPLES "$para_examples"
    printf 'int main(void) { return 0; }\n' > "$repo/src/tool.c"
    commit "the page"
}

# The paragraph main is about to lose, added as a merge of its own.
add_level_paragraph() {
    {
        printf '\n## LEVEL\n\n'
        printf '%s\n' "$para_level"
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

# Run the check inside the repository and report its status and streams. The
# git configuration it sees is this file's, empty unless a case fills it, so
# that no case depends on the configuration of whoever runs the suite and one
# case can hand the check a hostile one on purpose.
: > "$work/gitconfig"
run_check() {
    out="$work/out"
    err="$work/err"
    (
        cd "$repo" || exit 1
        GIT_CONFIG_GLOBAL="$work/gitconfig" GIT_CONFIG_SYSTEM=/dev/null \
            "$check" "$@"
    ) > "$out" 2> "$err"
    status=$?
    printf '\ncase: %s\n' "$case_name"
    printf '  exit status %s\n' "$status"
    sed 's/^/  stdout: /' "$out"
    sed 's/^/  stderr: /' "$err"
}

# Assert that a passing run really read the files.
assert_read() {
    grep -q ": $1 file(s) compared" "$out" ||
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
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
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

# Inside the window but not at its near edge, so that narrowing the window
# fails this case as widening it fails the ones that must pass.
case_name="a paragraph eight merges old is still inside the window"
new_repo
add_level_paragraph
pad_main 8
branch_off
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
commit "say what a board that stops answering does"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "8 merges before" "$err" ||
    fail "the distance back to the commit that added it is not reported"

# Attribution takes the oldest commit the paragraph's lines can be dated
# from, so that main editing one line of an old paragraph does not make the
# whole paragraph look new. The price is in the comment at the top of the
# check: a paragraph main touched and the branch then dropped reads as old.
case_name="one line edited on main does not re-date the paragraph"
new_repo
add_level_paragraph
pad_main 12
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples" \
    LEVEL "$para_level_lightly_edited"
commit "a word in the level paragraph"
branch_off
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
commit "say what a board that stops answering does"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "^note:" "$err" ||
    fail "the removal was not counted as one from outside the window"

case_name="the same removal passes with a Drops-merged-text trailer"
new_repo
add_level_paragraph
branch_off
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
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
    HOPPING "$para_hopping" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples" \
    LEVEL "$para_level_reworded"
commit "say the level more plainly"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
assert_read 1

case_name="a paragraph lightly edited and moved up the page is not reported"
new_repo
add_level_paragraph
branch_off
write_page "$repo/host/doc/page.md" \
    LEVEL "$para_level_lightly_edited" \
    HOPPING "$para_hopping" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
commit "put the level paragraph first"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
assert_read 1

case_name="a paragraph moved to another page is not reported"
new_repo
add_level_paragraph
branch_off
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
write_page "$repo/host/doc/other.md" \
    LEVEL "$para_level"
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
grep -q ": 1 file(s) compared" "$out" ||
    fail "the success line does not say the renamed page was compared"

# A rename is where attribution is easiest to get wrong: main has to be
# searched along the path it knows, not the new one, or a loss inside a rename
# passes as text no commit can be found for.
case_name="a renamed page that drops the text is reported"
new_repo
add_level_paragraph
branch_off
git_in mv host/doc/page.md host/doc/renamed.md || exit 1
write_page "$repo/host/doc/renamed.md" \
    HOPPING "$para_hopping" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
commit "rename the page and say the hopping part again"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "host/doc/renamed.md:" "$err" || fail "the renamed page is not named"
grep -q "the level beside a packet" "$err" ||
    fail "the commit that gave main the paragraph is not named"

# The same, with rename detection turned off in the configuration: the check
# asks for it on its own command line, so the answer must not change.
case_name="a renamed page that drops the text is reported, renames off"
printf '[diff]\n    renames = false\n' > "$work/gitconfig"
run_check
: > "$work/gitconfig"
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "host/doc/renamed.md:" "$err" || fail "the renamed page is not named"

# A deleted page is where the diff reads differently: its text is removed
# against /dev/null, and the path it belongs to is the one it had. A branch
# that deletes a page main had just added text to has lost that text, and the
# report has to name that page and not whichever one it read last.
case_name="a page deleted with its text is reported"
new_repo
write_page "$repo/host/doc/zzz.md" \
    TIMEOUT "$para_timeout"
commit "a page of its own"
pad_main 12
{
    printf '\n## LEVEL\n\n%s\n' "$para_level"
} >> "$repo/host/doc/zzz.md"
commit "the level beside a packet is the packet's own reading"
branch_off
git_in rm -q host/doc/zzz.md || exit 1
commit "drop that page"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "host/doc/zzz.md:" "$err" || fail "the deleted page is not named"
grep -q "strongest sample the radio reports" "$err" ||
    fail "the paragraph main had just gained is not the one reported"
grep -q "the level beside a packet" "$err" ||
    fail "the commit that gave main the paragraph is not named"
# The page's older paragraph went with it, and went out of the window seven
# merges ago: it is the note's business, not the report's.
grep -q "waited on forever" "$err" &&
    fail "the page's older paragraph was reported too"

case_name="a page deleted beside an edited one is named correctly"
new_repo
write_page "$repo/host/doc/zzz.md" \
    TIMEOUT "$para_timeout"
commit "a page of its own"
{
    printf '\n## LEVEL\n\n%s\n' "$para_level"
} >> "$repo/host/doc/zzz.md"
commit "the level beside a packet is the packet's own reading"
branch_off
git_in rm -q host/doc/zzz.md || exit 1
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples" \
    TIMEOUT "$para_timeout"
commit "drop that page, say the timeout part on the main one"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "host/doc/zzz.md:" "$err" || fail "the deleted page is not named"
grep -q "host/doc/page.md:" "$err" &&
    fail "the loss was attributed to the page that was only edited"

case_name="a plain README is read, extension or not"
new_repo
mkdir -p "$repo/firmware/image" || exit 1
write_page "$repo/firmware/image/README" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options"
commit "a README for the image"
{
    printf '\n## LEVEL\n\n%s\n' "$para_level"
} >> "$repo/firmware/image/README"
commit "the level beside a packet is the packet's own reading"
branch_off
write_page "$repo/firmware/image/README" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options"
commit "say the hopping part in the README"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "firmware/image/README:" "$err" || fail "the README is not named"

case_name="a build file that ends in .txt is not prose"
new_repo
printf 'add_library(one STATIC one.c)\n' > "$repo/CMakeLists.txt"
{
    printf '# %s\n' "$para_hopping"
    printf 'add_library(two STATIC two.c)\n'
} >> "$repo/CMakeLists.txt"
commit "a build file with a long comment in it"
branch_off
printf 'add_library(one STATIC one.c)\n' > "$repo/CMakeLists.txt"
commit "take the second library back out"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "no documentation page changed" "$out" ||
    fail "a CMake file was read as a documentation page"

case_name="a condensed list is not reported item by item"
# Two list items taken out of one section and said again, shorter, in another.
# Read as one block their wording dilutes and the pair reads as a loss; read
# an item at a time, each one's words are still there.
new_repo
{
    printf '\n## CHANGES\n\n%s\n' "$para_bullets"
} >> "$repo/host/doc/page.md"
commit "two entries for the capture tools"
branch_off
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    "UNDER THE HOOD" "$para_bullets_condensed" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
commit "move the entries under the hood, shorter"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
# Three blocks: the two items, judged one at a time, and the heading above
# them. Judged as one block the pair would read as a loss, so the count is
# what says the splitter is still there.
grep -q "3 removed block(s) read" "$out" ||
    fail "the removed list was not read as three blocks"

case_name="a removal under the word floor is not reported"
new_repo
{
    printf '\n## NOTE\n\nThe board keeps its own count of these.\n'
} >> "$repo/host/doc/page.md"
commit "say that the board counts them"
branch_off
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
commit "say what a board that stops answering does"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"

# Attribution has to follow a rename on main, or the rename looks like the
# commit that wrote every line of the page and dates the whole page from it.
case_name="a rename on main does not date the page from the rename"
new_repo
add_level_paragraph
pad_main 12
git_in mv host/doc/page.md host/doc/moved.md || exit 1
commit "move the page"
branch_off
write_page "$repo/host/doc/moved.md" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
commit "say what a board that stops answering does"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "^note:" "$err" ||
    fail "the removal was not counted as one from outside the window"

# A page can carry the character that marks the start of a file's text in the
# comparison's own input. The paragraph is in the page twice here, because
# what the marker would break is the file's own text after it, and a paragraph
# the branch moved would count as kept through what the branch adds instead.
case_name="an escape in a page does not break what counts as kept"
new_repo
{
    printf '\n## LEVEL\n\n%s\n' "$para_level"
    printf '\n## NOTE\n\n%s[1mbold%s[0m\n' "$esc" "$esc"
    printf '\n## LEVEL AGAIN\n\n%s\n' "$para_level"
} >> "$repo/host/doc/page.md"
commit "the level beside a packet is the packet's own reading"
branch_off
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
{
    printf '\n## NOTE\n\n%s[1mbold%s[0m\n' "$esc" "$esc"
    printf '\n## LEVEL AGAIN\n\n%s\n' "$para_level"
} >> "$repo/host/doc/page.md"
commit "say the level once instead of twice"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
assert_read 1

# Half a dozen settings would each change what a diff looks like: an external
# diff program, a textconv filter, no prefixes or mnemonic ones, more context,
# another algorithm, no rename detection. The check names every one of them on
# its own command line, and this is what says it still does.
case_name="a hostile git configuration cannot blind the check"
new_repo
add_level_paragraph
branch_off
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
commit "say what a board that stops answering does"
cat > "$work/gitconfig" <<'EOF'
[diff]
    renames = false
    noprefix = true
    mnemonicPrefix = true
    context = 7
    algorithm = histogram
    indentHeuristic = false
    external = /usr/bin/false
[core]
    quotePath = true
EOF
run_check
: > "$work/gitconfig"
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "strongest sample the radio reports" "$err" ||
    fail "the lost paragraph is not quoted"

# ---- paths ---------------------------------------------------------------

# An attribute can tell git that a page is binary, and a binary diff has no
# hunks in it: there would be nothing to read and nothing to report.
case_name="a page called binary by an attribute is still read"
new_repo
add_level_paragraph
branch_off
printf '*.md -diff\n' > "$repo/.gitattributes"
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
commit "call the pages binary, and say what a board that stops answering does"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "strongest sample the radio reports" "$err" ||
    fail "the lost paragraph is not quoted"

case_name="a page whose name has a space in it is read"
new_repo
write_page "$repo/host/doc/second page.md" \
    HOPPING "$para_hopping" \
    OPTIONS "$para_options"
commit "a second page"
{
    printf '\n## LEVEL\n\n%s\n' "$para_level"
} >> "$repo/host/doc/second page.md"
commit "the level beside a packet is the packet's own reading"
branch_off
write_page "$repo/host/doc/second page.md" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options"
commit "say what a board that stops answering does"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "second page.md:" "$err" || fail "the page with a space is not named"

case_name="a page whose name has a tab in it is refused, not skipped"
new_repo
write_page "$repo/host/doc/pa${tab}ge.md" \
    HOPPING "$para_hopping" \
    LEVEL "$para_level"
commit "a page with a tab in its name"
branch_off
write_page "$repo/host/doc/pa${tab}ge.md" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout"
commit "say what a board that stops answering does"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "cannot read" "$err" ||
    fail "a path it cannot read was not refused"
[ -s "$out" ] && fail "wrote to standard output on a refusal"

case_name="a page deleted into another page is not reported"
new_repo
write_page "$repo/host/doc/old.md" \
    TIMEOUT "$para_timeout"
commit "a page of its own for a board that stops answering"
branch_off
git_in rm -q host/doc/old.md || exit 1
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples" \
    TIMEOUT "$para_timeout"
commit "fold the old page into the main one"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
assert_read 2

case_name="a revert of text main gained long ago is not reported"
new_repo
add_level_paragraph
pad_main 12
branch_off
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
commit "take the level paragraph back out"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "^note:" "$err" ||
    fail "the removal outside the window is not mentioned on standard error"

case_name="a window that reaches further back reports the same removal"
run_check --window 20
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "12 merges before" "$err" ||
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
    printf '%s\n' "$para_level"
} > "$repo/host/doc/man/page.1"
commit "the roff page"
branch_off
printf '.TH UBERTOOTH\\-PAGE 1\n' > "$repo/host/doc/man/page.1"
commit "regenerate the roff page"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "no documentation page changed" "$out" ||
    fail "the roff page was read as prose"

case_name="the source page losing text a copy still has is reported"
new_repo
add_level_paragraph
{
    printf '.TH UBERTOOTH\\-PAGE 1\n.SH LEVEL\n'
    printf '%s\n' "$para_level"
} > "$repo/host/doc/man/page.1"
commit "the roff page"
branch_off
write_page "$repo/host/doc/page.md" \
    HOPPING "$para_hopping" \
    TIMEOUT "$para_timeout" \
    OPTIONS "$para_options" \
    EXAMPLES "$para_examples"
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
    printf '%s\n' "$para_timeout"
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
grep -q "no documentation page changed" "$out" ||
    fail "a branch with no prose change is not reported as such"

case_name="a checkout sitting on main has no branch to read"
new_repo
add_level_paragraph
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "no branch to read" "$out" || fail "it did not say there is no branch"

# ---- the command line --------------------------------------------------

case_name="an unknown option is refused"
new_repo
run_check --wobble
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "error: unknown option" "$err" || fail "no error line"
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
