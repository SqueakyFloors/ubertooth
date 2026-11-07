# Validation summary: RELEASE-TAG

| | |
|---|---|
| Fork commit | `SHA` (RELEASE-TAG) |
| libbtbb commit | `SHA` |
| Harness commit | `SHA` |
| Hosts | platform classes used |
| Start, end (UTC) | START, END (DURATION) |
| Verdict | qualified, or not qualified |

## Required gates

One row per row of the harness required test matrix.

| Gate | Plan and suite | Result | Notes |
|---|---|---|---|
| GATE | `testplans/PLAN.yaml`, `SUITE` | pass, fail, blocked, or skipped | disposition, or why it was not run |

## Measurements

The figures that decide the release: packet counts and rates, the measured
receive floors of the weak-link checks, restarts and memory over the stability
run, and the versions read back after each flash.

## Deviations and waivers

Each required gate that failed, was blocked, skipped, or had no hardware, with
what was done about it. A maintainer's waiver is recorded here in their own
words.

## Optional work

Soaks and other runs that are not gates, with their results, or "none".
