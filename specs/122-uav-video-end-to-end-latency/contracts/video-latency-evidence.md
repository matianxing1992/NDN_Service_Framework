# Video Latency Evidence Contract

## 1. Identity contract

The canonical identity is:

```text
(stream_id, session_epoch, source_frame_id, codec_config_epoch)
```

`publication_cursor`, FEC symbol index, packet sequence, decoder output ordinal, and GUI callback number remain separately labelled diagnostic fields. Equality between those numbers never establishes a source-frame association.

The protected UAV video metadata carries:

```text
binding_version
source_frame_id
capture_origin_ns
capture_clock_id
codec_pts
codec_time_base_num
codec_time_base_den
codec_config_epoch
key_frame
```

This metadata is covered by the existing authenticated encryption/signature path. A stale session, duplicate identity with different content, unsupported binding version, or PTS conflict fails closed.

## 2. Required stage vocabulary

Provider/application clock:

```text
capture-observed
encode-admitted
encoded-access-unit-ready
packetization-complete
protection-complete
data-materialized
data-put
```

Consumer clock:

```text
data-received
signature-validated
decrypted
reorder-ready
decoder-queued
decoder-input
decoder-first-output
decoder-output
```

GUI/render clock:

```text
gui-callback-queued
gui-widget-submitted
render-presented
```

`render-presented` is optional and requires a documented sink/compositor observation. Otherwise the end endpoint is `gui-widget-submitted`.

## 3. Join and duration rules

1. Exact canonical identity is mandatory for source-to-output/display joins.
2. Provider-local, consumer-local, and GUI-local durations require a common monotonic clock ID.
3. Cross-role same-host MiniNDN durations require a recorded shared clock authority.
4. Cross-host duration requires offset estimate and uncertainty; otherwise it is unavailable.
5. Startup, reconnect, warmup, and steady samples remain separate distributions.
6. An output interval measures cadence only; it is never per-frame decode latency.
7. Percentiles are computed from per-frame intervals first. Percentiles from different stages are never added.
8. Sampled-out, dropped, ambiguous, and missing stages are counted by reason rather than zero-filled.

## 4. Queue and drop contract

Every media queue declares item/byte capacity. A dropped frame records its canonical identity where known and exactly one reason:

```text
encoder-drop
transport-admission-drop
late-before-decode
decoder-error
superseded-by-newer-frame
gui-session-retired
gui-backpressure
security-reject
unknown-prohibited
```

`unknown-prohibited` invalidates an acceptance cell; it is not an acceptable steady-state category.

## 5. Pipeline adapter contract

The APP-owned adapter provides lifecycle and media callbacks conceptually equivalent to:

```text
probeCapabilities() -> VideoPipelineCapabilities
startCapture(onAccessUnit, onStageEvent)
startDecode(onDecodedFrame, onStageEvent)
submitAccessUnit(protectedPayloadMetadata, bytes)
stop() -> bounded, idempotent completion
snapshotQueues() -> bounded queue metrics
```

This is not a public NDNSF Stream API. It cannot publish Mapping, express Interests, validate NDN signatures, decrypt NDNSF payloads, or choose FEC policy.

## 6. Compatibility and rollback

- Existing Stream semantic names and Mapping/source/repair wire remain unchanged.
- The UAV metadata addition is versioned and additive; old `captureMs` remains readable but is labelled encoded-output-ready.
- `legacy-pipe` remains explicitly selectable until the replacement passes Spec 122.
- A measured cell records one backend and cannot silently fall back.
- Runtime fallback outside a measured cell must be operator-visible with a reason.

## 7. Acceptance evidence

Each cell must emit:

```text
run-summary.json
video-stage-events.csv
cell-summary.json
process-resource.csv
stdout/stderr or NDN logs
```

The summary reports identity coverage, unavailable intervals, startup and steady distributions, stage drops, frame accounting, future Interests, CPU/RSS/PIT/queues, exact command/environment, and terminal status.

Confirmatory summaries additionally record `evidenceRole=confirmatory`, the frozen candidate/configuration digest, pair ID, precommitted order, and predecessor cell. Spec 121 and T005-T008 probe cells use `evidenceRole=exploratory` and are never counted toward default acceptance. Confirmatory baseline `B` is `legacy-pipe + stdio-batched` with the same exact-identity instrumentation. More than 2-times its payload/Interest work per produced displayable frame is a hard default-rejection gate, even when the candidate is retained for further experimental study.
