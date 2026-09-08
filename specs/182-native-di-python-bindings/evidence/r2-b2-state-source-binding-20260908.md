# R2-B2 Explicit State Source Binding

## Design

基线 ff2e83d7。Qwen 候选使用抽象状态契约，而实际导出的状态张量可能为具体
形状或多个 tensor。当前 role producer 严格逐字段比较会拒绝这种合法 export，
不能删掉检查。新增显式 NativeStateTensorMapping（inputs/outputs，各为 role→
semantic state name→actual tensor names）；由 authenticated export configuration 提供。

SB-1：复用实际 source boundary 推导，bindStateContracts(model,candidate,mapping,control)
要求逐个覆盖候选声明状态，每个实际 tensor 在对应角色源边界、dtype 一致且不重复。
shape/字节预算只取源 metadata，不由 mapping 自报。完整 candidate digest 重新计算；
调用必须在候选选择/ACK 放置前，不能偷偷修改已选择候选。
SB-2：catalog 暴露同一绑定入口，不要求 bootstrap 另建 source inspector；保留
freeze/identity/owner/cancellation 门。SB-3：C++ actual-source 正负例与共享回归。
本批不改变 pure Qwen splitter 的既有结果，不假定抽象 rank/shape 就是 ONNX shape；
tensor layout 语义仍须由已认证 export 配置及 generation owner 绑定。

新增方法/值类型，不修改既有 class layout。共享 Spec182Preparation、NativePlanning、
CanonicalPublisher、V3Placement、OnnxExtraction，批末一次兼容增量 -j4 构建。
IN_PROGRESS；T008-A 和默认 requester 未关闭。

## Static Review

SB-1/SB-2/SB-3 STATIC_PASS / TESTS_DEFERRED。已加载官方 review-agent，当前执行者
只读复核新方法、catalog 转发、原 prepare/候选校验及全部新测试。复用 prepare
产生源边界，内部临时候选仅清空抽象 state，并重算摘要以通过其余身份检查；
它不会返回或发布。绑定后保留完整其他候选字段，只重建 source-derived state。
No findings；静态修正测试模板逗号的宏表达式，避免 BOOST_CHECK 宏误分参数。
真实 Spec175 源固定 hash、三类状态 [4,8]/128 bytes，另用 inline/external source
覆盖 catalog 转发、缺失/外角色/外语义/错误方向/重复/空映射、dtype 与取消负例。
READY_FOR_BATCH_TESTS。仍不证明真实 Qwen 大模型布局或多 Provider generation。

## First Build Boundary

r1 build exit 1，泛型 lambda 中 `size.get<uint64_t>()` 缺 dependent-template
消歧关键字；原日志 [build.log](../../../.codex-tmp/spec182-r2-b2/build.log)。修正为
`size.template get<uint64_t>()` 并复审调用/返回类型，测试尚未运行；状态 PARTIAL，
失败不计验收。后续使用新 r2 日志目录，复用原树增量，不 clean 或重建 Core/UAV。

r2 build exit 0（18.162s），[focused.log](../../../.codex-tmp/spec182-r2-b2-r2/focused.log)
exit 201、70/71 cases PASS；首边界为真实 causal source inspection 的重复 graph consumer。
维护 ONNX node/summary 的输入引用保留 operand 次数，而 NativeGraphSnapshot 要求
唯一 consumer。Concat 的 hidden_zero 在一个节点出现六次，另一个节点一次。
修复只影响规划 edge consumer 去重，原 node inputs/metadata/digest 域保留七次，
新断言同时要求 2 个消费者和 7 次引用。此实际合法源处理修复扩展 SB-1 Write 至
NativeOnnxRecipeAssembler.cpp；不弱化图校验、不改模型字节或冻结 expected。
官方只读复核去重范围与新正例后 READY_FOR_BATCH_TESTS；后续 r3 独立保留日志。

## Final Result

**DONE (batch only)**。r3 `waf -o .codex-tmp/spec182-yolo-semantic-r1/build build --targets=unit-tests -j4 -v`
exit 0，**30.522s**；只编译 ONNX inspector 和 publisher test，再链接 unit-tests，
[build.log](../../../.codex-tmp/spec182-r2-b2-r3/build.log)。沿用 system g++/binutils、
Boost 与既有 tokenizer target；有效 vmstat 采样 si/so=0，Core/UAV 没有重编。
r2 的成功构建耗时 18.162s；本批包含 r1 编译失败与 r2 行为失败，不只统计最后一次。

`timeout 90s .codex-tmp/spec182-yolo-semantic-r1/build/unit-tests --run_test=Spec182Preparation,Spec182CanonicalPublisher,Spec182NativePlanning,Spec182V3Placement,Spec182OnnxExtraction --report_level=detailed --log_level=message`
exit 0，**71/71 cases、1649/1649 assertions PASS**；见
[focused.log](../../../.codex-tmp/spec182-r2-b2-r3/focused.log)。真实 causal 源 29 条断言，
组合 catalog 用例扩展至 60 条断言；具体 shape/size、固定 source hash、元数据引用次数
与规划消费者数量都来自独立源/已知图结构，不更改 expected 迎合实现。
`validate_design.py` exit 0/errors=[]，见 [design.json](../../../.codex-tmp/spec182-r2-b2-r3/design.json)；
最终文档/diff 提交前复核。case-manifest 已登记新实际 suite/case。

本批没有运行真实 generation、integration、MiniNDN；大模型 export 配置、generation
state-layout 消费、实际 bootstrap/requester 仍需后续验收。调用者必须持有认证配置，
在 placement 前调用 bindStateContracts 并使用其返回的完整新候选，不能沿用旧摘要。
Context Mode active hash 过期，使用 canonical 仓库源与原始日志；其他会话两份设计
改动仍隔离。下一步接通实际配置调用与 T004/T010 的角色数据流/请求编排。
