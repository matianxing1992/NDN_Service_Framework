# R5-B11 Legacy Reachability Audit

日期：2026-09-08。此批次只更新迁移清单并核对旧 owner 的实际消费者，不删除 Python
runtime，也不运行网络资格。

## Inventory Result

`checklists/build_api_migration_manifest.py` 从当前源码重新生成
`contracts/compatibility-manifest.json`，输出 344 个 entries（explicit SDK 277、dynamic
SDK 67）。本次在流程 checkpoint `436dc449` 后重新生成，`sourceCommit` 为
`436dc4493c8c443eaae6352b097e2e17698e9be4`。
以下四个 T013-B 目标路径的 entries 均仍保留（状态为
`RETAINED_UNTIL_MIGRATION` 或 `PLANNED_NATIVE`），且 `removalEligible=false`：

- `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`
- `NDNSF-DistributedInference/ndnsf_distributed_inference/runtime_v1.py`
- `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/facades.py`
- `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`

清单同时标出 repository callers 或 `external_use_unknown`。因此当前证据不支持删除任何
一个旧 owner；R5-B10 的 YOLO native route 只是新增显式入口，尚未关闭旧 ACK-driven、
offline oracle、Provider 或 planner consumer。

## Review trace

- Skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Baseline: `d4e224fb` (R5-B10 checkpoint)
- Scope: generated manifest diff, four legacy target paths, maintained YOLO/Qwen callers,
  `test_spec182_legacy_exclusion.py`, and removal eligibility fields
- Finding: old implementations remain reachable; deletion is correctly refused. No other
  actionable finding in this audit-only batch.

## Validation

```text
timeout 120s python3 specs/182-native-di-python-bindings/checklists/build_api_migration_manifest.py  # exit 0, entries=344, elapsed=32.74s
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference python3 -m pytest -q \
  tests/python/test_spec182_legacy_exclusion.py                                                    # 6 passed
python3 specs/182-native-di-python-bindings/checklists/validate_design.py                           # ok=true
git diff --check                                                                                   # exit 0
```

本批次没有 native source/header 变化，因此无 C++ 或 extension build；manifest 的 source
identity 对应本批生成前的 `436dc449` source baseline；生成耗时已记录，不能将本清单
提交本身再写入 source identity。

## Result

`STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not
`QUALIFICATION_PASS`。T013-B 仍需所有 maintained callers 的 native route、默认 import/调用
图证明和最终 T016 no-Python 运行后才能考虑删除。
