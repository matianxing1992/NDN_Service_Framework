# Remaining Production Chain Review and Replan

## Decision and Scope

用户授权暂停新增实现、审查并重排剩余生产调用链。基线 ac962cc8，保留 f30c8c75
YOLO 批次和全部既有证据，不重建 Spec、不改产品源码、不重新运行产品构建/实验。
当前未提交的 native-dependency-design.md、native-generation-design.md 属其他会话。
本记录是生产入口和计划依赖审查，不是所有源文件的完整缺陷审查或 T015 PASS。
此前 review-agent 的加载不追认旧批次已使用官方技能；后续每个小任务按当前规则
实际加载并记录该技能与审查范围。

## Findings

| ID / Severity | Evidence and consequence | Planning correction |
| --- | --- | --- |
| RC-01 / HIGH | NativeInferenceClient.cpp:330 的 dispatchOperation 仍无条件返回 NATIVE_REQUEST_PIPELINE_NOT_READY；构造保存 preparation/admission/grants，未形成生产编排 | 将完整 request 状态推进列为独立可观察成果，不能用组件 case 数关闭它 |
| RC-02 / HIGH | NativeRequestPreparation.cpp:167/185/291 的 inspect/roles/artifacts 仍要求配置 port；NativeModelAdapter 的继承实现目前只找到测试类；YOLO fromOnnxCatalog 调用方当前是单测 | R2 必须交付实际模型适配器和 catalog/recipe/publication 组装入口，不能再次只交付一个可注入 helper |
| RC-03 / HIGH | T003/T004 剩余描述含 requester 接线，T010 又依赖这些卡；若把最终调用方接线当作前置卡 DONE 条件，会形成验收上的循环 | 局部 card 验收按其冻结契约；T010 拥有最终 requester 接线，T016 拥有完整 PO 实验。局部仍缺 oracle/接口则保持 PARTIAL，不能借重排直接勾选 |
| RC-04 / MEDIUM | NativePlanSealer.cpp:367/368 消费外部 inputs.dataflow/deviceBinding；grantViewForIdentity 消费 core/security/offer identity。生产字段生成方仍须与真实 preparation/ACK/role 对齐 | 按字段生产者→消费者分 R2，再统一验证 sealer/grant，避免逐字段改动和重复构建 |
| RC-05 / MEDIUM | 既有步骤按卡依赖顺序呈现，却没有统一列出每批新增的生产能力和最终证明责任 | R1–R7 映射所有未完成卡；每批规定出口、共同选择器和硬门，批末统一运行 |

这些是既有设计/覆盖缺口，不伪称本轮引入的回归。安全校验不得因优先闭合请求而
绕过；Core 继续负责网络认证、BeginCollaboration/CommitCollaborationPlan/Response，
DI 负责模型语义、候选、数据流和授权绑定。没有证据支持改写 Core 或另建编排框架。

## Verified Chain Map

| Segment | Current source / evidence | Remaining owner |
| --- | --- | --- |
| 输入、模型、图、候选 | NativePlanning.hpp NativeModelAdapter 仍为抽象接口；NativeRequestPreparation::inspectModel 委托 port；真实 ONNX helper、YOLO factory 与候选独立 oracle 已有 | R1 关闭模型语义差距；R2 提供实际 source/catalog/task adapter 接口实现 |
| ACK 和 placement | Core ServiceUser.hpp BeginCollaboration 已存在；NativeOfferAdmission 与 V3 placement 已有定向证据；默认 client 未消费这些输出 | R2 绑定可消费的完整输入；R3 接入 ACK_CLOSED 后的流程 |
| publication、seal、grant | NativeCanonicalArtifactPublisher 与重新认证已有回归；sealer 要求 dataflow/device inputs；T004/T005 仍 PARTIAL | R2 完成生产字段与 grant/role 绑定；不得继续传旧规划片段 |
| 提交、执行、结果 | Core CommitCollaborationPlan 已存在；NativeInferenceProvider.cpp:343 组装 makeNativeProviderCollaborationRuntime；默认 client 尚无成功提交/返回链 | R3 复用现有 provider host、Core 协作和终态；不得复制第二套 provider |
| 流式生成、会话 | NativeConversationCoordinator 有 begin/abort/prepareCheckpoint/commit/restore 方法；其存在不证明 client 接线 | R4 按 T010-C/T011 全部状态与恢复契约接线及验证 |
| 绑定、迁移、退出、隔离 | T012–T014 记录仍有未开始/部分完成；新 class ABI 不能由旧 import 证明 | R5 同库绑定和全部调用方；R6 退出及资格工具；R7 实测 |

## Reuse Ledger

- f30c8c75：已读 raw focused.log，52 cases/1277 assertions PASS；fresh build
  6m48.938s；design-final.json ok=true。证明实际 ONNX→semantic factory→完整候选
  的定向行为，不证明默认 catalog/client 或无 Python 网络执行。
- 7552523b、354a54a1、919dc689：复用原证据中的 publication/rank、完整候选、
  resource 契约结果；本轮没有重新运行，不将它们扩展为完整链证明。
- T001/T002/T006/T007/T009 的 DONE 和原证据原样保留；本轮只核对相关入口与记录，
  不重新签发这些卡的完整验收。依赖、ABI、源码改变时，仅失效受影响的证据。
- 最新可复用 native build tree 为 .codex-tmp/spec182-yolo-semantic-r1/build；
  ABI 改变须 fresh tree，兼容变更才增量。禁止同树竞争构建，不自动 clean/rebuild。
- 全局 workflow 检查的旧 anchor 已由 f30c8c75 修复；Context Mode active health
  本轮因 spec/plan/tasks hash 过期 exit=4，采用仓库与实时源码；CodeGraph 临时 staging
  副本不作为当前代码。原日志保留，不为索引问题重跑产品测试。

## Remaining Batches

以下 R1–R7 是能力阶段，不是七个预先固定的执行批次；保留本节 anchor 兼容已有链接。
阶段重组执行顺序，不替换 17 个父任务或 36 个 execution cards。Owner 均为当前
实现者。每阶段按下节规则领取具体逻辑批次，每小任务独立只读 review-agent 静态门，
批末组合审查后统一构建和相关单测；既不每个小改动构建，也不等整个大阶段写完才测试。
现有 Depends 和 ABI/安全硬门保留。R2 中 T008 与 T004 的顺序可按生产者先行，
二者都须先满足 T003-C；不存在将未测试静态结果自动当作前置验收的授权。

| Stage / Members | Stage exit behavior and coverage | Hard entry / available validation |
| --- | --- | --- |
| R1 Model and Candidate Closure / T003-A,B,C | 完成 Qwen semantic layer→实际装配节点/制品映射，复用 YOLO 实际图 factory；两模型策略产生完整、独立可验证的候选及 placement 输入；列清 source、role、artifact 字段生成方 | T002-A；按 A→B→C 既有依赖领取，仅补剩余验收。共享 Spec182NativePlanning、Spec182V3Placement；独立两模型 oracle，错身份/图/资源/rank 拒绝 |
| R2 Preparation to Authorized Plan / T008-A,B; T004-A; T005-A,B | 实际 native ModelAdapter、catalog/recipe/source owner 配置，输入/结果语义；认证 ACK→role→placement→publication→重新认证→seal/grant/projection 的生产字段全链；输入不得由测试 backfill | T003-C、T006-D、T007-B；T004-A→T005-A→T005-B、T008-A→T008-B 的硬顺序不变。共享 Preparation、CanonicalPublisher、OfferAdmission、V3Placement、PlanSealer、GrantClient 与 case-manifest authority 选择器；actual grantView 和篡改拒绝 |
| R3 Complete Request / T010-A,B | 默认 NativeInferenceClient 实际调用 R2 owner、Core BeginCollaboration、ACK_CLOSED、CommitCollaborationPlan、已存在 provider host、结果 decode；成功/失败/取消/超时/close/晚回调均到唯一终态。首个固定 YOLO 请求只是关键里程碑，不削减 Qwen 或后续范围 | T005-B、T008-B、T009-C 后 A→B。ClientState、NativeInferenceClient、计划中的 ClientRequest；运行前注册实际 suite。编写真实 request/Provider 集成 harness，真实跨进程执行仍留 T016 |
| R4 Streaming and Conversation / T010-C; T011-A,B,C | 同一请求 owner 接入 acceptance/replacement、sampling、stable epoch、KV/会话提交与恢复；完整 Qwen 文本、重复/中断/恢复语义一致 | R3 后 C→T011-A→B→C；StreamAcceptance、Sampling、EpochText、Conversation 按 manifest 实际注册；复用 tokenizer/worker，不新建平行 lifecycle |
| R5 Same-Library Callers / T012-A,B; T013-A | 可选 Python bindings 使用同一原生 owner，所有 maintained callers 迁移；两入口的参数、异常、寿命和结果一致 | R4；T012-A→B→T013-A。新 ABI extension/installed consumer 与真实调用方 parity；manifest 按最终源码再生，不能用 token inventory 替代调用方验收 |
| R6 Retirement and Qualification Tools / T013-B; T014-A,B; T015-A | 退出旧默认 Python runtime，完成隔离 collector 和所有 harness；逐条映射 FR/SC/PO 到实际入口、观测和判据，补审跨批生产接线 | R5 后按卡顺序；静态/工具单测，不提前跑集成。T015 未通过不能进入 R7 |
| R7 Final Qualification and Delivery / T016-A; T017-A | 按完整unit→integration→MiniNDN/no-Python执行全部既定行为/负例；真实请求、授权冷装配、两模型/两入口、流式恢复实测后交付同源版本 | T015-A；case-manifest 与 proof-design 的全部原有选择器及 qualification artifacts 原样保留。T016 全通过后 T017；SIF/Tiger 由实验机执行，不自动上传/Slurm |

R3 的“接通”先指源码与相关单测中的编排完成，不能宣传为真实网络请求已通过。
按 FR-013/018/019，真实完整请求证据在 R7 获得。若希望提前实跑，需另行明确修改
验证阶段；本次重排不偷偷把 integration 重命名为 unit/smoke。

## Dispatch and Stop Rule

### Executable Batch Selection

批内高内聚、批间依赖清楚；已有强关联代码一起改，目标不是增加架构耦合。
恢复执行后，只为当前就绪阶段登记下一批 `R<n>-B<k>`，不一次性猜定全部细批。
该登记复用 tasks.md 的 Current Checkpoint 和一份批次 evidence，不新增父任务或
按文件/字段建行政卡。登记必须包含：

- 一项完整可观察行为及出口，具体成员/符号与实际调用方，接口的生产者和消费者。
- implementation dependencies 与必须已测试/已验 ABI/安全的 acceptance dependencies；
  现有 Depends 不因新批次 ID 自动降级，未满足的硬门不得跨越。
- 单一负责人、共享 build tree/ABI 边界、去重后的实际测试选择器和独立判据；
  阶段表的 selectors 是候选集合，具体批次仅运行受影响范围，planned 用例先注册。
- 既有证据复用范围、尚待 T016 的集成义务和本批完成条件。未测试仍为 PARTIAL。

### Cohesion and Split Rules

| Stage | Execution batch guidance |
| --- | --- |
| R1 | 共享 graph/candidate 契约及其两端一起改；在契约稳定且硬前置满足时，YOLO/Qwen 独立特殊逻辑分别验收，不为凑一批互相等待。不改变 T003-A/B/C 的冻结 Depends；若发现其只有名义依赖，先以源码证据修订卡片依赖再调度 |
| R2 | 先确定 source/catalog/task adapter→role/placement→publication/recertification→seal/grant 的完整字段流。跨越同一契约的生产者、消费者、序列化和校验纳入同批；若前半段有稳定出口和独立行为判据，可先闭合再进入后一批。不能以缺省值或测试 backfill 把半条链当出口 |
| R3–R4 | 围绕完整请求或状态转换分批，相关成功、失败、取消、晚回调和清理一起实现；不能把错误处理推到功能批次之外。基础请求与流/会话扩展按现有硬顺序推进 |
| R5 | 先闭合本阶段冻结的原生 API/ABI 与绑定寿命，再在接口稳定、前置满足后按真实调用方族迁移；每族涵盖入口、参数/异常、结果和退出路径，不堆积全项目未验证改动。共享 API 变化时同批更新受影响调用方 |
| R6–R7 | 退出旧路径与检测它的 collector/harness 保持同批关联；最终验证仍按完整unit→integration→MiniNDN/no-Python的原定顺序，不因实现批次数变化删减或反复执行无变化的资格用例 |

仅因文件数量、模型名称或下一张卡开始，不触发拆批或重建。只有存在稳定接口、
完整行为及独立验收价值时拆批；共享契约尚在变化、修复会同时影响两端时应合批。
批次范围扩大或硬依赖变化，先更新该批登记和审查范围，保留已完成证据。
每个小任务编码→官方 review-agent 只读审查→修复/复审→同批下一任务；整批入口到
终态/清理审查通过后统一构建与相关单测。当前已闭合批次及时测试，不等待未来批次。
模板/ABI/链接等具体阻塞才允许已记录的最小诊断；失败修复后只重跑受影响范围。

产品新增实现保持暂停；本轮结束不自动领取 R1。用户明确恢复后，从 R1 的实际
未完成边界按上述规则登记 R1-B1，先冻结该批每个关键字段/接口的生产者和消费者，引用已有契约，
不再围绕每个字段分别建卡、建报告、构建。新发现若改变共享契约，合入当前批次并
重审受影响路径；无关优化另记，不扩张当前批次。批末记录新增能力、实际验证范围、
剩余闭合边和 build 次数/时间，不以 case 数或提交数代表收敛。
每批开始解析 manifest 的 existingSuite/selector；尚 planned 的用例须先注册，
不能以无匹配测试退出成功放行。安全身份、独立 oracle、no-Python 及失败/取消门不删。

## Review Limits and Validation

审查覆盖默认 requester→准备端口→seal/grant 输入→Core 提交入口及 provider runtime
接线，并核对后续任务责任；未重做全部 worker/tokenizer/security 数值或并发审计。
不将部分读码写成 No findings 或全系统 STATIC_PASS。结构/链接检查只证明计划可解析；
本轮不增加任何产品 DONE，不重置已有通过卡。阶段映射及分批规则是计划成果，不是运行成果。

本轮 validation：validate_design.py、audit_speckit_structure.py --strict、
check-prerequisites.sh --json --require-tasks --include-tasks 均 exit=0；git diff
--check PASS。自动核对 23 张未完成卡恰好映射到七个能力阶段，无遗漏、无重复；与 HEAD
比较全部 36 张卡的 DONE/PARTIAL/NOT_STARTED 原值不变。原始结构/链接/前置结果见
[design](../../../.codex-tmp/spec182-production-replan-20260908/design.json)、
[structure](../../../.codex-tmp/spec182-production-replan-20260908/structure.json)、
[prerequisites](../../../.codex-tmp/spec182-production-replan-20260908/prerequisites.json)。

后续用户授权细化分批：R1–R7 明确为能力阶段，新增 R<n>-B<k> 领取规则及
R1/R2/R5 等阶段的合批/拆批边界，不预先强制七次构建。仅修改计划/追踪文档。
validate_design.py 与 strict structure check exit=0，diff check PASS；比较全部
36 张原卡状态无变化。结果见 [design](../../../.codex-tmp/spec182-batch-selection-20260908/design.json)
和 [structure](../../../.codex-tmp/spec182-batch-selection-20260908/structure.json)。
