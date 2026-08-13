# Four-Provider Mobility Screening: Negative Mechanism Result

Date: 2026-08-06

## Scope

This artifact records the completed screening and the one complete
`all-selected` paired seed.  It is not a claim of statistically significant
performance: the registered ten-seed repetition gate was intentionally stopped
after the first complete seed because the mechanism already failed the
pre-registered success/cost screen.

All three systems used the same four-Provider trace for the stale-health cell:

- profile: `four-provider-multi-ap`
- AP layout: `multi-ap` coverage geometry at `(130,200)`, `(200,200)`, and
  `(270,200)` metres
- range: `75 m`
- speed: `15 m/s`
- workload: `300` logical requests (`5 RPS` for `60 s`)
- deadline/attempt timeout/health interval: `300/100/1000 ms`
- seed: `20`
- trace SHA-256:
  `0761a45a425f9fcebb6398cbd16d2295b7ad86488d2bdbe7e420c8eb195cd14f`

The full immutable campaign is under
`results/four_provider_mobility_repetitions_allselected_20260806/stale-health-seed-20/`.
Its manifest, per-cell evidence hashes, runtime commands, and source hashes
are retained.

## Completed all-selected seed

| System | Success | Attempts / executions | Success rate | Attempts/request |
| --- | ---: | ---: | ---: | ---: |
| NDNSF (`all-selected`) | 297/300 | 620 Provider executions | 99.00% | 2.067 |
| gRPC-HC-4 | 299/300 | 310 attempts | 99.67% | 1.033 |
| NSC-4 | 275/300 | 492 attempts | 91.67% | 1.640 |

NDNSF therefore trails gRPC by `0.67` percentage points while executing about
`2.0x` as many Provider handlers.  It exceeds NSC by `7.33` percentage points,
but this is not the requested NDNSF-vs-gRPC mobility advantage.

## First-responding negative control

The prior v2 stale-health batch completed seeds 20--24 with the same paired
trace protocol.  NDNSF minus gRPC success differences were `-0.67`, `-1.33`,
`-1.33`, `-1.33`, and `-1.33` percentage points respectively; NDNSF never
outperformed the four-target gRPC baseline in those completed cells.  NDNSF
did outperform sequential NSC in every one of those five cells, with the
mean difference `+7.80` percentage points.

## Decision

The screening does not justify a formal ten-seed `all-selected` campaign:
success is not better than gRPC and the duplicate execution cost is high.  The
repetition driver was stopped after seed 20, then run in `--aggregate-only`
mode to preserve the partial report.  Its verdict is
`INCONCLUSIVE_MISSING_CELL` because the registered moderate condition and the
remaining seeds are absent; this must not be reported as a positive or
negative statistical claim.

The single-AP `50 m` validation is retained separately as an all-unavailable
diagnostic.  It is not a paired baseline result because gRPC prewarming also
failed, so it is excluded from this decision.

## Interpretation boundary

`multi-ap` in this MiniNDN harness is deterministic nearest-AP coverage
geometry over a single stable AP/backhaul topology.  It is valid for matched
reachability scheduling, but it is not physical Wi-Fi association/handoff
evidence.  A later physical-handoff claim requires instrumented multiple AP
association events.
