# R11-B8-G2 Native Generic Stream Facade

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for this bounded sub-batch
**Parent**: R11-B8 / T012-B / T013-A

## Scope

本批在 G1 generic unary 出口之后接通 canonical `APPClient.request_streaming` 的
native compatibility seam。配置完整的 native requester 直接接收 bounded
`NativeStreamRequestOptions`，C++ handle 负责请求状态、事件观察、终态和结果；
Python 只负责输入/选项转换、回调适配、取消和历史结果句柄形状。Python
conversation、TOKEN_STREAMING 的 adapter-owned generation contract、其余
maintained callers、legacy retirement 与 no-Python qualification 不在本批范围内。

## Changed Files

- `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`
  增加 `NativeStreamingHandle` 以及 native generic stream route；保留模型、任务、
  schema、options identity 检查，拒绝 planner strategy/constraint 和未配置的
  Python conversation。C++ terminal observer 的空 marker 通过 native result
  读取最终 payload；非成功终态进入 `on_error`，不伪造 `on_complete`。
- `tests/python/test_ndnsf_di_app_sdk_compatibility.py`
  覆盖 native stream 的输入与 stream-option 转换、事件/完成回调、planner
  non-fallback、空 terminal marker 的最终结果读取、失败终态和 conversation
  fail-closed 边界。
- `contracts/native-first-execution.md`
  登记 R11-B8-G2 独立出口及其剩余边界。

## Static Review

按 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 的只读 defect-first
流程检查完整 WIP diff、native `observe`/terminal 实现、结果/取消寿命、线程锁、
callback 异常隔离、stream option owner 边界及 planner 旧路径。发现并修复了两项
接线问题：初始补丁误插入 `APPClient.request`，以及 native terminal observer
只携带空 marker、会把失败终态当作完成。修复后没有剩余 actionable finding。

## Validation

- `python3 -m py_compile NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`：PASS。
- `git diff --check`：PASS。
- `PYTHONPATH=NDNSF-DistributedInference python3 -m pytest tests/python/test_ndnsf_di_app_sdk_compatibility.py tests/python/test_spec180_generic_request_api.py -q`：41 tests PASS；其中 23 个 compatibility tests 覆盖本批 stream seam。
- C++ primary unit regression（本批无 C++ production source 变化）：
  `./build-nac182/unit-tests --run_test='Spec182*' --log_level=test_suite`：256 cases、7077 assertions、exit 0；复用匹配二进制，raw log：[spec182-r13-g2-unit.log](../../../.codex-tmp/spec182-r13-g2-unit.log)。
- C++ native Core/Provider integration regression：
  `./build-nac182/integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182*' --log_level=test_suite`：7 cases、`*** No errors detected`；raw log：[spec182-r13-g2-integration.log](../../../.codex-tmp/spec182-r13-g2-integration.log)。

这些检查证明 C++ native owner 回归和 wrapper 接线通过；它们不证明跨进程 caller
迁移、TOKEN_STREAMING adapter generation、conversation、legacy zero-use、
no-Python 或 T016/T017 资格。

## Batch Decision

该子批形成独立、可验证的 native generic stream compatibility 出口，关闭本批
验证。下一批应选择一个真实 maintained caller group，沿已经通过的 C++ stream
行为接入并记录旧路径零使用；不能把所有 caller、conversation 和 no-Python
closure 重新合并为一次大批。R11-B8 父卡仍为 `PARTIAL`，R11-B9 仍为
`NOT_STARTED`。
