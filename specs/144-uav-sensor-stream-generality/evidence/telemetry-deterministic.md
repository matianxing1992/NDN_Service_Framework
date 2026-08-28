# T004 Telemetry Deterministic Evidence

**Date**: 2026-07-24  
**Verdict**: PASS

UAV-APP owns `CompactTelemetrySample` serialization and
`LatestTelemetryAdmission`; Core sees one opaque source item in the generic
`compact-state` class. The provider uses `announceSample`,
`prepareSampleExtent`, and `publishSample`. The consumer opens at
`LiveStreamStart::Latest` with `AdaptiveSampleAtomic`; the APP does not issue
Interests or choose the runtime fetch window.

Deterministic native cases prove:

- exact 256/384/512-byte encoded-size cycle;
- identity, timestamp, position/motion, battery, readiness, and link fields;
- deterministic authenticated padding and malformed-padding rejection;
- wrong UAV identity rejection;
- duplicate and out-of-order observation without latest-state regression;
- exactly one source item, Mapping v2, 50 ms period, and no FEC.

The formal provider/consumer integration is the dedicated real UAV-APP
`UavSensorStreamNode`, using deployed UAV identities
`/example/uav/drone/A` and `/example/uav/gs`, the UAV controller/policy, and
the generic ServiceProvider/ServiceUser stream APIs. The existing Drone and
Ground Station `GetStatus` request/response implementation was not modified
and remains the complete snapshot/fallback path.

Verification:

- `UavProtocolState`: 70/70 PASS after the final boundary fix;
- Python metric/neutrality suite: 8/8 PASS;
- UAV stream security contract: PASS;
- final telemetry zero-loss preflight: to be frozen in
  `preflight-and-freeze.md`.
