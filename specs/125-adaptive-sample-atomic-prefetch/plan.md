# Implementation Plan: Adaptive Sample-Atomic Prefetch

**Branch**: `Experimental` | **Date**: 2026-07-19 | **Spec**: [spec.md](spec.md)

## Summary

Replace fixed `sourceItems` and one global `segmentsPerSample` mean with signed,
variable sample groups and bounded per-class conservative predictors. The
Provider uses the predictor to reserve future semantic names in Mapping; the
consumer schedules whole predicted groups, then uses authenticated actual
extent to stop excess work or fetch an underestimated tail. UAV publishes real
H.264 access-unit extents without empty padding and supplies opaque key/delta
class policy. Validate deterministic contracts first, retain the original
frozen 60-second MiniNDN run, then use uniquely named user-authorized
confirmation cells only to close demonstrated implementation defects without
overwriting that evidence.

## Technical Context

**Language/Version**: C++17, Python 3

**Primary Dependencies**: ndn-cxx, NFD/MiniNDN, current NDNSF Stream Mapping,
UAV GStreamer/FFmpeg integration

**Testing**: Boost.Test Stream/UAV suites, Python wrapper tests, existing UAV
MiniNDN 60-second harness and latency analyzer

**Target Platform**: Linux and MiniNDN; no Docker or iTiger work

**Performance Goals**: no partial predicted group scheduling; after warm-up no
UAV key/delta underprediction in the accepted run; no more than 15% Interest
work overhead; retain Spec 124 latency/continuity gates

**Constraints**: semantic Data names remain authoritative; exact Interests;
signed Mapping; bounded state; sampled `NDN_LOG`; APP encryption remains
outside prefetch; FEC optional; no H.264 logic in Core

## Constitution Check

- Canonical runtime and Stream handle ownership: PASS.
- Security remains on the Data path: PASS, with a versioned signed Mapping
  extension and fail-closed validation.
- CodeGraph-first impact trace: PASS.
- Spec Kit required for wire/API/UAV changes: PASS.
- MiniNDN final validation: PASS.
- Cohesive tasks: PASS; four behavioral outcomes, not per-file steps.
- Research grounding: PASS; paper behavior is separated from NDNSF-specific
  signed Mapping and security decisions.

## Design

### 1. Prediction is per class and conservative

Core stores at most `H` authenticated actual source counts for each of at most
`C` opaque classes. For class `c`:

```text
recentHigh(c) = max(last H authenticated actual counts)
predictedSources(c) = clamp(seed(c),
                            recentHigh(c) + safetyMargin(c),
                            hardMax(c))
```

The bounded history is a FIFO and therefore lowers a stale outlier only after
`H` newer observations of the same class. No time-dependent decay or cross-
class mean is permitted. Unknown classes get an independent cold-start profile.

Absolute `predicted >= actual` cannot be guaranteed from history alone. A valid
APP hard maximum can provide that guarantee; otherwise unprecedented large
samples are explicit underprediction events. The acceptance goal is zero after
warm-up, not a dishonest universal theorem.

For a fixed-rate video stream, the APP may initialize the profiles from:

```text
averageFrameBytes ~= targetBitrate / (8 * framesPerSecond)
averageSourceItems ~= ceil(averageFrameBytes / opaqueItemBytes)
```

and use different conservative multipliers/seeds for the known GOP key/delta
schedule. This is a cold-start hint only. Real admitted access-unit extents
train the profiles because fixed bitrate constrains an interval average, not
each frame.

### 2. Mapping v2 carries group scheduling context

Each Mapping entry continues to bind a cursor to the real semantic Data name,
but v2 also encodes canonical:

```text
groupId, opaqueClassId, groupItemIndex,
predictedSourceCount, predictedRepairCount
```

Every entry repeats the small group tuple so a block that begins in the middle
of a group can be validated independently. Resolver admission rejects changing
metadata, non-contiguous indexes, counts above declared caps, conflicting group
IDs, malformed block crossings, and mixed contract versions atomically. The
adaptive descriptor pins v2. There is no silent v1 interpretation. The v1
decoder and existing manual `MappedLiveFutureOn/Off` behavior remain available
for descriptors that explicitly pin v1, but v1 status never reports the new
sample-atomic guarantee.

The Mapping does not contain payload Data, keys, codec bytes, or consumer state.
It remains Provider-signed names-only scheduling metadata with digest continuity.

### 3. Provider reservation and actual extent

Before materializing the next sample, the APP supplies `sampleId`, an opaque
class ID, a semantic-name factory, and declared class bounds. Core predicts `M`
sources, adds selected repair names, and commits those names ahead in Mapping.

At publication, Core admits the real `N` sources:

- `N == M`: publish the group normally.
- `N < M`: publish only real sources and repair; authenticate actual `N`; mark
  predicted suffix terminal-unproduced and never reuse its cursors.
- `N > M`: append an authenticated continuation reservation and publish the
  tail; consumers request it immediately, but counters record an underprediction
  and do not count the tail as successful future prefetch.

The Core group envelope binds actual count, group ID, class, source/repair
indexes, lengths, and digests. APP ciphertext remains opaque content.

### 3a. Public API and ownership

The intended C++ shape is:

```cpp
LiveStreamDefinition definition;
definition.samplePeriodMs = 1000.0 / fps;
definition.sampleClasses = {
  SampleClassProfile::bounded("key", keySeed, maxItems),
  SampleClassProfile::bounded("delta", deltaSeed, maxItems),
};

auto publisher = provider.createLiveStream(definition);
auto sample = publisher->announceSample(
  sampleId, expectedClass,
  [semanticPrefix, sampleId] (size_t itemIndex, LiveStreamItemKind kind) {
    return makeApplicationDataName(semanticPrefix, sampleId, itemIndex, kind);
  });
auto exactNames = publisher->prepareSampleExtent(sample, realItemCount);
auto encryptedOpaqueItems = appProtect(realItems, exactNames);
publisher->publishSample(sample, encryptedOpaqueItems);
```

The Python surface mirrors this shape. `announceSample` may be called several
sample periods ahead when fixed FPS/GOP makes the future class schedule known.
`prepareSampleExtent` is required only when APP AEAD binds exact Data names.
Core chooses the predicted count and Mapping lead; the APP owns only timing,
class declarations, semantic names, already-protected bytes, and valid caps.

Consumer use remains intentionally small:

```cpp
LiveStreamOpenOptions options;
options.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
options.onItem = onOpaqueItem;
auto stream = user.openLiveStream(descriptor, options);
stream->start();
```

The consumer neither selects key/delta windows nor calls the predictor. Read-
only status exposes its decisions. Existing low-level `reserveAhead`/
`reserveGroup` may remain for non-adaptive/manual producers, but UAV and normal
adaptive examples use only the high-level sample API.

### 4. Consumer schedules groups, not arbitrary cursor suffixes

The resolver exposes complete predicted group boundaries. `schedule()` first
builds candidate groups beginning at `m_nextCursor`. It admits a group only if:

```text
all required Mapping is verified
AND group size <= configured atomic group cap
AND free aggregate capacity can hold the whole group
AND processing bounds remain valid
```

The packet window is then rounded upward or downward to group boundaries. A
large key frame may expand it; several small delta frames may share it. Pressure
can remove the last whole future group but never cut it. Mapping and
retransmission reserves remain separate so group atomicity cannot starve Mapping.

Steady demand is computed from actual future group predictions:

```text
sampleDemand = max(1, ceil(networkRetrievalDelay / samplePeriod))
packetDemand = sum(predictedSources_i + predictedRepairs_i,
                   i in next sampleDemand announced groups)
               + wholeGroupRecoveryReserve
```

It is never `sampleDemand * oneGlobalAverage`.

Definition validation also requires the largest declared source+repair group to
fit `aggregateInterestLimit - mappingReserve - retransmissionReserve`. This
turns an impossible atomic group into an activation error instead of a runtime
deadlock.

### 5. Variable FEC and UAV ownership

`LiveStreamFecOptions` declares a maximum variable source count, not one exact
count. `publishGroup()` validates each real group's count against this maximum
and creates the configured repair count from only real source bytes.

For a one-source group, XOR-one-repair is a recoverable copy of that single
opaque source under its distinct signed repair name. An APP may instead select
zero repair for that group. Both paths avoid empty padded source items.

UAV removes `while (dataChunks.size() < m_fecDataShards)` and publishes the
actual access-unit chunks. APP obtains key/delta classification from encoder
metadata where available, supplies class seeds/caps, and chooses the GOP and
packet size. Core neither parses H.264 nor chooses those values.

The UAV encoder freezes target bitrate, FPS, maximum GOP/key interval, and a
predictable keyframe policy for the acceptance cell. It announces several
future sample IDs/classes ahead according to that schedule. If encoder metadata
later disagrees with the announced class, Core records `class-mismatch`, closes
the unused reservation without reuse, and publishes a correctly classified
continuation; the event cannot count as successful future prefetch.

### 6. Evidence and experiment design

Deterministic tests use alternating, burst, outlier, block-crossing, and cap
traces. The MiniNDN cell remains 60 seconds at the existing zero-loss original-
load configuration. Warm-up duration is frozen before execution. Report:

- actual/predicted source count by opaque class;
- under/overprediction distributions and additional-RTT events;
- atomic expansions/deferrals and window group composition;
- actual source + repair items and total Mapping/payload Interest work;
- Provider future interests/hits;
- capture-to-decode and GUI continuity.

The original acceptance run is retained whether it passes or fails and is never
overwritten. The user has subsequently authorized post-fix confirmation after
the run exposed a concrete implementation defect. Each confirmation uses a new
result directory, changes code only in response to a diagnosed correctness
failure, and is retained. This is defect closure, not automatic performance
tuning.

### 7. Migration and rollback

- New adaptive C++/Python examples and UAV sessions produce Mapping v2.
- Existing v1 descriptors continue to open only under their existing manual
  policies; a resolver never admits v1 and v2 blocks into one session.
- Rollback stops the v2 stream and creates a new v1 session/epoch with the
  previous fixed/manual policy. Signed v2 names and cursors are never reused.
- No automatic on-wire downgrade is permitted.

## Project Structure

```text
ndn-service-framework/Stream.{hpp,cpp}
pythonWrapper/src/ndnsf/_ndnsf.cpp
pythonWrapper/ndnsf/streaming.py
examples/streaming/adaptive-sample-atomic.{cpp,py}
NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp
NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp
tests/unit-tests/stream.t.cpp
tests/unit-tests/uav-*.t.cpp
Experiments/NDNSF_UAV_GUI_Minindn.py
Experiments/analyze_stream_latency.py
specs/125-adaptive-sample-atomic-prefetch/
results/spec125-*/
```

## Complexity Tracking

| Complexity | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| Mapping v2 group metadata | Group boundaries must be authenticated before future Interests are issued | Parsing UAV semantic names in Core violates the generic boundary; learning after payload arrival is too late for atomic prefetch |
| Per-class bounded predictor | Key and delta frames have different distributions | One average reproduces the current underfetch/overfetch defect |
| Variable FEC group size | Real frames contain different item counts | Empty padding wastes Interests and masks the measured distribution |
| High-level sample API | Core needs future class/name information without exposing its window algorithm | Requiring every APP to call the predictor or compute packet demand duplicates policy and makes UAV-specific behavior leak into Core |
