# Spec186 pre-dispatch identity repair receipt

**Captured:** 2026-09-13 (America/Chicago)
**Scope:** all eight Spec186 candidates after collector digest repair

The profiles were reloaded and their candidate manifests rebuilt after binding
the current `spec186_candidate.py` digest
`b9bb5f886fc2ee39ab50f7d85814974c595d866ef1962164968165dad9697291` in both
collector fields. Each pre-dispatch invocation remained side-effect free.
That receipt is historical: the static launcher-boundary repair changed the
collector digest to
`4db0d68ed22f235bdba8c56ea67e2c048e46e9fc15f56e08f0cb2f973994527d`, which is
now bound by all eight profiles.

| Profile | Candidate digest | First asset/runtime boundary | SSH/rsync/staging/sbatch |
| --- | --- | --- | --- |
| `spec184-qwen06b-minindn-cpu` | `9eebad186e7f467b6d14d22977b5f44352f7daba4c96cc06461f68d80f27a05b` | missing app bundle, base SIF, model, tokenizer, stage manifest; native entrypoint/import unavailable; model digests are placeholders | `0/0/0/0` |
| `spec184-qwen06b-tiger-experimental` | `2842a8ca93a98948256c52399639c3d37f3a00ef370b555793432094430b1c91` | same missing app/base/model tuple and native closure; placeholder model digests | `0/0/0/0` |
| `spec184-yolo-minindn-negative` | `cdd659241e040a66f1356c1a02b088cbbccd44656fc01d796323c84d6961278b` | missing app bundle and base SIF; native entrypoint/import unavailable | `0/0/0/0` |
| `spec184-yolo-minindn-normal` | `36ded62a71183d9b4c8ba33c1214a9c1d5c49803a165017436cdfaa6d583651a` | missing app bundle and base SIF; native entrypoint/import unavailable | `0/0/0/0` |
| `spec184-yolo-tiger-single-gpu` | `dbe6136a4291fbf6bba946271f3d9c069cbe57ed50ffea6504ad31ad2c6661e1` | missing app bundle, base SIF and model; native entrypoint/import unavailable | `0/0/0/0` |
| `spec184-yolo-tiger-two-node-negative` | `49f3a769c4c52e701b82075aa31e7d318e7da26234f23224fd4d3ffbea5200e0` | missing app bundle, base SIF and model; native entrypoint/import unavailable | `0/0/0/0` |
| `spec184-yolo-tiger-two-node-normal` | `e7aeb3e6e4d05fc4eb4ae31b59b9f2de9dcc1503aa7bbe9fbd05ea6446504c21` | missing app bundle, base SIF and model; native entrypoint/import unavailable | `0/0/0/0` |
| `spec184-yolo-tiger-two-node-reuse` | `8ebe4b3ea3d381fbf2e27de2f3b1c03022ed47d5e5f86b740e0fc847b9113183` | missing app bundle, base SIF and model; native entrypoint/import unavailable | `0/0/0/0` |

These are current boundary receipts, not runtime results. The previous
`pre-dispatch-boundary-20260912.md` remains immutable and records the earlier
stale collector hash. No candidate was submitted.

The executable-contract repair changed the profile-bound collector to
`sha256:4db0d68ed22f235bdba8c56ea67e2c048e46e9fc15f56e08f0cb2f973994527d` and
adds role/backend/GPU, endpoint, placeholder-resource and Qwen harness checks.
A subsequent role-fallback and scheduler cwd/HOME repair changed the current
profile-bound collector to
`sha256:847b5261065419bf6467136f9d9739898409bf2cdd05d9a6214644bc40582490`;
new receipts must be regenerated before any runtime result is considered.

## Current post-19 offline receipt

After the role-fallback, scheduler run-isolation and nested-manifest schema
repairs, all eight manifests were rebuilt against collector
`847b5261065419bf6467136f9d9739898409bf2cdd05d9a6214644bc40582490`. Each gate
returned `ok=false` for declared missing external inputs or the known harness
contract drift, with `ssh=0`, `rsync=0`, `staging=0`, and `sbatch=0`:

| Profile | Candidate digest | Failure count | First boundary |
| --- | --- | ---: | --- |
| `spec184-qwen06b-minindn-cpu` | `051b6cc348036e2d33365c9686229015b66f2c00b8be729ec7a2a3d1917c3bd4` | 13 | missing Qwen model/stage/tokenizer/base SIF; GGUF/llama.cpp versus ONNX harness |
| `spec184-qwen06b-tiger-experimental` | `fb103816927d146411dba89037e2ab293e46ade78ef22f4ea8731f5149924e87` | 19 | missing app/base/model tuple and native closure |
| `spec184-yolo-minindn-negative` | `5c0b0e23fce2f8bf311bd4c3812bf430b186f3bd8bb8e0a0d2ea14aff538408b` | 4 | missing base SIF; canonical package/environment and role service map undeclared; model family drift |
| `spec184-yolo-minindn-normal` | `de3201580a7b05234bd2507c34f311bdfff457a61d13a624de2a0f614e6aa354` | 4 | missing base SIF; canonical package/environment and role service map undeclared; model family drift |
| `spec184-yolo-tiger-single-gpu` | `659e77af17a8b2cfef977746448239b8cf6e11b83f966857e5902729492adab2` | 13 | missing app/base/model; GPU UUID/signature placeholders |
| `spec184-yolo-tiger-two-node-negative` | `dc9e5fb7426300d7c3d41060e00d2438fa3d9d2da2d787ff1b446fcf35ad86dc` | 13 | missing app/base/model; GPU UUID/signature placeholders |
| `spec184-yolo-tiger-two-node-normal` | `70a9712d6355936776ee8891613b87c82a0c588c95c1af82b718481f6cf2a2c1` | 13 | missing app/base/model; GPU UUID/signature placeholders |
| `spec184-yolo-tiger-two-node-reuse` | `9dbbf44acffacc7feee667838fa2a4d09d1b975f5d5eb6ca3c72459fb0d1f761` | 13 | missing app/base/model; GPU UUID/signature placeholders |
