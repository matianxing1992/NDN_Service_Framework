# R11-B8-G3 Native Dynamic Conversation Binding

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for this bounded sub-batch
**Parent**: R11-B8 / T012-B / T013-A / R11-B4

## Scope

首轮 `FULL_CONTEXT` 的动态 placement 只能在 ACK closure 后确定。此前
`NativeConversationCoordinator::beginTurn` 要求 caller 预先提供
`planRoleMapDigest` 和完整 `expectedRoles`，通用 native caller 无法在不猜测
placement 的情况下打开 continuation。本批把这段 metadata ownership 收回 C++：
首轮可暂存未绑定 continuation；native planner 得到 sealed provider map 后，
coordinator 原子绑定 canonical role-map digest 与角色集合，再继续 Core plan commit。
`APPEND_DELTA`、replacement、checkpoint CAS 和 Provider receipt 约束保持原语义。

## Changed Files

- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp/.cpp`
  新增 `bindInitialPlanRoleMap`，只允许首轮未绑定的 `FULL_CONTEXT` turn，校验角色
  namespace/provider 非空并保存 coordinator-owned digest；已有 continuation 仍要求
  authenticated digest/role set。
- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.cpp`
  允许唯一的首轮未绑定状态在 placement 后生成 projection binding，同时继续严格校验
  append/replacement 的角色集合和 map digest。
- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp`
  在 native planning 返回后调用首轮或 replacement 对应的 coordinator binding，再提交
  sealed plan；caller 不计算或伪造 placement metadata。
- `tests/unit-tests/di-native-conversation.t.cpp`
  增加首轮动态绑定、角色排序、digest 生成和重复绑定拒绝的 C++ primary 用例。

## Static Review

按 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 的只读 defect-first 方法检查
完整 diff、`beginTurn`/planner/client 调用链、pending-turn 所有权、首轮与 append/
replacement 状态机、checkpoint CAS 和 projection digest。未发现 actionable finding。
首轮放宽只同时满足 `FULL_CONTEXT`、epoch 0、空 digest、空角色集合；绑定发生在
sealed placement 返回后，重复/错误状态通过 coordinator ticket 与状态检查拒绝。

## Validation

- `git diff --check`: PASS。
- system-first Waf unit rebuild (`-j2`，190/190): PASS。
- `./.codex-tmp/spec182-r11-b2-fresh-20260910/build/unit-tests --run_test='Spec182Conversation/InitialFullContextBindsDynamicPlanAfterPlacement' --log_level=test_suite`: 1 case, no errors。
- `./.codex-tmp/spec182-r11-b2-fresh-20260910/build/unit-tests --run_test='Spec182*' --log_level=message`: 257 cases, `*** No errors detected`。
  Raw log: [spec182-r13-dynamic-conversation-unit.log](../../../.codex-tmp/spec182-r13-dynamic-conversation-unit.log)。
- system-first Waf integration rebuild (`-j2`，119/119): PASS。
- New selector `./.codex-tmp/spec182-r11-b2-fresh-20260910/build/integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182R11B8G3RealProviderDynamicConversationPlacement' --log_level=test_suite`: 1 case, `*** No errors detected`。
  Raw log: [spec182-r11-b8-g3-integration.log](../../../.codex-tmp/spec182-r11-b8-g3-integration.log)。
- Full `./.codex-tmp/spec182-r11-b2-fresh-20260910/build/integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182*' --log_level=message`: 10 selected cases, `*** No errors detected` (the expected no-admitted-provider negative remains an asserted failure boundary)。
  Raw log: [spec182-r13-g3-integration.log](../../../.codex-tmp/spec182-r13-g3-integration.log)。

## Boundary

本批证明的是 C++ coordinator 在动态 placement 下正确收敛首轮会话 metadata。它不关闭
15 个 maintained caller、Qwen/`TOKEN_STREAMING` adapter generation、Provider KV
durable recovery、legacy zero-use、no-Python、T016/T017 或完整 Spec182 qualification。
R11-B8 父卡仍为 `PARTIAL`，R11-B9 仍为 `NOT_STARTED`。
