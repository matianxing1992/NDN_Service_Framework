# New-SVS Range/Speed/Timeout Matrix Results

## Status

The registered descriptive screen completed all `3 ranges x 3 speeds x 2
timeouts x 3 seeds x 3 systems = 162` system cells.  Each cell sent 300
requests during a 60-second measured window.  The paired aggregate is:

```text
results/ndnsf-new-svs-matrix-full-aggregate-20260808.json
```

The two source roots are:

```text
results/ndnsf-new-svs-matrix-500ms-seeds60-62-20260808/
results/ndnsf-new-svs-matrix-1000ms-seeds60-62-20260808/
```

## Frozen conditions and provenance

| Factor | Value |
|---|---|
| AP/topology | One AP at `(200,200)` in a `400 m x 400 m` field; four Providers |
| Coverage radii | `50 m`, `100 m`, `150 m` |
| Speeds | `2 m/s`, `5 m/s`, `10 m/s` |
| Timeout | `500 ms` or `1,000 ms` attempt and ACK timeout |
| Global deadline | `5,000 ms` |
| Workload | `5 RPS`, `60 s`, `300` requests/cell |
| Network/policy | `block_network=true`, admission control disabled, gRPC health oracle disabled |
| NDNSF strategy | `FirstResponding` |

All 162 records have status `passed`, exactly 300 sent requests, complete
condition/seed/system/timeout keys, and paired trace hashes.  The NDNSF
runtime provenance is identical across the matrix:

```text
SVS library SHA-256:       588d24a587c8a3ace33b410723e0b369941df9b40ae5f4ee385448e4af2af59e
framework library SHA-256: 468c60c59e8d1a3786022c2853c3f4da6d2412c75bf168f2dea79e52a16ff4f7
```

## Aggregate results

Entries are `success rate / mean successful-response latency`.  The rates
pool the three requests-per-seed summaries for description only; the seed is
the inference unit.  `d` is the paired NDNSF-minus-baseline success-rate
difference across seeds, shown as `mean pp (sample SD pp)`.

| Radius/speed | 500 ms: NDNSF / gRPC / NSC | 500 ms: d(gRPC), d(NSC) | 1,000 ms: NDNSF / gRPC / NSC | 1,000 ms: d(gRPC), d(NSC) |
|---|---|---|---|---|
| 50 m / 2 m/s | 70.89%/54.0 ms; 70.44%/119.5; 69.78%/328.4 | +0.44 (0.51), +1.11 (0.19) | 70.89%/52.3; 70.11%/209.8; 68.33%/615.3 | +0.78 (1.07), +2.56 (0.38) |
| 50 m / 5 m/s | 32.67%/264.3; 29.44%/184.7; 35.22%/403.3 | +3.22 (4.14), −2.56 (6.74) | 32.78%/290.3; 29.33%/303.5; 34.67%/790.4 | +3.44 (3.37), −1.89 (7.62) |
| 50 m / 10 m/s | 14.33%/552.6; 21.22%/192.5; 25.22%/582.9 | −6.89 (8.18), −10.89 (10.30) | 20.00%/827.0; 20.89%/397.2; 25.00%/1175.3 | −0.89 (11.83), −5.00 (10.48) |
| 100 m / 2 m/s | 100.00%/78.9; 100.00%/40.8; 100.00%/114.7 | 0.00 (0.00), 0.00 (0.00) | 100.00%/77.8; 100.00%/70.6; 100.00%/218.1 | 0.00 (0.00), 0.00 (0.00) |
| 100 m / 5 m/s | 87.22%/63.0; 88.44%/136.1; 90.00%/353.4 | −1.22 (2.12), −2.78 (4.81) | 87.22%/62.0; 89.44%/249.6; 84.00%/658.0 | −2.22 (3.85), +3.22 (13.46) |
| 100 m / 10 m/s | 60.78%/242.9; 80.67%/178.7; 79.44%/441.7 | −19.89 (36.48), −18.67 (30.79) | 62.33%/325.4; 80.78%/328.6; 81.89%/881.3 | −18.44 (26.79), −19.56 (25.48) |
| 150 m / 2 m/s | 100.00%/90.5; 100.00%/15.7; 100.00%/22.0 | 0.00 (0.00), 0.00 (0.00) | 100.00%/91.0; 100.00%/23.0; 100.00%/32.2 | 0.00 (0.00), 0.00 (0.00) |
| 150 m / 5 m/s | 100.00%/80.3; 100.00%/74.1; 100.00%/157.5 | 0.00 (0.00), 0.00 (0.00) | 100.00%/79.5; 100.00%/133.7; 100.00%/306.1 | 0.00 (0.00), 0.00 (0.00) |
| 150 m / 10 m/s | 96.33%/140.0; 100.00%/128.5; 100.00%/168.1 | −3.67 (6.35), −3.67 (6.35) | 98.22%/216.5; 100.00%/235.6; 100.00%/327.4 | −1.78 (2.52), −1.78 (2.52) |

## Seed sensitivity and interpretation

The sign and magnitude can change across seeds.  For example, at 100 m / 10
m/s and 500 ms, the NDNSF-minus-gRPC differences for seeds 60--62 are
`+9.67, −60.67, −8.67 pp`; at 1,000 ms they are `+1.00, −49.00, −7.33 pp`.
At 50 m / 10 m/s and 1,000 ms, the corresponding differences are
`+2.33, +9.00, −14.00 pp`.  These are trajectory effects, not evidence that
one seed is the “correct” result, so the report uses the three-seed mean and
sample SD.

The matrix does **not** support a universal NDNSF success-rate advantage:

* NDNSF is clearly worse at 100 m / 10 m/s for both timeout settings, and is
  worse at 50 m / 10 m/s at 500 ms (with a large latency penalty at both
  settings).
* At 50 m / 2 m/s and 50 m / 5 m/s, NDNSF is slightly better than gRPC in
  success rate and often has lower latency, but it is not uniformly better
  than NSC.
* At 100 m / 5 m/s, NDNSF's latency is lower than both baselines, while its
  success rate is mixed; at 1,000 ms it is below gRPC but above NSC.
* At 150 m, reachability is close to saturated.  Success-rate differences are
  small or zero, so latency—not a recovery-rate advantage—is the meaningful
  distinction.

Increasing the per-attempt/ACK timeout from 500 ms to 1,000 ms does not create
a universal NDNSF advantage.  For example, NDNSF gains `+5.67 pp` at 50 m /
10 m/s but adds about `274.5 ms` mean successful-response latency; at 100 m /
10 m/s it gains only `+1.56 pp` while remaining about `18 pp` below gRPC.

## Claim boundary

This is a complete three-seed descriptive screen, not a universal mobility
claim and not a request-level independent-sample test.  The defensible paper
statement is: **NDNSF can reduce successful-response latency and can improve
success under some coverage/speed conditions, but its mobility success-rate
advantage is conditional and disappears—or reverses—under high-speed,
narrow-coverage traces.**  Any stronger positive claim requires the
pre-registered ten-seed confirmation at 100 m / 5 m/s.
