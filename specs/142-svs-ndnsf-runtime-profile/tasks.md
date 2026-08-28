# Tasks: NDN-SVS Worker Validation Under the NDNSF Runtime Profile

**Input**: Design documents in
`specs/142-svs-ndnsf-runtime-profile/`

**Status**: Closed with a negative 400 pps qualification. The conditional
600/800 stage was not authorized and no higher-rate cell was started.

## Phase 1: Effective Profile And Evidence Foundation

- [x] T001 [US1] Deliver a source-verified effective-profile path by updating
  `Experiments/ndn-svs-pubsub-benchmark/svs-rsa-single-worker.cpp`, adding
  profile/linkage contract tests in
  `tests/python/test_spec142_svs_ndnsf_profile_worker.py`, and emitting a
  content-addressed `RuntimeProfileManifest` that proves V3, effective
  800-byte piggyback, NDNSF adaptive Fetch window, matched parallel Sync
  settings, RSA-2048, four-CPU affinity, exact runtime library hashes, signed
  publication wire size, and zero mode-independent differences.

**Traceability**: FR-002, FR-004, FR-005, FR-006, FR-007, FR-009, FR-010,
FR-011; SC-001.

**Gate**: The profile test fails before implementation and passes afterward;
the manifest rejects the former V2/one-byte profile, an effective-value/env
value mismatch, and installed/workspace library mixing.

## Phase 2: Corrected 400 pps Qualification

- [x] T002 [US2] Deliver the staged MiniNDN runner and strict receipt validator
  in `Experiments/NDN_SVS_NDNSF_Profile_Worker_Minindn.py` and
  `tests/python/test_spec142_svs_ndnsf_profile_worker.py`, including independent
  pacer qualification, two-node bidirectional operation, exactly-once cell
  ownership, 10/60/10 timing, raw-sample retention, resolved-profile parity,
  RSA/security checks, Fetch-health counters, and deterministic
  `PROFILE_VALID`/`PROFILE_INVALID`/`HARNESS_FAILED` classification.

**Traceability**: FR-003, FR-008, FR-010, FR-012, FR-013, FR-015, FR-016.

**Gate**: Focused tests cover +/-2% attempted-rate rejection, any
retry/timeout/Nack rejection, profile mismatch, process/security/accounting
failure, terminal receipt uniqueness, and refusal to authorize the formal
stage before a valid qualification pair.

- [x] T003 [US2] Build and freeze the exact binary/library manifest, run the
  pacer-only maximum-rate check, then run exactly one fresh 400-inline and one
  fresh 400-worker MiniNDN cell in
  `results/spec142-svs-ndnsf-runtime-profile/<campaign-id>/`; preserve both
  outcomes and issue the immutable qualification verdict without tuning or
  replacement.

**Traceability**: FR-012, FR-013, FR-014, FR-015, FR-016; SC-002, SC-003.

**Gate**: Both started cells have one terminal receipt. The formal stage remains
locked unless both receipts pass identity, profile, attempted-rate, process,
security, Fetch-health, and raw-accounting invariants.

**Observed result**: The canonical r4 campaign
`campaign-20260724T012559Z` preserved one complete inline and one complete
worker receipt. Both were `PROFILE_INVALID` because retry/timeout recovery
activated during the 60-second measurement window. Earlier r2/r3 development
campaigns remain preserved as non-canonical defect evidence; they are not
pooled with or substituted for r4.

## Phase 3: Conditional Higher-Rate Boundary

- [x] T004 [US3] If and only if T003's qualification verdict passes, run exactly
  the four authorized 600/800 MiniNDN cells using the unchanged manifest and
  preserve every success, unsustained load, invalid profile, or harness failure
  in `results/spec142-svs-ndnsf-runtime-profile/<campaign-id>/`; otherwise
  record the controlling qualification failure and do not start these cells.

**Traceability**: FR-014, FR-015, FR-016; SC-003.

**Gate**: Each started cell has exactly one terminal receipt, zero automatic
reruns, and a manifest hash identical to the 400 pps pair.

**Observed result**: The qualification verdict was `FAIL`; the runner kept the
formal stage locked, and zero 600/800 cells were started.

- [x] T005 [US3] Deliver the validity-first analyzer and final report using
  `Experiments/analyze_svs_ndnsf_profile_worker.py`,
  `tests/python/test_spec142_svs_ndnsf_profile_worker.py`, and
  `specs/142-svs-ndnsf-runtime-profile/evidence/final-report.md`: recompute raw
  counts and mean/p50/p95/p99, report piggyback/Fetch/retry/timeout/Nack/RSA/CPU
  metrics, compare only paired `PROFILE_VALID` receipts, label survivor
  distributions, and state the two-node/zero-loss/microbenchmark claim boundary.

**Traceability**: FR-001, FR-017, FR-018, FR-019; SC-004, SC-005.

**Gate**: The analyzer rejects synthetic fixture mismatches and the final
pre/post implementation audit confirms that no Spec 140/141 artifact was
modified or reused as corrected evidence.

**Evidence**:

- `evidence/build-and-test-gate.md`
- `evidence/final-analysis.json`
- `evidence/final-report.md`
- `evidence/ars-validation.md`
- `evidence/post-implementation-audit.md`

## Dependencies And Execution Order

```text
T001 -> T002 -> T003
                |
                +-- qualification PASS -> T004 -> T005
                |
                +-- qualification FAIL ----------> T005 (failure-boundary report)
```

The implementation is intentionally five cohesive outcomes. Test-first work,
implementation, focused validation, and evidence remain inside the task that
owns the behavior.
