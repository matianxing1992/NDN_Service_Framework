# Research: Adaptive Sample-Atomic Prefetch

## Paper finding

Gusev et al., *Real-Time Streaming Data Delivery over Named Data Networking*
(2016), state that a video consumer does not know the next frame's segment count
in advance. NDN-RTC therefore keeps separate estimators `Mkey` and `Mdelta`,
issues a bundle of Interests for the predicted frame, learns actual `N` from
segment metadata, requests more when `N > M`, and accepts some unanswered
Interests when `N < M` because that is preferable to an additional RTT.

This directly rejects a fixed `13`-Interest group and one global average.

## Current-code finding

The current implementation only partially realizes the paper design:

- `StreamAdaptiveFetcherState` learns one EWMA `m_segmentsPerSample` across all
  sample types.
- `livePacketDemand()` multiplies sample demand by that mean and adds a fixed
  recovery reserve, so a 24-packet decision can cut two 13-item groups as
  `13 + 11`.
- `schedule()` iterates cursors and stops at a packet budget without knowing
  sample boundaries.
- Mapping v1 binds only cursor to original name/tombstone, so Core cannot know a
  frame boundary before fetching payload.
- UAV pads every group to `m_fecDataShards`; small frames become empty source
  items, and real key/delta distributions disappear.
- Generic FEC rejects any group whose source count differs from one session-wide
  `sourceItems` value.

Therefore a window-only edit cannot fix the defect.

## Decisions

### Provider-side prediction for signed ahead Mapping

The paper places estimators at the consumer. NDNSF has an additional invariant:
future exact semantic names must be present in Provider-signed Mapping before a
consumer may issue them. The Provider-side Core therefore owns the authoritative
reservation predictor, while the consumer validates group metadata and records
prediction error. This is an NDNSF adaptation, not a claim about the paper.

### Per-class conservative history

Use the bounded recent maximum plus a safety margin for each class, not a fixed
value, percentile that intentionally misses a tail, or mean. The APP supplies
opaque class identity and defensible seed/cap policy. Core does not know that
`key` and `delta` refer to video.

### Whole predicted group as the scheduling atom

The scheduler may over-request a predicted group, but must not fetch only a
prefix because its scalar window ended. This implements the user's latency
priority: bounded excess work is preferable to an avoidable RTT.

### Honest guarantee boundary

History cannot guarantee the size of an unprecedented future frame. A hard
APP/codec cap can guarantee no underprediction only if valid. Otherwise the
system minimizes and measures underprediction; it does not assert that it is
impossible.

### Mapping metadata rather than name parsing

Encoding group/class structure into the application's semantic name would make
Core depend on APP naming. Repeating bounded canonical group metadata inside
Mapping v2 preserves arbitrary meaningful Data names and makes boundary checks
trustworthy before payload retrieval.

## Alternatives rejected

- **Set the window permanently to the largest observed key frame**: avoids some
  underfetch but wastes work for every delta frame and adapts poorly after a
  stream configuration change.
- **Keep fixed 12+1 FEC and call it atomic**: hides rather than learns variable
  frame extents and publishes empty source items.
- **Infer group boundaries from decrypted `StreamChunk`**: arrives after the
  scheduling decision and would couple Core prefetch to APP encryption/media.
- **Put payload Data inside Mapping**: restores Data-in-Data overhead and mixes
  control trust with application payload delivery.
- **Add a separate group-manifest fetch**: adds another control Interest and can
  add latency; group metadata fits the Mapping object that is already required.

## Experiment variables

- Independent: fixed legacy versus adaptive group policy; key/delta actual size
  trace; predictor seed/margin; loss remains zero for the initial gate.
- Dependent: underprediction events, excess predicted Interests, atomic
  deferrals, Provider future-hit ratio, Interest work/frame, capture-to-decode,
  decoded/displayed continuity.
- Controls: topology, RTT/link, video source, bitrate, GOP, packet cap, FEC,
  warm-up, measured 60-second window, logging sampler, build, GUI environment.
- Confounds: encoder keyframe decisions, startup codec output, Mapping lead,
  tracing overhead, and frame classification errors. Each must be logged or
  frozen rather than folded into the predictor result.

## Source

- D. Gusev et al., “Real-Time Streaming Data Delivery over Named Data
  Networking,” IEICE Transactions on Communications, vol. E99-B, no. 5, 2016.
  https://irl.cs.ucla.edu/data/files/papers/gusev2016realtime.pdf
