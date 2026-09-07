# Tasks: Native NDNSF-DI with Optional Python Bindings

**Revision**: 7 | **Status**: DRAFT / T001 IN_PROGRESS
**Input**: [spec](spec.md), [code design](contracts/code-design.md),
[proof](contracts/proof-design.md), [work units](contracts/work-units.md)

## Current Checkpoint

2026-09-06 T001 ONNX contract / IN_PROGRESS：补齐[native ONNX assembly design](contracts/native-onnx-assembly-design.md)的owned类型、9个函数、8步算法、recipe/manifest字节与native worker生命周期；修复直接进程内替换会丢失硬超时回收的设计缺口。原始Python实现仍未修改；独立reference提取24个普通numeric raw/typed向量PASS。额外诊断确认BFLOAT16 raw摘要内容错误、STRING跨进程摘要不稳定，见[identity evidence](evidence/identity-reference-20260906.json)及failure index。**O-002/O-004仍OPEN，T001未完成，产品实现0/17**。下一步冻结这两种表示的稳定身份/兼容处置，并继续完整能力和Provider注册清单；不复制错误oracle、不以I/O dtype代替initializer能力范围。

本单元检查 **PASS**：strict structure、design validator（160本地链接、17任务/0完成）、diff whitespace；24个model hash、12对identity、6条诊断及未修改reference源hash核对一致。generator锁定原onnx/numpy/source版本，补锁后typed诊断仍通过。Context Mode健康检查PASS，但relevance检索返回了较早的Dependency Design子节，checkpoint以实际tasks顶部为准；CodeGraph的临时副本结果仍剔除，算法按精确生产路径核对。未构建/测试产品，不把发现旧缺陷或补全设计计作T006完成。

### Isolation Design Checkpoint

2026-09-06 T001 isolation design：**O-005 CLOSED**。已冻结[native isolation design](contracts/native-isolation-design.md)的最小root/namespace、权限与服务白名单、逐进程exec/映射/endpoint观测、harness函数/字段和I01--I08反例。bwrap0.4.0/strace5.5工具正例exit0且CapEff=0/NoNewPrivs=1，缺解释器反例在exec边界ENOENT/exit1，均符合预期；raw `.codex-tmp/spec182-t001-isolation-r1/`。没有运行NDNSF、MiniNDN、SIF/Tiger，也未实现T014 detector。当前O-001/O-003/O-005按各自设计范围CLOSED，**O-002/O-004仍OPEN，T001仍未完成**；下一步补完整ONNX算法和迁移/注册/状态清单。

本隔离设计单元检查 **PASS**：strict structure、design validator（149本地链接、17任务/0完成）、`git diff --check`。已静态核对工具选项、namespace/文件根与外部harness分界；最小工具正反例不外推NFD/Repo/Controller或全部后代观测的运行证明。此前依赖设计单元已本地提交`5187e733`，tracked tree随该单元清理，原始构建/日志未入Git；未push。

### Dependency Design Checkpoint

2026-09-06 T001 / IN_PROGRESS：用户已授权在Experimental完成Spec182，按原目标实施与本地验收；上一轮只审计的范围已结束。先关闭O-002--005，不提前迁移业务或执行最终集成/MiniNDN。已补齐tokenizer特殊token参数，建立[依赖设计与探针契约](contracts/native-dependency-design.md)。T001保持未勾选；SIF/Tiger继续外部负责。

T001运行边界：ONNX1.17.0原生依赖-j2构建及四向量探针 **PASS / exit0**，四个模型字节与原Spec181 oracle完全一致。tokenizers0.20.3/Rust1.90.0静态ABI与C++consumer构建 **PASS**，84个完整ids/text对照和14个拒绝检查 **PASS**；两探针ldd均无Python。ABI、生产桥接路径/字段/所有权、许可及依赖锁已冻结，**O-003 CLOSED**。O-002的完整算法/叶子契约、O-004完整兼容与状态设计、O-005隔离方案仍OPEN；T001和T007未完成。Rust工具链R1/R2下载TLS失败在R3更换HTTPS实现后恢复。详见同一依赖设计记录及failure index；不计产品任务完成。

本依赖设计单元检查 **PASS**：prerequisites、strict structure、design validator（143本地链接、17任务/0完成）、`git diff --check`；lock与两个oracle及三个tokenizer JSON的hash一致，Cargo.lock中72个registry依赖均有精确checksum。probe静态审查已在运行前完成，输入/expected来自旧版本，未改变产品源或历史oracle。恢复时Context Mode提供的`probe.cpp write` timeline查询未通过高熵identifier/source guard，改用持久tasks/contracts与实际日志核对，不从被拒绝的session检索推断状态。下一步补O-002/O-004/O-005；不重跑已通过且输入未变的探针。

### Prior Audit Checkpoint

2026-09-06 revision 7 / SOURCE_ALIGNMENT_COMPLETE：用户确认另一台机器已接收，本轮只审计/修订Spec182。以Experimental `81e251a4ef1d8e6a394dc5f0c38bc44e44bfc973`核对代码，修正未提交合并/旧integration失败的过时表述；固定NAC/SVS/NDNSD身份与旧运行证据失效范围。O-001按源码身份和181承接范围CLOSED；O-002--005仍OPEN，T001未勾选，实现仍 **0/17**。当前SVS/NDNSD组合 **UNQUALIFIED**，接收不等于实验通过。审计发现与关闭责任见[audit](audit.md)，具体版本见[integrated baseline](contracts/integrated-baseline.md)。

本轮未构建或执行产品unit/integration/MiniNDN/SIF/Tiger，未管理接收机器，未恢复已停止的ABI构建。下一步只继续T001的ONNX/tokenizer依赖、兼容/字段/注册寿命及隔离设计关闭；后续182开发验证仍按下方任务分工，上一轮delivery-only移交不取消T016义务。

本轮实际文档检查 **PASS**：Spec Kit prerequisites、strict structure、`checklists/validate_design.py`（136链接、17任务/0完成、依赖无环、19 FR/11 SC/14 CD/16 PO）、`git diff --check`；12类137字段AST名称/类型/默认值一致，19 FR/11 SC/16 PO/14负例/48方法/137字段条目相对审计输入未删改。以上不计产品验收；O-002--005保持OPEN。改动仅12份Spec182文档，历史evidence及产品源码不变。

## Historical Checkpoints

以下为各时点事实；其旧“下一步”、失败、未push及验证结果均不覆盖上方当前checkpoint。旧结果只适用于当时身份。

2026-09-06 Source Handoff COMPLETE / SOURCE_READY：用户明确本轮只交付，编译与测试在另一台机器执行。本机额外构建已停止并确认无遗留编译进程；后续NDNSD/全部ABI消费者构建、unit/integration、两wrapper验证、MiniNDN、SIF/Tiger全部TRANSFERRED，不再作为本机交付条件。D001范围修订为固定版本及ABI要求移交，D001--D004已完成；四库Experimental、可迁移输入/模板与root skills已发布。已有检查与中断事实保留，不冒称完整验证PASS。接收步骤及证据见 [source handoff](../../Experiments/TigerCluster/docs/source-handoff.md)。Spec182仍0/17，不计T001--T017完成。

2026-09-06 Existing Presentation Checkpoint：补存此前未跟踪的`docs/NDNSF-UAV/slides/UPDATES.tex`及对应4页PDF；标题和全部命名frame的PDF文本核对PASS。旧版`UPDATES_UAV.pdf`、LaTeX缓存、原始实验输出及本地助手状态继续保留为本地产物，不计源码或182进度。活动指针/managed plan均指182，最终索引刷新后project与active健康检查PASS。

2026-09-06 Branch Closure：`Experimental` 已快进至整合提交 `e91ecc91`，本地只保留 `main`、`Experimental`；原生验证工作树改为detached并保留二进制/raw，临时分支已删除。主工作区原426项源码/文档状态保存在具名恢复stash `4bb5e0a5`，实际成果已归并，不能直接pop旧测试覆盖修复。未push。另将用户既有UAV slides源文件/PDF单独checkpoint：30个frame与30页PDF、标题/日期和两个新增Geo-Capture页的文本对应检查PASS；这是既有演示文档保存，不计182产品验收。

2026-09-06 Experimental Consolidation：原生合并修复已成为真实merge commit `c770f18bb7bf42c3b8a8274b5c029b4883141f60`，包含远端UAV历史；同步本机 `2e7865c7` 的revision6和Tiger目录布局。最终工作分支统一为Experimental，main保持稳定基线。原生生产代码与已验证基线逐字节一致；unit **759/759**、integration **154/154**、current Python **2171 passed / 22 skipped**、三个真实MiniNDN授权/撤销场景 **PASS**，见 [integration closure](evidence/integration-20260906.md)。此前152/154是已修复的历史失败，不再控制合并状态。

新增 [integrated baseline](contracts/integrated-baseline.md) 固定Core/UAV复用接口、生命周期修复与181承接。O-001已提供实际commit/证据/承接表，T001仍须核对最终Experimental差异并关闭O-002--005；182实现仍 **0/17**，其最终产品验收 **NOT_RUN**。不再独立续跑181最终资格，下一步是T001设计关闭。

2026-09-06 revision 6：合并重复审查与报告；实现任务只做静态审查、相关unit及必要构建，集成与MiniNDN在全部实现后由T016统一运行。类/方法/字段设计和既定真实运行用例保留。

本轮只改技能与文档；实现 **0/17**，产品STATIC_REVIEW与unit/integration/MiniNDN均 **NOT_RUN**。
文档结构、107链接、依赖及技能引用检查PASS；既定PO-001--014/负例/运行用例与符号字段表的保留检查PASS。范围见 [workflow simplification](evidence/workflow-simplification.md)。
O-001--005和合并修复状态未被本轮关闭。下一步执行T001确认基线与设计就绪。

## Tiger Directory Checkpoint

2026-09-06 Consolidation Review Closure：当前Tiger源码一并归入Experimental。静态复审修复ACK/选择/请求关联、原生错根拒绝证据组合、有限应用进程组清理；新增负例先复现19项失败，修复后新工具目录 **58/58 PASS**，旧目录兼容及关联工具 **162 passed / 3 skipped**。B001/B002开发检查更新，B003实际SIF/双节点/复用验收仍未完成，Local R8 FAIL保留。用户已停止实验，本轮未运行SIF/Tiger；详见 [baseline checkpoint](../../Experiments/TigerCluster/docs/two-node-baseline.md#usage)。

2026-09-06 Two-node baseline IN_PROGRESS：新增[基础实验契约与进度](../../Experiments/TigerCluster/docs/two-node-baseline.md)，独立B001--B003负责共享runtime、双节点profile、真实NDN/NDNSF探针及实际运行复用；不改变Spec182原生迁移任务状态。B001/B002实现与静态审查完成，相关unit **24/24 PASS**；本地/远端SIF哈希一致。B003尚待精确SIF集成与实际双节点/复用运行，尚无实验PASS。

2026-09-06 Usage Guidance：本地工作约定已补充Tiger唯一入口、共享owner、旧路径同步和双机分工；可随Git交付的说明见[Tiger README](../../Experiments/TigerCluster/README.md#compatibility-and-review-boundary)。路径/链接及文档一致性检查PASS；本轮只改说明，不运行产品测试、不改变实现进度或验收状态。

2026-09-06 Follow-up R2：64个迁移文件的哈希/权限/旧新路径、29个共享文件与审查工作树对比、Tiger文档本地链接均PASS，无新增同步差异。两份已同步测试未变，沿用R1的58/58工具单测证据，本轮不重复执行。审查工作树完整integration日志为152/154 PASS、2 failed；整合未完成，目录迁移无需追加修改，等待该owner交付最终基线。详见 [follow-up evidence](evidence/tiger-directory-migration-20260906.md#follow-up-r2)。

2026-09-06 Review Sync R1：64个迁移文件及共享lib/bin无新差异；两份测试修正已从审查工作树同步，相关工具/collector单测 **58/58 PASS，exit0**，关闭上轮原因码断言失败。源码迁移与原生实现状态不变；证据见下方migration evidence。

2026-09-06：Tiger目录迁移完成，64文件内容/权限与路径静态检查PASS；工具单测50 PASS / 1 FAIL，独立原布局已复现相同原因码失败，留给原工具owner。纯路径checkpoint保留49个已跟踪文件原HEAD内容，已有修改及15个未跟踪文件在新路径继续保留，不混入迁移提交。详见 [migration evidence](evidence/tiger-directory-migration-20260906.md)。本工作不计T001--T017实现或产品验收。

## Validation Standard

唯一规则见 [validation workflow](contracts/pre-test-static-review.md)。
T002--T014的任务[x]仅代表本任务实现、静态审查、相关单测和必要构建完成；
其Proof行引用完整行为义务，跨组件/跨进程与MiniNDN证明登记给T016，不要求各任务提前运行。
T015在全部实现后补审整体接线；T016执行完整unit→integration→MiniNDN并关闭全部必需PO。
不得将真实集成重命名为unit/smoke提前执行。Static review PASS != Behavior PASS。
每任务只保留简短结果或一份evidence链接，最终核对diff与证据，不另建S0/S1报告。

## Phase 1: Design and Native Components

- [ ] T001 [US5] **Successor Baseline and Design Closure**。冻结合并基线与181承接表、所有 schema/公开调用方/能力清单及原生依赖，关闭 O-001--005；修订叶子签名与任务至可执行。Dependencies: Merged baseline closure and Spec181 handoff。
  Design: FR-015,FR-016,FR-017,FR-018; CD-001--014。Proof: PO-012。
  [T001 contract](contracts/work-units.md#t001-successor-baseline-and-design-closure)。

- [ ] T002 [US1] **Installable Native Library Contract**。existing Provider runtime 可独立安装/链接；planned requester 公开声明先冻结，完整request由T010实现、T016运行验收。Dependencies: T001。
  Design: FR-001,FR-012; CD-001,CD-009。Proof: PO-001。
  [T002 contract](contracts/work-units.md#t002-installable-native-library-contract)。

- [ ] T003 [US2] **Native Split and Placement Decisions**。两个原生模型 splitter 与默认 placement 对固定输入生成合法且确定的方案。Dependencies: T002。
  Design: FR-003,FR-009,FR-016; CD-002。Proof: PO-002。
  [T003 contract](contracts/work-units.md#t003-native-split-and-placement-decisions)。

- [ ] T004 [US1] **Canonical Native Plan Sealing**。合法 proposal 转成可被真实 Core/Provider 接受的规范计划；非法投影在首边界拒绝。Dependencies: T003。
  Design: FR-002,FR-004; CD-003。Proof: PO-003。
  [T004 contract](contracts/work-units.md#t004-canonical-native-plan-sealing)。

- [ ] T005 [US1] **Native Requester Grant Path**。原生 requester 签名/申请/发布 grant，实际 Provider 验证并消费密钥。Dependencies: T004。
  Design: FR-005; CD-004。Proof: PO-004。
  [T005 contract](contracts/work-units.md#t005-native-requester-grant-path)。

- [ ] T006 [US1] **Native Cold ONNX Assembly**。Selection后原生装配与既有固定bytes一致；原生worker保留有界取消/清理，删除Python helper及其文件IPC。Dependencies: T002；O-002 closed。
  Design: FR-006,FR-016; CD-005。Proof: PO-005。
  [T006 contract](contracts/work-units.md#t006-native-cold-onnx-assembly)。

- [ ] T007 [US4] **Native Tokenizer Execution**。原生 encode/decode 完整文本，与固定 tokenizer oracle 一致，无子进程解释器。Dependencies: T002；O-003 closed。
  Design: FR-007; CD-006。Proof: PO-006。
  [T007 contract](contracts/work-units.md#t007-native-tokenizer-execution)。

- [ ] T008 [US1] **Native Request Preparation and Admission**。原生输入/认证模型/工件准备与 offer policy 校验闭合，GraphAdapter/TaskAdapter 端口由 native 实现。Dependencies: T003/T006/T007。
  Design: FR-001,FR-002,FR-004,FR-009,FR-016; CD-013。Proof: PO-013。
  [T008 contract](contracts/work-units.md#t008-native-request-preparation-and-admission)。

- [ ] T009 [US3] **Shared Native Provider Host**。CLI/C++/Python 共用服务注册、准备/执行接线与停止语义。Dependencies: T006/T007。
  Design: FR-001,FR-009,FR-010,FR-012; CD-014。Proof: PO-014。
  [T009 contract](contracts/work-units.md#t009-shared-native-provider-host)。

## Phase 2: Invocation and Compatibility

- [ ] T010 [US1] **Complete Native Request Lifecycle**。独立 C++ requester 从模型/输入到真实 Response，cancel/deadline/late callbacks 保持单一终态。Dependencies: T003/T004/T005/T006/T007/T008/T009。
  Design: FR-001,FR-002,FR-008; CD-001,CD-013,CD-014。Proof: PO-001,PO-003,PO-007,PO-013,PO-014。
  [T010 contract](contracts/work-units.md#t010-complete-native-request-lifecycle)。

- [ ] T011 [US4] **Native Conversation Continuation**。原生 requester 续接/有限恢复与既有 epoch/state runtime 协作，文本/lineage 正确。Dependencies: T010。
  Design: FR-008,FR-016; CD-007。Proof: PO-007,PO-008。
  [T011 contract](contracts/work-units.md#t011-native-conversation-continuation)。

- [ ] T012 [US3] **Thin Python Native Bindings**。支持的 Python 调用转发同一 native 库，无 Python strategy trampoline/业务状态机。Dependencies: T011。
  Design: FR-010; CD-008,CD-009。Proof: PO-009。
  [T012 contract](contracts/work-units.md#t012-thin-python-native-bindings)。

- [ ] T013 [US3] **Default Route and Legacy Retirement**。所有 maintained callers 默认原生；旧运行时退出默认 import/调用图。Dependencies: T012。
  Design: FR-011,FR-016; CD-010。Proof: PO-010。
  [T013 contract](contracts/work-units.md#t013-default-route-and-legacy-retirement)。

## Phase 3: Proof and Delivery

- [ ] T014 [US5] **Runtime Dependency Exclusion Gate**。harness 将被测 native scope 与 Python harness 隔离，能拒绝已知 interpreter/libpython/helper 旁路；交付正式 MiniNDN harness/collector 并完成本地单测；真实隔离反例在T016运行。Dependencies: T013。
  Design: FR-001,FR-011,FR-012,FR-014; CD-011。Proof: PO-001,PO-010,PO-012。
  [T014 contract](contracts/work-units.md#t014-runtime-dependency-exclusion-gate)。

- [ ] T015 [US5] **Design-code Convergence Audit**。整体静态读码核对FR/CD/INV/PO、生产接线、test/oracle/harness与依赖身份，控制性发现清零；记录整体审查结论并进入T016。Dependencies: T014。
  Design: FR-013,FR-017,FR-018; CD-001--014。Proof: PO-001--016。
  [T015 contract](contracts/work-units.md#t015-design-code-convergence-audit)。

- [ ] T016 [US5] **Local Native Qualification**。全部实现和T015审查完成后，同源完整unit→integration→YOLO/Qwen MiniNDN/no-Python及必要检错证明通过，核对最终diff与证据。Dependencies: T015 PASS。
  Design: FR-001,FR-005,FR-006,FR-007,FR-008,FR-010,FR-011,FR-012,FR-013,FR-016; CD-011。Proof: PO-001--016。
  [T016 contract](contracts/work-units.md#t016-local-native-qualification)。

- [ ] T017 [US5] **Native Development Handoff**。唯一开发交付版本、维护文档与两个入口示例；外部实验单列 TRANSFERRED。Dependencies: T016 PASS。
  Design: FR-014,FR-015; CD-012。Proof: PO-012。
  [T017 contract](contracts/work-units.md#t017-native-development-handoff)。

## Dependencies & Execution Order

Merged baseline closure and Spec181 handoff → T001 → T002；T003/T004/T005 依次收口；
T006/T007 依赖 T002 和已关闭 native dependency design；
T003/T006/T007 → T008；T006/T007 → T009；
T003--009 → T010 → T011 → T012 → T013 → T014 → T015 PASS → T016 → T017。
所有任务遵循FR-018/019和统一验证规则；每任务具体范围见work-units。只有最早未关闭门可进入其对应实施。每次失败先保留新 raw/evidence、更新本 tasks/failure index。
最终 checkpoint 前核对 task 状态与实际 diff/PO；不得 blanket stage 预存修改。
