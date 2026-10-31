# AI disclosure

This fork of Ubertooth was developed largely with the help of AI coding agents,
directed by the maintainer. This note explains how, so that anyone using,
packaging, or reviewing the fork can judge it with that in mind.

## What this means

- The maintainer directed the work, reviewed it, and takes responsibility for
  the result. The agents were a tool; the decisions, and the accountability,
  are human.
- Most of the code, test, and documentation changes in this fork since the
  upstream `2020-12-R1` release were written or revised by large-language-model
  coding agents working to a written ruleset, with a human reviewing what
  landed.
- Non-trvial changes were reviewed in one or more rounds by two independent 
  agents on a different model family before being considered for merge.

## What the agents were and were not allowed to do

Agents followed a written set of rules. In short:

- An agent could merge a change on its own only when the change was low-stakes
  and its correct behavior was already settled — by the documentation, the man
  pages, the command-line conventions, or a decision the maintainer had already
  made — and only after its tests passed.
- Everything else was prepared and left for the maintainer to decide and merge:
  new features or options, behavior changes, firmware API or dependency
  changes, bootloader or flash-layout work, and anything an agent was unsure
  about.
- Design questions were escalated to the maintainer rather than guessed.

## How changes were verified

- Every change was built and unit-tested, with fakes standing in for hardware
  where needed.
- Changes that affect what a real board does were, where possible, tested on
  real Ubertooth hardware, and the test was recorded in the pull request.
- Where a change could not yet be exercised on hardware, it was labeled clearly
  as pending a hardware test rather than presented as verified.

## Honesty about scope

This was also an exercise in learning to orchestrate AI agents, and the fork's
footprint is large. Not every change was strictly necessary. Before a release,
the fork is audited to remove additions that are out of proportion, to trim
comments back to the project's original style, and to keep only tests that earn
their place. The aim is that what remains is of real value — correctness fixes
to packet decoding and capture files, firmware reliability fixes, extensive 
unit tests, and improved documentation.

## Credit

Ubertooth was created by Great Scott Gadgets, with Michael Ossmann, and its
tools and firmware are the work of that team and many contributors over many
years. This fork builds on theirs. Where it fixes or extends something, it owes
the original a great deal.

## Licensing

Contributions here, however they were produced, are offered under the same
license as the rest of this repository (see `LICENSE`).
