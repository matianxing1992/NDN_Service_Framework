# B189-r152：Qwen3-0.6B Selection boundary

## Run identity

- 日期：2026-09-20（America/Chicago）。
- Run：`two-provider-global-r152-qwen06b-selection-fix`。
- Raw run：`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r152-qwen06b-selection-fix/`。
- Launch log：`.codex-tmp/spec189-qwen-two-provider-20260918/two-provider-global-r152-qwen06b-selection-fix-launch.log`。
- Candidate：`stage-manifest-qwen-v2.json`，SHA-256
  `4c90126bf974a91abeb555f57b25a786afcf68a11b45976f8a1d15ee1df9c097`。
- Build identity：`build-spec189-oracle/spec180-native-build.json`，SHA-256
  `71cefd68e6bdf9ccdb58bb389e00f188b604a16b732e127239884a985052886b`；运行使用安装态
  `/usr/local/bin` binaries，预检在复制 canonical initializer 前完成。

## 已通过的边界

本次真实 wired MiniNDN 已启动，Qwen3-0.6B 的两个 Provider 均完成：

1. `NDNSF_DI_NATIVE_PROVIDER_READY`，每个 Provider 仅激活一个 placement-bound stage；
2. `ACK_DECISION status=1`，两方均为 `ACCEPT_WITH_PREPARATION`，并报告相同的
   `model_digest=sha256:fb01465c11c3cce8aea1387fdfd6517f222e3b7223f8f4088a64990c4e59b9b3`；
3. `NDNSF_DI_GRANT_VERIFICATION` 的 `boundary=BEFORE_ASSEMBLY`，两方均为 `VERIFIED`。

Requester 路径记录为
`Runtime.open->User.prepare->PreparedModel.request`。本次停止前没有出现
Selection wire、`CACHE_HIT`、`ASSEMBLY_STARTED`、dependency fetch、runner ready、
execution 或 terminal response；Requester 最终因受控停止记录
`CANCELLED boundary=request`。因此本次不是模型 ONNX/KV/output 契约结果。

## 资源与缓存观察

`resource-samples.jsonl` 共 227 个样本：

- `availableBytes` 最低 `5040832512`（约 4.70 GiB）；
- `diskFreeBytes` 最低 `24318046208`（约 22.65 GiB）；
- `ownedSwapBytes` 峰值 `5255168`（约 5 MiB），低于 `268435456`（256 MiB）门限；
- supervisor：`boundary=CANCELLED`、`cleanup=PASS`、`remainingProcesses=[]`。

canonical graph、1,503,264,768-byte initializer 和 Repo material receipt 已发布到本次
requester 的 run-scoped Repo。stable root
`/var/tmp/ndnsf-di-native-artifacts/provider-62d4ad97f1c6464a2f1acf3506e93123d34e6b2eb9631fd894c11d2c5d9d8249/.staging/assembly-DEJI3b/root.json`
只留下 1,412-byte `ndnsf-di-canonical-model-manifest-v1` metadata，`state=ACTIVE`；其
目录没有 assembled `model.onnx` 或其他大文件，也没有 `CACHE_HIT`。这说明本次完成了
material-backed preparation，但没有越过 Selection 进入可复用 assembled artifact cache。

## 结论

状态保持 `PARTIAL` / `UNQUALIFIED`。首个生产边界是
`ACK/grant verification → Selection` 的 admission/stream liveness；不是
`ownedSwap` 超限、内存不足、磁盘不足、cache digest 错误或 Qwen adapter 缺失。
下一步先取得 Selection 的发布/消费证据，再验证同一 stable root 的 cold/warm cache
对照；在此之前不把该 staging metadata 计为 cache hit，也不把 Qwen 0.6B 计为完整推理
或资格 PASS。
