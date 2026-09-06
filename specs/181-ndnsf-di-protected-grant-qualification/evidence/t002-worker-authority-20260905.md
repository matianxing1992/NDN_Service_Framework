# T002 Native Worker Authority

**Status**: PASS (focused repair); T002 PARTIAL
**Evidence layer**: implemented / wired / executed (focused runtime and worker unit checks)

## First Boundary

R1 原生测试构建 PASS（23.152s）；4 个真实 `ProviderRoleWorker`
回归全部失败（14/28 assertions failed，exit 201）。正式
`ProtectedRuntime` 验证固定签名 grant 后登记明文租约；准备结束或
计算期间改变取消状态/到达过期时间，worker 仍返回结果，租约未在
该边界清理。此处为真实 runtime/worker 的 unit；模型用 lambda
观测执行次数，无网络或模型数值资格声明。

原始 `build.log`、`red.log` 保留在 ignored workspace temporary
directory 的 `spec181-native-worker-20260905-r1/`。

## Repair Plan

由 native handler 传入请求级检查回调；worker 在准备、计算、缓存
结果消费及发布边界调用既有 `ProtectedRuntime`，保持密码学与清理
所有权在该 runtime。回调不携带 wire 字段，不改变调度或放置协议。
保持 T002 未完成、T007 BLOCK，修复后执行定向回归与刷新生产构建。

## Implemented Boundary

`NativeProviderHandler` 仅接纳 `GrantVerified` 的 runtime；受保护
Selection 必须走准备 factory。它把请求级 `executionGuard` 经
`NativeProviderRuntime` 传入实际 `ProviderRoleWorker`。准备前后、
实际计算入口/返回、流式 fallback、缓存消费、事件/依赖发布及最终
返回均检查既有 `ProtectedRuntime::withContentKey`。授权与租约清理
仍归 runtime；不在持有内容密钥锁时执行模型，不新增 wire 字段。
检查拒绝后立即清理受管明文；正在执行的模型不声称可被抢占中断。

## Focused Validation

- R2 Waf `unit-tests` 重建 PASS（2m40.326s）；32 cases / 154 assertions PASS，
  包含全部 4 项原先失败的回归。
- R3 补有效请求及真实 worker 缓存命中、取消后事件拒绝；加强实际
  compute/fallback 和最终返回检查。重建 PASS（26.183s）。
- R3 `NativeProtected*,ProtectedRuntime*,NativeGrantVerifier*,ProviderRoleWorker*`：
  **51 cases / 280 assertions PASS**，exit 0。正例计算一次且第二次缓存
  命中；取消后 sink 收到 0 个事件，登记的明文租约已清理。
- 原始 build/green 日志分别保留在 ignored workspace temporary directory
  的 `spec181-native-worker-20260905-r2/`、`spec181-native-worker-20260905-r3/`；
  r1 失败证据保留。

## Remaining Acceptance

这是实际 runtime/worker 的定向 unit 证据；固定签名 grant 经生产
verifier 验证，模型使用 lambda 观测边界，不宣称新网络或数值资格。
完整 handler/可执行文件的既有源码闭包仍待核对、提交；下一次完整
生产控制前必须刷新统一 native 构建。T002 保持未勾选，4/12，T007 BLOCK。
CodeGraph 返回旧临时快照和过时行号，本轮用当前文件核对调用链。
Context Mode active authority 因任务文档更新而过期，先使用仓库文档，
随后重新索引；工具健康不作为资格证据。
