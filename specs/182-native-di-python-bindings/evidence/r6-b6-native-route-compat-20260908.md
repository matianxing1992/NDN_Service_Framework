# R6-B6 Native Route Compatibility Forwarding

日期：2026-09-08。此批次修复 public `InferenceClient` facade 在没有 conversation
owner 时错误传递第三个 `None` 参数的问题，并保留显式 owner 的三参数路径。修复只涉及
Python facade 和兼容性测试，不改变 native ABI、Core 运行时或资格边界。

## Batch allocation and change

- production entry/callers：`InferenceClient.configure_native_requester` public facade，
  以及其 canonical `APPClient` core target。
- implementation/wire：当 `conversations is None` 时调用 Core 的原有两参数契约；只有
  提供 opaque conversation owner 时才转发第三个参数。
- test/harness/oracle：`test_public_inference_client_exposes_explicit_native_route` 保持
  两参数兼容性；新增
  `test_public_inference_client_forwards_conversation_owner` 覆盖三参数 native continuation。
- build/source closure：Python source only；没有 C++ 头文件、扩展或 shared library 变化，
  因而 native rebuild 不适用。
- migration/evidence：真实 native requester/provider parity、maintained caller migration、
  legacy zero-use、跨进程行为和 T016 资格仍由原有任务负责。

## Review trace

- Skill：`/home/tianxing/.codex/skills/review-agent/SKILL.md`
- SHA-256：`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Scope：public facade diff、canonical Core call path、Python binding signature and focused
  compatibility tests。
- Findings：`No findings.` The optional argument is now forwarded only when present; the
  existing native route remains explicit and has no planner fallback.

## Validation

```text
/usr/bin/python3 -m py_compile \
  NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py
# exit 0

/usr/bin/python3 -m pytest -q \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_closure.py
# 47 passed in 1.17s

git diff --check
# exit 0
```

No C++ build, network request, MiniNDN run, or qualification campaign was run. This batch
does not change T012-B/T013/T016 status.

## Result and closure

`STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `DONE` for this bounded
compatibility batch; not `QUALIFICATION_PASS` for Spec182.

Closure decision: `CLOSED_FOR_VALIDATION` for optional-argument forwarding. The remaining
production requester/provider, stream/conversation parity, legacy retirement and external
qualification exits stay open in their owning tasks.
