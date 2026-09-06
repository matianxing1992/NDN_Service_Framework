# R002 — native ProtectedRuntime 诚实化（FAIL_CLOSED until T002）

> **Current scope correction (revision 6, 2026-09-05)**: T002 生产 grant 验收已完成，R002 的临时未实现状态已由真实 verifier/worker 路径吸收，见 [T002 acceptance](t002-acceptance-20260905.md)。缺配置或未验证 grant 时仍失败关闭；下方“until T002”只保留为历史范围，不描述当前已接线行为。T007 与正式资格仍 BLOCK。
> 当前裁决与下一步以 [audit.md](../audit.md) 为准，以下保留为原始范围记录。

**Layer**: implemented（C++ 诚实化代码 + C++ unit 负例）;executed
（6 项 C++ unit 测试全绿，2026-09-05，waf build-system-j2）;无 measured
声明。Integration 层随 T002 的 `tests/integration-tests/ndnsf-di-protected-grant.t.cpp`
落地一并验证（本文件为诚实声明，不把未执行证据标为 executed）。

Date: 2026-09-05. Source HEAD: `af85b7a7` plus the recorded spec181 Phase-0
worktree changes.

## 声称

在真实 grant 获取/解包（T002 实现）之前，native Provider 对非
`plaintext-v1` 纪元赋值必须失败关闭并给出明确错误
（`DI_PROTECTED_GRANT_UNAVAILABLE`），禁止仅凭绑定比对进入
`GrantVerified` 状态；现有绑定比对函数明确为"绑定一致性校验"。

## 代码实现（implemented）

### `NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.cpp`

- `verifyGrant()`（:121-134）：诚实门——无条件设置 `FailedClosed`、
  `m_terminalReason = "DI_PROTECTED_GRANT_UNAVAILABLE: native grant
  verification is not implemented (spec181 R002; real verifier lands in
  T002)"` 并抛出。任何绑定一致性比较都不能使 runtime 进入
  `GrantVerified`。
- `verifyBindingConsistency()`（:136-159）：仅做结构校验与过期检查，
  注释明确 "Execution authority is NOT granted: state stays NoGrant"。
- `cancel()`（:260-270，本任务修正）：仅 `Zeroized` 早退；
  `FailedClosed` 状态也执行 `drainLocked()` 尽力零化剩余明文（R002
  unit 契约 "Cleanup on an un-authorized runtime still drains cleanly"）。

### `NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp`

- :74-75 注释：`verifyGrant()` fails closed with
  `DI_PROTECTED_GRANT_UNAVAILABLE`;no binding-consistency comparison
  alone may move the runtime to GrantVerified。
- :88 注释：T002 installs the real verifier。

### `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp`

- :1880-1893：protected-epoch 投影到达时，若 factory 返回的 runtime
  处于 `NoGrant`/`FailedClosed`（R002 阶段任何诚实 factory 都如此），
  `ctx.fail("DI_PROTECTED_GRANT_UNAVAILABLE")` 在授权边界（Provider
  装配之前）拒绝并返回；无 factory 时
  `ctx.fail("DI_PROTECTED_RUNTIME_FACTORY_MISSING")` 同样失败关闭。

## 测试执行（executed — unit 层）

`tests/unit-tests/distributed-inference-protected-runtime.t.cpp`
（spec181 R002 honesty tests，4 个用例 + 其余 2 个同模块用例）:

```
./build-system-j2/unit-tests --run_test='*ProtectedRuntime*' --log_level=message
Running 6 test cases...
*** No errors detected
```

覆盖：`verifyGrant` 对完全一致的绑定仍失败关闭且错误含
`DI_PROTECTED_GRANT_UNAVAILABLE`、状态不进 `GrantVerified`
（`ProtectedRuntimeVerifyGrantFailsClosedUntilRealVerifier`）;绑定一致
性校验通过但不授予任何权威（状态 `NoGrant`、authorize/lease 全部
失败关闭、cancel 后干净排空至 `Zeroized`——
`ProtectedRuntimeBindingConsistencyGrantsNoAuthority`）;绑定替换与过期
失败关闭（`ProtectedRuntimeFailsClosedOnBindingSubstitution` /
`ProtectedRuntimeFailsClosedOnExpiry`）。

本次执行还发现并修复一个真实缺陷：原 `cancel()` 对 `FailedClosed`
早退导致未授权 runtime 无法干净排空（`state == Zeroized` 断言失败），
修正后 6 用例全绿（见 worktree diff）。

## Integration 层（T002 落地时验证）

真实生产调用链（Python 端到端 native provider 真实保护纪元投影被
拒）依赖 `protectedRuntimeFactory` 的 pybind 接线与
`tests/integration-tests/ndnsf-di-protected-grant.t.cpp`——这两项都是
T002 的交付物。R002 的 handler 拒绝路径（`NativeProviderHandler.cpp:1890`）
已实现且由本 unit 层间接覆盖其语义;T002 的集成测试将驱动真实投影
到达该边界并断言 `DI_PROTECTED_GRANT_UNAVAILABLE`。

## 吸收关系

T002 落地后由真实 grant 验证（fetch + 权威签名 + KeyChain 解包）取代
该失败关闭路径;届时更新本文件为历史记录。

## Verdict

PASS（R002 范围，unit 层）。保护纪元赋值明确 unavailable 错误、状态
不进 `GrantVerified`、绑定一致性校验不授予权威、清理路径干净排空。
Integration 层验证点随 T002 交付（见上）。
