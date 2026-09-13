# Spec186 pre-dispatch runtime-input binding receipt

**Captured:** 2026-09-13 (America/Chicago)
**Scope:** all eight profiles after source/app identity binding

The profiles now bind the provider-repair source commit
`4751148375dad9149c7c185c9381d5734c733e13`, source seal
`597c44a97b34655dfb67b9fc3bff3693b844f5cc1f10624870554bdee8e658e2`, and the
content-addressed app bundle digest
`badf6a0afb36e43d02f7103cba36383bf8f0336e2310a0fd546c33223734734d`. Local
profiles use `.codex-tmp/spec186-app-bundle-r4`; Tiger profiles use the staged
project-storage directory. Candidate manifests were regenerated before each
pre-dispatch call.

| Profile | Candidate digest | First remaining boundary | Side effects (SSH/rsync/staging/sbatch) |
| --- | --- | --- | --- |
| `spec184-qwen06b-minindn-cpu` | `dc9dd8df848f4a3e8ab5575151145ea98383c4a6975bde9b1caba2c7d647da3e` | missing base SIF and Qwen3 model/tokenizer/stage manifest; native extension import uses no matching base | `0/0/0/0` |
| `spec184-qwen06b-tiger-experimental` | `e1390afe7255cbdc513e6db93c03c801b4135a98e1c222de6df6346746269e29` | remote app/base/model inputs are not visible from this local host; Qwen3 tuple remains absent | `0/0/0/0` |
| `spec184-yolo-minindn-negative` | `2d905440d60543d06c4a2bd33d16599c0ac674c62bc43a3d3cfe8318e513b9dc` | missing base SIF; local app entrypoint resolves, extension needs matching base libraries | `0/0/0/0` |
| `spec184-yolo-minindn-normal` | `c0aad0c7127cb8259a41992fb0a4c3adacaca737062e7275c00ecad8ef4c7c9a` | missing base SIF; local app entrypoint resolves, extension needs matching base libraries | `0/0/0/0` |
| `spec184-yolo-tiger-single-gpu` | `e849bc8fb2ed857fceb5afea674fbae6ebebb8b51c5f2572c3a624687145c046` | staged Tiger app/base/model paths are not visible from this local host | `0/0/0/0` |
| `spec184-yolo-tiger-two-node-negative` | `eac5776dd9861a2264aba225e561dab12330e6a8a11c0919bddd10e356ca8e15` | staged Tiger app/base/model paths are not visible from this local host | `0/0/0/0` |
| `spec184-yolo-tiger-two-node-normal` | `f9ee9c404334f49fdce7bb397f492bc7f6c021b8996993e4aaa4fb4b984595dd` | staged Tiger app/base/model paths are not visible from this local host | `0/0/0/0` |
| `spec184-yolo-tiger-two-node-reuse` | `4e35c2c27e533c9c9d0f3656c8edeb275e6915240a005df621a033d0c522e064` | staged Tiger app/base/model paths are not visible from this local host | `0/0/0/0` |

The local host visibility boundary for Tiger paths is expected; the staged
bundle was independently verified on Tiger storage. These receipts are still
not execution or qualification results. The earlier identity-repair receipt
is immutable and remains at `pre-dispatch-repair-20260913.md`.
