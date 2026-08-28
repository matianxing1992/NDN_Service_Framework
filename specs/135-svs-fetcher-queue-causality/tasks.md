# Tasks: NDN-SVS Fetcher Queue Causality Diagnostic

**Input**: Design documents in `specs/135-svs-fetcher-queue-causality/`

## Phase 1: Freeze The Diagnostic

- [x] T001 Audit the exact source mechanism, RSA-2048 signing proof, factor
  necessity, five-plus-three cell order, boundary rule, claim thresholds,
  evidence boundary, rollback, and task
  cohesion in `spec.md`, `plan.md`, `research.md`,
  `contracts/diagnostic-contract.md`, and `traceability.md`; implementation is
  blocked by any unresolved HIGH/CRITICAL finding.

## Phase 2: Deliver The Isolated Diagnostic Path

- [x] T002 [US1] Create fail-first source/analyzer contracts, then build
  one isolated diagnostic descendant of Spec 133 with a bounded Fetcher-window
  setting, a driver-level ApplicationParameters setting, missing-batch
  evidence, a hash-bound builder, verified-route MiniNDN runner, and factorial
  analyzer in `Experiments/build_svs_fetcher_queue_causality.py`,
  `Experiments/NDN_SVS_Fetcher_Queue_Causality_Minindn.py`,
  `Experiments/analyze_svs_fetcher_queue_causality.py`,
  `Experiments/ndn-svs-pubsub-benchmark/svs-fetcher-queue-causality.cpp`, and
  `tests/python/test_spec135_svs_fetcher_queue_causality.py`; prove that Spec
  133 authorities and the active NDN-SVS checkout remain unchanged.

## Phase 3: Build And Admit

- [x] T003 [US2] Build the isolated library/driver with Boost 1.71, record
  parent/patch/tree/binary/library/compiler/linkage hashes, run the driver
  self-test and manifest/source-contract suite, and freeze exactly five
  stage-A cells in `build/spec135/` without consuming network evidence if any
  gate fails.

## Phase 4: Execute Once

- [x] T004 [US3] Show and execute the preregistered campaign command once,
  monitor process liveness/output/timeout, retain every terminal outcome without
  retry; close five stage-A receipts, seal the selected boundary and three-cell
  stage-B manifest, then close all three treatments plus raw logs under
  `results/spec135-svs-fetcher-queue-causality/confirm01/`.

## Phase 5: Interpret Without Fixing

- [x] T005 [US4] Analyze every receipt, the RSA rate boundary, and both matched
  contrasts, produce the
  ARS-compatible experiment result/validation/fallacy scan and
  `evidence/causality-report.md`, classify H1/H2/H3, rank solution directions
  without modifying production code, run the post-implementation audit, and
  freeze Spec 135.

## Dependencies

```text
T001 -> T002 -> T003 -> T004 -> T005
```

## Cohesion Review

Five tasks correspond to five independently reviewable gates: design,
diagnostic behavior, immutable build admission, irreversible campaign
execution, and interpretation. Tests, implementation, and focused evidence for
one behavior are not mechanically split.
