# Spec184 B3 checkpoint export evidence — 2026-09-11

## Status

`DYNAMIC_PASS / focused B3 validation; formal qualification pending`.

T004 is closed for this batch. This record proves the native checkpoint export
boundary and its C++ selectors; it does not prove T005 caller convergence or
the T006–T008 qualification and delivery gates.

## Dynamic Gate Card

| Field | Frozen value |
| --- | --- |
| Risk class | `lifetime/serialization` |
| Dynamic profile | `asan-ubsan` |
| C++ selector | `Spec184NativeCheckpoint/*` |
| Parameter boundary | new file; existing checkpoint; destination symlink; injected pre-rename failure; runtime loader path |
| Invariants | temporary file is regular, owned by effective user and `0600`; bytes are canonical JSON; symlink is rejected; pre-rename failure leaves old checkpoint; no sanitizer report |
| Repeats | normal selector once; unsuppressed ASan/UBSan selector three times |
| Toolchain/source identity | `/usr/bin/g++ -B/usr/bin`; system Boost 1.71; NAC-ABE install `nac-abe-integration-182/install`; current NDN-SVS source and rebuilt `ndn-svs/build`; ONNX prefix `spec182-t001-dependencies/onnx-install` |
| Output roots | `.codex-tmp/spec184-b3/`; normal `build-spec184-b2-normal`; sanitizer `build-spec184-b2-asan`; example `build-spec184-b3-examples` |

## Implemented boundary

`NativeCheckpointExport` now owns the native export path used by
`examples/DI_NativeRequester.cpp`. It creates a same-directory temporary file
with `O_EXCL|O_NOFOLLOW`, verifies regular-file ownership and mode `0600`, writes
canonical JSON, calls file `fsync`, closes, atomically renames, and calls the
parent-directory `fsync`. The destination is rejected when it is a symlink.
Failures before rename remove only the temporary file, preserving an existing
checkpoint. The public test hook is empty in production and is used only to
inject a deterministic pre-rename failure in the C++ fixture.

## C++ validation

The initial normal unit build compiled 192 actions with `-j4` in `5:49.80` and
peak RSS `1,658,444 KB` (`.codex-tmp/spec184-b3/normal-build.log`). After the
residue assertions were added, the focused rebuild completed in `16.04s` with
peak RSS `1,142,096 KB` (`normal-build-rerun.log`). The final selector ran all
three cases—successful export, symlink rejection, and old-checkpoint
preservation—and exited 0 with `*** No errors detected`
(`normal-test-rerun.log`).

The independent sanitizer build initially compiled 192 actions with `-j4` in
`9:04.37` and peak RSS `2,933,040 KB` (`.codex-tmp/spec184-b3/asan-build.log`).
The final test-only rebuild completed in `27.11s` with peak RSS `2,933,292 KB`
(`asan-build-rerun.log`). The selector was then repeated three times with unsuppressed
`ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; every run exited 0 with no
`ERROR`, `SUMMARY`, or `runtime error` (`asan-test-rerun1.log`,
`asan-test-rerun2.log`, `asan-test-rerun3.log`).

The example target was configured with `--with-examples` and built in 6:07.27
(`.codex-tmp/spec184-b3/examples-build.log`). A loader smoke using the example
build directory first in `LD_LIBRARY_PATH` returned the native usage contract
(`.codex-tmp/spec184-b3/examples-help-rpath.log`). An earlier smoke with
`/usr/local/lib` ahead of the example output selected a stale framework shared
library and failed symbol lookup; that is recorded as a library-path provenance
boundary, not as a product pass, and is why the evidence command pins the
candidate output directory first.

## Identity and limits

| Artifact | SHA-256 |
| --- | --- |
| normal unit binary | `f57835c06ec31849b16f8df1193921275bc652a024e0844c0cf9ff24c48c4a28` |
| ASan/UBSan unit binary | `4c7a46f3e6c085b6bda9f70a61276965e08d87ed50e1d2357b0b10d32fa55330` |
| `DI_NativeRequester` | `19e5affcd01ec2082fab3275be74d16aef95e7fa00e73e567879cea02c878ea4` |
| native DI shared library | `1f99dbd6a691ff516d53f395b6ef50f72dd6264dbd7e46cc0c201b627c591677` |

The fixture injects failure before rename; it does not force a real kernel
directory-`fsync` error. A directory-sync error occurs after the atomic rename,
so its durability outcome remains an explicit production limitation for later
qualification. This focused evidence also does not establish the complete
requester/provider path, caller mode matrix, or no-Python qualification.

## Static review record

The complete B3 source, example, test, and task/evidence diff was reviewed
read-only against baseline `eda9e346` using
`/home/tianxing/.codex/skills/review-agent/SKILL.md` (SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`). No
actionable P1–P3 finding was identified: the requester calls the native helper,
the helper owns temporary-file cleanup and symlink refusal, and the C++ fixture
exercises each injected pre-rename boundary. The `/usr/local/lib` loader result
was classified as an environment provenance failure because candidate-first
library ordering makes the same binary start and print its usage contract.
The unforced directory-`fsync` error remains recorded above as a qualification
limit rather than being hidden by a sanitizer suppression.
