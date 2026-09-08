# Spec182 Design Audit

**Revision**: 7 | **Mode**: source alignment / pre-implementation
**Verdict**: DRAFT / BLOCK for implementation
**Source**: `81e251a4ef1d8e6a394dc5f0c38bc44e44bfc973` / Experimental
**Evidence**: [current source and dependency baseline](contracts/integrated-baseline.md)

## Current Findings

### Offer Observation Audit 2026-09-07

**A8-02 / HIGH / OPEN**：NativeOfferAdmission 从 policy 合成 capabilities 而非读取
ACK offer，且缺少 policy-bound offer signature 验证。新增数据解码及 SDK oracle 已通过
定向检查，但没有形成可信 planning view；T008-B 保持 PARTIAL。
见 [observed offer evidence](evidence/t008-observed-offer-20260907.md)。真实 Core ACK provenance
和既有 offer signature 都必须接入，CPU 不制造资源观测、hasModel 不冒充 exact residency。

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
