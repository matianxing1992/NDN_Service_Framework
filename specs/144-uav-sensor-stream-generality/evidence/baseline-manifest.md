# T001 Baseline Manifest

**Recorded**: 2026-07-24  
**Authority**: current on-disk worktree  
**Formal Spec 144 cells started**: 0

## Frozen Historical Evidence

Directory roots use the contract command:

```bash
rg --files <directory> | sort | xargs sha256sum | sha256sum
```

| Frozen directory | SHA-256 | Status |
|---|---|---|
| `specs/127-cross-application-stream-generality` | `6756fba7fad110f267863f4bb9f5cb1abec5c68515642ba383235a6a6729136d` | MATCH definition |
| `specs/128-generic-multiloss-recovery` | `6f532069382d0dc6b3d853c3ab9251e71993e985ae98a1a57c955d719e03faee` | MATCH definition |
| `results/spec127-cross-application-20260720-confirmation03` | `ef35eb227db611253afe99d312e3eaa18e8c7123898570815cde8756ffd96787` | FROZEN baseline |
| `results/spec128-generic-recovery-20260720-confirmation03` | `0a47cc3da6862f261d687615344dd8e35865e53556c28d31b1758a5eda876c1d` | FROZEN baseline |

No Spec 127 or Spec 128 runner was invoked.

## Promoted Spec 145 Reference

| Artifact | SHA-256 / result | Status |
|---|---|---|
| post-implementation audit | `Verdict: PASS` | MATCH |
| `results/spec145-uav-video-runtime-20260724T064253Z/campaign-summary.json` | `8bad7f90d12647f2904e208000e189ca73a5369ebb33f359a8f07a5560ca2566` | MATCH |
| `zero-loss-20fps-run-01/run-summary.json` | `6d842b6b538ef112a2d0a961fda129d950a9e546769a07974049841bd8055243` | MATCH |

No Spec 145 runner was invoked. Its measured video cell is reference
provenance only and cannot enter a Spec 144 denominator.

## Host and Single-Writer Gate

| Check | Observation | Gate |
|---|---:|---|
| logical CPUs | 4 | record; builds use `-j2` |
| memory | 7.8 GiB total, 5.1 GiB available | PASS |
| swap | 2.0 GiB total | record |
| filesystem free | 45 GiB | PASS |
| active Spec 127/128/144/145 runner | none | PASS |
| active MiniNDN/Mininet/NFD UAV campaign | none | PASS |
| Spec 144 formal receipts | zero | PASS |

## CodeGraph Inventory

CodeGraph was current: 2,638 indexed files, 57,619 nodes, 187,480 edges.

Existing reusable Core path:

- Provider: `LiveStreamPublisher::announceSample`,
  `prepareSampleExtent`, and `publishSample`;
- consumer: `LiveStreamConsumerHandle` opened with
  `LiveStreamStart::Latest` and
  `LiveStreamPrefetchPolicy::AdaptiveSampleAtomic`;
- exact-name fetch: `fetchMapping`, `fetchPayload`, bounded retry,
  deadline/late handling, and generic FEC recovery;
- status: `LiveStreamStatus` already exposes Mapping Interests/Data/new Data,
  Payload initial/retry/future Interests, provider future hits, retry
  attempts/successes/suppressions, timeout, Nack, late/deadline/exhaustion,
  recovery attempts/exhaustions, and the actual `StreamFetchDecision`;
- bindings: existing native and Python `LiveStreamConsumerHandle.status()`
  paths expose these aggregate fields;
- Spec 145 UAV Video: APP owns session-frozen production truth and protected
  bytes; Core owns adaptive consumption and actual fetch decisions;
- existing UAV status: `GetStatus` remains the complete snapshot/fallback and
  is not replaced by the new telemetry stream.

The missing T002 behavior is exact per-attempt outcome conservation and repair
consumption attribution. It is an evidence/trace join, not permission to add a
workload selector or second fetch policy.

## Current On-Disk Baselines

Generic files are already modified by earlier user work. These hashes, not
`HEAD`, are the Spec 144 baseline:

| Path | SHA-256 |
|---|---|
| `ndn-service-framework/Stream.hpp` | `8ab1a74a8c491317b59ca4d24700c8a94bc3156b33d2ac307a5f239f5076faa4` |
| `ndn-service-framework/Stream.cpp` | `ec9f8278606ca7c5105f259ca21a484e481b08d2f42571a4623b2dc386500488` |
| `ndn-service-framework/TimelineTrace.hpp` | `c320167ef449add0fa653c803453496b22aa54c53a59abe934571371b03ad3f1` |
| `ndn-service-framework/TimelineTrace.cpp` | `7463209d47b209abb7563b9157ce33b7ab414719f97137a558e717b549ad4bf2` |
| `pythonWrapper/src/ndnsf/_ndnsf.cpp` | `00ca6ccba6cf391971aa8fd5dc5a57e3eeec40fada64d80968a96bf7e0361aae` |
| `pythonWrapper/ndnsf/streaming.py` | `bf824053fe7b4175c45e16207c99517c45fe8b352d411e94a1c13d562de29f15` |
| `NDNSF-UAV-APP/shared/UavProtocol.hpp` | `9e280080a2e8f222979a406ce1425280ba9f3c38c1b07a4b92c7f86d6ecdf7ec` |
| `NDNSF-UAV-APP/shared/UavProtocol.cpp` | `a0a7bc629cec941835ef4dd6837529b7a6e615be406d8e427dc3f8864ebc7faa` |
| `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp` | `6d90f1df716a421b9ba1131262ec28ebab127553bea77a34ef58ee91de0443ce` |
| `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp` | `852932b9d14dbb17e5d3108928c7578bddad53045e3800f1be00d5faf8d004a1` |
| `tests/unit-tests/stream.t.cpp` | `950ec9e7f5d4c3b12bac97cc1b182765fcb0f48169ff6b5c7ee5b787f3dd513e` |
| `tests/unit-tests/uav-protocol-state.t.cpp` | `3fe015c2ff78f05f24901180ba9acee7373c688cac6970c860955b3352c3e6f1` |

Existing reusable Spec 127 fixture baselines:

| Path | SHA-256 |
|---|---|
| `examples/python/live_stream/workload_common.py` | `d3c2f8a97b05f9f9d0a50b0bf26a478f02724462c46a5f27f098ac0b53c0ae0f` |
| `examples/python/live_stream/workload_provider.py` | `1d06f140887e4fa82e6670ecd9bb7d75cfb4c19085e84b5ebb4f8e9798ebe2b2` |
| `examples/python/live_stream/workload_consumer.py` | `945312a91c6f34304f708a2731a07f600b892887cbd63d7b1a383529052dac1c` |
| `Experiments/NDNSF_LiveStream_Generality_Minindn.py` | `45746a7574805d7adb0a123f37e01d2ea1608cb27cb23ad8160c50f55ce59ae2` |
| `tests/python/test_ndnsf_live_stream_generality.py` | `bf0aab301758940310acb4e06263367191bf4e0446a9505cab526aacb858d2f5` |
| `tests/python/test_ndnsf_live_stream_minindn.py` | `c058aef4707a05585aa1d2c7fa98f1a99df97d08def3fc8efd5c8e136389026d` |

## Anticipated Changed-File Set

Allowed:

- new `NDNSF-UAV-APP/shared/UavSensorStreams.hpp/.cpp`;
- focused integration hunks in the recorded Drone, Ground Station, and UAV
  protocol files;
- focused Stream/TimelineTrace/binding changes only if T002 proves existing
  aggregate status cannot satisfy exact generic outcome conservation;
- new Spec 144 analyzer, MiniNDN launcher, matrix runner, and Python tests;
- focused native Stream/UAV tests and build registration;
- Spec 144 evidence, traceability, and completion files;
- `.planning/STATE.md` session continuity.

Forbidden:

- changes to Specs/results 127, 128, or 145;
- UAV/telemetry/audio/codec/workload selectors in Core or bindings;
- a manual APP Interest loop or second prefetch policy;
- changes to frozen fault profiles or formal thresholds;
- cleanup or normalization of unrelated dirty-tree hunks.

## Baseline Literal Classification

The case-insensitive Core/binding scan found only:

- documentation examples mentioning video/telemetry/log feeds and
  codec-neutrality;
- generic `NetworkTelemetrySnapshot` ACK-selection metadata unrelated to UAV
  sensor meaning;
- one pre-existing TimelineTrace sampling comment describing a UAV frame rate.

No baseline Core/binding decision branch is selected by UAV identity, telemetry
fields, audio/acoustic meaning, codec identity, workload, or formal cell.

