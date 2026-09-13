# Spec186 r5 pre-dispatch receipt

All eight profiles were regenerated after binding the corrected r5 application
bundle. The offline gate remained side-effect free (`ssh=0`, `rsync=0`,
`staging=0`, `sbatch=0` for every profile).

| Profile | Candidate digest | First boundary |
| --- | --- | --- |
| `spec184-qwen06b-minindn-cpu` | `48f7e327f0cd32d417723c9fdde65a5db6c0ead9c03ccd58bd50a605b7feee2c` | missing base SIF and Qwen3 model/tokenizer/stage manifest |
| `spec184-qwen06b-tiger-experimental` | `82b0d71ce3a7f844a37ff1d57b0796f0af0c991dbf7dfa415555cc902ee934ae` | Tiger-local visibility plus missing base SIF and Qwen3 tuple |
| `spec184-yolo-minindn-negative` | `f3d779487a8a445578f1b398ae86d4485995dd1ccf1bee6b4f0bd8cf0b829016` | missing base SIF |
| `spec184-yolo-minindn-normal` | `40965636ed4dcb61c5f71c051db44f1717c9db5f834b6afe81284eb3f2315028` | missing base SIF |
| `spec184-yolo-tiger-single-gpu` | `2f48ef2acd31898e91d82204a1273041e4958f3564349370a9d07d368d9c32be` | Tiger-local visibility plus missing base SIF/model |
| `spec184-yolo-tiger-two-node-negative` | `e976d3e24f15d2add9da27bdb384019e2ad66c269336b40f72368f34b7f26dfd` | Tiger-local visibility plus missing base SIF/model |
| `spec184-yolo-tiger-two-node-normal` | `630aec951caa37859f9a90d19586a6b7384efd6d76c6ccc55854983e36568ad0` | Tiger-local visibility plus missing base SIF/model |
| `spec184-yolo-tiger-two-node-reuse` | `ccd15621cdd6d4f0ff83ff940ff1452bc4a4238da0a49e25839cf9e53cef83ab` | Tiger-local visibility plus missing base SIF/model |

The local YOLO entrypoint and extension resolve from
`.codex-tmp/spec186-app-bundle-r5`; the remote Tiger copy is independently
verified at `/project/tma1/ndnsf-di/apps/spec186/spec186-app-bundle-r5` with
the same tree digest `687610de859155449c51ec2ba4bb7b57c77614cbf0a53f106bb65152f8c07129`.
The missing source-sealed base SIF therefore remains the controlling gate; no
runtime result is inferred from these receipts.
