# Spec 170 performance-closure audit

**Scope.** This audit separates protocol-correctness evidence from the
publication-quality performance claim. It uses historical r23 Tiger evidence,
retained MiniNDN mobility artifacts, and a deterministic re-analysis of the
registered Spec171 holdout; it does not create another SIF or launch a new
TigerCluster campaign. The r23 measurements qualify only the source revision
sealed in r23; the holdout re-analysis qualifies its retained harness/data
manifests and does not claim current dirty-tree or SIF provenance.

## Evidence status

| Requirement | Status | Evidence or gap |
|---|---|---|
| D2b positive plus peer-mismatch, replay, and partial negatives | PASS (qualification) | Tiger jobs 201039--201042, historical r23 SIF `5b8bd6...9eeb` |
| D2h hybrid rank mapping `[1,2,1]` | PASS (qualification) | Tiger job 201045, numeric oracle max error 0 |
| D2h hybrid rank mapping `[2,1,2]` on historical r23 | PASS (qualification) | Tiger job 201046, numeric oracle max error 0 |
| Three clean-start performance blocks | MISSING | No complete three-block corpus |
| P01--P05, one measured cold and five measured warm requests per block | MISSING | No locked prompt/repetition matrix |
| 15 measured cold + 75 measured warm requests per configuration | MISSING | Current artifacts are smoke runs only |
| Matched treatment/baseline paired estimand | PARTIAL | Spec171 has a registered 10-seed range/speed matrix and a rerun 100 m opportunity holdout, but not the T036 three-block P01--P05 design |
| Hierarchical bootstrap (10,000), TOST, and Holm family correction | MISSING | No eligible corpus or analysis output |

## Current measurements

### Manifest verification (2026-08-19)

The retained manifests were re-read from the workspace before this audit. The
Spec171 range/speed corpus is complete (`total_cells=60`, `complete_cells=60`)
but its registered verdict remains `DESCRIPTIVE_RANGE_SPEED_MATRIX_ONLY`:

```text
results/spec171-burnin300-100-150m-2ms-seeds62-71-20260808/registration.json
  sha256=a39301a28db9f24165315b569b7c1741752e7bf4c235b40d715d0177b951cefc
results/spec171-burnin300-100-150m-2ms-seeds62-71-20260808/aggregate.json
  sha256=a855b1e145d3fa3b74605d7f87644b50601e7368ed4798a208f9352848d0279e
results/spec171-burnin300-100-150m-2ms-seeds62-71-20260808/latency-mechanism.json
  sha256=59933f5466ce0b8869033aa43e0aae1aa44f0915693a9ceffbb9810e00295628
```

The Spec173 confirmatory-v2 manifest also reports `status=pass` and
`manuscriptEligible=true` (campaign-summary SHA-256
`582d5ccc326e09426ccc8374a5537cf874195f4b127caee30976b7a3388d8e9b`), but
its aggregate rows explicitly set `significanceClaimAllowed=false` for the
registered three-repetition analysis. These checks confirm artifact identity;
they do not turn either corpus into T036 evidence.

The local claim-boundary regression also passes:

```text
python3 -m pytest -q tests/python/test_spec173_paper_submission_evidence.py \
  tests/python/test_spec164_performance_harness.py
18 passed
```

It verifies that descriptive/smoke artifacts cannot silently advertise an
unmeasured optimum or significance claim. This protects the interpretation
boundary; it does not create the missing T036 corpus.

The combined rerun on 2026-08-19 covered the paper-evidence, performance
harness, mobility holdout, cold/warm gate, and performance-analysis tests:
**40 passed in 2.30 s** (log SHA-256
`a9d1bef43ba9966c410e31a7bde92e3f1c1161386776057f9d73c109fc0c28ba`). The
claim guards remain green; the required T036 measurements remain absent.

The retained opportunity-holdout analyzer was independently rerun against its
original campaign directory and reproduced the same
`HOLDOUT_CONFIRMS_CONDITIONAL_END_TO_END_ADVANTAGE` verdict, with 1,312
switch-required requests, 28 transition-boundary requests, and 7 state
disagreements. This is a reproducibility check, not a new campaign or a global
optimality result.

The two canonical MiniNDN host-simulation smoke artifacts report one request
each, CPU ORT 1.26.0, no physical-GPU evidence, and controlled host telemetry
disabled:

* fixed11: makespan/p50/p95 about **203.857/202.899/202.899 ms**, maximum stable
  rate **4.9054 RPS**;
* fixed12: makespan/p50/p95 about **206.508/205.609/205.609 ms**, maximum stable
  rate **4.8424 RPS**.

These are useful regression references, not a performance comparison. They do
not establish an optimum, a treatment advantage, a p95 ratio, or a confidence
interval.

The existing `results/spec173-paper-submission-confirmatory-v2` corpus is
stronger and must be considered before making any performance claim. Its
manifest is marked `status=pass` and `manuscriptEligible=true`, and it contains
three repetitions for the one-Provider baseline. The mean successful-response
latencies are:

| Configuration | gRPC | NSC | NDNSF |
|---|---:|---:|---:|
| one Provider, 10 RPS | **82.57 ms** | **161.45 ms** | **403.93 ms** |
| one Provider, p95 | **84.14 ms** | **163.74 ms** | **409.08 ms** |

Thus this corpus does not support an NDNSF latency advantage in the one-
Provider condition; NDNSF is slower in the measured configuration. At 30 RPS,
the NDNSF FirstResponding multi-Provider runs reached 100% success with mean
latency about 691.58 ms, while the custom-selection variant reached only about
41% success with about 275.42 ms among successful responses. Those are two
NDNSF policies, not matched gRPC/NSC multi-Provider baselines, so they cannot
establish a cross-system optimum. At 70/100 RPS, admission-enabled and
admission-disabled cells have materially different load-generation behavior;
the disabled cells with zero completed responses are not valid evidence of a
capacity optimum.

The v2 campaign therefore supplies useful reproducible performance evidence,
but its result is a negative/conditional finding rather than proof of
optimality. A separate registered Spec171 matrix does provide a matched
multi-Provider descriptive comparison: ten independent seeds, 300 s mobility
warmup, 60 s measurement, single AP, 2 m/s, 5 RPS, four Providers, 1 s attempt
timeout, and 5 s global deadline. It uses the same 100 m and 150 m traces for
NDNSF, sequential gRPC, and sequential NSC, with gRPC health routing disabled
and NDNSF response-level retry enabled as explicitly recorded treatment
semantics. At 100 m, mean successful-response latency is **72.55 ms (NDNSF)**,
**80.33 ms (gRPC)**, and **1,039.44 ms (NSC)**; at 150 m it is **100.28 ms**,
**80.25 ms**, and **465.69 ms**, respectively. Success counts are equal at
100 m (2,939 NDNSF/gRPC and 2,950 NSC out of 3,000 issued) and at 150 m
(3,000/3,000 for all three). This supports a conditional NDNSF latency
advantage at the 100 m condition, not a global optimum: the ordering reverses
for NDNSF versus gRPC at 150 m, and the matrix is descriptive only.

The Spec171 artifact is therefore stronger than the v2 one-Provider corpus but
still does not close T036: it has one 10-seed block per condition rather than
three clean-start P01--P05 blocks, and it does not provide the required
10,000-bootstrap, TOST, and Holm analysis.

As an exploratory check only, resampling the ten per-seed mean latencies (10,000
seed-level draws, not a preregistered inferential result) gives NDNSF minus
gRPC mean differences of **−6.52 ms [−44.60, 29.86]** at 100 m and
**+20.03 ms [−14.89, 52.95]** at 150 m. The intervals include zero in both
conditions. The corresponding NDNSF minus NSC differences are **−975.45 ms
[−1,207.51, −754.24]** and **−365.41 ms [−544.08, −201.74]**. These values
support reporting a range-dependent descriptive pattern, not a statistically
verified NDNSF-versus-gRPC optimum.

This agrees with the registered Spec171 claim gate: an advantage statement was
allowed only if the run-level lower confidence bound was positive against both
sequential baselines. The artifact records
`DESCRIPTIVE_RANGE_SPEED_MATRIX_ONLY`, so it correctly does not authorize an
NDNSF advantage claim.

### Opportunity holdout re-analysis

The separate registered holdout for seeds 72--81 at 100 m and 2 m/s was
re-analyzed with the retained `analyze_spec171_opportunity_holdout.py` script.
The machine-readable result is
[`performance-opportunity-holdout-20260819.json`](performance-opportunity-holdout-20260819.json),
with the statistical interpretation in
[`performance-opportunity-holdout-20260819.md`](performance-opportunity-holdout-20260819.md).

The analysis reconciled all 3,000 request markers per system and identified
1,312 predeclared switch-required requests. A 20,000-draw paired bootstrap of
per-seed p95 differences produced NDNSF-minus-gRPC **−596.55 ms [−992.18,
−200.11]** and NDNSF-minus-NSC **−2,618.32 ms [−2,916.27, −2,315.09]**. This
supports a conditional p95 advantage in the switch-required subset. Across
all requests, however, successful-latency means were 106.34 ms (NDNSF), 98.77
ms (gRPC), and 1,008.30 ms (NSC), with success rates 77.80%, 77.23%, and
78.57%. The holdout therefore does not establish an overall latency or
success-rate advantage, and it cannot establish a global optimum.

The evidence files are content-addressed by the following hashes:

```text
registration.json       a39301a28db9f24165315b569b7c1741752e7bf4c235b40d715d0177b951cefc
aggregate.json          a855b1e145d3fa3b74605d7f87644b50601e7368ed4798a208f9352848d0279e
latency-mechanism.json  59933f5466ce0b8869033aa43e0aae1aa44f0915693a9ceffbb9810e00295628
```

## Statistical fallacy scan

1. **Simpson's paradox:** no subgroup aggregate comparison is available; not assessed.
2. **Ecological fallacy:** no aggregate-to-individual inference is made.
3. **Berkson/selection bias:** green smoke runs are selected; failure and timeout distributions are absent (**CAUTION**).
4. **Collider bias:** no causal adjustment model is used; not observed.
5. **Base-rate neglect:** this is not a diagnostic-classification result.
6. **Regression to the mean:** no pre/post extreme-value selection is used.
7. **Survivorship bias:** successful smoke requests are over-represented; incomplete evidence (**CAUTION**).
8. **Look-elsewhere effect:** historical configurations are numerous, but no preregistered performance family exists (**CAUTION**).
9. **Garden of forking paths:** prior diagnostic variants were explored; current smoke values are not a preregistered comparison (**CAUTION**).
10. **Correlation versus causation:** latency must not be attributed to the NDNSF-DI architecture from these runs.
11. **Reverse causality:** no temporal causal claim is made.

**Audit verdict:** protocol qualification is positive for the historical r23 D2b
cases and both D2h mappings. The one-Provider corpus shows no NDNSF latency
advantage. The matched Spec171 matrix and opportunity holdout show conditional
advantages in specific mobility/switching subsets, but not a global “best”
configuration. Performance closure and “best” status remain **not verified**;
the correct paper wording is a measured conditional result, not a
statistically supported general performance advantage.

## Disk-constrained next step

If publication-quality performance is still required, reuse the existing r23
SIF and scripts and run only the declared three-block paired matrix after
agreeing on retention limits. Keep summaries, manifests, hashes, and compact
metrics; do not retain duplicate SIFs or raw debug traces. If the disk budget
cannot safely hold that matrix, stop at this audit and explicitly report that
performance optimality remains unverified rather than launching partial runs.
