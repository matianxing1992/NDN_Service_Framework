# Pre-implementation Audit

**Verdict: PASS**

## Intent and Necessity

The feature directly tests the requested `StreamPublisher` and
`PredictiveStreamSubscriber` path at multiple rates. Existing frozen evidence
is insufficient because it fixes 30 fps and its file-source capture was not
wall-clock paced. A new Spec and result root are necessary; changing old
evidence is prohibited.

## Source and Ownership

CodeGraph and current source show:

- CLI accepts 1–60 fps and propagates it into UAV capture and stream
  `samplePeriodMs`;
- UAV GStreamer capture uses `videorate` and requested FPS caps;
- capture `appsink sync=false` permits non-live file input to run faster than
  its timestamps;
- Drone uses high-level `StreamPublisher`; Ground Station uses
  `PredictiveStreamSubscriber`;
- the pacing fix belongs to UAV `UavVideoPipeline.cpp`, not generic Core.

## Occam and Architecture

One sink-clock correction is sufficient. No new scheduler, API, wire field,
prefetch controller, retry policy, or workload branch is justified. Decode
remains unsynchronized so network arrivals are processed immediately.

## Security, Migration, Rollback

No names, signatures, encryption, permissions, wire data, or persisted state
change. Rollback is one UAV pipeline property change. Existing callback failure
containment and exact frame identity remain under focused tests.

## Experiment Readiness

The six-cell matrix changes only FPS, uses two real MiniNDN nodes, 5-second
warm-up and >=60-second measurement, freezes inputs, disallows retries, and
reports achieved FPS independently from segment PPS. Short diagnostic roots
may be repaired before formal freeze; formal results are immutable.

## Blocking Issues

None before T002. Formal execution remains blocked until focused tests,
build-Core linkage, analyzer tests, and low/high diagnostic gates pass.
