---
inclusion: always
---
# PR Descriptions and Code Comments: Short and Current

The code is well commented, so a PR description does not need to repeat it:
keep the description short and let the code and its comments carry the detail.

## PR description (the PR body)

Keep it short and keep it current. The visible part fits on one screen and
reads in a minute, in this order:

1. **The problem, in plain language** (2-4 sentences): what a user saw go
   wrong, or what was missing, and what is different after this PR. A
   before/after example of what the user sees is the best way to show it. No
   function names or commit hashes here.
2. **One small results table**, where the change produces numbers: before
   against after, only the figures that decide it, about six rows at most.
   Say plainly what the change has not been shown to do.
3. **One short excerpt** of real output, three to eight lines, where there is
   output to show.

Everything else goes in `<details><summary>...</summary>` sections:

- the setup and the boards, and the full commands and logs;
- how the change works;
- the tests, with the hardware line (board, firmware, the `main` commit it was
  based on), or "Hardware: pending" and the `pending hw test` label;
- the review findings, one line per finding fixed, deferred (with the issue
  number) or rejected (with the reason), naming each reviewer's model and
  verdict;
- the rebase notes, and what the runs do not show.

When the PR changes direction, rewrite the body to describe what the PR does
now. Delete superseded text rather than appending to it; history lives in the
commits.

## PR comments

Post a comment only for something a reader of the PR needs that does not
belong in the body (a hardware result table, a question for the maintainer).
Do not narrate progress. When a later comment supersedes an earlier one of
yours, delete or shorten the earlier one.

## Code comments

- Comments say why, not what: a constraint, a hardware quirk, a non-obvious
  reason. The code already says what it does.
- No change history, review history or issue narratives in comments (the
  commits and PRs hold those), and no issue numbers (see "Issue references").
