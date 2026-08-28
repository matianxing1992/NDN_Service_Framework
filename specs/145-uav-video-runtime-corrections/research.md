# Research and Source Audit

This feature needs no external literature review. Its design is derived from
the current implementation and frozen local evidence.

## Finding 1: the generic Streaming path is already the right path

The UAV Provider already uses Core `announceSample`, `prepareSampleExtent`, and
`publishSample`. Ground Station already opens with `LiveStreamStart::Latest`
and `LiveStreamPrefetchPolicy::AdaptiveSampleAtomic`. Therefore this repair
must not add an application fetch loop or another prefetch algorithm.

**Decision**: Preserve Core and repair only UAV-side session metadata,
callback safety, and status presentation.

## Finding 2: video class sources disagree outside the tested category

`ensureFutureSampleAnnouncementsLocked` predicts key frames from the configured
target FPS. The legacy fallback in `publishCurrentFrame` uses `frameSeq % 30`,
while its FFmpeg command fixes `keyint` and `min-keyint` at 60. More
fundamentally, the legacy pipe packetizes byte reads/timeouts rather than exact
H.264 access units, so it cannot truthfully name each group key or delta. The
GStreamer path does preserve exact access units, configures key interval from
FPS, and obtains actual class from `GST_BUFFER_FLAG_DELTA_UNIT`.

**Decision**: Freeze one validated backend class contract per session.
GStreamer uses a scheduled key/delta class checked against its authoritative
flag. Legacy uses one bounded conservative `opaque` class for announcement and
publication; a GOP setting cannot substitute for missing access-unit identity.

**Rejected alternatives**:

- Keep special cases for 30 fps: leaves supported non-30-fps configurations
  contradictory.
- Apply the FPS formula to legacy groups: produces internally matching but
  semantically false key/delta labels because groups are not access units.
- Relabel Mapping after encode: violates immutable future-name semantics.
- Ignore GStreamer mismatch: risks false key/delta metadata and undecodable
  joining behavior.

## Finding 3: exceptions can cross a C callback

`onCaptureSample` and `onDecodeSample` directly call C++ handlers. Those
handlers emit an application callback after releasing their mutex. A thrown
application exception can therefore escape into GStreamer C code.

**Decision**: Add one common no-throw adapter at the two static C entry points,
mark failure, suppress later emission, and return `GST_FLOW_ERROR`. Teardown
stays with the owner rather than running recursively in the streaming thread.

## Finding 4: the real Core decision already exists

The Ground Station live-stream status callback receives
`status.fetchDecision` and currently logs its actual window/lookahead. But
`currentVideoAdaptiveState` calls `computeVideoAdaptivePolicy` and assigns that
application decision's values to fields presented as transport state.

Core `StreamFetchDecision` already exposes window, lookahead, interest
lifetime, missing timeout, phase, policy mode, capacity reason, and reason.

**Decision**: Cache the actual active-generation decision and display it.
Keep application bitrate advice and resource-pressure interpretation separate.
Before the first real decision, display unavailable—not an estimate.

## Finding 5: frozen evidence remains historical evidence

Spec 125 confirm06 is valid evidence for the tested 30-fps GStreamer zero-loss
path. Spec 126 is a frozen loss/reorder matrix with retained failures. Neither
proves the repaired non-30-fps and callback/status edges, and neither should be
rerun.

**Decision**: Hash-check old evidence and create one new Spec 145 acceptance
cell at 20 fps. This cell validates only the repair boundary.
