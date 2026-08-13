# Mobility comparison analysis

- Condition: `range-50-speed-2p0`
- Claim verdict: `NO_HOLDOUT_CONFIRMATION`
- Aggregate SHA-256: `d9f40684ab2ed31d321cbcdb17b25e41a20f617fb2b0a21b63d5024195b31d2d`

| System | Success | Mean successful latency (ms) | Mean p95 (ms) | Attempts/executions per request |
|---|---:|---:|---:|---:|
| gRPC-1 (no failover) | 47.87% | 8.16 | 9.27 | 1.000 |
| gRPC-SEQ-4 | 79.73% | 140.12 | 881.72 | 2.113 |
| NDNSF | 77.27% | 112.50 | 500.05 | 0.773 |

Successful-response latency is conditional on a response; success rate and deadline misses are reported separately.

## Seed-level paired success

The 300 requests within a seed share one mobility/coverage trace; they are not treated as independent seed replicates.

| Seed | NDNSF | gRPC-SEQ-4 | NDNSF minus gRPC (pp) |
|---:|---:|---:|---:|
| 43 | 98.33% | 91.00% | +7.33 |
| 44 | 61.67% | 61.67% | +0.00 |
| 45 | 76.67% | 76.67% | +0.00 |
| 46 | 76.67% | 96.33% | -19.67 |
| 47 | 73.00% | 73.00% | +0.00 |

## Paired gates

| Gate | Result |
|---|---|
| `control_success_lower_positive` | `False` |
| `sequential_success_noninferior` | `False` |
| `mean_latency_ratio_upper_below_one` | `False` |
| `p95_latency_ratio_upper_below_one` | `False` |
| `confirmed_conditional_advantage` | `False` |

## Paired bootstrap intervals

| Baseline | Success difference mean [95% interval] | Mean latency ratio mean [95% interval] | p95 ratio mean [95% interval] |
|---|---:|---:|---:|
| gRPC-1 (no failover) | 0.2940 [-0.0533, 0.7140] | 7.2031 [6.1073, 7.9995] | 7.4730 [6.5162, 8.4487] |
| gRPC-SEQ-4 | -0.0247 [-0.1180, 0.0440] | 0.8785 [0.4705, 1.2864] | 1.7121 [0.0688, 4.1111] |
