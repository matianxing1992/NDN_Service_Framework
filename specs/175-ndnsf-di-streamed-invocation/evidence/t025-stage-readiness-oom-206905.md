# T025 stage-readiness resource failure: Job 206905

After the missing-runner staging defect was corrected, the same current SIF,
Qwen manifest, model root, and stage-readiness parameters reached a real
three-GPU allocation. The job still failed because the Slurm script requested
the default `mem=1G` per CPU (3G total), while the previously successful G5
run used `mem=96G`.

| Item | Evidence |
|---|---|
| Candidate | `spec175-runtime-fe285147` |
| Job | `206905`, node `itiger02`, `bigTiger`, 3 × RTX 6000 |
| Slurm result | `OUT_OF_MEMORY`, elapsed `00:03:52`, batch exit `0:125` |
| Wrapper terminal | `exitCode=137`, `status=FAIL` |
| MaxRSS | `3118036K`; allocated memory was `3G` |
| Error log | `/project/tma1/ndnsf-di/submits/spec175-runtime-fe285147/ndnsf175-stage-206905.err` |
| Error SHA-256 | `sha256:51bf97d3d22903f70e19ce79add9d88277ff519a51963ec633e1c34bb7e22d02` |

The log shows ONNX Runtime CUDA graph initialization followed by:

```text
/var/spool/slurmd/job206905/slurm_script: line 69: ... Killed
slurmstepd: error: Detected 1 oom_kill event in StepId=206905.batch.
```

This is a scheduler memory-envelope failure, not a model, CUDA fallback, or
NDNSF-DI protocol failure. Historical successful G5 job `206782` used the same
three-GPU stage shape with `mem=96G` and completed in 4:44. A bounded
replacement uses the same resource envelope (`--mem=96G`) without changing the
SIF, model, workload, or stage parameters.
