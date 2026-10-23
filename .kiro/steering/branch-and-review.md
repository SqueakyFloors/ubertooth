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

- CI runs on pull requests and on pushes to `main` only. A branch with no open
  PR gets no CI; open a PR (a draft is enough) to get a run.
- A newer commit on a PR cancels that PR's older run. Read results with
  `gh pr checks <PR>`, which reports the current head commit, not with the id of
  a run you saw earlier. A "cancelled" run for a superseded commit is expected,
  not a failure.
- Only the run for the head commit that will be merged counts. Wait for it to
  finish before merging; do not merge on a cancelled, queued, or older run.
- Runs for `main` and the weekly schedule are never cancelled.
- Cancelling runs yourself is for runs you made redundant (for example a PR you
  closed as superseded); never cancel another agent's or PR's runs.

## Changelog discipline

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

## Documentation clarity

- MUST expand each acronym at its first use in a document or independently read
  section, except universally understood command names and file extensions.
- MUST preserve exact error strings that users are likely to search for, such
  as `ModuleNotFoundError: No module named 'specan'`, in the relevant
  troubleshooting documentation even when changelog prose is condensed.

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
