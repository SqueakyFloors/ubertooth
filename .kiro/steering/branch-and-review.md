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
  both independent agent-review verdicts, and whether
  the merge is low-stakes or requires maintainer approval.
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

- MUST receive at least two independent read-only agent reviews.
- MUST give each reviewer the same final merge-base diff and acceptance criteria
  without sharing the other reviewer's conclusions.
- SHOULD run reviewers in parallel.
- MUST resolve every blocker and rerun affected validation.
- If any candidate content changes after a blocker is reported, both reviewers
  MUST review the resulting final diff again.

## CI results

- CI has two tiers. Pull requests and pushes to `main` run the minimal tier:
  host and firmware on `ubuntu-24.04` and `macos-26`. The `install-sh` jobs run
  on every push to `main` but on a PR only when `install.sh` changes; the
  `Man pages` job the same way, when anything under `host/doc` changes. The
  weekly schedule, a published release, and a manual run run the full matrix.
  `issue-references` and the two `scripts` jobs run on both tiers and need
  nothing built. `image-revision` and `dfu-package` run on both tiers too,
  after the firmware job, on the images that job built: `image-revision` is the
  only check that reads a built image's firmware info block the way
  `ubertooth-dfu` reads it, rather than comparing the magic bytes, and
  `dfu-package` the only one that packages an image into the `.dfu` users flash
  and asks the tool whether it would accept it. `api-version` runs on pull
  requests and pushes to `main` only, because it compares a branch with `main`
  and on the other triggers there is no branch to compare. `libbtbb-pin` is its
  own workflow with no path filter, so it runs on every change, documentation
  included: two of the files that have to agree on the pin are
  `COMPATIBILITY.md` and the build guides. It also runs
  `tools/check-workflow-libbtbb.sh`, which refuses a workflow file naming a
  libbtbb repository of its own: only `build.yml`, where the pin is decided,
  and `manpages.yml`, which cannot read another workflow's env, may name one,
  and both must name the pinned repository. A job that wants the library reads
  the pin instead of writing a repository.
  A branch with no open PR gets no CI; open a PR (a draft is enough) to get a
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

- **No CI wait** for changes limited to documentation or agent rules
  (`*.md`, `.kiro/steering/`): merge as soon as the PR is ready. Two
  exceptions: a change under `host/doc` waits for `Man pages`, since it is
  what catches a page left unrefreshed, and any documentation change waits
  for `libbtbb-pin`, which takes about five seconds.
- **Gating set: one Linux and one Mac.** A code change is good to merge once
  `host (ubuntu-24.04)` and `host (macos-26)` have passed and nothing has
  failed. Changes under `firmware/` also need both firmware jobs,
  `image-revision` and `dfu-package`, which read and package the images they
  built; a change to the host's DFU code needs `dfu-package` too, since it
  builds and runs the packaging tool; changes to `install.sh` need both
  `install-sh` jobs; changes under `host/doc` need the `Man pages` workflow,
  which `gh pr checks` prints under its job name, `refresh`.
  `issue-references`, `api-version`, `libbtbb-pin`, `scripts (ubuntu-24.04)`
  and `scripts (macos-26)` gate every change they run on, whatever the change
  touches: they take seconds to a minute, need nothing built, and a red one
  means the change itself broke the rule they check. A full-tier failure is
  followed up on `main`.
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
the firmware reports as the USB `bcdDevice`) is bumped **once per firmware
change the host relies on**, in the PR that makes it.

- A PR that adds, removes or changes a request, or changes the behaviour of an
  existing one, MUST bump the minor version and update `CHANGELOG.md`,
  `COMPATIBILITY.md`, `TEST_PLAN.md` and the pinned strings in the tests. The
  number is BCD, so after `0x0119` comes `0x0120`, not `0x011a`. Host-only PRs
  MUST NOT touch it. Take the value after whatever `main` carries, re-checking
  after a rebase.
- That list is the whole list, because **sample output** writes the version as
  `API:<version>` rather than a number, in the man pages, the README, `docs/`
  and the CHANGELOG alike. Keep it that way: pinning a real value in a sample
  means every bump has one more place to chase, and the ones that went stale
  were found by review rather than by any check. What a bump does update is
  `TEST_PLAN.md`'s expected `API:` string, `COMPATIBILITY.md`'s firmware row,
  the pinned strings in the tests and the CHANGELOG's reflash note -- the
  places that state the requirement rather than illustrate output. A version a
  sample is making a point about -- `btbr`'s fixed 1.06, or `2020-12-R1`'s
  1.07 -- is a fixed number and stays.
- The `api-version` CI job runs `tools/check-api-version.sh`, which compares
  the value on the branch with `main`'s tip and with their merge base and
  applies the two halves of the rule above. Run it before pushing: a value
  that was free when the branch was cut may not be free now.
- To decide whether a branch changes the firmware interface, that check
  compares the **content** of a list of files, whole-line comments and blank
  lines not counted: the sources compiled into the `bluetooth_rxtx` image,
  that image's own Makefile and linker scripts, and the interface header
  (`tools/check-api-version.sh --list-watched` prints the list). In
  `firmware/common.mk` it compares only the lines that set one of a listed set
  of compile flags -- the board define, `-DTX_ENABLE`, `-fpack-struct` and the
  rest of the list in the script -- and ignores every other line of that file.
  A watched name counts as a whole name and only where the line sets it: an
  assignment to that exact name, a `-D` or `-U` flag naming it, or a
  conditional on it. A line that merely uses one, such as the link command
  that passes `$(LINKER_SCRIPT)` to the linker, and a longer name that begins
  with a watched one, such as `LINKER_SCRIPT_DIRS`, do not count.
  It is an approximation, so it is not the rule, the paragraph above is. A
  firmware change the host relies on that touches nothing on that list still
  needs the bump by hand, and a change to a watched file that the host cannot
  observe is still asked for one. The false reds to expect: a change to the
  BTBR or BTCTL half of the interface header, which belongs to the separate
  `btbr` image; a change to one of the image's sources the host never reads,
  such as the LED patterns or the debug UART; and code moved verbatim between
  two watched files.
- So `api-version` is the one job in the gating set whose red is sometimes
  wrong. If it refuses a branch that changes nothing the host sees, do NOT
  bump the version to clear it and do NOT edit the watched list to suit one
  branch: say on the PR which files it named and why none of them changes
  what the host sees, and leave it for the maintainer to merge red.
- A release moves it to the next major version to mark the break in
  host/firmware pairing; the first fork release takes `0x0200` (2.00). That is
  a release step, not a per-PR one (see `docs/source/release_procedure.rst`).
  The check knows about it: one major up **from the merge base**, with the
  minor digits at `00`, above `main`, and with `CHANGELOG.md`,
  `COMPATIBILITY.md` and `TEST_PLAN.md` moving with it, is allowed with no
  firmware change, so the release's own PR is not refused. **No other branch may take a major version**, whether or not it
  changes the firmware: a board accepts only the exact value, so a `0x0200`
  typed in place of `0x0135` would refuse every user's board until it was
  reflashed, and it burns the value the release reserves.
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

- **Main sections** (New features, Reliability fixes, Build and compatibility,
  Documentation): what a user sees change in normal use — a wedge or a hang,
  wrong data, a new or changed option, a message or an exit code.
- **"Under the hood"**, the last section of the release: real fixes with no
  visible change in normal use — firmware bounds, retries, buffer limits,
  resources given back, edge cases only stress testing or odd hardware reaches.
  One line each, plain words, no function names.
- **No entry at all**: tests, CI, internal refactors, test-only firmware
  images (`usb_test`, `assembly_test`) and development-only tools. Shipped
  tools and images, `btbr` and `ubertooth-btbr` included, do get entries.

When unsure which bucket a change belongs in, ask rather than guessing.

Within a bucket:

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

Maintainer review MAY be omitted only when every changed path is limited to one
or more of these low-stakes categories:

- prose-only spelling, formatting, link, or clarity corrections that do not
  change documented commands, safety guidance, compatibility claims, or
  promised behavior;
- changelog condensation that preserves every distinct user-facing change,
  useful issue/pull-request links, and external credit;
- comments-only changes with no generated or compiled output; or
- additive tests, or any change limited to repository-local agent rules
  (`.kiro/steering/`), that cannot alter shipped artifacts, runtime behavior,
  or security controls.

The issue or review record MUST state why the candidate qualifies. A candidate
is NOT low-stakes if it changes source or firmware, build/install/package
behavior, configuration, automation workflows, dependencies, generated
artifacts, security or privacy behavior, release contents, or any operational
command. If classification is uncertain, MUST treat it as requiring maintainer
review.

For every candidate outside that exception, fetch the authoritative remote,
present the exact final diff to the maintainer, and MUST wait for explicit
maintainer approval of that candidate.
