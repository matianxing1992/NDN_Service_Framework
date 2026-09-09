# R10-B33 Native Unary Repository Reference Request — 2026-09-09

## Scope and boundary

本批补齐一个可观察的 native 请求出口：在既有 R4-B6 真实 `ServiceProvider` fixture
中同时启用 `repositoryInput=true` 与 `unaryRequest=true`。请求仍由
`NativeInferenceClient::request` 经过 native preparation、grant/admission、placement、
Core commit 和 Provider handler；Provider 先用 v2 `REPO_REF` 恢复已发布的加密对象，
再通过 `CollaborationContext::publishFinalResponse` 返回普通 `Response`。本批不改变
协议、Python facade 或 conversation 状态机，也不把单进程 fixture 提升为跨进程资格。

## Review trace

- **Reviewer**: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- **Review SHA**: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- **Baseline**: `613db8b9c6e6f038a8ca473e8d9af40429bb6f3f`
- **Diff**: `tests/integration-tests/ndnsf-di-core-flow.t.cpp` selector registration plus
  this progress/evidence/plan update。
- **Result**: `No findings.`

Static Gate Release Checklist:

| Lane | Check and result |
| --- | --- |
| `production entry/callers` | `runR4B6RealProviderConversationCase(false, false, true, true)` reaches the existing native requester and real Provider handler; the new selector is registered under `Spec170NdnsfDiCoreFlow`. |
| `test/harness/oracle` | The helper checks the recovered plaintext against the published bytes, then asserts the exact unary result payload; `--list_content` showed the selector before execution. |
| `build/source closure` | `integration-tests` is the existing Waf target containing `ndnsf-di-core-flow.t.cpp`; only that target was rebuilt, and the linked binary hash was recorded below. |
| `migration/evidence` | The batch preserves the one-process boundary and carries forward the open cross-process, maintained-caller/no-Python and T016 limits; no prior compile/runtime miss required a retry gate change. |

## Verification

Build (system-first toolchain, current host default `-j4`):

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin \
  CXX=/usr/bin/g++ CC=/usr/bin/gcc WAFLOCK=.lock-waf \
  ./waf -o build-nac182 build --targets=integration-tests -j4
```

Result: exit `0`, elapsed `36.514 s`; compiler emitted only the existing aggregate
initialization warnings. `vmstat` subsequent samples had `si/so` `52/0`, then `0/0`,
so no sustained swap-in/out was observed. Raw logs: [build.log](../../../.codex-tmp/spec182-r10-b33/build.log)
and [vmstat.txt](../../../.codex-tmp/spec182-r10-b33/vmstat.txt) (local, untracked).

The first filter attempt omitted the Boost.Test suite prefix and returned the framework's
no-match setup code `200`; this was a selector spelling correction, not a product failure.
The registered selector then passed:

```text
./build-nac182/integration-tests \
  --run_test='Spec170NdnsfDiCoreFlow/Spec182R10B33RealProviderUnaryRepositoryReferenceRequest' \
  --report_level=detailed --log_level=nothing
```

Result: exit `0`, `1` case, `3` assertions, elapsed `3.510 s`; raw output:
[selector.log](../../../.codex-tmp/spec182-r10-b33/selector.log).

Related regression selectors (conversation, replacement, alternate replacement, streaming
repository reference, unary inline, and this unary repository reference) all passed: `6`
cases, exit `0`, elapsed `34.205 s`; raw output:
[related-selectors.log](../../../.codex-tmp/spec182-r10-b33/related-selectors.log).

## Batch retrospective

- **Static miss**: none. The release checklist covered the helper branch, selector registration,
  result oracle and existing target closure.
- **Compile miss**: none. The target rebuilt successfully with the current host default.
- **Runtime miss**: the initial unqualified filter was a test-name setup mistake; the corrected
  suite-qualified selector passed and no native behavior was inferred from the failed setup.
- **Build cost**: one affected-target rebuild (`36.514 s`) followed by six focused selectors
  (`34.205 s` total); no unrelated Core/Repo/UAV target was rebuilt.

## Closure decision

`CLOSED_FOR_VALIDATION` for this bounded unary `REPO_REF` requester/Provider boundary;
`OPEN_FOR_NEXT_BATCH` for the stable next exits: deployed Provider worker and cross-process
transport, maintained caller/no-Python migration, I02--I08 isolation cases, and T016 final
qualification. This batch does not change the parent status of T004/T005/T008/T010/T011/T013/
T014/T015/T016/T017.

Binary identity: `build-nac182/integration-tests` SHA-256
`db4bc5cde88bc957a756a1e5b89fa670b2738c308a4adb928bac24615b5c8224`.
