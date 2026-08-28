# Gate C job 182401 — v38 exact-SIF CUDA pass

Status: **PASS**, Slurm `COMPLETED 0:0`, elapsed 20 seconds on `itiger07`.

- Source identity: `sha256:1e192ca033600a6c4b10ad1e2393172989d08ff129956aaf551712d5e06b1949`
- Source bundle: `sha256:7703964b4813502835c1b39c8106dd0795596988e74af18ed992d82ba24b2a8c`
- Runtime SIF: `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`
- Stage manifest: `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`
- Preflight result: `sha256:860771ff5ef2cf7f9386587e3f05b7f0719ca46cde5d578ea5899140269f0d76`

The existing Qwen3-0.6B three-stage artifact loaded all stages on `cuda:0` of
an NVIDIA RTX 5000 Ada Generation. CPU fallback was false for every stage and
the reference comparison matched (`expectedTopToken=8065`,
`actualTopToken=8065`). This proves exact SIF/source-overlay/CUDA compatibility;
its declared boundary is a single-node environment preflight, not a distributed
request-lifecycle result.

Jobs 182398 and 182399 were separately retained as pre-Apptainer submission
configuration failures. They did not execute this candidate and are not folded
into the PASS claim.
