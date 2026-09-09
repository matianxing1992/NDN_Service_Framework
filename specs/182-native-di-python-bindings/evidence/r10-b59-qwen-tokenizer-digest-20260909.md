# R10-B59 Native-config Qwen tokenizer digest binding — 2026-09-09

## Outcome

本批修复了 native-config Qwen 请求在进入 Core 前使用空 tokenizer digest 的实际阻断。
`request.tokenizer_digest` 现在是 `TOKEN_STREAMING` 配置的必需 operator-pinned 字段，
由 `APPClient.configure_native_requester_from_config` 校验并绑定到 client；维护中的
`_native_qwen_request` 只从该 native client 读取摘要。automatic planner 的临时模型状态
仅保留给未配置 native requester 的兼容路径，不能填充 native-config 请求。

## Five-lane record

- **production entry/callers**：`llm_pipeline/user.py::_native_qwen_request` →
  `APPClient.native_tokenizer_digest` → `request_native_reference`。
- **implementation/wire**：`APPClient.configure_native_requester_from_config` 要求
  `sha256:` 加 64 位小写十六进制；`TOKEN_STREAMING` 缺失摘要直接 fail-closed；摘要同时
  写入 typed `NativeGenerationExecutionContractV1` 和 application options JSON。
- **test/harness/oracle**：使用真实已构建 `_ndnsf` pybind DTO 执行 Qwen helper，检查 typed
  generation digest、application `tokenizerDigest` 和 native-config 缺失摘要边界；使用
  fake transport 只终止请求，不替代 native DTO 构造。
- **build/source closure**：本批只修改 Python 与配置契约，未改变 C++/extension ABI；测试
  使用现有候选 `_ndnsf` extension，Python 语法和 focused suite 通过。
- **migration/evidence**：修复 T013-D/F 的 native-config 前置阻断；真实 Provider、跨进程
  stream、conversation owner、YOLO/Qwen 端到端 caller 迁移和 T016 仍未完成。

## Validation

```text
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_spec182_native_bindings.py
13 tests passed

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec182_legacy_exclusion.py \
    tests/python/test_spec182_native_bindings.py
21 passed

python3 -m py_compile \
  NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py \
  examples/python/NDNSF-DistributedInference/llm_pipeline/user.py \
  tests/python/test_spec182_native_bindings.py
git diff --check
```

上述检查均 exit 0。没有运行网络请求或 T016 qualification；因此本批状态为
`STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`，不是
`QUALIFICATION_PASS`。

## Review boundary

静态审查确认摘要来源不再依赖 `model.semantics_digest` 或 automatic planner 的临时
`_automatic_tokenizer_digest`。配置契约与实现保持一致；没有修改其他会话遗留的设计文档
未提交改动。
