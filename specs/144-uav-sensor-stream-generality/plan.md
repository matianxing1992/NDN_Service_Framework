# Implementation Plan: UAV Sensor Stream Generality

**Branch**: `[144-uav-sensor-stream-generality]` | **Date**: 2026-07-24 |
**Spec**: [spec.md](spec.md)

**Input**: Feature specification from
`/specs/144-uav-sensor-stream-generality/spec.md`

## Summary

Add two UAV application workloads over the existing generic Mapping v2 live-
stream surface and build a fresh, one-shot MiniNDN confirmation around them.
The 20 Hz compact telemetry stream exercises the unresolved periodic
single-item/no-FEC boundary. The 40 ms acoustic/audio block stream exercises
variable 2/3/4-source groups with generic GF(256) two-repair recovery.

The implementation is application- and evidence-focused. Existing
`announceSample`, `prepareSampleExtent`, `publishSample`,
`LiveStreamStart::Latest`, and `AdaptiveSampleAtomic` behavior remains the
canonical Core path. A Core change is permitted only for a reproducible generic
contract defect found before formal execution; it requires deterministic
coverage and renewed audit. Formal failures are preserved and deferred.

The corrected Spec 145 UAV Video path is the APP-side integration reference,
not a workload implementation to clone. Telemetry and acoustic/audio reuse its
session-frozen announcement/publication ownership, existing Core consumer,
exact-name/security admission, callback containment, and generation-fenced
Core status pattern. They do not reuse video class, key/delta, FPS/GOP, codec,
or payload semantics.

## Technical Context

**Language/Version**: C++17 for Core/UAV applications and Python 3 for MiniNDN
runner, analyzer, and binding-level tests

**Primary Dependencies**: ndn-cxx, NDNSF generic Streaming/Mapping v2,
MiniNDN/Mininet/NFD, Linux `tc netem`, Boost.Test, Python `unittest`

**Storage**: In-memory live retention plus local immutable experiment
directories; existing UAV snapshot/status storage remains unchanged

**Testing**: C++ unit suites, Python binding/contract/runner tests, UAV stream
security contract, MiniNDN smoke gates, and a fresh 32-cell formal matrix

**Target Platform**: Linux two-node MiniNDN testbed; real microphone and
physical wireless devices are out of scope

**Project Type**: C++ framework library with C++ UAV applications, Python
bindings, and Python experiment tooling

**Performance Goals**: 20 Hz telemetry with bounded AoI; 25 blocks/s
acoustic/audio with bounded end-to-end latency; high provider-confirmed future
hits; explicit low nonproductive-Interest ratio

**Constraints**: 60-second measured window; one formal invocation per cell;
single MiniNDN writer; no historical rerun; no application/codec/workload
branch in Core; repair protection cost reported separately from useful source
delivery

**Scale/Scope**: Two nodes, two workload families, four network profiles,
1+5+5+5 repetitions per workload, 32 formal cells total

## Constitution Check

*GATE: Passed before Phase 0 and re-checked after Phase 1.*

- **Canonical dynamic runtime: PASS.** The workloads use the current generic
  live-stream API and do not restore generated/static APIs or split service
  names.
- **Security in the Data path: PASS.** Existing signer/provider/session/name
  checks remain mandatory; sensitive application bytes remain opaque and
  protected rather than entering Mapping or logs.
- **CodeGraph first: PASS.** Current publisher, consumer, adaptive fetcher,
  status metrics, UAV video path, and telemetry polling path were traced before
  planning.
- **Spec-driven durable work: PASS.** Spec 144 owns the new workloads, evidence
  contract, matrix, neutrality gate, and negative-outcome policy.
- **Right-scope verification: PASS.** Deterministic and security gates precede
  MiniNDN; host NFD cannot serve as final evidence.
- **Cohesive tasks: PASS.** Tasks close behavioral outcomes rather than
  separating tests, implementation, commands, and evidence mechanically.
- **GSD/resumability: PASS by design.** Formal execution requires one campaign
  owner, unique destinations, frozen commands, and resumable manifests.
- **ARS experiment rigor: PASS.** [experiment-plan.md](experiment-plan.md)
  freezes RQs, variables, confounds, metric definitions, repetition counts, and
  claim boundaries before implementation.

Post-design re-check: PASS. No constitution exception or complexity waiver is
required.

## Design Overview

### 1. Preserve historical evidence

Before implementation, recompute deterministic directory-root hashes for Specs
127 and 128 and compare them with the values recorded in `spec.md`. Do not
invoke either historical runner. Repeat the hash check after final closure.

Also verify the Spec 145 promotion dependency before implementation:

- `specs/145-uav-video-runtime-corrections/evidence/post-implementation-audit.md`
  has verdict PASS;
- `results/spec145-uav-video-runtime-20260724T064253Z/campaign-summary.json`
  hashes to
  `8bad7f90d12647f2904e208000e189ca73a5369ebb33f359a8f07a5560ca2566`;
- the reference is used only for APP/Core ownership and lifecycle patterns,
  never for workload semantics.

### 2. Add one compact telemetry stream to UAV-APP

The drone-side application derives a compact `TelemetrySample` from existing
UAV state. A deterministic fixture backend produces the exact 20 Hz and
256/384/512-byte formal schedule; the same application boundary can later be
fed from MAVLink without changing Core.

The provider declares one `telemetry` sample class with source extent exactly
one and no FEC, announces names ahead of production, protects the opaque sample,
and publishes it through Core. Ground Station opens at Latest with
AdaptiveSampleAtomic, admits only monotonic validated samples, computes AoI,
and retains the existing `GetStatus` request/response path as snapshot/fallback.

### 3. Add one short-block acoustic/audio stream to UAV-APP

The formal source is deterministic/file-backed. Every 40 ms block cycles
through three exact APP-owned extent classes for two, three, and four opaque
source items, each at most 512 encoded bytes. The provider announces the exact
class and publishes the protected sources with generic GF(256) two-repair
protection.

Ground Station admits a block only after all original source bytes are directly
delivered or validly recovered. Audio device, codec, playback, concealment, and
semantic interpretation remain outside Core and outside formal acceptance.

### 4. Extend generic observability without semantic branches

The public status surface already exposes Mapping/Payload, retry, timeout,
Nack, future-hit, and recovery counters. Add only the unsampled generic
terminal-attempt ledger needed to conserve each Payload attempt and each repair
Data item against actual recovery consumption. Existing sampled TimelineTrace
output remains diagnostic rather than authoritative. Any shared implementation
change must be
named and expressed in generic terms such as sample, group, source, repair,
Mapping, cursor, attempt, or outcome.

The analyzer computes:

- delivery and continuity;
- AoI or capture-to-delivery mean/p50/p95/p99/max;
- longest accepted-item gap;
- Mapping novelty and provider-confirmed future-hit ratios;
- retry/timeout/Nack/late/skip/exhaustion counts;
- recovery attempts, successes, failures, and provenance;
- application-useful, protection-only, and nonproductive Interest ratios.

### 5. Use a fresh one-shot MiniNDN matrix

Each workload independently executes:

| Profile | Loss | Added delay/jitter | Reorder | Repetitions |
|---|---:|---|---|---:|
| zero-loss | 0% | topology 1 ms | 0% | 1 |
| loss | 1% iid | topology 1 ms | 0% | 5 |
| reorder | 0% | 20 ms / 10 ms normal | 25%, correlation 50%, gap 5 | 5 |
| combined | 1% iid | 20 ms / 10 ms normal | 25%, correlation 50%, gap 5 | 5 |

The runner discovers and verifies the actual Mininet netem child rather than
assuming an interface hierarchy. Both directions receive the profile. Every
cell records before/after qdisc statistics, process ownership, readiness,
source/binary/config hashes, exact command, and terminal status.

### 6. Close with independent and shared verdicts

Telemetry and acoustic/audio are evaluated separately. Exact accepted-count
intervals are reported for every five-repetition treatment. The shared verdict
is positive only if both workload families pass all controlling criteria. No
threshold is relaxed and no failed cell is replaced after observation.

## Project Structure

### Documentation

```text
specs/144-uav-sensor-stream-generality/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── experiment-plan.md
├── quickstart.md
├── traceability.md
├── tasks.md
├── checklists/
│   └── requirements.md
└── contracts/
    ├── workload-contract.md
    ├── evidence-contract.md
    └── neutrality-contract.md
```

### Anticipated Source and Evidence Paths

```text
NDNSF-UAV-APP/
├── shared/
│   └── UavSensorStreams.*              # app-level opaque payload/session helpers
└── tools/uav_sensor_stream_node.cpp     # real UAV provider/consumer executable

ndn-service-framework/
├── Stream.hpp                           # generic changes only if audit-approved
└── Stream.cpp

pythonWrapper/
├── src/ndnsf/_ndnsf.cpp                # parity only if generic status changes
└── ndnsf/streaming.py

Experiments/
├── NDNSF_UAV_Sensor_Stream_Generality_Minindn.py
├── run_spec144_uav_sensor_stream_matrix.py
└── analyze_spec144_uav_sensor_stream.py

tests/
├── unit-tests/
│   ├── stream.t.cpp
│   └── uav-protocol-state.t.cpp
└── python/
    ├── test_ndnsf_uav_sensor_stream_generality.py
    └── test_spec144_uav_sensor_stream_runner.py

results/spec144-uav-sensor-stream-<fresh-id>/
```

**Structure Decision**: Keep transport mechanisms in existing generic Core,
payload semantics in UAV-APP, and all fault injection/evidence interpretation
in experiment tooling. No new service-specific framework wire type is planned.

## Implementation Phases

### Phase 0 - Baseline and pre-implementation audit

Freeze hashes, identify exact code owners through CodeGraph, confirm metric
availability, and issue PASS/BLOCK before source implementation.

### Phase 1 - Shared measurement foundation

Define generic per-Interest outcome and per-sample/block trace schemas, metric
conservation tests, analyzer fixtures, and one-shot runner invariants.

### Phase 2 - Telemetry application slice

Implement the compact UAV telemetry provider/consumer path, deterministic
source, exact-size encoding, monotonic admission, AoI measurement, and focused
tests without changing `GetStatus`.

### Phase 3 - Acoustic/audio application slice

Implement deterministic short-block production, variable extent, two-repair
recovery, complete-block admission, protection-cost attribution, and focused
tests with no codec/device dependency.

### Phase 4 - Build, preflight, and formal execution

Rebuild source and bindings, pass deterministic/security suites, validate one
non-formal smoke per workload, freeze hashes/commands, and execute the 32 formal
cells once under a single MiniNDN owner.

### Phase 5 - Analysis and closure

Generate per-cell and aggregate reports, exact intervals, neutrality and
historical-hash audits, independent workload verdicts, and the bounded shared
claim or preserved negative conclusion.

## Complexity Tracking

No constitution violation is planned. A true microphone path, codec comparison,
physical wireless campaign, new reliability semantic enum, or post-formal Core
repair would require a separate feature decision.
