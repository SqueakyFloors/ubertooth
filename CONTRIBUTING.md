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
- Host tools and firmware are released together: the host refuses firmware of
  another API version, so a change to firmware behaviour the host relies on
  bumps `UBERTOOTH_API_VERSION`.

## The checks a pull request has to pass

Everything below runs from `.github/workflows/`, on every pull request that
touches code:

| Check | What it is for |
| --- | --- |
| `Build / host` | Builds the host tools on Ubuntu and macOS and runs the host test suite, a strict-C-standard build, and a smoke test of each binary |
| `Build / firmware` | Builds every firmware image on both runners, runs the firmware test suite, and checks that a flag or linker-script change rebuilds the images |
| `Build / scripts` | shellcheck, plus a self-test of each `tools/check-*.sh` that proves the check still catches what it is for |
| `Build / issue-references` | No issue or pull request number in source, tests or build files: the reason for a line belongs in its comment, the link in the commit message or the pull request |
| `Build / image-revision` | Every shipped firmware image carries a readable revision |
| `Build / api-version` | The firmware API version against `main`, using `main`'s copy of the check |
| `Build / dfu-package` | Each shipped image can be packaged and the packaged file reads back |
| `install.sh` | Runs the installer end to end on both runners (only when `install.sh` changes) |
| `libbtbb pin` | The pinned libbtbb commit is the same in every file that repeats it |
| `Man pages` | The committed roff man pages match their Markdown sources (only when `host/doc/` changes) |

A change to documentation only (`**.md`, `docs/`, `.kiro/`) runs none of
these, by the `paths-ignore` in each workflow.

### Nothing enforces them

`main` has no branch-protection rule, so a red pull request can still be
merged, and GitHub reads workflow files from the branch under review, so a
branch can weaken an `if:` condition, drop a step or delete a job and its own
run will not complain. The `api-version` job fetches `main`'s copy of its
script for that reason, but the job definitions themselves cannot defend
themselves this way.

So a reviewer reads `.github/workflows/` and `tools/check-*.sh` in a diff as
carefully as the code: a change there is a change to the gate, not a change to
a test. Requiring these checks on `main` is a repository-settings decision
that has not been made.
