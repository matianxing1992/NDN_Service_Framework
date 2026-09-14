# Advisor Review Reconciliation — 2026-09-14

## Scope and Status

本轮逐句复核英文 Proposal 的摘要、七章正文及所有被引用的正文模块，逐页复核当前 presentation 的句子、表格行、图示标签和研究边界。中文稿同步本轮变动；此前未改动的中文模块沿用既有双语审查，不冒充本轮重新翻译。老师的两份批注原件保持不变：`../reference-pdfs/proposal 2.pdf`（11 页，批注已嵌入页面文字）和 `../NDNSF_proposal_reviewed.pptx`（批注主要为红色文本框，并非 PowerPoint comments parts）。

`advisor-review-validation-20260914.json` 保存源文件身份、提取到的 19 条 PDF 批注、26 条 PPTX 批注、构建及覆盖检查。PPTX 的 C01–C26 沿用 `review.md` 的编号。下文使用稳定的节名／slide 标题，实际页码见验证记录，避免插页后映射失效。

状态定义：`TEXT_REVISED` 表示对应表达或组织问题已在文稿落实，不代表导师已认可；`OPEN_RESEARCH` 表示完整文献比较或实验证据尚缺。问号等含义不完全确定的批注按所在页面的主张解释，并保留导师确认边界。不能把“所有文字批注已有处理记录”写成“所有研究问题已经解决”。

## Review Findings and Changes

1. 引言先解释实际决策缺口，再介绍 NDN。通信方式本身不是缺陷；建立通信不能自动确定当前意愿、权限与完成情况。NDN 库提供可组合基础机制，不应被描述为缺少应有的高层框架功能。
2. 在应用动机中加入按调用生命周期推导需求的段落，并关联 RICE、NSC、DNMP。需求表限定本文范围，不声称两个应用足以穷尽所有服务框架功能。Slides 在 RQ 前加入 `Application Needs Determine the Research Scope`。
3. 名称的对象标识、Interest 转发和信任规则三种用途在引言及 NDN slides 明确表达；详细背景仍放在第二章。
4. 在四消息定义前加入 `Invocation Overview: Request a Timely Aerial Image`：A 愿意参与，B 忙碌；User 选 A，A 验证 Selection 后拍摄。图中箭头表示应用因果关系，不表示绕过 Interest 的 Data 推送。
5. 修正引言的采集时序：新照片在 Selection 授权后产生，不能说它已经随 Selection 交付。既有图像作为处理输入可在 Selection 后交付，与新采集图像是不同情形。
6. 摘要将授权设计与协作扩展分成独立段落；正文仍区分初版多候选选一和后续多角色协作。保持 UAV 控制、遥测、直播、录制、任务及 DI 准备、单机／分布式推理、K/V、增量输出的完整覆盖。
7. 收紧 positive/negative ACK、Content 保密、角色／身份、重放、grant/withdraw 等用语。没有把 RequestNonce 改回 Interest Nonce，也没有把 ProviderChallenge 当作可复用的 bearer credential。
8. CaSCON 引用从机构新闻改成可确认的正式论文条目。只确认书目信息；完整特性比较仍为 `OPEN_RESEARCH`。Spring 2027 的两项关键日期重新核对官方日历。

## PDF Comment Traceability

PDF 页码指老师原件。引号保留批注意思与关键词；完整提取原文保存在验证记录中。

| 原页／批注 | 本轮处理及当前位置 | 状态 |
| --- | --- | --- |
| P2 “are there non-service oriented apps?” | 摘要从具体调用问题开始，不把所有联网应用概括成某一种应用范式 | TEXT_REVISED |
| P2 “facts, not the problems themselves” | Introduction / Service Discovery and Authorized Invocation：明确通信与应用决策之间仍需解决的工作 | TEXT_REVISED |
| P2 “not reflected in intro” | 摘要的发现、授权、协作、评价与引言三个 RQ 对应，不保留无展开的复杂度结论 | TEXT_REVISED |
| P2 “lack” / primitives | 引言与 NDN Background 正面说明由 primitives 构建高层功能，不归咎于 NDN 库 | TEXT_REVISED |
| P2 “Which includes” | Initial NDNSF Foundation 及摘要列出发现、选定、授权；应用章节解释具体服务 | TEXT_REVISED |
| P2 “paragraphs have glitches” | 摘要按问题／授权／协作／证据分段，正文按需求→机制→验证组织 | TEXT_REVISED |
| P2 “what kinds of providers” | 引言用相机 Provider 开场；动机比较感知与计算 Provider，角色章节定义可兼任的软件角色 | TEXT_REVISED |
| P3 “intro didn't mention ... complexity” | 删除无测量支撑的复杂度改善承诺；管理复用为理由，成本为拟评价量 | TEXT_REVISED; OPEN_RESEARCH |
| P3 “different issues ... structure” | Application Needs 生命周期推导＋需求表＋三个 RQ，避免不同抽象层混列成 gap | TEXT_REVISED |
| P6 “services?” | 初版四消息定义及具体相机服务例子，不把一般 Data 获取直接当执行调用 | TEXT_REVISED |
| P6 “insightful ... explain why” | 补为何已知服务/历史地址不等于当前可行执行者；比较指定车辆操作与可替换计算者 | TEXT_REVISED |
| P7 “not host-centric ... mismatch” | 明确问题不是 host-based communication 自身，也不声称成熟 RPC 不能发现或流式调用 | TEXT_REVISED |
| P7 “replace ... data-centric” | NDN 使用 data-centric；不把“data-driven”当架构名称替代词 | TEXT_REVISED |
| P7 “simple example ... invocation?” | 以 Request–ACK–Selection–Response 解释调用，另在背景解释普通 Interest/Data 获取 | TEXT_REVISED |
| P7 background placement / naming's three roles | 背景第二章；引言与 slides 明确对象标识、转发和信任规则三种命名用途 | TEXT_REVISED |
| P7 “not also adopt” | 表述为 NDN 的架构特征，不写成附加选项 | TEXT_REVISED |
| P11 primitives / “Remove the negativity” | 不把应用层决策当作 NDN 原语缺陷 | TEXT_REVISED |
| P11 “issues at different levels” | 需求分类是相互关联的职责，不是严格四层；授权与状态跨类别 | TEXT_REVISED |
| P11 “not missed something?!” | 明确有限研究范围，保留 `origin-coverage-review.md` 的原版逐节覆盖；不声称功能普适完备 | TEXT_REVISED; scope subject to advisor confirmation |

## PPTX Comment Traceability

| ID / 原页 | 老师关注点 | 论文／当前 slides 的处理 | 状态 |
| --- | --- | --- | --- |
| C01 / 3 | 通信事实不是问题 | 引言先解释应用决策；`Application Needs Determine the Research Scope` | TEXT_REVISED |
| C02 / 3 | “You have not shown this” | 去除普遍 IP/NDN 优劣推断；RQ3 的结论限于具体配置 | TEXT_REVISED; OPEN_RESEARCH |
| C03 / 3 | 需要例子 | 相机可达、可行、愿意与选定的区别；新增完整调用例子 | TEXT_REVISED |
| C04 / 4 | 对“NDNSF is the dissertation contribution”的问号 | `Research Contributions and Current Evidence` 列出待检验贡献，非以系统名称代替贡献 | TEXT_REVISED; exact intent subject to advisor confirmation |
| C05 / 4 | 如何确定框架主要功能 | 生命周期推导、需求表、既有调用/授权文献、UAV/DI 互补覆盖 | TEXT_REVISED; no exhaustive-function claim |
| C06 / 5 | trust anchor | Participants and Trust Bootstrap；组织批准权限、预配信任锚与 Controller material 验证 | TEXT_REVISED |
| C07 / 7 | Packet example 的目的不清 | 不恢复无目的 packet-format 图；命名例子解释三种职责，调用图解释四消息因果链 | TEXT_REVISED |
| C08 / 8 | 数据安全应紧随 NDN 动机 | NDN Primitives 页将命名、转发、签名／schema、Content 加密放在同一基础说明中 | TEXT_REVISED |
| C09 / 9 | “shared” | 内容加密与共享获取相容；候选共享可读范围≠指定接收者，缓存不获解密权限 | TEXT_REVISED |
| C10 / 10 | ABE 的目的 | Reason 1 非公开发现；Reason 2 权限管理复用；附加权限聚合理由 | TEXT_REVISED |
| C11 / 11 | 多个 Provider 的含义 | 首两页明确初版多候选选一，互补角色为扩展 | TEXT_REVISED |
| C12 / 12 | gap 与框架关系 | 需求→RQ→机制→应用覆盖→实验；不是只列 NDN 特点 | TEXT_REVISED |
| C13 / 12 | 读取内容的授权 | 加密管理 read access；服务权限、身份和重放检查分开；DNMP 是真实命令授权先例 | TEXT_REVISED |
| C14 / 13 | “Reliability of what?” | 使用截止时间内完成率、返回对象验证、终止/取消等具体结果，撤掉宽泛可靠性主张 | TEXT_REVISED; OPEN_RESEARCH |
| C15 / 13 | 功能清单是否完整 | 明确 bounded scope；四需求组、完整应用覆盖、排除项与 Origin 对照 | TEXT_REVISED; scope subject to advisor confirmation |
| C16 / 14 | contribution 含义不清 | 按 RQ 写预期贡献及现有证据，不用“完成系统”代替研究结论 | TEXT_REVISED; OPEN_RESEARCH |
| C17 / 15 | RQ 是否由机制倒推；mobility 是否已由 NDN 解决 | RQ 前补需求来源；移动性实验测覆盖/完成/时延，不声称发明移动性机制 | TEXT_REVISED; OPEN_RESEARCH |
| C18 / 17 | Define entities | User、Provider、Controller 是协议角色，同一设备可兼任 | TEXT_REVISED |
| C19 / 17 | 谁发现 Provider；Controller 如何知道实体 | 组织注册与服务权限先于任务；User 发现当前参与意愿；Controller 不决定本次候选与分工 | TEXT_REVISED |
| C20 / 17 | 先有整体流程图 | `Invocation Overview` 放在四消息与授权机制前；论文保留架构与调用图 | TEXT_REVISED |
| C21 / 18 | 字段含义不明 | 消息职责表与 RequestNonce/ProviderChallenge 独立说明；不在主 slides 罗列 wire fields | TEXT_REVISED |
| C22 / 18 | 用具体例子解释 | 新增 A positive ACK / B busy negative ACK / User selects A / A captures 的调用例子 | TEXT_REVISED |
| C23 / 18 | 机制含义/作用不明 | Request descriptor 与执行 input 分开；解释为何参与前要知道区域与期限 | TEXT_REVISED |
| C24 / 19 | 像 manual/spec 而非 dissertation | 主 slides 讲决策与目的；论文保留精简 API 例子解释职责，不照搬接口手册 | TEXT_REVISED |
| C25 / 20 | 谁在何时授权谁 | Controller 管理长期权限，双方检查当前服务权限，User Selection 指定执行者，Provider 执行前核验 | TEXT_REVISED |
| C26 / 21 | 为何用 tokens，NDN 能分发 schema | DNMP 具体方案与完整加密适配；protected-value checks、身份、重放分开；ABE-backed 为可评价设计选择 | TEXT_REVISED; OPEN_RESEARCH |

黄底标记的 application-layer、implementation details、endpoint repair、service lifecycle、contribution、distributed inference 均纳入上述范围检查；不把单纯高亮解释为要求删除。口头引用建议通过首次使用处的相邻可编辑标记落实：NDN、Sync/SVS、NAC、NAC-ABE、Trust Schema、DNMP、MiniNDN、PX4 SITL、Repo、ONNX 和 K/V 说明均有对应来源。

## Evidence Boundaries and Source Checks

- 英文全部正文模块与所有 slide 文本经过本轮人工阅读；生成的句子／主张导航不是自动真值认证。新增中文段落与英文语义同步。原版重要内容的依据为 `origin-coverage-review.md`；本轮保留其全部正文模块、章节及实验表，不恢复旧稿错误或过时功能表述。
- 当前 `ServiceUser.cpp` 的 Selection-free Targeted 拒绝 `SelectionGatedInputV1`，request-scoped confidentiality 使用 Selection 交付 key envelope；精确核对仍支持正文的条件性描述。CodeGraph 查询首先返回临时 staging 副本，canonical node 查询未给出有效结果，故使用 canonical 源文件的精确文本核对，不以索引命中当源码依据。
- [DNMP 原论文](https://named-data.net/wp-content/uploads/2019/10/kathleen.pdf) §3.2–3.3、Figure 8 支持 role signing、Network Key、schema 验证和有界 replay state 的描述；没有推断证书不能复用，也没有声称该文规定 NDNSF 的撤销协议。
- [CaSCON 作者机构页面](https://www.rit.edu/directory/mjkvcs-mohan-kumar) 与 [IEEE DOI](https://doi.org/10.1109/SMARTCOMP65954.2025.00061) 支持正式书目。IEEE 全文访问仍受限；没有据此填写未知机制为“不支持”。
- [Graduate School Calendar](https://www.memphis.edu/gradschool/news-events/calendar.php) 于本轮核对：Spring 2027 申请／博士 candidacy 截止 February 5，defended final ProQuest 上传截止 March 26。February defense 是内部目标，非已批准安排。
- 运行时实现和测试由并行开发继续推进。本轮没有新增 native、MiniNDN、UAV flight 或模型实验；不改变任何实验数字，不将局部测试升级为完整 qualification。

## Review Verdict and Remaining Work

写作与组织层面的主要问题已按上表落实，原稿重要范围已保留。作为 final-defense readiness，仍需 `OPEN_RESEARCH`：完整 CaSCON/工作流安全比较、同需求授权替代设计的成本实验、协作与恢复端到端证据，以及历史小模型诊断的原始模型／run provenance。新增章节不能替代这些证据。

Academic Research Suite 采用既有 LaTeX 的局部 patch＋覆盖校验，而非重写整稿或注入 Markdown anchors；NDN Slides Review 驱动术语、因果链、首次引用、密度、可编辑文字与逐页渲染检查。未声称 ARS 全流程审稿或导师批准。首轮新增流程页出现 24.76523 pt 的 overfull vbox，已改为紧凑的四消息图并通过后续构建；这是文档排版失败，不是协议实验结果。

最终构建、PPTX 可编辑性、页边界、数字保持及镜像一致性以配套验证 JSON 为准。LibreOffice round-trip 只验证该渲染器；Microsoft PowerPoint 和 Google Slides 的实际客户端未运行。下一步请导师确认研究范围与对照实验方案，再闭合上述研究证据，而不是继续通过措辞加大主张。

## Final Checks

- English 50 页、中文 38 页；slides 49 页、讲稿 10 页。四组双入口提取文本一致，摘要和关键词仍同页。
- 八份最终 LaTeX 日志没有 overfull、缺字或未定义引用；PDF 与 LibreOffice 回读逐页检查未发现文字越界或明显重叠。普通 slide 的最终 PDF 文本不超过 100 词，文献页豁免。
- PPTX：1,006/1,006 源文字 spans 恰好转换一次，692 个可编辑文本框；过滤后的图形背景不含可提取文字。49 份 notes 与49页一致；notes parser 2/2 测试通过。
- 原有七章标题与各语言七个正文模块保持，六个未改动模块逐字一致；评价、计划和结论尾部逐字保持，六张实验 slides 的内容逐字保持。
- 新句子／主张导航含 1,758 个单元，含双语、镜像摘要及图表标签，不等于 1,758 个独立科学命题均已获证实。
- NDN preflight 的剩余提示经人工分类：否定的 guarantee/prove、已限定的 matched 或计划中的比较、加密 recipient group 术语，以及参考文献密度。不存在据关键词告警而删去必要限制的情况。
- PPTX 初次导出被生成器的目录保护拒绝；改用获准的独立 `slides/build/advisor-review-20260914-r3` 目录，未绕过保护或清除他人的构建目录。第一次失败日志仍在本轮 raw 目录。
