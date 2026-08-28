# Implementation Plan: Cross-Application Stream Generality

**Branch**: `Experimental` | **Date**: 2026-07-20 | **Spec**: [spec.md](spec.md)

## Summary

Validate the accepted Mapping v2 adaptive sample-atomic prefetch mechanism with
two non-UAV, application-neutral workloads: a 10 Hz periodic opaque sensor
stream and a 10 Hz variable-size opaque stream spanning one, two, four, and
eight source segments. Reuse the current generic Python live-stream API and
Core unchanged, add workload adapters plus one MiniNDN runner/analyzer, freeze
12 one-shot 60-second cells, and require independent correctness, proactive-
utility, latency, continuity, and Interest-efficiency success for both
workloads before making a bounded generality claim.

## Technical Context

**Language/Version**: C++17 runtime; Python 3.8 experiment applications and analysis

**Primary Dependencies**: current NDNSF Mapping v2 live-stream API, ndn-cxx
Face/Validator, NFD/MiniNDN, Linux `tc netem`, Python standard library, SciPy
for exact binomial intervals

**Storage**: immutable JSON/CSV/log/hash evidence; bounded in-memory stream state

**Testing**: Boost.Test, Python unittest, security contract, 60-second MiniNDN cells

**Target Platform**: Linux/MiniNDN; no host-NFD, physical, Docker, or iTiger gate

**Project Type**: C++ framework with Python bindings and application-neutral
example/experiment programs

**Performance Goals**: periodic zero-loss delivery >=99.9% with p95 <=200 ms;
variable zero-loss complete delivery >=99% with p95 <=250 ms and p99 <=500 ms;
zero-loss future hits >=99% and Payload overhead <=15%; accepted impaired runs
future hits >=95% and Payload overhead <=25%

**Constraints**: Core remains application-neutral and wire-compatible; Mapping
v2 continues to require adaptive sample-atomic scheduling; every live command
runs once; failures remain evidence; all measurements use a 60-second window
after a fixed 5-second warm-up

**Scale/Scope**: two workloads at 10 samples/s; 600 measured samples/run;
variable samples have 1/2/4/8-source upper-bound classes and one generic XOR
repair; one zero-loss plus five combined-impairment repetitions per workload

## Constitution Check

- Canonical dynamic runtime: PASS; both fixtures use the current generic
  `LiveStreamPublisher`/consumer API and unified service runtime.
- Security in the Data path: PASS; existing validation, provider binding,
  Mapping continuity, name-bound admission, and replay rules remain mandatory.
- CodeGraph first: PASS; current publisher, consumer, policy, example, runner,
  status, and test paths were traced before planning.
- Spec-driven durable work: PASS; Spec 127 owns requirements, experiment
  boundaries, tasks, and rollback.
- Right-scope verification: PASS; deterministic contract and neutrality checks
  precede MiniNDN; final network evidence remains MiniNDN-only.
- Cohesive outcome tasks: PASS; workload delivery, shared evidence, frozen
  execution, and final audit are behavioral slices rather than per-file steps.
- GSD: PASS; health is valid and `.planning/STATE.md` carries the active feature.
- ARS: PASS; [experiment-plan.md](experiment-plan.md) freezes variables,
  confounds, repetition identity, effect boundaries, and no-rerun rules.

Post-design re-check: PASS. No constitution exception is required.

## Design

### 1. Reuse Core; add only application-layer fixtures

`Stream.{hpp,cpp}` and the Python binding remain the accepted implementation.
The new provider fixture selects a workload definition, creates opaque sample
bytes, calls `announce_sample`, reconciles the actual extent with
`prepare_sample_extent`, and commits through `publish_sample`. The consumer
admits authenticated opaque items, groups them by the already signed sample
envelope, and records complete-sample outcomes. No payload meaning reaches Core.

Core edits are not planned. If a deterministic fixture proves a reusable Core
defect, stop live execution, preserve the failing fixture, revise this plan and
audit, then make the smallest generic correction. A threshold miss alone does
not authorize algorithm tuning.

### 2. Periodic opaque sensor workload

The sensor workload publishes 256-byte deterministic opaque samples at 10 Hz.
It declares one generic sample class with seed and hard maximum of one source
item, no FEC repair, and monotonically increasing sample identity. A five-second
warm-up precedes exactly 600 measured publications over 60 seconds. The
application receipt checks byte digest, sample identity, order, duplicates,
and exact publication-to-delivery timing; it never parses a sensor field.

### 3. Variable-size multisegment workload

The variable workload publishes at 10 Hz with 4096-byte maximum source
segments. Four opaque sample classes have source upper bounds 1, 2, 4, and 8.
Within successive balanced seed-shuffled blocks, actual extents are:

```text
cap-1: 1
cap-2: 2, 2, 1
cap-4: 4, 4, 3
cap-8: 8, 8, 7
```

This keeps the application-provided class conservative while still exercising
terminal predicted suffixes and transitions. Every class appears at least once
per 12-sample block. One generic XOR repair is selected for each sample. The
consumer emits only after every source segment is authenticated or one missing
source is recovered; partial groups never count as delivered samples.

The workload seed is `12720260720`. Payload bytes derive from
`SHA-256(seed, sampleId, segmentIndex)` expansion so receipts can verify exact
content without retaining payloads in analysis artifacts.

### 4. Shared evidence contract

Both workloads emit the same run schema. Required fields include expected,
published, complete, duplicate, partial, out-of-order, recovered, skipped,
maximum stall, measurement coverage, exact latency, Payload Interests and
necessary items, Mapping Interests/Data/new Data/bytes, retry, timeout, Nack,
Provider future Interests/hits, workload/config hashes, qdisc evidence, process
return codes, and per-check verdicts.

`mappingNewDataRatio = mappingNewDataResponses / mappingDataResponses` when
the denominator is nonzero; otherwise the field is explicitly unavailable and
the run cannot pass. Payload overhead uses
`(payloadInterests - necessarySourceRepairItems) / necessarySourceRepairItems`.
Failed runs retain every available field and a reason for each unavailable one.
Latency, continuity, and coverage exclude warm-up samples. Traffic ratios use
the complete fixed run (including the common five-second warm-up) so one
future-Interest/Data pair cannot be split across two counter windows; every run
records this as `trafficCounterScope=full-run-including-warmup`.

### 5. Frozen 12-cell MiniNDN matrix

The topology, security setup, logging, warm-up, duration, cadence, and analyzer
are common. Workload-specific payload shape is the independent variable being
tested, not a cross-workload performance comparison.

| Workload | Network profile | Repetitions | Required accepted |
|---|---|---:|---:|
| periodic sensor | zero-loss, topology delay 1 ms | 1 | 1 |
| periodic sensor | Spec 126 combined profile | 5 | 4 |
| variable multisegment | zero-loss, topology delay 1 ms | 1 | 1 |
| variable multisegment | same Spec 126 combined profile | 5 | 4 |

The combined profile is 1% loss, 20/10 ms delay/jitter, 25% reorder, 50%
correlation, gap 5. Each command gets a unique immutable directory. The runner
refuses existing nonempty output, concurrent MiniNDN/cleanup ownership, missing
qdisc proof, source drift, workload drift, missing metrics, or automatic retry.
Defect closure requires a separately named complete 12-cell confirmation.

### 6. Effect and claim boundary

This feature uses absolute engineering gates rather than a demand-driven
counterfactual. Mapping v2 currently requires adaptive sample-atomic policy;
the existing `MappedLiveFutureOff` policy is a Mapping v1 behavior, so comparing
them would confound wire contract, scheduling semantics, and predictor state.
Adding a new public no-prefetch mode solely for the experiment would violate
scope and weaken the generality claim.

A successful run must contain nonzero Provider-confirmed future Interests and
meet latency, continuity, future-hit, Mapping novelty, and Payload overhead
gates. Passing both workload families supports only the bounded claim that the
same generic mechanism works for these two families under declared profiles.

### 7. Rollback and evidence preservation

- Remove only Spec 127 workload fixtures, runner, tests, and documents.
- No wire, persisted state, Core, binding, or public API migration is planned.
- Hash scoped sources, workload manifest, and retained Spec 125/126 canonical
  evidence before live execution; verify identical hashes afterward.
- Preserve negative or invalid run directories until documented.

## Project Structure

```text
examples/python/live_stream/
├── workload_common.py
├── workload_provider.py
└── workload_consumer.py
Experiments/
├── NDNSF_LiveStream_Generality_Minindn.py
└── run_spec127_cross_application_matrix.py
tests/python/
├── test_ndnsf_live_stream_generality.py
└── test_spec127_cross_application_runner.py
specs/127-cross-application-stream-generality/
results/spec127-cross-application-*/
```

**Structure Decision**: Keep workload meaning in new generic example fixtures,
MiniNDN ownership in an experiment runner, and metrics in a shared analyzer.
Reuse Core and bindings unchanged; do not route this work through UAV code.

## Complexity Tracking

No constitution violation requires justification.
