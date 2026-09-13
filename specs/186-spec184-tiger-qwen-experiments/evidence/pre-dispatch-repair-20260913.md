# Spec186 pre-dispatch identity repair receipt

**Captured:** 2026-09-13 (America/Chicago)
**Scope:** all eight Spec186 candidates after collector digest repair

The profiles were reloaded and their candidate manifests rebuilt after binding
the current `spec186_candidate.py` digest
`b9bb5f886fc2ee39ab50f7d85814974c595d866ef1962164968165dad9697291` in both
collector fields. Each pre-dispatch invocation remained side-effect free.

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
