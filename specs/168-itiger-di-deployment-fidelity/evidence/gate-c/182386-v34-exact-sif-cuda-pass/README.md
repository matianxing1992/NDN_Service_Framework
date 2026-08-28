# Gate C job 182386 - v34 exact-SIF CUDA PASS

- Slurm: `COMPLETED`, exit `0:0`, elapsed `00:01:49`, node `itiger07`.
- Source identity: `sha256:e090406547cd36ea426b7d6c63784741f60ddb6d2eea3449e7e8e60837686d4a`.
- Source bundle: `sha256:fe70b185eba5384143c0f9ce240aea0e4f4ec8712f01324033e83ddf3a955133`.
- Exact SIF: `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`.
- Qwen3-0.6B stage manifest: `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.
- Result digest: `sha256:2b96645315de1663adc2adf0eb39c53b8981e7f43d95ff490f53302fedf50deb`.

The job verified every source byte and mode, preserved both SIF native
extensions through the full copy-up overlay, and removed the ephemeral overlay
before returning to the batch wrapper. All three existing Qwen stages loaded,
warmed, and forwarded on `cuda:0`; CPU fallback was zero. Expected and actual
top token were both 8065.

Stage load/warmup/forward milliseconds and peak allocated bytes:

| Stage | Load | Warmup | Forward | Peak bytes |
|---:|---:|---:|---:|---:|
| 0 | 4650.570 | 463.875 | 50.725 | 603832320 |
| 1 | 800.518 | 11.247 | 9.472 | 292667392 |
| 2 | 1580.837 | 12.327 | 26.526 | 647075840 |

This is a one-node environment/overlay Gate C, not distributed-inference
acceptance. Three-node Gate E remains controlled by the separate Gate D audit.
