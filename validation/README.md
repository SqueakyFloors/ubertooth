# Validation

This folder holds the hardware validation material for this fork:

- **The manual checklist**, [TEST_PLAN.md](TEST_PLAN.md): the tests a person
  runs on a real Ubertooth per release, with the command and the expected
  result for each.
- **The lab setup** used for validation: which Ubertooths, peer radios, and
  hosts are on the bench, and how they are wired. It will be written here from
  the configuration of the
  [test harness](https://github.com/SqueakyFloors/ubertooth-test-harness), so
  that it describes the bench as it actually is rather than a plan for one.
- **The evidence kept with each release**, in [releases/](releases/README.md):
  what was tested, on which commits and hardware, and what the results were.

## Release validation

The [test harness](https://github.com/SqueakyFloors/ubertooth-test-harness)
qualifies a release. Its
[release-testing procedure](https://github.com/SqueakyFloors/ubertooth-test-harness/blob/main/docs/release-testing.md)
holds the required test matrix and the schedule, and its
[hardware document](https://github.com/SqueakyFloors/ubertooth-test-harness/blob/main/docs/hardware.md)
describes the bench.

A release is qualified by one fork commit, the libbtbb commit that commit pins,
and one harness commit, all resolved before the first run and recorded in every
report. The required gates are the rows of that procedure's required test
matrix, run from those commits; the whole set must finish within six hours.
Soaks longer than the four-hour stability gate, the 24-hour and 72-hour runs
among them, are optional rather than gates, and are recorded separately so
their absence does not read as a missing gate.

Every required gate must pass. A required test that failed, was blocked,
skipped, or whose hardware was unavailable leaves the release unqualified until
it is resolved, or until the maintainer waives it in that release's
`summary.md`.
