# T002 Native Handler Source Closure

**Status**: IN_PROGRESS
**Evidence layer**: executed (focused unit)

## First Boundary

Merge 源码提交 `7ff25893` 后核对 handler：准备 metadata 校验未
比较 Selection 的输出形状，允许 K=300 的契约被准备 spec 改成
K=1。R1 `NativeYoloMergeBindsPreparedOutputBudgetToSelection` 失败，
1/2 assertions PASS，exit 201；Waf 构建 PASS（34.541s）。测试
调用生产 projection builder 和 `validateNativePreparedRunnerSpec`，
固定有效正例后仅替换输出预算。

原始 build/red 日志保留在 ignored workspace temporary directory 的
`spec181-native-handler-20260905-r1/`。修复先补精确输出契约比较，
随后完成 handler 请求输入与 factory 的生产源码闭包。4/12，T007 BLOCK。

## R2 Observation Boundary

修复后的 Waf build 尚在运行时，误启动了测试；该调用执行的是旧
binary，不能评价修复。`spec181-native-handler-20260905-r2/green.log`
保留但标记 INVALID（validation started before build completion）。
继续等待同一个构建进程，不重新启动构建；完成后使用新日志记录
定向结果。这是验证编排错误，不是新的协议结果。

## Rebuilt Focused Result

原构建结束并确认 exit 0（59.612s）后，`rebuilt-green.log` 记录
`NativeYoloMerge*,NativeV3ProtectedRuntime*,NativePreparedRunner*,NativeProviderRuntimeReadiness*,NativeProtected*,ProtectedRuntime*,NativeGrantVerifier*`
为 **46 cases / 242 assertions PASS**，exit 0；K 预算替换已拒绝。
T002 handler/factory 源码闭包仍在核对。用户进一步要求比较 Qwen/YOLO
的公共机制复用，下一步据当前实现明确 Spec 的共用路径和适配器边界。
