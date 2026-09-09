# R5-B8 Native Qwen Stream Callback Route

**Status**: `STATIC_PASS; FOCUSED_BEHAVIOR_PASS; PARTIAL`

本批只处理维护中的 Qwen full-generation native caller：`_native_qwen_request` 可把
observer 传给 `APPClient.request_native_payload`，full-generation 分支收集非终态
`GenerationTokenEventV1` 快照，等待 C++ observer 的 terminal 通知，再返回最终响应。
没有增加 Python planner、token loop、conversation owner 或 Provider 逻辑；conversation
继续在缺少 native continuation owner 时 fail-closed。

## Static review

按官方 `$review-agent` defect-first 方法检查了：

- `_native_qwen_request` 的可选 callback 参数、请求参数映射和返回边界；
- `full_generation_call` 的 native route 选择、callback 顺序、终态等待和异常记录；
- `APPClient.request_native_payload` → `NativeInferenceHandle.observe` 的生命周期与
  C++ observer ownership；
- maintained Qwen caller、Python source gate、conversation fail-closed 分支以及没有
  planner fallback 的 route 顺序。

No actionable finding。静态 coverage matrix 如下：

| Lane | Status | Evidence |
| --- | --- | --- |
| production entry / callers | covered | `llm_pipeline/user.py` `_native_qwen_request` and `full_generation_call`; `APPClient.request_native_payload` |
| implementation and wire | covered | Python callback forwarding and `GenerationTokenEventV1` schema check; native event ordering remains in `NativeInferenceClient` |
| test / harness / oracle | covered for caller/source route and native observer/token-order selectors; gap for real Provider/cross-process delivery | 28-case Spec182 Python suite; C++ observer/deadline/public-conversation selectors |
| build / source closure | N/A for native sources | this batch changes only the maintained Python caller; the previously built extension and shared DI target are unchanged and referenced as R5-B7 evidence |
| migration / evidence | PARTIAL | Qwen full-generation route is callback-aware; YOLO, Provider retirement, conversation owner and T016 remain open |

## Verification

```text
python3 -m py_compile examples/python/NDNSF-DistributedInference/llm_pipeline/user.py \
  tests/python/test_spec182_legacy_exclusion.py -> exit 0

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  /usr/bin/python3 -m pytest -q \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py
-> 28 passed in 1.30s

build-nac182/unit-tests --run_test=Spec182ClientState/SlowObserverDoesNotBlockCancelAndLateReplaySurvivesClientClose
-> exit 0; elapsed=0.09s
build-nac182/unit-tests --run_test=Spec182ClientState/RealDeadlineDoesNotWaitForWorkOrSlowObserver
-> exit 0; elapsed=0.29s
build-nac182/unit-tests --run_test=Spec182V3Placement/PublicClientConversationCommitsSeededReceiptAndCheckpoint
-> exit 0; elapsed=0.41s

git diff --check -> exit 0
```

## Result classification

| Category | Result |
| --- | --- |
| Static findings | none; caller, facade, lifetime, route ordering and source gate re-reviewed |
| Compile/build misses | none; Python-only change, no native source closure changed |
| Runtime/test misses | none in the focused selectors; no real Provider or cross-process callback was run |
| Build measurement | native build N/A for this Python-only batch; prior R5-B7 shared/extension builds remain valid for unchanged native sources |
| Behavior result | `STATIC_PASS; FOCUSED_BEHAVIOR_PASS; PARTIAL`; not `QUALIFICATION_PASS` |

## Review trace and closure decision

- **review-agent**: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
  SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- **Baseline / diff**: baseline `54b859511a10db7e10c2413090eb642a25d36f7b`, reviewed
  implementation diff through commit `04be23ed93d446d94593be481c52034ed94e85a7` for
  `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py` and its focused test;
  composition scope also covered `APPClient.request_native_payload` and the native observer
  path named in this record.
- **Review result**: `No findings`; callback lifetime, terminal ordering, route selection,
  source gate and test selector registration were rechecked after the implementation review.
- **Closure decision**: `OPEN_FOR_NEXT_BATCH`. The local Qwen callback exit is observable and
  validated, but a real Core/Provider stream, cross-process delivery and native conversation owner
  are not yet present. The next caller-shaped batch must provide those selectors before T013-A or
  T016 can advance.

## Remaining boundary

The maintained Qwen caller now has one explicit callback-aware native full-generation route,
but the callback is still observed only through local C++/Python fixtures. A real Provider
stream, conversation continuation owner, YOLO caller migration, legacy retirement and T016
qualification remain separate exits.
