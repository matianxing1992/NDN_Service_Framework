# Tasks: Four-Provider Multi-AP Mobility Advantage

**Input**: [spec.md](spec.md), [plan.md](plan.md),
[experiment-contract.md](contracts/experiment-contract.md)

## Phase 1: Foundational contract

- [x] T001 [US1] Add the four-Provider campaign profile, endpoint/prefix
  registry, AP layout validation, speed/range arguments, deterministic trace
  schema, and no-oracle manifest checks in
  `Experiments/WifiRouterMobilityReliability.py` and
  `tests/python/test_mobility_harness_contract.py`; pass static contract tests
  while preserving the Spec169 three-Provider default.

## Phase 2: User Story 1 - matched four-Provider execution

- [x] T002 [US1] Generalize the gRPC failover client and its focused tests from
  exactly three targets to a declared four-target list while preserving one
  attempt per Provider, one logical deadline, explicit failover accounting,
  and the existing three-target compatibility gate in
  `Experiments/gRPC/greeter_failover_client.py` and
  `tests/python/test_grpc_three_provider_failover.py`.

- [x] T003 [US1] Generalize the NSC campaign wiring and focused tests to pass
  four Provider prefixes, preserve timeout/Nack-only sequential attempts and
  late-callback fencing, and retain the existing three-Provider behavior in
  `Experiments/NDN_NSC/consumer.cpp` and
  `tests/python/test_nsc_three_provider_failover_client.py`.

## Phase 3: User Story 2 - multi-AP and speed smoke

- [x] T004 [US2] Deliver the four-Provider multi-AP coverage trace and
  matched three-system smoke, including four Provider startup, one forced
  failover, `SMOKE_OK`, separate output paths, and terminal summary
  validation. Add strict `gRPC-SEQ-4` (no proactive health routing), preserve
  labelled `gRPC-HC-4` as a secondary diagnostic, and add the registered
  `single-active-handoff` trace with manifest/command plumbing and tests proving
  exactly one Provider is in coverage at each epoch.

## Phase 4: User Story 3 - evidence and claim gate

- [x] T005 [US3] Run the paired moderate and harsh 60-second screening cells
  against `gRPC-SEQ-4`, NSC-4, and NDNSF, retain `gRPC-HC-4` as a secondary
  diagnostic where requested, aggregate logical success and retry/control cost
  by trace, and write evidence under
  `specs/171-four-provider-mobility-advantage/evidence/`; do not rerun failed
  cells automatically.

- [x] T006 [US3] Execute the registered formal repetitions only if screening
  passes the pre-registered gate, compute paired run-level confidence bounds,
  and update the mobility claim in the paper/slides only when SC-005 is met;
  otherwise record `NO_DEMONSTRATED_ADVANTAGE` and preserve the old table as
  historical diagnostic evidence.

## Dependencies

- T001 precedes T002--T004.
- T002 and T003 can proceed independently after T001.
- T004 depends on both clients and the profile contract.
- T005 depends on the smoke gate passing.
- T006 depends on complete screening evidence and the claim contract.

## Completion Gate

The feature is not complete when only a smoke passes. Completion requires
trace-paired evidence for NDNSF, gRPC-SEQ-4, and NSC-4, explicit retry/control
metrics, and a claim decision that satisfies or falsifies SC-005/SC-005a.
`gRPC-HC-4` is optional sensitivity evidence and never replaces the strict
baseline.

## 2026-08-06 topology and oracle amendment

- [x] T007 [US2] Register the original one-AP/four-Provider range-speed pilot
  (50/100 m, 2/15 m/s, seeds 40/41/42), set gRPC/NSC attempts and NDNSF ACKs
  to 1 s, and make the custom application health oracle explicit opt-in in
  `Experiments/single_ap_range_speed_pilot.py` and the evidence plan.
- [x] T008 [US3] Run the finite pilot only after the smoke gate, then retain
  paired range/speed summaries and admit a positive claim only under SC-005a.
  The 36-cell primary matrix and six-cell single-provider no-failover control
  are recorded in `evidence/single-ap-range-speed-matrix-20260806.md`; the
  claim gate remains descriptive because the paired differences are mixed.
  The secondary 35 m/15 m/s lower-coverage retry-stress pilot is recorded in
  `evidence/low-coverage-stress-35m-15ms-20260806.md` and also reports
  `NO_DEMONSTRATED_ADVANTAGE`. The 70/80/90 m sweep and three-seed 90 m
  confirmation are recorded in `evidence/coverage-sweep-70-80-90-20260806.md`;
  the one-seed 90 m positive screen did not reproduce. The high-coverage
  110/120 m boundary screen is recorded in
  `evidence/coverage-sweep-110-120-20260806.md` and is non-positive.

- [x] T009 [US3] Add and execute a strictly paired 2 s attempt / 5 s global
  timeout sensitivity pilot on the registered 50 m / 2 m/s boundary with
  seeds 40/41/42. Keep NDNSF `FirstResponding`, record the explicit
  `ackTimeoutMs=2 s` semantic note, and aggregate lifecycle-stage failures
  (`ACK_MATCHED` -> `PROVIDER_SELECTED` -> `RESPONSE_OBSERVED`) without
  treating the sensitivity result as a new primary claim. Evidence:
  `evidence/timeout-sensitivity-2s5s-50m2ms-3seed-20260806.md`.

## Phase 5: Confirmatory gRPC comparison and publication figure

- [x] T010 [US3] Close the trace-phase pairing gap as one evidence outcome:
  record measurement-window coverage and reject traffic-offset mismatches in
  `Experiments/single_ap_range_speed_pilot.py`; add a source-hashed analysis
  and figure generator with focused tests; pre-register and run the frozen
  50 m / 2 m/s seeds 43--47 holdout for NDNSF, `gRPC-SEQ-4`, and diagnostic
  `gRPC-1`; then retain the machine-readable aggregate, publication figure,
  exact command, hashes, paired confidence bounds, negative outcomes, and the
  SC-008 claim verdict under `evidence/`. Result:
  `evidence/holdout-confirmation-result-20260806.md` and
  `evidence/holdout-comparison-20260806/` (`NO_HOLDOUT_CONFIRMATION`).

## Phase 6: Seed-variation follow-up

- [x] T011 [US3] Run the registered follow-up in
  `evidence/followup-seed-repeat-registration-20260807.md`: ten new
  trace-paired seeds (50--59) for NDNSF, `gRPC-SEQ-4`, and NSC-4, plus one
  independent process repeat for seeds 50, 54, and 58. Keep seed-level
  inference separate from within-seed process variation and retain the
  unchanged SC-008 claim gate. Result:
  `evidence/seed-repeat-followup-20260807/`
  (`NO_POSITIVE_MOBILITY_CONFIRMATION`; all repeat trace hashes matched).

## Phase 7: NDNSF reverse-ACK recovery diagnosis

- [x] T012 [US3] Pause matrix expansion and replay the fixed
  100 m / 10 m/s / seed 61 / 500 ms NDNSF cell with per-request lifecycle
  tracing and publication-time coverage alignment. Result:
  `evidence/ndnsf-100m-10ms-seed61-ack-path-diagnosis-20260808.md`.
- [x] T013 [US3] Run one strictly paired periodic-Sync sensitivity changing
  only `NDNSF_SVS_PERIODIC_SYNC_MS` from 30000 to 500. The sensitivity exactly
  reproduced the 116/300 success prefix and did not restore reverse ACK
  delivery. Do not resume the wider matrix until the state-vector/mapping/
  publication-fetch seam is instrumented.

## Phase 8: Parallel Sync production correctness gate

- [x] T014 [US3] Add actual gate-application timestamps and per-request
  lifecycle analysis; reproduce the seed-61 boundary with low-overhead
  Sync/fetch/rejection counters; isolate parallel Sync production with two
  single-variable treatments; make the optimization explicit opt-in in both
  User and Provider runtimes; and validate the corrected default on the same
  35-second trace. Evidence:
  `evidence/ndnsf-100m-10ms-seed61-ack-path-diagnosis-20260808.md` and
  `results/ndnsf-periodic-diagnostic-20260808/default-serial-production-boundary-35s-v3-linked/`
  (175/175; every lifecycle stage 175/175).

## Phase 9: Full-window recovery gate

- [x] T015 [US3] Run independent 60-second replays of the fixed canonical
  seed-61 trace, classify each failure by lifecycle stage, and test parallel
  receive, periodic-Sync, and core-TRACE sensitivities without expanding the
  range/speed matrix. The independent replay falsified the 35-second
  sufficiency claim: the no-core-TRACE corrected default completed 192/300,
  with 47 missing ACKs, 19 late/unmatched ACKs, and 42 post-selection Response
  failures. Evidence is appended to
  `evidence/ndnsf-100m-10ms-seed61-ack-path-diagnosis-20260808.md`.
- [x] T016 [US3] Without changing Response behavior, trace each synchronously
  published ACK sequence through User state-vector receipt, mapping fetch,
  publication fetch, and ACK match; identify and implement the smallest
  evidence-supported ACK-path repair, then pass SC-010 with three independent
  60-second canonical replays. The isolated non-postponed 500 ms periodic timer
  treatment is rejected by its 116/300 replay and is not part of the active
  NDN-SVS patch. PCAP isolated the active cause to stale NFD UDP faces/FIB
  nexthops after an iptables reconnect. Numeric face-ID rebinding passed three
  independent 60-second fixed-trace replays: each completed all 297 requests
  published with at least one reachable Provider through ACK fetch, selection,
  and Response; each run's only three timeouts were published while all four
  Providers were unreachable. SC-010 is satisfied.
- [x] T017 [US3] After T016 passes, implement bounded Response-level
  retry/reselection under the existing 5 s global deadline, add a real
  multi-Provider lifecycle regression, and pass SC-011 with three independent
  60-second canonical replays before resuming any range/speed matrix. The
  default-disabled mechanism, lifecycle analyzer, and controlled MiniNDN
  regression are implemented. A 25-request pilot completed 25/25 with one
  successful A-to-B reselection and 26 Provider executions. A later 150 m
  fixed-trace pilot found and repaired a pre-decryption ACK gate that discarded
  standby ACKs after FirstResponding selected its first Provider. The paired
  repaired replay completed 75/75 requests; all 17 Response-attempt timeouts
  reselected and recovered, with 92 Provider executions recorded. Three
  independent 60-second replays then completed 900/900 requests: all 177
  requests with a Response-attempt timeout recovered through bounded
  reselection, with 179 total reselections, 1,077 Provider executions, and no
  timeout after reselection. SC-011 passes.

## Requirements Traceability

| Requirement | Implementing task | Verification artifact |
|---|---|---|
| FR-001--FR-004 | T001--T004 | four-provider manifests, client summaries, focused tests |
| FR-005--FR-007 | T001, T004 | AP/coverage trace metadata and single-active contract test |
| FR-008--FR-012 | T004, T005, T007 | cell receipts, command/source/trace hashes, paired aggregate |
| FR-013--FR-014 | T005--T006 | preserved Spec169 evidence and claim decision report |
| SC-001 | T004 | four-provider smoke summary and `SMOKE_OK` |
| SC-002--SC-004 | T005, T007 | 60-second paired screening/formal aggregate |
| SC-005--SC-006 | T006, T008 | pre-registered confidence-bound gate and verdict |
| SC-007 | T009 | paired timeout-sensitivity manifest, summaries, and lifecycle-stage report |
| FR-016--FR-017, SC-008 | T010 | same-phase holdout manifests, paired statistical summary, and publication figure |
| FR-018, SC-009 | T012--T014 | paired ACK-path diagnosis, explicit opt-in contract test, and 35-second corrected-default lifecycle summary |
| FR-019--FR-020, SC-010 | T015--T016 | independent lifecycle classification and publication/state/mapping/fetch sequence evidence |
| FR-021, SC-011 | T017 | 25/25 controlled pilot, 75/75 repaired fixed-trace pilot, and three 60-second replays with 900/900 success and 177/177 timed attempts recovered |
| FR-006, FR-009, SC-002 | T018 | deterministic burn-in contract, realized coverage distribution, and paired 100 m/150 m mechanism pilot |
| SC-005a, research Decision 6 | T019 | ten-seed 100 m/150 m paired aggregate, seed-level bootstrap, coverage distribution, retry cost, and latency verdict |
| SC-012 | T020 | post-repair 50 m ten-seed paired extension with seed distributions and paired inference |

## Phase 10: Convergence

- [x] T018 [US2] Add a deterministic, manifest-recorded mobility burn-in to
  campaign trace generation, retain the same post-burn-in trace for every
  paired system, and verify that the 60-second 100 m/150 m pilot reports the
  realized reachable-Provider distribution before any seed-matrix expansion,
  per FR-006, FR-009, and SC-002 (partial). The 300-second burn-in pilot passed
  all six receipts. At 100 m, NDNSF completed 300/300 requests versus 297/300
  for `gRPC-SEQ-4` and 300/300 for NSC-4; NDNSF p95 was 96.81 ms versus
  1017.41 ms and 2036.90 ms. At 150 m, all systems completed 300/300 and NDNSF
  retained lower p95 but not lower mean latency. This is one-seed mechanism
  evidence only; the registered multi-seed follow-up remains required.

## Phase 11: Convergence

- [x] T019 [US2] Register and execute the frozen 300-second burn-in,
  100/150 m, 2 m/s paired follow-up for independent seeds 62--71 with NDNSF
  FirstResponding plus bounded Response reselection, gRPC-SEQ-4 and NSC-4,
  preserve a common trace per seed/condition across systems, checkpoint every
  terminal cell without automatically rerunning failures, and report
  seed-level paired bootstrap intervals, coverage distributions, retry cost,
  and latency before applying SC-005a (partial). All 60 cells completed without
  reruns. At 100 m, NDNSF and gRPC both completed 2,939/3,000 requests, while
  NDNSF reduced median seed p95 from 524.15 ms to 90.55 ms; NSC completed
  2,950/3,000 at 3,027.76 ms median seed p95. The paired success interval was
  exactly zero against gRPC and [-1.10, 0.00] percentage points against NSC,
  so SC-005a did not pass. At 150 m all systems completed 3,000/3,000 and gRPC
  had lower mean and median seed p95, confirming that the latency advantage is
  conditional on partial coverage. Evidence is retained in
  `evidence/random-waypoint-burnin-10seed-results-20260808.md`.

## Phase 12: Post-repair 50 m extension

- [x] T020 [US2] Execute the preregistered 50 m / 2 m/s extension for seeds
  62--71 with the same 300-second deterministic burn-in, frozen runtime,
  workload, timeouts, common traces, disabled admission/health oracle, and
  no-automatic-retry policy as T019. Analyze success and latency with mobility
  seed as the inference unit, retain every seed-level point and realized
  coverage distribution, and do not pool any pre-repair or no-burn-in 50 m
  measurements. Registration:
  `evidence/random-waypoint-burnin-50m-10seed-registration-20260808.md`.
  Completed 30/30 cells without retry. NDNSF averaged 54.57% success versus
  53.80% for gRPC and 55.47% for NSC; neither paired 95% interval established
  an NDNSF success advantage. Mean any-Provider coverage was 54.90% with two
  zero-coverage seeds. Evidence and the conditional 100 m latency conclusion
  are retained in
  `evidence/random-waypoint-burnin-50m-10seed-results-20260809.md`.

## Phase 13: 100 m latency-mechanism audit

- [x] T021 [US2] Reconstruct every 100 m gRPC successful logical-request
  latency from the frozen sequential-attempt logs, validate each reconstructed
  seed-p95 against its summary, compare the resulting retry-status bands with
  NDNSF's FirstResponding source path and seed-p95 distribution, and update the
  proposal wording so the cross-seed median is not presented as a typical
  request. Evidence:
  `evidence/random-waypoint-burnin-100m-latency-mechanism-20260809.md`.
