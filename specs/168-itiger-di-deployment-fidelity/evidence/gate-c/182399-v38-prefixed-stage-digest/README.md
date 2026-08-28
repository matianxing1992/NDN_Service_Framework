# Gate C job 182399 — prefixed stage digest (no CUDA execution)

Status: **FAILED PRECONDITION** in 51 seconds with exit code `1:0`.

The source bundle verified. The job then hashed the existing 9.5 GiB SIF and
reached the stage-manifest binding check. The submission supplied
`SPEC168_STAGE_MANIFEST_SHA256=sha256:8d8475...`, while the shell contract
compares that variable to the raw 64-hex `sha256sum` output. The mismatch
terminated the job before output-directory creation, Apptainer, Python, CUDA,
or model execution. Slurm stderr is empty; stdout contains only the successful
source-bundle check.

This is a submission-configuration failure, not a v38 candidate result. A new
Gate C job supplies the exact same digest bytes without the `sha256:` scheme
prefix. Job 182399 is retained and must not be reported as an inference attempt.
