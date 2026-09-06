# T002 Native Merge Source Closure

**Status**: PASS (focused Merge repair); T002 PARTIAL
**Evidence layer**: executed (focused unit)

## First Boundary

真实 Y-B 使用的 `NativeYoloMergeRunner.{hpp,cpp}` 尚未纳入源码
检查点，注册与 `RunnerKind` 转换夹在既有混合修改中。新增直接
回归走真实 projection-to-spec、runner 与 tensor codec，模型输出
使用手工可计算的六路 tensor；不启动网络，不宣称模型资格。

R1 Waf unit target 构建 PASS（18.634s）；`NativeYoloMerge*` 为
3/6 cases PASS、22/26 assertions PASS，exit 201。错误 identity、
阈值尾缀、形状尾逗号和错误 output dtype 未拒绝。排序、K 上限、
空结果、缺失/错误形状输入检查通过。原始 build/red 日志保留在
ignored workspace temporary directory 的 `spec181-native-merge-20260905-r1/`。

## Repair Plan

在专用 YOLO runner 校验其实际实现的后处理 identity，严格消费
数值文本，projection-to-spec 要求唯一 float32 输出。代码及直接
回归与注册/类型转换组成单独源码闭包。T002 未完成、4/12，T007 BLOCK。

## Repair and Validation

runner 现拒绝未实现的后处理 identity、未消费完的阈值文本和尾逗号
形状；projection-to-spec 要求唯一 float32 输出。保留正常置信度
筛选、确定性排序、K 上限及 `[1,0,6]` 空输出语义。

R2 Waf `unit-tests` 构建 PASS（27.584s）；`NativeYoloMerge*` 为
**6 cases / 26 assertions PASS**。连同 `ExecutionEvidence*` 和
`NativeProviderRuntimeReadiness*`，**10 cases / 69 assertions PASS**，exit 0。
原始 build/green/closure 日志保留在 ignored workspace temporary directory
的 `spec181-native-merge-20260905-r2/`。本单元纳入专用 Merge 源码、
生产 backend 注册、证据类型转换及直接回归；handler 准备/输入接线
与其他已有修改继续按单独边界核对。本轮测试基于当前工作区，尚无
新的完整统一生产构建或 MiniNDN 资格声明。
