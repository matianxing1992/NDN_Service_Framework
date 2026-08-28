# Tasks: Bidirectional NDN-SVS Capability Comparison

## Phase 1: Freeze The Correct Experiment Contract

- [x] T001 [US1] Replace the invalid adapter design with the reviewed two-peer direct-call contract, preserve the failed adapter run as non-admissible evidence, and close the pre-implementation audit in `specs/132-svs-pacer-isolation/`.

## Phase 2: Implement Real Subject Usage Models

- [ ] T002 [US1] Add a failing source contract, then rewrite the peer so both processes publish and subscribe concurrently, Face runs on its I/O thread, the application main thread directly calls the compile-time-selected `publish()` or `publishAsync()`, fixed boundaries stop overdue work, and no harness queue/post remains; pass source and C++ self-test gates in `tests/python/test_spec132_svs_bidirectional_commit_latency.py` and `Experiments/ndn-svs-pubsub-benchmark/svs-pubsub-bench.cpp`.

## Phase 3: Seal Direction-Aware Once-Only Evidence

- [ ] T003 [US2] Extend the builder, runner, and analyzer with Spec 132 subject labels, isolated Boost 1.71 builds, symmetric peer launch, per-direction joins, sustainable-ceiling classification, immutable 10-cell scheduling, subject-vs-infrastructure failure receipts, and backward exclusion of Spec 131/failed-adapter evidence; pass the focused Python suite in `Experiments/build_svs_pubsub_commit_bench.py`, `Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py`, `Experiments/analyze_svs_pubsub_commit_latency.py`, and `tests/python/test_spec132_svs_bidirectional_commit_latency.py`.

## Phase 4: Execute And Close The Matrix

- [ ] T004 [US3] Build and self-test both exact-commit capability subjects, seal one fresh manifest, execute all five synchronous cells followed by all five asynchronous cells exactly once under MiniNDN, preserve every terminal outcome, generate direction/rate comparison tables, and complete post-implementation audit evidence in `build/spec132/`, `results/spec132-svs-pacer-isolation/<campaign-id>/`, and `specs/132-svs-pacer-isolation/evidence/`.

## Dependencies

`T001 -> T002 -> T003 -> T004`. There are no parallel formal tasks because the
build/manifest identity must freeze before execution and the baseline receipt
block gates treatment admission.

## Cohesion Review

Each task closes one independently reviewable outcome with its test and
evidence. Test creation, implementation, validation, and direct evidence are
kept together rather than split into mechanical file tasks.
