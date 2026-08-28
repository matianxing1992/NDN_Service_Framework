# Tasks: Single-Worker Necessity Confirmation

**Input**: Design documents in `specs/138-svs-worker-necessity/`

## Phase 1: Freeze The Successor Contract

- [x] T001 Audit Spec 138 against the original two-mode objective, current
  NDN-SVS/benchmark source, Spec 137 frozen hashes, four-core ownership,
  control-only pressure selection, necessity decision, once-only receipts, and
  task cohesion; record a pre-implementation PASS in
  `specs/138-svs-worker-necessity/evidence/pre-implementation-audit.md`.

## Phase 2: Admit The Same Binary And Select Pressure

- [x] T002 [US1] Implement test-first Spec 138 same-subject admission,
  protected-hash checks, runtime-delta verification, one-worker/one-signer
  invariants, immutable receipt primitives, and complete base evidence parsing
  in
  `Experiments/NDN_SVS_Worker_Necessity_Minindn.py`,
  `Experiments/analyze_svs_worker_necessity.py`, and
  `tests/python/test_spec138_svs_worker_necessity.py`; verify the exact frozen
  binary in a fresh preflight without creating a calibration or formal receipt.

- [x] T003 [US2] Add and test bounded host-quiescence admission, per-core
  contamination evidence, control-only descending calibration, one-worker
  qualification, and immutable rate selection in
  `Experiments/NDN_SVS_Worker_Necessity_Minindn.py`,
  `Experiments/analyze_svs_worker_necessity.py`, and
  `tests/python/test_spec138_svs_worker_necessity.py`; run the fresh
  calibration/qualification once and stop without formal receipts if no valid
  pressure rate exists.

## Phase 3: Execute The Once-Only Confirmation

- [ ] T004 [US3] Seal the selected rate and exact AB/BA/AB matrix, display the
  manifest, and execute all six 10/60/10 MiniNDN cells once under
  `results/spec138-svs-worker-necessity/<campaign-id>/`, preserving every
  terminal or contaminated receipt without retry.

## Phase 4: Decide And Freeze

- [ ] T005 [US3] Reproduce all hashes and emit run, paired, stage, traffic,
  host-load, and conclusion outputs plus
  `specs/138-svs-worker-necessity/evidence/worker-necessity-report.md`; apply
  the registered taxonomy, perform the post-implementation Spec Kit audit,
  verify Spec 137 is unchanged, and freeze Spec 138.

## Dependencies

```text
T001 -> T002 -> T003 -> T004 -> T005
```

No task is parallel: the subject gate, rate authority, irreversible execution,
and conclusion form one dependency chain.
