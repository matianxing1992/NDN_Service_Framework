# T035 Tiger Candidate 179481

Job `179481` used source manifest
`73ab801d8ee314b3adda5bf8c0da6e79762ae4e4ec689b13f2d2ee4c8d422a5c`
on `itiger07–09`. It was cancelled at `00:02:59` before repository
registration after a read-only audit proved the frozen preparation
`policy.yaml` contained no
`/NDNSF/DistributedRepo/Artifact/v2/STORE` service.

The first `USR1` cancellation design also exposed a second issue: Bash deferred
the signal trap while its foreground `srun` was active, and signalling the
rank step alone did not complete before the bounded operator wait. The default
Slurm cancellation therefore supplied the terminal authority:

```text
179481|CANCELLED by 64102|0:0|00:02:59|itiger[07-09]
179481.0|CANCELLED|0:0|00:02:20|itiger[07-09]
```

No bootstrap token or selection key existed. This failure is retained. The
replacement makes a candidate-local policy copy, records its input/output
hashes, and runs `srun` in the background so the batch shell can handle
`USR1`, terminate the rank step, remove secrets, and write `result.json`.
