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
# host/doc/ubertooth-btle.md: three paragraphs two pull requests had added hours
# earlier went out with one rebase resolution and were found by accident half a
# day later.
#
# So this compares the branch with its merge base and asks, of every paragraph
# the branch removes: is it text main had only just gained, and is it gone? In
# full, a removed paragraph is reported when all four hold.
#
#   1. It is in a hunk that removes lines and adds none. A paragraph replaced
#      where it stood is the author editing their own page, not a resolution
#      dropping someone else's.
#   2. It is at least MIN_WORDS words long, so a removed heading or list item
#      is not read as a lost paragraph.
#   3. Under COVERAGE_MAX of its wording survives in that file as the branch
#      leaves it, or anywhere the branch adds text. The comparison is by
#      SHINGLE-word runs, which follow prose through re-wrapping and small
#      edits, and the "anywhere the branch adds" half is what lets a paragraph
#      move to another page, or into a renamed file, without being reported.
#   4. main gained it within WINDOW merges of the merge base -- text that
#      landed while the branch was in flight, which is the only text a rebase
#      resolution can drop. Older text in the same shape is the ordinary
#      rewriting of a page: the author had it in front of them and chose.
#
# The fourth test is what makes the first three usable. Measured over three
# days of this repository, 167 merged pull requests, the first three tests alone
# report nine removed paragraphs: the three real ones and six rewrites of
# paragraphs between five hours and a day old. With the window at its default
# the same run reports the three and nothing else.
#
# Scope: *.md, *.rst and *.txt. host/doc/man/*.1 is generated from the matching
# host/doc/*.md and follows it, so the roff pages are not read -- a loss shows
# in the source page. Code is not read either, and that is a decision rather
# than an omission: the same four tests over the C, header, test and build files
# of those three days report 78 removed blocks, every one of them a rename, a
# refactor or a revert, because moving a function and dropping one look alike
# in a diff. A build and a test suite watch code; this watches what they do
# not.
#
# What it cannot see, so the next reader knows:
#   - A branch that has not been rebased past the text yet. Its merge base is
#     older than the text, so the text is not in the branch's diff at all. CI
#     runs this on every push, and the rebase before a pull request is opened
#     or merged is the push that matters.
#   - Text dropped by a `git merge` of main into the branch rather than a
#     rebase, if the merge commit is the one that resolved it: this reads the
#     branch's diff against the merge base, and a merge commit's resolution is
#     part of that diff only where it differs from main. (The rule in
#     .kiro/steering is to rebase.)
#   - A paragraph main gained longer ago than the window, and a single removed
#     sentence under the word floor.
#
# Exits 1 and reports each removed paragraph on standard error, 0 and one line
# on standard output otherwise. CI runs this same script, so what fails there
# fails here.

set -u

self=$(basename "$0")

# How far back a paragraph may have arrived on main, in first-parent commits
# from the commit that added it to the merge base. main is a line of squash
# merges, so this counts merges. Five is about a couple of hours at this
# repository's pace, and the nearest ordinary rewrite in the three days
# measured above was seventeen merges.
WINDOW=5

# A removed paragraph is gone when under half its wording is left. Prose
# re-wrapped and lightly edited keeps most of its runs; a different paragraph
# on the same subject keeps few.
COVERAGE_MAX=0.5

# Length of the word runs compared, and the shortest removed block worth
# reading. Both come from the audit that found the one known loss: eight-word
# runs follow re-wrapped Markdown, and twenty-five words is a paragraph rather
# than a heading or a list item.
SHINGLE=8
MIN_WORDS=25

usage() {
    cat <<EOF
usage: $self [--main <ref>] [--window <merges>]

Reports paragraphs this branch removes that main had gained within <merges>
merges of the branch's merge base, and whose wording the branch does not keep.

  --main <ref>       compare against this ref (default: the first of
                     origin/main, squeaky/main, main that exists)
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
            echo "error: --window takes a count of merges, not '$2'" >&2
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
        echo "error: unknown argument '$1'" >&2
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

# main by name rather than by hash: the question is what main has now, not
# what the clone was left at. A named ref that does not resolve is an error
# rather than a quiet comparison with something else, because every way of
# reading nothing here would otherwise pass.
if [ -n "$main_ref" ]; then
    if ! git rev-parse --verify --quiet "$main_ref^{commit}" > /dev/null; then
        echo "error: '$main_ref' is not a commit in this checkout" >&2
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
    echo "error: HEAD and $main_ref have no common ancestor" >&2
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

# The pathspecs, in one place, used by each pass below. host/doc/man is left
# out by extension: it holds only the generated roff pages.
diff_prose() {
    git diff --no-color "$1" "$base" "$head" -- '*.md' '*.rst' '*.txt'
}

changed=$(git diff --name-only "$base" "$head" -- '*.md' '*.rst' '*.txt')
compared=$(printf '%s\n' "$changed" | grep -c .)
if [ "$compared" -eq 0 ]; then
    echo "$self: no .md, .rst or .txt file changed since $short_base."
    exit 0
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

# What counts as kept: each changed file as the branch leaves it, and every
# line the branch adds to any of these files. The second half is what lets
# text move between pages. Each file's own text is kept apart from the others
# because the same paragraph appears on several man pages here, and a page
# losing it while its neighbors keep it is exactly the loss being looked for.
#
# The escape character marks the start of a file's text. A prose file does not
# contain one, and a path cannot. The marker for the added lines is always
# written, so this file is never empty -- which matters, because the reading
# pass below tells its two inputs apart by FNR == NR.
present=0
{
    printf '%s\n' "$changed" | while IFS= read -r file; do
        [ -n "$file" ] || continue
        if git cat-file -e "$head:$file" 2> /dev/null; then
            printf '\033%s\n' "$file"
            git show "$head:$file"
        fi
    done
    printf '\033+added\n'
    diff_prose -U0 |
        awk '/^\+\+\+/ { next } /^\+/ { print substr($0, 2) }'
} > "$work/kept"
for file in $changed; do
    if git cat-file -e "$head:$file" 2> /dev/null; then
        present=$((present + 1))
    fi
done

diff_prose -U3 > "$work/diff"

# The reading pass. First input is the kept text, second the diff; it writes
# one index line per reported paragraph, plus the paragraph itself and the
# line to search main's history with, in files of their own. Keeping the text
# out of the index means no path or sentence has to survive a round trip
# through field splitting.
awk -v SH="$SHINGLE" -v MINW="$MIN_WORDS" -v COVER="$COVERAGE_MAX" \
    -v OUT="$work" '
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

# Decide on one run of removed lines, and report it if all the content tests
# hold. pure says whether the hunk it came from added nothing.
function flush(pure,   i, j, text, n, arr, run, total, hit, cov, longest) {
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
    # The longest removed line is what main is searched for: long enough to be
    # that paragraph and nothing else, and a substring of the file as it was,
    # which is what git log -S wants. A line with a tab in it would be awkward
    # to hand on, and another line of the paragraph serves as well.
    longest = ""
    for (i = 1; i <= bn; i++)
        if (index(B[i], "\t") == 0 && length(B[i]) > length(longest))
            longest = B[i]
    if (longest == "") {
        bn = 0
        return
    }
    reported++
    printf "%d\t%s\t%d\t%.2f\n", reported, file, bstart, cov > (OUT "/index")
    print longest > (OUT "/cand-" reported ".key")
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
            if (trim(body) != "") {
                bn++
                B[bn] = body
                if (bn == 1)
                    bstart = oldline
            } else {
                flush(pure)
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
    if (substr($0, 1, 1) == "\033") {
        bucket = substr($0, 2)
        qn = 0
        next
    }
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
    oldpath = substr($0, 7)
    next
}
substr($0, 1, 6) == "+++ b/" {
    file = substr($0, 7)
    next
}
substr($0, 1, 5) == "+++ /" {
    # +++ /dev/null: the file is deleted, and the removed text belongs to the
    # path it had.
    file = oldpath
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
    if (remold <= 0 && remnew <= 0)
        inhunk = 0
    next
}

END {
    endhunk()
    printf "%d %d %d\n", shingles + 0, blocks + 0, reported + 0 > (OUT "/summary")
}
' "$work/kept" "$work/diff" || {
    echo "error: the comparison itself failed" >&2
    exit 1
}

read -r shingles examined candidates < "$work/summary" || {
    echo "error: the comparison wrote no summary" >&2
    exit 1
}

# A floor, so a pathspec or a marker that has stopped matching cannot turn this
# into a comparison of nothing that passes. Changed prose files that exist at
# HEAD must have given the comparison some text to match against.
if [ "$present" -gt 0 ] && [ "$shingles" -eq 0 ]; then
    echo "error: $present changed file(s) are in the tree but the comparison" >&2
    echo "       read no text out of them, which cannot be right: this script" >&2
    echo "       has stopped reaching the files." >&2
    exit 1
fi

tab=$(printf '\t')
report="$work/report"
: > "$report"
reported=0
aged=0
excused=0
unattributed=0

if [ "$candidates" -gt 0 ]; then
    while IFS="$tab" read -r n file line coverage; do
        key=$(cat "$work/cand-$n.key")

        # Which commit gave main this text. -S counts occurrences of the
        # string, and the search is along the merge base's own history, so the
        # answer is the commit that put it there.
        adder=$(git log -1 --format=%H -S"$key" "$base" -- "$file")
        if [ -z "$adder" ]; then
            # Text that cannot be attributed to a commit on main is not
            # reported: without the commit there is no telling whether it is
            # recent, and the message would have nothing to point at.
            unattributed=$((unattributed + 1))
            continue
        fi
        merges=$(git rev-list --count --first-parent "$adder..$base") || exit 1
        if [ "$merges" -gt "$WINDOW" ]; then
            aged=$((aged + 1))
            continue
        fi

        # The way out for a deliberate removal, named in the message below: a
        # trailer in the commit that does it. It says in the branch's own
        # history, and so in the pull request, that the author meant to take
        # recent text out.
        if git log --format=%B "$base..$head" |
                sed -n 's/^[Dd]rops-merged-text:[[:space:]]*//p' |
                grep -qxF "$file"; then
            excused=$((excused + 1))
            continue
        fi

        case "$merges" in
        0) when="with the very commit this branch sits on" ;;
        1) when="one merge before this branch's merge base" ;;
        *) when="$merges merges before this branch's merge base" ;;
        esac

        reported=$((reported + 1))
        {
            printf '\n  %s:%s\n' "$file" "$line"
            printf '    main gained this paragraph %s,\n' "$when"
            printf '    in %s "%s".\n' \
                "$(git rev-parse --short "$adder")" \
                "$(git log -1 --format=%s "$adder")"
            printf '    %s%% of its wording is left in that file or' \
                "$(printf '%s' "$coverage" | awk '{ printf "%d", $1 * 100 }')"
            printf ' anywhere this branch adds:\n\n'
            sed 's/^/      /' "$work/cand-$n.txt"
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
    if [ "$aged" -gt 0 ] || [ "$unattributed" -gt 0 ]; then
        {
            printf 'note: %s removed paragraph(s) are gone from their page\n' \
                "$((aged + unattributed))"
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
green. Keep main's text and put your change on top of it: read what main added
to the file, then rebase again and take BOTH sides in every conflict -- main's
paragraphs and your change.

    git log -p $short_base..$main_ref -- <the file>
    git rebase $main_ref

If a removal is deliberate -- the paragraph is being rewritten, or the change
it describes is being reverted -- put a trailer in the commit message that
removes it, and say why in the pull request:

    Drops-merged-text: <path of the file>

What is read, and what is not, is in the comment at the top of $self.
--window changes how recently main must have gained the text to be reported.
EOF
} >&2
exit 1
