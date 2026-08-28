# T004 Actual Core Fetch Status

## Red

Command:

```bash
./waf build --targets=unit-tests -j2
```

The new
`VideoCoreFetchDecisionSnapshotReportsOnlyCurrentCoreState` test failed to
compile because the UAV APP had no Core decision snapshot or explicit
availability/source fields. `currentVideoAdaptiveState()` instead copied
window, lookahead, Interest lifetime, and missing timeout from the APP bitrate
policy.

## Implementation

- The active `LiveStreamStatus.fetchDecision` is copied into an APP-owned
  snapshot under `m_liveStreamMutex`.
- Every consumer replacement or stop advances a generation and clears the
  snapshot.
- A callback is accepted only when its captured generation equals both the
  active generation and snapshot generation.
- `VideoAdaptiveState.window`, `lookahead`, `interestLifetimeMs`, and
  `missingTimeoutMs` now come only from that Core snapshot.
- When Core has not supplied a decision, those values are zero and the state
  explicitly reports `core_fetch_decision_available=false` and
  `core_fetch_decision_source=unavailable`.
- APP-owned bitrate, pressure, backlog, reorder, and `futureProbeLimit` remain
  separate; `future_probe_limit_source=uav-app-policy` makes that ownership
  explicit.

## Green

Commands:

```bash
./waf build --targets=unit-tests,UavGroundStationApp -j2
./build/unit-tests \
  --run_test=UavProtocolState/VideoCoreFetchDecisionSnapshotReportsOnlyCurrentCoreState \
  --log_level=test_suite
./build/unit-tests \
  --run_test=UavProtocolState/VideoAdaptiveStateRoundTripsAndReportsPressure \
  --log_level=test_suite
```

Result: PASS. The tests cover unavailable state, exact active Core values,
missing decision, stale retired generation rejection, replacement reset, and
complete field round-trip. The bitrate decision function and its inputs were
not changed.
