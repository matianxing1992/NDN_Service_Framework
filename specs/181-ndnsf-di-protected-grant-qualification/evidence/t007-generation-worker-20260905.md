# Shared Generation Worker Authority

**Status**: PASS (focused repair); T007 BLOCK
**Evidence layer**: implemented / wired / executed (focused RED and GREEN)

## Finding

FR-015 调用链核对发现，`NativeEpochCoordinator` 在提交角色前后
调用 `stopCheck`，但 `NativeProviderRuntime::executeRoleAsync` 与
registered-runner `ProviderRoleWorker::executeAsync` 没有传递公共
`executionGuard`。排队中的角色可能在取消/截止后仍进入模型；现有
prepared-runner 检查不能证明生成入口也受保护。handler 也尚未把
已有 ProtectedRuntime guard 交给 generation coordinator。

## RED

原始目录为 ignored workspace temporary directory 下
`spec181-generation-worker-20260905-r1/`。Waf `unit-tests` 完成后
（2m20.626s，exit 0）才执行 3 项实际 runtime/coordinator 回归。
结果 **1 case PASS / 2 cases FAIL；14/18 assertions PASS**，exit 201。

测试占用唯一 worker，确认 generation role 已在 ready queue，再
触发取消或截止并释放 worker。两个新用例均实测模型调用 1 次，
而预期为 0；随后出现缺失 decode-state output 错误，而非
`ATTEMPT_CANCELLED` / `REQUEST_DEADLINE`。执行前取消的已有检查
通过。因此首个缺口在 worker 消费排队工作时，不是模型格式或网络。

## Repair and GREEN

沿既有 runtime/worker 入口传递同一个 guard；coordinator 的
stopCheck 与 ProtectedRuntime guard 在已有 worker 执行、缓存、
发布及 runtime 状态提交边界复核，继续使用原清理/回滚 owner。
测试覆盖排队停止与实际 grant 失效；不增加 Qwen 模型资格。
本记录不关闭 T007，不授权正式矩阵。

R2 原始目录 `spec181-generation-worker-20260905-r2/`。先等待 Waf
`unit-tests` 构建完成（3m9.876s，exit 0），再运行
`NativeEpochCoordinator*,ProtectedRuntime*,NativePreparedRunner*,NativePreparationContext*,NativeYoloMerge*`：
**48 cases / 366 assertions PASS**，exit 0。

新检查覆盖 generation 排队取消、截止、独立 authority callback 失效；
registered runtime 使用真实签名 grant/verifier，检查计算中取消/
过期和缓存命中后取消/过期的拒绝、明文 lease 清理及 Zeroized。
现有生成状态/回滚接口与 YOLO adapter 数值检查一并通过。

当前工作区调用链：`NativeProviderHandler.cpp:2670` 交付已有
ProtectedRuntime guard；`NativeEpochCoordinator.cpp:52,937` 组合并
传递 stopCheck/authority；`NativeProviderRuntime.cpp:1296,1450,1476,1484`
覆盖提交入口及状态候选/提交；`ProviderRoleWorker.cpp:128,148` 的
两个 registered overload 转发给同一 `executeAsyncImpl`。文件路径
均相对 `NDNSF-DistributedInference/cpp/ndnsf-di/`，行号对应修复工作区。

检查运行在当前工作区；其他预存 generation 改动不因本修复获得资格。
本轮只重建 unit target；公共 C++ API 已变化，下一次 native live
control 或候选封印前须重新刷新统一 native manifest，不复用前一
公共准备控制的 manifest 作为新源身份。
