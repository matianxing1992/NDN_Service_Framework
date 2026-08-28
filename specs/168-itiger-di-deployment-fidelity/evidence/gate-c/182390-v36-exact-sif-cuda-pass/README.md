# Gate C job 182390 - v36 exact-SIF CUDA PASS

- Slurm: `COMPLETED 0:0`, elapsed `00:01:55`, node `itiger07`.
- Source: `sha256:9b936cf5bde4b1dd7f0d55cfd885050a8f9ce07d159ee5a5ea16fb7956e31ecb`.
- Source bundle: `sha256:c71ad8c936c5089862e467e89a28b628f45fe2027ccd20004c7722469a33c3b3`.
- Exact SIF: `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`.
- Qwen3-0.6B stage manifest: `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.
- Result: `sha256:db717e2ca43318059b14e1d12c7f123c6654e4e82b0a8acee9c5a6ced78486c6`.

The complete 165-file closure and source modes passed before execution. All
three Qwen3-0.6B stages ran sequentially on one RTX 5000 under the exact SIF,
used `cuda:0`, reported zero CPU fallback, and produced expected and actual top
token 8065. The Python overlay was removed before the job passed.

This is a bounded one-node CUDA admission gate. It is not three-node
distributed-inference evidence.
