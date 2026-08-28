# Gate C job 182489 — v62 exact-SIF CUDA pass

Status: **PASS**, Slurm `COMPLETED 0:0`, elapsed 20 seconds on `itiger07`.

- Source identity: `sha256:d9815ff11485b4395afa9eee3cb29253e0f6524a9c7b7b1c77d3817ae7de9b6a`
- Source bundle: `sha256:0134b5d0f47b3de1cbf84ab5b256408392e5532a2a873368f9b4df9cf683360a`
- Runtime SIF: `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`
- Stage manifest: `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`
- Preflight result: `sha256:5dd8f723f87850ecef00590647e84a319383130597bd8b3a60abca7c1ec6250c`

The remote login-node preflight first verified all 168 source files and their
declared modes. The Slurm job then verified the Repo and NDNSF Python/native
closures, imported the complete overlay, and loaded all three pinned
Qwen3-0.6B stages on `cuda:0` of an NVIDIA RTX 5000 Ada Generation. Every stage
reported `cpuFallback=false`; expected and actual top token were both `8065`.

This is bounded single-node exact-SIF/CUDA environment evidence, not a
three-node distributed-inference result. Job 182488 is retained separately as
a pre-Apptainer source-mode transfer failure and contributes no CUDA evidence.
