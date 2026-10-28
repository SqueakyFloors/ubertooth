#!/bin/sh
# Fail when a branch claims a firmware API version main has already taken, or
# when a branch with no firmware change moves it at all.
#
# The rule is in .kiro/steering/branch-and-review.md, "Firmware API version".
# The host accepts firmware of exactly UBERTOOTH_API_VERSION, which both
# run-time images report as their USB bcdDevice, so a branch that changes the
# host-facing interface of either has to take a version main has not used.
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
# Either way, a value whose minor digits are 00 is a major version, which only
# a release may take: the release procedure reserves 0x0200 for the first fork
# release, and a board accepts only the exact value, so a major version on an
# ordinary branch refuses every user's board until it is reflashed. Both legs
# ask the same question about it (release_shaped below), so a typo is refused
# whether or not the branch also changed the firmware.
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
#     else. firmware/common.mk is half covered: the compile flags that decide
#     what the host sees are watched one by one (see watched_flags below), and
#     the rest of that file -- where object files go, the warning flags, the
#     rules -- is not.
#   - It calls changes interface changes that are not. Two kinds: a change to
#     one of the images' sources that the host never reads, the LED patterns,
#     the debug UART and tinyprintf among them; and code moved verbatim
#     between two watched files. Each is a false red on a gating job, and the
#     way out is the maintainer merging it red, not a version nobody needs;
#     the message below says so.
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
does not must leave it as the merge base had it. Neither may take a major
version (minor digits 00), which a release reserves; a release is the one
exception, and it takes the next major on its own.

main-ref is the commit to treat as main's tip, and defaults to the remote this
checkout tracks main from, then origin/main.

--list-watched prints the files a change to which asks for a new version, and
reads no commit, so it takes no main-ref. One of them, firmware/common.mk, is
read a line at a time: only the lines that set one of the compile flags listed
in this script count -- an assignment to one, a -D or -U flag naming one, or a
conditional on one -- and every other line of it is ignored.
EOF
}

# Run a git command over what goes into the two run-time images: the sources
# they compile, the shared code they link (firmware/common, lpcusb included),
# the linker script they are laid out with, their own Makefiles, which are
# little but the lists of those sources, and the shared build file that sets
# the compile flags. One list, so the check and --list-watched cannot drift
# apart.
#
# Both images, because both report this version as their bcdDevice and both
# answer the shared requests. firmware/btbr was left out while it reported a
# fixed 1.06 of its own, which this version said nothing about.
#
# Only files that are compiled or linked, that say which files are, or that
# say how they are compiled: a README under one of those directories cannot
# change what the host sees, and counting it would let a branch authorise its
# own bump by appending a line to one.
#
# The two linker scripts are named rather than matched, because firmware/common
# holds four and these images use two of them: common.mk's default,
# LPC17xx_Linker_Script_with_bootloader.ld, and the sections.ld it INCLUDEs.
# The other two belong to the bootloader and to assembly_test.
#
# Left out because it is in neither image: firmware/tests (host-compiled unit
# tests), firmware/bootloader (its bcdDevice is fixed and nothing checks it,
# because ubertooth-dfu has to work against any board), the development-only
# images (blinky, cc2400_test, clock_test, usb_test, assembly_test,
# tc13badge), firmware/Makefile (which picks which images to build), and
# documentation.
#
# firmware/common.mk is watched, but only a few lines of it. It is the shared
# build file every image includes and it carries the compile flags, so a change
# there can alter what the host sees with no source change at all -- and most of
# it cannot: its changes in main's recent history moved where object files are
# written and which shared source every image compiles in, neither of which a
# host can observe. So it is read through watched_flags below, which names the
# flags that move the interface and ignores every other line.
# firmware/ctags.mk, which it includes, is not watched at all: it builds editor
# tag files.
#
# Written as one pattern over whole paths, matched against a plain listing of
# the tree, rather than as git pathspecs. Pathspec globbing is not the same
# command to command -- the wildcards that select these files for `git diff`
# select nothing for `git ls-tree` -- and a list that silently comes back
# empty is the answer that asks for no bump.
#
# Every source arm reaches any depth, so none stops at a directory separator:
# firmware/common/ has to reach lpcusb/target/, where the shared USB sources
# live, and firmware/btbr/ keeps its sources in src/ and include/.
# firmware/bluetooth_rxtx/ holds no subdirectory today; were it to grow one,
# matching only its direct children would see the Makefile edit that
# introduced the subdirectory and nothing in it ever after, which is a silent
# pass rather than a visible red. The READMEs and the flash helper beside the
# sources are left out by the list of extensions, not by the depth, so
# reaching further costs nothing.
watched_pattern='^(firmware/(bluetooth_rxtx|btbr)/(.*\.(c|h|S|s|ld)|Makefile)|firmware/common/.*\.(c|h|S|s)|firmware/common/(LPC17xx_Linker_Script_with_bootloader|sections)\.ld|firmware/common\.mk|host/libubertooth/src/ubertooth_interface\.h)$'

# The one watched file that is read a line at a time rather than whole.
common_mk=firmware/common.mk

# The compile flags and make variables in firmware/common.mk that can move what
# the host sees: which control requests the board answers, what its replies look
# like, which modes it has. A change to a line that sets one of these asks for a
# new API version; every other line of the file is ignored.
#
# Each name below is matched as a whole name, and only where the line is
# really setting it. name_pattern below says which lines those are; in short,
# an assignment to that exact name, a -D or -U flag naming it, or a make
# conditional testing it. A line that merely uses the name -- the link command
# that passes $(LINKER_SCRIPT) to the linker, the recipe that echoes
# $(BOARD) -- does not count, and neither does a longer name that starts with
# a watched one, LINKER_SCRIPT_DIRS being the one that made this matter.
#
# A flag that is commented out still counts, because uncommenting
# -fpack-struct is the change rather than adding the text. Reindenting a line
# is not a change, because the text compared has its runs of blanks collapsed.
watched_flags() {
    # Lines beginning with # are the reasons; the rest are the names, which
    # come out with the indentation here taken off.
    grep -v '^[[:space:]]*#' <<'EOF' | collapse_blanks | grep .
    # -D$(BOARD), so -DUBERTOOTH_ONE or -DUBERTOOTH_ZERO. It picks the USB
    # vendor and product ID the image reports (firmware/bluetooth_rxtx/
    # ubertooth_usb.c), which is how the host finds a board at all, and the
    # BOARD_ID the host reads back with UBERTOOTH_GET_BOARD_ID
    # (firmware/common/ubertooth.h).
    BOARD
    # -DTX_ENABLE gates whole control requests: without it the board does not
    # answer UBERTOOTH_JAM_MODE and refuses the jamming modes of UBERTOOTH_EGO
    # (firmware/bluetooth_rxtx/bluetooth_rxtx.c). Which requests a board
    # answers is the thing this version pairs a host with.
    TX_ENABLE
    # The variable that decides whether -DTX_ENABLE is set, so it moves the
    # same interface under another name.
    DISABLE_TX
    # The variable that carries the board define and -DTX_ENABLE into the
    # compile. A -D added here reaches every firmware source, so a new
    # conditional interface can appear with neither name above changing.
    UBERTOOTH_OPTS
    # The second passthrough of -D options, which reaches both CDEFS and
    # CPPDEFS. Nothing in this file sets it: an image's own Makefile does, and
    # those are watched whole, so the entry is here for the day this file
    # gives it a default of its own.
    COMPILE_OPTS
    # The line that assembles the -D options for C sources, where the two
    # variables above arrive. A flag added straight to it is as visible to the
    # host as one added through them.
    CDEFS
    # The same line for C++ sources.
    CPPDEFS
    # Commented out today. It changes struct padding, and the packet structs in
    # ubertooth_interface.h are compiled by both sides, so turning it on in the
    # firmware alone would make the host read every reply at the wrong offsets.
    -fpack-struct
    # Commented out today. It changes how wide an enum is, which breaks the
    # same way: an enum that crosses the USB boundary would be a different size
    # on each side.
    -fshort-enums
    # -DLPC17xx: which chip's USB registers the shared lpcusb driver is built
    # for (firmware/common/lpcusb/target/usbhw_lpc.c). The wrong one and the
    # board does not enumerate.
    LPCUSB_TARGET
    # What puts host/libubertooth/src on the firmware's include path, so the
    # firmware and the host compile the same ubertooth_interface.h. Drop it and
    # the two can disagree about every request number.
    EXTRAINCDIRS
    # The default image layout. The with_bootloader script is what leaves the
    # DFU bootloader in place, so an image laid out without it cannot be
    # reflashed with ubertooth-dfu.
    LINKER_SCRIPT
    # Any -D option, wherever in the file it is written. The names above are
    # where the defines are set today, but a -D added straight to a CFLAGS line
    # reaches every firmware source just the same, and the file already does
    # that with -D__thumb2__=1.
    -D
    # The same for -U, which takes a define away again.
    -U
EOF
}

# Left off the list, because bumping the API version is a remedy only where a
# host and a firmware have to be paired:
#
#   - OPT and the warning flags. Optimisation changes how fast the firmware is,
#     not which requests it answers or what a reply looks like. A timing
#     regression shows up as fewer packets, and pairing the host with that
#     firmware does not help.
#   - The CPU and architecture flags (-mcpu, -march, -mthumb, -msoft-float,
#     -mfix-cortex-m3-ldrd, -mapcs-frame, -mthumb-interwork), -fno-builtin,
#     -fno-hosted, -ffunction-sections. Get one wrong and the image does not
#     run, and the host cannot misread a board that never answers.
#   - CSTANDARD. The C dialect does not decide the layout of the shared
#     structs; the processor's ABI does. -D__thumb2__=1 belongs here for the
#     same kind of reason, nothing in the tree reading it, but it is written
#     as a -D and so the -D entry above watches its line anyway. That is a
#     false red the entry is worth having, and a cheap one: the line has not
#     changed in main's history.
#   - LINKER_OPTS, the passthrough for linker options, which lay the image out
#     rather than define anything: the previous point covers it.
#   - OBJDIR, OUTDIR, FORMAT, per_image, DEBUG, the listing and depfile flags,
#     CROSS_COMPILE, DFU_TOOL, LPCISP, PROGDEV, the message strings and every
#     rule in the file: build and tooling plumbing.
#   - ADEFS and ASFLAGS, the -D options and flags for assembler sources. The
#     image has none (ASRC is empty), and a define added to either is caught by
#     the -D entry above in any case.
#   - OBJ, LST, DEPFILES and the shared sources added to them. A source added
#     to every image here can be host-facing -- the firmware info block
#     ubertooth-dfu reads arrived that way -- but the source itself is watched,
#     because it lives in firmware/common. Only dropping such a line while
#     leaving the file in the tree would pass unnoticed, and watching these
#     names would bring back the plumbing churn this narrowing is for: the
#     changes to this file in main's recent history move one of them.

# One line of text with its runs of blanks collapsed to one space and the ends
# trimmed, so reindenting a flag line is not a change to it.
collapse_blanks() {
    sed 's/[[:space:]][[:space:]]*/ /g; s/^ //; s/ *$//'
}

# One string with its extended-regular-expression metacharacters escaped, so a
# watched name is matched as the text it is. Nothing in the list needs it
# today; it is here so that adding a name with a dot or a plus in it cannot
# turn into a pattern that matches more than the name.
regex_escape() {
    printf '%s\n' "$1" | sed 's/[][\\^$.*+?(){}|]/\\&/g'
}

# One watched name, as the patterns that find a line of firmware/common.mk
# setting it. They come out one per line, which is how grep -E reads a list of
# patterns, so this is both what the filter below greps with and what names
# the flags a branch changed.
#
# A watched name is matched as a whole name, and only where the line sets it.
# Three shapes count:
#
#   - An assignment to that exact name: "LINKER_SCRIPT = x" and the other
#     operators make has -- "+=", ":=", "?=", "!=", "::=", ":::=" -- with
#     "override" or "export" allowed in front, and "define NAME" for the
#     multi-line form.
#   - A -D or -U flag naming it: "-DTX_ENABLE" and "-DTX_ENABLE=1" both count.
#   - A make conditional testing it: "ifeq ($(DISABLE_TX), )", "ifdef
#     DISABLE_TX". DISABLE_TX is assigned nowhere in the file -- the
#     conditional on it is what decides whether -DTX_ENABLE is compiled in --
#     so without this a branch could invert that line and take the jamming
#     requests away with nothing noticing.
#
# A leading # is allowed throughout, because a setting someone turned off is
# still a setting: uncommenting it is the change. Prose comments are dropped
# before this ever runs.
#
# What no longer counts, now that a name has to be set rather than mentioned,
# and what that costs:
#
#   - A line that only uses a watched variable. The link command passes
#     $(LINKER_SCRIPT) to the linker and the showtarget recipe echoes
#     $(BOARD); rewriting either used to be a false red on a gating job.
#     Five more lines of the file go with them, and four of those are the cost
#     of this: the two that carry $(CDEFS) and $(CPPDEFS) into the compile
#     flags, and the two that turn $(EXTRAINCDIRS) into the -I options.
#     Deleting one would take the defines or the shared interface header off a
#     compile command line with nothing noticing here, though not quietly: the
#     compile itself then fails. The fifth, BUILD_FLAGS_LINK, records the link
#     command so that changing it relinks, and costs nothing: it is a rebuild
#     mechanism rather than anything the host sees.
#   - A longer name that begins with a watched one. LINKER_SCRIPT_DIRS,
#     LINKER_SCRIPT_FOUND and LINKER_SCRIPT_FILES are three such names, and
#     matching the watched name as plain text anywhere on a line is what made
#     the check fail on a branch whose built image was byte-identical.
#
# Matching a flag is not quite matching a name. -D and -U take a macro name,
# attached or after a blank, so they are found wherever one follows; every
# other flag is matched whole, so -fpack-struct is not found inside
# -fpack-structure. Either way the flag has to start a word, so a -D inside a
# word -- "OBJDIR = build-Dir" -- is not one.
#
# What can begin the macro name after -D or -U: a letter, an underscore, a
# make expansion, or a quote around either, since -D"FOO=1" is a define as
# much as -DFOO=1 is.
define_start="[A-Za-z_\$\"']"

name_pattern() {
    pat=$(regex_escape "$1") || return 1
    case $1 in
    -D | -U)
        printf '(^|[^A-Za-z0-9_])%s[[:space:]]*%s\n' "$pat" "$define_start"
        ;;
    -*)
        printf '(^|[^A-Za-z0-9_])%s([^A-Za-z0-9_-]|$)\n' "$pat"
        ;;
    *)
        printf '^[[:space:]#]*((override|export)[[:space:]]+)*%s[[:space:]]*([+?!:]?|::|:::)=\n' "$pat"
        printf '^[[:space:]#]*((override|export)[[:space:]]+)*define[[:space:]]+%s([[:space:]]|$)\n' "$pat"
        printf '^[[:space:]#]*(else[[:space:]]+)?(ifeq|ifneq|ifdef|ifndef)([^A-Za-z0-9_].*)?[^A-Za-z0-9_]%s([^A-Za-z0-9_]|$)\n' "$pat"
        printf '(^|[^A-Za-z0-9_])-[DU]%s([^A-Za-z0-9_]|$)\n' "$pat"
        ;;
    esac
}

# Those patterns for every watched name, which is what the filter greps with.
# Written to a file so that one grep answers for the whole list.
name_patterns() {
    while IFS= read -r watched; do
        name_pattern "$watched" || return 1
    done < "$work/flags"
}

# The watched names whose lines differ in $work/mk.diff, so a message can say
# which settings a branch changed rather than only which lines. Each name is
# tested with its own patterns, so a LINKER_SCRIPT_DIRS line is not reported
# as a change to LINKER_SCRIPT.
changed_names() {
    sed 's/^[-+] //' "$work/mk.diff" > "$work/mk.lines" || return 1
    while IFS= read -r watched; do
        pattern=$(name_pattern "$watched") || return 1
        # -e, because a flag's pattern begins with a dash and grep would read
        # it as an option. The patterns come out one per line and grep reads
        # a newline in a pattern as the separator between two, so one -e
        # carries all of a name's shapes.
        if grep -qE -e "$pattern" "$work/mk.lines"; then
            printf '%s\n' "$watched"
        fi
    done < "$work/flags"
    return 0
}

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
    # firmware/common.mk is the one file read a line at a time: what counts as
    # its interface is the lines that set a watched flag, nothing else.
    if [ "$2" = "$common_mk" ]; then
        # Comments that are prose go first, comments that are a setting
        # someone turned off stay. The file tells the two apart by a blank
        # after the #: prose is written "# Place -D or -U options here", a
        # disabled setting "#CFLAGS += -fpack-struct". So uncommenting a flag
        # is still a change, and rewording a comment that happens to mention
        # one is not.
        #
        # grep exits 1 when nothing matched, and here that is an answer rather
        # than a failure: this commit sets none of the watched flags. Anything
        # above 1 is a real failure, an unreadable pattern file above all, and
        # must not pass as that answer: empty is what asks for no bump.
        printf '%s\n' "$text" | grep -v '^[[:space:]]*#[[:space:]]' |
            collapse_blanks | grep -E -f "$work/patterns"
        matched=$?
        [ "$matched" -le 1 ] || return 1
        return 0
    fi
    printf '%s\n' "$text" | strip_noninterface
}

# What the check sees change in firmware/common.mk between two commits, written
# to $work/mk.diff as the differing lines with a leading - and +.
#
# It diffs exactly the text interface_text compared, rather than looking each
# flag up again, so the message cannot name something other than what the
# decision was made on. Two watched lines swapped is the case where the two
# would part company: no flag's own setting changed, but the file the check
# reads did, and the diff says which lines.
common_mk_difference() {
    interface_text "$1" "$common_mk" > "$work/mk.a" || return 1
    interface_text "$2" "$common_mk" > "$work/mk.b" || return 1
    diff "$work/mk.a" "$work/mk.b" |
        sed -n 's/^< /- /p; s/^> /+ /p' > "$work/mk.diff"
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

# The watched flag names, written once, for the greps below to read.
watched_flags > "$work/flags" || exit 1
[ -s "$work/flags" ] || {
    echo "error: the watched flag list is empty, so a change to a compile flag" >&2
    echo "       in $common_mk would pass unnoticed" >&2
    exit 1
}

# And the patterns that find each of them being set, written once for the same
# reason. Empty is the answer that asks for no bump, so an empty pattern file
# is a failure rather than a pass.
name_patterns > "$work/patterns" || exit 1
[ -s "$work/patterns" ] || {
    echo "error: the watched flag list produced no patterns, so a change to a" >&2
    echo "       compile flag in $common_mk would pass unnoticed" >&2
    exit 1
}

# And a floor on what those patterns find, because having patterns is not the
# same as their matching anything. The build file does set these flags, so a
# pattern list that finds none of them has stopped reading the file -- and the
# filtered text would then be empty at both commits, compare equal, and ask
# for no bump. That is the one shape of wrongness this script is meant not to
# have, so it says so instead.
if git cat-file -e "$head_commit:$common_mk" 2> /dev/null; then
    interface_text "$head_commit" "$common_mk" > "$work/mk.head" || {
        printf 'error: could not read %s at HEAD\n' "$common_mk" >&2
        exit 1
    }
    [ -s "$work/mk.head" ] || {
        printf 'error: no line of %s at HEAD sets any of the watched compile\n' \
            "$common_mk" >&2
        echo "       flags, so a change to one of them would pass unnoticed: the" >&2
        echo "       patterns in tools/check-api-version.sh have stopped matching" >&2
        echo "       the file. tools/check-api-version.sh --list-watched prints" >&2
        echo "       the watched files." >&2
        exit 1
    }
fi

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

# When firmware/common.mk is one of the changed files, the lines of it that
# differ and the flags they set, so a message can name them rather than send
# the reader to diff a 600-line build file. Every differing line sets one of
# the watched flags, because that is what put it in the comparison.
flag_lines=
flag_names=
if grep -qxF "$common_mk" "$work/changed"; then
    common_mk_difference "$merge_base" "$head_commit" || {
        printf 'error: could not read %s to say which flags changed\n' "$common_mk" >&2
        exit 1
    }
    flag_lines=$(cat "$work/mk.diff")
    # No name matches only if a differing line sets no watched flag, which
    # cannot happen: that is what put the line in the comparison. The message
    # below falls back to the lines alone if it ever does.
    flag_names=$(changed_names | sort -u) || {
        printf 'error: could not read which flags changed in %s\n' "$common_mk" >&2
        exit 1
    }
fi

# What was compared, for the record: a log that only says "passed" cannot be
# read later to see which main the answer was about.
compared=$(printf 'compared with %s (%s), merge base %s, which had %s' \
    "$main_ref" "$(git rev-parse --short "$main_commit")" \
    "$(git rev-parse --short "$merge_base")" "$base_pretty")

# Whether this branch is the release that is allowed to take a major version.
# A release takes the next major with the minor digits at 00, to mark that its
# host tools and firmware only work with each other
# (docs/source/release_procedure.rst).
#
# Four conditions, and the branch has to meet all four: a single major up from
# the merge base, the minor digits at 00, above main rather than merely not
# below it, and the three documents that record the version moving with it,
# which a release changes and a slip of the finger does not.
#
# Above main, because a value equal to main's is the collision this whole
# check is about, and the release shape must not be a door around it: a
# release branch cut before another release landed, or re-cut after one, would
# otherwise claim a version whose firmware is already in the tree. The
# push-to-main leg would not catch it either, since there the merge base is
# main and an equal value leaves through the "unchanged" exit below. A release
# rebased on main has its merge base at main's tip, so the value it takes is
# above main's and this still holds.
#
# Both legs ask this, which is what makes them agree about a major version.
# Before they did, only the leg for a branch with no firmware change refused
# one: a firmware branch needed nothing but a value above main's, so 0x0200
# typed in place of 0x0135 passed, burned the value the first fork release
# reserves, and would have refused every user's board until it was reflashed.
release_doc_list=$(git --no-pager diff --name-only "$merge_base" "$head_commit" -- \
    CHANGELOG.md COMPATIBILITY.md TEST_PLAN.md) || {
    echo "error: could not tell whether this branch is a release" >&2
    exit 1
}
# grep -c exits 1 on a count of 0, so its status is not read here.
release_docs=$(printf '%s\n' "$release_doc_list" | sort -u | grep -c .)
release_shaped=no
if [ "$(( cur_decimal % 100 ))" -eq 0 ] &&
    [ "$(( cur_decimal / 100 ))" -eq "$(( base_decimal / 100 + 1 ))" ] &&
    [ "$cur_decimal" -gt "$main_decimal" ] &&
    [ "$release_docs" -eq 3 ]; then
    release_shaped=yes
fi

# Whether the next value after main's is a major version, which no ordinary
# branch may take: main at x.99 is the one place the two rules meet, and the
# suggestion below would otherwise name a value this check then refuses.
free_is_major=no
if [ "$main_decimal" -lt 9999 ] && [ "$(( (main_decimal + 1) % 100 ))" -eq 0 ]; then
    free_is_major=yes
fi

# What the two legs tell a branch that typed a major version it may not have.
# Written once, because a message that differs between the legs would read as
# two different rules.
say_major_reserved() {
    echo "       The minor digits 00 mark a release: a release takes the next"
    echo "       major version to say that its host tools and firmware only"
    echo "       work with each other, and docs/source/release_procedure.rst"
    echo "       reserves 0x0200 for the first fork release. A board is"
    echo "       accepted only if it reports exactly this version, so a major"
    echo "       version on an ordinary branch refuses every user's board"
    echo "       until it is reflashed."
    echo ""
    echo "       If this branch IS the release, it also moves CHANGELOG.md,"
    echo "       COMPATIBILITY.md and TEST_PLAN.md, and takes one major up"
    echo "       from its merge base."
    # Only where one exists. At main x.99 the next value is itself a major, so
    # there is none, and say_free_is_major below says so instead.
    if [ "$free_is_major" = no ]; then
        echo "       Otherwise take a value with minor digits of its own."
    fi
}

say_free_is_major() {
    echo "       $free_value is a major version, which only a release may take,"
    echo "       so there is no value free for an ordinary branch: main is at"
    echo "       $main_pretty and the cycle has used every minor digit. Ask the"
    echo "       maintainer, who decides between cutting the release and"
    echo "       renumbering."
}

if [ -n "$changed" ]; then
    if [ "$(( 0x$cur_digits ))" -gt "$(( 0x$main_digits ))" ] &&
        { [ "$(( cur_decimal % 100 ))" -ne 0 ] || [ "$release_shaped" = yes ]; }; then
        echo "check-api-version: this branch changes the firmware interface and"
        echo "takes $cur_pretty, above main's $main_pretty."
        echo "$compared."
        exit 0
    fi
    # A value above main's whose minor digits are 00, on a branch that is not
    # shaped like a release. It is the one refusal on this leg that is not
    # about the value being too low, so it gets its own message rather than a
    # "has to be above main" that the branch already satisfies.
    if [ "$(( 0x$cur_digits ))" -gt "$(( 0x$main_digits ))" ]; then
        {
            echo "error: this branch takes $cur_pretty, a major version that a"
            echo "       release reserves, and it is not shaped like a release."
            echo ""
            echo "         at HEAD:       $cur_pretty"
            echo "         at merge base: $base_pretty"
            echo "         on main:       $main_pretty"
            if [ "$free_is_major" = no ]; then
                echo "         next free:     $free_value"
            fi
            echo ""
            echo "       main here is $main_ref."
            echo ""
            say_major_reserved
            if [ "$free_is_major" = yes ]; then
                echo ""
                say_free_is_major
            fi
            # The same list the message below prints, because the same escape
            # applies: the branch may not need any bump at all, and a reader
            # told only that the value is reserved would take the next minor
            # instead of asking whether the red is real.
            echo ""
            echo "       The firmware interface files this branch changes:"
            printf '%s\n' "$changed" | sed 's/^/         /'
            echo ""
            echo "       If none of them change what the host sees, this is a"
            echo "       false red: put the version back to 0x$base_digits, say so on"
            echo "       the pull request, and let the maintainer merge with this"
            echo "       job red, rather than taking any new version."
            echo "       tools/check-api-version.sh --list-watched prints the list."
        } >&2
        exit 1
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
        if [ "$free_is_major" = yes ]; then
            say_free_is_major
            echo ""
        else
            echo "       Set it to $free_value in"
            echo "       $header, and move"
            echo "       CHANGELOG.md, COMPATIBILITY.md, TEST_PLAN.md and the pinned"
            echo "       strings in the tests with it."
            echo ""
        fi
        echo "       A board is accepted only if it reports exactly this version,"
        echo "       so reusing one main has already shipped lets tools built from"
        echo "       this branch accept a board carrying the older image."
        echo ""
        echo "       The firmware interface files this branch changes:"
        printf '%s\n' "$changed" | sed 's/^/         /'
        if [ -n "$flag_names" ]; then
            echo ""
            echo "       In $common_mk only the lines that set one of a few"
            echo "       compile flags are read, and this branch changes these:"
            printf '%s\n' "$flag_names" | sed 's/^/         /'
            echo ""
            echo "       The lines, as the check reads them:"
            printf '%s\n' "$flag_lines" | sed 's/^/         /'
            # The same line on both sides is a line that moved. Saying so
            # beats leaving the reader to wonder why a line is shown as both
            # removed and added.
            if [ -n "$(printf '%s\n' "$flag_lines" | sed 's/^[-+] //' |
                sort | uniq -d)" ]; then
                echo ""
                echo "       A line shown on both sides moved rather than changed."
            fi
        fi
        echo ""
        echo "       If none of them change what the host sees, this is a false"
        echo "       red. The usual reasons: the file is compiled into an image"
        echo "       but the host never reads it, such as the LED patterns or the"
        echo "       debug UART; the code was moved verbatim between two of"
        echo "       these files; or the line that changed in firmware/common.mk"
        echo "       sets a flag the host cannot observe. Say so on the pull"
        echo "       request and let the maintainer merge with this job red,"
        echo "       rather than taking a version nobody needs."
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
# version, whether or not the cycle changed the firmware. Without this the
# release's own pull request would be refused by a job it cannot satisfy. What
# counts as release-shaped, and why it stays narrow, is above.
if [ "$release_shaped" = yes ]; then
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
    if [ "$free_is_major" = yes ]; then
        say_free_is_major
        echo ""
    fi
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
