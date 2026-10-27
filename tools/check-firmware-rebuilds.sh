#!/bin/sh
# Check that changing a compile flag or a linker script rebuilds the firmware.
#
#     tools/check-firmware-rebuilds.sh
#
# The firmware makefiles record the flags they compile with and make every
# object depend on that record, and they make each link depend on the linker
# script files, so that editing either rebuilds what it affects instead of
# leaving the previous image in place with nothing said.
#
# firmware/tests/test_firmware_object_paths.c checks that the lines doing this
# are in the makefiles, by reading them as text. That catches a mechanism that
# goes missing; it cannot catch one that is there and wrong -- a compare the
# wrong way round, a record that differs on every run, a stamp that lands in
# the wrong direction -- which leaves the text intact and the objects stale.
#
# So this builds for real. It copies the firmware sources, builds an image,
# changes one thing, builds again, and looks at which outputs were rewritten.
# The firmware unit tests cannot: they compile with the host compiler, and
# this needs the ARM toolchain, which is why it lives here and runs in the CI
# job that installs one.
#
# Three images, for three shapes of makefile: blinky uses the shared one as
# most images do, the bootloader has its own, and assembly_test overrides
# which linker script to use. Per image: a first build compiles and links; a
# second with nothing changed rebuilds nothing; a different optimisation level
# recompiles the objects; touching the linker script, or the file it includes,
# relinks the image without recompiling anything.
#
# Two more legs then break the mechanism in the copy and assert the opposite,
# because a leg that would pass either way proves nothing: with the link's
# prerequisite on the script removed, touching the script must relink nothing;
# with the objects' dependency on the recorded flags removed, changing a flag
# must recompile nothing.
#
# make compares mtimes to the whole second (GNU make 3.81, which is what macOS
# ships), so a build that changes something in the same second as the previous
# build's output would see nothing to do. Every leg below therefore waits out
# the current second before it changes anything -- the `sleep 1` in mark().
# That is honest about the window rather than racing it.
#
# Exits 0 when every leg holds, 1 otherwise. Prints each leg as it runs, so a
# failing run says which one failed without a rerun.

set -u

self=$(basename "$0")
here=$(cd "$(dirname "$0")" && pwd) || exit 1
top=$(cd "$here/.." && pwd) || exit 1

usage() {
    cat <<EOF
usage: $self

Builds firmware images in a copy of the tree and checks that a compile-flag or
linker-script change rebuilds what it affects. Needs the ARM toolchain.
EOF
}

case ${1:-} in
    -h|--help)
        usage
        exit 0
        ;;
    "")
        ;;
    *)
        printf '%s\n' "error: $self takes no arguments" >&2
        exit 1
        ;;
esac

if ! command -v arm-none-eabi-gcc > /dev/null 2>&1; then
    printf '%s\n' \
        "error: arm-none-eabi-gcc is not on PATH, so no firmware can be built" >&2
    exit 1
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

tree="$work/tree"
failures=0

fail() {
    printf '%s\n' "FAIL: $*" >&2
    failures=$((failures + 1))
}

pass() {
    printf '%s\n' "ok: $*"
}

# A copy of the tracked firmware sources, which is what makes the result mean
# something: a build in the real tree would start from whatever objects are
# already there -- the first build would then rebuild nothing -- and would
# leave its own behind. The copy comes from git rather than cp -R for the same
# reason, since build output is ignored, not tracked.
#
# host/libubertooth/src comes along because the firmware includes the
# host/firmware interface header from it.
#
# Tracked means tracked: an edit to a tracked file is copied, but a file nobody
# has added yet is not there at all, so a run against a half-staged tree checks
# a tree nobody has. CI always runs this on a clean checkout.
if ! git -C "$top" ls-files firmware host/libubertooth/src > "$work/files"; then
    printf '%s\n' \
        "error: cannot list the tracked files under $top; this check needs a" \
        "       git checkout to copy the firmware sources out of" >&2
    exit 1
fi
# A handful of files cannot be the firmware: the listing worked but matched
# almost nothing, which a renamed directory would do. Without this the first
# symptom is make complaining about a missing file.
file_count=$(wc -l < "$work/files")
if [ "$file_count" -lt 100 ]; then
    printf '%s\n' \
        "error: only $file_count tracked files under firmware, which cannot be" \
        "       right; has one of the directories this check names moved?" >&2
    exit 1
fi
while IFS= read -r file; do
    mkdir -p "$tree/$(dirname "$file")" || exit 1
    if ! cp "$top/$file" "$tree/$file"; then
        printf '%s\n' \
            "error: $file is tracked but could not be copied, so the copy" \
            "       would not build what the tree describes" >&2
        exit 1
    fi
done < "$work/files"

# The copy is a repository of its own, because the build asks git for the
# revision it stamps into the image. Without one the build would take its
# "no git here" path and write that header once, and the legs below would
# never exercise the rewrite-only-when-it-changed handling a real build goes
# through -- a regression that rewrote the header every time would recompile
# everything for real and still pass here.
if ! (cd "$tree" && git init -q . && git add -A && \
      git -c user.name=check -c user.email=check@invalid \
          commit -q -m "firmware sources under test") > "$work/git.log" 2>&1; then
    printf '%s\n' "error: could not make the copy at $tree a git repository:" >&2
    sed 's/^/  /' "$work/git.log" >&2
    exit 1
fi

# The makefiles read the script's own INCLUDE lines, one level deep. A script
# that included a file which included another would leave that third file off
# every prerequisite list, silently, which is the bug this whole check exists
# for. Nothing in the tree nests them today; this says so out loud, so the day
# someone does the check fails here instead of years later.
nested=$(awk '$1 == "INCLUDE" { print FILENAME }' \
    /dev/null "$tree"/firmware/common/sections.ld | sed "s|^$tree/||")
if [ -n "$nested" ]; then
    printf '%s\n' \
        "error: $nested is included by a linker script and includes one" \
        "       itself; the makefiles only read one level, so a prerequisite" \
        "       would be missing" >&2
    exit 1
fi

# Wait out the current second, then plant a marker. Everything a build writes
# after this is newer than the marker and everything an earlier build wrote is
# older, so `find -newer` below says exactly what the next build rewrote. The
# wait is what makes that true where mtimes are whole seconds.
mark() {
    sleep 1
    touch "$work/marker" || exit 1
}

# The files matching $2 under the image directory $1 that were written since
# the marker.
rewritten() {
    find "$1" -name "$2" -newer "$work/marker"
}

# Build one image. $1 is its directory, anything after it is passed to make as
# a variable setting. A build that fails is not a leg failing: nothing can be
# concluded from it, so it stops the check and prints its output.
#
# DIRTY= holds the revision stamp still. The makefiles mark the revision they
# stamp into an image with a "*" when the checkout has uncommitted changes, and
# regenerate the version header when that stamp changes -- which recompiles
# whatever includes it. Several legs below edit a file in the copy, so without
# this the stamp would flip from clean to dirty under them and they would see a
# rebuild they did not ask for. Everything else about the version header is
# left alone, so a build still generates it, compares it and leaves it.
build() {
    build_dir=$1
    shift
    if ! make -C "$build_dir" elf DIRTY= "$@" > "$work/build.log" 2>&1; then
        printf '%s\n' "error: building $build_dir with 'make elf $*' failed:" >&2
        sed 's/^/  /' "$work/build.log" >&2
        exit 1
    fi
}

# Replace the one line of $1 that reads exactly $2 with $3. The count has to
# be one: an edit that found nothing would leave the mechanism in place, and
# the leg relying on it would pass for the wrong reason.
replace_line() {
    count=$(grep -cxF -- "$2" "$1") || count=0
    if [ "$count" -ne 1 ]; then
        printf '%s\n' \
            "error: expected exactly one line '$2' in $1, found $count" >&2
        exit 1
    fi
    awk -v want="$2" -v new="$3" '$0 == want { print new; next } { print }' \
        "$1" > "$1.edited" || exit 1
    mv "$1.edited" "$1" || exit 1
}

# Put a file in the copy back the way the tree has it.
restore() {
    cp "$top/$1" "$tree/$1" || exit 1
}

# The legs that every image's makefile has to pass. $1 is the image's
# directory, $2 the image it links, $3 its linker script and $4 the file that
# script includes; the paths are relative to firmware/, as they are written in
# the makefiles and reported below.
check_image() {
    name=$1
    elf=$2
    script="$tree/firmware/$3"
    included="$tree/firmware/$4"
    image="$tree/firmware/$1"
    script_name=$3
    included_name=$4

    mark
    build "$image"
    if [ -z "$(rewritten "$image" '*.o')" ] || [ -z "$(rewritten "$image" "$elf")" ]; then
        fail "$name: a first build produced no objects or no $elf, so the" \
            "legs below would have nothing to say"
        return
    fi
    pass "$name: a first build compiles and links"

    mark
    build "$image"
    if [ -n "$(rewritten "$image" '*.o')" ] || [ -n "$(rewritten "$image" "$elf")" ]; then
        fail "$name: a second build with nothing changed rebuilt:" \
            "$(rewritten "$image" '*.o') $(rewritten "$image" "$elf")"
    else
        pass "$name: a build with nothing changed rebuilds nothing"
    fi

    # A different optimisation level is a different compiler command line and
    # the same sources, which is the change make cannot see for itself.
    mark
    build "$image" OPT=1
    if [ -z "$(rewritten "$image" '*.o')" ]; then
        fail "$name: changing OPT recompiled nothing, so the objects in the" \
            "image were compiled with other flags"
    elif [ -z "$(rewritten "$image" "$elf")" ]; then
        fail "$name: changing OPT recompiled the objects but left $elf alone"
    else
        pass "$name: changing OPT recompiles the objects and relinks"
    fi

    # The script decides the memory map, so editing it has to relink -- and
    # only relink: it reaches no compile command line. OPT stays where the
    # previous leg left it, so the only change here is the script's mtime.
    mark
    touch "$script" || exit 1
    build "$image" OPT=1
    if [ -z "$(rewritten "$image" "$elf")" ]; then
        fail "$name: touching $script_name left the previous $elf in place"
    elif [ -n "$(rewritten "$image" '*.o')" ]; then
        fail "$name: touching $script_name recompiled objects, which no" \
            "linker script change needs"
    else
        pass "$name: touching $script_name relinks"
    fi

    # The same for the file the script pulls in with an INCLUDE line, which
    # the makefiles read out of the script rather than naming.
    mark
    touch "$included" || exit 1
    build "$image" OPT=1
    if [ -z "$(rewritten "$image" "$elf")" ]; then
        fail "$name: touching $included_name left the previous $elf in place"
    else
        pass "$name: touching $included_name relinks"
    fi
}

printf '%s\n' "Firmware rebuild checks, in a copy of the tree at $tree"

check_image blinky blinky.elf \
    common/LPC17xx_Linker_Script_with_bootloader.ld common/sections.ld

# The bootloader has its own makefile, because it is the one image written in
# C++, so the same mechanism is there in its own copy of these lines.
check_image bootloader bootloader.elf \
    common/LPC17xx_Linker_Script_bootloader.ld common/sections.ld

# assembly_test is the one image that chooses a different linker script, which
# it does before including the shared makefile. The resolution there is a
# snapshot taken as that makefile is read, so this is the image that shows an
# override reaching it -- not the one users flash, but the one that would catch
# a change breaking that order.
check_image assembly_test assembly_test.elf \
    common/LPC17xx_Linker_Script_without_bootloader.ld common/sections.ld

# The two legs that show the ones above are not vacuous, both against
# firmware/common.mk and the blinky image it builds.
#
# They run in this order, and the first keeps OPT where check_image left it,
# so that neither needs a build in between to settle: each changes exactly one
# thing and asserts that nothing comes of it.
printf '%s\n' "Now with the mechanism broken on purpose, nothing may rebuild"

common_mk=firmware/common.mk
# The quoted strings here and below are makefile text, matched and written
# literally: $(OBJ) and the rest are make's variables, not the shell's.
# shellcheck disable=SC2016
replace_line "$tree/$common_mk" '%.elf: $(OBJ) $(LINKER_SCRIPT_FILES)' '%.elf: $(OBJ)'
mark
touch "$tree/firmware/common/LPC17xx_Linker_Script_with_bootloader.ld" || exit 1
build "$tree/firmware/blinky" OPT=1
if [ -n "$(rewritten "$tree/firmware/blinky" blinky.elf)" ]; then
    fail "with the link's prerequisite on the linker script removed, touching" \
        "the script still relinked, so that leg would pass either way"
else
    pass "without the prerequisite, touching the linker script relinks nothing"
fi
restore "$common_mk"

# The flag record is rewritten whenever the flags change, whether or not the
# objects depend on it, so this leg has to watch the objects and not the image:
# the link's prerequisite on the script, restored above, relinks the image on
# its own after the touch in the leg before.
# shellcheck disable=SC2016 # makefile text, as above
replace_line "$tree/$common_mk" '$(OBJ): $(BUILD_FLAGS_FILE)' ''
mark
build "$tree/firmware/blinky"
if [ -n "$(rewritten "$tree/firmware/blinky" '*.o')" ]; then
    fail "with the objects' dependency on the recorded flags removed," \
        "changing OPT still recompiled, so that leg would pass either way"
else
    pass "without the dependency, changing OPT recompiles nothing"
fi
restore "$common_mk"

if [ "$failures" -ne 0 ]; then
    printf '%s\n' "error: $failures of these rebuild checks did not hold" >&2
    exit 1
fi
printf '%s\n' "Every rebuild check holds"
