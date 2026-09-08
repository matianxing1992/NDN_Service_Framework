# Spec182 Design Audit

**Revision**: 7 | **Mode**: source alignment / pre-implementation
**Verdict**: DRAFT / BLOCK for implementation
**Source**: `81e251a4ef1d8e6a394dc5f0c38bc44e44bfc973` / Experimental
**Evidence**: [current source and dependency baseline](contracts/integrated-baseline.md)

## Current Findings

### YOLO Fragment Registration Binding 2026-09-07

原生 fragment 字符串哈希遗漏注册摘要和有序节点，已改维护的规范 JSON 身份。
同时对齐 CPU/CUDA backend、1.1 余量和无 Merge 角色时的原子候选行为。实际 Python
splitter oracle 及相关 82 cases/1084 assertions PASS；case-manifest 覆盖全部当前
YOLO 用例。完整 candidate identity 与 catalog/interface 仍待闭合，见
[fragment evidence](evidence/t003-yolo-fragment-20260907.md)。

### Complete Model Descriptor Identity 2026-09-07

新增完整 AdapterDescriptor 的规范 JSON/摘要，ModelDescriptor 保留 sourceRevision
并严格绑定 adapter 兼容字段；prepare/inspect 不再仅比较简化模型字段。六组维护
Python 规范字节/摘要与替换负例通过，相关 81 cases/1067 assertions PASS。
graph adapter、splitter/candidate 全部字段及规范摘要仍待闭合，见
[descriptor evidence](evidence/t003-model-descriptor-20260907.md)。

### Candidate Node Ownership and State 2026-09-07

共享候选补 nodeRoles 与 state I/O，Qwen/YOLO 均验证全节点/全角色覆盖和依赖
无环；Qwen 三组状态精确字段对照与非法输入检查通过。新 ABI build PASS；
修复空角色测试 helper 后 69 cases/972 assertions PASS。完整 adapter descriptor、
candidate 规范摘要和实际装配映射仍缺，保持 PARTIAL，见
[node/state evidence](evidence/t003-node-state-contracts-20260907.md)。

### Shared Candidate Validation 2026-09-07

补齐 graph/model、摘要格式、ingress/egress、合法 cut/dependency tensor 集合及
rank 工件完整性校验。新校验揭示旧 SDK/native fixture 的重复 rank artifact；
修复为独立工件后 68 cases/836 assertions PASS。完整候选字段/规范摘要仍未闭合，
见 [candidate validation](evidence/t003-candidate-validation-20260907.md)。

### Graph Edges and Candidate Identity 2026-09-07

源码对照发现原生 YOLO 按节点相邻关系合成依赖、按节点数估算预算，Qwen 又哈希
既有 artifact 作为 fragment。已补真实 tensor edges、分支依赖及已知大小预算，
Qwen 保留 artifact 摘要和 onnxruntime family；新 ABI 构建及 67 cases/818 assertions
PASS。T003-A/B 撤回 DONE，完整候选 node/state/interface/identity 仍待闭合，
见 [tensor edge audit](evidence/t003-yolo-tensor-edges-20260907.md)。

### Native Core Artifact Publication 2026-09-07

新增 native publisher，作为既有 ArtifactPort 复用 Core prepared-request、加密大对象
发布和 I/O 调度。冻结 ONNX inline/external 源通过实际 native 身份核验；排队取消/
超时释放与停止后续 publication、Core 失败原因保留、真实 Core LocalMock-key 发布
均已定向检查。65 cases/779 assertions PASS；此为 native API/单元证据，不是 NAC
bootstrap、网络权限或完整 requester 资格。模型 source inspection、发布配置和
requester 主链仍待接线，见 [Core publisher](evidence/t008-core-artifact-publisher-20260907.md)。

### Publication Recertification 2026-09-07

原生已验证发布后业务 root 的原始字节及 source/initializer/model/profile 绑定，
复用既有 recipe encoder 更新 manifest/recipe，并在封存前重验原 placement 和最终
recipe 的 admitted offer 可行性。稳定工件名与 root fetch 名分开保存。
相关 58 cases/710 assertions PASS，包括真实 SDK 摘要对照和旧 exact-reuse 拒绝。
实际本地 source inspection、Core publisher 与 requester 主链仍未接通，T008-A/T004-A
保持 PARTIAL，见 [publication evidence](evidence/t008-publication-recertification-20260907.md)。

### Planning and Canonical Graph Spaces 2026-09-07

维护中的 YOLO binding 在 recipe 中使用 canonical ONNX graph，顶层保留 planning graph；
原生强制相等的校验不符合源码，现显式拆分身份并新增不同 digest 的 SDK core oracle。
新 ABI 构建与最终 58 cases/594 assertions PASS，错误摘要替换与缺失均拒绝。
同时发现 publication 后 manifest 更新与 artifact/fetch 名分离尚未由原生 owner 实现，
当前 source/manifest 前提不能直接承接维护路径。T008-A/T004-A 继续 PARTIAL，见
[graph identity evidence](evidence/t008-graph-identity-spaces-20260907.md)。

### V3 Artifact Publication 2026-09-07

ensureArtifacts/ArtifactPort 已移除旧简化 proposal 输入，直接使用候选与选定 V3 roles。
返回工件必须逐项等于角色要求，修复“合法 SHA 但对应另一工件”仍可通过的缺口。
新 ABI 构建与最终 58-case 定向验证 PASS；旧 sealer fixture 的内存预算失败与修正已留存。
真实 catalog/Repo owner、网络发布与 requester 主链未完成，T008-A 保持 PARTIAL，见
[publication evidence](evidence/t008-v3-artifact-publication-20260907.md)。

### V3 Strategy Interface 2026-09-07

placement 基类已改为完整 V3 虚接口，默认实现和只实现 V3 的自定义原生策略都通过
同一基类调用；新 ABI 构建及 58-case 定向验证 PASS。旧简化 propose 仅留在具体默认类
供迁移 fixture，不能通过策略基类回退。publication port 仍接收旧 proposal，真实
requester 尚未调用 placement；T003-C 保持 PARTIAL，见
[strategy evidence](evidence/t003-v3-strategy-interface-20260907.md)。

### V3 Sealer Connection 2026-09-07

完整 proposal/admitted offers 已直接连接 sealCore/grantView，复用准备角色和设备可行性
校验，合法 exact reuse 不再被旧 preparationAccepted 条件拒绝。CPU/GPU/multi-rank
SDK core digest 对照及相关 55-case 定向检查通过。真实 requester/catalog 与 device/dataflow
生成仍缺，T004-A 保持 PARTIAL；见 [V3 bridge evidence](evidence/t004-v3-sealer-bridge-20260907.md)。

### Inspection Source Checkpoint 2026-09-07

**A8-03 / HIGH / PARTIAL**：inspectModel 原先拼造 catalog 名称，并丢失请求完整模型描述。
已改完整 inspection 结果、expectedModel/manifest 绑定与 prepareRoles 验证；真实 catalog/Repo
owner 和 requester 主链仍缺，不能把本地 fixture 视为认证 I/O。见
[inspection evidence](evidence/t008-inspection-roles-20260907.md)。

### V3 Placement Checkpoint 2026-09-07

完整 role/rank、admitted offer 的 proposeRoles 已实现 capability/device 优先、exact
residency 与确定性成本排序；真实 SDK assignment/device 对照通过。旧简化 candidate
仍缺完整 assembly metadata，主链尚未迁移，不能用新入口的单测宣称 T003-C 或 T010 完成。
见 [V3 placement evidence](evidence/t003-v3-placement-20260907.md)。

### Offer Observation Audit 2026-09-07

**A8-02 / HIGH / PARTIAL**：policy 合成 capabilities 与缺失 offer signature 的旧入口已删除，
替换为 Core AckSelectionCandidate payload、candidate-bound policy/public-key registry 和
Ed25519 verify，真实 SDK signed fixture 对照通过。尚未接 requester 的真实订阅和完整 planner
view，T008-B 保持 PARTIAL。见 [Core offer evidence](evidence/t008-core-offer-admission-20260907.md)。
CPU 不制造资源观测，hasModel 不冒充 exact residency；网络资格仍待 T016。

### Implementation Audit 2026-09-07

以下 revision7 设计审计保留为历史；当前执行状态以 tasks 为准，不再以旧的 T001
未完成/全局 implementation BLOCK 推断现状。基线 8e86bca5，新增 **A8-01 / CRITICAL /
OPEN**：[T004 wire/identity audit](evidence/t004-wire-reopened-20260907.md)。真实
NativePlanSealer::encode 输出不满足生产 parser，且伪工件摘要、非规范 core digest、
不完整 grantView 和硬编码 projection 不能支撑完整请求。T004-A/父 T004 重开，
T010 继续完整接线前必须修复；不以本轮 13 个 Core I/O/handle 单测关闭这些义务。

### Prior Revision 7 Findings

2026-09-06追加[native capability reuse review](evidence/native-reuse-review-20260906.md)，审查源码`5239b229`。2026-09-07以[native generation contract](contracts/native-generation-design.md)补齐复用比较和采样修复语义，并移除plan旧授权句：A7-10/A7-11 CLOSED。A7-08 HIGH/OPEN仍需stream状态算法；A7-09 HIGH/OPEN已有算法处置但native修复/测试未运行，新增Top-K校验、Greedy校验与float32→double差异纳入同一T011。产品实现继续BLOCK，T001/O-004未完成；reference诊断不证明native修复。

revision 7审计已按最新源码修订文档；用户随后授权在Experimental完成182，当前T001设计收口中。已停止管理已接收交付的实验机器。
源码身份与历史状态的文档漂移已修订；未实现功能继续planned，未闭合设计继续BLOCK。
T001有界依赖探针见[native dependency design](contracts/native-dependency-design.md)，不计产品实现或T015/T016资格；后续unit/integration/MiniNDN按任务门执行，SIF/Tiger由外部负责。

| Finding / severity | Source evidence / controlling requirement | Correction / owner / closing proof |
| --- | --- | --- |
| A7-01 / HIGH / RESOLVED | spec Relationship/Assumptions与symbol readiness仍写未提交合并、integration失败；实际HEAD包含整合历史，生产路径与merge及交付源无diff；FR-015 | 更新当前baseline、分离历史checkpoint，O-001仅按源码范围CLOSED；T001仍未完成 |
| A7-02 / HIGH / RESOLVED | baseline原标VALIDATED却未包含SVS `9f2d8a47` / NDNSD `375a35c5`；当前lock和停止记录明确ABI消费者未全验证；FR-012/014，INV-008 | 固定四库pin与ABI失效范围；旧759/154结果保持历史，当前组合UNQUALIFIED；CD-009设计、T015审查、T016运行证明 |
| A7-03 / HIGH / OPEN | ServiceProvider.hpp公开addService/addCollaborationHandler，无逐服务注销；CD-014/M47需要共享宿主close语义；FR-008/010/017 | runtime-boundaries补确切接线及缺口；T001/O-004冻结registration/ACK/Selection fence、lease共享和重复注册，T009实施，PO-014检出误停共享服务与晚到工作 |
| A7-04 / MEDIUM / RESOLVED | requester仍在app_sdk/placement.py::_request_v3；NativeCanonicalOnnxAssembler.cpp::runPythonHelper与NativeStandaloneTokenizer.cpp::makeNativeStandaloneTokenizerDecoder仍启动Python；独立DI库/新facade不存在；FR-001/006/007/012 | 保持CD-001/005/006/009为planned，复用现有Provider/安全/epoch机制；T002/006/007/010及PO-001/005/006负责目标实现与证明 |
| A7-05 / MEDIUM / RESOLVED | 旧baseline链接、revision2现行声明、交付任务混入Current Checkpoint；当前root skills与Tiger交付工具已存在；FR-014/017 | 当前authority统一指integrated-baseline，旧checkpoint标历史，plan/T017复用共享技能和工具；旧Python交付模板不冒称182 no-Python成果 |
| A7-06 / HIGH / DESIGN_RESOLVED | 原Python helper有进程超时回收；ONNX checker/shape inference无取消接口，直接进程内替换会削弱deadline/cleanup；FR-008 | CD-005细化具名native worker、owned FD/进程组、steady deadline、部分输出拒绝与Provider激活fence；T006实施、T016真实证明，设计见native-onnx-assembly-design |
| A7-07 / HIGH / OPEN / CONFIRMED | 原版本独立探针：BFLOAT16 raw摘要与已知bits不符、typed正确；STRING相同模型跨进程摘要不同；FR-006/016 | 24个普通numeric稳定向量已冻结，见[identity evidence](evidence/identity-reference-20260906.json)；O-002/O-004补稳定规范和兼容处置，不复制错误/进程指针字节，未修改产品Python或原oracle |

| Open item | Controlling gap | Owner |
| --- | --- | --- |
| O-001 / CLOSED | 当前源身份、合并差异及181承接已核对；不等于当前依赖运行PASS | T001部分完成 |
| O-002 | ONNX原生装配字节契约与依赖锁 | T001 |
| O-003 / CLOSED | 精确crate/toolchain锁、C ABI/释放/串行寿命/生产路径及84对照+14负例PASS；产品迁移/隔离尚未完成 | T001设计完成；T007/T016实施证明 |
| O-004 | 12类137字段与当前源一致；完整旧能力/调用方/selectors、嵌套schema及A7-03注册寿命仍缺 | T001 |
| O-005 / CLOSED | 最小root/namespace+strace工具正反例PASS；权限/服务白名单、函数/字段、I01--I08及观测失败规则冻结；不计完整运行资格 | T001设计完成；T014/T016实施证明 |

O-002--005的有界关闭条件见[code-design](contracts/code-design.md#open-questions)，依赖探针结果与完整算法/兼容设计关闭分别记账。
T002--T014的各项实现、测试工具编写、静态审查、局部单测完成后，T015补审整体接线，
T016收齐真实运行证据，T017交付。验收标准满足即结束；变化或具体缺陷才触发受影响回归。

## Source Checks and Limits

- `git diff --name-only c770f18b HEAD -- ndn-service-framework NDNSF-DistributedInference NDNSF-DistributedRepo NDNSF-UAV-APP examples pythonWrapper wscript`与同范围`447f7584..HEAD`均无差异；读取四仓库HEAD与交付lock，核对祖先关系。tracked源码无预存改动，本地未跟踪日志/构建目录不纳入审计或提交。
- CodeGraph先查生产符号，再精确读取ServiceUser.hpp、ServiceProvider.hpp、DI_NativeProviderExecutable.cpp、两helper、examples/wscript及Python requester。宽泛结果混入`.codex-tmp/compare-*`，拒绝其作为当前源码证据，不重建索引或扫描整个临时树。
- source-field-coverage.json的12类137字段按当前Python AST核对名称/类型/默认值；这只覆盖已有表，不证明全部嵌套schema或公开调用方穷尽。现有137字段表保留，不重复建立第二份DTO权威。
- 原生Provider接线必须保留execution lease服务、V3 offer、provisioning/readiness、permission、protected preparation、epoch与结果路径；纯C++host不是只包装最终runtime.handler。Core控制publication不是通用remote-abort。
- 任务仍为17个行为单元，未因审计机械拆分。FR-001--019、SC-001--011、CD-001--014、PO-001--016及既定负例保留；实施相关unit与T016完整集成/MiniNDN的分工不变。
- Context Mode project/active健康检查通过；宽泛`status` timeline查询被guard拒绝，改用精确file-backed active tasks的relevance检索并对照源码文档。持久文件为authority，不用旧session状态裁决。

文档检查使用`check-prerequisites.sh --json --require-tasks --include-tasks`、`audit_speckit_structure.py ... --strict`、`checklists/validate_design.py`及`git diff --check`；实际结果记tasks的当前checkpoint。它们不是产品测试，也不能关闭O-002--005。

## Bounded Executor Review

用户要求使Spec182可由Spark执行；新增[执行卡](contracts/spark-execution.md)与共享技能模式，
保留17个父任务及全部FR/SC/CD/PO。未决设计继续由T001收口，Spark只领取满足依赖与设计冻结条件的实现卡。
本轮发现最新Host契约包含Core scoped registration/ExecutionLeaseService，已展开T009-A/B/C及精确源路径，
避免执行者只改DI facade而漏掉真正的代次/共享资源owner。
T006真实worker反例统一由T016执行；T006仍须交付case及纯unit，父契约和proof同步。
文档结构/路径检查不证明Spark运行效果，实际检查与边界见[spark preparation evidence](evidence/spark-execution-preparation.md)。

## History

既有审计发现的历史理由与证据保留在Git及
[revision 2](evidence/audit-revision2.md)、
[revision 3](evidence/skill-and-design-revision3.md)、
[revision 4](evidence/static-review-gate-revision4.md)、
[revision 5](evidence/adversarial-review-revision5.md)。
旧revision中固定五项风险、独立S0/S1报告与逐任务integration要求由revision 6替代。
历史源码快照与运行结果不回填为当前实现或资格证明。

## Next Action

T001继续关闭O-002--005：固定ONNX/protobuf字节契约、tokenizer ABI、完整兼容/注册/状态设计与独立测试selector、no-Python隔离方案。无需重开合并或续跑181资格；各设计项满足其完整关闭条件后才关闭。
任务完成 **0/17**；T001依赖探针单独记录，T015产品收敛审查及产品构建/unit/integration/MiniNDN **NOT_RUN**。

## Progress Registry Amendment

2026-09-07：执行状态统一到 [Execution Progress](tasks.md#execution-progress)，
全部执行单元使用 [generic cards](contracts/execution-units.md)，不依赖 Spark。
旧 Spark 设计/试用记录保留历史含义；本次不改变 FR/SC、父任务验收或 Gate Order。
逐行状态为保守迁移，相关实现未经过本单元验收；检查见 [registry evidence](evidence/task-progress-registry-20260907.md)。
# A9 Placement and Artifact Binding Follow-up

2026-09-07 / OPEN：T004 已移除角色名伪工件摘要并补齐 grant 输入，33 个相关单测通过。
但 T003-C 将全部角色分配给同一 Provider，且按无关 residency 数量排序；已重开该卡与
依赖完成状态。A8-01 的 canonical wire 部分保持 OPEN。完整证据见
[binding repair and placement audit](evidence/t004-artifact-grant-bindings-20260907.md)。
## A9 Repair Verification

2026-09-07：逐角色独立 Provider、真实目标工件提示排序与预算拒绝修复定向 PASS（37 cases）。
旧 sealer fixture 依赖无关缓存加分，现已纠正；完整 proof/device/rank DTO 和 Python oracle
尚缺，A9 与 T003-C 保持 OPEN/PARTIAL。见 [placement evidence](evidence/t003-role-placement-20260907.md)。
## A8 Typed Wire Repair Verification

2026-09-07：typed shape 贯通 Selection/worker/Provider 消费者，完整 projection encoder
已通过生产 parser 往返；77 cases、2521 assertions PASS。旧 sealer 的七字段 encode 与
不完整 project 仍在原位，A8-01 保持 OPEN；不得将新 encoder 可用解释成 requester 链已闭合。
见 [typed shape and wire evidence](evidence/t004-typed-shape-20260907.md)。
## A8 Sealer Integration Verification

2026-09-07：旧七字段 encoder 和硬编码 project 已移除。完整输入通过生产 parser；core/final
摘要与真实 Python SDK oracle 一致；新 ABI 构建与 62 cases/2358 assertions PASS。
上游真实 metadata、完整 generation/device/rank oracle 及 requester 主链尚未完成，
T004 保持 PARTIAL。见 [integrated sealer evidence](evidence/t004-sealer-integrated-20260907.md)。
