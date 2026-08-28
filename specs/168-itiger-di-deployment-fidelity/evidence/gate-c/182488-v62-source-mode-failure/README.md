# Gate C job 182488 — remote source-mode materialization failure

Status: **BLOCK**, Slurm `FAILED 1:0`, elapsed 1 minute 34 seconds on
`itiger07`.

- Candidate: `20260804T091435Z-v62-admission-container-path`
- Source identity: `sha256:d9815ff11485b4395afa9eee3cb29253e0f6524a9c7b7b1c77d3817ae7de9b6a`
- Source bundle: `sha256:0134b5d0f47b3de1cbf84ab5b256408392e5532a2a873368f9b4df9cf683360a`
- Slurm job: `182488`
- Failure: `SPEC168_SOURCE_MODE_INVALID:compat/spec162/generation-rank-inner.sh`

The upload command preserved file bytes but normalized every regular file to
`0644`, while `source-modes.json` requires executable job scripts to remain
`0555`. The job failed in the source-bundle verifier before Apptainer, CUDA, or
model loading, so it is not an NDNSF-DI runtime failure.

The corrective admission rule is to run the mode-aware
`spec168_verify_source_bundle.py` on the remote materialization before `sbatch`;
a remote `sha256sum -c` check alone is insufficient. The corrected bundle was
verified on the login node before authorizing a distinct Gate C attempt. No
three-node inference campaign was submitted.
