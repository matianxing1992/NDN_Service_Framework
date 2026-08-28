# Implementation Plan: Loss and Reordering Resilience

**Branch**: `Experimental` | **Date**: 2026-07-19 | **Spec**: [spec.md](spec.md)

## Summary

Close the packet-loss and bounded-reordering boundary left by Spec 125 without
changing Mapping v2 or the public sample API. Strengthen the existing consumer
cursor/recovery lifecycle so timeout, Data, validation, FEC, and stop callbacks
have one terminal outcome; retain ordered decoder delivery through the existing
bounded reorder buffer; expose enough counters to attribute every recovery or
skip; then execute one preregistered 60-second MiniNDN matrix with no automatic
reruns and preserve all negative results.

## Technical Context

**Language/Version**: C++17 and Python 3

**Primary Dependencies**: ndn-cxx Face/Validator, NFD/MiniNDN, Linux `tc netem`,
current Stream Mapping v2, UAV GStreamer pipeline, SciPy exact beta intervals

**Storage**: In-memory bounded stream state and immutable JSON/CSV/log evidence

**Testing**: Boost.Test, Python unittest, existing UAV security contract,
MiniNDN 60-second GUI campaign

**Target Platform**: Linux/MiniNDN; no host-NFD, physical, Docker, or iTiger gate

**Project Type**: C++ framework with Python bindings and UAV application

**Performance Goals**: retain Spec 125 zero-loss gates; under impairment keep
retry overhead at most 25%, future-hit success at least 95%, p95 at most 300 ms,
p99 at most 600 ms, and no final-ten-second stall longer than one second

**Constraints**: exact semantic names; Mapping v2 unchanged; one terminal
cursor outcome; three-attempt maximum retained unless deterministic evidence
proves it unsafe; no automatic tuning or campaign retry; bounded sampled logs

**Scale/Scope**: one Provider, one consumer, 1200-kbit/s 320-pixel UAV stream,
one repair item, one 60-second zero-loss run, and five repetitions for each of
three impaired profiles

## Constitution Check

- Canonical dynamic runtime: PASS; no generated/static API or naming change.
- Security on the Data path: PASS; reordered Data crosses the same Validator,
  provider/name, extent, session, and application-admission checks.
- CodeGraph first: PASS; existing retry, repair, resolver, decoder, and runner
  call paths were inspected before design.
- Spec-driven durable change: PASS; Spec 126 owns requirements and rollback.
- Right-scope verification: PASS; deterministic races precede MiniNDN.
- Cohesive tasks: PASS; tasks close lifecycle, reorder, compatibility, campaign,
  and final evidence as behavioral outcomes rather than per-file fragments.
- GSD: PASS; repository health is valid and `.planning/STATE.md` will carry the
  continuation point.
- ARS: PASS; [experiment-plan.md](experiment-plan.md) preregisters variables,
  matched controls, repetitions, exact intervals, and no-rerun rules.

Post-design re-check: PASS. No constitution exception or complexity waiver is
required.

## Design

### 1. One cursor, one terminal outcome

The consumer enforces one bounded logical lifecycle for each owned cursor.
Network callbacks acquire an attempt identity, but the cursor remains the authority.
Only the active attempt may transition `Pending -> Received`; only its
validation may transition `Received -> Delivered` or `Recovered`. Timeout/Nack
may transition the active attempt back to `Pending` for the next exact-name
attempt, or to `Skipped` when budget/deadline is exhausted. `Delivered`,
`Recovered`, `Skipped`, `TerminalUnproduced`, and `Stopped` are terminal.

This is first a testable contract over the existing in-flight, processing,
completed, attempt, generation, and timing state. Keep those structures if the
deterministic permutations prove they already enforce the contract. Add or
consolidate an explicit lifecycle record only when a reproduced race cannot be
closed by a smaller ownership/generation correction; either representation must
make its indexes agree with the single logical lifecycle.

### 2. Recovery is group-scoped and order-independent

The existing authenticated Mapping and `LiveStreamFecRepair` define group
membership. Sources and repair may arrive in any order. When exactly one source
is absent and all other inputs plus one valid repair are present, recovery
produces one byte-exact source and attempts the same application admission as
signed Data. A later source or repair becomes an observable late duplicate.

When more than one source is absent, no partial reconstruction occurs. Retry
continues only while the cursor budget and playout deadline allow. Expiry marks
the affected sample explicitly and advances later schedulable groups; it does
not weaken validation or reuse a cursor.

### 3. Ordered media remains an APP boundary

Core delivers authenticated items without assigning codec semantics. UAV maps
publication cursors to source-only media sequences and uses the existing
bounded `StreamConsumerReorderBuffer`. The buffer admits valid out-of-order
chunks, drops duplicates/stale sessions, and emits only contiguous media
sequences. Deadline skip and overflow are explicit; neither can silently train
the Core predictor or claim FEC recovery.

### 4. Observability is bounded

`LiveStreamStatus` and Python parity add aggregate counters only when current
fields cannot express the result: retry attempts, late arrivals, deadline
skips, retry exhaustion, and maximum reorder depth. Per-cursor detail remains
sampled timeline/log evidence under `NDNSF_TIMELINE_TRACE_SAMPLE_RATE`; secrets
and payload bytes are never logged.

No wire field is added. Status additions are local read-only API extensions;
if implementation can derive the required evidence from existing fields,
prefer that smaller path.

### 5. Impairment harness

Create a dedicated Spec 126 campaign wrapper around
`Experiments/NDNSF_UAV_GUI_Minindn.py`. The runner receives a narrow experiment-
only impairment profile, applies it through the actual Mininet endpoint
interfaces after `ndn.start()` and before NFD/apps start, captures both endpoint
qdiscs, and fails preflight if the expected netem child cannot be discovered or
verified. The wrapper owns one launcher/cleanup process, creates a unique
topology and run directory for each cell, freezes commands, and records process
ownership plus source identity.

The fixed profiles are:

| Cell | Loss | Added delay/jitter | Reorder | Repetitions |
|---|---:|---|---|---:|
| zero-loss | 0% | topology 1 ms | 0% | 1 |
| loss | 1% iid | topology 1 ms | 0% | 5 |
| reorder | 0% | 20 ms / 10 ms normal | 25%, correlation 50%, gap 5 | 5 |
| combined | 1% iid | 20 ms / 10 ms normal | 25%, correlation 50%, gap 5 | 5 |

For reorder cells, inspect each endpoint's qdisc and replace only the existing
Mininet netem child after MiniNDN creates the 1000-Mbit HTB link. The current
environment uses parent `5:1` and handle `10:`, but the runner verifies rather
than blindly assumes them. Capture `tc -s qdisc show` before and after the run.
Both directions receive the same profile. The installed kernel/iproute2 exposes
these parameters but no stable netem RNG seed, so repetition identity and
effective counters are recorded and no exact packet-loss sequence
reproducibility is claimed.

### 6. Frozen execution and defect closure

Before the first matrix command, record source hashes, command matrix, free
space, single-writer ownership, environment, Spec 125 evidence hashes, and all
deterministic gate results. Every planned command runs once. The analyzer
produces exact Clopper-Pearson intervals for completion counts and reports
effects descriptively; five repetitions do not support a general reliability
claim.

If a run exposes a concrete implementation defect, retain the whole original
matrix. Fix the defect only after a deterministic regression reproduces it.
Any post-fix network validation is a separately named complete confirmation
series; it cannot replace selected failed runs.

### 7. Rollback

- Revert only Spec 126 lifecycle/status/harness changes; Mapping v2 and Spec 125
  source/evidence remain the rollback baseline.
- A new status counter is additive and read-only; old callers ignore it.
- Stop clears all cursor/group attempt state by generation before a replacement
  session starts.
- No persisted or on-wire migration is needed.

## Project Structure

```text
ndn-service-framework/Stream.{hpp,cpp}
pythonWrapper/src/ndnsf/_ndnsf.cpp
pythonWrapper/ndnsf/streaming.py
NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp
tests/unit-tests/stream.t.cpp
tests/unit-tests/uav-protocol-state.t.cpp
tests/python/test_ndnsf_core_streaming.py
tests/python/test_ndnsf_uav_unified_video.py
tests/run_uav_stream_security_contract.py
Experiments/NDNSF_UAV_GUI_Minindn.py
Experiments/run_spec126_loss_reorder_matrix.py
specs/126-loss-reorder-resilience/
results/spec126-loss-reorder-*/
```

**Structure Decision**: Repair the existing Core and UAV owners. Add one
campaign wrapper and no new runtime subsystem, protocol version, or public mode.

## Complexity Tracking

No constitution violation requires justification.
