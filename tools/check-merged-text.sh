#!/bin/sh
# Check that this branch does not delete documentation main had already
# gained.
#
#     tools/check-merged-text.sh [--main <ref>] [--window <merges>]
#
# A rebase is how a branch gets on top of what merged while it was being
# written, and a conflict resolution that keeps the branch's side of a file
# deletes the other side's work. The compiler and the tests catch that in code.
# Nothing reads prose, so a rebase that drops a paragraph of a man page merges
# green and the page is simply shorter afterwards. That happened to
# host/doc/ubertooth-btle.md: three paragraphs two pull requests had added
# hours earlier went out with one rebase resolution and were found by accident
# half a day later.
#
# So this compares the branch with its merge base and asks, of every paragraph
# the branch removes: is it text main had only just gained, and is it gone? In
# full, a removed paragraph is reported when all four hold.
#
#   1. It is in a hunk that removes lines and adds none. A paragraph replaced
#      where it stood is the author editing their own page, not a resolution
#      dropping someone else's. The diff is read with no context lines, so an
#      unrelated addition a line or two away is a hunk of its own and does not
#      hide a deletion.
#   2. It is at least MIN_WORDS words long, so a removed heading or list item
#      is not read as a lost paragraph. A run of removed lines is split at
#      blank lines, headings, list markers and table rows, so that two list
#      items removed together are judged one at a time: judged as one block
#      their wording dilutes, and a list rewritten shorter reads as a loss.
#   3. Under COVERAGE_MAX of its SHINGLE-word runs are left in that file as the
#      branch leaves it, or anywhere the branch adds text. Word runs follow
#      prose through re-wrapping and small edits, and the "anywhere the branch
#      adds" half is what lets a paragraph move to another page, into a renamed
#      file, or into a comment, without being reported.
#   4. main gained it within WINDOW merges of the merge base. That is as close
#      as this can get to "text that landed while the branch was being
#      written": a rebase rewrites the dates of the commits it replays, and a
#      squash merge rewrites them again, so when a branch began is not in the
#      history any more. What is left is how new the text was when the branch
#      was rebased onto it, and text that new is what a resolution can have
#      landed over. Older text in the same shape is the ordinary rewriting of
#      a page: the author had it in front of them and chose.
#
# The fourth test is what makes the first three usable. Measured over the heads
# of the 167 pull requests merged in three days, the first three alone report
# nine removed paragraphs: the three real ones and six rewrites of paragraphs
# between five hours and a day old. With the window at its default the same run
# reports the three and nothing else.
#
# Scope: *.md, *.rst, *.txt and the plain READMEs, less the build files that
# happen to end in .txt. host/doc/man/*.1 is generated from the matching
# host/doc/*.md and follows it, so the roff pages are not read -- a loss shows
# in the source page. Code is not read either, and that is a decision rather
# than an omission: the same tests over the C, header, test and build files of
# those three days report 78 removed blocks, every one of them a rename, a
# refactor or a revert, because moving a function and dropping one look alike
# in a diff. A build and a test suite watch code; this watches what they do
# not.
#
# What it cannot see, so the next reader knows:
#   - A branch that has not been rebased past the text yet. Its merge base is
#     older than the text, so the text is not in the branch's diff at all. CI
#     runs this on every push, and the rebase before a pull request is opened
#     or merged is the push that matters.
#   - A resolution that dropped main's text in the same hunk as a change of its
#     own that it kept -- both sides adding a section at the same place, for
#     instance. Test 1 reads that as a replacement. Dropping test 1 would
#     report every paragraph anyone rewrites in place, which is most of the
#     ordinary work on a page.
#   - A paragraph main edited a line of inside the window, but wrote longer
#     ago than that. Attribution takes the oldest commit any of the lines
#     searched can be dated from, so that an edit since does not make an old
#     paragraph look new, and the price is that a paragraph main touched and
#     the branch then dropped reads as old.
#   - A paragraph main gained longer ago than the window, and a single removed
#     sentence under the word floor.
#   - A paragraph whose wording no commit on main can be found for: if the
#     search comes back empty there is no telling whether it is recent, so it
#     is counted in the note on standard error rather than reported.
#   - Most of a paragraph. A resolution that kept a phrase of it and dropped
#     the rest leaves COVERAGE_MAX of its runs behind, and this reads that as
#     kept. It is whole paragraphs that go missing in a resolution.
#
# A `git merge` of main into the branch rather than a rebase is covered: the
# merge base becomes the main commit that was merged, so a resolution that
# dropped main's text shows in the branch's diff as a plain deletion like any
# other.
#
# And what it reports that is not a loss: a paragraph main gained inside the
# window that this branch deliberately rewrites somewhere else in the file,
# reverts, takes out along with the page or section it was part of -- a
# clean-up of something main had only just added -- or stops repeating,
# leaving it on the one page of several that had it. There is no telling any of
# those from a resolution that dropped it, so the answer is the
# Drops-merged-text trailer the failure message names.
#
# It reads commits, not the working tree, as CI does: an edit that is not
# committed yet is not compared.
#
# Exits 1 and reports each removed paragraph on standard error, 0 and one line
# on standard output otherwise. CI runs this same script, so what fails there
# fails here.

set -u

self=$(basename "$0")

# How far back a paragraph may have arrived on main, in first-parent commits
# from the commit that added it to the merge base. main is a line of squash
# merges, so this counts merges. Ten is three or four hours at this
# repository's pace; the nearest ordinary rewrite, over every pull-request head
# this repository has, is seventeen merges away, and widening it to sixteen
# adds no report at all. Ten leaves that margin and still covers a branch whose
# last rebase lagged main by an afternoon.
WINDOW=10

# How much of a removed paragraph's wording has to be left for it to count as
# kept rather than lost, and the length of the word runs compared. Four-word
# runs at 15 per cent is what separates the known loss (none of it left, and
# 8 per cent for the one paragraph whose subject the page still discusses) from
# the ordinary rewriting of a changelog entry that keeps its sense in fewer
# words (31 per cent, measured on a real pull request that condensed two).
SHINGLE=4
COVERAGE_MAX=0.15

# The shortest removed block worth reading: a paragraph rather than a heading
# or a list item. From the audit that found the one known loss.
MIN_WORDS=25

usage() {
    cat <<EOF
usage: $self [--main <ref>] [--window <merges>]

Reports paragraphs this branch removes that main had gained within <merges>
merges of the branch's merge base, and whose wording the branch does not keep.

  --main <ref>       compare against this ref (default: main's upstream, or
                     the first of origin/main, squeaky/main, main that exists)
  --window <merges>  how recently main must have gained the text
                     (default: $WINDOW; 0 means only the merge base itself)
EOF
}

main_ref=
while [ "$#" -gt 0 ]; do
    case "$1" in
    --main)
        [ "$#" -ge 2 ] || { echo "error: --main needs a ref" >&2; exit 1; }
        main_ref=$2
        shift 2
        ;;
    --window)
        [ "$#" -ge 2 ] || { echo "error: --window needs a number" >&2; exit 1; }
        case "$2" in
        ''|*[!0-9]*)
            printf "error: --window takes a count of merges, not '%s'\n" \
                "$2" >&2
            exit 1
            ;;
        esac
        WINDOW=$2
        shift 2
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        printf "error: unknown option '%s'\n" "$1" >&2
        exit 1
        ;;
    esac
done

top=$(git rev-parse --show-toplevel) || exit 1
if [ -z "$top" ]; then
    echo "error: not inside a git checkout" >&2
    exit 1
fi
cd "$top" || exit 1

tab=$(printf '\t')
esc=$(printf '\033')

# main by name rather than by hash: the question is what main has now, not what
# the clone was left at. A named ref that does not resolve is an error rather
# than a quiet comparison with something else, because every way of reading
# nothing here would otherwise pass.
#
# main's own upstream comes first, so that a clone whose `origin` is the
# upstream project rather than this fork is not compared with upstream's main.
if [ -z "$main_ref" ]; then
    main_ref=$(git rev-parse --abbrev-ref --symbolic-full-name "main@{upstream}" \
        2> /dev/null) || main_ref=
fi
if [ -n "$main_ref" ]; then
    if ! git rev-parse --verify --quiet "$main_ref^{commit}" > /dev/null; then
        printf "error: '%s' is not a commit in this checkout\n" \
            "$main_ref" >&2
        exit 1
    fi
else
    for candidate in origin/main squeaky/main main; do
        if git rev-parse --verify --quiet "$candidate^{commit}" > /dev/null; then
            main_ref=$candidate
            break
        fi
    done
    if [ -z "$main_ref" ]; then
        echo "error: no origin/main, squeaky/main or main in this checkout," >&2
        echo "       so there is nothing to compare with. Fetch main, or name" >&2
        echo "       a ref with --main." >&2
        exit 1
    fi
fi

head=$(git rev-parse --verify HEAD) || exit 1
base=$(git merge-base HEAD "$main_ref") || exit 1
if [ -z "$base" ]; then
    printf 'error: HEAD and %s have no common ancestor\n' "$main_ref" >&2
    exit 1
fi
short_base=$(git rev-parse --short "$base")

# A checkout sitting on main, or behind it, is not a branch: there is no
# resolution to have gone wrong. The release and schedule runs of CI are in
# that position, and a comparison of main with itself would be a green that
# proves nothing.
if [ "$base" = "$head" ]; then
    echo "$self: HEAD is contained in $main_ref, so there is no branch to read."
    exit 0
fi

# Every git diff below: no external diff program, no textconv filter, text
# whatever the attributes say, the prefixes this script parses rather than
# whatever diff.noprefix or diff.srcPrefix say, and paths unquoted. A
# configuration file or a .gitattributes must not be able to turn this into a
# comparison of nothing: `*.md -diff` alone would otherwise leave every page
# reading "Binary files differ", with no hunk to read and nothing to report.
# Rename detection, the algorithm and the hunk heuristic are named too, not
# because the defaults are wrong but because they are the defaults: a setting
# in anyone's configuration that moved a hunk boundary or turned a rename into
# a delete and an add would change what this reports, and a check whose answer
# depends on the reader's git config is not a check.
git_diff() {
    git -c core.quotePath=false diff --no-ext-diff --no-textconv --text \
        --no-color --src-prefix=a/ --dst-prefix=b/ --find-renames \
        --diff-algorithm=myers --indent-heuristic "$@"
}

# The prose pathspecs, in one place, and the same list is the workflow's path
# filter. host/doc/man is left out by extension: it holds only the generated
# roff pages. The two exclusions are build files that end in .txt and are not
# prose at all: a removed CMake block is code, and code is out of scope for the
# reason at the top. The plain READMEs under firmware/ carry no extension, so
# they are named.
diff_prose() {
    git_diff "$1" "$base" "$head" -- '*.md' '*.rst' '*.txt' 'README' '*/README' \
        ':(exclude)*CMakeLists.txt' ':(exclude)*requirements.txt'
}

changed=$(git_diff --name-only "$base" "$head" -- \
    '*.md' '*.rst' '*.txt' 'README' '*/README' \
    ':(exclude)*CMakeLists.txt' ':(exclude)*requirements.txt') ||
    { echo "error: git diff failed" >&2; exit 1; }
compared=$(printf '%s\n' "$changed" | grep -c .)
if [ "$compared" -eq 0 ]; then
    echo "$self: no documentation page changed since $short_base."
    exit 0
fi

# A path this script could not carry through a line-based pipeline is refused
# rather than skipped: a tab or an escape would be read as a field separator or
# a marker below, and git quotes a path with a newline or a control character
# in it, so a leading quote says the same thing. None of this can happen to a
# documentation page in this tree; a silent pass could.
case "$changed" in
*"$tab"* | *"$esc"*)
    echo "error: a changed path has a tab or an escape in it, which this" >&2
    echo "       script cannot read. Rename the file." >&2
    exit 1
    ;;
esac
case "
$changed" in
*'
"'*)
    echo "error: git quoted a changed path, so it has a newline or a control" >&2
    echo "       character in it, which this script cannot read. Rename the" >&2
    echo "       file." >&2
    exit 1
    ;;
esac

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

# What counts as kept: each changed file as the branch leaves it, and every
# line the branch adds anywhere, prose or not, so that text moved into a
# comment or a script header is not read as lost. Each file's own text is kept
# apart from the others because the same paragraph appears on several man pages
# here, and a page losing it while its neighbors keep it is exactly the loss
# being looked for.
#
# The escape character marks the start of a file's text; paths carrying one
# were refused above. The marker for the added lines is always written, so this
# file is never empty -- which matters, because the reading pass below tells
# its two inputs apart by FNR == NR.
{
    printf '%s\n' "$changed" | while IFS= read -r file; do
        [ -n "$file" ] || continue
        if git cat-file -e "$head:$file" 2> /dev/null; then
            printf '%s%s\n' "$esc" "$file"
            # The escape is the marker, so a file that contains one must not
            # be able to open a bucket of its own. Nothing is compared on
            # punctuation, so dropping it costs nothing.
            git show "$head:$file" | LC_ALL=C tr -d '\033'
        fi
    done
    printf '%s+added\n' "$esc"
    git_diff -U0 "$base" "$head" |
        awk '/^\+\+\+ (b\/|\/)/ { next } /^\+/ { print substr($0, 2) }'
} > "$work/kept"

# Files the branch leaves with something in them. An empty one gives the
# comparison no text to match against and must not count towards the floor
# below, or a branch that adds an empty page would fail with a message blaming
# this script.
present=$(printf '%s\n' "$changed" | while IFS= read -r file; do
    [ -n "$file" ] || continue
    size=$(git cat-file -s "$head:$file" 2> /dev/null) || continue
    [ "$size" -gt 0 ] && echo x
done | grep -c .)

diff_prose -U0 > "$work/diff" ||
    { echo "error: git diff failed" >&2; exit 1; }

# The reading pass. First input is the kept text, second the diff; it writes
# one index line per reported paragraph, plus the paragraph itself and the
# lines to search main's history with, in files of their own. Keeping the text
# out of the index means no path or sentence has to survive a round trip
# through field splitting.
awk -v SH="$SHINGLE" -v MINW="$MIN_WORDS" -v COVER="$COVERAGE_MAX" \
    -v ESC="$esc" -v OUT="$work" '
function wordify(s,   t) {
    t = tolower(s)
    gsub(/[^a-z0-9]+/, " ", t)
    return t
}

# Feed text into a bucket, as one run of words: the runs cross line endings,
# so re-wrapped prose still matches.
function feed(bucket, s,   n, i, j, run, arr) {
    n = split(wordify(s), arr, " ")
    for (i = 1; i <= n; i++) {
        qn++
        q[qn] = arr[i]
        if (qn >= SH) {
            run = q[qn - SH + 1]
            for (j = qn - SH + 2; j <= qn; j++)
                run = run " " q[j]
            kept[bucket, run] = 1
            shingles++
        }
    }
}

function trim(s) {
    gsub(/^[ \t]+|[ \t]+$/, "", s)
    return s
}

# Does this line start something of its own -- a list item, a heading, a table
# row, a quote? Then it is not a continuation of the paragraph above it, and a
# block ends here.
function starts_item(s,   t) {
    t = trim(s)
    if (t ~ /^#/)                               # a heading of any level
        return 1
    if (t ~ /^([-*+]|[0-9]+[.)])([ \t]|$)/)     # a list item
        return 1
    if (t ~ /^[|>]/)                            # a table row, a quote
        return 1
    if (t ~ /^(```|~~~)/)                       # a code fence
        return 1
    if (t ~ /^(=+|-+|~+|\^+)$/)                 # an rst underline
        return 1
    return 0
}

# Decide on one run of removed lines, and report it if all the content tests
# hold. pure says whether the hunk it came from added nothing.
function flush(pure,   i, j, text, n, arr, run, total, hit, cov, line, keys) {
    if (bn == 0)
        return
    blocks++
    text = ""
    for (i = 1; i <= bn; i++)
        text = (i > 1) ? text " " B[i] : B[i]
    n = split(wordify(text), arr, " ")
    if (!pure || n < MINW) {
        bn = 0
        return
    }
    total = 0
    hit = 0
    for (i = 1; i + SH - 1 <= n; i++) {
        run = arr[i]
        for (j = i + 1; j <= i + SH - 1; j++)
            run = run " " arr[j]
        total++
        if (((file SUBSEP run) in kept) || (("+added" SUBSEP run) in kept))
            hit++
    }
    cov = (total > 0) ? hit / total : 1
    if (cov >= COVER) {
        bn = 0
        return
    }
    # The lines main is searched for: the longest of the paragraph, which are
    # long enough to be that paragraph and nothing else, and substrings of the
    # file as it was, which is what git log -S wants. Several of them, because
    # a line that was re-wrapped or had a typo fixed since would date the whole
    # paragraph from that edit; the oldest answer wins, in the shell below. A
    # line with a tab in it would be awkward to hand on, and another line of
    # the paragraph serves as well.
    keys = 0
    while (keys < 3) {
        line = ""
        for (i = 1; i <= bn; i++)
            if (index(B[i], "\t") == 0 && !(i in used) &&
                length(B[i]) > length(line)) {
                line = B[i]
                j = i
            }
        if (line == "")
            break
        used[j] = 1
        keys++
        K[keys] = line
    }
    for (i in used)
        delete used[i]
    if (keys == 0) {
        # Every line of it has a tab in it, so there is nothing to search main
        # with. Counted rather than dropped quietly: the note on standard
        # error says how many removals went unanswered.
        unsearchable++
        bn = 0
        return
    }
    reported++
    printf "%d\t%s\t%s\t%d\t%.2f\n", reported, file, oldfile, bstart, cov \
        > (OUT "/index")
    for (i = 1; i <= keys; i++)
        print K[i] > (OUT "/cand-" reported ".key")
    close(OUT "/cand-" reported ".key")
    for (i = 1; i <= bn; i++)
        print B[i] > (OUT "/cand-" reported ".txt")
    close(OUT "/cand-" reported ".txt")
    bn = 0
}

# A hunk is read whole, because whether it adds anything decides how its
# removals are read, and that is only known at its end.
function endhunk(   i, c, body) {
    if (!hn) {
        inhunk = 0
        return
    }
    pure = 1
    for (i = 1; i <= hn; i++)
        if (substr(H[i], 1, 1) == "+")
            pure = 0
    oldline = oldstart
    bn = 0
    for (i = 1; i <= hn; i++) {
        c = substr(H[i], 1, 1)
        body = substr(H[i], 2)
        if (c == "-") {
            if (trim(body) == "")
                flush(pure)
            else if (starts_item(body) && bn > 0)
                flush(pure)
            if (trim(body) != "") {
                bn++
                B[bn] = body
                if (bn == 1)
                    bstart = oldline
            }
            oldline++
            continue
        }
        flush(pure)
        if (c == " ")
            oldline++
    }
    flush(pure)
    inhunk = 0
    hn = 0
}

# --- the kept text ---
FNR == NR {
    if (substr($0, 1, 1) == ESC) {
        bucket = substr($0, 2)
        qn = 0
        next
    }
    keptlines++
    feed(bucket, $0)
    next
}

# --- the diff ---
#
# A hunk is counted out by the line counts in its own header, so a line of
# content inside it can never be read as a header of the next file: a man page
# that quotes a diff has lines starting --- and +++ in it, and this rule comes
# first.
inhunk {
    hn++
    H[hn] = $0
    c = substr($0, 1, 1)
    if (c == " ") {
        remold--
        remnew--
    } else if (c == "-") {
        remold--
    } else if (c == "+") {
        remnew--
    }
    if (remold <= 0 && remnew <= 0)
        endhunk()
    next
}
substr($0, 1, 6) == "--- a/" {
    # The path as the merge base had it, which is the path main must be
    # searched along: a renamed file does not exist there under its new name.
    oldfile = substr($0, 7)
    next
}
substr($0, 1, 5) == "--- /" {
    oldfile = ""
    next
}
substr($0, 1, 6) == "+++ b/" {
    file = substr($0, 7)
    if (oldfile == "")
        oldfile = file
    next
}
substr($0, 1, 5) == "+++ /" {
    # +++ /dev/null: the file is deleted, and its text belongs to the path it
    # had.
    file = oldfile
    next
}
substr($0, 1, 2) == "@@" {
    n = split($2, bounds, ",")
    oldstart = substr(bounds[1], 2) + 0
    remold = (n > 1) ? bounds[2] + 0 : 1
    n = split($3, bounds, ",")
    remnew = (n > 1) ? bounds[2] + 0 : 1
    hn = 0
    inhunk = 1
    next
}

END {
    endhunk()
    printf "%d %d %d %d\n", keptlines + 0, blocks + 0, reported + 0, \
        unsearchable + 0 > (OUT "/summary")
}
' "$work/kept" "$work/diff" || {
    echo "error: the comparison itself failed" >&2
    exit 1
}

read -r keptlines examined candidates unsearchable < "$work/summary" || {
    echo "error: the comparison wrote no summary" >&2
    exit 1
}

# Two floors, so that a pathspec, a marker or a diff format that has stopped
# matching cannot turn this into a comparison of nothing that passes: changed
# files that exist at HEAD must have given the comparison text to match
# against, and a diff that removes prose must have given it blocks to read.
if [ "$present" -gt 0 ] && [ "$keptlines" -eq 0 ]; then
    printf 'error: %s changed file(s) are in the tree but the comparison\n' \
        "$present" >&2
    echo "       read no text out of them, which cannot be right: this script" >&2
    echo "       has stopped reaching the files." >&2
    exit 1
fi
removed_lines=$(grep -c '^-[^-]*[A-Za-z0-9]' "$work/diff")
if [ "$removed_lines" -gt 0 ] && [ "$examined" -eq 0 ]; then
    printf 'error: the diff removes %s line(s) of prose but the\n' \
        "$removed_lines" >&2
    echo "       comparison read no removed block out of it, which cannot be" >&2
    echo "       right: this script has stopped reading the diff." >&2
    exit 1
fi

report="$work/report"
: > "$report"
reported=0
aged=0
excused=0
unattributed=0

if [ "$candidates" -gt 0 ]; then
    while IFS="$tab" read -r n file oldfile line coverage; do
        # Which commit gave main this text, and how far back that was. -S
        # counts occurrences of the string, and the search is along the merge
        # base's own history, so the answer is the commit that put it there.
        # Several lines of the paragraph are asked, and the oldest answer
        # wins: an edit to one line since -- a typo, a re-wrap -- must not
        # date the whole paragraph from that edit.
        case "$line" in
        ''|*[!0-9]*)
            echo "error: the comparison wrote an index line this script" >&2
            printf "       cannot read, so it has a bug: '%s %s %s'\n" \
                "$n" "$file" "$line" >&2
            exit 1
            ;;
        esac
        adder=
        merges=
        while IFS= read -r key; do
            [ -n "$key" ] || continue
            # --follow, so a rename on main does not date the whole page from
            # the rename; :(literal), so a bracket or an asterisk in the path
            # is a character rather than a pattern that matches nothing.
            was=$(git log -1 --format=%H --follow -S"$key" "$base" \
                -- ":(literal)$oldfile")
            [ -n "$was" ] || continue
            back=$(git rev-list --count --first-parent "$was..$base") || exit 1
            if [ -z "$merges" ] || [ "$back" -gt "$merges" ]; then
                merges=$back
                adder=$was
            fi
        done < "$work/cand-$n.key"
        if [ -z "$adder" ]; then
            # Text that cannot be attributed to a commit on main is not
            # reported: without the commit there is no telling whether it is
            # recent, and the message would have nothing to point at.
            unattributed=$((unattributed + 1))
            continue
        fi
        if [ "$merges" -gt "$WINDOW" ]; then
            aged=$((aged + 1))
            continue
        fi

        # The way out for a deliberate removal, named in the message below: a
        # trailer in a commit on the branch. It says in the branch's own
        # history, and so in the pull request, that the author meant to take
        # recent text out of that file.
        if git log --format=%B "$base..$head" |
                sed -n 's/^[Dd]rops-merged-text:[[:space:]]*//p' |
                grep -qxF "$file"; then
            excused=$((excused + 1))
            continue
        fi

        case "$merges" in
        0) when="with the very commit this branch sits on" ;;
        1) when="one merge before the merge base" ;;
        *) when="$merges merges before the merge base" ;;
        esac

        reported=$((reported + 1))
        {
            printf '\n  %s:%s (the line it was on at %s)\n' \
                "$file" "$line" "$short_base"
            printf '    this wording arrived on main %s,\n' "$when"
            printf '    in %s "%s".\n' \
                "$(git rev-parse --short "$adder")" \
                "$(git log -1 --format=%s "$adder")"
            printf '    %s%% of its wording is left in that file or' \
                "$(printf '%s' "$coverage" | awk '{ printf "%d", $1 * 100 }')"
            printf ' anywhere this branch adds:\n\n'
            # The escape is dropped so that a page carrying terminal control
            # sequences cannot write them into this report.
            LC_ALL=C tr -d '\033' < "$work/cand-$n.txt" | sed 's/^/      /'
            # The path is quoted, because it is printed as a command for the
            # reader to paste and a path is not this script's to vouch for.
            printf '\n    What that commit added to the file:\n\n'
            printf "        git show %s -- '%s'\n" \
                "$(git rev-parse --short "$adder")" \
                "$(printf '%s' "$oldfile" | sed "s/'/'\\\\''/g")"
        } >> "$report"
    done < "$work/index"
fi

if [ "$reported" -eq 0 ]; then
    printf '%s: %s file(s) compared with %s, %s removed block(s) read' \
        "$self" "$compared" "$short_base" "$examined"
    if [ "$excused" -gt 0 ]; then
        printf ', %s excused by a Drops-merged-text trailer' "$excused"
    fi
    printf ', nothing main gained is missing.\n'
    # Said out loud rather than kept in the script: these are the removals the
    # window and the attribution let through, and a reader wondering why a
    # loss was not reported should see that there were candidates at all.
    if [ "$aged" -gt 0 ] || [ "$unattributed" -gt 0 ] ||
            [ "$unsearchable" -gt 0 ]; then
        {
            printf 'note: %s removed paragraph(s) are gone from their page\n' \
                "$((aged + unattributed + unsearchable))"
            printf '      too, but main gained them more than %s merge(s)\n' \
                "$WINDOW"
            printf '      before the merge base, or not in a commit this\n'
            printf '      comparison could name, so they are the ordinary\n'
            printf '      rewriting of a page as far as this can tell.\n'
            printf '      --window reaches further back.\n'
        } >&2
    fi
    exit 0
fi

{
    echo "error: this branch removes documentation main had just gained:"
    cat "$report"
    cat <<EOF

A rebase resolution that keeps the branch's side of a file takes the other
side's work out with it, and nothing here reads prose, so the loss merges
green. Read the paragraph out of the commit named above, put it back where it
was, and keep your own change: in a conflict, both sides go in.

If a removal is deliberate -- the paragraph is being rewritten elsewhere, or
the change it describes is being reverted -- put a trailer in the commit
message that removes it, and say why in the pull request:

    Drops-merged-text: <path of the file>

Any commit on the branch will do, and it excuses every removal from that file,
so it names the path and no line number.

What is read, and what is not, is in the comment at the top of $self.
--window changes how recently main must have gained the text to be reported.
EOF
} >&2
exit 1
