---
inclusion: always
---
# Branch Naming and Review Rules

This file is intentional version-controlled workspace policy and MUST NOT be
excluded by `.gitignore`. It is authoritative for these topics and supplements
other repository or user-level safety/merge rules. When rules differ, follow
the stricter requirement.

## Issue-first branches

- For every new branch that will modify repository content, MUST identify or
  create the tracking issue before creating the branch whenever the issue
  tracker is available and issue creation is permitted.
- The new branch MUST start with `<issue-number>_`, followed by a short
  descriptive slug, for example `33_fix-python-install` or `45_agent-rules`.
- For work spanning several issues, use the primary issue number and reference
  the others in commits and review notes.
- Existing published branches, externally named review refs, read-only work,
  unavailable issue tracking, and emergency recovery are exceptions; record
  why the issue-first prefix was impractical.

## Pull-request traceability

- Every merge candidate MUST be submitted through a GitHub pull request (PR)
  targeting `main`, including candidates classified as low-stakes. A low-stakes
  classification can remove the maintainer-review stop, not the PR record.
- Each PR SHOULD have exactly one primary issue. Its description MUST identify
  that issue and use `Closes #<number>` when merging the PR completes the
  issue's acceptance criteria.
- A single atomic PR MAY address tightly coupled secondary issues. Use
  `Also closes #<number>` only when it fully completes them; otherwise use
  `Related to #<number>` so they remain open.
- One issue MAY require multiple independently useful PRs. Each PR MUST state
  its slice of the work; intermediate PRs use `Part of #<number>`, and only the
  completing PR uses `Closes #<number>`.
- Research, support, hardware-blocked, and other issues that produce no
  repository change do not require a PR.
- The PR description MUST summarize user impact and scope, validation results,
  each independent agent-review verdict, and whether the merge is low-stakes or
  requires maintainer approval. `pr-and-comments.md` has the layout.
- Direct merges are exceptions only for emergency recovery or when GitHub PRs
  are unavailable. The merge commit and reason MUST be recorded on the issue.
  MUST NOT manufacture retrospective PRs for changes already merged.

## Branch cleanup

- Once a PR is closed or its change is merged to `main`, MUST delete its remote
  and local branch, including superseded branches from earlier attempts.
- MUST NOT delete `main`, the authoritative remote's default branch, or a
  branch whose work is neither merged nor recorded elsewhere without asking.

## Independent agent review

Changes limited to repository-local agent rules (`.kiro/steering/`) are
exempt: MUST NOT be sent for agent review, and are merged through their PR
without waiting for maintainer approval.

Every other merge candidate containing code, configuration, workflow, policy,
or non-trivial operational documentation changes:

- MUST receive at least one independent read-only agent review. A small
  host-only or test-only change (under roughly 150 changed lines, no firmware,
  no libbtbb) may have one; everything else, and every firmware or libbtbb
  change, MUST have two, on different model families so they do not share the
  same blind spots.
- MUST give each reviewer the same final merge-base diff and acceptance criteria
  without sharing the other reviewer's conclusions.
- SHOULD run reviewers in parallel.
- MUST tell every reviewer that read-only includes hardware: a reviewer MUST
  NOT run a built tool against an attached board, and MUST NOT flash one.
- MUST resolve every blocker and rerun affected validation.
- If any candidate content changes after a blocker is reported, both reviewers
  MUST review the resulting final diff again.
- SHOULD run a further round while a round still finds real bugs relevant to
  the code changed or the issue's topic, and stop after a round that finds only
  minor, out-of-scope, or already-deferred items. Pin every round to the exact
  commit SHA and name each reviewer's model in the PR.

## CI results

- `CONTRIBUTING.md`, "The checks a pull request has to pass", is the
  inventory: every check, the runners each uses, the path filters, and what a
  documentation-only change runs. Do not repeat it here. What it leaves out:
  - CI has two tiers. Pull requests and pushes to `main` run the minimal
    tier; the weekly schedule, a published release, and a manual run run the
    full matrix.
  - `image-revision` and `dfu-package` are the only checks that handle a
    built image the way `ubertooth-dfu` does: one reads its firmware info
    block rather than comparing the magic bytes, the other packages it into
    the `.dfu` users flash and asks the tool whether it would accept it.
  - `host-installed-libubertooth` is the only run that reaches the host test
    directory's `else()` branches. What it covers -- which include directory a
    target is given -- is the same on every platform, so it has no Mac leg.
  - `api-version` runs on pull requests and pushes to `main` only: it compares
    a branch with `main`, and the other triggers have no branch to compare.
  - `libbtbb pin` also runs `tools/check-workflow-libbtbb.sh`, which refuses a
    workflow file naming a libbtbb repository of its own. Only `build.yml`,
    where the pin is decided, and `manpages.yml`, which cannot read another
    workflow's env, may name one, and both must name the pinned repository. A
    job that wants the library reads the pin instead of naming a repository.
- A branch with no open PR gets no CI; open a PR (a draft is enough) to get a
  run.
- Before cutting a release, run the full tier manually and wait for it:
  `gh workflow run build.yml` and `gh workflow run install-sh.yml`. The run on
  a published release is a record, not a gate.
- A newer commit on a PR cancels that PR's older run. Read results with
  `gh pr checks <PR>`, which reports the current head commit, not with the id of
  a run you saw earlier. A "cancelled" run for a superseded commit is expected,
  not a failure.
- Runs for `main` and the weekly schedule are never cancelled.
- Cancelling runs yourself is for runs you made redundant (for example a PR you
  closed as superseded); never cancel another agent's or PR's runs.

### Time budget and the good-enough call

Speed matters. Do not wait for every job.

- **Almost no CI wait** for changes limited to documentation or agent rules
  (`*.md`, `.kiro/steering/`). The required checks still have to report, but a
  documentation-only branch builds nothing, so the whole set is green about a
  minute after the run starts. A change under `host/doc` also waits for
  `Man pages`, which is what catches a page left unrefreshed; that one is not
  required, but it is the check such a change exists to satisfy.
- **Twelve checks are required on `main`** and GitHub refuses the merge until
  each has reported green or skipped. `CONTRIBUTING.md` names them, under
  "They are required". The rule does not apply to the repository owner, which
  is there for the one case where the maintainer has looked at a red check and
  decided to merge anyway: a check the maintainer has allowed to be red is the
  maintainer's merge, not yours. Say on the PR why you think it should not
  block, and leave it. The one `--admin` merge an agent may make is the macOS
  delay rule below.
- **A macOS leg pending more than 7 minutes may be merged past.** The macOS
  runners queue far longer than the rest, so the maintainer's standing
  direction is: once every Linux and platform-independent required check has
  passed, and the only unfinished required checks left are `macos-26` legs --
  or `image-revision` or `dfu-package` waiting on `firmware (macos-26)` --
  that have been queued or running for more than 7 minutes, merge with
  `gh pr merge <PR> --squash --admin --match-head-commit <sha>`. Say on the PR
  which macOS checks were still pending and for how long. Then watch those
  legs of `main`'s push run, and file an issue at once if one fails. A macOS
  leg that has COMPLETED with a failure still blocks: fix it or ask. The
  release candidate is held to the full run, macOS included, before it is
  named. This is the only case where `--admin` is allowed.
- **Gating set: one Linux and one Mac.** A code change is good to merge once
  `host (ubuntu-24.04)` and `host (macos-26)` have passed and nothing has
  failed. A change under `host/` also needs `host-installed-libubertooth`,
  which is where the installed-library build is tested.
  Changes under `firmware/` also need both firmware jobs,
  `image-revision` and `dfu-package`, which read and package the images they
  built; a change to the host's DFU code needs `dfu-package` too, since it
  builds and runs the packaging tool; changes to `install.sh` need both
  `install-sh` jobs; changes under `host/doc` need the `Man pages` workflow,
  which `gh pr checks` prints under its job name, `refresh`.
  `issue-references`, `api-version`, `libbtbb-pin`, `doc-api-version`,
  `scripts (ubuntu-24.04)` and `scripts (macos-26)` gate every change they run
  on, whatever the change touches: they take seconds to a minute, need nothing
  built, and a red one means the change itself broke the rule they check.
  A full-tier failure is followed up on `main`.
- **Time budget**: wait at most 10 minutes for the gating set once the run has
  started. Poll every 30 seconds (`gh pr checks <PR>`) rather than one long
  `--watch`, and merge as soon as the gating set is green. If jobs have not started after 5 minutes, say so and decide with the
  user instead of polling.
- **Stuck job**: a job with no progress for 10 minutes while the others are
  done is infrastructure noise; ignore or cancel it and note it.
- **A failure in the gating set blocks**; a failure in a non-gating job needs
  a quick look (is it related?) but does not by itself block.
- **Test on current main.** Rebase the branch onto the latest `squeaky/main`
  before running the tests and before any hardware test, so the results cover
  the code that will actually land and a problem from another merge shows up
  here, not after the squash. If `main` moves after testing, rebase again and
  re-run the automated tests before merging. Repeat the hardware test only
  when `main` changed something that test exercises: firmware, libubertooth,
  the same tool, or the build and install path. Record in the PR which `main`
  commit the hardware test ran on.
- Run the independent reviews in parallel with CI, not after it.
- **Poll, do not block.** Never run one long `gh pr checks --watch` or chained
  `sleep` loops in the foreground. Check `gh pr checks <PR>` every 30 seconds
  between other work (reviews, the next issue) and merge the moment the gating
  set is green.

## Firmware API version

`UBERTOOTH_API_VERSION` (`host/libubertooth/src/ubertooth_interface.h`, which
the firmware reports as the USB `bcdDevice`) moves **only for a change that
breaks older host tools**, plus once at each release (maintainer, 2026-10-29).

- During development a firmware PR MUST NOT bump it. The host tools and the
  firmware are built, installed and flashed together, and a development build
  only warns when the two disagree, so an ordinary firmware change -- a new
  request, a new mode, a fix -- leaves the number where it is. A host-only PR
  MUST NOT touch it at all: it has no new firmware to pair with, so moving the
  number only refuses boards that are fine.
- A PR MUST bump the minor version when the change really does break an older
  host tool: a request removed or renumbered, a reply or record layout an
  older host would misread. Say in the PR why it breaks one; a bump that
  follows this rule needs no maintainer decision (maintainer, 2026-10-30). The
  number is BCD, so after `0x0119` comes `0x0120`, not `0x011a`. Take the
  value after whatever `main` carries, re-checking after a rebase.
- A bump updates `validation/TEST_PLAN.md`'s expected `API:` string,
  `COMPATIBILITY.md`'s firmware row, the pinned strings in the tests, and the
  CHANGELOG's reflash note -- the places that state the requirement -- and
  nothing else. **Sample output** writes the version as `API:<version>` rather
  than a number, in the man pages, the README, `docs/` and the CHANGELOG
  alike: keep it that way, so a bump has one place to change instead of one
  more to chase. A version a sample is making a point about --
  `2020-12-R1`'s 1.07, say -- is a fixed number and stays.
- The `doc-api-version` CI job runs `tools/check-doc-api-version.sh`, which
  asks all of that, and its own comments say exactly how. The part to know
  before writing a release note: the CHANGELOG's **top section** has to say,
  in those words, "the firmware API version is now `<version>`", so dropping
  that sentence is a failure rather than a pass. Run the check before pushing;
  it reads files and takes a second.
- The `api-version` CI job runs `tools/check-api-version.sh`, which compares
  the value on the branch with `main`'s tip and with their merge base. It
  cannot judge whether a change breaks an older host tool, so it asks what is
  left: a branch that changes the firmware interface may leave the value as
  the merge base had it, or take one above `main`'s; a branch that changes no
  firmware interface file must leave it alone; and only a release takes a
  major version. Run it before pushing if you are bumping: a value that was
  free when the branch was cut may not be free now.
  - It decides whether a branch changes the firmware interface by comparing
    the **content** of a watched list of files, blank lines and whole-line
    comments not counted: the sources compiled into the two run-time images,
    their own Makefiles and linker scripts, and the interface header.
    `tools/check-api-version.sh --list-watched` prints the list, and the
    script's own comments say which lines of `firmware/common.mk` count. That
    is an approximation of the rule above, not the rule, and all it decides is
    whether a branch is ALLOWED to bump.
  - The one false red left is the other direction: a change that breaks an
    older host tool from a file the check does not watch has its bump refused
    as though the branch were host-only. Do NOT edit the watched list to suit
    one branch: say on the PR what changed and why an older host tool would
    misread it, and leave it for the maintainer to merge red.
- A release moves it to the next major version to mark the break in
  host/firmware pairing; the first fork release takes `0x0200` (2.00). That is
  a release step, not a per-PR one (see `docs/source/release_procedure.rst`).
  The check knows about it: one major up **from the merge base**, with the
  minor digits at `00`, above `main`, and with `CHANGELOG.md`,
  `COMPATIBILITY.md` and `validation/TEST_PLAN.md` moving with it, is allowed
  with no firmware change, so the release's own PR is not refused. **No other
  branch may take a major version**, whether or not it changes the firmware: a
  board accepts only the exact value, so a `0x0200` typed in place of `0x0135`
  would refuse every user's board until it was reflashed, and it burns the
  value the release reserves.
- On a pull request the job runs **`main`'s copy** of
  `tools/check-api-version.sh`, not the branch's, so a branch cannot edit the
  check that gates it. A branch's own changes to the script are reviewed and
  are exercised by `tools/check-api-version-selftest.sh`, which runs from the
  branch; they take effect for other branches once they are on `main`. The
  job's own definition still comes from the branch, as GitHub reads workflow
  files from the branch; branch protection is where that is answered.
- Host tools and firmware built from the same commit always match, so after
  pulling, rebuild and reflash both from the same commit
  (`./install.sh --flash`).

## Changelog discipline

Each release section has three buckets, and every change goes in exactly one:

- **Main sections**: what a user sees change in normal use — a wedge or a
  hang, wrong data, a new or changed option, a message or an exit code.
- **"Under the hood"**, the last section of the release: real fixes with no
  visible change in normal use — firmware bounds, retries, buffer limits,
  resources given back, edge cases only stress testing or odd hardware reaches.
  One line each, plain words, no function names.
- **No entry at all**: tests, CI, internal refactors, test-only firmware
  images (`usb_test`, `assembly_test`) and development-only tools. Shipped
  tools and images, `btbr` and `ubertooth-btbr` included, do get entries.

When unsure which bucket a change belongs in, ask rather than guessing.

The main sections of a release are grouped so that a user of one tool can read
what changed for it, in this order:

1. **Highlights**: the largest wins of the release.
2. **Stability and lockups**: a board or a tool that stopped answering,
   wedged, reset itself, or crashed, and the reporting that makes those
   visible.
3. **Across all tools**: behavior the tools share, under the headings
   `The command line and option values`; `Output streams, messages, and exit
   status`; `Choosing a board, and what it is running`; `Captures:
   destinations, files, and what a capture reports`; `Capture timestamps`;
   `Starting and stopping a capture`; and `Generic radio mode`.
4. **One section per tool**, in the README tool catalog's order, each with
   `New features` before `Fixes`.
5. **Firmware** (`bluetooth_rxtx`, `btbr`, and the bootloader), then
   **Libraries** (libubertooth, and libbtbb).
6. **Build and compatibility**, **Documentation**, and **Under the hood**.

So a new bullet goes under its tool, or under "Across all tools" when the
behavior is shared, or under the firmware image, library, or build section it
belongs to. A bullet that spans two or three tools goes under the tool it is
mostly about, named in its first words, rather than being repeated.

Within a bucket:

- MUST phrase a bug fix as a fix: say what was wrong and that it is fixed
  ("Fixed: AUX1, DV, EV3, EV4, and EV5 payloads did not decode; they now
  do."), not as though it were a new feature.
- MUST use American spelling in the fork's own entries. Leave the upstream
  release sections as upstream wrote them.
- MUST describe user-facing impact concisely, normally in one bullet of one to
  three short lines.
- MUST keep implementation history, detailed validation evidence, diagnostics,
  and long rationale in the linked issue or documentation instead.
- SHOULD combine closely related behavior into one entry and avoid repeating
  information already present elsewhere in the same release section.
- MUST NOT add product changelog entries for agent policy, process-only or
  tests-only changes, comments, or internal refactors without user-visible
  behavior.
- MUST NOT add changelog entries for documentation-only changes, including
  updates to the `COMPATIBILITY.md` matrix and general documentation.

## Issue references

- MUST NOT put issue or PR numbers (for example `(#53)` or `issue #15`) in
  source, tests, or build files. Upstream and this fork number issues
  independently, so a bare number is ambiguous. Explain the reason in the
  comment itself instead.
- Issue links belong in commit messages, PR descriptions, and `CHANGELOG.md`;
  there, use full URLs (`https://github.com/SqueakyFloors/ubertooth/issues/N`)
  or `owner/repo#N`.
- Leave vendored upstream files as they are.
- The `issue-references` CI job runs `tools/check-issue-references.sh`, which
  scans the files this rule covers. Run it before pushing; a match that is not
  an issue reference (an assembly immediate, a message a test asserts on) goes
  in the allowlist in that script, with the reason.

## Documentation clarity

- Do NOT spell out common acronyms: assume the reader knows them. Examples:
  LED, USB, Wi-Fi, BLE, RF, RSSI, dBm, MHz, API, CLI, GPIO, CPU, PCAP, DFU,
  CRC, UUID, MAC. Write "LED", never "light-emitting diode (LED)".
- Expand an acronym at its first use in a document only when it is specific to
  Bluetooth internals or this project and a reader may not know it (for
  example LAP, UAP, AFH, CLKN), and only once per document.
- MUST preserve exact error strings that users are likely to search for, such
  as `ModuleNotFoundError: No module named 'specan'`, in the relevant
  troubleshooting documentation even when changelog prose is condensed.
- After editing a man page source in `host/doc/`, MUST regenerate its
  committed roff page with `cmake -S host -B build && cmake --build build
  --target refresh-manpages` and commit the `.1` with the `.md`. The
  `Man pages` CI job fails otherwise.

## Public text: no lab or company details

This repository is public. In code, tests, docs, commit messages, branch
names, issues, PRs and comments:

- MUST NOT name lab machines: no host names, SSH aliases, IP addresses,
  user@host strings or lab home-directory paths. Write "the lab", "the lab
  host", "the Linux lab host" or "the jump host". Scripts take the host from
  configuration or an argument, never a built-in host name.
- MUST NOT name other systems or services that run in the lab, or the
  maintainer's employer's internal tools, repositories or services. Describe
  such workloads generically ("a MAC polling workload", "other software on
  the lab host").
- Board serial numbers of lab Ubertooths are fine.

## Public merge gate

Agent reviews supplement rather than replace maintainer review where it is
required. Before merging into `main`, every final branch MUST satisfy all
existing clean-tree, latest-main, test, and independent-review gates, and MUST
land as one squashed commit by squash-merging its PR. The PR branch needs no
squashing beforehand; never open a new branch or PR only to squash. Determine the authoritative main remote from `branch.main.remote` or
repository documentation/configuration; if missing or ambiguous, stop and ask
rather than guessing.

Maintainer review MAY be omitted when the change is low-stakes. A change is
low-stakes when its correct behavior is already decided and it is one of:

- a bug fix whose right answer is settled by the documentation, a man page,
  `.kiro/steering/cli-output.md`, or a maintainer decision recorded on the
  issue;
- tests;
- documentation, comments, changelog wording, or repository-local agent rules
  (`.kiro/steering/`), including changelog condensation that keeps every
  distinct user-facing change, the useful links, and external credit;
- a small consistency fix that brings one tool in line with the rest.

A change is NOT low-stakes, and MUST be left open with the `needs maintainer`
label and one line saying what to decide, when it:

- asks a design or behavior question the issue or the maintainer has not
  already answered, or picks between real alternatives;
- adds, removes, or changes an option, a feature, or a default;
- changes a dependency, other than moving the libbtbb pin to a commit of this
  project's libbtbb fork, which merges itself like any other change once CI is
  green (maintainer, 2026-11-03);
- touches the bootloader or the flash layout, the USB protocol, or firmware API
  policy. A firmware API version bump that follows "Firmware API version"
  above is not a maintainer question (maintainer, 2026-10-30): say in the PR
  why an older host tool would misread the firmware, and merge;
- is large firmware rework (the USB stack, interrupt or SPI atomicity, radio
  timing);
- adds tooling or process: a script under `tools/`, a CI job or workflow, a new
  check or test framework, or more than roughly 300 lines that are not the fix
  itself;
- or would merge past a red required check.

If classification is uncertain, MUST treat it as requiring maintainer review.
A red required check is never a maintainer question: fix it, or wait for the
fix. Never use `--admin` outside the macOS delay rule above, and never change
branch protection.

Some questions are settled for every change and MUST NOT be asked again: a bad
or meaningless request is an error, never silently accepted; the least
surprising behavior for the user wins; an honest report beats a quiet success.

A low-stakes change still passes every other gate in this document before it
merges: a clean tree, rebased on the latest `main`, the tests and the
independent reviews, the required checks green, and the hardware run on that
rebased head where the change can reach a board. A low-stakes classification
removes the maintainer-review stop and nothing else.

For every candidate outside that exception, fetch the authoritative remote,
present the exact final diff to the maintainer, and MUST wait for explicit
maintainer approval of that candidate. Remove the `needs maintainer` label, and
the question at the top of the description, in the same step as the answer that
settles it.
