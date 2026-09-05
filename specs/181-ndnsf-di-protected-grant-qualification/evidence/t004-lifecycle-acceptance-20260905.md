# T004 Real Native Lifecycle Acceptance

**Date**: 2026-09-05 | **Source baseline**: `4d378b6d` + working tree
**Layer**: implemented / executed / measured（focused integration） | **Status**: PASS

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
Python background thread 启动前的 waiter 提前结束。该次仅完成
真实 RED 复现，当时尚未修改 Provider 源码或取得 GREEN。

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

T004 的既定定向验收已闭合；T007 保持 BLOCK。Provider 初始等待
状态修复已通过统一 native 构建、7 项 Python readiness、3 项
rebuilt grant parity 和下列真实进程探针。本记录不声明完整 Spec
或正式 MiniNDN 资格通过。

## R3 Build and Probe Preflight

`spec181-t004-lifecycle-20260905-r3/native-build.log` 记录统一构建
exit 0 与 `SPEC180_NATIVE_IDENTITY_OK`。当前构建 manifest 摘要为
`8a0071f327e8d1041fc3f100dc742b7ea6c832b217a05a71dd109c0ac035c6d7`。
源身份为 `4d378b6d` 加本轮工作区 Provider 修复，不是干净提交资格。

同目录 `run.log` 记录 probe exit 1：`OUTPUT_ROOT_MISSING`。
维护 runner 要求调用者预先创建独有 case 输出目录；本次入口缺少
该目录，在创建网络和 Provider 前即失败，不能作为修复负例。
下一次在新目录中预建 case 输出目录，再执行相同实际调用链。

## Rebuilt Native Integration Results

各探针使用维护 runner 的实际输入、keychain、原始 Controller 入口
及真实 native 对象。Controller 脚本仅由测试包装生命周期入口，
未替换 native Controller/Provider 或 Core 探针逻辑。外层运行在
独立 user/net/mount/PID namespace，使用私有 tmpfs `/run`、`/tmp`。

| Run | Production boundary and stimulus | Measured result |
|---|---|---|
| r6 `provider-idle` | 真实 Provider 在 `run()` 前等待 80 ms，再经 Python `start_background` 正常启动，停止后再次等待 | 83.21 ms；就绪成功；停止后 5.05 us 返回 false；线程收集 PASS |
| r4 `margin` | 将实际 Controller `run()` 延迟 10.25 s，仍使用普通 Python 15000 ms waiter 与真实 NFD/Core 启动 | 12.3937 s 就绪；未误报超时；线程收集 PASS |
| r5 `cancel` | 主 Face 使用真实 NFD；Core 独立 probe Face 接入静默 Unix endpoint，accept 后取消实际 Controller | 等待 wall 302.74 ms / CPU 0.565 ms；停止 2.293 ms；run/wait 线程均收集；拒绝原因断言 PASS |

每行原始记录位于 `spec181-t004-lifecycle-20260905-rN/`，含
`run.log`、`case/controller.log`、`case/lifecycle-probe-result.json`。
三次 probe 均 exit 0；结果仅在子进程收集及 `runtime.stop()` 返回
成功后写入。结果字段 `childrenCollected`、`networkCleanupCompleted`
均为 true。测试前后原有 5 个 NFD PID/PPID 不变，无新增遗留 NFD。

R1 的 6 项 Core 检查沿用本次未修改的 Core 源码/库；Python readiness
与 grant parity 日志分别在 r3 的 `python-readiness.log` 和
`grant-parity.log`。构建与 probe 使用上列 manifest 身份；这里只证明
T004 生命周期边界，不将测试延迟、静默 endpoint 或 grant parity
解释为完整协议资格。
