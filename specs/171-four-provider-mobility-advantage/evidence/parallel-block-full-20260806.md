# Historical Full Blocked-Network Parallel-gRPC Repetition (2026-08-06)

> **Superseded for fairness.** This artifact was produced before the NDNSF
> absolute measurement barrier was aligned with gRPC/NSC. Its metrics are
> retained for historical diagnosis only and must not be used as final fair
> evidence. See [`fairness-audit-20260806.md`](fairness-audit-20260806.md) for
> the corrected pilot and current paper boundary.

## Historical verdict (not current claim)

`NDNSF_MOBILITY_ADVANTAGE` for the registered two-condition campaign. All 20
seed-condition groups and all 60 system cells passed. The retained aggregate
is [`aggregate.json`](../../../results/four_provider_parallel_block_full_20260806/aggregate.json).

This result extends the robustness evidence to the tested `gRPC-PAR-4`
first-success fan-out diagnostic. It does not claim superiority over every
parallel gRPC implementation: this diagnostic issues four application RPCs
per logical request and can execute duplicate server work.

## Registered configuration

- Seeds 20--29, conditions `moderate` and `stale-health`, four providers, and
  the `single-active-handoff` trace profile.
- 300 logical requests per cell, 60-second measured window, 5 RPS, and the
  same seed-matched trace shared by NDNSF, gRPC-PAR-4, and NSC-4.
- `block_network=true` with interface-level coverage-gate packet blocking. The
  gRPC/NSC clients used a four-second barrier, but this historical NDNSF path
  still used its legacy two-second delay; that mismatch is why this artifact
  is superseded.
- NDNSF uses `first-responding`; gRPC health routing is disabled. `gRPC-PAR-4`
  concurrently issues one unary call to each of the four providers and accepts
  the first successful response.

## Aggregate results

| Condition | System | Success | Median attempts/request | Median p95 (ms) |
|---|---:|---:|---:|---:|
| moderate | NDNSF | 3000/3000 (100.00%) | 1.000 | n/a |
| moderate | gRPC-PAR-4 | 1979/3000 (65.97%) | 4.000 | 149.943 |
| moderate | NSC-4 | 1827/3000 (60.90%) | 3.067 | 637.139 |
| stale-health | NDNSF | 2990/3000 (99.67%) | 0.997 | n/a |
| stale-health | gRPC-PAR-4 | 1706/3000 (56.87%) | 4.000 | 70.309 |
| stale-health | NSC-4 | 1380/3000 (46.00%) | 2.523 | 237.046 |

Paired NDNSF-minus-baseline success differences (lower/mean/upper, percentage
points) were:

- `moderate`: gRPC-PAR-4 = 33.03/34.03/34.97; NSC-4 = 32.13/39.10/45.63.
- `stale-health`: gRPC-PAR-4 = 40.43/42.80/45.37; NSC-4 = 44.90/53.67/62.63.

Both lower bounds remain well above the formal 10-point claim threshold, and
the attempt gate passes with NDNSF at approximately one attempt/request versus
baseline medians of 3.067 (`moderate`) and 2.523 (`stale-health`).

## Fan-out cost and coverage evidence

- gRPC-PAR-4 issued exactly 4 attempts per logical request in every cell and
  had zero serial failovers. Median cancellations were 583.5 (`moderate`) and
  520.5 (`stale-health`).
- The instrumented server extra-execution metric had medians 27.5 and 11.0,
  respectively. This is an important cost/semantics caveat, not a hidden
  success benefit.
- Retained `network-gate-counters.txt` files show DROP packets in NDNSF and
  gRPC cells for both conditions; NSC packet totals vary, including zero in the
  moderate NSC aggregate. The chain was installed and counters are retained,
  but packet counts are not treated as equal traffic-volume evidence.

## Reproducibility and anomalies

- 20/20 campaign summaries are `passed`; 60/60 cells returned zero and passed
  cell validation.
- Every campaign has one manifest trace SHA-256, and all three system runs in
  that campaign carry the same trace hash. All 20 manifests share one source,
  protocol, generated-artifact, and runtime hash set.
- The earlier three-seed pilot and this full campaign are excluded from fair
  claims because NDNSF did not share the baseline measurement barrier. The
  corrected barrier is documented separately in
  `fairness-audit-20260806.md`.
- The retained evidence subset is about 2.54 MB and contains aggregate,
  manifests, summaries, trace metadata, cell summaries, and gate counters;
  large runtime logs remain only in the local campaign directory.

## Paper boundary

The historical formal serial-baseline claim also remains on timing-audit hold
until its corrected replay is complete. This historical artifact therefore
does not support adding a robustness statement to the paper. Do not generalize
its parallel result to current fair evidence or to all parallel gRPC systems.

## Validation

- Focused Python regression suite: 37 passed, 1 existing Matplotlib warning.
- Python byte-compilation of the modified harness and gRPC client passed.
- Aggregate, manifests, and retained JSON artifacts parse successfully.
- No experiment processes remained after campaign completion; free `/tmp`
  space was approximately 37.45 GB.
