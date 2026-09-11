# Negative dependency evidence — v56

Slurm job `210473` (`itiger02,itiger03`, run `tiger-negative-dependency-v56-r1`)
completed with exit 0 and collector qualification
`EXPECTED_REJECTION_PASS`. Verdict SHA is
`sha256:15cbf0df6f48e685aab3c15fdf2f0f3be90402fccc5a1f3ed0e98e575fdbd76b`.
The run reaches genuine Selection, withholds exactly the bound
DetectShard0→Merge dependency, records the native `DEPENDENCY_DATA_MISSING`
failure, writes a User `OBSERVATION_ONLY` record, has no successful response or
reselection, and closes all private processes and directories within budget.
The native reason wrapper accepted by v56 is documented in the runtime
checkpoint and failure log.
