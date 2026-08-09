# Historical Blocked-Network Parallel-gRPC Pilot (2026-08-06)

> **Superseded for fairness.** This selected-condition pilot predates the
> corrected absolute measurement barrier and is retained only to explain the
> earlier decision path. Do not cite its success-rate gap as final evidence;
> use [`fairness-audit-20260806.md`](fairness-audit-20260806.md).

## Historical decision-gate verdict (not current claim)

**Pilot gate: PASS for the selected `stale-health` condition.** NDNSF remains
well above the independently implemented four-provider parallel first-success
gRPC diagnostic (`gRPC-PAR-4`) under interface-level packet blocking. This is
not a replacement for the registered formal campaign and is not evidence for a
general claim about every parallel gRPC implementation.

The campaign runner reports `INCONCLUSIVE_MISSING_CELL` at the top level because
this pilot intentionally ran only the selected harsh `stale-health` condition;
the `moderate` condition was not silently treated as complete.

## Registered pilot

- Three matched seeds: 20, 21, and 22; one 60-second measured window per seed.
- Four providers, `single-active-handoff`, 75 m range, 15 m/s mobility,
  one-second handoff period, 5 RPS, 300 logical requests per cell.
- `block_network=true`: the coverage gate installs interface-level packet
  drops and retains `network-gate-counters.txt` in each cell.
- `gRPC-PAR-4`: all four providers receive a concurrent unary RPC for each
  logical request; the first successful response wins. Health routing is
  disabled, and this is not serial failover.
- NDNSF uses `first-responding`; all three systems use the same per-seed trace
  hash. This historical run is not the corrected four-second-barrier pilot.
- Aggregate artifact: `/tmp/ndnsf-mobility-parallel-block-pilot-phase4-20260806/aggregate.json`.

## Results

| Seed | NDNSF success | gRPC-PAR-4 success | NSC-4 success | gRPC attempts | gRPC p95 (ms) |
|---:|---:|---:|---:|---:|---:|
| 20 | 299/300 (99.67%) | 157/300 (52.33%) | 145/300 (48.33%) | 1200 | 77.027 |
| 21 | 300/300 (100.00%) | 149/300 (49.67%) | 78/300 (26.00%) | 1200 | 63.136 |
| 22 | 299/300 (99.67%) | 150/300 (50.00%) | 59/300 (19.67%) | 1200 | 49.449 |

Across the three paired samples:

- NDNSF: 898/900 = 99.78% pooled success; median per-seed success 99.67%.
- gRPC-PAR-4: 456/900 = 50.67% pooled success; median per-seed success
  50.00%.
- Paired NDNSF-minus-gRPC success gap: 47.33--50.33 percentage points per
  seed; mean 49.11 points; bootstrap interval recorded by the runner is
  47.33--50.33 points.
- gRPC issued exactly 4 attempts per logical request (1200/300), had zero
  serial failovers, and recorded 445--468 cancellations per seed. The server
  observed 7--12 request IDs with multiple executions per seed, so the
  fan-out cost must be reported with the result.
- The retained network-gate counters show non-zero DROP packets in every
  system cell (gRPC: 26--29 packets; NDNSF: 204--215; NSC: 4 packets across
  the recorded provider interfaces). These counters verify that the
  `block_network` path was active; packet totals are not used as a claim of
  equal traffic volume.

## Setup anomaly and exclusion

The earlier three-seed attempt and this selected-condition run are retained as
historical artifacts. The corrected absolute-barrier pilot, including the
setup-failure provenance and manual replacement, is documented in
`fairness-audit-20260806.md`.

## Paper boundary

The existing formal evidence remains unchanged and continues to support only
the scoped comparison against serial `gRPC-SEQ-4` and `NSC-4`. This pilot
removes the immediate concern that the result is solely an artifact of a
serial baseline: even the parallel first-success diagnostic is substantially
below NDNSF in this blocked-coverage trace. It does **not** justify changing
the paper to “NDNSF is superior to parallel gRPC” because the diagnostic fans
out four application RPCs and is not a full protocol-equivalent baseline.

## Follow-up

The pilot's historical decision gate is superseded. The corrected pilot and
the timing-audit disposition are recorded in
[`fairness-audit-20260806.md`](fairness-audit-20260806.md); no full parallel
repetition is justified by the corrected result.
