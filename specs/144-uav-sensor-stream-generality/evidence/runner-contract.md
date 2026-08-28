# T003 One-Shot MiniNDN Runner Contract

**Date**: 2026-07-24  
**Verdict**: PASS / FORMAL NOT STARTED

## Frozen Structure

- topology: two MiniNDN nodes, `provider` and `consumer`;
- application: one real C++ UAV-APP executable,
  `build/examples/UavSensorStreamNode`, run once per node with provider and
  consumer roles;
- warm-up/measured windows: explicit command arguments `5 s` and `60 s`;
- cells: exactly 32 unique IDs: two workloads times one zero-loss plus five
  repetitions for each of loss, reorder, and combined;
- retry policy: `automaticRetry=false`, `rerunAllowed=false`;
- destinations: both campaign and cell runners reject non-empty paths.

## Single Ownership and Freeze

The matrix takes one global nonblocking `flock` at
`/tmp/ndnsf-spec144-uav-sensor-stream.lock`, so a second campaign cannot run
under a different output root. It also records a campaign-local owner receipt.
Every cell is marked invoked before launch, becomes immutable terminal
evidence after launch, and is never retried. Before every cell, the runner
rehashes Core, binding, UAV-APP, runner, analyzer, contract, and binary inputs.

The single-cell runner records NFD readiness, commands, launcher and child
PIDs, cleanup ownership, endpoint routes, effective qdisc before applications
and at teardown, process return codes, raw application records, and terminal
status. Effective `tc -j` values must exactly match the requested delay,
jitter, loss, reorder, correlation, and gap; the presence of an arbitrary
`netem` qdisc is insufficient.

## Verification

`tests/python/test_spec144_uav_sensor_stream_runner.py` passes 8/8 cases:

- exact 32-cell layout and unique IDs;
- exact bidirectional fault-profile commands and qdisc mismatch rejection;
- real C++ two-role UAV-APP and explicit 5/60-second windows;
- reused destination rejection;
- duplicate ID, second invocation, retry, and mutation rejection;
- frozen-input drift rejection;
- runtime/security binary, configuration, and environment identity coverage;
- global cross-output campaign ownership rejection.

The two final zero-loss preflights additionally verify real MiniNDN readiness,
qdisc installation, process ownership, expected counts, analysis gates, and
cleanup. They are diagnostics only and never enter the formal denominator.
