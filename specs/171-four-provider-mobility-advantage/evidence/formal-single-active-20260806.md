# Formal Single-Active Mobility Evidence (2026-08-06)

> **Timing audit hold.** This aggregate was generated before NDNSF adopted the
> shared absolute monotonic measurement barrier. Its gRPC/NSC cells used the
> four-second registered barrier, while the historical NDNSF launcher used a
> two-second delay and did not emit the now-required start/lateness fields.
> Retain the artifact for historical diagnosis, but do not treat its claim
> verdict as final fair evidence until a corrected serial replay is completed.
> See [`fairness-audit-20260806.md`](fairness-audit-20260806.md).

## Historical verdict (on-hold pending corrected replay)

`NDNSF_MOBILITY_ADVANTAGE`.

The full paired campaign completed 20/20 cells: 10 `moderate` and 10
`stale-health`, with seeds 20--29 in both conditions. The aggregate is recorded
in [`aggregate.json`](../../../results/four_provider_single_active_formal_fixed_20260806/aggregate.json).

## Registered configuration

- Four providers, `single-active-handoff` trace, identical trace per seed and
  system, 5 RPS, 300 logical requests per cell, 60-second measured window.
- Baselines: `gRPC-SEQ-4` with proactive health routing disabled and `NSC-4`.
- NDNSF strategy: `first-responding`.
- The campaign used fixed seeds 20--29. A seed is shared by the three systems
  within a matched cell; the seed set is retained for repeated paired samples.
- The final runner used one startup-only free-space check with
  `min_free_gb=5`; it did not add periodic disk polling or a background
  monitor.

## Aggregate results

| Condition | System | Success | Median attempts/request | Median p95 (ms) |
|---|---:|---:|---:|---:|
| moderate | NDNSF | 3000/3000 (1.000) | 1.000 | n/a |
| moderate | gRPC-SEQ-4 | 2547/3000 (0.849) | 2.550 | 635.917 |
| moderate | NSC-4 | 2460/3000 (0.820) | 2.550 | 638.665 |
| stale-health | NDNSF | 2997/3000 (0.999) | 1.000 | n/a |
| stale-health | gRPC-SEQ-4 | 2222/3000 (0.741) | 2.242 | 234.188 |
| stale-health | NSC-4 | 2208/3000 (0.736) | 2.247 | 244.045 |

Paired NDNSF-minus-baseline success differences were:

- `moderate`: gRPC lower/mean/upper = 10.27/15.10/19.67 percentage points;
  NSC = 13.00/18.00/23.00 points.
- `stale-health` (harsh claim condition): gRPC = 23.00/25.83/28.83 points;
  NSC = 23.63/26.30/29.17 points.

The SC-005 claim gate passes: both harsh lower bounds exceed 10 percentage
points, and NDNSF's median attempts/request (1.0) is no more than twice the
lower baseline (2.242).

## Reproducibility and retained anomalies

- All 20 final manifests contain the same source/protocol/generated/binary
  hash set; the canonical `WifiRouterMobilityReliability.py` hash is
  `70ab21954282ccedf84a0881734d4139c56142bbc978c04a9b339c0123af818a`, and
  the fixed gRPC failover client hash is
  `acd57ed76b3d3bcf17ad22c53db8ee23d025748e13ba9f20d7ed4613d437847c`.
- There is one trace hash per final cell, shared by all three systems; the 20
  cell trace hashes are distinct across the paired seed/condition units.
- The earlier interrupted/setup-failure artifacts for moderate seeds 20, 22,
  and 23 remain under
  `results/four_provider_single_active_formal_fixed_20260806/_quarantine/`;
  they were not deleted or counted as final cells. The repaired runs use the
  same seeds and registered parameters.
- Final host free space after aggregation was approximately 35 GB; no model
  artifact was copied by this campaign.

This historical evidence was intended to support the scoped claim that under
the registered four-provider single-active mobility trace, NDNSF's
multi-provider selection avoids the serial failover penalty observed in
`gRPC-SEQ-4` and `NSC-4`. Because its NDNSF measurement phase was not aligned,
that claim remains on hold pending a corrected serial replay. The corrected
parallel pilot does not support a claim of superiority over parallel gRPC.

## ARS methodology audit

**Material Passport**: `formal-single-active-20260806`; type `experiment-result`;
status `VERIFIED` for protocol inputs and aggregate completeness, with timing
metrics treated as environment-sensitive.

- Statistical interpretation is descriptive: no p-values are claimed; the
  registered paired lower/mean/upper success differences and attempt gate are
  reported directly.
- Reproducibility inputs are verified: fixed seed schedule, identical paired
  traces, one source-hash set across all 20 cells, complete manifests, and
  successful same-seed repair runs for the three interrupted cells. Exact
  timing byte identity is not required for this environment-sensitive MiniNDN
  workload.
- Fallacy scan: **11/11 checked**. Simpson/ecological/Berkson/collider bias,
  base-rate neglect, regression-to-mean, survivorship, look-elsewhere,
  forking-paths, correlation-as-causation, and reverse-causality issues were
  either not applicable to this controlled system-level comparison or were
  bounded by the registered conditions, retained anomalies, and scoped claim.
