# Follow-up mobility seed/repeat registration

Purpose: test whether the five-seed holdout's mixed ordering is caused by
trace-to-trace heterogeneity rather than a stable protocol effect.

## Primary paired campaign

- New independent mobility seeds: `50--59` (10 traces; no reuse of 43--47).
- One AP, four Providers, 50 m coverage, 2 m/s random-waypoint mobility.
- Systems: NDNSF, `gRPC-SEQ-4`, and NSC-4.
- Same trace per seed and same 4 s trace-relative traffic barrier.
- 5 RPS, 60 s measured window, 1 s attempt/ACK timeout, 5 s global deadline.
- `block_network=true`, admission disabled, health routing disabled.
- Each system receives a separate process and separate output directory.

The primary inference unit is the mobility seed. The 300 requests within one
seed share one trace and are not treated as 300 independent seed replicates.
The existing SC-008 paired-gate thresholds remain unchanged; adding seeds does
not relax the claim contract.

## Independent process repeats

After the primary campaign, repeat seeds `50`, `54`, and `58` once each for all
three systems in separate campaign roots. Repeated runs must regenerate or
replay a trace with the same trace SHA-256 as the corresponding primary seed.
They are reported as within-seed process-variation diagnostics, not additional
independent mobility seeds and not pooled into the primary confidence interval.

## Planned outputs

- Primary results: `results/ndnsf-mobility-followup-20260807-primary-10seeds/`.
- Repeat results: `results/ndnsf-mobility-followup-20260807-repeats/`.
- A follow-up aggregate with seed-level success differences, coverage, process
  repeat spread, and the unchanged claim verdict.

No positive claim is admitted unless the paired seed-level gate passes after
the primary analysis. If it does not, the result strengthens the conclusion
that the current mobility condition is heterogeneous rather than demonstrating
an NDNSF superiority.
