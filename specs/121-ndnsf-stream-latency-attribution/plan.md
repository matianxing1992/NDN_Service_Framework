# Implementation Plan: NDNSF Stream Latency Attribution and Continuity

**Branch**: `Experimental` | **Date**: 2026-07-18 | **Spec**: [spec.md](spec.md)

## Summary

Repair the live consumer's Mapping-frontier handoff so scheduling continues across future Mapping blocks, correct latest-join reorder initialization, and replace mixed cursor/frame correlation with a stable source identity and honest startup/steady metrics. Preserve the one-variable 20 ms batching result as interim evidence; defer default acceptance until Spec 122 supplies exact acquisition-to-display identity.

## Supersession Boundary

Spec 121 stops after the corrected continuity/attribution implementation and one measured candidate pair. Repeating the original T007 matrix would multiply an incomplete proxy metric and cannot decide true live-video latency. Spec 122 exclusively owns media acquisition identity, codec PTS/output association, capture-to-widget/presentation evidence, confirmatory experiment ordering, and default promotion. Spec 121 remains the authoritative prerequisite for Stream continuity and its negative/ambiguous measurement findings.

## Technical Context

**Language/Version**: C++17, Python 3

**Primary Dependencies**: ndn-cxx 0.9, NFD/MiniNDN, NDNSF Stream Core, Qt ground station, FFmpeg/libx264

**Storage**: JSON/CSV/log evidence under unique `results/spec121-*` directories; no new runtime persistence

**Testing**: Boost unit tests, Python contract tests, deterministic trace fixtures, MiniNDN UAV GUI experiment

**Target Platform**: Ubuntu/MiniNDN; headless drone and GUI-capable ground station

**Project Type**: C++ framework plus UAV application and Python experiment driver

**Performance Goals**: Continuous 60-second consumption; at least 99% eligible future-hit ratio; corrected steady-state p95 improvement of at least 20% for any retained optimization

**Constraints**: Preserve semantic names, wire formats, validation, AES-GCM/replay checks, optional FEC, bounded trace state, and 60-second measured windows; no Data-in-Data or unsampled per-item INFO output

**Scale/Scope**: One provider/one consumer MiniNDN baseline crossing at least three 32-slot Mapping blocks, followed by matched zero-loss and controlled-loss cells

## Constitution Check

- **NDN semantics**: PASS. Exact semantic Data names and names-only Mapping remain authoritative.
- **Security**: PASS. No trust, encryption, nonce, signer, or replay invariant changes.
- **Layer ownership**: PASS. Mapping scheduling/correlation primitives belong to Stream Core; H.264 media sequence and decoder behavior remain UAV APP concerns.
- **Evidence before claims**: PASS. Current one-second aggregate is classified as invalid/ambiguous until correlation is repaired.
- **MiniNDN-first validation**: PASS. No host NFD, Docker, iTiger, or real UAV is required.
- **Task cohesion**: PASS. Tasks are behavioral slices, not one item per file/test/command.
- **Dirty worktree safety**: PASS. Edits stay within named Stream/UAV/spec paths; no unrelated cleanup, commit, or push.

## Diagnosis Baseline

1. The accepted Spec 120 trace run produced to cursor 304, but the consumer issued only 30 payload Interests and delivered 24 source items. `LiveStreamConsumerHandle` admits later Mapping blocks into `StreamNameResolverState` but never calls `StreamAdaptiveFetcherState::updateMappingFrontier`; `advanceNextCursor` therefore rejects progress at the initial `nextReserved=32` frontier.
2. Latest join advertised cursor 10, but the decoder reorder buffer was recreated with media sequence 0. It waited for a synthetic gap and skipped 0 through 7 after about 205 ms.
3. Consumer `decrypted` events use publication cursors, while `reorder-ready` uses the remapped source-media sequence. The existing analyzer can therefore correlate unrelated items with equal integers.
4. One FEC input group contributes one correlation record while FFmpeg may emit several frames; popping one record per JPEG creates false pairings and 92 correlation misses in the sampled run.
5. Cold FFmpeg startup contributes roughly 700 ms after the first decoder input, while later sampled decoded frames were 10–12 ms. Startup and steady state must be separate.
6. The Provider reads H.264 with `fread(..., 8192)`. On the low-entropy test video, filling that stdio request can take close to one second, and the provider timestamp begins only after the read returns. This hidden producer batching must be measured and bounded before tuning prefetch windows.

## Design Decisions

### 1. Frontier authority

`StreamNameResolverState` remains the verified Mapping authority. After a block is accepted, the consumer derives the resolver's monotonic frontiers and updates the adaptive fetcher before any cursor advancement or rescheduling. Stale/regressive values fail closed with an explicit status reason. A unit test crosses multiple Mapping blocks and proves continued scheduling.

### 2. Source identity and sequence domains

Timeline events carry a stable session-scoped source correlation ID derived from the immutable Mapping binding. `publication_cursor` remains the Core ordering key. UAV adds `media_sequence`, `frame_id`, and `segment_index` as separate metadata. Repair items never masquerade as sources. The analyzer joins only identical correlation IDs and compatible event roles.

### 3. Latest join and decoder readiness

Core continues to select the verified `safeJoinCursor`. UAV converts the first accepted source item at or after that cursor into the decoder's initial media sequence and lazily initializes reorder there. This removes synthetic pre-join loss without making Core understand H.264 or FEC media numbering. A mid-GOP join remains observable and waits for the next authenticated key-frame boundary rather than claiming corruption as network latency.

### 4. Decoder attribution

An encoded group is not assumed to map one-to-one to output frames. The ground station records input-group time separately, tags output with a monotonically increasing output ordinal, and reports group-to-first-output plus steady inter-output/display age. Unmatched output remains counted, never guessed. Startup ends only after the first valid decoded frame and a bounded warmup rule recorded in evidence.

### 5. Experiment order

The Spec 121 order is fixed: deterministic failing regressions, correctness fix, corrected 60-second baseline, then one provider encoder-pipe/packetization candidate probe. The original five-pair acceptance matrix is withdrawn here rather than executed against an ambiguous H.264 group/output-frame metric. Spec 122 resumes from the frozen pair only after installing an exact source-frame feedback loop.

## Project Structure

```text
ndn-service-framework/
├── Stream.hpp
└── Stream.cpp
NDNSF-UAV-APP/
├── shared/UavProtocol.{hpp,cpp}
├── drone/DroneServiceContainer.inc.hpp
├── ground-station/GroundStationServiceContainer.inc.hpp
└── ground-station/GroundStationWindow.inc.hpp
Experiments/
├── NDNSF_UAV_GUI_Minindn.py
└── analyze_stream_latency.py
tests/
├── unit-tests/stream.t.cpp
├── unit-tests/uav-protocol-state.t.cpp
└── python/test_ndnsf_stream_latency.py
specs/121-ndnsf-stream-latency-attribution/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── contracts/latency-evidence.md
├── quickstart.md
└── tasks.md
```

**Structure Decision**: Extend the existing Stream Core and UAV integration; add one reusable evidence analyzer rather than embedding analysis in the GUI driver.

## Validation Strategy

1. Deterministic unit test reproduces stale frontier failure by advancing beyond the initial Mapping reservation.
2. Deterministic correlation fixture contains numeric cursor collisions, repair symbols, gaps, and one-to-many decoder output.
3. Focused UAV test proves latest join does not wait for pre-join media sequences and preserves key-frame/FEC rules.
4. One 60-second zero-loss correctness candidate establishes continuous delivery and corrected metrics.
5. The frozen pair supports diagnosis only, not an optimization or default claim.
6. Spec 122 supplies new, non-reused confirmatory identities for five counterbalanced baseline/candidate pairs and trace-on/off controls.

## Post-Design Constitution Check

PASS. No new protocol, public API, security bypass, duplicate stream path, or application policy in Core is introduced. Correctness precedes performance claims, and rollback is a bounded reversion of the frontier handoff, join initialization, correlation metadata, or individual candidate.
