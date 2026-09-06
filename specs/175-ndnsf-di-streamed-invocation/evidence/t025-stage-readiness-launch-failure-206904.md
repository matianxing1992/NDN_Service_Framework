# T025 stage-readiness launch failure: Job 206904

The first current-candidate stage-readiness submission was dispatched only
after the candidate/SIF/model preflights passed, but the Slurm wrapper exited
before opening the SIF because the minimal remote submit tree did not contain
the runner referenced by `qualify-stage-readiness.sbatch`:

```text
SPEC175_STAGE_RUNNER_MISSING
```

| Item | Evidence |
|---|---|
| Candidate | `spec175-runtime-fe285147` |
| Job | `206904`, node `itiger02` |
| Slurm result | `FAILED`, elapsed `00:00:01`, exit `4:0` |
| Error log | `/project/tma1/ndnsf-di/submits/spec175-runtime-fe285147/ndnsf175-stage-206904.err` |
| Error SHA-256 | `sha256:51bf97d3d22903f70e19ce79add9d88277ff519a51963ec633e1c34bb7e22d02` |

No SIF, model, CUDA, or NDNSF-DI application process ran in this job. The
exact source runner was then added to the submit tree and its SHA-256 was
matched (`ef932584339d07b4927b72ceea5a9716730b1eaffe8c8a4426948bb60f17992d`)
before the bounded replacement submission `206905`.
