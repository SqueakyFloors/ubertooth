#!/bin/sh
# Fail when a workflow names its own libbtbb instead of using the pinned one.
#
# The libbtbb this fork builds against is pinned in build.yml's env, and the
# libbtbb-pin workflow requires every file that writes that pin out to name
# the same repository and the same commit. That says nothing about a file
# naming a DIFFERENT libbtbb, because such a file is not claiming to use the
# pin at all -- and that is the gap that let the Man pages job fetch upstream
# with no ref. Some functions the host tools call exist only in the fork, so
# configuring the host tree against another libbtbb fails outright, and every
# change under host/doc was blocked until someone read the log.
#
# So: no workflow file may name a libbtbb repository literally except the few
# that have a reason to, and what those name has to be the pinned repository.
# Any other job that wants the library reads the pin out of build.yml's env
# rather than writing a repository of its own.
#
# Run it from anywhere in a checkout:
#
#     tools/check-workflow-libbtbb.sh [tree]
#
# tree is the checkout to read, and defaults to the checkout the working
# directory is in. The self-test passes a copy with a workflow broken on
# purpose.
#
# It reports every match as file:line: text on standard error and exits 1, or
# says on standard output what it scanned and exits 0. CI runs this same
# script, so what fails there fails here.
#
# What it does not catch, so the next reader knows. A libbtbb fork under
# another name: the pattern below is a repository called libbtbb, which is
# what a job reaching for the library writes, not a proof that no other
# library can arrive. And a file outside .github/workflows, a composite
# action under .github/actions among them. The tree has none today, and
# widening the scan to all of .github would put the issue templates and the
# dependabot configuration under a rule about fetching a library, where a
# repository named in prose would be a red about nothing.

set -u

# Byte order and byte matching, so the owner and repository patterns below are
# about bytes rather than about what the runner thinks a letter is.
LC_ALL=C
export LC_ALL

usage() {
    cat <<'EOF'
usage: tools/check-workflow-libbtbb.sh [tree]

Check that no workflow file names a libbtbb repository literally except the
ones that have a reason to, and that those name the libbtbb pinned in
.github/workflows/build.yml.

tree is the checkout to read, and defaults to the checkout the working
directory is in.
EOF
}

tree=
end_of_options=no
while [ $# -gt 0 ]; do
    if [ "$end_of_options" = no ]; then
        case $1 in
        -h | --help)
            usage
            exit 0
            ;;
        --)
            # So a tree whose name begins with a dash can still be named.
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
    if [ -n "$tree" ]; then
        printf "error: one tree at most, and '%s' is a second\n" "$1" >&2
        exit 1
    fi
    tree=$1
    shift
done

if [ -z "$tree" ]; then
    tree=$(git rev-parse --show-toplevel 2> /dev/null) || {
        echo "error: not inside a git checkout, so there are no workflows to" >&2
        echo "       read. Name a tree: tools/check-workflow-libbtbb.sh <tree>" >&2
        exit 1
    }
fi
# Tested rather than left to cd, so a tree that is not there is an error line
# naming it rather than the shell's own message about a directory.
[ -d "$tree" ] || {
    printf "error: '%s' is not a directory, so there is no tree to read\n" "$tree" >&2
    exit 1
}
cd "$tree" || exit 1

workflows=.github/workflows
build=$workflows/build.yml

# The files that may name a libbtbb repository, and why.
#
# build.yml is where the pin is decided and where every build fetches it from.
# manpages.yml keeps a copy of it because a workflow cannot read another's
# env. The libbtbb-pin workflow's own list of files that must name the pin has
# both, so a third file with a reason to name a libbtbb goes in both places.
allowed="$build $workflows/manpages.yml"

[ -d "$workflows" ] || {
    printf 'error: %s is not a directory, so this scanned nothing\n' "$workflows" >&2
    exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

# The files to scan, and a floor on how many: a path that has stopped matching
# would otherwise turn this into a scan of nothing that passes.
find "$workflows" -type f \( -name '*.yml' -o -name '*.yaml' \) |
    sort > "$work/files" || exit 1
scanned=$(grep -c . "$work/files")
if [ "$scanned" -lt 2 ]; then
    printf 'error: only %s workflow file in %s, which cannot be right: this\n' \
        "$scanned" "$workflows" >&2
    printf '       scanned next to nothing and passed\n' >&2
    exit 1
fi

for f in $allowed; do
    [ -f "$f" ] || {
        printf 'error: %s is allowed to name a libbtbb repository but is not\n' "$f" >&2
        printf '       there, so that allowance names a file this tree does not\n' >&2
        printf '       have. Update the list in tools/check-workflow-libbtbb.sh.\n' >&2
        exit 1
    }
done

# The pin itself, read out of build.yml's env rather than repeated here, for
# the reason this whole check exists: a copy would be another thing to drift.
# The same sed the libbtbb-pin workflow uses, with trailing blanks and a
# carriage return taken off so a file saved on another platform still reads.
pinned=$(sed -n 's/^  LIBBTBB_REPO: *//p' "$build" | sed 's/[[:space:]]*$//')
pins=$(printf '%s\n' "$pinned" | grep -c .)
if [ "$pins" -ne 1 ]; then
    printf 'error: %s sets LIBBTBB_REPO %s times, not once, so there is no\n' \
        "$build" "$pins" >&2
    printf '       one pinned libbtbb to compare the others with\n' >&2
    exit 1
fi
# And it has to be an owner and a repository, the two things GitHub resolves,
# rather than a path or a URL. A malformed pin is worth refusing on its own,
# and it is also what lets the pinned name be taken out of a line below with
# no escaping: an owner is letters, digits and hyphens, so this form carries
# no character sed would read as a pattern.
if ! printf '%s\n' "$pinned" |
    grep -qE '^[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9._-]+$'; then
    printf "error: %s pins LIBBTBB_REPO to '%s', which is not an\n" \
        "$build" "$pinned" >&2
    printf '       owner and a repository such as someone/libbtbb\n' >&2
    exit 1
fi

# A libbtbb repository named literally: an owner and a repository called
# libbtbb, as "owner/libbtbb" is written in a checkout's repository: input or
# in a github.com URL.
#
# The owner is matched the way GitHub spells one, letters, digits and hyphens.
# A slash is allowed in front of it, so the github.com/owner/libbtbb form of a
# URL counts; an underscore, a dot, a dollar or a hyphen is not, which is what
# keeps the local paths these files already use from reading as repository
# names -- "$GITHUB_WORKSPACE/libbtbb" has an underscore before the only run
# of owner characters, and "${{github.workspace}}/libbtbb" has a brace, which
# is no owner character at all.
#
# What follows has to end the repository name, so "owner/libbtbb.git" counts
# and "$RUNNER_TEMP/libbtbb-pin.git" does not.
#
# The greps below match it with -i, because GitHub resolves a repository name
# whatever its case: someone/LibBTBB fetches the same library, and a guard
# that only knew the lowercase spelling would wave it through. -i costs
# nothing else here, since every other part of the pattern is a class holding
# both cases already.
#
# A directory path whose last two components are something/libbtbb would be
# read as a repository here. Nothing in these files writes one, and the forms
# above are how a workflow names the directory it cloned into.
reference='(^|[^A-Za-z0-9._$-])[A-Za-z0-9][A-Za-z0-9-]*/libbtbb([^A-Za-z0-9_-]|$)'

# One repository name out of a line, as the pattern finds it: the character
# the pattern needed in front and the one that ended the name are taken off
# again, leaving owner/libbtbb. The trailing strip is by character class
# rather than by name, so it works whatever the case of the name.
first_reference() {
    grep -oiE -e "$reference" |
        sed -e 's/^[^A-Za-z0-9]*//' -e 's/[^A-Za-z0-9_-]*$//' | sed -n '1p'
}

# One grep a file, and its status read, rather than one grep over the whole
# list through xargs: xargs reports a command that exited 1 as a failure of
# its own (123 with GNU xargs, 1 with the BSD one), so a scan that simply
# found nothing would be indistinguishable from a scan that went wrong -- and
# the two have opposite answers here. A handful of workflow files, so the
# extra processes cost nothing.
#
# grep's own errors are kept: a file listed but unreadable should be heard
# about, not scanned silently as nothing. -H, so the name is printed for a
# single file too.
: > "$work/hits"
while IFS= read -r file; do
    grep -n -H -iE -e "$reference" "$file" >> "$work/hits"
    scan=$?
    if [ "$scan" -gt 1 ]; then
        printf 'error: could not scan %s (grep exited %s)\n' "$file" "$scan" >&2
        exit 1
    fi
done < "$work/files"

status=0
found=0
: > "$work/reported"
while IFS= read -r hit; do
    file=${hit%%:*}
    rest=${hit#*:}
    line=${rest%%:*}
    text=${rest#*:}
    found=$((found + 1))
    case " $allowed " in
    *" $file "*)
        # An allowed file may name the pinned repository and nothing else.
        #
        # Decided by taking the pinned name out of the line and asking the
        # pattern again, rather than by reading the names out one at a time.
        # grep -o consumes the character that ended one match, and that is
        # the character the next match needs in front of it, so a second name
        # a single space away is never looked at -- which in an allowed file
        # would be a clean pass over a rogue one. Nothing is read out of the
        # line until a rogue name is known to be in it.
        #
        # The pinned name goes in unescaped, which is safe because it was
        # checked above to be an owner and a repository: no character in that
        # form means anything to sed. Taken out case-sensitively, so a
        # case-variant spelling of the pin is reported rather than accepted:
        # the libbtbb-pin check wants the one spelling in every file.
        rogue=$(printf '%s\n' "$text" | sed "s|$pinned|PINNED|g" |
            first_reference)
        [ -n "$rogue" ] || continue
        printf '%s:%s names %s, and the pinned libbtbb is %s\n' \
            "$file" "$line" "$rogue" "$pinned" >> "$work/reported"
        ;;
    *)
        # Not allowed to name one at all, so the first is enough to act on.
        printf '%s:%s names the libbtbb repository %s\n' \
            "$file" "$line" "$(printf '%s\n' "$text" | first_reference)" \
            >> "$work/reported"
        ;;
    esac
    status=1
done < "$work/hits"

# So this cannot pass by having stopped looking, which is how the gap it
# closes went unnoticed: the files allowed to name a libbtbb do name one, and
# if the pattern above ever stops finding theirs it says so rather than
# reporting a clean tree.
if [ "$found" -eq 0 ]; then
    printf 'error: no workflow file names a libbtbb repository at all, not even\n' >&2
    printf '       %s, so the pattern in\n' "$build" >&2
    printf '       tools/check-workflow-libbtbb.sh has stopped matching and this\n' >&2
    printf '       check is scanning for nothing\n' >&2
    exit 1
fi

if [ "$status" -eq 0 ]; then
    printf 'check-workflow-libbtbb: %s workflow files scanned, %s lines\n' \
        "$scanned" "$found"
    printf 'naming a libbtbb repository, all of them the pinned %s\n' "$pinned"
    printf 'and all of them in:\n'
    for f in $allowed; do
        printf '  %s\n' "$f"
    done
    exit 0
fi

{
    echo "error: a workflow names its own libbtbb:"
    sed 's/^/  /' "$work/reported"
    cat <<EOF

Only these files may name a libbtbb repository, and only the pinned one:

  $build, where the pin is decided
  $workflows/manpages.yml, which cannot read another workflow's env

A job in build.yml reads the pin from that file's own env, as every libbtbb
step there already does. A job in another workflow cannot read it, so either
read it at run time or keep a copy -- and a file that keeps one has to be
added to the list in libbtbb-pin.yml, which makes it agree with the pin, as
well as to the list in this script.

A job that names a libbtbb of its own drifts from the pin without the
libbtbb-pin check seeing it, because it is not claiming to use the pin. That
is how the Man pages job came to fetch upstream, which is missing functions
the host tools call, and block every documentation change.

If a match is a local path rather than a repository, write it the way the
other jobs do -- libbtbb/build, \$GITHUB_WORKSPACE/libbtbb. If it is a comment
rather than a command, this cannot tell the two apart: name the repository in
words, as "upstream libbtbb", or leave the owner off.
EOF
} >&2
exit 1
