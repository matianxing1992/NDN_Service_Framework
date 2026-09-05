# T002 Native Production Launch Repair

**Date**: 2026-09-05 | **Source baseline**: `516c1754` + working-tree wiring
**Layer**: implemented / focused repair | **Status**: OPEN

## Controlling Boundary

A01：受保护 Y-B 的 process-spec 分支仍强制选择 Python Provider。
`test_spec181_native_production_launch.py` 首次定向运行 1 failed：
`di-native-provider` 未出现在生成命令中。原始记录在忽略的工作区
临时根目录 `spec181-t002-native-live-20260905-r1/launch-red.log`。
该 RED 是 launch-vector unit，没有运行网络。

## Bounded Validation Plan

移除自动 Python 替代后先检查 launch vector 与既有明文入口；随后在
新的独有目录执行一次 native 受保护请求，用于关闭 A01 获取/装配
接线。复用维护 runner 的监督与清理，不采用重试聚合器，不删除旧
目录，不把该定向复现作为 T008 正式资格。源/构建与有效输入由现有
native build manifest 和 case runtime 记录。T007 保持 BLOCK。

旧 `/tmp/spec181-yb-retry/attempt-6/controller.log` 为 0 bytes；
`yb39.log` 六次仅记录外层 `CASE_RUNTIME_PROCESS_START_FAILED:control`，
没有异常原因，不能继续据此重试或归因网络。本次单次调用保留完整
异常链以解析 launch 边界。名称映射另发现生成计划采用
`/Model/YOLO26n`，注册表采用 `YOLO26n`；仅剥离明确单组件 `/Model/`
前缀，不接受任意命名空间或多级名称作为同一族。

## R1 Results

launch-vector 修复后 3 tests PASS；native family/runtime/storage 定向
14 cases PASS；新统一构建 exit 0 / `SPEC180_NATIVE_IDENTITY_OK`，
重建扩展的 3 grant parity tests PASS。build manifest SHA-256：
`ccebc37b16c5ab0cc554a313e0de8e38e8a913077df845d020a72f3baa1c8033`。

单次复现命令：`unshare --user --map-root-user --net --mount --pid
--fork --mount-proc bash <r1>/run-once.sh`。脚本读取既有 Y-B 输入后，
在私有命名空间隔离 `/run` 与 `/tmp`，防止旧 MiniNDN 全局清理影响
此前五个 NFD 或旧日志。输出为 r1 的 `case/` 与 `run-once.log`。

结果 exit 1 / **UNQUALIFIED**。真实异常链首次确认
`CASE_RUNTIME_PROCESS_EXITED_BEFORE_READY:controller:returncode=-11`，
再包装为 `CASE_RUNTIME_PROCESS_START_FAILED:control`。
Controller 原始日志为 0 bytes；首个边界是 Controller startup SIGSEGV，
不是 grant 拒绝或协议结果。Provider/User 均未启动。后续只做该崩溃
的最小启动复现，再决定新 native 请求；不得自动重试整轮。

## Controller Probe R1

最小 `controller.py --help` 导入 exit 0。首次仅启动 Controller 的
probe 未复现原 SIGSEGV：其环境漏掉正式运行器显式加入的 operator
user-site，节点 HOME 下报 `ModuleNotFoundError: onnxruntime`，进程
exit 1。保留 `spec181-controller-startup-20260905-r1/{probe.log,
case/controller.log}`，这属于复现器环境差异，不是根因或修复证据。
下一 probe 使用正式子进程的相同 PYTHONPATH 顺序。

## Controller Probe R2

恢复相同 PYTHONPATH 后，Controller 到达 `SPEC180_CONTROLLER_READY`
和 `SPEC180_RUNTIME_CATALOGUE_PUBLISHED`，25 秒仍存活，测试主动发送
SIGINT 并收集退出。没有调用 Provider/User。保留
`spec181-controller-startup-20260905-r2/{probe.log,case/controller.log}`。
该复现器尚未调用原流程的 `configure_routing`，因此不能关闭原崩溃。

当前可证伪假设：① 路由/启动网络活动触发 native 初始化竞态，加入
同一路由后应复现；② 完整子进程环境仍有差异，逐项对齐后行为应随
差异变化；③ native 初始化本身非确定，完全相同输入也可能交替结果，
需捕获实际崩溃栈。R3 只加入既有路由步骤，不改协议或 SVS 参数。

R3 同样完成 Controller 就绪和 catalogue 发布，25 秒仍存活；记录在
`spec181-controller-startup-20260905-r3/`。仅加入路由未复现，不能把
路由判作已证实根因。R4 复用原 `_run_live_case_once` 的完整前段和
环境，只在即将启动 Repo 时停止，且启用 fault handler；该停止点
是诊断控制，不代表任何业务拒绝或资格结果。

R4 从原入口运行到 `SPEC180_RUNTIME_CATALOGUE_PUBLISHED`，按停止点
输出 `CONTROLLER_COMPLETE_BEFORE_REPO`，进程及网络均完成收集；记录
在 `spec181-controller-startup-20260905-r4/`。三个缩小测试通过仍不足
以关闭原 SIGSEGV。随后 A01 live-r2 采用同一构建及输入、启用
`PYTHONFAULTHANDLER=1`，仅一次请求，以继续验证 native 授权/装配并
在崩溃复发时捕获栈；保留 live-r1，不合并为资格结果。

## Live R2 Binding Failure

`spec181-t002-native-live-20260905-r2/{run-once.log,case/}` 保留一次
原生请求。Controller、Repo 和四个 native Provider 全部就绪，User
完成 ACK/Selection；四个角色随后均报
`protected runtime binding is incomplete`，User exit 1。本轮
**UNQUALIFIED**，首个业务失败边界在授权绑定，尚未装配或运行 ORT。

源码确认 `ProviderGroupCoordinator` 对 epoch key 和 capability
canonical bytes 的 SHA-256 均输出无前缀的 64 位小写十六进制；
Python group capability 使用同样格式。`ProtectedRuntime` 错把这两
个字段当作带 `sha256:` 前缀的计划摘要，原单元 fixture 也使用错误
格式。修复应匹配既有 group wire contract，保留完整字段严格校验与
exact binding 比较；不得改变冻结的 group wire 格式。

Controller 原 SIGSEGV 仍 OPEN。下一次针对绑定修复的单次复现使用
独有目录和定向 `NDN_LOG`；如崩溃复发，保留 fault handler/GDB 栈，
不输出密钥或栈局部变量。T007 仍 BLOCK。

## Group Binding Regression

定向目录 `spec181-t002-group-binding-20260905-r1/` 的 `red.log` 记录
原实现对有效 group wire 摘要抛出 `invalid_argument`（2 tests，1
failure）。修复将 group 字段单独校验为严格 64 位小写 hex，并给出
group 专用错误；plan/security/grant 摘要仍要求 `sha256:`。回归还
覆盖空值、长度、大小写、非法字符、错误前缀和替换 epoch key ID。
旧 group fixture 同步为真实 wire 格式。

修复后 `green.log` 为 20 C++ cases PASS；`launch-green.log` 为 3
Python launch tests PASS。第二次实际 native/library/extension 构建
exit 0 / `SPEC180_NATIVE_IDENTITY_OK`，构建记录在 live-r3；其
manifest SHA-256 为
`509c9a85b72602ac5969bd27c55810302dec7a98297daa0ada5a99e6c0c4d935`。
live-r3 使用该构建，Controller 由 batch GDB 监督，关闭栈参数输出；
ServiceProvider 单模块 DEBUG，保留相同网络/进程隔离。它仍是 A01
修复诊断，不是 T008 或正式矩阵。

## Live R3 Grant Fetch Failure

live-r3 的单次请求越过 group binding 验证，但四个 native Provider
均报 `DI_PROTECTED_GRANT_REJECTED: exact grant Data fetch timed out`。
User 超时退出；本轮 **UNQUALIFIED**，装配/ORT 未到达。新扩展
3 grant parity tests PASS；它不能代替网络 fetch 验收。

`case/controller.log` 记录 GDB 下 READY、catalogue 发布及清理时的
SIGINT/KeyboardInterrupt，没有 SIGSEGV；原间歇崩溃仍未关闭。
下一诊断核对真实 APP Data 发布名、精确 Interest 与现有路由，不
通过放宽 CanBePrefix、跳过授权或重试聚合来获得 PASS。
