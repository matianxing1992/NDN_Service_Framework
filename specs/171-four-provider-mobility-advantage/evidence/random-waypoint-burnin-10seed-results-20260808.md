# RandomWaypoint burn-in 10-seed result

## Verdict

`NO_DEMONSTRATED_SUCCESS_RATE_ADVANTAGE` under SC-005a. The 100 m condition
does, however, show a reproducible retry/tail-latency advantage over sequential
gRPC and NSC while matching gRPC's logical success exactly. This result does
not satisfy the stronger `single-active-handoff` claim in SC-005.

## Completion and integrity

- Output: `results/spec171-burnin300-100-150m-2ms-seeds62-71-20260808`.
- All `60/60` cells completed without an automatic rerun.
- All ten registered seeds `62--71` completed for both radii and all three
  systems.
- Each of the 20 seed/radius groups has exactly one trace hash shared by NDNSF,
  gRPC, and NSC.
- Every NDNSF cell used framework SHA-256
  `23c1018b55b70b070111ae25dbc819cd85092029f2fcbb4af0e4d8dd624b7c99`
  and NDN-SVS SHA-256
  `b16760781518854e4bfe29987b06eda82952c67936f3be3efe23053d8a1f2990`.
- The retained result tree is approximately `30.6 MiB`.

## Aggregate results

Latency columns report the pooled mean successful-response latency and the
median of the ten per-seed p95/p99 values. Attempt/failover cost is normalized
by all requests.

| Radius | System | Success | Mean (ms) | Median seed p95 (ms) | Median seed p99 (ms) | Attempts/request | Failovers/request |
|---:|---|---:|---:|---:|---:|---:|---:|
| 100 m | NDNSF | 2,939/3,000 (97.97%) | 72.55 | 90.55 | 101.68 | n/a | n/a |
| 100 m | gRPC-SEQ-4 | 2,939/3,000 (97.97%) | 80.33 | 524.15 | 871.88 | 2.068 | 1.068 |
| 100 m | NSC-4 | 2,950/3,000 (98.33%) | 1,039.44 | 3,027.76 | 3,039.43 | 2.045 | 1.045 |
| 150 m | NDNSF | 3,000/3,000 (100%) | 100.28 | 229.08 | 352.55 | n/a | n/a |
| 150 m | gRPC-SEQ-4 | 3,000/3,000 (100%) | 80.25 | 31.55 | 1,022.58 | 1.449 | 0.449 |
| 150 m | NSC-4 | 3,000/3,000 (100%) | 465.69 | 1,037.17 | 1,537.80 | 1.440 | 0.440 |

At 100 m, NDNSF matched gRPC success on every seed. Relative to gRPC, NDNSF
reduced the pooled successful-response mean by about 9.7%, the median seed p95
by about 82.7% (`5.79x`), and the median seed p99 by about 88.3% (`8.57x`).
NSC completed eleven additional requests on the one trace with a long
all-unreachable interval, but paid roughly one second of mean latency and over
three seconds at p95/p99.

The 150 m control prevents an overbroad claim. With high redundant coverage,
gRPC had lower mean and median seed p95 than NDNSF. NDNSF retained a lower
median seed p99, but its ACK/selection path is not universally faster than a
successful first static gRPC target.

## Seed-level success inference

The seed, not the individual request, is the inference unit.

| Radius | Paired comparison | Mean success difference | 95% paired bootstrap interval |
|---:|---|---:|---:|
| 100 m | NDNSF - gRPC-SEQ-4 | +0.00 pp | [+0.00, +0.00] pp |
| 100 m | NDNSF - NSC-4 | -0.37 pp | [-1.10, +0.00] pp |
| 150 m | NDNSF - gRPC-SEQ-4 | +0.00 pp | [+0.00, +0.00] pp |
| 150 m | NDNSF - NSC-4 | +0.00 pp | [+0.00, +0.00] pp |

The lower bounds are not positive against both baselines, so SC-005a fails and
no success-rate advantage claim is permitted.

## Realized coverage

| Radius | Mean any-Provider coverage | Minimum seed any-Provider coverage | Mean two-or-more coverage | Mean all-unreachable | Maximum seed all-unreachable |
|---:|---:|---:|---:|---:|---:|
| 100 m | 98.07% | 80.67% | 56.08% | 1.93% | 19.33% |
| 150 m | 100.00% | 100.00% | 94.72% | 0.00% | 0.00% |

Across all 6,000 measurement epochs per radius, the mean reachable-Provider
count was 1.592 at 100 m and 2.672 at 150 m. The per-Provider in-range
fractions are retained in each `trace-info.json`; their ten-seed means at
100 m were 59.18% (ucla), 43.65% (wustl), 10.97% (uiuc), and 45.37%
(arizona), confirming that the result was not produced by four equivalent
always-reachable paths.

Seed 68 contains the 100 m disconnection interval: NDNSF and gRPC each
completed 239/300 requests, while NSC completed 250/300. The other nine 100 m
seeds completed 300/300 for all systems. This heterogeneity explains why a
single seed was not sufficient evidence.

## Mechanism boundary

NDNSF Response-timeout reselection was enabled but recorded zero reselections
in this slow-mobility matrix. Responses normally completed before coverage
changed after selection. The measured 100 m latency benefit therefore comes
from runtime ACK-based FirstResponding selection among currently reachable
Providers, not from Response-level retry. gRPC and NSC were given four static
targets/prefixes and incurred sequential timeout/failover cost; NDNSF did not
pre-register a Provider endpoint list.

The defensible paper statement is therefore: under the tested one-AP partial-
coverage condition, NDNSF preserved the same logical success as sequential
gRPC while substantially reducing tail latency and retry delay; the advantage
disappeared for typical latency when redundant coverage was high.
