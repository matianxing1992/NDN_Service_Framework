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

源码显示维护 runner 使用 `NdnRoutingHelper.calculateRoutes()` 预装
requester identity 路由，却额外强制设置假定由 NLSR 宣告的
`/ndn/<user-node>-site/<user-node>` forwarding hint。下一 live-r4
仅在诊断启动器中移除该 hint，记录各节点 FIB 并开启 ServiceUser
发布/Interest 日志：若精确 grant 成功获取，支持错误 hint 导致
超时；若仍失败，再区分发布/路由/IMS 边界。尚未改变正式源码。

## Live R4 Routing Diagnosis

移除 hint 后 User 日志记录 4 次 grant 发布和 4 条对应精确 Interest；
四个 native Provider 均越过 grant 获取/验证。FIB 记录确认远端节点
存在 requester identity 路由，也存在 router-name 路由；所以不能
把原因简写为「所有节点都没有 router 路由」。移除非必需 hint 后
原超时消失，正式 runner 应直接使用其已安装的 identity 路由。

首个失败变为 BackboneNeck 的 `DI_PROVIDER_ASSEMBLY_PATH_UNSAFE`；
其余角色随后因上游未输出而 dependency fetch 失败，User 超时。
本轮 **UNQUALIFIED**。源码显示受保护装配将解密副本命名为
`loaded-model.onnx`，而现有 prepared-runner contract 严格要求
`model.onnx`。修复保持私有 leased staging 目录，只改副本 basename，
不得放宽路径校验。原始记录为 live-r4 的 FIB、User/Provider 日志。

路由环境回归 `route-red.log` 先复现错误 hint 被继承；修复后
`route-green.log` 为 5 tests PASS。正式 runner 不再强加该 hint，
并清除继承的同名变量，使用已安装的 requester identity 路由。
live-r4 清理后缓存保留 `model.onnx.cipher`（9128285 bytes）；扫描
未发现 `.onnx`/`.onnx.data` 明文或残留 staging 子目录。此证据仅
覆盖本次失败后的文件清理，不替代 ORT 成功/取消/过期生命周期。

## Live R5 Build Identity

文件名修复后的 native/library/extension 统一构建 exit 0，输出
`SPEC180_NATIVE_IDENTITY_OK`；manifest SHA-256 为
`802d5cb14c3c252f8d7d42671df526073847dd16949c0236c812818070df0df4`。
live-r5 使用正式 runner 路由，沿用 live-r3 的 GDB-only Controller
包装，不再 monkeypatch grant hint。构建与单次日志独立保留。

## Live R5 Captured Startup SIGSEGV

live-r5 在 Controller READY 之前再次崩溃；GDB 成功捕获，外层
Controller returncode 255 是 debugger 的返回值，不应误读成新的
应用错误。`case/controller.log:13--40` 记录 thread 7 SIGSEGV：
`ndn::UnixTransport::resume()` → `Face::Impl::ensureConnected()` →
`Face::Impl::expressInterest()` → `ServiceController::start()` 的
`io_context::run_for()` → `NativeServiceController::runControllerLoop()`。
本轮 **UNQUALIFIED / startup**；Provider 未启动，文件名修复尚未
完成真实执行验收。新扩展 3 grant parity tests PASS。

后续缩小为 Controller 启动诊断，检查 transport 的非密钥状态与
对象生命周期。该栈定位了实际崩溃边界，但尚不能在未检查指针/状态
前断言根因为连接竞态、对象销毁或残留回调。保留 GDB 参数屏蔽，
只检查 transport 指针与状态，不打印局部业务对象或私钥。

## Controller Probe R6

`spec181-controller-startup-20260905-r6/` 的完整启动前段再次捕获相同
SIGSEGV。NDN 日志显示 connect、4 次 resume、close，然后约 10 ms
后再次 resume 并崩溃。优化后的 ndn-cxx 栈没有可用 `this` 符号，
GDB 状态查询因此停止；不得据此声称已经确认空指针状态。下一次
最小 probe 在 `UnixTransport::close` 设置自动继续的断点，仅输出
关闭者栈，以确定关闭发生在哪个 owner，避免猜测。

R7 最小 probe 到达 READY/catalogue，停止在 Repo 之前；没有捕获
关闭断点，不能因此关闭 R5/R6 的失败。源码另发现可证伪的等待
竞态：`start_background()` 启动 Python thread 后立即等待，而
native `waitUntilReady()` 将初始 `m_running == false` 当作终态。
调度尚未进入 `run()` 时会提前返回 false，Python 随即 `stop()`；
晚到的 `run()` 又重置取消状态。下一 probe 在启动线程前显式调用
短时 readiness wait，验证它是否错误地立即返回，而非等待期限。

R8 确定性 RED：在真实 node 环境构造 native Controller，尚未启动
线程时调用 `wait_until_ready(80)`，仅 **0.00001462 s** 就返回 false，
触发 `SPEC181_READINESS_PREMATURE_TERMINAL`。这确认初始 idle 被错误
当作完成/失败终态；正常控制路径会随即调用 stop。修复以显式启动
终态（就绪、失败或取消）唤醒等待者，不以初始 running=false 判定。

生产修复位于 `NativeServiceController`：新增由 `m_startMutex`
保护的完成状态，初始 idle 不唤醒 waiter；start/run 开始时清除，
就绪、异常和 stop 时设置。stop 清除 ready，避免已停止对象仍报告
就绪；成功路径也再次检查 running。真实探针维护在
`tests/fixtures/spec181/controller-readiness-before-run.py`，Python
接口 seam 7 tests PASS；最终以重建 native 探针结果判定本缺陷。

R9 统一构建 PASS，manifest SHA-256
`49f77e3d3dd57842ad6c930eeb79a00f40d0d50c5df1c34672a5e6b6fe027e64`；
重建扩展 3 grant parity tests PASS。真实 native 探针记录
`ready=False elapsed=0.08105735`，随后正常 READY、catalogue 发布，
在 Repo 前按设计停止并完成清理。确定性 pre-thread readiness
缺陷 **CLOSED**；此单次探针不代表所有取消/超时/热转验收完成。
下一 live-r6 使用同一构建与正式 runner，继续先前装配修复的单次
请求诊断，并保留 GDB。T007 保持 BLOCK。

## Live R6 Native Execution and Debugger Cleanup

同一构建下三个 ONNX Runtime Provider 与 native Merge 执行，User
输出 `YOLO_ACK_DRIVEN_RESULT status=true payload_bytes=1267`，plan
digest 为 `sha256:6506d58ed380aacf83764e799fd58f7ef94b9d6410dd4b4bb5c3ce3921bf80cf`。
装配 basename 和启动等待修复已越过原失败边界。但 runner 最终
报 `CASE_TERMINAL_CLEANUP_FAILURE:controller:255`：GDB 在清理 SIGINT
后以 255 返回，而监督器期望普通 Python Controller 的退出语义。
本轮整体仍 **UNQUALIFIED / debugger cleanup**，不得作为正式 PASS。

下一 live-r7 移除 GDB 包装，保留相同构建、正式进程命令和定向
日志，仅验证正常清理/采集边界；不放宽 exit-code 验收，不删除 r6。

## Live R7 Focused Acceptance

普通进程命令、同一 r9 构建下，单次入口 exit 0，输出
`SPEC180_CASE_RESULT status=PASS case=Y-B`；`subcase-result.json` 为
`spec180-yolo-subcase-result-v1 / PASS / CONTROL`。User 返回
`status=true payload_bytes=1267`，plan digest 为
`sha256:463f1f605e940b6a1b11fb2618fa2714ac82f023e50b1631aaa40a20a61a6b52`。
三个角色使用 ONNX Runtime CPU，Merge 使用 native postprocess；
这是受保护 native 请求的定向正例及正常清理证据。

清理后保留 3 个 `model.onnx.cipher`，未发现 `.onnx`/`.onnx.data`
明文或 staging 子目录；本次 NFD、Controller、Provider 均被收集，
五个原有 NFD 保持运行。所有此前失败目录仍保留，未使用重试聚合。

**Verdict**: focused repairs PASS；T002/T004 partial；T007 BLOCK。
尚未完成生产负例、全部资源/取消/过期验收，也未运行正式 T005/T008。
factory/assembler/handler 仍含先前工作区接线，后续须单独审查并形成
可提交源码闭包；本记录的二进制由 manifest 全部源哈希标识，不能
仅以检查点 HEAD 代替该构建身份。
