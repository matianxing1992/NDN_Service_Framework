# Seed/repeat mobility follow-up

- Condition: `range-50-speed-2p0`
- Primary seeds: `50,51,52,53,54,55,56,57,58,59`
- Process-repeat seeds: `50,54,58`
- Claim verdict: `NO_POSITIVE_MOBILITY_CONFIRMATION`

The primary unit is one mobility seed; the 300 requests within a seed share one trace.
Process repeats use the same trace hash and are diagnostics, not extra independent seeds.

| System | Success | Mean successful latency (ms) |
|---|---:|---:|
| NDNSF | 2235/3000 (74.50%) | 81.59 |
| gRPC-SEQ-4 | 2223/3000 (74.10%) | 187.70 |
| NSC-4 | 2174/3000 (72.47%) | 887.64 |

| Paired comparison | Mean difference | 95% bootstrap interval |
|---|---:|---:|
| NDNSF minus gRPC-SEQ-4 | +0.40 pp | [-0.03, +0.97] pp |
| NDNSF minus NSC-4 | +2.03 pp | [+1.50, +2.43] pp |

The gRPC lower bound is not positive, so the follow-up does not prove a positive NDNSF mobility advantage.
The NSC difference is positive but far below the registered 10 percentage-point superiority threshold.
All nine process repeats reproduced the primary success count for the same seed/system; remaining repeat variation is latency-only.

Combining the prior seeds 43,44,45,46,47 with the ten new seeds gives 15 paired traces:
NDNSF minus gRPC-SEQ-4 = -0.56 pp [-3.73, +1.62] pp.
This combined interval also includes zero and remains non-superior.
