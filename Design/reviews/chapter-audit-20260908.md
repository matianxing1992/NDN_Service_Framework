# 四模块设计逐章可理解性与完整性审阅

## Conclusion

**NEEDS_REVISION。现有 R2 可以作为架构概览和源码导航，尚不能作为完整的 API 开发者指南。**
用户指出第 53 章缺少实质内容是成立的。上一轮的 PDF、摘要和声明校验 PASS 仍是相应技术检查的
历史结果，不能证明读者可以仅凭文档实现调用或扩展。本轮完成审阅，文档改写尚未完成。

## Scope and Method

- 审阅当前设计全部 62 章、目标设计全部 67 章。相同正文完整读一次，再逐项核对目标差异；
  覆盖 129 个章节位置、67 个主题。不是按页数或关键字计数代替阅读。
- 阅读 current-content.tex 的 35 章、23 组 JSON 契约及渲染签名、BC-01 至 BC-04、
  target-roadmap.tex 的五项目标；比对目标正文、契约及补充章节差异。
- 当前 PDF 66 页，SHA-256 `879b5b47c128ea9b1aa439f46c56edad2a134676f56e040ec3eb0dec4fb3d8cb`；
  目标 PDF 69 页，SHA-256 `0662984b30fb0d84262b565b2895f72d342da4d51730ab05ded1cd7f8aaf869b`。
  第 53 章分别从当前第 57 页、目标第 58 页开始，正文延续到下一页。
- 源码抽查基于 `39f39a75418991d321c63892bb708695a827b7c0` 及审阅时工作区。
  重点核对 grant、生成协调入口、Qwen 状态机、Provider KV 操作、会话 journal 和流发布参数。
  没有宣称所有章节全部函数都重新完成语义审计，也未刷新 R2 的历史源码快照。
- 读者假定懂基本 C++/Python 和 NDN Interest/Data，但不熟悉本仓库的内部缩写。
  “可理解”按该读者能否说明一次调用和一次失败如何发生判断，未经独立读者测试。
- Context Mode active health 返回 rc=4（tasks 索引摘要过期），使用仓库文件。
  CodeGraph 定位到 canonical grant/Qwen 声明；epoch 查询混有 staging 副本，剔除后读维护源文件。
- 不修改产品源码、不构建产品、不运行实验；本轮审阅记录不等于修订后的设计验收。

## Review Criteria

按章节用途评判，概览和索引无需承担全部 API 细节。接口章节必须让读者知道：
解决什么问题、所属对象与调用者、参数从哪里来、返回值代表什么、允许的调用顺序、失败与恢复、
线程及资源寿命、一个具体例子，以及实现/测试的精确定位。字段数量不能代替字段含义。

| Verdict | 含义 |
|---|---|
| KEEP | 在概览/导航或其明确限定的用途上已基本清楚；不等于所有 API 完整 |
| EXPAND | 有实质信息，但缺少完成阅读任务必需的细节、例子或导航 |
| REWRITE | 标题承诺的核心流程/API 未形成可用说明，应重新组织本章 |
| CORRECT | 存在已确认错误、前后冲突或明显误导，先勘误再补细节 |

## Priority Findings

### F-01 / HIGH — 第 53 章标题与接口内容不匹配

[AC-18](../api-contracts.json) 仅选择 NativeInferenceHandle::cancel，PDF 显示 `void cancel();`。
其余四段列出 owner、身份字段、暂停恢复术语和资格限制，没有任何生成/KV/会话的核心方法。
读者不能知道怎样启动 prefill、怎样推进 token、怎样恢复 KV 或怎样提交下一轮会话。
第 22 章主要重复概览，第 60 章补的是请求句柄，不能补足第 53 章的主题缺口。

应按下表重组；这些是本轮找到的实际源码入口，不是已完成的替代教程：

| 问题 | 需要讲清的真实接口/类型 | 必须补的行为 |
|---|---|---|
| 一轮 token 如何推进 | [runNativeEpochCoordinator / NativeEpochCoordinatorConfig](../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp) | runtime/plan/assignment/io 如何提供；attemptEpoch、streamEpoch 和 token 轮次的区别；角色输入/反馈、停止检查与输出 |
| 会话处于什么阶段 | [QwenGenerationSessionStateMachine](../../NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.hpp) | Created、Selecting、Prefilling、Decoding 等状态；beginSelection、activate、completePrefill、completeTokenEpoch、beginReplacement 的前置条件；claimTerminalResponse 的用途 |
| KV 保留和迁移 | [NativeProviderSession](../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp) | pauseConversationStateToHost、prefetchConversationStateToGpu、pin/unpin/releaseConversationState 的参数、结果、缓存丢失及资源寿命 |
| 下一轮对话如何承接 | [NativeConversationCoordinator](../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp) | beginTurn、prepareCheckpoint、commitTurn、abortTurn、restore；parent/successor epoch、checkpoint、提交失败和恢复 |
| token 如何变为用户可见文本 | tokenizer / stable decoder / epoch 输出及 Core writer | token、稳定文本前缀、最终 flush、EOS 与终态分别由谁产生；接通情况逐项注明 |

重写需至少一个具体情景贯穿：输入 prompt → prefill → 两个 token → 结束 → 保存状态 → 下一轮。
另列一个旧 attempt 回调或 KV miss 的失败分支。图示、示例数据和伪代码必须回查实现；
未接通的 requester 段明确标识，不把各组件按想象拼成现有生产流程。

### F-02 / HIGH — 参数可见，但调用方法仍不清楚

第 38 章讲普通调用却只展开 Targeted；第 44 章讲租约/观测/并发却只展开 release；
第 54 章只列 sendMavlink 和两个 override，没有 Fields 的键与值；第 50/51 章出现大量 DTO，
没有足够的字段语义与生产者/消费者对照。多数章节缺完整调用示例和具体失败结果。
自动生成的声明经常只显示 `void cancel()`、`bool publish(...)` 等局部签名，完整限定名、
对象构造和参数来源不在同一阅读位置。应保留精确签名，同时直接标明类、职责和所属路径。

### F-03 / HIGH — 第 59 章把内部完整策略更新写成调用方输入

[current-behavior.tex](../current-behavior.tex) 和目标副本称调用者应提交“完整预期策略”。
实际 [ServiceController::grant](../../ndn-service-framework/ServiceController.cpp:592) 接收
identity、serviceName、authorizationAttribute；内部添加服务/属性，再计算 effectiveAttributesFor
并替换 Authority 中该身份的完整策略。调用方不提交整个策略对象，也不需先列出全部已有权限。
正确解释应明确“外部授权操作”与“内部 ABE 策略物化”的不同层次。第 10 章的三参数步骤较准确。
这是本轮确认的文档错误，不是产品缺陷，不能以“未验证”代替勘误。

### F-04 / HIGH — 目标文档的旧基线叙述与新目标冲突

第 13 章仍写“本版尚未采纳更新范围优化”，第 18 章保留目标 R0 口径，第 35 章写下一步由用户
指定目标；但第 63—67 章已经记录用户接受的目标方向。冻结历史基线可以保留，必须标明是历史事实，
并从相关正文链接到取代/扩展它的目标条目。仅把 TG 章节追加到结尾，不足以形成自洽目标设计。

### F-05 / MEDIUM — 边界声明多，机制解释少

“接口存在不代表完整资格”“不能混用身份”“具体以源码为准”等限制有必要，但在第 19—23、
49—54、62 章中占据了本该解释流程的位置。未接通可以写清；已实现部分仍应说明输入、状态、输出
和错误。应集中说明公共资格口径，各章只保留本章特有边界，并增加正面机制说明。

### F-06 / MEDIUM — 读者缺少术语与字段导航

ACK_CLOSED、proposal、core、projection、grant view、lineage、epoch、recipe、exact reuse、
owner 等术语高密度出现，少有贯穿样例或可点击定义。不同 epoch/digest 的生产者、消费者、
比较规则与生命周期也分散。应增加统一术语/身份表，并在首次使用处链接解释。
第 9—13 章对授权作用域的具体例子，比多数 DI 章节更接近可理解的写法。

### F-07 / MEDIUM — 勘误与补充没有回融原章

第 17 章将 LOOKUP/QUERY 的过滤能力混写，后面第 48/61 章才澄清 C++ lookup 只按精确名读取。
第 59/60 章重复前文，同时第 60 章插入模型描述符的开发 checkpoint；第 62 章把 UAV 内容与
全项目覆盖说明放在一起。读者要反复翻页才能拼合答案。应把补充合并回相关主章，历史修订另存。

## Chapter Review — Shared Topics 1–35

下表同时适用于当前和目标的对应章节；目标特有差异在后文另列。每行均为人工阅读后的判断。

| Chapter | 主题 | Verdict | 已说明与仍需补充 |
|---|---|---|---|
| 1 | 文档范围与源码基线 | KEEP | 四模块、快照与事实口径清楚；增加读者前置知识、概览/教程/API 三条阅读路线，“完整视图”不能等同完整契约。 |
| 2 | 系统组成与职责边界 | KEEP | 分层图和所有权表可理解；补部署进程边界和一个跨模块实例即可，不需在此展开全部 API。 |
| 3 | Core 角色、命名与启动 | EXPAND | 角色职责清楚；名字文法转交源码，启动只列概念。补完整名字例子、最小配置、初始化顺序和失败位置。 |
| 4 | 普通与 Targeted 调用 | EXPAND | 五步流程和时序图有效；补 payload/回调实例、token bootstrap 的触发与恢复、每类失败如何到达调用者。 |
| 5 | 协作规划与选择事务 | REWRITE | DEFERRED、选择事务和 reservation 混在一章；拆开对象、调用顺序和状态，给最小 CollaborationPlan 及事务崩溃分支。 |
| 6 | 对象、连续流与调用流 | EXPAND | 三种数据形态区分较好；各补一组真实入口、数据单位、结束条件与容量配置，明确 FEC 使用条件。 |
| 7 | 能力、并发、持久状态与观测 | REWRITE | 多个大主题仅各一段；拆线程/资源/持久恢复/观测，给所有权图、队列配置和关闭顺序。 |
| 8 | 身份与请求保密 | EXPAND | 签名、授权、加密的分工清楚；补各密钥产生/传递/销毁表、一个请求字段绑定示例和失败入口。 |
| 9 | 授权版本与权威 | KEEP | 各版本/摘要和比较边界较具体；补一组实际版本样例及首次术语定义，保持独立于目录 epoch。 |
| 10 | 在线授予 | EXPAND | 操作顺序和目标/无关节点影响有实质内容；补合法属性构造、三参数调用样例、false 的具体分支，避免第 59 章错误外推。 |
| 11 | 在线撤回 | KEEP | 持久化、轮换失败和追溯边界解释较清楚；补三种 RevocationTarget 构造及重复调用结果便于使用。 |
| 12 | 版本发现 | KEEP | 触发条件、默认配置、Interest 名称与时效边界明确；补离线恢复时序和失败回退；目标链接 TG-01。 |
| 13 | 授权变化影响 | CORRECT | A/B/S/T 例子有帮助；目标 R0 的“尚未采纳”与 TG-01 冲突，区分历史基线和已接受目标并回链。 |
| 14 | Repo 对象模型 | EXPAND | 组件和对象分类可读；补对象/packet/artifact 的具体 JSON 与相互引用、各字段权威来源和部署能力表。 |
| 15 | Repo 持久化与可见性 | EXPAND | SQLite/缓存/制品后端已区分；补写入失败与崩溃恢复时序、覆盖/并发规则和准确 commit 可见点。 |
| 16 | Repo 可恢复传输 | EXPAND | 高级操作和阶段已列出；补一次 upload/fetch 调用、恢复参数来源、receipt 样例和取消后文件状态。 |
| 17 | Repo 目录与副本 | CORRECT | 混写 LOOKUP/QUERY 过滤集合，容易误认为 C++ 同样支持；按入口列能力并与第 48/61 章统一，补 tombstone/TTL 的具体状态。 |
| 18 | DI 入口和实现层次 | EXPAND | Python/native 未接通边界明确；缺最小应用、配置和返回对象。目标 R0 说明需链接 TG-02，避免重复旧目标。 |
| 19 | DI 图、候选与放置 | REWRITE | 只有步骤和术语，无法解释如何产生合法候选；用一张小图贯穿节点/边、角色/rank、预算、offer、assignment。 |
| 20 | DI 计划、授权与制品 | REWRITE | proposal/core/grant/projection 的字段关系不清；补一组完整数据例子、摘要生成顺序和发布前后身份变化。 |
| 21 | DI 角色执行与数据流 | REWRITE | owner 表不等于执行设计；补两角色输入等待/执行/发布时序、TensorBundle 样例、线程与取消/失败传播。 |
| 22 | DI token、KV 与延续 | REWRITE | 流程概述有方向，但没有状态迁移、缓存操作或实际例子；与第 53 章共同重构，明确每种 epoch。 |
| 23 | DI 部署与扩展 | REWRITE | 部署、管理、host、策略扩展混杂；拆出部署生命周期和扩展教程，列支持矩阵、配置及真实加载入口。 |
| 24 | UAV 容器与界面 | EXPAND | 服务名和状态组有用；补启动配置和一次 GUI 动作对应的 request/response、状态变化与错误展示。 |
| 25 | 飞控与操作权 | EXPAND | 三层权限有解释；补 Fields schema、MAVLink 命令/ACK 例子、检查所在进程及各 backend 阻塞/失败行为。 |
| 26 | 巡逻与补偿 | EXPAND | mission/job 分离和补偿规则可读；用两个 part 示例说明超时、迟到结果、新 attempt 和任务完成判定。 |
| 27 | 视频、遥测与录像 | EXPAND | 采集/流/存储关系明确；补 start→descriptor→subscribe→stop→playback 时序和具体队列/保留参数。 |
| 28 | 检测与多视角 | EXPAND | 替身与真实模型路径区分清楚；补最小 job/profile/views/result 示例及算法注入位置，避免只有限制。 |
| 29 | 跨模块完整路径 | REWRITE | 标题承诺完整路径，正文仍为三段摘要；至少一个带 API、消息/数据名字、状态和失败出口的完整例子。 |
| 30 | 构建配置部署 | EXPAND | 边界说明清楚但不能据此启动应用；链接精确维护命令和配置示例，说明最小依赖、必需字段及就绪判断。 |
| 31 | 实现状态与验证 | EXPAND | 证据层级合理；补能力→实际入口→状态→精确证据映射，避免仅列测试目录和通用“已有”。 |
| 32 | Core 源码索引 | KEEP | 作为导航可用；引用编号改可点击，缩写路径补足规范根；不把导航当行为契约。 |
| 33 | Repo/DI 源码索引 | EXPAND | 多组只有类名或省略路径，D6 漏掉第 53 章实际 Qwen/KV 入口；补全路径和对应契约节。 |
| 34 | UAV/交付索引 | EXPAND | 有模块入口，但部分路径需读者推导；标明 module-inventory 是 R0 历史盘点，避免误作最新范围。 |
| 35 | 文档维护 | CORRECT | 仍带 R0/“下一步由用户指定目标”等旧叙述；与 R2 MANAGEMENT 的独立快照/构建身份规则统一，历史放修订记录。 |

## Chapter Review — API Topics 36–58

| Chapter | 主题 | Verdict | 已说明与仍需补充 |
|---|---|---|---|
| 36 | API 阅读方法 | EXPAND | 范围限制清楚；补读者怎样从任务找到 API、怎样阅读一个条目的示例，定义 owner/epoch/lineage 等术语。 |
| 37 | Core 容器生命周期 | EXPAND | start/stop/注册顺序明确；补构造/addUser/addProvider/hook 的完整例子，明确借用与拥有对象的区别和异常结果。 |
| 38 | Core 请求/Targeted | REWRITE | 只列 Targeted 两个签名，普通 RequestService 让读者另查；分别给普通/Targeted 可理解的调用与 callback/error 表。 |
| 39 | Provider 作用域注册 | EXPAND | RAII 和线程前提有实质内容；补 ACK handler 与执行 handler 类型、调用样例和 close 与执行竞争的具体结果。 |
| 40 | 延迟规划/提交 | EXPAND | 参数、闭包摘要和时序清楚；补 CollaborationPlan 字段、onAckClosed 输入例子、重复/过期提交返回表。 |
| 41 | 调用流 API | REWRITE | 未说明 publish 的 bool、finish reason、writer 获得方式和 options 默认值；补能串起 User/Provider 的流例子及背压状态表。 |
| 42 | 对象/连续流 API | REWRITE | 跨三种 API 家族仅列少数入口；push 实际接收 signed Data，正文“带元数据的块”不够准确。分拆输入类型、构造、取数和停止。 |
| 43 | 授权 API | EXPAND | 三个关键签名已有，bool 解释仍笼统；逐方法列无变化/拒绝/轮换失败的区别，提供 RevocationTarget 和 PolicyStatus 示例。 |
| 44 | 租约/观测/并发 API | REWRITE | 仅有 release，无法知道 lease 怎样取得、消费、续期和观测；按生命周期补方法、ExecutionLeaseResult 与线程设置作用域。 |
| 45 | Repo 对象/packet API | EXPAND | 参数和 packet 完整性说明有用；补 manifest 字段例子、putDataPacket、查找失败和本地/网络重载区别。 |
| 46 | Repo 上传提交 API | EXPAND | 默认值、提交和网络限制明确；缺 descriptor/result/receipt 实例、错误类型及 idempotency 行为表。 |
| 47 | Repo 下载状态机 API | REWRITE | 混合高层 fetch_file 与底层 receive/markVerified，却没解释桥接；展开 **kwargs，给 session 状态迁移和一次中断恢复例子。 |
| 48 | Repo 目录/副本 API | EXPAND | 精确 lookup 勘误有效；补 delta 过旧/空结果、目录记录 schema 和各入口能力表，统一第 17 章。 |
| 49 | DI Python/native 请求 API | REWRITE | 签名存在，但 model/task/input/config 如何构造未讲；分别提供当前 Python 路径与原生句柄能力例子，错误状态链接第 60 章。 |
| 50 | DI 模型/放置扩展 API | REWRITE | 两个虚方法不足以实现扩展；补 descriptor/graph/candidate/offer/proposal 字段、最小策略、注册及被调用位置。 |
| 51 | DI 准备/发布/封存 API | REWRITE | 三个签名没有完整可调用链；补 prepare/inspect/ensure/bind/seal/grant/project/encode 的输入输出关联与当前接线状态。 |
| 52 | DI Provider 执行 API | REWRITE | serve 参数对象和真实 runner/dependencyIo 配置缺失；补构造/注册/关闭实例、grant 验证输入来源和错误分类。 |
| 53 | DI 生成/KV/会话 API | REWRITE | 只有 cancel 签名和概括段落；按 F-01 重写生成循环、状态机、KV API、journal 与贯穿例子，标清未接通段。 |
| 54 | UAV 飞控 API | REWRITE | 三个 sendMavlink 声明重复但无 Fields schema；提供一条命令和 ACK/拒绝/超时结果，分清接口与具体 backend。 |
| 55 | UAV 任务/报告 API | EXPAND | 身份、reason 和恢复前提有用；补 task/part/job/report 数据样例、允许迁移表与补偿后迟到结果处理。 |
| 56 | UAV 视频/遥测 API | EXPAND | 遥测单位和返回标志明确；补 push bool、popLatest 空值/丢帧规则，以及 capture 到播放/录像的实际接口连接。 |
| 57 | UAV 多视角扩展 API | EXPAND | 默认替身与真实模型限制明确；补 job/profile/views/result 字段与可注入算法示例、每种失败结果。 |
| 58 | API 扩展检查 | EXPAND | 作为 checklist 有帮助，作为教程缺示范；增加一次新增服务/策略的最小示例及对应检查结果，避免重复管理规则。 |

## Chapter Review — Behavior and Target Topics 59–67

| Chapter | 主题 | Verdict | 已说明与仍需补充 |
|---|---|---|---|
| 59 | BC-01 授权安装 | CORRECT | F-03 的调用方完整策略说法错误；修正后并入第 10/43 章，保留内部 ABE 策略与外部操作的区别。 |
| 60 | BC-02 句柄等待/观察 | EXPAND | 局部超时、线程禁止、回放、迟到 dispatch 有具体行为；加 result/observe/cancel 示例并回融第 49 章；描述符增补迁入第 50 章。 |
| 61 | BC-03 精确目录查询 | KEEP | 参数、读取、锁范围和能力边界说明具体；补 store 的具体异常映射后并入第 48 章，避免双处答案。 |
| 62 | BC-04 UAV/覆盖 | REWRITE | UAV 内容主要是资格限制，后半转为全项目覆盖；把覆盖规则移管理章，将 UAV 的已知方法/状态补回第 54—57 章。 |
| 63 | TG-01 授权影响范围 | EXPAND | 动机与迁移边界可懂；仍为目标方向。补版本字段与作用域矩阵、通知/拉取状态、旧客户端行为和可判定场景。 |
| 64 | TG-02 原生唯一 owner | EXPAND | 迁移方向清楚；缺目标对象依赖图、完整调用顺序和每个 Python owner 的接管映射，补与 Spec182 契约的精确链接。 |
| 65 | TG-03 异步契约 | EXPAND | 原则清楚；尚未落实到每个 API 的执行器、终态、取消传播、背压配置，需矩阵及竞争场景。 |
| 66 | TG-04 Repo 恢复 | EXPAND | 幂等/能力方向可懂；补 operation key 生命周期、条件更新/冲突、持久化边界、支持矩阵和格式兼容决策。 |
| 67 | TG-05 UAV 类型边界 | EXPAND | 分层目标可懂；缺命令/结果 schema、兼容转换、状态机和 Qt/后端调用边界；真实飞控验收另列。 |

## Target Overlay Review

目标第 1 章已声明冻结基线与 TG 优先，但第 13/18/35 章仍用“本版/下一步”描述历史状态，
应改成明确的“冻结基线行为”，并在相应位置指向目标差异。目标第 48 章已单独标注冻结精确查询，
这种做法可推广。目标第 60 章不含当前模型描述符工作区增补，属于有意差异，不要求强行同步。
目标的独立 API 清单保留了签名身份，但 JSON 清单本身不是易读开发文档，仍需人可读的字段和流程。
五个 TG 章节现在可作为改进方向及待决事项，不能称为已完成的 API 目标设计。

## Rewrite Order and Acceptance

1. 先勘误第 59 章和目标第 13/18/35 章，统一第 17/48/61 章的 Repo 能力描述。
2. 以第 22/53 章为首个完整重写单元，按 F-01 填齐真实接口、状态、字段与例子；再处理第 18—21、49—52 章。
3. 补 Core 协作/流/租约和 Repo 恢复，再补 UAV 飞控/任务/视频；逐章按本清单收敛。
4. 将 BC 内容回融原章，增加术语、身份/摘要字典、能力表和精确交叉引用。
5. 最后逐项细化 TG 的 API 决策与迁移矩阵；仍未决定的接口标明待决，不用新增类名制造完整感。

每个重写单元按以下读者任务验收：能从文档构造一次调用；能说出参数由谁生成；能追踪
状态与返回；能判断一次失败是否可重试；能识别资源何时释放；能定位真实源码及测试。
示例若未编译/未执行则明确标记，不能标为已验证。复杂状态必须给图或转移表，而不是堆叠名词。
一次规范示例可被相邻章节引用，无需每章复制。篇幅由机制决定，不设置凑页数指标。

## Work Unit Status

逐章审阅完成；上述问题均为待处理项。R2 的历史技术检查结果不撤销，增加本轮语义可用性
NEEDS_REVISION 结论。产品任务、产品设计目标、源码和两份 PDF 均未在本轮改写。
