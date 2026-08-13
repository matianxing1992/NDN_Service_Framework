# RandomWaypoint burn-in 100 m latency-mechanism audit

## Question and method

This audit explains the registered 100 m result without adding a new
MiniNDN run or changing the frozen matrix. The analyzer reconstructs every
successful gRPC logical-request latency by grouping its sequential
`GRPC_FAILOVER_ATTEMPT` records and summing attempt durations. The reconstructed
nearest-rank p95 is checked against each frozen cell summary before any
mechanism conclusion is emitted.

- Analyzer: `Experiments/analyze_spec171_latency_mechanism.py`.
- Machine-readable result:
  `results/spec171-burnin300-100-150m-2ms-seeds62-71-20260808/latency-mechanism.json`.
- Result SHA-256:
  `59933f5466ce0b8869033aa43e0aae1aa44f0915693a9ceffbb9810e00295628`.
- Maximum reconstructed-versus-reported seed-p95 error: `0.818 ms`.

## gRPC mechanism

The 100 m gRPC result is bimodal rather than centered around 524 ms.

| Seed group | Seeds | Reported seed p95 |
|---|---|---:|
| Fast-tail | 66, 67, 68, 70, 71 | 24.54--31.16 ms |
| One-second tail | 62, 63, 64, 65, 69 | 1,017.14--1,023.44 ms |

The reported median seed-p95, `524.153 ms`, is the arithmetic midpoint of the
fifth and sixth ordered seed values (`31.162` and `1,017.144 ms`). No seed has a
p95 near 524 ms, so the median must not be described as a typical request.

Across all ten seeds:

- 6,205 application RPC attempts contain 2,939 `OK`, 3,102 `UNAVAILABLE`, and
  164 `DEADLINE_EXCEEDED` terminal attempt statuses.
- Of 2,939 successful logical requests, 2,733 complete below 100 ms, 45 between
  100 and 900 ms, and 161 at or above 900 ms.
- 157 successful requests first incur at least one one-second deadline; these
  requests create the approximately one-second p95 cluster.
- 1,728 successful requests first encounter at least one fast `UNAVAILABLE`.
  This is native gRPC channel connectivity state, not the disabled health
  oracle; health probes and health-directed selections remain zero.

Thus gRPC usually fails over quickly after its channels learn an endpoint is
unavailable, but several mobility traces still produce enough successful
requests that pay a full first-attempt deadline to move p95 to about one second.

## NDNSF mechanism

NDNSF's ten seed-p95 values remain between `83.33` and `103.05 ms`, with median
`90.55 ms`. Seed 68 has 239 successes and 61 no-coverage deadline failures;
every other seed completes all 300 requests. Provider executions equal
successful logical requests, and Response reselections are zero.

The current `ServiceUser` implementation records the first matching ACK, stores
the authenticated candidate, and, for ordinary FirstResponding calls, invokes
ACK selection immediately on the first positive ACK. If that ACK is selected,
it publishes Selection on the fast path rather than waiting for the one-second
ACK deadline (`ServiceUser.cpp`, `handleRequestAckByName`). The frozen metrics
are consistent with that path: successful requests do not accumulate a
one-second sequential-attempt tail.

This audit does not claim that Response reselection caused the 100 m advantage;
it was enabled but not exercised. The supported mechanism is first-positive-ACK
selection versus explicit sequential RPC attempts to four preregistered IPs.

## Paper-safe statement

At 100 m, NDNSF and gRPC both average `97.97%` logical success. gRPC seed-p95 is
bimodal: five seeds are below 32 ms and five are approximately 1.02 s because a
minority of successful requests first exhaust a one-second attempt. NDNSF stays
between 83 and 103 ms across all ten seeds. Therefore the evidence supports a
conditional tail-stability result under partial coverage, not universal lower
latency and not a higher-success claim.
