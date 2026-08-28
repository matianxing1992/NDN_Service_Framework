## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: run
- Origin Date: 2026-07-31
- Verification Status: UNVERIFIED
- Version Label: exp_result_v1

## Experiment Result

- **ID**: 20260731T074943Z-346da1f8
- **Type**: generic
- **Status**: crashed
- **Command**:
  `python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py --output-root results/spec165-local-gates`
- **Working Directory**:
  `/home/tianxing/NDN/ndn-service-framework`
- **Exit Code**: 1

### Output Summary

The final-source Spec 165 Gate A-D rerun stopped while writing a unit-gate log:

```text
OSError: [Errno 28] No space left on device
```

At failure, `/dev/sda5` had 112 KiB available and was 100% full. This run
therefore provides no external-validation authorization and must not be treated
as a model, MiniNDN, candidate-container, or deadline result.

The earlier run `20260731T074249Z-1edd6ad0` passed all four gates, but it
predates the final profile-policy tightening and is not used to authorize
`source-004`.

### Output Files

The partial run is retained under:

`results/spec165-local-gates/20260731T074943Z-346da1f8`

### Anomalies Detected

- Local storage exhaustion during evidence writing.
- No TigerCluster job or `source-004` was created.
- No automatic experiment retry occurred.
- Exactly 5.235 GB of rebuildable Docker build cache was removed afterward.
  Candidate images, model files, source bundles, and experiment evidence were
  not deleted. The root filesystem then had approximately 5.2 GB available.
