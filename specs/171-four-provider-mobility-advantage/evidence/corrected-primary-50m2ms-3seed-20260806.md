# Corrected Primary Condition: 50 m / 2 m/s (2026-08-06)

## Purpose

After fixing the missing SVS `rate_rps` configuration and explicitly recording
disabled admission control, the seed-40 screen was the only primary condition
with a positive signal.  Seeds 41 and 42 were therefore rerun against their
canonical traces.

## Configuration

- Four Providers, one physical AP, `block_network=true`.
- Coverage 50 m, fixed speed 2 m/s, random-waypoint traces, seeds 40/41/42.
- 5 RPS, 60-second measured window, 300 logical requests per seed.
- NDNSF ACK timeout 1 s and request deadline 5 s.
- Every corrected NDNSF user log reports `adaptiveAdmission=disabled` and
  `expectedRps=5 window=32`.

## Paired results

| Seed | NDNSF | gRPC-SEQ-4 | NSC-4 | NDNSF−gRPC | NDNSF−NSC |
|---:|---:|---:|---:|---:|---:|
| 40 | 207/300 (69.00%) | 197/300 (65.67%) | 191/300 (63.67%) | +3.33 pp | +5.33 pp |
| 41 | 239/300 (79.67%) | 229/300 (76.33%) | 224/300 (74.67%) | +3.33 pp | +5.00 pp |
| 42 | 232/300 (77.33%) | 220/300 (73.33%) | 216/300 (72.00%) | +4.00 pp | +5.33 pp |
| **Aggregate** | **678/900 (75.33%)** | **646/900 (71.78%)** | **631/900 (70.11%)** | **+3.56 pp** | **+5.22 pp** |

The sign is consistent across all three seeds.  NDNSF used 678 Provider
executions for 900 logical requests (0.753/request) and no cross-Provider
retry; the sequential baselines issued roughly 2.4 attempts/request.  NDNSF
also had lower successful-response mean latency in each seed than both
baselines.  This is a conditional result for slow mobility and 50 m coverage,
not a general mobility claim.

## Boundary conditions

The other corrected seed-40 primary cells were not positive:

- 50 m / 15 m/s: NDNSF 68/300 (22.67%), below both baselines.
- 100 m / 2 m/s: NDNSF 300/300 (100%), tied with both baselines.
- 100 m / 15 m/s: NDNSF 221/300 (73.67%), below both baselines.

Therefore the paper may report a narrow conditional advantage at 50 m / 2 m/s
if it labels the condition, trace, and retry-cost trade-off explicitly.  It
must not claim an unconditional NDNSF mobility-reliability advantage.

## Evidence

- Corrected seed-40 cell: `/tmp/ndnsf-corrected-primary-seed40-v1/range-50-speed-2p0`.
- Corrected seed-41 cell: `/tmp/ndnsf-corrected-primary-50m2ms-v1/seed-41`.
- Corrected seed-42 cell: `/tmp/ndnsf-corrected-primary-50m2ms-v1/seed-42`.
- Existing gRPC/NSC paired cells: `/tmp/ndnsf-single-ap-primary-matrix-20260806-v3`.
- All six new/paired logical workloads had `sent=accepted=300` and measurement
  start lateness below 1 ms for the corrected NDNSF cells.
