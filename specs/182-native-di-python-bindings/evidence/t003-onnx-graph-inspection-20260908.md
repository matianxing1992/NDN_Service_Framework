# T003 Owned ONNX Graph Inspection

## Changes and Review

源码基线 354a54a1。源码审计确认 NativeModelAdapter 仍只有测试实现，preparation 的
InspectPort 仍依赖注入；既有原生 ONNX helper 只提供装配摘要，没有返回规划图事实。
本批增加 inspectNativeOnnxPlanningGraph，从实际 owned ONNX bytes 生成维护算法的
adapter-bound planning graph、语义 node names、原始 ONNX node indices 和 graph metadata。

复用 ownedSourceModel（含严格 external tensor 处理）、官方 ONNX 1.17 InferShapes
及既有原图/initializer identity，不新写 parser。推断在副本上执行，失败恢复原图；
检查推断没有重排/改变节点接口。未知维度/算子类型保留，标量与零维度预算按维护算法，
超出 uint64 的 tensor size 明确拒绝。规划 identity 绑定完整 adapter descriptor 摘要，
并匹配 expectedModel.graphDigest；原始装配 identity 单独返回，不能混为同一图摘要。
NativeGraphSnapshot 同时补空 opType 拒绝，匹配 GraphNodeView 要求。

审查覆盖 source/limit/cancel、推断副本、tensor 元数据优先级、initializer 输入排除、
原始节点顺序、producer/consumer、全部顺序 cut 的并集和两类 graph identity。
六组实际 Python ONNX oracle 对照每个节点/edge/tensor/metadata/摘要，负例覆盖不同
adapter ABI、不同源图、错误摘要、解析失败、源大小限制、deadline 与 cancel。

## Validation

PARTIAL。Python ONNX 1.17.0 生成 6 组 oracle；无既有结构布局变化，仅新增
函数/返回类型，复用上一批 ABI tree 增量 -j4 build PASS（49.585s）。工具链/Boost/NAC-ABE cache 已复核。
首轮 focused rc=201，112/114 cases PASS；两个新 case 的 fixture 没传必需 requireActive，
既有 checkActive 在解析前返回 ASSEMBLY_TIMEOUT，不能解释为真实超时或图算法失败。
修复 fixture 传显式回调，并保留空 callback 拒绝测试。r2 build PASS（40.739s），
113/114 cases PASS；唯一失败为 test reader 读取 op_type，而维护 GraphNodeView 的
字段是 operation。修正 reader（产品逻辑不变），独立 r3 build PASS（23.480s），
114/114 cases、2212/2212 assertions PASS；binding 源码语法编译、文档及 diff 检查 PASS。
原始失败结果不覆盖，不把 fixture 初始化或 reader 错误当作图算法/协议失败。
源码和测试首次执行前已审查；未运行 integration/MiniNDN/SIF/Tiger。

Commands:

- `python3 tests/fixtures/spec182/author-onnx-planning-graph-oracle.py`
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin python3 ./waf -o .codex-tmp/spec182-t003-candidate-r1/build build --targets=unit-tests -j4 -v`
- `timeout 60s .codex-tmp/spec182-t003-candidate-r1/build/unit-tests --run_test=Spec182CanonicalPublisher,Spec182V3Placement,Spec182Preparation,Spec182OfferAdmission,Spec182NativePlanning,Spec182PlanSealer,Spec182GrantClient,Spec182NativeInferenceClient,Spec182ClientState,Spec182OnnxIdentity,Spec182OnnxExtraction,Spec182NativeAssembly --report_level=detailed --log_level=message`
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/g++ -B/usr/bin -std=c++17 -fsyntax-only -I. -I/usr/local/include -I/usr/include/python3.8 -I/home/tianxing/.local/lib/python3.8/site-packages/pybind11/include pythonWrapper/src/ndnsf/di_bindings.cpp`

本入口不取得或认证网络来源，不把 model contentDigest 假定为原始 ONNX 文件哈希。
当前一一 node mapping 只适用于通用 ONNX planning graph，不替代 Qwen semantic layer
的多节点映射。NativeModelAdapter 生产实现、catalog/semantic interface 校验及
requester 的实际 InspectPort 接线仍待完成，不据此标记完整任务 DONE。

首次暂存遇到并行 Design 写 index.lock；撤出本轮纯文件的暂存，保留对方索引。
Design checkpoint fc78f2be 完成后重新核对差异，再独立提交本批；没有混入文档修订。

Evidence: [oracle](../../../.codex-tmp/spec182-t003-onnx-graph-r1/oracle.log)、
[build](../../../.codex-tmp/spec182-t003-onnx-graph-r1/build.log)、
[first focused failure](../../../.codex-tmp/spec182-t003-onnx-graph-r1/focused.log)、
[r2 reader failure](../../../.codex-tmp/spec182-t003-onnx-graph-r2/focused.log)、
[r3 build](../../../.codex-tmp/spec182-t003-onnx-graph-r3/build.log)、
[r3 focused](../../../.codex-tmp/spec182-t003-onnx-graph-r3/focused.log)、
[binding syntax](../../../.codex-tmp/spec182-t003-onnx-graph-r2/binding-syntax.log)、
[resource sample](../../../.codex-tmp/spec182-t003-onnx-graph-r1/vmstat.log)、
[design check](../../../.codex-tmp/spec182-t003-onnx-graph-r1/design-validation.json)。
