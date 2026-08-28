## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: run
- Origin Date: 2026-07-31
- Verification Status: UNVERIFIED
- Version Label: exp_result_v1

## Experiment Result

- **ID**: 20260731T075231Z-753be3db
- **Status**: crashed
- **Command**:
  `python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py --output-root results/spec165-local-gates`
- **Exit Code**: 1

### Result

Gate A and Gate D passed. Gate B and Gate C failed, and the aggregate correctly
reported:

```text
externalValidationAuthorized=false
tigerClusterSubmitted=false
```

The root filesystem again reached 100%. Inspection showed that fresh Gate B
preparation writes approximately 5.2 GB into each run's
`minindn/runtime/qwen-onnx-stage-artifacts` directory.

The failed run's stage-0 and stage-1 ONNX hashes matched the retained canonical
artifacts:

```text
stage-0: 6f02058ba2cc420b4f11c6e7ab391451c3fbdb9bc4ca4ba8adebd633de60ec06
stage-1: b9e4acb15b5ea2ba009f4bd6a132ce7efb90c18b5062aadc6769bd5514f3e501
```

Stage-2 was truncated at 727,633,920 bytes, compared with the retained
canonical 1,251,892,730-byte artifact, and had a different digest. The
duplicate/truncated artifact directory was therefore classified as
rebuildable failed-run input rather than evidence. Its exact removal preserves
the run's aggregate verdict, summary, logs, fidelity records, workload, and
generation campaign.

### Next admissible command

The next user-authorized run must use the existing
`--reuse-prepared-run results/spec165-local-gates/20260731T074249Z-1edd6ad0`
contract. That path symlinks the already checksum-verified ONNX artifacts while
still executing the current source through real MiniNDN and the candidate
container. It avoids creating another 5.2 GB artifact copy and does not reuse
old generation results.
