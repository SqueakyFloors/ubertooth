# Contributing

This is a fork of [Great Scott Gadgets' Ubertooth](https://github.com/greatscottgadgets/ubertooth).
Changes for upstream belong there; this repository takes changes to the fork.

Before you start:

- [REQUIREMENTS.md](REQUIREMENTS.md) lists what a build needs, and
  [README.md](README.md) has the install and build commands.
- [COMPATIBILITY.md](COMPATIBILITY.md) says which platforms, Python versions
  and libbtbb commit a change has to keep working.
- Host tests are `ctest` in the host build directory; firmware tests are
  `make -C firmware test`. Both run on a machine with no Ubertooth attached,
  and a change is expected to come with the tests that would have caught the
  bug it fixes.
- After editing a man page source in `host/doc/`, regenerate the committed
  roff page as README's Documentation section describes.
- Host tools and firmware are released, installed and flashed together, and
  the host refuses firmware of another API version. So an ordinary firmware
  change leaves `UBERTOOTH_API_VERSION` alone, and only a change that breaks
  older host tools -- a request removed or renumbered, a reply layout an older
  host would misread -- bumps it. A host-only change never touches it.

## License notices

[COPYING](COPYING) licenses the project. A source file does not repeat it: a
new non-test source file starts with one line naming the license, and nothing
else about it.

- C and C++: `/* SPDX-License-Identifier: GPL-2.0-or-later */` as line 1.
- Python and shell: `# SPDX-License-Identifier: GPL-2.0-or-later` as line 1,
  or line 2 where a `#!` line comes first.
- Test files -- anything under a `tests/` directory, or named `test_*` --
  carry no license notice at all.

Notices that came from upstream are kept exactly as they are, and so is any
notice naming a holder other than the fork. That includes a file upstream
wrote that was later renamed or moved here: the name changed, the notice does
not.

## The checks a pull request has to pass

Everything below runs from `.github/workflows/`. The table says what each
check is for; the paragraphs after it say which ones a given change runs.

| Workflow / job | What it is for |
| --- | --- |
| `Build` / `host` | Builds the host tools and runs the host test suite, a strict-C-standard build, and a smoke test of each binary |
| `Build` / `firmware` | Builds the ARM firmware images and the bootloader, runs the firmware test suite, and checks that a flag or linker-script change rebuilds the images. The ToorCon 13 badge (`firmware/tc13badge`, a Renesas R8C part) is not among the images built: nothing here has that toolchain, so the test suite compiles its C for syntax only and the rest of a change to it is checked by reading the source |
| `Build` / `host-installed-libubertooth` | Builds the tools against an installed libubertooth instead of the copy in the build tree, so what a packager ships keeps working |
| `Build` / `scripts` | shellcheck; a self-test of each `tools/check-*.sh` that proves the check still catches what it is for; that the list of libbtbb entry points the tools call is still the list libbtbb has; and, on the macOS leg alone, the documentation API version check run over the real tree with the BSD tools |
| `Build` / `issue-references` | No issue or pull request number in source, tests or build files: the reason for a line belongs in its comment, the link in the commit message or the pull request |
| `Build` / `image-revision` | Every shipped firmware image carries a readable revision |
| `Build` / `api-version` | The firmware API version against `main`, using `main`'s copy of the check |
| `Build` / `dfu-package` | Each shipped image can be packaged and the packaged file reads back |
| `install.sh` / `install-sh` | Runs the installer end to end, on Ubuntu, macOS and a Fedora container (on a pull request, only when `install.sh` or its own workflow file changes). The Fedora leg installs nothing beforehand, so the script's own `dnf` package list is what the run uses |
| `libbtbb pin` / `libbtbb-pin` | The pinned libbtbb commit is the same in every file that repeats it |
| `Man pages` / `refresh` | The committed roff man pages match their Markdown sources (only when `host/doc/` changes) |
| `Doc API version` / `doc-api-version` | No documentation sample pins the firmware API version, and `TEST_PLAN.md`, `COMPATIBILITY.md` and the CHANGELOG's top section state the version the tree carries |

On a pull request, `host` runs on two Ubuntu runners -- 22.04, the oldest
glibc a hosted runner offers, and 24.04 -- and one macOS runner; `firmware`
and `scripts` run on one Ubuntu and one macOS runner; `install-sh` on those
two and in a Fedora container. Each reports once per runner:
`host (ubuntu-22.04)`, `host (ubuntu-24.04)`, `host (macos-26)`. The weekly,
release and on-request runs are a fuller tier: those jobs cover more runners,
and three more jobs (`host-ubuntu-20_04`, `firmware-ubuntu-20_04`,
`host-fedora`) build on the oldest supported Ubuntu and on Fedora. None of
the extra tier gates a pull request.

A change to documentation only builds nothing, but every `Build` name still
reports, and the whole required set is green about a minute after the run
starts. The `changes` job sees that each changed file is a Markdown file,
under `docs/` or `.kiro/`, or a generated man page, and skips `host`,
`host-installed-libubertooth`, `firmware` and `scripts`; `image-revision` and
`dfu-package` skip with the firmware job whose images they read. The cheap
Linux checks (`issue-references`, `api-version`, `libbtbb-pin` and
`doc-api-version`) run as they always do, and `Man pages` runs whenever
`host/doc/` changes, which is where the man page sources live. `install.sh`
asks for its own two paths and is not reached.

One man page is an exception. `host/doc/ubertooth-btbr.md` states the
transmit formats' data lengths and the counts the slot bound allows, and a
host test compares those with the tool's own table and the firmware's, so a
branch that changes only that page still runs the `host` jobs. `firmware` and
`scripts` are skipped for it as for any other documentation branch.

`libbtbb pin` and `Doc API version` have no path filter at all, because what
they read includes documentation: the pinned commit is written down in
`COMPATIBILITY.md` and the build guides, and the API version check reads every
tracked documentation file, so a documentation-only change is the one it most
needs to see. Both also run on the push to `main`, which `Build` does not for
a change under `docs/`, `.kiro/` or any `*.md`.

The skipped `Build` names are reported by stand-in jobs (`host-skipped`,
`firmware-skipped`, `scripts-skipped`), one per leg of the pull-request
matrix, each carrying the same check name as the leg it stands in for and
running on the opposite condition. They exist because GitHub does not expand
the matrix of a job it skips: a skipped `host` would report once, as `host`,
while the rule on `main` asks for `host (ubuntu-24.04)` and
`host (macos-26)`, which nothing would then report. So adding or renaming a
runner in the pull-request matrix means editing the stand-in's matrix, the
protection rule on `main`, and the names above, together.

That is also why `gh pr checks` lists entries under a bare name (`host`) or an
unevaluated one (`host (${{ matrix.os }})`): GitHub does not evaluate the name
of a job it skips. Neither is required, and neither means anything went wrong.
Each check name is reported by exactly one job per pull request, which keeps a
stand-in from ever masking a real failure.

### They are required

`main` carries a branch-protection rule, and twelve of the names above are
required status checks on it: `host (ubuntu-24.04)` and `host (macos-26)`,
both `firmware` legs, both `scripts` legs, `issue-references`,
`image-revision`, `api-version`, `dfu-package`, `libbtbb-pin` and
`doc-api-version`. A pull request with one of them red, or with one still to
report, cannot be merged. `install-sh` and `refresh` are not required, because
each runs only when its own paths change and a required check that does not
run leaves a pull request pending forever; `doc-api-version` has no path
filter and reports on every pull request, which is why it can be required.
The third host leg, `host (ubuntu-22.04)`, and `host-installed-libubertooth`
report like the rest but do not gate a merge.

The rule is not strict, so a branch need not be up to date with `main` to
merge, and it requires no reviews: the project's own rules say who reviews
what and who may merge. It does not apply to the repository owner
(`enforce_admins` is off).

**Do not reach for `--admin`.** A red required check is a stop: if you believe
it should not block, say so on the pull request and leave it for the
maintainer. There is one standing exception, for the macOS runners, which
queue far longer than the rest. Once every Linux and platform-independent
required check has passed, and the only unfinished ones left are `macos-26`
legs -- or `image-revision` or `dfu-package` waiting on `firmware
(macos-26)` -- that have been queued or running for more than seven minutes,
the pull request may be merged with `--admin`. Say on the pull request which
checks were still pending and for how long, then watch the same legs of
`main`'s push run and file an issue if one of them fails. A macOS leg that has
finished and failed still blocks: fix it or ask. A release candidate is held
to the full run, macOS included, before it is named.

What the rule does not fix is that GitHub reads workflow files from the branch
under review, so a branch can still weaken an `if:` condition, drop a step or
delete a job, and its own run will not complain. The `api-version` job fetches
`main`'s copy of its script for that reason; the job definitions cannot defend
themselves the same way. So a reviewer reads `.github/workflows/` and
`tools/check-*.sh` in a diff as carefully as the code: a change there is a
change to the gate, not a change to a test.
