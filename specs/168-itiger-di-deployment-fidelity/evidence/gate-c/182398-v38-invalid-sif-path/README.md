# Gate C job 182398 — invalid SIF path (no CUDA execution)

Status: **FAILED PRECONDITION** in 2 seconds with exit code `1:0`.

The submission used a SIF directory name truncated from a compacted session
summary (`spec167-native-file-producer-0f834...`) instead of the complete,
repository-evidenced immutable path ending in
`0f834dbaad7496628fb31bfcdfc87f6ead0874a03db65329c5536d8b1da63d92`.
The job failed at `test -r "$SPEC168_RUNTIME_SIF"` before Apptainer, Python,
CUDA, or model execution and produced empty Slurm stdout/stderr.

This is a submission-configuration failure, not a result for the v38 runtime
candidate. The source identity and source bundle remain unchanged. A new Gate C
job uses the exact SIF path read from the frozen v37 campaign evidence; job
182398 is retained and must not be described as an inference attempt.
