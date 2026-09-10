# R11-B8-G1 Native Generic Request Facade

**Date**: 2026-09-10  
**Status**: `CLOSED_FOR_VALIDATION` for this bounded sub-batch  
**Parent**: R11-B8 / T012-B / T013-A  

## Scope

本批只接通 canonical `APPClient.request_task` 与 public
`InferenceClient.request_task` 的 generic native compatibility seam。配置完整的
native requester 现在直接进入 `_ndnsf.NativeInferenceClient`；Python 仅完成
`ApplicationInput` 的 inline/`REPO_REF` 转换、参数身份核对和结果句柄适配。
自动 planner 不再作为该入口的隐式 fallback。stream、conversation、Provider
retirement、其余 maintained callers 和 no-Python qualification 不在本批范围内。

## Changed Files

- `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`
  增加 `NativeRequestHandle`、generic native route、model/task/schema/options
  identity checks，并在 public facade 转发该 route。
- `tests/python/test_ndnsf_di_app_sdk_compatibility.py`
  增加 inline native route、result-handle 形状、planner 不回退及模型身份拒绝
  检查。

## Static Review

按 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 的只读 defect-first
流程检查完整 diff、CodeGraph caller path、native owner boundary、输入/任务/模型
身份、strategy 处理、结果/取消寿命和现有调用方。初次检查发现 native route 会
忽略 Python strategy 以及未核对完整 task/schema identity；两项均在批末前修复，
修复后无剩余 actionable finding。该审查只覆盖本批代码，不代表 R11-B8 全量迁移
或 T013/T016 资格通过。

## Validation

- `git diff --check`：PASS。
- `python3 -m py_compile`（changed Python files）：PASS。
- `PYTHONPATH=NDNSF-DistributedInference python3 -m pytest tests/python/test_ndnsf_di_app_sdk_compatibility.py tests/python/test_spec180_generic_request_api.py -q`：38 tests PASS。
- C++ primary unit regression：
  `./build-nac182/unit-tests --run_test='Spec182*' --log_level=test_suite`：256 cases、7077 assertions、exit 0；raw log：[spec182-r12-native-facade-unit.log](../../../.codex-tmp/spec182-r12-native-facade-unit.log)。
- C++ native Core/Provider integration regression：
  `./build-nac182/integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182*' --log_level=test_suite`：7 cases、`*** No errors detected`；raw log：[spec182-r12-native-facade-integration.log](../../../.codex-tmp/spec182-r12-native-facade-integration.log)。

本批没有 C++ production source 变化，故复用已构建的 matching native binary 并
重新运行 C++ selectors；该结果证明 native owner 回归通过，不证明 Python wrapper
已经提供跨进程或完整 caller qualification。

## Batch Decision

该子批次形成稳定 generic unary compatibility 出口，关闭本批验证。下一子批次应
单独处理 native stream handle/callback 状态，因为它有不同的事件、终态、conversation
和 cancellation 契约；不能为了减少构建把 stream、legacy retirement 或 no-Python
合入本批。R11-B8 仍保留 15 个未完成 caller/实际入口行为证据，R11-B9 仍未开始。

