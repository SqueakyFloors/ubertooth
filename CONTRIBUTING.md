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

Everything below runs from `.github/workflows/`; each row says when, and the
two paragraphs after the table say what a documentation-only change runs:

| Workflow / job | What it is for |
| --- | --- |
| `Build` / `host` | Builds the host tools and runs the host test suite, a strict-C-standard build, and a smoke test of each binary |
| `Build` / `firmware` | Builds the ARM firmware images and the bootloader, runs the firmware test suite, and checks that a flag or linker-script change rebuilds the images. The ToorCon 13 badge (`firmware/tc13badge`, a Renesas R8C part) is not among the images built: nothing here has that toolchain, so the test suite compiles its C for syntax only and the rest of a change to it is checked by reading the source |
| `Build` / `host-installed-libubertooth` | Builds the tools against an installed libubertooth instead of the copy in the build tree, so what a packager ships keeps working |
| `Build` / `scripts` | shellcheck, a self-test of each `tools/check-*.sh` that proves the check still catches what it is for, that the list of libbtbb entry points the tools call is still the list libbtbb has, and, on the macOS leg alone, the documentation API version check below run once over the real tree with the BSD tools |
| `Build` / `issue-references` | No issue or pull request number in source, tests or build files: the reason for a line belongs in its comment, the link in the commit message or the pull request |
| `Build` / `image-revision` | Every shipped firmware image carries a readable revision |
| `Build` / `api-version` | The firmware API version against `main`, using `main`'s copy of the check |
| `Build` / `dfu-package` | Each shipped image can be packaged and the packaged file reads back |
| `install.sh` / `install-sh` | Runs the installer end to end, on Ubuntu, macOS and a Fedora container (on a pull request, only when `install.sh` or its own workflow file changes). The Fedora leg installs nothing beforehand, so the script's own `dnf` package list is what the run uses |
| `libbtbb pin` / `libbtbb-pin` | The pinned libbtbb commit is the same in every file that repeats it |
| `Man pages` / `refresh` | The committed roff man pages match their Markdown sources (only when `host/doc/` changes) |
| `Doc API version` / `doc-api-version` | No documentation sample pins the firmware API version, and `TEST_PLAN.md`, `COMPATIBILITY.md` and the CHANGELOG's top section state the version the tree carries. It is the gate for that rule, because it has no path filter and `Build` skips a documentation-only change |

On a pull request, `firmware` and `scripts` run on one Ubuntu and one macOS
runner, `install-sh` on those two and in a Fedora container, and `host` runs
on two Ubuntu runners and one macOS runner:
Ubuntu 22.04, the oldest glibc a hosted runner offers, as well as 24.04. Each
reports once per runner: `host (ubuntu-22.04)`, `host (ubuntu-24.04)`,
`host (macos-26)`. The weekly, release and on-request runs are a fuller tier:
those jobs cover more runners, and three more jobs
(`host-ubuntu-20_04`, `firmware-ubuntu-20_04`, `host-fedora`) build on the
oldest supported Ubuntu and on Fedora. None of the extra tier gates a pull
request.

A change to documentation only builds nothing, but every `Build` name still
reports. The `changes` job sees that each changed file is a Markdown file,
under `docs/` or `.kiro/`, or a generated man page, and skips `host`,
`host-installed-libubertooth`, `firmware` and `scripts`; `image-revision` and
`dfu-package` skip with the firmware job they read. The cheap Linux checks
(`issue-references`, `api-version`, `libbtbb-pin` and `doc-api-version`) run
as they always do, and `Man pages` runs whenever `host/doc/` changes, which is
where the man page sources live.
`install.sh` asks for its own two paths and is not reached.

One man page is an exception. `host/doc/ubertooth-btbr.md` states the
transmit formats' data lengths and the counts the slot bound allows, and a
host test compares those with the tool's own table and the firmware's, so a
branch that changes only that page still runs the `host` jobs. `firmware` and
`scripts` are skipped for it as for any other documentation branch.

`libbtbb pin` has no path filter at all, because the commit it checks is
written down in `COMPATIBILITY.md` and the build guides as well as in the
build files. `Doc API version` has none for the same kind of reason: what it
reads is every tracked documentation file, so a documentation-only change is
the one it most needs to see, and `Build` is the workflow that skips those. It
runs on the push to `main` as well, which `Build` does not for a change under
`docs/`, `.kiro/` or any `*.md`.

The skipped names are reported by stand-in jobs (`host-skipped`,
`firmware-skipped`, `scripts-skipped`), one per leg of the pull-request
matrix, each carrying the same check name as the leg it stands in for and
running on the opposite condition. They exist because GitHub does not expand
the matrix of a job it skips: a skipped `host` would report once, as `host`,
and the rule on `main` requires `host (ubuntu-24.04)` and `host (macos-26)`,
which nothing would then report. Adding or renaming a runner in
the pull-request matrix therefore means editing the stand-in's matrix, the
protection rule on `main`, and the names above, together.

That same rule explains two sets of odd-looking entries in `gh pr checks`,
neither of which is required and neither of which means anything went wrong.
A skipped real job reports under its bare name (`host`, `firmware`,
`scripts`), and a skipped stand-in reports under its unevaluated one
(`host (${{ matrix.os }})`), because GitHub does not evaluate the name of a
job it skips either. On any one pull request each check name is reported by
exactly one job, which is what keeps a stand-in from ever masking a real
failure.

### They are required

`main` carries a branch-protection rule, and eleven of the names above are
required status checks on it: `host (ubuntu-24.04)` and `host (macos-26)`,
both `firmware` legs, both `scripts` legs, `issue-references`,
`image-revision`, `api-version`, `dfu-package` and `libbtbb-pin`. A pull
request with one of them red, or with one still to report, cannot be merged.
`install-sh` and `refresh` are not required, because each runs only when its
own paths change and a required check that does not run leaves a pull request
pending forever. That reason does not apply to `doc-api-version`, which has no
path filter and reports on every pull request: it belongs in the required set
and only the maintainer can put it there. The project's own rules treat it as
gating meanwhile, so do not merge past a red one. The third host leg,
`host (ubuntu-22.04)`, and `host-installed-libubertooth` report like the rest
but do not gate a merge.

The rule is not strict, so a branch need not be up to date with `main` to
merge, and it requires no reviews: the project's own rules say who reviews
what and who may merge. It does not apply to the repository owner
(`enforce_admins` is off), which exists for the one case where the maintainer
has looked at a red check and decided to merge anyway. **That is the
maintainer's call and the maintainer's merge. Never reach for `--admin`**: if
a required check is red and you believe it should not block, say so on the
pull request and leave it.

There is one standing exception, and a red check is not it. The macOS runner
pool queues for half an hour or more, and sometimes never starts a job at all.
So when every other required check has passed and the only unfinished ones are
`macos-26` legs that have waited more than 7 minutes, the merge goes through
with `--admin`, with a note on the pull request saying which legs were pending
and for how long, and the macOS legs of `main`'s own run are read afterwards.
A macOS leg that has finished and failed still blocks. A release candidate is
cut only from a commit with a complete green run, macOS included.

What the rule does not fix is that GitHub reads workflow files from the branch
under review, so a branch can still weaken an `if:` condition, drop a step or
delete a job, and its own run will not complain. The `api-version` job fetches
`main`'s copy of its script for that reason; the job definitions cannot defend
themselves the same way. So a reviewer reads `.github/workflows/` and
`tools/check-*.sh` in a diff as carefully as the code: a change there is a
change to the gate, not a change to a test.

One practical consequence. A pull request whose last run skipped a matrix job
before the stand-ins existed reports that job under its bare name, and the leg
names the rule asks for have nothing to report them, so it sits blocked. A
rebase onto `main` and a fresh run clears it.
