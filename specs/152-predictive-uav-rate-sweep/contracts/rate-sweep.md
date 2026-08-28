# UAV Predictive Rate-sweep Contract

## Invariants

```text
publisher API = start -> push(signed Data)* -> flush -> stop
consumer API = PredictiveStreamSubscriber
discovery mode = sequential payload-first predictive
mapping Interests = 0
topology/config/binaries identical across cells except configured FPS
```

## Rate Accounting

```text
requestedFps = --video-fps
measuredFrameCount =
  exact provider frame-origin events inside the measurement window
achievedFps = measuredFrameCount / measuredSeconds
rateError = abs(achievedFps - requestedFps) / requestedFps
```

`STREAM_PUSH` counts signed Data segments, not frames. It is reported as
segment PPS and is never used as achieved FPS.

## Acceptance

```text
measurement >= 60 s
rateError <= 0.05
deliveryRatio >= 0.98
futureHitRatio >= 0.95
mappingInterests == 0
readyQueueDepth == 0
terminalGapQueueDepth == 0
exact wire identity
decoded video
complete mean/p50/p95/p99
end-to-end p99 <= 1000 ms
longest delivery gap <= 1000 ms
```

Formal campaign roots are immutable after preparation and execution begins.
No automatic retry and no selective cell replacement are permitted.
