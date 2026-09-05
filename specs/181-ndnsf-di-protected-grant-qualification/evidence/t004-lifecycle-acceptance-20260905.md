# T004 Real Native Lifecycle Acceptance

**Date**: 2026-09-05 | **Source baseline**: `447bfe3b` + working tree
**Layer**: implemented / focused integration | **Status**: IN_PROGRESS

## Acceptance Scope

沿用实际 Controller/Provider native 对象、维护 MiniNDN 的输入、节点
keychain、进程启动和清理。只做 T004 生命周期定向测试，不运行矩阵。
真实 pre-thread waiter、超过 10 s 的 Python 就绪边界、Core readiness
等待期间取消与 CPU/线程退出检查分别保留独有目录。

## R1 Provider Fixture Boundary

原始记录在忽略的工作区临时根目录
`spec181-t004-lifecycle-20260905-r1/{run.log,case/controller.log}`。
首次 provider-idle probe 在等待判断之前失败：尚未启动 Controller
服务，Provider 构造时 NAC-ABE 公共参数获取报 Nack/Error 并退出。
这不是 Provider waiter 负例；下一 probe 先启动真实 Controller，
遵循当前启动流程既有的 PUBPARAMS cache freshness 窗口，再构造
Provider 并执行短时 wait。源码仍显示 Provider waiter 用初始
`!m_running` 判终态，但尚需真实复现后再作为修复证据。

## R2 Provider Waiter Failure

原始记录为 `spec181-t004-lifecycle-20260905-r2/{run.log,case/controller.log}`，
位于同一忽略的工作区临时根目录。修正 fixture 启动顺序后，真实
Provider 构造成功；在尚未进入 `run()` 时调用
`wait_until_ready(80)`，实际约 6 us 即返回 false，报告
`SPEC181_PROVIDER_READINESS_PREMATURE_TERMINAL`，probe exit 1。
这次失败到达预期等待边界：初始 `!m_running` 被当作终态，导致
Python background thread 启动前的 waiter 提前结束。当前仅完成
真实 RED 复现，尚未修改 Provider 源码或取得 GREEN。

## Focused Core Results

当前构建库配合现有 `tests/standalone/service-controller-readiness.cpp`
及隔离 launcher 执行以下 6 项定向检查，全部 exit 0 / PASS：

| Case | Result |
|---|---|
| `cancel-before-start-and-reset` | PASS |
| `cancel-during-start` | PASS |
| `native-stop-sequence` | PASS |
| `stopped-event-loop` | PASS |
| `timeout-without-hot-loop` | PASS |
| `--real-nfd` | PASS；2 次随机 PUBPARAMS probe，独立 NFD exit 0 |

launcher 输出保存在 r1 的 `core-*.log`；各文件列出独立临时
artifact 目录与完整 `test.log`。这些结果只覆盖相应 Core 边界；
不能替代 Python/native 启动超过 10 s 的余量测试、实际 readiness
等待中的取消测试，或 Provider waiter 修复验收。

## Current Disposition

T004 保持 IN_PROGRESS；T007 保持 BLOCK。下一步修复 Provider 的
初始等待状态误判，使用独有运行目录保留 RED/GREEN，再完成启动
余量及取消检查。本记录不声明完整任务或正式 MiniNDN 资格通过。
