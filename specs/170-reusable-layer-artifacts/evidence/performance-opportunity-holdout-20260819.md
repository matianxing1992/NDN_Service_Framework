# Spec171 opportunity-holdout performance analysis (2026-08-19)

## Material Passport

- **Material ID:** `spec171-opportunity-holdout-analysis-20260819`
- **Type:** matched mobility performance re-analysis
- **Verification Status:** `ANALYZED` (the analysis script was rerun against retained experiment artifacts; the network campaign itself was not rerun)
- **Source:** `results/spec171-opportunity-holdout-100m-2ms-seeds72-81-20260809`
- **Analysis script:** `Experiments/analyze_spec171_opportunity_holdout.py`
- **Machine-readable record:** [`performance-opportunity-holdout-20260819.json`](performance-opportunity-holdout-20260819.json)

## Registered design

The holdout contains seeds 72–81, 100 m coverage, 2 m/s movement, four
Providers, 300 requests per system and seed, `block_network=true`, 1 s attempt
timeout, and a 5 s global deadline. gRPC uses four static targets with
sequential failover and health routing disabled. NDNSF uses FirstResponding
with the recorded response-retry option. The analysis script reconciles every
system's 300 request markers and derives the opportunity class from the
recorded provider-availability trace.

## Reproduction

```bash
python3 Experiments/analyze_spec171_opportunity_holdout.py \
  results/spec171-opportunity-holdout-100m-2ms-seeds72-81-20260809 \
  <analysis-output-directory>
```

The rerun completed with:

```text
verdict=HOLDOUT_CONFIRMS_CONDITIONAL_END_TO_END_ADVANTAGE
switch_required_requests=1312
transition_boundary_requests=28
state_disagreement_requests=7
```

On the 1,312 predeclared switch-required requests, the paired 20,000-draw
bootstrap of per-seed p95 differences reported:

| Comparison | Mean NDNSF minus baseline | 95% bootstrap interval |
|---|---:|---:|
| gRPC | −596.55 ms | [−992.18, −200.11] ms |
| NSC | −2,618.32 ms | [−2,916.27, −2,315.09] ms |

This is a conditional p95 result. Across all 3,000 requests, the successful
latency means were 106.34 ms (NDNSF), 98.77 ms (gRPC), and 1,008.30 ms (NSC),
with success rates 77.80%, 77.23%, and 78.57%, respectively. Thus the holdout
does **not** show an overall NDNSF latency or success-rate advantage.

## Interpretation and limits

The result supports the narrow claim that NDNSF can reduce p95 latency in a
predeclared mobility window where the initially selected Provider is not
reachable. It does not establish a global optimum: it covers one range and
speed, one holdout block, and a selected switch-required subset. The 28
transition-boundary and 7 state-disagreement requests are reported rather than
silently folded into the subset. Successful-latency conditioning and subset
selection are explicit cautions; the 11/11 statistical-fallacy checklist was
reviewed with no red-flag finding.

The result therefore strengthens, but does not close, T036. The required three
clean-start P01–P05 blocks, hierarchical bootstrap, TOST/equivalence test, and
Holm family correction remain absent.
