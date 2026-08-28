# Boundary-safe opportunity holdout result

## Verdict

`HOLDOUT_CONFIRMS_CONDITIONAL_END_TO_END_ADVANTAGE`

The preregistered seeds 72--81 holdout confirms a conditional tail-latency
advantage for NDNSF when the initially configured baseline endpoint is outside
coverage but another Provider is reachable (`SWITCH_REQUIRED`). This is not an
unconditional claim over all mobility requests.

## Acceptance and provenance

- All 30 cells completed exactly once: 10 seeds x 3 systems.
- Each seed has one trace hash shared by NDNSF, gRPC, and NSC.
- Every cell passed manifest, request-count, trace-source, and 4.05-second
  traffic-phase reconciliation.
- The three systems retained actual monotonic request-publication timestamps.
- The analyzer excluded 28 non-atomic transition-boundary rows and 7 rows whose
  actual opportunity states disagreed across paired cells.
- The frozen analyzer SHA-256 remained
  `6a3404a21926aff8dc32b9e1118aa8c512b1d653215666f7c2741dee9c6ab7b9`.
- The accepted comparison contains 1,312 paired `SWITCH_REQUIRED` requests.

## Preregistered primary result

The inference unit is the mobility seed. Differences below are NDNSF minus the
baseline for each seed's successful end-to-end p95 latency, with a fixed-seed
20,000-replicate paired bootstrap 95% interval.

| Baseline | Mean difference | 95% CI | Registered gate |
|---|---:|---:|---|
| gRPC-SEQ-4 | -596.55 ms | [-992.18, -200.11] ms | Pass |
| NSC-4 | -2,618.32 ms | [-2,916.27, -2,315.09] ms | Pass |

Both upper bounds are below zero, so SC-015 passes. The gRPC effect is
heterogeneous: NDNSF has lower p95 in six seeds, while gRPC has lower p95 in
four seeds that do not incur a one-second sequential-attempt tail. The paired
mean and its interval therefore support a conditional tail-risk claim, not a
claim that every mobility realization favors NDNSF.

## Secondary descriptive result

These metrics were specified as explanatory outputs, not as the preregistered
confirmation gate.

| System | Successes | Success rate | Attempts or selected executions per request |
|---|---:|---:|---:|
| NDNSF | 1,311/1,312 | 99.92% | 1.00 |
| gRPC-SEQ-4 | 1,297/1,312 | 98.86% | 2.74 |
| NSC-4 | 1,296/1,312 | 98.78% | 2.70 |

The seed-paired NDNSF-minus-gRPC success-rate difference is +1.62 percentage
points, with a post-hoc 95% interval of [-0.03, +3.79] points. The holdout
therefore does not establish a statistically positive success-rate advantage
over gRPC. NDNSF's success rate is descriptively similar and slightly higher.
The corresponding NDNSF-minus-NSC difference is +1.31 points [0.00, +3.14].

NDNSF counts selected Provider executions, whereas gRPC and NSC count
sequential endpoint attempts. These are useful mechanism indicators but are
not identical wire-message units and must not be presented as direct protocol
overhead ratios.

## Publication interpretation

Together with the provider-transition evidence, this holdout supports two
separate statements:

1. NDNSF can discover a newly available Provider without that Provider being
   present in a client's preconfigured endpoint list; static gRPC/NSC controls
   cannot use that capacity.
2. When all systems know the same four endpoints but the initially configured
   endpoint is unavailable, NDNSF's runtime Provider selection reduces
   successful-response tail latency relative to sequential gRPC/NSC failover.

The unconditional range results must remain visible beside this conditional
panel. They show that coverage opportunity can dominate overall success and
prevent the conditional result from being generalized to every request or
topology.

## Artifacts

- `holdout-summary.json`: frozen primary analysis and per-seed metrics.
- `holdout-requests.csv`: actual-state request-level opportunity table.
- `descriptive-summary.json`: explicitly post-hoc success and mechanism-count
  summary.
- Raw campaign: `results/spec171-opportunity-holdout-100m-2ms-seeds72-81-20260809/`.
