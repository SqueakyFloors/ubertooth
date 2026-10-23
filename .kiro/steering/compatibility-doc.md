---
inclusion: always
---
# Keep COMPATIBILITY.md Up to Date

`COMPATIBILITY.md` is the authority on what this fork requires, was tested
against, and does not support. Keep it correct in the SAME change (or an
adjacent commit on the same branch) as whatever alters it.

## Update it when a change

- raises, lowers, or newly pins a requirement: CMake minimum or policy range,
  GNU make, Python, compiler (host GCC or `arm-none-eabi-gcc`), `libusb`,
  `libbtbb`, `setuptools`, or any other build or runtime dependency;
- adds, removes, or changes a supported or unsupported operating system or
  distribution version, including `install.sh` distribution support;
- changes the CI matrix (`.github/workflows/`), including runners, versions, or
  which jobs build host tools, firmware, or run on hardware;
- adds a platform-specific workaround, limitation, or known issue that a user
  would need to know before building or running the tools;
- produces new verification evidence for a platform, toolchain, or device, for
  example a build, test run, or hardware run on a new OS or version.

## How to update it

- Follow the document's honesty rule: mark a cell *Verified* only if it was
  exercised by CI or on real hardware. Use *Target (unverified)* for intended
  but unproven support. Never claim support that has not been proven.
- Record what was verified and how (CI job, hardware, date) in the matrix or the
  relevant compatibility note.
- If a verified claim is no longer true, downgrade it in the same change.
- Updating the matrix or other docs does not need a changelog entry. A change
  to the code or build that alters support gets its own changelog entry as usual.

## Check before finishing

Before presenting work as done, ask: did this change or verify anything listed
above? If yes and `COMPATIBILITY.md` is unchanged, update it or state why no
update is needed in the PR description.
