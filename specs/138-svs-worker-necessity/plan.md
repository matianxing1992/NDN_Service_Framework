# Implementation Plan: Single-Worker Necessity Confirmation

**Branch**: `138-svs-worker-necessity` | **Date**: 2026-07-23 |
**Spec**: [spec.md](spec.md)

## Summary

Reuse the currently latest NDN-SVS subject and exact frozen Spec 137 binary,
without touching Spec 137. Add a small Spec 138 orchestration and analysis
layer that runs the same binary in only `face-serial` and `worker-serial`
modes, blocks cell start on host-quiescence, selects the highest valid
control-only pressure rate from a preregistered ladder, qualifies one worker at
that rate, seals three AB/BA/AB pairs, and applies a deterministic necessity
decision.

## Technical Context

**Language/Version**: Python 3.8+ orchestration and analysis; existing C++17
benchmark binary

**Primary Dependencies**: MiniNDN, MiniNDN NFD, current NDN-SVS/ndn-cxx,
Boost 1.71; Python standard library only for new tooling

**Storage**: Immutable JSON/JSONL/CSV/Markdown evidence under a new result root

**Testing**: Python `unittest`, binary self-tests, fail-closed offline verifier,
real two-node MiniNDN

**Target Platform**: Existing four-vCPU Ubuntu MiniNDN host

**Project Type**: Experiment runner and offline evidence analyzer

**Performance Goals**: Select one valid 200–1000 pps pressure point; sustain
offered load within +/-2%; deliver at least 99%; measure run-level treatment
effects over 60 seconds

**Constraints**: One current source commit, one binary, two runtime modes,
one worker only, one publisher, one receiver, no formal retries, no Spec 137
modification

**Scale/Scope**: At most five short control calibration cells, one worker
qualification, and six formal cells

## Constitution Check

- CodeGraph/source verification: PASS. The benchmark and patched NDN-SVS path
  show one runtime switch controls only Sync-production placement.
- Spec-driven durable experiment: PASS. This successor owns the new validity
  and conclusion contract.
- MiniNDN final validation: PASS. All network evidence uses two real MiniNDN
  nodes and independent applications.
- Frozen negative evidence: PASS. Spec 137 paths are read-only authorities and
  are hash-checked before and after Spec 138.
- Cohesive tasks: PASS. Design/audit, harness qualification, irreversible
  formal execution, and closure are independent reviewable outcomes.

## Source-Verified Mechanism

Common path in both modes:

```text
application pacer -> publishAsync() -> Face commit -> updateSeqNo()
```

Control:

```text
Face -> snapshot -> extension -> encode -> sign -> express
```

Treatment:

```text
Face -> snapshot -> enqueue
one worker -> extension -> encode -> sign -> post
Face -> stale check -> express
```

The worker is one thread. The four CPUs serve the full host layout:

```text
one CPU: both NFDs and background system duty
one CPU: publisher pacer + Face
one CPU: receiver
one CPU: publisher's sole worker in treatment
```

The exact CPU identities are selected once from a pre-run utilization sample
and frozen for the campaign.

## Experiment Design

### Calibration

For each rate in `1000, 800, 600, 400, 200`:

1. wait for host-quiescence before creating the cell receipt;
2. run one `face-serial` 5/15/5 cell;
3. accept the first rate meeting offered-load, delivery, mechanism, and Face
   pressure gates;
4. do not inspect a worker result while selecting the rate.

Run one `worker-serial` 5/15/5 qualification at the selected rate. If it fails,
stop without trying another rate.

### Formal matrix

Seal before execution:

| Ordinal | Pair | Mode |
|---:|---:|---|
| 01 | 1 | face-serial |
| 02 | 1 | worker-serial |
| 03 | 2 | worker-serial |
| 04 | 2 | face-serial |
| 05 | 3 | face-serial |
| 06 | 3 | worker-serial |

Every formal cell uses 10/60/10 timing. A started cell gets exactly one receipt.
If offered-load or evidence completeness fails after start, the receipt is
inadmissible without asserting an unmeasured external cause.

### Decision

`NECESSARY_AT_TESTED_BOUNDARY` requires the exact FR-014 conjunction. The word
“necessary” is local to the tested boundary: it means the registered
Face-responsiveness target cannot be met as well by inline serial production
while correctness and offered load remain controlled.

## Project Structure

```text
Experiments/
├── NDN_SVS_Worker_Necessity_Minindn.py
└── analyze_svs_worker_necessity.py
tests/python/
└── test_spec138_svs_worker_necessity.py
specs/138-svs-worker-necessity/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── contracts/experiment-contract.md
├── quickstart.md
├── traceability.md
├── tasks.md
└── evidence/
results/spec138-svs-worker-necessity/<campaign-id>/
```

**Structure Decision**: Import the frozen Spec 137 runner/parser as read-only
mechanism code and add a thin Spec 138 campaign, quiescence, analysis, and
decision layer. Do not duplicate or mutate the benchmark or NDN-SVS patch.

## Complexity Tracking

No constitution violation or additional runtime mechanism is introduced.
