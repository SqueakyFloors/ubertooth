# Release validation evidence

One folder per release, named after its tag: `2026-11-RC1/`, `2026-11-R1/`.
Development (`-DEVn`) tags get none. Each folder holds three files:

| File | Contents |
|---|---|
| `summary.md` | The commits tested, the plans and suites run, UTC start and end, the totals, the measurements that decide the release, and the verdict. Fill in [summary-template.md](summary-template.md). |
| `logs.tar.zst` | One archive of the sanitized run reports, event logs, and command output. |
| `SHA256SUMS` | Checksums of the other files in the folder. |

The harness builds the bundle; see
[its release-testing procedure](https://github.com/SqueakyFloors/ubertooth-test-harness/blob/main/docs/release-testing.md)
and the release-validation section of [the parent README](../README.md).
Committing it here is the release owner's own step, never automatic, and the
release notes link the folder's `summary.md` together with the harness version
used.

## Sanitizing

Everything here is public. Remove local paths, machine names, private aliases,
operator details, hardware serials, and captured traffic unrelated to the test.
Keep public commits, release names, tool and firmware versions, UTC times, plan
and suite names, aggregate measurements, and verdicts. A required gate that was
not run is named in `summary.md` rather than left out. The unredacted records
stay on the test host.
