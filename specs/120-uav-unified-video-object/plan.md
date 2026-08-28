# Implementation Plan: Unified Named UAV Video

**Branch**: `Experimental` | **Date**: 2026-07-18 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/120-uav-unified-video-object/spec.md`

## Summary

Replace the UAV application's recording-only raw-chunk/encryption/playback path with retention of the exact semantically named, encrypted, Provider-signed packets already produced by Spec 118 over the Spec 119 LiveStream API. NDNSF Core exposes an app-neutral bounded feed of immutable published packet wires; UAV APP owns the feed-draining Repo worker, recording manifest, permission-bound key grants, and lifecycle policy; DistributedRepo stores opaque signed wires. Live and replay consumers use the same Mapping, exact-name, validation, decryption, replay, and decoder-admission path. Names-only Mapping remains the default and must lead payload production by measured RTT plus jitter. A nested canonical-packet mode is only a versioned, security-gated fallback if matched evidence proves adequate lead cannot meet the latency objective.

## Technical Context

**Language/Version**: C++17 runtime/UAV/Repo; Python 3 experiment and contract harnesses

**Primary Dependencies**: ndn-cxx 0.9, NFD/MiniNDN, NDNSF LiveStream, NAC-ABE/HybridMessageCrypto, OpenSSL AES-GCM, NDNSF-DistributedRepo, FFmpeg/libx264

**Storage**: Existing RepoCore tiered SQLite backend, storing opaque exact signed Data wires and recording metadata

**Testing**: Boost unit tests, Python contract tests, deterministic H.264 fixtures, fresh-process MiniNDN campaigns

**Target Platform**: Linux UAV/ground-station processes; MiniNDN acceptance environment

**Project Type**: C++ framework plus UAV application and distributed storage service with Python validation tooling

**Performance Goals**: One H.264 encode and one media encryption per canonical source packet; nonblocking bounded retention; at least 99% Interest-before-production for eligible 0% loss prefetch; matched live completion when storage fails; truthful stage-level latency attribution with bounded sampling overhead before any optimization claim

**Constraints**: Signed packets remain within the NDN wire cap; names-only Mapping is default and inline mode stays disabled without the versioned amendment gate; no plaintext keys in persistence/logs; Core imports neither UAV nor Repo; network cells use 60-second measured windows; existing dirty worktree is preserved

**Scale/Scope**: One or more UAV camera sessions, multiple live consumers, one app-owned retention worker per recorded stream, bounded retained ranges and queues

## Constitution Check

### Pre-design gate

- **Canonical Dynamic Runtime — PASS**: No new service invocation mode or split service name; camera control keeps the current dynamic/Targeted path.
- **Security Is Part Of The Data Path — PASS**: Existing permission, protected response, Provider signature, Mapping validation, AES-GCM, and replay gates remain mandatory.
- **CodeGraph First — PASS**: Current `VideoPublisher::captureLoop`, `recordRawChunk`, `publishCurrentFrame`, playback, LiveStream, and Repo paths were traced before design.
- **Spec-driven Change — PASS**: The change spans Core/UAV/Repo contracts and is governed by Spec 120.
- **Right-scope Verification — PASS**: Deterministic wire/security tests precede MiniNDN network acceptance; hardware is explicitly deferred.
- **Cohesive Tasks — PASS**: Tasks are behavioral slices combining test-first work, implementation, validation, and evidence; no one-file or one-command task chain.

### Post-design gate

PASS. The design adds one app-neutral bounded packet-feed boundary but no new network protocol, persistence engine, media codec, crypto scheme, or duplicate fetch controller. Ownership and removal paths are explicit in [contracts/unified-video-retention.md](contracts/unified-video-retention.md).

## Architecture And Ownership

| Concern | Owner | Contract |
|---|---|---|
| Semantic names, Mapping, exact-name prefetch, packet signing, bounded published-wire feed | NDNSF Core LiveStream | Queues immutable packet records; knows no UAV, H.264, keys, or Repo |
| H.264 capture/boundaries, one-time media protection, recording policy, key grants, manifests, retention status | NDNSF-UAV APP | Produces opaque content once and queues exact signed wires without blocking Core |
| Opaque durable packet/manifest storage and exact retrieval | NDNSF-DistributedRepo | Does not parse H.264, decrypt content, or decide permissions |
| Live/replay validation, decryption, ordering, decoder admission | Existing LiveStream handle plus UAV callback | Identical gate for live and retained packets |
| FEC repair | NDNSF LiveStream transport | Optional; separately named; not canonical recording content |
| Correlated timing events and local monotonic spans | Owning Core/UAV stage | Emits bounded sampled facts only; no stage estimates another owner's time |
| Performance aggregation and optimization decisions | Experiment tooling/UAV APP | Joins sampled facts, records clock uncertainty, reports stage distributions and matched evidence |

## Data And Control Flow

```text
FFmpeg H.264 unit
  -> UAV semantic name + Stream reservation
  -> UAV AES-GCM protection (once)
  -> LiveStream signs one canonical Data packet
       |-> NFD/cache/live consumer
       `-> bounded Core published-packet feed
             -> UAV retention worker drains feed
             -> Repo stores exact Data wire + signed Mapping wire
             -> manifest advances durable checkpoint

Protected start/open response
  -> live recipient grant for the session content key

Protected recording-manifest response
  -> historical recipient grants for permitted packet key epochs

Replay producer
  -> serves stored signed wires unchanged under original names
  -> existing LiveStream consumer and UAV admission/decode callback
```

## Prefetch Latency Contract

The latency claim is conditional and uses explicit event origins:

- With a pending Mapping Interest but no Mapping lead, Mapping delivery, payload Interest travel, and payload Data return cost approximately `3/2 RTT` from simultaneous Mapping/payload availability.
- With Mapping published at least one measured RTT plus bounded jitter before payload production, the exact payload Interest is pending at the Provider; production-to-delivery is approximately one-way (`1/2 RTT`) plus processing.
- If neither Mapping nor its Interest is pending, cold retrieval can approach two RTTs.

Spec 120 first requires the existing adaptive Mapping horizon to expose lead time and size its reservation horizon from measured RTT, sample period, and jitter while remaining bounded. If that cannot satisfy the acceptance gate, a separately versioned Mapping mode may carry the exact canonical signed Data wire. Smaller segmentation addresses its wire cap but not security or cache semantics, so the candidate must independently validate outer and inner packets, bind cursor/name/digest/Provider/session, prevent recursion/downgrade, retain ordinary semantic-name service, and pass the matched adoption gate before becoming available.

## Performance Attribution Contract

The current UAV timestamp called `captureMs` is taken only after FFmpeg has
already emitted encoded H.264 bytes. It is therefore an
`encoded-output-ready` event, not a camera-capture event. The existing aggregate
must be renamed `encoded-output-to-decoder-output`; genuine
`capture-to-decoder-output` is valid only when the input acquisition timestamp
is carried through the encoded stream with a recorded uncertainty bound.

Every sampled item uses one stable `(streamId, sessionEpoch, cursor, frameId,
segmentIndex)` correlation key and records the following events when owned by
that stage:

```text
Provider local monotonic clock:
  source-acquired? -> encoded-output-ready -> group-ready
  -> protection-complete -> signed-and-materialized
  -> Mapping-available / payload-Interest-arrived -> Data-put

Consumer local monotonic clock:
  Data-received -> signature-validated -> decrypted
  -> reorder/reassembly-ready -> decoder-input -> decoder-output
```

Question marks denote optional events that must be absent, not synthesized, if
the source cannot supply them. Same-process subtraction yields authoritative
stage durations. Cross-node subtraction is allowed only when clock offset and
uncertainty are measured and retained; otherwise experiments report RTT,
Interest-before-production causal order, and the two local timelines without a
false one-way number.

The shared `NDNSF_TIMELINE_TRACE_SAMPLE_RATE` stable sampler selects the same
cursor IDs at every stage. Events use a dedicated `NDN_LOG` category at
TRACE/DEBUG level; no `std::cout`, `std::cerr`, `printf`, `fprintf`, or
per-packet INFO path is permitted. Bounded per-stage trace queues expose drops
and missing events. Aggregation reports counts, missing counts, p50/p95/p99,
queue/high-water state, CPU/memory, Mapping bytes, Interest/Nack/timeout load,
and tracing overhead. Optimization proceeds only from a frozen baseline: rank
the measured dominant stages, change one algorithm or implementation concern,
run matched pairs, and accept the claim only through SC-016. This separates
Mapping-lead optimization from encryption/signature, retention, scheduling,
and decoder optimization instead of attributing all residual delay to one
component.

## Migration And Rollback

1. Add packet-wire observation, truthful stage timing, tracing-overhead checks,
   and exact-wire deterministic tests without changing current behavior.
2. Add the canonical recorder and replay producer behind the current recording configuration, with a test-only comparison mode that verifies equality but never publishes two authoritative recordings.
3. Switch new recording writes to the canonical format; reject legacy DB format with an export/cleanup message.
4. Remove `recordRawChunk`, recording-only AES-GCM envelope/key file, raw-chunk manifest fields, recording-only decoder reconstruction, and their tests/configuration.
5. Rollback before step 4 by disabling canonical retention. After step 4, rollback requires reverting the cohesive migration commit and recreating test databases; no production compatibility promise exists for experimental legacy DBs.

Stop migration if packet-wire identity, key secrecy, live completion, bounded retention, restart replay, or same-admission validation fails.

## Project Structure

### Documentation

```text
specs/120-uav-unified-video-object/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/unified-video-retention.md
├── checklists/requirements.md
└── tasks.md
```

### Source And Validation

```text
ndn-service-framework/
├── Stream.hpp
└── Stream.cpp

NDNSF-UAV-APP/
├── shared/UavProtocol.hpp
├── shared/UavProtocol.cpp
├── drone/DroneServiceContainer.inc.hpp
├── ground-station/GroundStationServiceContainer.inc.hpp
└── configs/uav_runtime.conf

NDNSF-DistributedRepo/
├── include/ndnsf-distributed-repo/
└── src/

tests/
├── unit-tests/stream.t.cpp
├── unit-tests/uav-protocol-state.t.cpp
├── python/test_ndnsf_core_streaming.py
└── python/test_ndnsf_uav_unified_video.py

Experiments/
└── NDNSF_UAV_Unified_Video_Minindn.py
```

**Structure Decision**: Extend the established LiveStream, UAV, and Repo modules at their current ownership boundaries. Do not create a fourth streaming library, recording protocol, storage engine, or decoder.

## Complexity Tracking

No constitution violation requires justification. The single new Core bounded-feed boundary is necessary because the exact signed packet wire is created inside LiveStream and cannot be reconstructed by UAV APP without a second signature/packet identity.
