# R1-B1 Qwen Metadata to Candidate

## Contract and Scope

源码基线 cb771b79，未提交两份 native design 文档属其他会话。用户新设全任务目标
恢复执行，按原 T003-A / T002-A 硬前置领取此批。QM-1→QM-2 为同批实现依赖；
QM-3 统一验收前均不宣称 card DONE。

维护 adapters/qwen/placement.py::build_qwen_three_stage_adapter 从 model name、
revision、precision、contiguous layer ranges 生成 embedding/layer-N/final-norm-head
语义图，graph digest 包含固定 decode/modality/MTP/thinking profile 及全部边/切点。
它并不把 layer ID 当作 canonical ONNX index。已有 NativeQwenLayerSplit 仅消费
注入 graph，本批补 NativeQwenLayerSplit::inspectGraph(model, revision, maxNodes) 和
enumerateFromMetadata(model, revision, maxNodes, budget)，后者直接复用既有 enumerate。
maxNodes 是显式图分配上限，分配前以减法检查 layer count，避免 +2 溢出。
完整 model.graphDigest 必须与 metadata 图摘要匹配，不合成模型 contentDigest。

Write：NativeQwenPlanner.hpp/.cpp、di-native-planning.t.cpp、对应 Python oracle
及 case-manifest。只新增方法，不改变已有 class layout。独立 oracle 使用维护
Python builder，比较完整图与候选；覆盖多层范围、跨十进制层号、错误 revision/
摘要和预算拒绝。两个模型共用候选/placement 契约不复制。实际 source/recipe owner
及网络目录不在本批，不用模型名推测其认证或 ONNX 位置。

## Validation Plan

QM-1 和 QM-2 各加载官方 review-agent 只读审查，记录实际 findings/修复；整批
入口→graph identity→splitter→candidate 审查通过后统一系统工具链 -j4 build。
布局兼容时复用 spec182-yolo-semantic-r1/build；发生 ABI 改变则 fresh tree。
相关 selectors：Spec182NativePlanning、Spec182V3Placement，planned 测试先注册。
无 integration/MiniNDN/SIF/Tiger。实际批次结果见下方 Final Validation。

## Current Member Progress

以下保留编码与审查过程；最终状态为 QM-1/QM-2/QM-3 DONE（仅本批），不改变
T003-A 整卡 PARTIAL 或 R1 阶段状态。

QM-1 及 QM-2 的生产入口代码已写入 NativeQwenPlanner：元数据生成节点、真实语义
tensor 契约、切点及完整 graph digest，验证 expectedModel 后复用 enumerate。
未更改 class 数据成员布局。独立 oracle、负例和官方静态门尚待完成，未运行编译，
初始代码未提交；此初始状态不代表 STATIC_PASS 或批次通过。

QM-1/QM-2 已补独立 Python builder→splitter 对照（2/12 层、跨 layer-09/10），
完整候选 identity、全部图边/tensor 契约以及 revision/graph/节点上限/预算负例。
实际加载 /home/tianxing/.codex/skills/review-agent/SKILL.md，SHA256
07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228。
只读完整源码差异、维护 Python builder/策略及测试后，产品 No findings；coverage
gap 为内部 tensor 契约断言不足，已补逐边及 input/output JSON 对照并复审。
组合入口→元数据图→身份验证→候选路径及预算/分配前边界已审查。
STATIC_PASS / TESTS_DEFERRED / R1-B1，整批 READY_FOR_BATCH_TESTS；不证明默认
requester 或真实 source/recipe 接线。下一步复用兼容 build tree 统一构建/测试。

## Final Validation

单次兼容增量 -j4 build PASS（19.721s），未改变 class layout。system compiler/
linker 与 Boost cache 已核对，vmstat 第二次采样 si/so=0。官方只读审查使用上方
固定技能；修复测试 coverage gap 后完成组合审查，再统一执行测试。
Spec182NativePlanning、Spec182V3Placement：32/32 cases、1165/1165 assertions PASS。
两组维护 Python 元数据图及完整候选对照、revision/graph/maxNodes/budget 负例通过。
未验证真实 requester/source/recipe 接线，不因本批完成关闭 T003-A。

- [oracle](../../../.codex-tmp/spec182-r1-b1/oracle.log)：`python3 tests/fixtures/spec182/author-qwen-metadata-oracle.py`。
- [build](../../../.codex-tmp/spec182-r1-b1/build.log)：`waf -o .codex-tmp/spec182-yolo-semantic-r1/build build --targets=unit-tests -j4 -v`，沿用原显式依赖前缀及 tokenizer target。
- [focused](../../../.codex-tmp/spec182-r1-b1/focused.log)：`timeout 60s .codex-tmp/spec182-yolo-semantic-r1/build/unit-tests --run_test=Spec182NativePlanning,Spec182V3Placement --report_level=detailed --log_level=message`。
- 下一批从共享候选/模型身份与实际 role/source 接口的剩余缺口领取；不重复 R1-B1 的已通过实现。
