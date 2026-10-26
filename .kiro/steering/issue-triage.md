---
inclusion: always
---
# Issue Triage: Release, Post-Release, Backburner

Every open issue carries exactly one of these labels. Work on `release`
issues first; do not start `post-release` or `backburner` work while
`release` issues are open, unless the maintainer asks.

- **`release`**: must be fixed before the first fork release. Either an
  **advertised feature does not work** (a tool, mode or option the README, a
  man page or `-h` offers, including the `btbr` image and two-board modes such
  as the range test; the goal is that every advertised feature works, or is
  removed or documented as not working), or a user doing something ordinary
  would hit it and it gives wrong data, hangs or wedges a board, crashes or
  corrupts memory, breaks flashing or recovery, breaks the build or install on
  a supported platform, or acts on stale or unchecked data from the host. A
  feature that cannot be tested here still counts: fix it with tests and leave
  it `pending hw test`.
- **`post-release`**: real but low impact. Inconsistent messages or exit
  codes, option combinations nobody uses, leaks in short-lived tools (freed at
  exit anyway), docs gaps, test and CI improvements.
- **`backburner`**: new features and design work deferred on purpose (not broken advertised features).

Also used: `needs maintainer` (a decision is needed), `pending hw test`
(merged or ready, hardware validation outstanding).

## Filing an issue

- First line: **User impact:** one sentence on what a user sees go wrong, and
  how often they would hit it.
- Apply `release` or `post-release` by the rules above. When unsure, use
  `post-release` and say why.
- Search for an existing issue first; add to it rather than filing a
  duplicate.
- Several small consistency findings in one tool go in one issue, not one
  issue each.
