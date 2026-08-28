# Formal Matrix Contract

## Fixed configuration

```text
rates: 10, 20, 30, 40, 50, 60 fps
topology: two-node Memphis <-> UCLA MiniNDN UAV experiment
loss/reorder: 0 / 0
warm-up: 5 s
measured window: >=60 s
bitrate: 8 Mbps
width: 480
FEC repair items: 1
provider API: StreamPublisher start -> push* -> flush -> stop
consumer API: PredictiveStreamSubscriber
discovery: sequential payload-first predictive
Mapping Interests: 0
```

## Per-cell acceptance

```text
abs(achievedFps-requestedFps)/requestedFps <= 5%
deliveryRatio >= 98%
futureHitRatio >= 95%
retryAttempts / max(1, payloadInterests) <= 2%
timeouts / max(1, payloadInterests) <= 2%
Mapping Interests == 0
readyQueueDepth == 0
terminalGapQueueDepth == 0
exact wire identity and valid decoding
complete AoI/end-to-end mean/p50/p95/p99
end-to-end p99 <= 1000 ms
longest delivery gap <= 1000 ms
provider and consumer API lifecycle markers present
normal process exit and terminal status marker present
```

The runner prepares one unique root, freezes hashes, disables automatic retry,
and executes every cell in order. Once execution starts, no source, binary,
configuration, command, threshold, or completed cell may change.
