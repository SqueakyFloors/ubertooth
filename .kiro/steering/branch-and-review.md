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

## Independent agent review

Every merge candidate containing code, configuration, workflow, policy, or
non-trivial operational documentation changes:

- MUST receive at least two independent read-only agent reviews.
- MUST use different recorded model identifiers or model families; two prompts
  or roles on the same model do not satisfy this requirement.
- MUST give each reviewer the same final merge-base diff and acceptance criteria
  without sharing the other reviewer's conclusions.
- SHOULD run reviewers in parallel.
- MUST resolve every blocker and rerun affected validation.
- If any candidate content changes after a blocker is reported, both reviewers
  MUST review the resulting final diff again.
- If two different review models are unavailable, MUST disclose that the gate
  is unsatisfied and MUST NOT describe the change as review-complete.

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

## Documentation clarity

- MUST expand each acronym at its first use in a document or independently read
  section, except universally understood command names and file extensions.
- MUST preserve exact error strings that users are likely to search for, such
  as `ModuleNotFoundError: No module named 'specan'`, in the relevant
  troubleshooting documentation even when changelog prose is condensed.

## Public merge gate

Agent reviews supplement rather than replace maintainer review where it is
required. Before merging into `main`, every final branch MUST satisfy all
existing clean-tree, latest-main, one-commit, test, and independent-review
gates. Determine the authoritative main remote from `branch.main.remote` or
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
- additive tests or repository-local agent/process guidance that cannot alter
  shipped artifacts, runtime behavior, or security controls.

The issue or review record MUST state why the candidate qualifies. A candidate
is NOT low-stakes if it changes source or firmware, build/install/package
behavior, configuration, automation workflows, dependencies, generated
artifacts, security or privacy behavior, release contents, or any operational
command. If classification is uncertain, MUST treat it as requiring maintainer
review.

For every candidate outside that exception, fetch the authoritative remote,
present the exact final diff to the maintainer, and MUST wait for explicit
maintainer approval of that candidate.
