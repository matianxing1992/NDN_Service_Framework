# 修订记录

## Object handles and replica locators · 2026-09-19

按用户确认增加 R6：返回 handle 分离不可变对象身份和可更新副本位置，绑定签名位置记录、receipt 和生命周期。区分插入来源 hint 与读取副本 hint；客户端负责有界位置更新/切换，部署负责网络与本地转发。DI PreparedModel 保持材料摘要和选择授权边界。仅高层目标，不改变现有 API 或资格状态。

## Repo modes and DI ownership · 2026-09-19

按用户接受的两模式分析更新 highlevel-design.md：in-app 拒绝外部写入与副本责任，server 承担约定持久提交；新增 R5。补充 DI 本地发布、Selection 后主动缓存、显式归档、来源 lease 及可变 KV 边界。仅高层约束，不改变 API、源码、冻结目标或功能验收；下层迁移仍待设计。

## Reusable base and complete runtime SIF · 2026-09-19

在 highlevel-design.md 增加 S1–S3：外部依赖及 SDK 归可封存 base，NDNSF 自有目标在对应容器环境内构建安装并输出新完整 runtime SIF。明确源码更新的复用条件、依赖/ABI 变更的失效范围、最终封装成本及平台边界。沿用 iTiger 两层归属和现有维护入口；仅文档更新，未构建或验证新镜像。

## Build/install and MiniNDN constraints · 2026-09-19

按用户要求新增 highlevel-design.md 的 B1–B3、M1–M3：根 Waf 只构建 NDNSF 自有原生目标；依赖由各项目安装（当前 NDN-SVS 用 Waf，NAC-ABE 用 CMake）；所有后续 MiniNDN 使用系统已安装库、应用、worker 和被测模块。允许独立运行配置、日志及数据目录，禁止临时软件路径注入。仅更新约束，不宣称启动器已全部迁移，不重写历史证据。

## High-level design baseline · 2026-09-19

新增 [highlevel-design.md](highlevel-design.md)，将四模块各压缩为约 500–1000 字的用途、用例、当前设计和约束，设立 G1–G6 与 C/U/R/D 原则。README、管理规则和架构阅读入口要求后续变更首先核对原则，原则变更须显式接受。四节汉字数为 616/656/625/653，文档内链接检查通过；没有修改产品行为、冻结目标、API/PDF 或实验结论。此次是高层文档入口与治理约束更新，不表示细节文档和产品验收全部完成。

## Spec188 progress gate reset · 2026-09-17

进度审计再次把 Spec188 分为 Core product gate（T005→T011）、核心通过后的 Delivery gate（T014–T016）和 Follow-up inventory（T002–T004、T007、T012、T013）。当前候选的 Repo owner 注入、lease-bound request 和 zero request-time republication 仍未验收；本次没有新增 API、生产机制或 PASS。详见 [Spec188 progress audit r3](../specs/188-model-preparation-disk-backed-memory/evidence/progress-audit-20260917-r3.md)。

## Spec188 Scope reset · 2026-09-17

Spec188 的核心出口收窄为当前 `User::prepare(modelKey, PrepareOptions)`、Repo owner/receipt/lease、reference-only request、Selection 后 Provider assembly/drain 以及两轮当前候选 YOLO。通用 RepoCore/RepoNode/RepoClient 大对象迁移、完整 segmented serving、GB 级资源压力、Qwen 数值推理、扩展故障和 SIF/Tiger 改为 `FOLLOW_UP`/外部门；已有 T002/T003/T004/T007 focused 结果保留复用。`prepare(ModelRef)` 修正为当前真实公开入口；本次只改 Spec/契约/执行计划和证据，没有新增 API 或产品 PASS。详见 [scope audit r2](../specs/188-model-preparation-disk-backed-memory/evidence/scope-audit-20260917-r2.md)。

## Spec188 Model Preparation and Disk-Backed Artifact Memory Control · 2026-09-17

登记 prepare-time 模型制品发布、Repo 文件/范围存储、reference-only request、Selection 后 Provider assembly 及分层内存计量目标。Spec188 当前为 IN_PROGRESS；T002/T003 的 r7 Repo 静态缺陷已修复并经 r8/r9 只读复审、canonical Repo build 与三个 C++ selector 通过；真实 ENOSPC/short-write/cancel、erase/fsync ambiguous fault、TSan 及后续请求链仍未闭合。当前 T005 已加入 RepoCore-backed `RepositorySourceProvider`、Runtime 同步/异步接线和 receipt 路径 transient source release，并获 v4/v2 静态复审和 focused Runtime selector 通过；provider 仍返回完整 source vector，lease 计量、当前 caller 注入及 B188-7 的 current-candidate zero request-time publication 仍未闭合。r14 publication lines 保留为历史证据，不作为当前实现事实。Provider/Memory/YOLO 文档已改用实际注册 target/suite，未新增 Qwen、SIF 或 Tiger 核心机制。详见 [Spec188 设计变更记录](spec-design-changes.md) 与 [任务清单](../specs/188-model-preparation-disk-backed-memory/tasks.md)。

补记：T005 新增 `RuntimeConfig::RepositorySourceLoader` 与 typed `RepositorySourceError`，允许
Repo-backed catalog 在无本地 `source.file` 时延迟取得并校验模型源；当前首选 provider 接线、
receipt transient-source release 已经通过 v4/v2 静态门，matching Boost 1.71/NDN-CXX/SVS/NAC-ABE
闭包下的 `spec185-runtime` provider/lifecycle/full selectors 分别为 1/1、1/1、11/11。T005
仍为 `PARTIAL`，API snapshot/PDF provenance 因工作树漂移尚未通过完整文档门禁，lease-bound
request 与 YOLO zero-republication 仍待验证。

## Spec185 Core and Application Ownership · 2026-09-12

C-09划分既有Core调用机制、拟下移通用运行时及DI领域逻辑；T017/T018组成B0C前置批次。
现18任务12批，全部NOT_STARTED；当前源码未改，目标PDF同步，本轮不刷新历史源码快照。

## Spec185 Implementation Design Binding · 2026-09-12

C-08补内部类/字段/函数/流程与逐任务Design binding；合作策略前移至Package构造前，保持16任务11批。
Spec技能强化开工前设计就绪检查，模板及安装副本同步；目标PDF更新，不刷新历史当前源码或计产品完成。

## Spec185 Complete API and Lifecycle · 2026-09-12

新增C-07统一API与Python绑定总表、值类型及生命周期矩阵；补Subscription、有限异步等待、流错误/取消与关闭后行为。
仍16任务11批、全部未实现；目标PDF同步，当前历史源码快照及既有漂移保持原边界。

## Spec185 API and Standalone C++ Revision · 2026-09-12

全API审计新增C-05易用性与C-06独立C++契约；16任务11批，完整原生资格先于Python包装。
目标PDF同步安装/ABI、模型key、异步owner和可靠流；当前PDF仍保留54文件历史漂移边界，未计产品完成。

## Spec185 Prepared Model Runtime · 2026-09-12

目标PDF新增准备模型对象、缓存/授权分离、会话和Provider边界；完整API契约与14任务归Spec185。
当前PDF明确标注既有快照相对工作树54文件漂移，未冒充当前源码语义刷新。
双PDF仅文档构建身份更新，当前/目标API清单与源码快照独立保留；产品实现NOT_STARTED。

## TG-02 Native-First Execution · 2026-09-10

目标设计补充独立 artifact authority、真实 C++ 跨进程链路先于调用方迁移、
C++ 行为测试先于 Python wrapper checks。关联 Spec182 R11-B1 至 B9；状态 PLANNED。
当前实现/API 快照与冻结目标 API 不变，本次不计产品资格进展。

## R3 图解补充 · 2026-09-08

新增四模块图解章，两份 PDF 各 9 个视图：模块依赖、Core/DI/Repo/UAV 核心对象关系、
Core/Repo/UAV 时序及 DI 当前状态/目标流程。G7 两侧独立，明确原生请求链未就绪与
PLANNED 的区别；图例、来源和维护规则保存在 diagrams/README.md。当前 API/源码
身份按既有规则刷新，冻结目标不覆盖。图解不关闭产品验收。

## R3 · 2026-09-08

按逐章审阅修正 grant、连续流 Data 输入、精确目录、目标历史叙述和 UAV 门禁位置。
23 组关键 API 契约补入所属符号、字段、状态、结果、异常和示意；第 53 章重写生成/KV/会话。
四个 BC 补充章合入原章，当前/目标 58/63 章；TG-01 至 TG-05 展开接口责任与待决边界。
渲染器支持跨页行为表和例子，修正带参数长方法名断行，目标声明继续使用独立冻结清单。
详见 [逐章修订记录](reviews/chapter-revision-r3-20260908.md) 和 [本轮证据](../specs/182-native-di-python-bindings/evidence/design-r3-20260908.md)。

## R1 · 2026-09-07

参考 NFD Developer’s Guide 的组件、数据结构、处理流程和触发/动作组织，新增 23 组中文 API 契约与 53 个签名示例，目录可跳转至 58 个章节。
增加四模块完整声明参考，保留 public/protected、Python 命名可见性、参数/返回类型、默认值、字段、原始注释与源码位置；公开、内部与测试入口明确区分。
新增 MANAGEMENT.md，并在本机 AGENTS.md 固定同步规则；当前/目标契约独立保存，R1 初始仍一致。源快照覆盖 350 文件，API 清单覆盖其中 295 文件。
R0 历史文件可由 9c019a17 恢复；当前 source-baseline 与工作树补丁已推进到 R1，不用于冒充 R0 原始身份。

## R0 · 2026-09-07

建立四模块中文设计基线，包含 35 个章节。当前与目标技术正文一致，仅文档身份页眉不同。
覆盖 Core 调用、授权与流；Repo 对象、制品与副本；DI 规划、执行、生成与部署；UAV 飞控、任务、视频与识别。
显式保留原生 DI 请求链尚未接通、授权版本更新仍影响服务级状态等实现边界。
本目录依用户要求仅在本机保存。

## R0 维护修订 · 2026-09-07

用户调整保存要求：Design 完整纳入 Git，并新增 spec-design-changes.md 跟踪每个 Spec 的设计影响。
两份 PDF 同步更新版本管理说明，技术设计保持一致。保存精简验证结果与源码基线的工作树补丁，便于跨提交核对。

## 后续记录约定

每次目标变化记录日期、受影响章节、用户确认的行为、与当前实现的差异和待验证事项。
当前设计更新另记源码基线；不得把目标变化标成已实现。
# R2 — 基线可靠性与目标演进（2026-09-07）

扩展实现/配置基线，冻结独立目标 API/源码快照，增加 PDF 输入身份和反例回归。
修正 Repo lookup 契约，补 BC-01 至 BC-04 行为边界，登记全函数覆盖状态。
目标纳入 TG-01 至 TG-05，均为 PLANNED；见 spec-design-changes.md 与 R2 证据。
