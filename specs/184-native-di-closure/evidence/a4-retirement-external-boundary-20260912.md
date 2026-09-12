# Spec184 T007 A4 Retirement and External Model Boundary

**Date**: 2026-09-12
**Status**: `PARTIAL` / local evidence refreshed; no qualification promotion

## Scope

本记录把当前本机能力、实验机只读预检和 A4 旧路径退出检查放在同一个证据边界内。
它不修改产品 API、模型选择器、兼容入口或资格判据，也不启动 SIF、Slurm、模型传输或
MiniNDN 实验。

## Local model boundary

用户确认本机只能运行 `Qwen3-0.6B`，不能运行 `Qwen/Qwen3.6-27B`。因此本机的
`Qwen3-0.6B` 只能用于 C++ smoke/ABI 边界，不能关闭 T007-A3。

同一 `build-spec184-b6-candidate-tests` 树的 C++ 验证已经通过：

* `integration-tests` 构建 exit `0`，耗时 46.811 秒；
* `di-native-assembly-worker` 与五个故障注入 worker 在同一树构建成功；
* 完整 `integration-tests` exit `0`，`*** No errors detected`；
* 完整 `unit-tests` 为 1034/1034 cases、71072/71072 assertions；
* Qwen 原生配置流式、会话续接及相关 C++ selector 通过。

这些 selector 使用仓库内源绑定的小型 Qwen ONNX fixture 和 CPU runtime contract，
没有加载实际 Qwen3-0.6B 权重，更没有执行 27B。因此该结果只关闭同树 C++ build/test
边界，不改变 A3 的模型资格状态。详细构建和运行日志见
[T007 model capability evidence](t007-model-capability-20260912.md)。

## External read-only preflight

对 `itiger` 的检查仅为只读，原始记录如下：

* SSH 用户 `tma1` 可用，`/usr/bin/sbatch`、`/usr/bin/apptainer` 和
  `/usr/bin/singularity` 可见；登录节点没有 `nvidia-smi`；
* 项目存储找到 `Qwen/Qwen3-0.6B` revision
  `e6de91484c29aa9480d55605af694f39b081c455`，权重 1,503,300,328 bytes，
  `model.safetensors` SHA-256 为
  `f47f71177f32bcd101b7573ec9171e6a57f4f4d31148d38e382306f42996874b`；
* 后续只读扫描发现一个 `Qwen/Qwen3.6-27B` Hugging Face cache 快照，但它不是可运行
  bundle：快照只有 9 个文件，其中仅有 `model-00001-of-00015.safetensors`，大小
  `3,968,861,352` bytes；
* cache tree 元数据列出 15 个权重分片、总大小 `55,563,006,400` bytes，但当前快照只
  有 `1/15`，其余 14 个分片缺失；没有 `refs/main`，也没有 Spec184 当前 candidate；
* 该 cache 的 `config.json` 标记 `model_type=qwen3_5`、
  `architectures=[Qwen3_5ForConditionalGeneration]`，只能作为不完整的外部缓存线索，
  不能作为 Spec184 的模型身份、运行时或资格证据；
* 没有开始外部模型 staging、SIF 构建、Slurm 提交或资格运行。

只读预检的原始日志及摘要哈希为：

| Record | SHA-256 |
| --- | --- |
| `.codex-tmp/spec184-external-readonly-preflight-20260912.log` | `289f5df52a634533cd63c47ade23a7100be75679c0095d13a856470bea3806f3` |
| `.codex-tmp/spec184-external-model-inventory-20260912.log` | `8344e0de98dc3884e783e3a11f7b75e054c295460a53c4e7da2d9991e7957834` |
| `.codex-tmp/spec184-external-model-weights-20260912.log` | `897b0bf6310d31f6b20f69733cd5af5199596508406131633721da9c58dddcb4` |
| `.codex-tmp/spec184-external-model-cache-inventory-20260912.log` | `66fb5bdb505302bed5322f316d6c52cafd2cbbe09953a3210d12a9c28fca52be` |

这证明的是外部可达性和当前模型库存边界，不是协议失败，也不证明 27B 已可执行。A3
仍需实验 owner 提供完整的 15 分片模型、tokenizer、CUDA runtime、三阶段 manifest、
候选身份和结果证据。上述快照路径为
`/project/tma1/ndnsf-di/cache/qwen36-27b-huggingface/models--Qwen--Qwen3.6-27B/`
下的 revision `6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`；本次检查只执行目录、文件大小
和小型 JSON 元数据读取，没有读取权重内容、修改缓存或提交作业。

## Legacy retirement boundary

聚焦 Python 兼容/路由检查通过：

```text
python3 -m pytest -q tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_bindings.py
30 passed in 1.00s
```

日志为 `.codex-tmp/spec184-a4-python-retirement-20260912.log`，SHA-256 为
`b9b8c8afccd8828baaf4f93a477f182cd930d50f821c1ff22f9f697c316a2632`。这些检查证明
默认 native route、binding 边界和兼容清单约束仍然可观察；它们不证明旧实现已经可以删除。

当前 Spec182 compatibility manifest 有 344 个 entry，全部为 `removalEligible=false`：
其中 174 个为 `RETAINED_UNTIL_MIGRATION`，128 个为 `RETAINED_OR_OFFLINE`，42 个为
`PLANNED_NATIVE`；`externalUseStatus` 为 324 个 `repository_callers_found` 和 20 个
`external_use_unknown`。清单 SHA-256 为
`31eadb67b0f30748ce4a127582e7648e8cd614c391a910476418a961c368bbfd`。

因此 A4 的 Python retirement 仍为 `PARTIAL`：必须先完成 caller/外部使用核对、默认
不可达证明和删除/构建注册回归，才能删除实现。I05 的 trace budget 边界仍按契约记为
`UNQUALIFIED`，不能改写成业务拒绝 `PASS`。

## Disposition

| Gate | Status | Boundary |
| --- | --- | --- |
| `T007-A3 Qwen3.6-27B` | `WAITING_EXTERNAL_INPUT` | 本机只能运行 0.6B；实验机只有不完整的 1/15 分片 cache，没有可运行的 27B bundle |
| Local Qwen3-0.6B | `AVAILABLE_AS_SMOKE_ONLY` | 可作 C++ smoke/ABI fixture，不是 A3 qualification |
| `T007-A4 inherited negative/retirement` | `PARTIAL` | focused checks 通过；兼容入口、I05 和外部 owner rows 仍开放 |
| `T008 native development handoff` | `BLOCKED_BY_T007` | 等 T007 完整资格和显式 external transfer |

## Next action

本机继续只关闭有当前 C++ selector 和完整 candidate-bound evidence 的 A4 行；不再尝试
本机 27B。实验 owner 补齐 15 分片 bundle、运行时和候选 manifest 后，沿现有 candidate
receipt 和 transfer matrix 执行 A3；在此之前不得把 0.6B smoke、ONNX fixture、单分片
cache 或 full C++ test 记为 27B qualification。
