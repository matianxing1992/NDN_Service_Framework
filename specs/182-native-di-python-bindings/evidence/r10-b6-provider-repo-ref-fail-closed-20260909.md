# R10-B6 Provider REPO_REF Fail-Closed Evidence

**Date**: 2026-09-09
**Batch**: R10-B6
**Baseline**: `4b88d73a` (R10-B5 evidence-format checkpoint)
**Scope**: the native ingress integration fixture, its Provider negative selectors, and the
existing `integration-tests` target. The pre-existing native design documents and unrelated
worktree artifacts are outside this batch.

## Behavior boundary

本批在 R10-B5 正向 Provider fetch/decrypt 出口上补齐三个 fail-closed 负例。真实
`ServiceProvider`/`NativeProviderHandler` 分别处理缺失的加密 `REQUEST-LARGE` 对象、声明的
明文尺寸不一致和 malformed v2 envelope；每个边界都通过 selection execution status
报告失败，不进入 runner，也不发布成功 Response。缺失对象用例只在测试作用域设置
`NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS=1000`，由 RAII 在测试结束时恢复原环境值；生产默认
30 秒预算和 SegmentFetcher/legacy fallback 逻辑没有修改。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `ServiceProvider::CollaborationContext::fetchEncryptedLargeData`; `NativeProviderHandler::initialInputsFromRequest`; `runNativeIngressCase` | CodeGraph/`rg -n "initialInputsFromRequest|fetchEncryptedLargeData|runNativeIngressCase"` | 三个负例均经过真实 Provider handler；缺失对象首轮只暴露固定 pump 边界，已保留并修正测试预算 |
| `implementation and wire` | `covered` | v2 `schema`, `input_transport=REPO_REF`, `input_reference.dataName`, `plaintextSize`; `ctx.fail` status path | `rg -n "input_transport|input_reference|plaintextSize|failed to fetch native DI request" NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp` | 缺失对象得到 fetch failure，尺寸 mismatch 得到精确错误，malformed JSON 在 parser boundary 拒绝；无生产代码修改 |
| `test/harness/oracle` | `covered` | `ProductionIngressRejectsMissingNativeRepositoryReference`; `ProductionIngressRejectsNativeRepositoryReferenceSizeMismatch`; `ProductionIngressRejectsMalformedNativeRepositoryEnvelope`; observed runner input and selection status | `./build-nac182/integration-tests --run_test=Spec170NativePostSelection --log_level=test_suite` | 完整 suite 8/8 通过；三个负例均 `statusFailed=true`, `providerInputMatches=false`, `responseReceived=false`, `timedOut=false` |
| `build/source closure` | `covered` | `tests/integration-tests/ndnsf-di-core-flow.t.cpp`; Waf `integration-tests`; `NativeProviderHandler.cpp` linked source | `rg -n "ndnsf-di-core-flow.t.cpp|NativeProviderHandler.cpp" tests/wscript build-nac182`；system-first Waf build | 118/118 tasks 编译/链接；变更测试源和生产 handler 均在 target closure 内 |
| `migration/evidence` | `gap` | R10-B2--R10-B4 requester/caller route; T013/T016 | `rg -n "request_native_reference|T016|cross-process" specs/182-native-di-python-bindings/{plan.md,tasks.md}` | 本批只关闭本地 Provider 负例；跨进程、maintained caller network execution、legacy retirement 和 T016 仍未观测 |

## Review trace

- 官方只读技能：`/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA-256
  `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。
- Review scope: baseline `4b88d73a` to the R10-B6 source/docs diff, including the timeout guard,
  reference envelope construction, Provider parser/fetch path, runner observer, selector
  registration, Waf source closure, and failure evidence.
- Static review covered all five lanes and rechecked the first runtime/test miss. No actionable
  finding remained after the RAII timeout guard and evidence updates. The shared
  `speckit-code-design` Minimum Review Record and miss-feedback rule from R8-SKILL remain the
  governing reusable skill; this batch did not add a new skill change because the miss was a
  first-instance fixture deadline boundary, not a repeated coverage-class omission.
- `git diff --check` and `python3 specs/182-native-di-python-bindings/checklists/validate_design.py`
  were run after the final evidence/task updates; the design validator reports `errors: []`.

## First failure and repair

The first full-suite attempt entered the missing-object case but stopped after the fixture's
fixed 200-round (about 3 s) pump before the production fetch could exhaust its default 30 s
shared budget. The assertions for `statusFailed` and its message failed; no success response or
runner input was observed. The other two negative cases passed. The unchanged first output is
`.codex-tmp/spec182-r10-b6/suite-missing-failure.log`; build and `vmstat` captures are in the
same directory. `docs/failure-log.md` records this as a `runtime/test` boundary rather than a
product pass.

The repair scopes a 1 s fetch budget only while `runNativeIngressCase` executes the missing-object
case. This lets the existing production SegmentFetcher-to-legacy fallback return its normal
failure reason inside the local oracle window; the RAII destructor restores any prior environment
value for subsequent cases.

## Build and test result

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  /usr/bin/python3 ./waf -o build-nac182 build --targets=integration-tests -j4 -v
exit=0; Waf elapsed=34.427s; 118/118 tasks
```

Compile and link commands used `/usr/bin/g++ -B/usr/bin`; `vmstat 1` showed no sustained swap
in/out (the raw samples are in `.codex-tmp/spec182-r10-b6/rebuild-vmstat.log`). The complete
`Spec170NativePostSelection` suite entered all eight cases and exited 0 in 8.98 seconds. The
three new selectors passed with these observed failure reasons:

- missing object: `failed to fetch native DI request input reference: ...`;
- declared size mismatch: `native DI request input plaintext size mismatch`;
- malformed JSON: `malformed native DI request envelope: ...`.

## Batch retrospective

- `static`: no actionable product finding after review; the timeout guard, parser/fetch ordering,
  runner non-entry, selector registration and target closure are explicit in the matrix.
- `compile/link`: none after repair; the changed integration source compiled and linked in the
  registered target (118/118).
- `runtime/test`: the first fixed-pump miss is preserved as a harness deadline boundary; the
  scoped budget and full suite then passed 8/8.
- `unobserved`: cross-process repository fetch, maintained YOLO/Qwen network execution, caller
  retirement, stream/conversation execution and T016 qualification remain open.
- `batch expansion`: kept to one Provider fixture and one selector family; no new requester,
  Python, or cross-process responsibility was added.

## Closure decision

`CLOSED_FOR_VALIDATION` for the local Provider REPO_REF fail-closed negative boundary. The
independent exit is that missing-object, size-mismatch, and malformed-envelope requests fail in
the real Provider handler before runner input or successful response. Parent T004/T010/T013/T016,
cross-process behavior, maintained caller execution, and final Spec182 qualification remain
`PARTIAL`/open and are not promoted by this focused batch.
