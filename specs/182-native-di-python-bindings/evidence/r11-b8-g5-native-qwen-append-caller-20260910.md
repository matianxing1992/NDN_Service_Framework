# R11-B8-G5 Native Qwen Conversation Caller

## Scope

本批次只验证 Qwen native-config caller 的 continuation DTO 接线。C++ coordinator
仍拥有 checkpoint、journal、role binding 和 commit；Python 只读取已提交的 opaque
wire，转换有限的 token metadata，并把结果 bytes 转发给上层。

## Changed boundary

- `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py`
  - `FULL_CONTEXT` 构造 typed `NativeConversationContinuation`。
  - `APPEND_DELTA` 从已认证 checkpoint wire 读取 epoch、service、plan digest 和
    role receipts，并拒绝 malformed/epoch-mismatch/fallback 输入。
  - native canonical input 去除 frozen generation oracle suffix，只保留 committed
    parent transcript 加当前 delta。
  - native handle 成功后只导出 `conversation_checkpoint` bytes。
  - Spec175 G6C 的 unavailable-role control 尚未有 native Provider 控制，native
    campaign 因此 fail-closed，避免混用 Python coordinator。
- `llm_pipeline_lib.py` adds an optional opaque checkpoint field that is omitted from
  the public JSON projection.
- Python tests cover FULL_CONTEXT mapping, authenticated APPEND_DELTA metadata,
  oracle-suffix removal, checkpoint forwarding, and the native observer boundary.

## Review

按 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 逐项检查完整 diff、调用方、
状态/所有权和兼容边界；未发现新增的 P0--P3 缺陷。native G6C unavailable-role
control remains an explicit next batch boundary rather than a Python fallback。

## Verification

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  /usr/bin/python3 -m pytest -q \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_spec182_legacy_exclusion.py
29 passed in 0.82s

./build-nac182/unit-tests --run_test='Spec182*' --report_level=short
256 test cases out of 256 passed
7077 assertions out of 7077 passed

Raw selector log: `.codex-tmp/spec182-r15-native-append-20260910/cpp-spec182-selector-r2.log`.

py_compile: PASS
git diff --check: PASS
```

## Qualification boundary

本记录是 caller mapping 的 `CLOSED_FOR_VALIDATION`，不是 Spec182 或 G6C 的协议资格
通过。尚未证明真实 Provider 第二轮 `APPEND_DELTA`、unavailable-role control、其余
15 个 maintained callers、legacy zero-use、no-Python closure 或 T016/T017。
