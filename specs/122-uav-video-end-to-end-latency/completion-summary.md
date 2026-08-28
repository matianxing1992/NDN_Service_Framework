# Spec 122 Completion Summary

## Outcome

Spec 122 is complete with a **negative promotion decision**. The work added a real acquisition identity, authenticated frame/PTS binding, a GStreamer access-unit pipeline, exact decoder-output association, and widget-submission timing. The new path works end to end, but it is not eligible to replace `legacy-pipe`.

## Measured result

- Both 60-second legacy baselines played about 30 FPS but exposed no verifiable acquisition-to-output identity. They are `INVALID_IDENTITY`, not latency baselines.
- `gstreamer + mapped-pressure` completed only 41 frames and is a terminal negative cell.
- `gstreamer + mapped-live-v1-future-on` completed 725 of 1830 produced frames. Among the exact sampled frames, capture-to-widget p95 was 1053.178 ms and p99 was 1310.454 ms.
- The same exact cell measured capture-to-decode p95 1052.030 ms and decode-to-widget p95 8.797 ms. The dominant delay is before decoder output, not GTK widget submission.
- Future-on used 14.091 mapping-plus-payload Interests per decoded frame versus 4.900 for the rollback reference. This exceeds the hard two-times work gate. Provider future-hit ratio was 88.96%, below the 99% gate.
- Physical scan-out remains unavailable; all GUI figures stop at GTK widget submission.

Canonical evidence is recorded in `baseline-report.json` and the four unique `results/spec122-*` directories named there. No failed or invalid cell was replaced or rerun.

## Implementation and compatibility

- `frameBindingVersion=1` protects source frame ID, provider steady-clock acquisition time, codec PTS/timebase, and codec configuration epoch inside the existing authenticated UAV video payload.
- Legacy version 0 packets retain their original wire encoding and AES-GCM golden vector.
- GStreamer is explicit through `NDNSF_UAV_VIDEO_PIPELINE=gstreamer`; the default remains `legacy-pipe`.
- Stream names, signed Mapping, Provider validation, AES-GCM nonce rules, and FEC wire contracts were not bypassed.

## Task decisions

- T001-T004: implemented and verified.
- T005: both frozen 60-second baselines executed once; invalid identity preserved; bottlenecks ranked from the exact candidate evidence.
- T006: no startup change retained. Future-on already reached 54 ms startup, below the 150 ms gate; the pressure cell's 270 ms was part of a continuity failure.
- T007: GStreamer pressure and future-on candidates retained as negative evidence; neither passed continuity, future-hit, or Interest-work gates.
- T008: no direct sink retained because decode-to-widget p95 was only 8.797 ms while capture-to-decode exceeded one second. The bounded queue contract remains available.
- T009: no confirmatory pair matrix was executed because no exploratory candidate was admissible. The legacy default and explicit GStreamer experimental path remain.

## Verification

- Build: `unit-tests`, `UavDroneApp`, `UavGroundStationApp`.
- Unit: `Stream` 37/37 and `UavProtocolState` 57/57.
- Python: latency/oracle, GStreamer capability, live-stream check-only loss/FEC, prefetch campaign, UAV parity campaign, and unified video tests passed.
- MiniNDN: one 10-second exact-identity integration smoke passed with 310 GUI frames; four frozen 60-second exploratory cells were then executed once.
- Post-implementation audit: task closure PASS; runtime promotion REJECTED. The audit also closed duplicate/decreasing decoder-PTS conflicts fail-closed.

## Remaining engineering problem

The next optimization must target long-run mapped Stream continuity and Interest efficiency before codec or GUI work. In particular, explain why future Interests stop keeping pace or remain pending as the producer advances, then prove bounded latest recovery without exceeding the two-times Interest-work gate. This is follow-up work, not an uncompleted Spec 122 task.
