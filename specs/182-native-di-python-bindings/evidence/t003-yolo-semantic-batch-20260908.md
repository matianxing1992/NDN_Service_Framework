# B-G1-YOLO-SEMANTIC

## Baseline and Scope

源码基线 7552523b。工作树另有五份 speckit-code-design skill 及两份 native
design 文档修改，本批不拥有这些修改。批次成员、共享构建和测试选择器由 plan
的 B-G1-YOLO-SEMANTIC 定义；当前状态 PARTIAL / DESIGN_REVIEW。

## Source Review

NativeYoloComponentSplit::enumerate 当前把 nodeNamesByRole 的字符串直接作为
graph.nodes[].id；但维护 Yolo26Splitter._role_assignment 使用 graph_node_names
将注册 semantic node names 映射到 topological_order IDs。实际 ONNX inspection
返回两者，不能忽略名称映射。旧 fixture 使用同名节点不能证明真实 catalog 可用。

维护算法还验证 tensorInterfaces 的生产者、跨角色消费者、dtype/shape，
dependencyEdges/safeCuts 与实际 role-pair tensor 集合一致，并从 graph_metadata
推导角色输入输出后对照 roleInterfaces。当前 C++ component spec 没有这些输入，
因此只补映射或复用当前小型 fixture 都不能关闭 T003-B。

默认 NativeInferenceClient::dispatchOperation 仍在 PreparingInput 后返回
NATIVE_REQUEST_PIPELINE_NOT_READY。该事实不因前两轮 publication 修复而改变。
T008-A 的既有依赖未关闭，本轮没有绕过它新建默认 requester 的简化路径。

## Next Implementation and Proof

YS-1/YS-2 增加 catalog partition 与真实 inspection 的完整校验；YS-3 从生产
splitter 入口消费已核对的 partition。保留 registered candidateDigest 和完整
SplitCandidate digest 的区别。禁止从 semantic 名称猜 ONNX index，或把 Qwen
semantic layer 当作 ONNX 单节点。新增接口先同步 runtime/strategy 契约。

独立判据采用维护 Python Yolo26Splitter 对实际 ONNX inspection 的输出；
正例节点名必须与 onnx-node-N 不同。负例独立修改名称 cover、tensor 端点、
dtype/shape、依赖、safe cuts、角色外部输入输出。批内不逐小任务构建；静态完成
后统一运行计划选择器。尚未实现上述成员，不能记录 STATIC_PASS 或测试 PASS。

## Documentation Check Boundary

批次记录 checkpoint f9e5e257。最后全局 validate_design.py exit=1：并行修改的
contracts/pre-test-static-review.md 缺 `## One Completion Record`、`最小具名诊断`
及对应 anchor。原始输出见
[validation](../../../.codex-tmp/spec182-preparation-rank-r1/semantic-batch-design-final.json)。
不把该失败算作产品/YOLO 行为失败，也不宣称全局文档 PASS；未覆盖或回退他人工作流
修改。本批 diff 格式检查通过，三个批次文档引用存在；没有运行新的产品构建或测试。
