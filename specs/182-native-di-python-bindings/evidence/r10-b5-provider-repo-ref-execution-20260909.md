# R10-B5 Provider REPO_REF Execution Evidence

**Date**: 2026-09-09
**Batch**: R10-B5
**Baseline**: `afea5053` (R10-B4 documentation checkpoint)
**Scope**: `tests/integration-tests/ndnsf-di-core-flow.t.cpp` native ingress fixture and
the existing `integration-tests` target.  The pre-existing contract files and unrelated
proposal worktree changes are outside this batch.

## Behavior boundary

本批把 R10-B1--R10-B4 已建立的引用提交边界推进到真实 Provider 消费。测试发布一个加密
`REQUEST-LARGE` 对象，只把 v2 `ndnsf-di-request-envelope-v2` 的 `REPO_REF` 元数据放进
Request；生产 `NativeProviderHandler` 经 `CollaborationContext::fetchEncryptedLargeData`
取得并解密对象，校验 `plaintextSize`，再把 `request-input` 交给 native runner。runner
记录到的明文必须等于发布前的明文。没有新增 requester 解密、引用解析器或 Core wire 类型。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `ServiceProvider::CollaborationContext::fetchEncryptedLargeData`; `makeNativeProviderCollaborationRuntime`; `runNativeIngressCase` | CodeGraph query `NativeProviderHandler initialInputsFromRequest ...`; `rg -n "runNativeIngressCase|NativeProviderHandler" tests/integration-tests/ndnsf-di-core-flow.t.cpp` | 真实 Provider handler 入口及调用路径已读；无 actionable finding |
| `implementation and wire` | `covered` | `NativeProviderHandler.cpp::initialInputsFromRequest`; v2 `input_transport=REPO_REF`, `input_reference.dataName`, `plaintextSize` | `rg -n "input_transport|input_reference|fetchEncryptedLargeData" NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp` | Provider 保持 fetch/decrypt ownership；尺寸不一致仍 fail-closed；无修改生产实现 |
| `test/harness/oracle` | `covered` | `ProductionIngressRunsNativeRepositoryReferenceIntoProvider`; `makeNativeIngressTestRunnerFactory`; observed `request-input` map | `./build-nac182/integration-tests --run_test=Spec170NativePostSelection --log_level=test_suite` | 五个 suite cases 全部通过；新 case 的 runner 明文 oracle 与发布明文相等 |
| `build/source closure` | `covered` | `tests/integration-tests/ndnsf-di-core-flow.t.cpp`; Waf `integration-tests`; linked `NativeProviderHandler.cpp` | `rg -n "ndnsf-di-core-flow.t.cpp" tests/wscript`; system-first Waf build with `-j4` | 118/118 tasks compiled/linked；target 已包含变更源文件 |
| `migration/evidence` | `gap` | R10-B2 facade, R10-B3 YOLO, R10-B4 Qwen callers; T013/T016 | `rg -n "request_native_reference|request_native_payload" ...`; T016 remains externally gated | 本批只证明本地 Provider consumption；cross-process, legacy retirement and T016 remain open |

## Review trace

- 官方只读技能：`/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA-256
  `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。
- Review scope: baseline `afea5053` to the R10-B5 source/docs diff, including the existing
  runner factory, helper call path, new wire envelope, test registration and Waf source list.
- Static review checked production caller/handler wiring, v2 field names and failure ordering,
  plaintext ownership, fixture input observation, test registration, and existing compatibility
  branches. No actionable finding remained after re-review.
- `git diff --check` and `python3 specs/182-native-di-python-bindings/checklists/validate_design.py`
  were run after the evidence and task updates; the design validator reports `errors: []`.

## Build and test result

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  /usr/bin/python3 ./waf -o build-nac182 build --targets=integration-tests -j4 -v
exit=0; Waf elapsed=1m50.876s; 118/118 tasks
```

The generated compile commands used `/usr/bin/g++ -B/usr/bin`; `vmstat 1` samples after the
header showed no sustained `si`/`so` activity.  The full `Spec170NativePostSelection` suite
entered five cases and exited 0 in 5.09 seconds.  The new selector
`ProductionIngressRunsNativeRepositoryReferenceIntoProvider` passed in 0.93 seconds; the
existing assignment-fetch, device-mismatch, four-role, and missing-backbone cases also passed.

The first attempted combined filter used comma-separated values and returned Boost.Test setup
code 200 before any case ran.  This is retained as a test-command `runtime/test` miss in
`docs/failure-log.md` and `.codex-tmp/spec182-r10-b5/`; the corrected suite selector was then
used for the result above.

## Batch retrospective

- `static`: no product finding; coverage explicitly included the production handler, wire fields,
  runner oracle, and Waf source registration.
- `compile/link`: none; the command compiled and linked the registered integration target.
- `runtime/test`: the first comma filter was a harness setup miss (exit 200, no test entered),
  then the corrected suite passed 5/5.
- `unobserved`: malformed/missing REPO_REF negatives, cross-process fetch, maintained caller
  network execution, legacy retirement, and T016 qualification remain unobserved.
- Batch size remained bounded to one handler fixture and one selector family after the stable
  Provider consumption exit became observable; no unrelated caller was added.

## Closure decision

`CLOSED_FOR_VALIDATION` for the local Provider REPO_REF execution boundary.  The independent
exit is a real `ServiceProvider` handler recovering the published encrypted input and delivering
the expected bytes to the native runner.  `T004`, `T010`, `T013`, `T016`, cross-process behavior,
negative reference cases, and final qualification remain open; the next batch must register a
separate boundary rather than treating this focused result as Spec182 completion.
