# Gate C job 182392 - v37 exact-SIF CUDA PASS

- Slurm: `COMPLETED 0:0`, elapsed `00:01:35`, node `itiger07`.
- Source: `sha256:315c94fd0c75f8491c19e7f01f0811620a4992c9544f185bdec38bd5e6f2a6f4`.
- Source bundle: `sha256:20248e1b4de0397983f6fa4a19de9740fcfc917e578a423c4bc0fe3d337c898d`.
- Exact SIF: `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`.
- Qwen3-0.6B stage manifest: `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.
- Result: `sha256:e7d04d380e0914ec6c4aa856aa4a7ddb493d1a275de8f7cd52dc85b6414c3781`.

All three stages used `cuda:0`, reported zero CPU fallback, and produced
expected and actual top token 8065. This bounded one-node CUDA admission also
binds the overlay-order and cross-rank-abort repair source; it is not
three-node distributed-inference evidence.
