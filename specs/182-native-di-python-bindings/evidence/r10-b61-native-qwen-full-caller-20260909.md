# R10-B61 Native-config Qwen full-generation caller — 2026-09-09

## Outcome

本批把 native-config Qwen 的维护 full-generation caller 真正执行到结果校验边界。
修复了 native response 使用未定义 `decode` 的问题，并将协议字段 `tokenIds` 映射为
`run_full_qwen_generation` 要求的 `generatedTokenIds`。响应 schema、token 列表、observer
事件和 terminal 通知现在在 caller 边界统一检查。

## Five-lane record

- **production entry/callers**：`_run_qwen_transformer_generation_sample` 的
  native-config full-generation 分支 → `_native_qwen_request` → `request_native_reference`。
- **implementation/wire**：使用已有 `decode_payload`；要求 `NDNSF-DI-FINAL-V1` 与
  `tokenIds`，再生成 caller 内部的 `generatedTokenIds`；operator-pinned tokenizer digest
  仍来自 native requester configuration，不由 planner 补值。
- **test/harness/oracle**：测试直接执行维护中的 full-generation helper，使用真实 `_ndnsf`
  generation/stream DTO，触发 `GenerationTokenEventV1` observer 和 terminal 回调，并核对
  结果 token、文本、EOS、typed digest 与 application `tokenizerDigest`。请求传输保留为
  明确的测试 seam，因此不把本批称为跨进程 Provider qualification。
- **build/source closure**：只修改 Python caller 与 Python test，未改变 C++/extension ABI；
  `py_compile` 与 `git diff --check` 通过。
- **migration/evidence**：关闭 native-config Qwen caller 的解码/字段接线缺口；真实
  requester → Core → Provider worker/process、跨进程 stream、conversation owner、YOLO
  migration 与 T016 qualification 仍未完成。

## Validation

```text
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec182_native_bindings.py \
    tests/python/test_spec180_qwen_entrypoint.py
20 passed

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec182_legacy_exclusion.py
8 passed

python3 -m py_compile \
  examples/python/NDNSF-DistributedInference/llm_pipeline/user.py \
  tests/python/test_spec182_native_bindings.py
git diff --check
```

上述检查均 exit 0。没有运行网络请求、MiniNDN 或 T016 qualification；本批状态为
`STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`，不是
`QUALIFICATION_PASS`。
