# Spec 176 compensation and command-reconciliation evidence

**Candidate**: `UAV-Experimental` working tree, 2026-08-28

## Focused regressions

```text
./build/unit-tests --run_test=UavTwoLifecycle/CompensationPreservesCompletedWorkAndCommandTimeoutNeedsReconciliation --log_level=message
  1/1 case passed, process return code 0

./build/integration-tests --run_test=UavCollaborationFlow/CompensationAndCommandTimeoutRequireAuthoritativeRecovery --log_level=message
  1/1 case passed, process return code 0
```

The unit and integration fixtures create two mission parts, complete one with
an immutable response digest, mark the other missing, and enter the explicit
`DEGRADED -> COMPENSATING -> ACTIVE` path. Compensation changes only the
missing part to `COMPENSATED`; the completed part keeps its response digest and
waypoint progress.

The same fixtures create a timed-out `FlightCommandState`. The command remains
`accepted=false` with `ack=timeout`; `UavStabilityState` reports
`operator-decision`, and the safety state records no manual replay. The
MissionSession then enters `RECOVERING` and returns to `ACTIVE` only through
`reconcileVehicleAndStreams()`.

## Boundary

This is a deterministic application-state and Ground Station contract
regression. It proves that a finite request or physical-command timeout cannot
erase completed patrol work or authorize blind replay. It does not claim that
the fixture supplies authoritative PX4 telemetry; that evidence remains the
T020 SITL gate. The Ground Station command callbacks already publish timeout
state and operator-decision status, while patrol compensation emits only
missing-part retry objects; the new tests make those safety boundaries
machine-checkable without adding a Core wire type.

## Pressure isolation

```text
./build/unit-tests --run_test=UavTwoLifecycle/ResourcePressureDoesNotBlockCommandOrJobLifecycle --log_level=message
  1/1 case passed, process return code 0
```

The regression pushes 1,024 detector jobs into a four-item queue. The queue
remains bounded at four items and reports 1,020 drops; while it is overloaded,
the independent MissionSession still advances a collaboration job to its
explicit timeout and records a timed-out flight command without changing the
mission state or authorizing replay. This is a deterministic bounded-pressure
check; multi-threaded scheduling and hardware throughput remain outside the
CPU gate.
