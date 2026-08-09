# RandomWaypoint Burn-in Pilot (2026-08-09)

## Status

Mechanism pilot passed. This is one paired seed and does not authorize a
publication claim or slide update.

## Why the earlier 60-second trace was not representative

`RandomWaypointCoverage` initializes every Provider 50 m from the AP. At the
registered 2 m/s speed, a 60-second run can move a Provider by at most 120 m
before accounting for direction changes. In the no-burn-in seed-61 trace, the
100 m condition therefore kept at least two Providers reachable for the entire
measurement window and kept all four reachable for 85% of the window. That
warm-start bias explained the unexpectedly high success rates.

The harness now accepts `--mobility-warmup-s`. It advances RandomWaypoint
deterministically before trace timestamp zero without adding wall-clock delay,
passes the value to every child command, and records it in the campaign
manifest and trace metadata. Explicit NDNSF campaign cells also replay the
same parent trace as gRPC and NSC. Future campaigns additionally record the
measurement-window reachable-Provider distribution in trace metadata.

## Registered pilot configuration

- One AP at `(200, 200)` in a 400 m by 400 m field
- Four Providers; fixed speed 2 m/s; seed 61
- 300 s deterministic mobility burn-in; 60 s measured window
- Coverage radii 100 m and 150 m
- NDNSF FirstResponding with bounded Response retry enabled
- `gRPC-SEQ-4` and NSC-4 with four preconfigured Provider targets, sequential
  1 s attempts, no health oracle, and a common 5 s deadline
- 5 RPS, 5 ms service delay, `block_network=true`, admission disabled
- Experimental NDN-SVS library SHA-256
  `b16760781518854e4bfe29987b06eda82952c67936f3be3efe23053d8a1f2990`
- NDNSF framework library SHA-256
  `23c1018b55b70b070111ae25dbc819cd85092029f2fcbb4af0e4d8dd624b7c99`

Campaign:
`results/spec171-burnin300-paired-100-150m-2ms-seed61-20260809`

## Realized measurement-window coverage

| Radius | Reachable-Provider epochs (0/1/2/3/4) | Any reachable | Mean reachable | Per-Provider reachable fraction (A/B/C/D) |
|---|---:|---:|---:|---:|
| 100 m | 1 / 1 / 58 / 0 / 0 | 98.33% | 1.95 | 98.33% / 0% / 0% / 96.67% |
| 150 m | 0 / 0 / 5 / 28 / 27 | 100% | 3.37 | 100% / 91.67% / 45% / 100% |

The trace SHA-256 values are
`2ccc272d36d15b3ad2723b9ea64c344be263840dfcde72066f839c73f23c4ea1`
for 100 m and
`cad10fd8d196ea7f527e254aa374c1fea62e24ae87d155cd11f05f9325d903f8`
for 150 m. All six cell receipts bind to these parent traces.

## Results

| Radius | System | Success | Mean | p50 | p95 | p99 | Attempts / executions |
|---|---|---:|---:|---:|---:|---:|---:|
| 100 m | NDNSF | 300/300 | 125.39 ms | 69.96 ms | 96.81 ms | 2035.14 ms | 300 executions |
| 100 m | gRPC-SEQ-4 | 297/300 | 103.21 ms | 13.51 ms | 1017.41 ms | 1034.17 ms | 538 attempts, 238 failovers |
| 100 m | NSC-4 | 300/300 | 816.99 ms | 1015.87 ms | 2036.90 ms | 2050.85 ms | 537 attempts, 237 failovers |
| 150 m | NDNSF | 300/300 | 131.74 ms | 95.36 ms | 350.60 ms | 502.44 ms | 300 executions |
| 150 m | gRPC-SEQ-4 | 300/300 | 76.11 ms | 14.26 ms | 1013.19 ms | 1019.83 ms | 353 attempts, 53 failovers |
| 150 m | NSC-4 | 300/300 | 202.32 ms | 27.35 ms | 1034.63 ms | 2030.39 ms | 353 attempts, 53 failovers |

At 100 m, NDNSF improved success by 1 percentage point over gRPC and matched
NSC success. Its p95 was 10.5 times lower than gRPC and 21.0 times lower than
NSC. Its mean remained 22.19 ms above gRPC because gRPC's successful
first-attempt fast path is much cheaper, while only a subset of gRPC requests
paid retry cost. At 150 m, all systems reached 100% success; gRPC retained the
best mean, while NDNSF retained a 2.9-times lower p95 than both sequential
baselines.

## Decision

The pilot supports the intended mechanism: when a configured endpoint list
contains currently unreachable Providers, NDNSF can select among runtime ACKs
without paying one-second serial endpoint timeouts. It does not yet establish
a general advantage because this is one seed and the 100 m trace leaves two
Providers unreachable for the entire window. The next evidence step is a
pre-registered 10-seed, trace-paired 100 m condition with the same 300 s
burn-in, followed by independent-process repeats for selected seeds. The
150 m condition remains a useful higher-coverage control.

The earlier no-burn-in campaign and the first trace-mismatched campaign are
diagnostic only and must not be pooled with this pilot.

## Verification

- `python3 tests/python/test_mobility_harness_contract.py`: 11/11 passed
- `python3 tests/python/test_four_provider_mobility_profile.py`: 12/12 passed
- `python3 tests/python/test_spec171_mobility_comparison_analysis.py`: 2/2 passed
- Campaign status: `passed`; six of six terminal cell receipts accepted
