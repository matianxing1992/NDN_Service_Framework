# Job 182493: retained pre-runtime contract failure

Job 182493 is an immutable failed attempt. Slurm reports `FAILED`, exit code
`1:0`, elapsed time `00:00:13`, on `itiger[07-09]`. The job stopped before
`srun`, Provider startup, model fetch, or inference because its inline formal
workload check rejected the valid extra field `repetitions.sequential=true`.
The exact error is retained in `slurm-182493.err` as
`SPEC168_FORMAL_REPETITIONS_REQUIRED`.

This was a local test-fidelity defect, not a TigerCluster, DistributedRepo, or
Qwen failure. The synthetic contract fixture represented only
`warmupPerPrompt` and `measuredPerPrompt`; it did not pass the exact frozen
formal manifest through the job's validation path. The linked repair requires
all three fields, validates the real manifest through one shared contract
utility, and must use a new source and campaign identity. This attempt must
never be relabelled or retried in place.
