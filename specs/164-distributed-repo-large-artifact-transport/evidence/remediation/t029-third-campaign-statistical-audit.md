# T029 Third-Campaign Statistical and Scope Audit

## Material Passport

- Type: experiment validation
- Status: ANALYZED
- Frozen subject:
  `results/spec164-artifact-stability-campaign-20260730T0935Z`
- Original derived verdict: SC-003 `FAIL`
- Post-hoc diagnostic:
  `results/spec164-third-campaign-posthoc-scope-20260730T1020Z`
- Confirmatory use of post-hoc diagnostic: prohibited

## Findings

The third frozen campaign retained 144/144 scheduled runs: 24 warmups and 120
measured runs. All warmups passed. Three measured 1 MiB r1/c16 runs failed:

```text
s1048576-r1-c16-digest-only-rep2
s1048576-r1-c16-legacy-exact-packet-rep3
s1048576-r1-c16-digest-only-rep5
```

Each failing cold consumer timed out on the first CanBePrefix Interest before
the adaptive segmented window began. The subsequent adaptive window already
had a bounded retry budget; the initial Interest did not. No failure reported
`NO_ROUTE` or registration rejection, so T027's registration correction
remained effective.

The original analyzer also applied SC-003 to 1 MiB cells. Phase attribution
shows that 1 MiB signed-root asymmetric verification was only about
0.7–12.5 ms across the tested shapes, while signed/digest transfer-phase
differences reached 20–38 ms. The small-cell ratios were dominated by
process/network scheduling. The 64 MiB large-artifact cell had:

```text
signed/digest median: 0.995826
bootstrap 95% CI: [0.988893, 1.076105]
```

It passed the unchanged 0.90 threshold. Because Spec 164 targets large
artifacts and SC-002 already starts at 64 MiB, SC-003 is prospectively
clarified to the same lower bound. One MiB cells remain mandatory diagnostics
and every failure remains reported.

The post-hoc diagnostic reanalysis returns SC-003 `PASS` for the clarified
domain, but it is not confirmation and does not replace the third campaign's
original `FAIL`. Only a fourth campaign frozen after the amendment may close
the gate.

## Eleven-Fallacy Scan

Coverage: 11/11 checked.

| Fallacy | Finding |
|---|---|
| Simpson's paradox | Active risk: a pooled ratio would conceal failing/noisy 1 MiB strata; therefore per-cell distributions remain reported and the domain is explicit |
| Ecological fallacy | Not applicable; claims remain at workload-cell and campaign level |
| Berkson's paradox | No filtered-success-only sampling is permitted; all scheduled failures are retained |
| Collider bias | No post-outcome resource or latency covariate is used to select runs |
| Base-rate neglect | Not applicable; this is a throughput ratio, not a classifier |
| Regression to the mean | Re-running until a favorable result is prohibited; a new campaign is prospectively frozen |
| Survivorship bias | Detected in the old ratio construction when failed digest runs lacked pairs; completion remains a separate hard gate and failures stay visible |
| Look-elsewhere effect | Threshold and large-artifact lower bound are fixed before the fourth campaign; no best-cell selection is allowed |
| Garden of forking paths | Material risk because scope was clarified after observing data; mitigated by retaining the third `FAIL` and requiring new confirmatory evidence |
| Correlation is not causation | Phase attribution is diagnostic; it does not claim scheduling caused every ratio deviation |
| Reverse causality | Not applicable to the controlled matched benchmark |

## Verdict

`CAUTION`, not confirmatory PASS. The first-Interest retry gap requires a code
fix and a new frozen campaign. The 64 MiB eligibility amendment is
scientifically justified but post-hoc; therefore prior evidence cannot close
SC-003.
