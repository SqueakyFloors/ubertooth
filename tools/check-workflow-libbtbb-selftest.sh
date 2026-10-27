#!/bin/sh
# Check that tools/check-workflow-libbtbb.sh accepts and refuses the workflows
# it is meant to.
#
#     tools/check-workflow-libbtbb-selftest.sh
#
# The check it exercises closes the gap that let the Man pages job fetch
# upstream libbtbb and block every documentation change: the pin check could
# not see that job, because a file naming another libbtbb is not claiming to
# use the pin. It has to be right in both directions -- a job that writes its
# own repository must go red, and the local paths these workflows already use
# must not -- because a check that cries wolf gets ignored.
#
# Each case below copies this tree's .github/workflows into a throwaway
# directory, changes one thing, and asserts the status and the message. The
# copies are of the real files, so a case is about the workflows the fork
# actually has rather than about files written to suit it.
#
# Prints each case with its exit status and both output streams, so a failing
# run says what happened without a rerun. Exits 0 when every case holds, 1
# otherwise.

set -u

here=$(cd "$(dirname "$0")" && pwd) || exit 1
check="$here/check-workflow-libbtbb.sh"
top=$(git -C "$here" rev-parse --show-toplevel) || exit 1
workflows=.github/workflows
build=$workflows/build.yml
manpages=$workflows/manpages.yml

if [ ! -x "$check" ]; then
    printf '%s\n' "error: $check is missing or not executable" >&2
    exit 1
fi
for f in "$build" "$manpages"; do
    if [ ! -f "$top/$f" ]; then
        printf '%s\n' "error: $top/$f is not there, so the cases below would" >&2
        printf '%s\n' "       read a workflow this tree does not use" >&2
        exit 1
    fi
done

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

failures=0
case_name=
trees=0
tree=

fail() {
    printf '%s\n' "FAIL: $case_name: $*" >&2
    failures=$((failures + 1))
}

# A throwaway tree holding a copy of the real workflows.
new_tree() {
    trees=$((trees + 1))
    tree="$work/tree$trees"
    mkdir -p "$tree/$workflows"
    cp "$top/$workflows"/* "$tree/$workflows/" || exit 1
}

# A workflow file written into the copy, under a name no real one has.
add_workflow() {
    cat > "$tree/$workflows/$1"
}

# Rewrite one line of a copied workflow. The grep afterwards is the guard: a
# case whose pattern no longer matches would change nothing and then pass for
# the wrong reason, which is worse than a failure.
edit_line() {
    file="$tree/$workflows/$1"
    sed "s|$2|$3|" "$file" > "$file.new" || exit 1
    mv "$file.new" "$file"
    grep -qF -- "$3" "$file" || {
        printf '%s\n' "error: could not rewrite '$2' as '$3' in the copy of" >&2
        printf '%s\n' "       $workflows/$1: that line has changed shape" >&2
        exit 1
    }
}

# Run the check over one tree, keeping its status and both streams. No set -e:
# most cases are about a check that is meant to fail.
run() {
    case_name=$1
    shift
    printf '\n%s\n' "case: $case_name"
    printf '%s\n' "  tools/check-workflow-libbtbb.sh $*"
    "$check" "$@" > "$work/out" 2> "$work/err"
    status=$?
    printf '%s\n' "  exit status $status"
    sed 's/^/  stderr: /' "$work/err"
    sed 's/^/  stdout: /' "$work/out"
}

want_status() {
    [ "$status" -eq "$1" ] || fail "exit status $status, expected $1"
}

want_stdout() {
    grep -qF -- "$1" "$work/out" || fail "standard output does not mention '$1'"
}

want_stderr() {
    grep -qF -- "$1" "$work/err" || fail "standard error does not mention '$1'"
}

want_empty_stdout() {
    [ ! -s "$work/out" ] || fail "wrote to standard output and should not have"
}

want_empty_stderr() {
    [ ! -s "$work/err" ] || fail "wrote to standard error and should not have"
}

# The tree as it is. This is the case that says the two files allowed to name
# a libbtbb do name the pinned one, so the cases below are refusing something
# a clean tree does not have.
run "the tree as it is" "$top"
want_status 0
want_empty_stderr
want_stdout "check-workflow-libbtbb:"
want_stdout "SqueakyFloors/libbtbb"
want_stdout "$build"
want_stdout "$manpages"

# The shape that got past the pin check: a job that fetches upstream libbtbb.
# It names a repository, so the pin check had nothing to compare, and the host
# tree configured against upstream fails for want of functions it calls.
new_tree
add_workflow rogue.yml <<'EOF'
name: rogue
on: [push]
jobs:
  rogue:
    runs-on: ubuntu-24.04
    steps:
    - uses: actions/checkout@v4
      with:
        repository: 'greatscottgadgets/libbtbb'
        path: 'libbtbb'
EOF
run "a workflow fetches upstream libbtbb" "$tree"
want_status 1
want_empty_stdout
want_stderr "a workflow names its own libbtbb"
want_stderr "rogue.yml"
want_stderr "names the libbtbb repository greatscottgadgets/libbtbb"

# Any other libbtbb, not only upstream: the rule is that a job reads the pin,
# not that it avoids one particular fork.
new_tree
add_workflow rogue.yml <<'EOF'
name: rogue
on: [push]
jobs:
  rogue:
    runs-on: ubuntu-24.04
    steps:
    - run: git clone https://github.com/someone-else/libbtbb.git
EOF
run "a workflow clones some other libbtbb" "$tree"
want_status 1
want_stderr "names the libbtbb repository someone-else/libbtbb"

# The file that legitimately keeps a copy of the pin still has to keep the
# pinned one, which is what the pin check already says. Here it drifts.
new_tree
edit_line manpages.yml "LIBBTBB_REPO: SqueakyFloors/libbtbb" \
    "LIBBTBB_REPO: greatscottgadgets/libbtbb"
run "the pin-reading file names another libbtbb" "$tree"
want_status 1
want_stderr "manpages.yml"
want_stderr "and the pinned libbtbb is SqueakyFloors/libbtbb"

# A job that reads the pin passes, which is the whole point of the rule: there
# is a way to want the library that this check does not refuse.
new_tree
add_workflow reader.yml <<'EOF'
name: reader
on: [push]
jobs:
  reader:
    runs-on: ubuntu-24.04
    steps:
    - uses: actions/checkout@v4
    - run: |
        build=.github/workflows/build.yml
        repo=$(sed -n 's/^  LIBBTBB_REPO: *//p' "$build")
        git clone "https://github.com/$repo" libbtbb
EOF
run "a workflow that reads the pin" "$tree"
want_status 0
want_empty_stderr
want_stdout "check-workflow-libbtbb:"

# The local paths the real jobs already write are not repository names, in
# every form those files use. Each of these used to be the obvious false red
# of a pattern that looked for the word libbtbb after a slash.
new_tree
add_workflow paths.yml <<'EOF'
name: paths
on: [push]
jobs:
  paths:
    runs-on: ubuntu-24.04
    steps:
    - run: cmake -S libbtbb -B libbtbb/build
    - run: cmake "$GITHUB_WORKSPACE/libbtbb/" -DENABLE_PYTHON=OFF
    - run: cmake -E make_directory ${{github.workspace}}/libbtbb/build
    - run: git clone --bare "$RUNNER_TEMP/libbtbb-pin.git"
EOF
run "local libbtbb paths are not repository names" "$tree"
want_status 0
want_empty_stderr
want_stdout "check-workflow-libbtbb:"

# A .yaml is a workflow too, so the scan has to reach one.
new_tree
add_workflow rogue.yaml <<'EOF'
name: rogue
on: [push]
jobs:
  rogue:
    runs-on: ubuntu-24.04
    steps:
    - run: git clone https://github.com/greatscottgadgets/libbtbb
EOF
run "a .yaml workflow is scanned" "$tree"
want_status 1
want_stderr "rogue.yaml"

# So the check cannot pass by having stopped looking, which is how the gap it
# closes went unnoticed in the first place: with the pin written in a form the
# pattern does not find, there are no matches at all, and the floor refuses
# that rather than reporting a clean tree.
new_tree
edit_line build.yml "LIBBTBB_REPO: SqueakyFloors/libbtbb" \
    "LIBBTBB_REPO: SqueakyFloors/libbtbb-renamed"
edit_line manpages.yml "LIBBTBB_REPO: SqueakyFloors/libbtbb" \
    "LIBBTBB_REPO: SqueakyFloors/libbtbb-renamed"
run "nothing matches at all" "$tree"
want_status 1
want_empty_stdout
want_stderr "no workflow file names a libbtbb repository at all"

# The pin is read out of build.yml, so a build.yml that does not set it leaves
# nothing to compare the other files with. Failing beats comparing with an
# empty string, which every file would fail against for the wrong reason.
new_tree
edit_line build.yml "  LIBBTBB_REPO: SqueakyFloors/libbtbb" \
    "  LIBBTBB_REPOSITORY: SqueakyFloors/libbtbb"
run "build.yml sets no pin" "$tree"
want_status 1
want_empty_stdout
want_stderr "sets LIBBTBB_REPO 0 times, not once"

# A file allowed to name a libbtbb that is not there means the allowance names
# a file the tree does not have, which is a list to update rather than a tree
# to pass.
new_tree
rm -f "$tree/$manpages"
run "an allowed file is gone" "$tree"
want_status 1
want_empty_stdout
want_stderr "is allowed to name a libbtbb repository but is not"

# A workflows directory with next to nothing in it is a path that has stopped
# matching, not a clean tree.
new_tree
rm -f "$tree/$workflows"/*
cp "$top/$build" "$tree/$build"
run "almost no workflow files" "$tree"
want_status 1
want_empty_stdout
want_stderr "which cannot be right"

# The command line. Usage goes to standard output on -h and the exit status is
# 0, because printing it is what was asked for; a refused command line is an
# error on standard error.
run "the help text" -h
want_status 0
want_empty_stderr
want_stdout "usage: tools/check-workflow-libbtbb.sh"

run "an unknown option" --nope
want_status 1
want_empty_stdout
want_stderr "error: unknown option '--nope'"

run "two trees" "$top" "$top"
want_status 1
want_empty_stdout
want_stderr "one tree at most"

printf '\n'
if [ "$failures" -ne 0 ]; then
    printf '%s\n' "$failures case(s) failed" >&2
    exit 1
fi
printf '%s\n' \
    "check-workflow-libbtbb.sh refuses a workflow that names its own libbtbb" \
    "and passes the paths and the pin-reading the real jobs use."
