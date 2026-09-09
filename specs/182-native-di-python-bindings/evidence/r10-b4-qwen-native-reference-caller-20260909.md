# R10-B4 Qwen Native Reference Caller — 2026-09-09

## Scope and allocation

本批只迁移维护入口 `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py`
的 `_native_qwen_request`。每次 Qwen typed context bundle 仍由调用方生成，但现在先
通过 `APPClient.publish_application_input_reference` 发布为加密对象，再将返回的
journal-bound reference 交给 `APPClient.request_native_reference`。generation options、
native observer 和 request identity 保持不变；没有引入 Python planner、requester 解密或
conversation fallback。未配置 native conversation owner 时原有 fail-closed 检查仍在入口。

## Coverage matrix

| Lane | Evidence |
| --- | --- |
| `production entry/callers` | `_native_qwen_request` called by full-generation and token-diagnostic native branches; `main` selects it only with `--native-requester-config` |
| `implementation and wire` | one `publish_application_input_reference(SERVICE, bytes(payload), ...)` followed by `request_native_reference`; existing options and `on_event` are forwarded unchanged |
| `test/harness/oracle` | `test_maintained_qwen_native_route_uses_repository_reference`, existing explicit-native Qwen source selectors, and R10-B2 facade selectors |
| `build/source closure` | Qwen user module and `tests/python/test_spec182_legacy_exclusion.py`; R10-B2 Python facade and R10-B1 native ABI reused |
| `migration/evidence` | Qwen native generation/diagnostic helper reaches REPO_REF; automatic-planning, tiny-stream and legacy paths remain separate; real Provider fetch, cross-process stream/conversation behavior, and T016 remain open |

## Static review

The read-only review used `/home/tianxing/.codex/skills/review-agent/SKILL.md`
(SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`)
against the complete Qwen helper diff, its full-generation/token-step callers, and
the R10-B2 facade contract. It checked publication ordering, payload identity, options
and observer forwarding, conversation fail-closed behavior, and isolation from the
automatic planner and legacy routes. **No actionable findings.**

## Validation

```text
python3 -m py_compile llm_pipeline/user.py test_spec182_legacy_exclusion.py  exit=0
pytest test_spec182_legacy_exclusion.py test_ndnsf_di_app_sdk_compatibility.py  25 passed
source helper checks (publish-before-reference/no-inline/no-planner/fail-closed)  exit=0
git diff --check (scoped R10-B4 paths)                                   exit=0
validate_design.py                                                       ok=true
```

No C++ rebuild was needed because this batch changes only a maintained Python
caller and reuses the validated R10-B2 facade/R10-B1 native ABI. No network,
Provider, cross-process, MiniNDN or qualification run was attempted.

## Batch retrospective

- Static review found: no actionable defect; all five lanes and the full-generation
  plus token-diagnostic caller paths were covered.
- Compile/link found: no compile miss; Python syntax and design checks pass.
- Runtime/test found: 25 focused compatibility/legacy cases pass; source oracle
  confirms the helper publishes before reference submission and cannot call the
  inline native payload helper or automatic planner.
- Still unobserved: real encrypted fetch/decrypt, Provider execution, stream and
  conversation cross-process behavior, caller retirement, and T016 qualification.

`Closure decision`: `CLOSED_FOR_VALIDATION` for the local Qwen native caller
migration; `T004/T010/T013/T016` and final qualification remain `OPEN_FOR_NEXT_BATCH`.
