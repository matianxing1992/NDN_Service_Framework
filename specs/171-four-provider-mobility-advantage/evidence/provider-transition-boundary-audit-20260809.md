# Provider-transition boundary audit

## Trigger

Replay 1 originally reported 120/120 successful post-retirement gRPC requests
but only 119 Provider-D successes. The frozen run and raw logs were retained;
no cell was rerun or replaced.

## Reproduction

The sole non-D success was `request_id=180`:

```text
GRPC_FAILOVER_ATTEMPT request_id=180 attempt=1 provider=ucla status=OK latency_ms=9.091 server_provider=ucla
```

The client began measurement at monotonic time `285153.620999`. At 5 RPS,
request 180 was nominally scheduled at `285189.620999`. The trace-40.0 UCLA
network gate was actually applied at `285189.635032`, 14.033 ms later. The RPC
completed in 9.091 ms, before the gate was installed. Request 181 and every
subsequent successful request used Provider D.

## Root cause

The analyzer assigned phases from the ideal request index. A trace transition
updates four independent iptables gates sequentially, so a request scheduled at
the exact epoch can observe an intermediate state. This was an analysis-window
classification error, not a Provider-selection or network-gate failure.

## Correction

- Frozen logs retain requests at trace 20.0 and 40.0 in a separate
  `transition_boundary` phase. Neither request is credited to an adjacent
  steady-state claim.
- The steady post-retirement result is therefore 357 requests across three
  replays: NDNSF 357/357 through D, gRPC static-3 0/357, gRPC
  pre-registered-4 357/357 through D, NSC static-3 0/357, and NSC
  pre-registered-4 357/357 through D.
- New NDNSF, gRPC, and NSC client logs record each request's actual monotonic
  publication time. Future analysis classifies requests from the actual applied
  gate state and uses the boundary phase only while a transition is incomplete.
