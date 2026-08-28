# Gate C v30 Exact-SIF CUDA Pass

- Slurm job: `182384`
- State/exit: `COMPLETED`, `0:0`
- Elapsed: 80 seconds
- Allocation: one node (`itiger07`), one RTX 5000, 4 CPUs, 16 GiB RAM
- Candidate source digest:
  `sha256:9cc623472357d0f0f52e2f6fc669f82eb072b2e6dd5c5b63f88e3bde55b1dae4`
- Source bundle digest:
  `sha256:c4428cf40dbf7f3da74b050d2418a1d72c82476a69ae37cda367f4243cd333f1`
- Exact SIF digest:
  `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`
- Qwen3-0.6B stage manifest digest:
  `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`
- Runtime: Torch `2.6.0+cu124`, CUDA `12.4`
- GPU: NVIDIA RTX 5000 Ada Generation, 33,826,537,472 bytes

The preflight verified the exact SIF, all 125 source/package-data files, the
stage manifest, and all three reused stage artifact hashes. It loaded, warmed,
and executed the three stages sequentially on `cuda:0`. Every stage reported
`cpuFallback=false`. The measured first token was `8065`, exactly matching the
frozen reference.

| Stage | Load ms | Warmup ms | Forward ms | Peak allocated bytes |
|---|---:|---:|---:|---:|
| 0 | 4657.823 | 462.859 | 49.964 | 603,832,320 |
| 1 | 789.514 | 11.304 | 9.648 | 292,667,392 |
| 2 | 1553.522 | 12.367 | 26.828 | 647,075,840 |

This is bounded single-node exact-SIF/CUDA environment evidence. It is not a
three-node distributed-inference result and does not satisfy Gate E.

Retained raw evidence:

- `preflight-result.json` SHA-256
  `1223ee3b915116ca89d577133d7808a6ab565a5c643bebe595c22e8e9c2fdd51`
- `slurm.out` SHA-256
  `f167d2fcd11385702506e16aa95ff3a1f5826fa4ac3750f5628e54a1d1bb9007`
- `slurm.err` is empty, SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
