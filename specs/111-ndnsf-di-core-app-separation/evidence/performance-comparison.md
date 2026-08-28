# Performance Comparison

Date: 2026-07-14  
Verdict: **PASS — paired correctness and 5% non-regression gates satisfied**

Source: `performance-analysis.json`  
Analysis: 10,000 deterministic paired-bootstrap repetitions, seed `11195`.
Relative latency is positive when treatment is slower; relative throughput is
positive when treatment is faster.

| Measure | Baseline median | Treatment median | Median paired change | 95% paired bootstrap |
|---|---:|---:|---:|---:|
| completed measured requests | 600/600 | 600/600 | 0 failures | correctness PASS |
| p50 latency | 117.315 ms | 116.832 ms | +0.10% | −8.04% to +4.51% |
| p95 latency | 141.178 ms | 137.951 ms | +1.04% | −7.30% to +4.01% |
| throughput | 1.000 req/s | 1.000 req/s | 0.00% | 0.00% to 0.00% |
| process-tree peak RSS | 858158 KiB | 709370 KiB | −17.29% | −17.43% to −17.18% |
| queue evidence | 0 samples | 0 samples | unavailable | not an acceptance input |

Both latency upper bounds remain below the declared +5% degradation margin,
and the throughput lower bound remains above −5%. RSS is descriptive, not an
acceptance metric, but consistently decreased for the treatment. No failed
cell was imputed and no optional stopping or replacement run was used.
