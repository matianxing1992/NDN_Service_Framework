# Implementation Plan: UAV Video True End-to-End Latency

**Branch**: `Experimental` | **Date**: 2026-07-19 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/122-uav-video-end-to-end-latency/spec.md`

## Summary

Replace the ambiguous encoded-group/output-frame timing with a codec-aware source-frame identity and an honest acquisition-to-render stage model. Establish a corrected 60-second baseline first. Then evaluate persistent decoding, low-delay codec settings, a timestamp-preserving GStreamer backend, bounded newest-frame delivery, and direct rendering one variable at a time. Retain a new default only after five counterbalanced matched MiniNDN pairs pass latency, continuity, security, Interest-efficiency, and resource gates; keep the existing FFmpeg pipe path as an explicit rollback until then.

## Technical Context

**Language/Version**: C++17, Python 3

**Primary Dependencies**: ndn-cxx 0.9, NFD/MiniNDN, NDNSF Stream, GTKmm, FFmpeg 4.2 legacy subprocess, GStreamer 1.16 system development API with `appsrc`, `appsink`, `h264parse`, `avdec_h264`, `v4l2src`, `x264enc`, and `gtkglsink` capability probes

**Storage**: Sampled NDN logs plus JSON/CSV evidence under unique local `results/spec122-*` directories; no runtime database

**Testing**: Boost unit tests, Python trace/experiment tests, Xvfb GUI smoke, 60-second MiniNDN UAV GUI cells

**Target Platform**: Ubuntu 20.04/MiniNDN; deterministic file/test source for acceptance, optional V4L2 and hardware probes

**Project Type**: C++ framework plus UAV application, GTK ground station, and Python experiment harness

**Performance Goals**: Valid capture-to-widget p95/p99; first decoder output at most 150 ms; 30 FPS output-gap p99 below 67 ms; retained candidate improves true p95 by at least 25% in four of five matched pairs

**Constraints**: Preserve semantic names, signed Mapping, exact-name prefetch, Provider validation, AES-GCM/replay resistance, optional FEC, bounded queues, sampled `NDN_LOG`, 60-second measured windows, and no Data-in-Data

**Scale/Scope**: One Provider/one Ground Station MiniNDN stream at 30 FPS, fixed resolution/bitrate/GOP, zero loss for attribution followed by bounded loss regression; GPU, iTiger, Docker, and outdoor flight are excluded

## Constitution Check

- **Canonical runtime**: PASS. Camera control and Stream public entry points remain unchanged unless an additive versioned frame-identity field is proven necessary.
- **Security data path**: PASS. Frame identity becomes authenticated application metadata; no signing, encryption, nonce, replay, or Provider-validation bypass is allowed.
- **CodeGraph/source reality**: PASS. Current code confirms `captureMs` is stamped after encoded output, FFmpeg emits MJPEG through `image2pipe`, and GTK performs idle/Pixbuf/Dispatcher work.
- **Spec-driven durable work**: PASS. This is a cross-file application/media architecture and experiment-plan change.
- **Right validation scope**: PASS. Deterministic tests and 60-second MiniNDN cells precede hardware probes.
- **Cohesive tasks**: PASS. Each task owns one behavioral result including its failing test, implementation, focused gate, and evidence.
- **Dirty worktree**: PASS. No cleanup, commit, branch rewrite, or push is included.

## Current Stage-Time Reality

| Boundary | Current evidence | Interpretation | Required action |
|---|---:|---|---|
| camera acquisition to encoded output | unavailable | `captureMs` currently begins after FFmpeg emits encoded bytes | create acquisition identity/timestamp and codec-aware association |
| encoded output to packet flush | bounded by the provisional 20 ms code default | current POSIX partial flush removed the old near-one-second stdio wait | retain as baseline; do not call it accepted; coordinate with access-unit boundaries |
| Data put to receive p95 | 14.643 ms | measured same-host delivery with a future exact Interest already pending | preserve; it is not the dominant current stall |
| receive through decoder input | separately traceable but not yet closed as one frame distribution | validation, decrypt, reorder, and queue stages need the new frame identity | add exact stage joins and queue/drop accounting |
| first decoder input to first output | 87 ms | one-time startup, not per-frame decode latency | test persistent/prewarmed decoder and key-frame readiness |
| decoder output gap p99 | 142 ms | steady tail stall of roughly four 30 FPS frame periods | primary steady-state optimization target |
| decoder callback to GTK widget p95 | 19 ms | near one 60 Hz refresh interval, but only widget submission | eliminate extra MJPEG/Pixbuf/callback work where evidence supports it |
| widget submission to physical scan-out | unavailable | GTK callback completion is not compositor presentation | label honestly; add direct presentation observation only if backend exposes it |

## Architecture And Ownership

| Concern | Owner | Rule |
|---|---|---|
| Mapping, exact Interests, validation, bounded fetch, FEC | NDNSF Stream Core | No codec or GUI policy enters Core. |
| capture timestamp, frame identity, codec PTS, access-unit boundaries | UAV Provider APP | Identity is created before encode/batching and integrity-bound to protected payload metadata. |
| decode lifecycle, PTS-to-output association, newest-frame queue | UAV Ground Station runtime | Never infer identity from FIFO position. |
| GTK widget/direct video sink and display-drop policy | UAV GUI | Report widget submission separately from observed presentation. |
| experiment orchestration and analysis | `Experiments/` | Freeze cells, reject invalid joins, and preserve negative results. |

### Spec 121/122 ownership boundary

Spec 121 is complete for Mapping-frontier continuity, latest-join initialization, sequence-domain separation, and rejection of false H.264 FIFO correlation. Its one baseline/candidate pair remains exploratory evidence only. Spec 122 owns the missing acquisition identity, codec PTS/output binding, true capture-to-widget or observable-presentation metric, all new confirmatory identities, and the only gate allowed to promote a performance default. No Spec 121 result cell may be counted in Spec 122's five-pair acceptance matrix.

## Design Decisions

### 1. Measurement correctness precedes optimization

Introduce a `SourceFrameIdentity` at acquisition and a `VideoStageEvent` vocabulary matching the contract. A frame can enter end-to-end distributions only when codec PTS or an equivalent authenticated mapping proves the source/output association. The old `captureMs` field remains wire-readable during migration but is relabelled `encodedOutputReady`; it cannot silently become acquisition time.

For backend-independent baseline validation, the deterministic acceptance source also embeds a small H.264-resilient machine-readable frame marker. The analyzer recovers it after decode as an independent oracle. This oracle is experiment-only: production identity remains authenticated metadata plus codec PTS, and the oracle must agree with it before an end-to-end sample is admitted.

### 2. APP-owned media pipeline boundary

Add a narrow `UavVideoPipeline` adapter owned by the UAV application. It exposes captured-frame/access-unit metadata, decoded-frame identity, lifecycle, bounded queue metrics, and rendering capability. It does not expose or replace NDNSF Stream APIs. The current FFmpeg subprocess becomes the `legacy-pipe` adapter; a timestamp-preserving GStreamer adapter is a candidate, not an assumed winner.

### 3. GStreamer feasibility before migration

The local system has the required GStreamer development API and plugins, but `/opt` and system versions differ. A focused capability gate must build against the selected system ABI, carry PTS through `videotestsrc/v4l2src -> x264enc -> appsink` and `appsrc -> h264parse -> decoder -> appsink/gtkglsink`, and prove teardown under Xvfb. If GStreamer fails, the same gate may perform one bounded in-process libavcodec/libavformat PTS probe. If neither backend proves exact association and bounded lifecycle, implementation is BLOCKED at the capability gate; the legacy adapter remains operational but cannot be used to manufacture production end-to-end attribution.

### 4. Codec-aware identity

Provider output is aligned to H.264 access units. The source-frame ID, capture-origin time, and codec PTS are authenticated inside the UAV protected payload metadata. The consumer supplies the same PTS to the decoder input and accepts an output association only when the decoder returns that PTS. B-frames are disabled in the low-latency candidate; identity correctness still depends on PTS rather than output order.

### 5. Persistent decoding and join readiness

Measure process/pipeline creation, codec configuration, first key-frame arrival, first decoder input, and first output separately. Test persistent/prewarmed decoding independently from GOP/SPS/PPS changes. Latest join must start from an authenticated decodable boundary; it may wait for a key frame but cannot decode arbitrary delta data or manufacture latency.

### 6. Bounded newest-frame rendering

Replace multi-stage unbounded work with a bounded latest-frame mailbox at the APP/GUI boundary. Drop decisions retain identity and reason. Evaluate removal of `H.264 -> MJPEG -> Pixbuf` separately from queue policy. Direct rendering is retained only if it exposes a usable widget or presentation observation and preserves headless operation.

### 7. Fixed experiment ladder

The dependency order is: deterministic identity fixture; APP adapter boundary and capability probe; exact runtime correlation; corrected legacy/provisional-code-default baseline; one-variable exploratory startup candidates; one-variable exploratory steady pipeline/renderer candidates; freeze one selected configuration; five new counterbalanced confirmatory pairs; five new counterbalanced trace-off/on pairs; controlled-loss regression. Do not combine persistent decoding, encoder flags, GStreamer, direct rendering, and queue policy in one experiment cell.

## Candidate Decision Table

| Candidate | Prediction | Primary metric | Reject when |
|---|---|---|---|
| persistent/prewarmed legacy decoder | startup falls; steady cadence unchanged | first-input to first-output | startup does not improve in 4/5 pairs or lifecycle leaks appear |
| low-delay encoder with frequent authenticated key-frame readiness | join/startup falls | join-to-first-output | bitrate/CPU or frame completion violates bounds |
| access-unit-aligned packetization | output bursts and FEC amplification fall | output-gap p99, Interest work | future-hit/correctness regresses |
| GStreamer timestamp backend | valid source/output joins become available and pipe jitter falls | attribution coverage, capture-to-widget p95 | ABI/plugin/teardown gate fails or no repeatable benefit |
| in-process libav fallback probe | supplies exact PTS if GStreamer capability fails | PTS round trip and lifecycle | probe fails; return to design rather than integrate |
| bounded latest-frame mailbox | GUI backlog and frame age fall under induced GUI pressure | queue depth, frame age | unexplained/drop completion exceeds gate |
| direct video sink | conversion and callback work fall | render-stage p95, CPU | no presentation observation, headless regression, or integration instability |
| hardware decoder | CPU falls without latency/correctness loss | CPU and output-gap | unavailable fallback, output mismatch, or startup regression |

## Experiment Design

### Variables

- **Independent variable**: exactly one candidate selection or parameter per cell.
- **Primary dependent variable**: steady capture-to-widget p95 for exactly correlated frames.
- **Secondary variables**: startup, capture-to-widget p99, output-gap p99, displayed completion, stage drops, future-hit ratio, Interest work, CPU, RSS, PIT, queue high-water marks.
- **Controls**: source asset, frame rate, resolution, bitrate, GOP, FEC, Mapping policy, topology, link, warmup, duration, logs, sampler, build, and GUI environment.

### Matched analysis

Freeze the candidate and every confirmatory cell before execution. In confirmatory pairs, `B` is the exact-identity-instrumented `legacy-pipe + stdio-batched` rollback configuration and `C` is the complete selected configuration; this prevents the provisional 20 ms path's existing 8.3-times amplification from becoming the denominator. Use the precommitted sequence `B→C, C→B, B→C, C→B, B→C` for the five baseline/candidate pairs, and `OFF→ON, ON→OFF, OFF→ON, ON→OFF, OFF→ON` for trace controls. Report pairwise absolute/relative differences and the median paired effect; do not treat the 10 cells as independent samples. Exploratory T005-T008 and Spec 121 cells are excluded from confirmatory counts. The retention gate requires the direction and thresholds in at least four pairs. Failed or invalid cells are terminal and never silently replaced.

### Stop conditions

Stop a candidate family on identity ambiguity, protocol/security failure, continuity loss, unexplained frame loss, queue bound violation, more than 2x Interest work, more than 15% CPU, more than 10% RSS/PIT increase, or lack of benefit after the focused probe. A >2x Interest candidate may be documented as experimental but is ineligible for default promotion.

## Project Structure

```text
NDNSF-UAV-APP/
├── shared/
│   ├── UavProtocol.{hpp,cpp}
│   └── UavVideoPipeline.{hpp,cpp}          # APP-owned adapter and timing model
├── drone/DroneServiceContainer.inc.hpp     # provider integration
└── ground-station/
    ├── GroundStationServiceContainer.inc.hpp
    └── GroundStationWindow.inc.hpp
Experiments/
├── NDNSF_UAV_GUI_Minindn.py
├── analyze_stream_latency.py
└── run_spec122_video_latency_matrix.py
tests/
├── unit-tests/uav-protocol-state.t.cpp
└── python/test_ndnsf_stream_latency.py
specs/122-uav-video-end-to-end-latency/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── contracts/video-latency-evidence.md
├── quickstart.md
└── tasks.md
```

**Structure Decision**: Keep media capture/codec/rendering entirely within `NDNSF-UAV-APP`; reuse the existing Stream Core without a new transport API or wire name. Add only authenticated UAV payload metadata required for exact frame correlation.

## Validation Strategy

1. Deterministic fixture proves frame identity across one-to-many, drop, reorder, reconnect, and retired-session cases.
2. GStreamer capability gate proves selected ABI/plugins, PTS preservation, bounded start/stop, and Xvfb/headless behavior before integration.
3. Corrected 60-second legacy/provisional-code-default baseline reports every available stage and refuses unavailable capture/presentation claims.
4. Single-variable probes rank the bottleneck using true stage evidence.
5. Five new counterbalanced matched 60-second pairs decide whether to retain one default; separately counterbalanced trace-off/on cells quantify instrumentation overhead.
6. Zero-loss attribution is followed by controlled-loss/FEC and stop/restart regression, not a new tuning matrix.

## Post-Design Constitution Check

PASS. The plan keeps NDN transport/security invariants unchanged, places media policy in the APP, rejects false cross-clock/frame joins, provides rollback, uses MiniNDN, and turns test/implementation/evidence for one behavior into cohesive tasks.
