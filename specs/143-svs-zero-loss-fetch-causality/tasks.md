# Tasks: NDN-SVS Zero-Loss Fetch Timeout Causality

**Input**: Design documents in
`specs/143-svs-zero-loss-fetch-causality/`

**Status**: Complete — pre-implementation audit passed, the once-only
diagnostic cell closed `DIAGNOSED`, and the post-implementation audit passed
for the diagnosis-only scope.

## Phase 1: Causal Observability

- [x] T001 [US1] Deliver bounded causal observability and process-resource
  snapshots by adding structured Fetcher, SVSyncBase, MappingProvider, and
  SVSPubSub NDN logs in `/home/tianxing/NDN/ndn-svs/ndn-svs/`, adding the Spec
  143 compatible measurement resource fields to
  `Experiments/ndn-svs-pubsub-benchmark/svs-rsa-single-worker.cpp`, and adding
  focused NDN-SVS/Python tests that prove event identity, retry-attempt
  separation, unchanged packet/state behavior, valid CPU arithmetic, and
  compatibility with the earlier summary schemas.

**Traceability**: FR-002–FR-004, FR-010, FR-013; SC-001, SC-004.

**Gate**: NDN-SVS focused/full tests and resource/schema tests pass. Source
review confirms logging adds no state transition, wire mutation, new public
API, or scheduling callback.

## Phase 2: Minimal Diagnostic Harness

- [x] T002 [US2] Deliver a synthetic-trace-first analyzer and an
  exactly-once MiniNDN runner in
  `Experiments/analyze_svs_zero_loss_fetch_causality.py`,
  `Experiments/NDN_SVS_Zero_Loss_Fetch_Causality_Minindn.py`,
  `Experiments/build_svs_zero_loss_fetch_causality.py`, and
  `tests/python/test_spec143_svs_zero_loss_fetch_causality.py`. The behavior
  includes all five timeout classes, full-name plus Nonce cross-peer
  correlation, raw event references, no cross-peer monotonic subtraction,
  separate inner/outer retry accounting, exact profile comparison, new
  result-root ownership, terminal receipt uniqueness, and a closed-by-default
  conditional inline gate.

**Traceability**: FR-005–FR-012, FR-014, FR-016; SC-001–SC-004.

**Gate**: Synthetic fixtures exercise each classification and malformed,
ambiguous, repeated-attempt, later-same-name-Data, zero-timeout,
profile-mismatch, and duplicate-cell cases. The runner dry run proves it cannot
write Spec 142 or start inline without a prior authorization record.

## Phase 3: Once-Only MiniNDN Evidence and Closure

- [x] T003 [US3] Freeze the exact Spec 143 build/runtime manifest, record
  the Spec 142 before-inventory, run one worker-rsa 400 pps/peer 10/60/10
  MiniNDN cell, analyze both peer traces and resource windows, retain every
  outcome without retry or tuning, record the Spec 142 after-inventory, and
  produce `evidence/final-report.md`, `evidence/ars-validation.md`, and a
  post-implementation audit. Mark the result `DIAGNOSED` only at at least 95%
  timeout coverage; otherwise mark it `INCONCLUSIVE` and identify the next
  missing seam.

**Traceability**: FR-001, FR-005–FR-017; SC-002–SC-005.

**Gate**: One terminal receipt exists; all raw artifacts and references hash
correctly; process/resource arithmetic recomputes; Spec 142 hashes are
unchanged; no recovery-policy or configuration change exists.

## Dependencies And Execution Order

```text
pre-implementation audit -> T001 -> T002 -> T003
```

The three tasks are cohesive outcomes: observability, diagnostic
classification/orchestration, and measured closure. No formal 600/800 matrix
belongs to Spec 143.

## Closure Evidence

- Canonical campaign:
  `results/spec143-svs-zero-loss-fetch-causality/diagnostic-20260724T020332Z`
- Exactly one network cell: `01-worker-rsa-400`
- Attempted/accepted load: 24,000/24,000 per peer in 60 seconds
- Timeout classification: 829/829, 100% coverage
- Tests: Spec 143 Python 20/20; Spec 142 compatibility 11/11; NDN-SVS 75/75;
  NDNSF 363/363
- Spec 142 baseline tree SHA-256 before/after:
  `12c7d504db09acaaa16241ea7fa410e7c50cac0cbb54c237852f856c8848cb3d`
- Analyzer correction used only the retained raw traces; the network cell was
  not rerun. See `01-worker-rsa-400/analysis-revision.json`.
