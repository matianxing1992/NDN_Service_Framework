# T003 Node Ownership and State Contracts

## Source and Review

基线 9ba91d16。维护 splitter.py 的 RoleExecutionPlan 要求节点角色覆盖全部声明角色、
role dependency 无环；SplitCandidate.validate_against 要求覆盖全部 graph nodes。
QwenThreeStageSplitter 还声明三组 attention KV/recurrent/convolution state I/O。
原生候选缺这些字段，因此之前局部校验无法证明规划完整性。

NativeSplitCandidate.nodeRoles 保存规划节点归属（对应 Python execution_plan.node_roles），
不混入 legacy runtime wire。Qwen 按 layer ranges 分配 embedding/layers/final node；
YOLO 保存已验证的 semantic partition。共享 validator 检查全部节点及全部角色覆盖，
依赖角色 DAG 检查复用现有 dependency 字段，不新增第二套边。
状态 input/output 使用同一 NativeTensorContract；增加共享 validate 方法，仅检查
非空 name/dtype，保留整数/符号形状及未知大小。状态映射须成对、全角色覆盖、
非空且同侧名称唯一。Qwen 保存维护算法的三组状态，YOLO 空映射保持无状态语义。

测试核对精确节点 owner、Qwen 状态名称/类型/形状/未知大小与 YOLO 空状态；
负例覆盖遗漏/外国节点、无节点角色、缺失/重复/无效状态与 role cycle。
preparation 的通用 fixture 改为三个独立规划节点，支持实际测试的一至三个角色；
不能继续声称一个节点属于多个角色。planning node 与 canonical ONNX node 的映射
以及 state→实际装配绑定仍归后续 adapter owner，不能从本轮字段校验推断完成。

## Validation

r1 configure PASS（6.655s），新 ABI build PASS（410.204s），68/69 cases PASS。
空角色负例的测试 helper 为新 nodeRoles 分配取模时除零，未到产品边界；r1 原始
日志保留。修复 helper 让空候选继续抵达原有拒绝路径，独立 r2 重验。
布局改变，使用全新 `.codex-tmp/spec182-t003-node-state-r1/build`；
冻结 system compiler/binutils、Boost/NAC/SVS/ONNX/ORT/tokenizer 依赖，单一 -j4。
必要构建后执行相关 planning/preparation/placement/sealer/publisher 单元；
不运行 integration/MiniNDN/SIF/Tiger。

r2 build PASS（21.493s），69/69 cases、972/972 assertions PASS；文档 validator
及 git diff --check PASS。ldd 无缺失库，NAC/SVS/ORT 为 preflight 指定位置。
vmstat 短样本有少量 si，so 均为 0；不作全程峰值资格。Python extension 未重建，
旧 ABI 产物不计新布局证据。T003-A/B/C 继续 PARTIAL，不新增 DONE。
Context Mode active health 因 tasks.md source hash 过期失败，使用当前仓库文件及
CodeGraph/源码核对，没有用旧索引判断进度。

Commands:
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin python3 ./waf -o .codex-tmp/spec182-t003-node-state-r1/build build --targets=unit-tests -j4 -v`
- `timeout 60s .codex-tmp/spec182-t003-node-state-r1/build/unit-tests --run_test=Spec182CanonicalPublisher,Spec182V3Placement,Spec182Preparation,Spec182OfferAdmission,Spec182NativePlanning,Spec182PlanSealer,Spec182GrantClient,Spec182NativeInferenceClient --report_level=detailed --log_level=message`

Evidence:
- [r1 preflight](../../../.codex-tmp/spec182-t003-node-state-r1/preflight.log)、[configure](../../../.codex-tmp/spec182-t003-node-state-r1/configure.log)、[build](../../../.codex-tmp/spec182-t003-node-state-r1/build.log)、[focused failure](../../../.codex-tmp/spec182-t003-node-state-r1/focused.log)、[ldd](../../../.codex-tmp/spec182-t003-node-state-r1/ldd.log)。
- [r2 build](../../../.codex-tmp/spec182-t003-node-state-r2/build.log)、[focused](../../../.codex-tmp/spec182-t003-node-state-r2/focused.log)、[design](../../../.codex-tmp/spec182-t003-node-state-r2/design-validation.json)。

## Remaining Identity Boundary

源码对照确认完整候选摘要还包含 ModelDescriptor.adapter 的 ABI、schema digests、
格式/task/backend/precision 能力与 source_revision，以及 SplitterDescriptor 的
deterministic 字段；现有 NativeModelDescriptor/NativeStrategyIdentity 仅保留简化字段。
后续必须补真实 descriptor 数据与规范序列化，不能给当前简化 DTO 重算哈希冒充
Python candidate_digest。YOLO fragment 的节点序列来自 graph.topological_order，
不能用 std::map 的排序代替原顺序。estimated_costs/postprocessing 和 model-specific
semantic interface 也继续归完整候选闭合范围，不影响本轮新增字段的独立检查。
