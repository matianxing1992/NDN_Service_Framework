# Gate C job 182412 — v39 exact-SIF CUDA pass

Status: **PASS**, Slurm `COMPLETED 0:0`, elapsed 1:47 on `itiger07`.

- Source identity: `sha256:5122149d43b2a918ac9188c506857a4d74bdcde6032fc3091824f95d1f13d5e7`
- Source bundle: `sha256:576bd17903c8075219a5f51426a1c7663cfd4628f668e434932f0d0b9b5714ad`
- Runtime SIF: `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`
- Stage manifest: `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`
- Preflight result: `sha256:c2fba5b65e9e0138d08fddb3c9dc733974416d5dd520d20987a4a7a694729f67`

The existing Qwen3-0.6B three-stage artifact loaded every stage on `cuda:0`
of an NVIDIA RTX 5000 Ada Generation. CPU fallback was false for all stages;
expected and actual top token were both 8065. This is exact
SIF/source-overlay/CUDA compatibility evidence, not a distributed lifecycle
claim.
