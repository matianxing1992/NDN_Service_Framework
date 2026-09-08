# R5-B3 Maintained Caller Route Audit

## Scope and boundary

2026-09-08，基于源码 checkpoint `d47e14ea` 审查 T013-A 的七个登记入口和
`compatibility-manifest.json`。本单元只做调用链核对和批次重排，不修改产品源码、
不运行 MiniNDN/Provider、不把旧 Python 请求结果当作 native parity。manifest 的
`sourceCommit` 仍为 `62157804fca46ac332592b0519a6162d4151bd19`，因此只作 caller
inventory；下面的语义判断以当前源码为准，最终迁移前必须按最终源码重新生成。

## Caller matrix

| Caller | Current entry and owner | Native status | Next bounded work |
| --- | --- | --- | --- |
| `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` | launches `yolo_2x2/user.py` at `:940-943`; starts `di-native-provider` at `:753-775`; focused `_focused_request_wire()` at `:2740-2769` calls Python `AutomaticPlanningCoordinator` only for a negative fixture | provider executable is native in this harness, but requester remains Python `APPClient.request_task` through the user script; fixture encoder is offline oracle | keep harness orchestration; migrate requester only after native catalog/runtime config and C++ parity are available |
| `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py` | validates sealed inputs and `execvpe()` delegates to `Experiments/NDNSF_DI_LlmPipeline_Minindn.py` at `:294-362` | launcher has no native requester construction; delegated runner owns the current Python route | add a native Qwen runtime-config handoff, then switch the delegated requester as its own batch |
| `Experiments/NDNSF_DI_StreamedGeneration_Minindn.py` | builds a fixed command for `NDNSF_DI_LlmPipeline_Minindn.py` at `:151-192` and executes it at `:223` | wrapper is a harness/collector; native request ownership is not present in this file | preserve collector; migrate the delegated stream caller after native stream/options binding exists |
| `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` | constructs canonical `APPClient.from_config()` at `:719-727`; production request is `client.request_task()` at `:556-563`; legacy layout path uses `distributed_inference()` at `:835-843` | still uses Python planner/deployment facade; no `request_native()` call | first requester-family migration candidate, after a concrete native catalog + grant/admission config is available |
| `examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py` | constructs `APPProvider.from_config()` at `:348-357` and executes Python ONNX role handlers | Python Provider runtime remains the owner; this is not closed by the harness's separate native provider command | separate provider-host migration/retirement work; do not claim all maintained callers migrated from requester-only changes |
| `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py` | constructs `APPClient.from_config()` at `:3900-3908`; uses `request_streaming()` at `:1153-1166` and `:2763-2774`, `request()` at `:2895-2901`, and `distributed_inference()` at `:2798-2806`/`:4200-4210` | several Python routes coexist, including automatic-planner introspection at `:2885-2891`; no explicit `request_native()` route | split ordinary/streaming/conversation migration by stable native options and conversation support; remove planner fallback only after parity |
| `examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py` | constructs `APPProvider.from_config()` at `:4553-4564`; Python Qwen/stream/conversation handlers remain in this process | provider-side runtime is still Python; no native executable selection here | provider host migration follows requester parity and native generation/stream ownership |

The current public native route is intentionally explicit: `APPClient.request_native()` calls
the configured `NativeInferenceClient` and raises when it is absent (`app_sdk/client.py:337-344`).
Configuration requires a `NativeRequestRuntime` with catalog/contract, preparation, and offer
admission (`app_sdk/client.py:320-334`); the binding exposes catalog loading, grant-client
construction, and runtime composition (`pythonWrapper/ndnsf/service.py:2399-2425`). None of the
seven maintained entries currently supplies this complete object set. A blind replacement of
`request_task()`/`request_streaming()` would therefore fail closed at configuration time or
silently reintroduce the Python planner, both of which violate T013-A.

## Five-lane coverage matrix

| Lane | Status | Evidence and check |
| --- | --- | --- |
| `production entry/callers` | covered for the seven registered paths; gap for unlisted manifest callers | `rg -n "APPClient|request_task|request_streaming|request_native|distributed_inference|APPProvider|execvpe|di-native-provider"` over the seven files; manifest remains token inventory |
| `implementation and wire` | gap for caller composition | native `request_native()`/runtime composition exists, but no maintained caller builds catalog, preparation, grant client and admission together |
| `test/harness/oracle` | gap for native caller behavior | Python compatibility tests cover explicit route shape; no C++ target/selector currently drives a maintained caller's full request; harnesses are write-only or delegated |
| `build/source closure` | covered for existing facade extension; gap for maintained executable/provider closure | R5-B2 extension build/import evidence; no caller-family native runtime/Provider executable closure recorded |
| `migration/evidence` | gap | `compatibility-manifest.json` is stale and marks retained compatibility; no zero-caller snapshots or rollback evidence for old routes |

## Replanned bounded batches

1. **R5-B3 Caller Route Audit (this record)** — freeze the matrix and fail-closed boundary;
   exit is a reviewed inventory with no product source change. `T013-A` remains `PARTIAL`.
2. **R5-B4 Native Requester Config Fixture** — materialize one operator-pinned catalog,
   preparation, grant/admission and `NativeRequestRuntime` for the YOLO requester; prove the
   production C++ request target/selector against the same config before changing a maintained
   caller. Delivered by the R5-B4 parser/fixture batch.
3. **R5-B5 YOLO Native Composition** — route the maintained C++ requester entry through the
   shared runtime parser, preserve arguments/oracles/cleanup, and run binding-route checks plus
   the named C++ selector. This does not yet switch `yolo_2x2/user.py`; that caller migration is
   a remaining T013-A subtask and the Python route check cannot close native behavior by itself.
4. **R5-B6 Qwen/streaming Requester Migration** — after native generation/stream options and
   conversation parity are available, migrate the two delegated harnesses and LLM user routes
   as separate stable exits; do not combine them with provider retirement.
5. **R6-B1 Provider Host Migration and Legacy Retirement** — migrate the two Provider examples
   to the native Provider executable/host, then run zero-caller and blocked-import checks. This
   remains behind requester parity and T013-A acceptance.

These IDs are execution-batch labels, not new parent tasks; existing T013/T014 dependencies,
acceptance gates and the final T016 qualification remain unchanged. No batch is ready for
implementation until its native C++ selector, operator config, and rollback evidence are named.

## Static review and validation

The read-only `/home/tianxing/.codex/skills/review-agent/SKILL.md` profile covered all seven
caller files, the native facade entry, manifest identity, and the batch split above; result:
`No findings`. Checks were read-only and completed with exit 0:

```text
codegraph explore "APPClient request request_task request_streaming AutomaticPlanningCoordinator"
rg caller/route inventory over the seven maintained files
python3 specs/182-native-di-python-bindings/checklists/validate_design.py
git diff --check
```

The design validator still reports 17 parent tasks, 36 execution cards, 3 complete parents,
and no document errors. No product build, runtime test, or experiment ran in this audit.
