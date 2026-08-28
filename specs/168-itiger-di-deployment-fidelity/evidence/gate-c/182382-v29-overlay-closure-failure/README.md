# Gate C v29 Exact-SIF Overlay Closure Failure

- Slurm job: `182382`
- Node/GPU class: `itiger07`, one RTX 5000 allocation
- State/exit: `FAILED`, `1:0`
- Elapsed: 30 seconds
- Candidate source digest:
  `sha256:213a425256e599c7b76b58b6b99dce8845f961e198a018e727d8561376bb9851`
- Source bundle digest:
  `sha256:495051fb7378c61fc84cbfc6e16e0b5bb5383d52cfd49f424cbe73c207d8f545`
- Exact SIF digest:
  `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`
- Qwen3-0.6B stage manifest digest:
  `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`

The job verified the SIF, source bundle, stage manifest, and all three existing
stage artifact hashes. It then failed before CUDA/model loading because the
bundle overlaid current `core/__init__.py` on the SIF while leaving the SIF's
older `core/execution.py` in place. The current initializer imports
`new_legacy_rollback_plan`, which the older execution module does not export.

This is an exact-container source-overlay closure failure. It is not a model,
Repository, CUDA, memory, or inference failure. The v29 identity remains failed
and immutable. The replacement identity must bind and overlay the complete DI
Python package, pass the equivalent overlay import smoke locally, and rerun
Gate B before one new Gate C qualification.

Retained raw evidence:

- `preflight-result.json` SHA-256
  `ab0c32f3097b01e50e3e07f6157b7153f6b4697baa9a41c183cffe86a3d4aeea`
- `slurm.out` SHA-256
  `fde0c94d40fc1ce47c831d0bda5147f3e85a3a011bcb927ba64aecb9764b2e9c`
- `slurm.err` is empty, SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
