# UAV Video Runtime Correction Contract

## Class Schedule

1. One immutable backend class contract is created before the first future
   announcement.
2. Announcement and publication query the same contract.
3. GStreamer exact access units use a frozen FPS-derived key/delta schedule.
4. GStreamer actual key/delta flag is validated against that schedule.
5. Legacy byte groups always use one conservative `opaque` class with a hard
   source-extent bound and never infer key/delta from sequence.
6. Committed Mapping is never relabeled.
7. A contradiction terminates only the affected video session with a stable
   failure reason.

## GStreamer Callback Boundary

Both `onCaptureSample` and `onDecodeSample` satisfy:

```text
C entry
  -> no-throw adapter
  -> C++ handler
  -> application callback
  -> GstFlowReturn
```

On any exception:

```text
retain first bounded reason
transition Running -> Failed
suppress future application callbacks
return GST_FLOW_ERROR
do not recursively stop the pipeline
```

No exception may cross the C ABI. Payload bytes, keys, and protected content
must not appear in failure text.

## Core Status Truth

Ground Station accepts a fetch-decision snapshot only when:

```text
callbackGeneration == activeConsumerGeneration
and status.fetchDecision is present
```

The display then copies actual Core fields. When the predicate is false, the
display reports `available=false`, `source=unavailable`, and no estimated
window/lookahead.

Application bitrate decisions and configured caps remain visible only under
APP-owned labels. They do not overwrite or masquerade as Core transport state.

## Ownership Boundary

Allowed implementation owners:

- UAV video class scheduling;
- UAV GStreamer callback adapter;
- Ground Station decision snapshot/presentation;
- UAV protocol state serialization for additive display metadata;
- focused tests and fresh Spec 145 analyzer/runner configuration.

Forbidden owners without a renewed BLOCK-resolving audit:

- generic `Stream` fetcher/publisher;
- ServiceUser/ServiceProvider;
- Python bindings;
- Mapping/FEC/retry algorithms;
- Spec 125/126 documents, runners, or results.
