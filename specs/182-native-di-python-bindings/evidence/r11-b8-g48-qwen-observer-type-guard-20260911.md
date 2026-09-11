# R11-B8-G48 Qwen Native Observer Type Guard

## Scope and boundary

本批修复维护中的 Qwen native caller 对 observer payload 的类型校验缺口。
`decode_payload()` 可以成功解析 JSON array 或 scalar；此前 caller 随即调用
`.get()`，而 C++ observer 隔离边界会吞掉该异常，导致 terminal 通知仍可能让
full-generation caller 继续完成。修复在 caller 边界要求解码结果为 mapping，并
把拒绝原因留在既有 `native_stream_errors` 汇总中。

本批只改变 Python caller 的畸形事件拒绝，不改变 C++ wire、生成器、Provider
执行或任务状态。真实 Core/Provider、跨进程、maintained caller 全量迁移、no-Python
和 T016/T017 仍未关闭。

## Review and verification

| Lane | Result | Evidence |
| --- | --- | --- |
| production entry/callers | PASS | `llm_pipeline/user.py::_run_qwen_transformer_generation_sample` → `on_native_event`；非 mapping event 与 JSON array payload 均在 caller 边界记录错误 |
| implementation/wire | PASS | 仅增加 `isinstance(token_event, dict)` 门；terminal/event schema 和 C++ observer contract 未改 |
| test/harness/oracle | PASS | 原有 non-mapping event、legacy diagnostic rejection 与新增 JSON array payload regression 均通过 |
| build/source closure | PASS | Python-only；`py_compile` 通过，native ABI 和 Waf target 未改变 |
| migration/evidence | PARTIAL | G48 关闭一个 caller-edge fail-open；其余 maintained caller、legacy zero-use、no-Python、T015/T016/T017 保持原状态 |

官方 `$review-agent` 只读审查按 SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228` 执行；结果为
`No findings.` 本批改动范围为新增类型门和对应回归用例，未发现新的可执行缺陷。

实际检查：

```text
python3 -m py_compile examples/python/NDNSF-DistributedInference/llm_pipeline/user.py
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:pythonWrapper:examples/python/NDNSF-DistributedInference/llm_pipeline \
  python3 -m pytest -q \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec180_qwen_entrypoint.py
36 passed
git diff --check: PASS
```

## Closure decision

`CLOSED_FOR_VALIDATION` for the Qwen native observer type gate only. This does not
promote R11-B8 maintained callers or any parent task. The next production exit remains
one current-config maintained caller result through the independent C++ requester/Core/
Provider path, followed by no-Python and qualification gates.
