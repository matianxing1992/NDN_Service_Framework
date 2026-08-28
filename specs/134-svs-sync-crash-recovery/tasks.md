# Tasks: Historical NDN-SVS Threading-Contract Recovery

**Input**: Design documents in `specs/134-svs-sync-crash-recovery/`

**Execution boundary**: Existing cross-thread diagnostics are immutable and
ineligible for formal profiling. No NDN-SVS repair is authorized.

## Phase 1: Source contract and evidence correction

- [x] T001 [US1] Audit the exact historical README, public header, examples, unit tests, source synchronization, async/parallel introduction commits, and Spec 133/134 runtime evidence; record what each source proves, resolve the contradiction without inventing a contract, and update Specs 133/134 plus `evidence/threading-contract-audit.md`.
- [x] T002 [US1] Preserve the existing ASan/UBSan and TSan outputs, original cross-thread driver/manifests, and experimental repair identities as immutable diagnostic evidence; label them ineligible for formal performance or single-I/O-path defect claims and prohibit merge/runtime reuse.

**Checkpoint**: The project no longer claims that an example comment is a
README/API/test-backed contract.

## Phase 2: Correct single-I/O-thread harness

- [x] T003 [US2] Add fail-first source contracts, then implement a new `svs-sync-io-qualification.cpp` and compatible builder/runner mode with exactly two MiniNDN nodes/processes, one Face/io_context execution thread per process, one absolute-deadline application timer, synchronous `publish()` on that thread, skipped-release accounting, no catch-up burst, correct local-vs-remote delivery classification, exact hashes/linkage, bounded cleanup, unique paths, and no disallowed async/worker/publisher/pacer/cross-thread behavior.

**Checkpoint**: Local/source tests prove the corrected call model before any
network qualification is consumed.

## Phase 3: Qualify once

- [x] T004 [US2] Build the exact clean historical subject plus only the canonical Boost 1.71 patch, freeze the corrected driver/binary/library hashes and linkage, and execute exactly one fresh 1000-target-pps two-peer MiniNDN qualification with 256-byte payload and 10/60/10 timing; preserve one terminal receipt and do not retry. The immutable outcome is `NOT_QUALIFIED`; it was not rerun.

**Checkpoint**: The result is `QUALIFIED`, `NOT_QUALIFIED`, or
`INFRA_FAILURE`; it is never a throughput/bottleneck result.

## Phase 4: Gate the real profiling experiment

- [x] T005 [US3] Reconcile the terminal negative result before touching Spec 133. The receipt remains `NOT_QUALIFIED`, but saved route evidence from the same runner family proves the remote publication route was never installed after an ignored `Error 409`. Do not rerun or relabel Spec 134. Correct routing under Spec 133's new driver/manifest identity, require verified dual-prefix RIB evidence, and admit formal execution only through a fresh three-arm Spec 133 preflight. That new preflight passed before any of its five formal cells was consumed.
- [x] T006 [US3] Run the post-implementation audit and reconcile both Specs' traceability, task counts, evidence labels, and next-step gate without modifying any previous result.

## Dependencies

```text
T001 + T002 -> T003 -> T004 -> T005 -> T006
```

## Cohesion Review

- T001 closes one source-contract decision and its document consequences.
- T002 independently preserves/reclassifies non-repeatable evidence.
- T003 owns the complete corrected harness and its tests because they share one
  acceptance gate.
- T004 is separate because it consumes the once-only network qualification.
- T005/T006 depend on the terminal qualification and cannot be checked early.
