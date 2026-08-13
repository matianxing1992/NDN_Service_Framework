# One-AP Range/Speed Screening Result: 50 m, 2 m/s

## Registration

- Campaign output: `/tmp/ndnsf-single-ap-screen-50m-2ms-20260806`
- Registration SHA-256: `c031366c9e8d2f28c7b6ffe13386920aaca4d53caf06c56c6f59a6e470c24185`
- Topology: one physical AP, four Providers, `block_network=true`
- Workload: 5 RPS, 60 s, 300 requests per cell, 5 s logical deadline
- Timeouts: NDNSF ACK 1 s; gRPC/NSC attempt 1 s
- gRPC condition: `gRPC-SEQ-4`, health routing disabled, zero health checks
- Seeds: 40, 41, 42; all 9 cells completed with matching manifests and trace
  hashes

## Results

| Seed | NDNSF | gRPC-SEQ-4 | NSC-4 | NDNSF−gRPC | NDNSF−NSC |
|---:|---:|---:|---:|---:|---:|
| 40 | 197/300 (65.67%) | 197/300 (65.67%) | 191/300 (63.67%) | 0.00 pp | +2.00 pp |
| 41 | 229/300 (76.33%) | 228/300 (76.00%) | 224/300 (74.67%) | +0.33 pp | +1.67 pp |
| 42 | 222/300 (74.00%) | 220/300 (73.33%) | 216/300 (72.00%) | +0.67 pp | +2.00 pp |
| **Aggregate** | **648/900 (72.00%)** | **645/900 (71.67%)** | **631/900 (70.11%)** | **+0.33 pp** | **+1.89 pp** |

The registered bootstrap intervals over the three paired seeds were `[0,
0.67]` percentage points for NDNSF−gRPC and `[1.67, 2.00]` percentage points
for NDNSF−NSC. The sample is only a three-seed screening condition, so these
intervals are descriptive and do not establish a paper-level mobility claim.

Control-cost observations:

- gRPC attempts: 903, 654, 662; failovers: 603, 354, 362; health checks: 0.
- NSC attempts: 907, 660, 667; failovers: 607, 360, 367.
- NDNSF attempts equal successful terminal executions in the summaries; the
  current NDNSF summary does not emit comparable p50/p95 latency fields.
- gRPC p50 was 7.55--8.18 ms, but p95 was about 1.01 s because of coverage
  outages. NSC p95 ranged from 1.01--3.02 s. These latency values are not a
  complete three-system comparison because NDNSF latency was not emitted.

## Trace qualification

| Seed | Trace SHA-256 | At least one reachable | All unreachable | At least two reachable |
|---:|---|---:|---:|---:|
| 40 | `fcbe67b6cb260378dc7ee703e612c24adda42e0c43db21b5b02ddbafb28aff1b` | 60.90% | 39.10% | 0.14% |
| 41 | `4f4a2c1a23b4e89c400b910c9480ed12e86d9dfb0f808edc338b34462ad89c55` | 70.04% | 29.96% | 69.76% |
| 42 | `95e3cbe635687d6623bc9e54c7850c95811586613b8b4059fffc7c57d87d8b87` | 67.93% | 32.07% | 63.01% |

The 50 m condition mixes substantial all-unreachable time with redundant
coverage (especially seeds 41 and 42). It therefore tests availability and
retry cost together, not an isolated multi-Provider coordination mechanism.

## Verdict

`DESCRIPTIVE_RANGE_SPEED_MATRIX_ONLY`. This condition is consistent with a
small NDNSF success-rate improvement over sequential baselines, but it does not
demonstrate a general mobility advantage. Do not update paper or slides from
this screening result alone.
