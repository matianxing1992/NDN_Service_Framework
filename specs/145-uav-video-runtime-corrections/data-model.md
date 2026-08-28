# Data Model

## VideoSampleClassSchedule

Application-local immutable session value.

| Field | Meaning |
|---|---|
| `fps` | validated accepted session FPS |
| `mode` | `exact-key-delta` or `bounded-opaque` |
| `keyFrameInterval` | exact-mode interval; absent in opaque mode |
| `opaqueHardMaxSources` | conservative legacy extent bound |
| `sessionGeneration` | owning UAV video session generation |

Operations:

- `classFor(sampleId) -> key|delta|opaque`
- validate FPS/interval before the first future announcement
- no mutation after Mapping announcement begins

Only exact mode may return key/delta. Bounded-opaque mode always returns
`opaque` because the legacy byte group has no exact access-unit identity.

## PipelineFailure

First-failure-only application-local record.

| Field | Meaning |
|---|---|
| `failed` | whether the pipeline entered terminal failure |
| `code` | stable bounded category such as `capture-callback-exception` |
| `reason` | bounded sanitized diagnostic; never payload bytes |
| `direction` | `capture` or `decode` |

State transition:

```text
Idle -> Running -> Failed -> Stopped
```

`Running -> Failed` is atomic and retains the first failure. Failed pipelines
emit no later application frames.

## CoreFetchDecisionSnapshot

Ground Station application cache of an already-computed Core decision.

| Field | Meaning |
|---|---|
| `available` | an actual decision exists for the active generation |
| `source` | `core-live-status` or `unavailable` |
| `consumerGeneration` | active local handle generation |
| `observedAtMs` | receipt time of status callback |
| `decision` | copied Core `StreamFetchDecision` |

The snapshot is cleared on consumer start, stop, or replacement. A callback
whose captured generation differs from the current generation is ignored.

## VideoAdaptiveState Ownership

| Field group | Owner/source |
|---|---|
| window, lookahead, interest/missing timeout | actual Core decision |
| phase, policy mode, capacity reason, decision reason | actual Core decision |
| Core availability/source | Ground Station cache metadata |
| requested/accepted/suggested bitrate and bitrate action | UAV application |
| decoder backlog/reorder and pressure interpretation | UAV application |
| configured future-probe/resource cap | UAV application configuration |
| delivery/FEC/timeout/Nack counters | observed runtime counters |

Any additive encoded fields use backward-compatible defaults:
`available=false`, `source=unavailable`, empty Core labels. Old readers may
ignore the fields; new readers must never infer availability from nonzero
window values.
