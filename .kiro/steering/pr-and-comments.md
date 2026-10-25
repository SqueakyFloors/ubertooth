---
inclusion: always
---
# PR Descriptions and Code Comments: Short and Current

The code should speak for itself. PR text and code comments explain what the
code cannot, briefly.

## PR description (the PR body)

Keep it short and keep it current. In this order:

1. **The problem, in plain language** (2-4 sentences): what a user saw go
   wrong, or what was missing, and what is different after this PR. No
   function names here. A before/after example of what the user sees is the
   best way to show it.
2. **How** (a few bullets): the technical change, enough for a reviewer to
   know where to look.
3. **Tests** (a few lines): what was run and the result, including the
   hardware line (board, firmware, the `main` commit it was based on), or
   "Hardware: pending" with the `pending hw test` label.
4. **Review fixes** (a short list or table): one line per finding that was
   fixed, deferred (with the issue number) or rejected (with the reason).

When the PR changes direction, rewrite the body to describe what the PR does
now. Delete superseded text rather than appending to it; history lives in the
commits. Aim for one screen.

## PR comments

Post a comment only for something a reader of the PR needs that does not
belong in the body (a hardware result table, a question for the maintainer).
Do not narrate progress. When a later comment supersedes an earlier one of
yours, delete or shorten the earlier one.

## Code comments

- Comment why, not what: a constraint, a hardware quirk, a non-obvious reason.
  If the code says it, do not repeat it.
- One or two lines is normal; a paragraph needs a reason.
- No change history, review history or issue narratives in comments (the
  commits and PRs hold those), and no issue numbers (see "Issue references").
- When you change code, trim or delete comments it made stale.
