## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: validate
- Origin Date: 2026-07-24
- Verification Status: ANALYZED
- Version Label: validation_v1

## Validation Report

- **Source**: Spec 142 canonical r4 campaign
- **Overall Confidence**: CAUTION

### Statistical Findings

| Metric | Test | Value | Effect Size | Confidence |
|---|---|---|---|---|
| Offered load | Contract check | 400.00 pps per peer in all receipts | N/A | SOLID |
| Delivery | Descriptive, one run per mode | inline 98.742%--100%; worker 100% | Not estimated | CAUTION |
| Latency | Raw survivor distributions | worker has lower descriptive mean/p50/p95/p99 | Not estimated | CAUTION |
| Recovery validity | Predefined zero-delta gate | nonzero retry/timeout in both modes | N/A | SOLID |

There are no p-values, confidence intervals, or inferential effect sizes. With
one run per mode and both receipts invalidated by recovery, no population-level
or causal performance estimate is supported.

### Warnings

| Type | Detail | Affected |
|---|---|---|
| Invalid treatment isolation | Recovery activated during the 60-second measurement window in both modes. | All worker-performance comparisons |
| No replication | One canonical cell per mode cannot estimate run-to-run variance. | Delivery and latency differences |
| Survivor distribution | Latency is defined only for delivered publications; the report labels this explicitly. | Inline peer-b in particular |
| Development-path multiplicity | r2/r3 exposed and corrected defects before canonical r4; none are pooled. | Reproducibility/provenance |
| Missing resource metric | CPU utilization was not captured by the frozen r4 schema. | Resource comparison |

### Fallacy Scan

- **Coverage**: 11/11 fallacy types checked

| Fallacy | Severity | Detail | Recommendation |
|---|---|---|---|
| Simpson's paradox | NOTE | Peer-level and aggregate directions were inspected; no aggregate causal result is reported. | Keep peer rows visible. |
| Ecological fallacy | NOTE | Conclusions are limited to the two-node setup, not individual applications or deployments. | Preserve the claim boundary. |
| Berkson's paradox | NOTE | No selected human/sample population is involved. | N/A |
| Collider bias | NOTE | No regression controls or conditioned causal model are used. | N/A |
| Base-rate neglect | NOTE | No diagnostic-classification metric is interpreted. | N/A |
| Regression to the mean | NOTE | No extreme-case pre/post selection is used. | N/A |
| Survivorship bias | CAUTION | Latency excludes undelivered publications, although attrition is reported and below 15%. | Continue labeling latency as a survivor distribution. |
| Look-elsewhere effect | NOTE | Rates and gates were specified before the canonical run; 600/800 were not searched after failure. | Do not add post-hoc rates to Spec 142. |
| Garden of forking paths | CAUTION | Two development campaigns preceded r4 after concrete implementation defects were found. | Preserve all campaigns and use only r4 as canonical post-fix evidence. |
| Correlation != causation | RED_FLAG | Descriptive worker latency is lower, but recovery invalidates the sole-treatment comparison. | Do not claim worker causality from Spec 142. |
| Reverse causality | NOTE | Not applicable to the controlled software treatment, and no causal claim is accepted. | N/A |

### Reproducibility

- **Method**: deterministic raw-sample re-analysis only; network experiment not
  rerun because the frozen campaign contract forbids replacement runs
- **Verdict**: CANNOT_VERIFY for cross-run reproducibility

| Metric | Original | Re-analysis | Diff | Status |
|---|---|---|---|---|
| Sample count | peer summary | raw CSV count | 0 for all peers | MATCH |
| mean/p50/p95/p99 | peer summary | raw CSV recomputation | 0 for all peers | MATCH |
| Manifest/terminal identity | frozen SHA-256 | analyzer SHA-256 | 0 mismatches | MATCH |
