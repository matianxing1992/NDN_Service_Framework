# Workload Contract

## Shared transport contract

Both workloads:

1. create one provider-owned Mapping v2 live stream;
2. announce future sample groups before payload materialization;
3. use APP-defined opaque class IDs only as bounded extent hints;
4. bind protected source/repair bytes to exact mapped names and session;
5. open the consumer at Latest with AdaptiveSampleAtomic;
6. let Core select runtime window/lookahead from generic state;
7. deliver only validated complete application units;
8. expose all status and trace data required by the evidence contract.

These integration steps use the corrected Spec 145 UAV Video path as their
formal APP-side reference. Reuse is limited to lifecycle ownership,
exact-name/security admission, fail-closed APP callback boundaries, and
generation-fenced actual Core status. Video class labels, key/delta scheduling,
FPS/GOP rules, codec behavior, payload schema, and measured video thresholds
are not part of this contract and MUST NOT be copied.

No application may pass a fixed predictive fetch window or tell Core that a
sample is telemetry, audio, keyframe, or codec data.

## Telemetry workload

| Property | Frozen value |
|---|---|
| Workload ID | `uav-telemetry-20hz-single` |
| Semantic role | latest-state observation |
| Period | 50 ms |
| Measured duration | 60 s |
| Measured samples | 1,200 |
| Encoded size cycle | 256, 384, 512 bytes |
| Source items/sample | exactly 1 |
| Repair items/sample | 0 |
| FEC | none |
| Join | latest safe join |
| State admission | strictly monotonic sample ID |
| Fallback | existing `GetStatus`; not part of delivery counts |

The compact payload contains a deterministic serialization of UAV identity,
source time, position/motion, battery, readiness, and link state. Padding used
to reach the frozen size is deterministic and authenticated.

## Acoustic/audio workload

| Property | Frozen value |
|---|---|
| Workload ID | `uav-acoustic-40ms-variable-two-repair` |
| Semantic role | continuous-media block |
| Period | 40 ms |
| Measured duration | 60 s |
| Measured blocks | 1,500 |
| Source-count cycle | 2, 3, 4 |
| APP extent classes | `opaque-block-2`, `opaque-block-3`, `opaque-block-4`; exact 2/3/4 bounds |
| Maximum source bytes | 512 bytes/item |
| Repair items/block | exactly 2 |
| Recovery capacity | two erasures |
| Join | latest safe join |
| Admission | complete ordered block only |
| Formal source | deterministic/file-backed opaque bytes |

The formal contract does not name or require PCM, Opus, ALSA, playback, or a
microphone. Those may be added later at the UAV application boundary.

The three class labels are opaque to Core. UAV-APP selects the class from the
already-known block extent so Core never predicts or fetches an unproduced
source slot; Core sees only exact generic lower/upper item bounds.

## Timing contract

- Readiness must complete before warm-up.
- Warm-up is five seconds and excluded from delivery/latency denominators.
- The measured window is exactly 60 seconds.
- Traffic counters cover the full run and carry
  `trafficCounterScope=full-run-including-warmup`.
- Sample/block counts and latency observations carry
  `measurementScope=measured-window-only`.
- A formal cell fails preflight if the shared monotonic clock domain cannot be
  established.

## Prohibited substitutions

- no variable publish rate in place of the frozen cadence;
- no FEC added to telemetry;
- no source count outside the acoustic 2/3/4 cycle;
- no codec-dependent Core policy;
- no shortened formal window;
- no replacement of a failed cell;
- no use of Spec 127/128 results as Spec 144 repetitions.
