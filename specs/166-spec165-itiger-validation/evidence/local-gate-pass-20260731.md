## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: run
- Origin Date: 2026-07-31
- Verification Status: VERIFIED
- Version Label: exp_result_v1

## Experiment Result

- **ID**: `20260731T153957Z-4f22b8c2`
- **Type**: Spec 165 local deployment gates
- **Status**: completed
- **Command**: `python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py --output-root results/spec165-local-gates --reuse-prepared-run results/spec165-local-gates/20260731T074249Z-1edd6ad0 --candidate-image ndnsf-di:spec165-minindn-gate`
- **Gate A**: PASS
- **Gate B**: PASS, real MiniNDN and Qwen ONNX
- **Gate C**: PASS, real frozen candidate container
- **Gate D**: PASS
- **External validation authorized**: true
- **TigerCluster submitted by local runner**: false

The canonical ONNX artifacts were symlinked after checksum validation; no new
5--6 GB physical artifact copy was created. The preceding identity
`20260731T153737Z-e2592596` remains a failure because its invocation omitted
the required candidate-image argument.

## Final-source revalidation

- **ID**: `20260731T164038Z-d1540ad6`
- **Status**: completed; Gate A-D PASS
- **Purpose**: revalidate the final request-ID tensor propagation and atomic
  timing-marker changes after TigerCluster closure.
- **Prepared artifact source**:
  `results/spec165-local-gates/20260731T074249Z-1edd6ad0`
- **External validation authorized**: true
- **TigerCluster submitted by local runner**: false

This run used the same command and reuse contract recorded above. It created no
new physical ONNX artifact copy.
