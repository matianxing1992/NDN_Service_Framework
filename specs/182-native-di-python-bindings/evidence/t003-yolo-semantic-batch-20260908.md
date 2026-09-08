# B-G1-YOLO-SEMANTIC

## Baseline and Scope

源码基线 7552523b。工作树另有五份 speckit-code-design skill 及两份 native
design 文档修改，本批不拥有这些修改。批次成员、共享构建和测试选择器由 plan
的 B-G1-YOLO-SEMANTIC 定义；当前状态 PARTIAL / focused batch PASS。

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
后统一运行计划选择器。实现与审查结果见下节；测试待执行不宣称行为 PASS。

## Implementation Review

YS-1/YS-2/YS-3 已实现：fromOnnxCatalog 从实际 source 调用 shared inspection，
完整验证 semantic partition 后产生绑定模型身份和实际图的 splitter；enumerate
使用持有图事实。typed constructor 仍接受已解析 planning IDs，不宣称执行 catalog
校验。注册摘要由 catalog owner 提供，不将 digest 字段当作网络认证。

逐成员 STATIC_PASS / TESTS_DEFERRED / B-G1-YOLO-SEMANTIC；组合审查覆盖
source/control→inspection→名称归属→tensor/dependency/cut/role interface→splitter
enumerate→完整 candidate identity。修正测试宏模板逗号，并补工厂返回前 deadline/
cancel fence。实际 Python splitter 生成真实四节点分支 ONNX oracle，名称包含中文
tensor 与 unnamed Relu；测试直接调用生产 factory/enumerate，并比较完整候选及
九类 partition 篡改拒绝。模型 revision 和外来 graph identity 拒绝，调用方提供的
图事实不覆盖源图。default requester/模型目录的网络读取仍属于后续接线，未实现。

READY_FOR_BATCH_TESTS 后，新增 class 状态采用 fresh tree configure/build，-j4 串行
build PASS（6m48.938s）。已核对 system g++ 9.4、ld 2.34、Boost system 路径与
原 NAC-ABE/NDN-SVS/ONNX 前缀；vmstat 第二次采样 si/so=0。
实际相关 52/52 cases、1277/1277 assertions PASS，含完整候选 canonical bytes/
digest 与真实 Python splitter 一致、九类 semantic 篡改拒绝以及 preparation/
publisher/V3 placement 回归。未运行 integration/MiniNDN/Tiger，也未声称旧 Python
extension 已按新 C++ class ABI 重建；绑定及安装消费者的最终 ABI 验收仍保留。

Commands and evidence:

- `python3 tests/fixtures/spec182/author-yolo-semantic-oracle.py`，真实 Python oracle 生成 PASS。
- [configure](../../../.codex-tmp/spec182-yolo-semantic-r1/configure.log)：fresh prefix / with-tests / with-examples / system toolchain，精确参数保留在 build/config.log。
- [build](../../../.codex-tmp/spec182-yolo-semantic-r1/build.log)：`waf -o .codex-tmp/spec182-yolo-semantic-r1/build build --targets=unit-tests -j4 -v`。
- [focused](../../../.codex-tmp/spec182-yolo-semantic-r1/focused.log)：`timeout 60s .codex-tmp/spec182-yolo-semantic-r1/build/unit-tests --run_test=Spec182NativePlanning,Spec182Preparation,Spec182CanonicalPublisher,Spec182V3Placement --report_level=detailed --log_level=message`。
- [first document check](../../../.codex-tmp/spec182-yolo-semantic-r1/design.json)：下述 workflow anchor/rule 失败保留；[repaired document check](../../../.codex-tmp/spec182-yolo-semantic-r1/design-repaired.json) exit=0 PASS。本批 diff check PASS。

## Documentation Check Boundary

批次记录 checkpoint f9e5e257。该轮全局 validate_design.py exit=1：并行修改的
contracts/pre-test-static-review.md 缺 `## One Completion Record`、`最小具名诊断`
及对应 anchor。原始输出见
[validation](../../../.codex-tmp/spec182-preparation-rank-r1/semantic-batch-design-final.json)。
不把该失败算作产品/YOLO 行为失败，也不宣称全局文档 PASS；未覆盖或回退他人工作流
修改。该文档计划轮 diff 格式检查通过，三个批次文档引用存在；当时未运行产品构建或测试。

后续 e31e96c7 将工作流正式集中到共享 skill，确认上述检查是旧路径/anchor 失配。
本批修复 validate_design.py：继续检查 feature 的 ownership/阶段条款，同时在共享
规则文件验证 One Completion Record、最小具名诊断及 Batch Static Gate；proof-design
链接指向真实共享 anchor。不恢复重复规则或降低门禁，独立重跑 exit=0 PASS。
