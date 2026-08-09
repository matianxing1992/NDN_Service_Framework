# RandomWaypoint burn-in 50 m 10-seed results

## Verdict

The preregistered 50 m extension completed all 30 cells without retry. It does
not establish an NDNSF logical-success advantage over either sequential
baseline. The paired NDNSF-minus-gRPC mean is `+0.77` percentage points with a
seed-bootstrap 95% interval of `[-0.13, +1.87]`; the paired
NDNSF-minus-NSC mean is `-0.90` points with interval `[-2.17, +0.27]`.

This negative result is retained. At 50 m the mobility trace, not the client,
dominates success: mean realized any-Provider coverage is `54.90%` with sample
SD `33.61` points, and seeds 62 and 68 have zero reachable Providers for the
entire 60-second measurement window.

## Execution and integrity

- Output: `results/spec171-burnin300-50m-2ms-seeds62-71-20260808`.
- Aggregate SHA-256:
  `b42756f10b87731072f2ae385007771410a249f4f9dbfa44dc078e764d91d9b0`.
- Registration SHA-256:
  `dbc2a065504ea957848b4d4393bb536bc4f8a10da4c23acc7aff72aa58c1dc5d`.
- Completion: `30/30`; every cell is `complete`; no cell was retried.
- Every cell passed manifest, request-count, measurement-phase, and trace-source
  validation. Within each seed, NDNSF, gRPC, and NSC have the same trace hash.
- The frozen NDNSF and NDN-SVS library hashes, Experimental source identity,
  Boost 1.71 linkage, workload, timeouts, admission setting, and disabled health
  oracle match the registration.

## Seed-level success and realized coverage

All values are percentages. These ten points, rather than the 3,000 pooled
requests, are the inference observations.

| Seed | Any Provider reachable | NDNSF | gRPC-SEQ-4 | NSC-4 |
|---:|---:|---:|---:|---:|
| 62 | 0.00 | 0.00 | 0.00 | 0.00 |
| 63 | 43.33 | 42.33 | 42.33 | 45.67 |
| 64 | 67.17 | 66.00 | 61.67 | 69.67 |
| 65 | 83.00 | 82.33 | 81.00 | 83.00 |
| 66 | 88.67 | 91.00 | 87.67 | 90.00 |
| 67 | 42.33 | 41.00 | 41.67 | 42.33 |
| 68 | 0.00 | 0.00 | 0.00 | 0.00 |
| 69 | 60.83 | 59.33 | 60.00 | 63.00 |
| 70 | 70.00 | 70.00 | 70.00 | 69.67 |
| 71 | 93.67 | 93.67 | 93.67 | 91.33 |

| System | Seed mean | Median | Sample SD | Seed-bootstrap 95% CI | Pooled successes |
|---|---:|---:|---:|---:|---:|
| NDNSF | 54.57% | 62.67% | 33.84 pp | [34.17%, 73.53%] | 1,637/3,000 |
| gRPC-SEQ-4 | 53.80% | 60.83% | 33.18 pp | [33.80%, 72.43%] | 1,614/3,000 |
| NSC-4 | 55.47% | 66.33% | 33.53 pp | [34.97%, 74.10%] | 1,664/3,000 |

The deterministic bootstrap uses 20,000 seed resamples and RNG seed 171, the
same rule as the campaign's paired intervals.

## Latency and retry cost

Latency is conditional on success. Seeds 62 and 68 have no successful response
and therefore no latency distribution; they are missing, not zero-latency
observations.

| System | Pooled mean successful latency | Median seed p95 | Issued attempts/request | Failovers/request |
|---|---:|---:|---:|---:|
| NDNSF | 95.67 ms | 93.61 ms | n/a | 0.000 |
| gRPC-SEQ-4 | 24.06 ms | 30.04 ms | 3.142 | 2.142 |
| NSC-4 | 1,443.99 ms | 3,046.29 ms | 3.120 | 2.120 |

The low conditional gRPC latency is not a health-oracle result. Health probes
and health-directed selections are both zero. Across the ten cells, 7,662 of
9,426 issued gRPC attempts terminate as native `UNAVAILABLE`, while only 150
reach the one-second `DEADLINE_EXCEEDED` boundary. Once a gRPC channel has
observed the harness's provider-side `iptables DROP`, its transport connectivity
state can reject later RPCs quickly, so the explicit sequential client reaches a
currently usable preregistered endpoint without paying one second on every
attempt. This behavior is part of the measured gRPC implementation and must not
be described as an external health oracle.

## Interpretation with the 100/150 m controls

- At 50 m, large trace variation and long zero-coverage periods dominate; no
  paired success advantage is established and gRPC has lower conditional
  latency.
- At 100 m, NDNSF and gRPC both average `97.97%` success, but NDNSF has lower
  median seed-p95 latency (`90.55 ms` versus `524.15 ms`). This is the bounded
  positive result: lower retry-induced tail latency under partial coverage.
- At 150 m, all systems reach `100%`; gRPC has lower latency. This control rules
  out a universal NDNSF latency claim.

The final claim is therefore conditional: runtime multi-Provider discovery and
selection can reduce sequential-retry tail latency in the 100 m partial-coverage
regime without requiring a preregistered endpoint list. The current mobility
matrix does not prove higher success than a four-target sequential gRPC client.
