# 300 s range/speed/timeout matrix (seed 40)

## Scope and provenance

This is a descriptive MiniNDN-WiFi matrix, not a significance test. Every cell uses one fixed mobility seed (`40`), a 300 s measurement window, 5 RPS (1,500 logical requests), one AP, four Providers, `block_network=true`, disabled gRPC health routing/oracle, admission control disabled, a 5 s global deadline, and NDNSF `FirstResponding`. The attempt and ACK timeouts are paired at either 500 ms or 1,000 ms.

The 500 ms cells are in:

- `results/ndnsf-mobility-300s-seed40-20260807-sudo`
- `results/ndnsf-mobility-300s-seed40-20260807-500ms-speed10-retry`

The 1,000 ms cells are in:

- `results/ndnsf-mobility-300s-seed40-20260807-1000ms`
- `results/ndnsf-mobility-300s-seed40-20260807-1000ms-tail3`

The original 1,000 ms campaign reached its 5,400 s outer timeout while starting the final cell. Its partial directory, plus the failed `tail`/`tail2` startup attempts, are retained but excluded from the matrix. `tail2` failed before measurement because Provider A could not obtain the DKEY; `tail3` completed the missing cell after a clean retry.

Audit result: 36/36 system summaries passed; every summary reports `sent=1500`, `status=passed`, seed 40 in its cell manifest, and a valid measurement-window coverage oracle.

## Results

Values are `success rate % / mean successful-response latency ms`; the oracle is the fraction of measurement epochs with at least one reachable Provider.

| Timeout | Range / speed | Coverage oracle | NDNSF | gRPC-SEQ-4 | NSC-4 |
|---|---:|---:|---:|---:|---:|
| 500 ms | 100 m / 2 m/s | 89.70% | 22.07 / 105.00 | 82.27 / 68.30 | 89.73 / 372.03 |
| 500 ms | 100 m / 5 m/s | 86.57% | 37.80 / 326.57 | 84.33 / 96.21 | 81.67 / 591.74 |
| 500 ms | 100 m / 10 m/s | 88.73% | 31.20 / 520.96 | 81.20 / 144.84 | 79.87 / 529.29 |
| 500 ms | 150 m / 2 m/s | 100.00% | 99.87 / 102.22 | 100.00 / 56.98 | 100.00 / 199.62 |
| 500 ms | 150 m / 5 m/s | 100.00% | 89.47 / 202.45 | 99.60 / 104.71 | 100.00 / 311.17 |
| 500 ms | 150 m / 10 m/s | 100.00% | 23.53 / 120.52 | 100.00 / 153.00 | 85.40 / 526.73 |
| 1,000 ms | 100 m / 2 m/s | 89.70% | 21.93 / 110.97 | 84.13 / 133.70 | 89.73 / 718.95 |
| 1,000 ms | 100 m / 5 m/s | 86.57% | 21.73 / 282.59 | 83.40 / 170.65 | 80.47 / 1181.29 |
| 1,000 ms | 100 m / 10 m/s | 88.73% | 23.93 / 577.54 | 79.33 / 253.51 | 78.73 / 1137.82 |
| 1,000 ms | 150 m / 2 m/s | 100.00% | 73.00 / 101.24 | 100.00 / 88.32 | 100.00 / 538.44 |
| 1,000 ms | 150 m / 5 m/s | 100.00% | 94.13 / 157.35 | 99.73 / 187.10 | 100.00 / 596.10 |
| 1,000 ms | 150 m / 10 m/s | 100.00% | 99.07 / 218.00 | 100.00 / 282.80 | 85.20 / 1064.29 |

## Interpretation

1. The low-coverage behavior is reproducible within this seed: at 100 m, NDNSF is 21.7--37.8% (500 ms) and 21.7--23.9% (1,000 ms), while the reachability oracle remains 86.6--89.7%. This is not evidence of an NDNSF reliability advantage.
2. At 150 m, NDNSF can approach the oracle (99.87%, 94.13%, 99.07%) but is highly condition-sensitive; gRPC remains near 100% in five of six 150 m cells, while NSC has much larger latency.
3. The same seed and trace hash are used for each paired timeout condition. Nevertheless, NDNSF changes by -26.87 percentage points at 150 m/2 m/s and +75.54 points at 150 m/10 m/s when the paired timeout changes. Therefore the large variation is not explained by a different mobility trace or by too few requests alone; it indicates timeout/runtime scheduling sensitivity in the current path.
4. The 300 s window supplies 1,500 requests per cell, but those requests are temporally correlated samples from one mobility trajectory. A single seed cannot support a general mobility claim. Extending the same replayed trajectory is weaker than independent seed/process repetitions.
5. The current NDNSF `FirstResponding` path selects on the first successful ACK and does not retry/reselect another Provider after a later response failure. Thus a reachable-provider oracle can be much higher than end-to-end NDNSF success.

## Recommended follow-up

Keep this matrix as a one-seed diagnostic result. Run independent seeds (preferably 41--45, separate processes) first for 100 m/2, 100 m/5, 150 m/2, and 150 m/10 at both timeout settings. Only after those repetitions should a multi-seed aggregate be used in Slides 32--33 or as evidence for a general NDNSF mobility advantage. If the variance persists, report the claim narrowly as a timeout-/coverage-dependent FirstResponding result and avoid an unconditional reliability claim.
