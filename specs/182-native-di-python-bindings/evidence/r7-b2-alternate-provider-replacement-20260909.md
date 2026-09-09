# R7-B2 Alternate-Provider Replacement

**Date**: 2026-09-09
**Status**: `CLOSED_FOR_VALIDATION` for the local native replacement batch; overall
T011-C and Spec182 remain `PARTIAL`.
**Baseline**: `54e5c16a` (R7-B2 registration)
**Owner**: native requester/conversation implementation

## Scope and Allocation Basis

本批围绕同一个公开入口 `NativeInferenceClient::beginReplacement` →
`beginCoreRequest` → `NativeRequestPlanner::planNativeRequest` →
`NativeConversationCoordinator`，只处理一次有界 replacement 的 execution identity、
contract digest 和 role/provider map。共享接口/状态契约是 `NativeConversationTurn`、
parent CAS、receipt/control ACK 与 V2 Selection name；独立出口是同一真实
`ServiceProvider` harness 中的单 Provider fail-closed 和双 Provider alternate replacement。
Python caller migration、跨进程会话和 T016 资格具有不同 owner/selector，未纳入本批。

实际差异范围：

- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp/.cpp`
- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp`
- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.cpp`
- `ndn-service-framework/utils.cpp`
- `tests/integration-tests/ndnsf-di-core-flow.t.cpp`
- `tests/unit-tests/di-native-conversation.t.cpp`
- `tests/unit-tests/generic-dynamic-api-prepared.t.cpp`
- 本批契约/计划/进度文件

## Review Trace

官方只读技能：`/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。审查基线为
`54e5c16a`，实现者差异范围为上列源码、测试及本批契约/进度文件；其他会话未提交的
`native-dependency-design.md`、`native-generation-design.md` 不在本批。覆盖查询包括：

```text
codegraph node NativeConversationCoordinator::bindAttemptPlanRoleMap
codegraph node NativeRequestPlanner::bindConversationProjections
codegraph node parseServiceSelectionNameV2
codegraph node parseServiceSelectionDecisionNameV2
rg -n "conversationTurn|beginReplacement|bindAttemptPlanRoleMap" NativeInferenceClient.cpp tests/
git diff --check
```

静态复核覆盖调用链、状态与 owner lock、request/attempt/contract/map digest 绑定、
V2 wire/parser 兼容、测试 fixture/oracle、Waf source closure 与测试注册。首轮发现
coordinator 公开 map binding 未校验 expected role set/provider 非空，已修复并加入缺 role
负例；复审无 actionable finding。另一个初版 parser 修正会误拒绝 legacy
`request-1/3` decision，已在 legacy selector 前修复，最终复审无 actionable finding。

## Coverage Matrix

| Lane | Result and concrete scope |
| --- | --- |
| `production entry/callers` | `covered`: `NativeInferenceClient::beginReplacement`, `beginCoreRequest` and `NativeConversationCoordinator::{replaceAttempt,bindAttemptPlanRoleMap,prepareCheckpoint,commitTurn}`; checked with CodeGraph and caller reads. |
| `implementation and wire` | `covered`: `NativeConversationTurn.executionRequestId`, replacement `requestContractDigest`, immutable parent map CAS, successor map digest, `NativeRequestPlanner::bindConversationProjections`, and `utils.cpp` V2 Selection parsing. |
| `test/harness/oracle` | `covered` for local native behavior: `Spec182Conversation/ReplacementFencesOldAttemptAndPreservesAcceptedPrefix`, `GenericDynamicApi/PreparedAndMessages/V2RequestAndResponseNames`, `GenericDynamicApi/DeploymentControl/R1SelectionDecisionNameTargetsProviderAndAttempt`, plus three real-provider R4-B6 selectors. Cross-process oracle is intentionally `gap` and remains T016. |
| `build/source closure` | `covered`: existing Waf globs register coordinator/client/planner/parser and both test targets; final build compiled/linked `306/306` tasks. Build command and compiler/linker lines are in `r8/build.log`. |
| `migration/evidence` | `gap` for maintained Python callers, cross-process requester/provider and qualification; retained as T013/T016 acceptance dependencies. Local raw runs and this record are durable evidence. |

## Batch Retrospective

- **Static**: coordinator role-set/provider validation was initially missing; fixed before
  final gate. Parser compatibility was reviewed against structured and legacy names; final
  re-review had no findings.
- **Compile/link**: the first implementation build failed at the integration helper syntax
  boundary (`tests/integration-tests/ndnsf-di-core-flow.t.cpp:952`); the raw log is retained in
  `.codex-tmp/spec182-r7-b2-replacement-map-20260909/build.log`. No final compile/link miss.
- **Runtime/test**: the first alternate-provider run reached the recovery Selection but the
  old parser truncated `/NDNSF/DI/REQUEST/1/recovery/2` before provider1 callback; trace is
  retained in `.codex-tmp/spec182-r7-b2-replacement-map-20260909-r4/integration-alternate-trace2.log`.
  The final parser and legacy regression selectors pass.
- **Unobserved**: cross-process restart/restore, no-Python caller migration, Provider retirement,
  full stream matrix and T016 qualification remain unobserved; they are not upgraded by this batch.
- **Batch size**: the stable alternate-replacement exit was reached before adding Python
  migration or cross-process responsibilities; no post-exit batch inflation occurred.

## Build and Focused Results

Final build (system-first toolchain, 6 logical CPUs/12 GB host):

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  ./waf -o build-nac182 build --targets=unit-tests,integration-tests -j4 -v
exit=0; Waf tasks=306/306; elapsed=32.93s
```

`vmstat 1` samples after the first line showed no sustained swap-in/out. Final selectors,
all exit `0`:

```text
unit-tests --run_test='Spec182Conversation/ReplacementFencesOldAttemptAndPreservesAcceptedPrefix'
unit-tests --run_test='GenericDynamicApi/PreparedAndMessages/V2RequestAndResponseNames'
unit-tests --run_test='GenericDynamicApi/DeploymentControl/R1SelectionDecisionNameTargetsProviderAndAttempt'
unit-tests --run_test='GenericDynamicApi/*'                 # 126 cases
integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversationReplacement'
integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversationAlternateReplacement'
integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation'
```

Selector timings and return codes are preserved in
`.codex-tmp/spec182-r7-b2-replacement-map-20260909-r9/`.

## Behavior Result and Closure

`STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; not `QUALIFICATION_PASS`. The single
Provider failure remains `NATIVE_REQUEST_STAGE_FAILED` at `ACK_CLOSED` with
`DI_NATIVE_NO_ADMITTED_PROVIDER` and no checkpoint. The two Provider case completes one
replacement on the alternate Provider; the successor checkpoint carries the alternate map and
recovery request ID, while the parent CAS remains bound to the original map. The original
positive FULL_CONTEXT path remains green.

`Closure decision: CLOSED_FOR_VALIDATION` for this local alternate-provider behavior. Trigger for
the next batch is a named cross-process conversation selector and maintained native caller
closure; continue with T013/T016 dependencies without reopening this local batch.
