# Implementation Plan: UAV Low-rate Decode and Stop-race Repair

## Design

Focused red evidence disproved capture callback delay and isolated the
four-frame latency to `avdec_h264`: at 10 fps the live capture-to-decode median
is 400.423 ms. The encoder already sets `bframes=0`, so automatic frame-thread
buffering adds latency without decoding-order benefit.

```text
Before: avdec_h264 max-threads=0 (auto; about four buffered frames)
After:  avdec_h264 max-threads=1 (ordered low-latency decode)
```

Capture remains clock-paced at the encoded sink because its focused 10/60-fps
rate and capture-callback lag test already passes.

The MiniNDN launcher changes one immediate post-exit assertion:

```text
require_log(drone, "video stopped")
  ->
wait_log(drone, "video stopped", 5, drone_proc) then require
```

No formal factor or acceptance threshold changes. After focused/full gates, a
new Spec 153 wrapper freezes and executes the same six-rate matrix under new
hashes and a unique root.

## Constitution Check

- Dynamic predictive API/security: unchanged.
- CodeGraph/source verified: capture and launcher ownership confirmed.
- Spec-driven/frozen evidence: successor Spec, no old rerun.
- MiniNDN and >=60-second windows: retained.
- Cohesive tasks and GSD resumability: retained.
- ARS matched experiment design: factors and gates unchanged.
