# Research-first Proposal Revision and Evidence Audit

## 2026-09-10 Abstract Narrative Revision

按用户确认的逻辑，将中英文摘要改为“运行时服务需求 → NDNSF 研究方向 →
现有授权设计与拟议协作机制 → 初步依据与计划评估 → 预期研究结论”。首句先定义
NDN 并说明研究需求，不再直接罗列 Proposal 的功能。英文 235 词；删除摘要中的
毕业时间及内部验收术语，但保留已有研究不能证明拟议协作有效性的必要边界。
Abstract 对应当前提交的研究方案，不被限定为历史代码快照，也不代表全部计划
已经实现。Academic Research Suite 的摘要准则用于结构和中英文语义对齐。

仅修改四个正文入口的 abstract 内容并重建对应 PDF；其余章节、slides、讲稿和
实验数字不变。四入口构建通过，最终日志无 Overfull、缺字或未定义引用；英文
双入口各 29 页、中文双入口各 21 页，同语言逐页提取文本一致。与修改前比较，
仅 PDF P2 改变。两种语言摘要页截图均检查，无越界、重叠或跨页，正文分页未变。
原始构建和截图：`.codex-tmp/proposal-abstract-20260910/`；摘要检查单列于
`research-revision-validation.json`，不把上一轮 PPTX 检查视为本轮重跑。
未运行产品实验或重新核定当前实现状态，不改变 Spec182 的验收结论。

## 2026-09-10 Two-design Comparison Revision

按用户要求删除自定义的逐服务角色凭证替代方案，正文、表格和讲稿仅保留
DNMP-inspired 授权加配套加密与 NDNSF ABE-backed 的比较。同步中英文共享正文、
slides 和 review 指引；保留 DNMP 的真实角色授权说明，以及配套加密和生命周期
机制属于公平对照要求、并非 DNMP 原文已实现 NDNSF 工作流的边界。

首轮六个 LaTeX 入口构建通过。PPTX 转换首轮在 `pdftohtml` 输入解析处失败：
生成器相对于自身 slides 目录解释路径，仓库相对路径因此被重复拼接。
失败日志保留在 `.codex-tmp/proposal-two-designs-20260910/pptx-build.log`。
改用绝对 PDF／notes 路径及新的私有构建目录后，PPTX 转换通过，原始成功日志为
同目录 `pptx-retry.log`。此为文档工具输入边界，没有运行或判定任何产品协议测试。

最终检查 PASS：英文双入口各 29 页、中文双入口各 21 页、slides 双入口各 38 页、
讲稿双入口各 7 页；每对入口提取文本一致。PDF、PPTX 页面与全部 38 页 notes 中
均无已删除方案的名称或三方案标题残留。与本轮修改前 slides 逐页比较，仅 P13
文字改变，实验页及数字未改动。最终 LaTeX 日志无 Overfull、缺字或未定义引用。

按照 NDN Slides Review 检查所有正文、slides、讲稿 contact sheets，并放大检查
比较页；PPTX 经 LibreOffice 重导出的 38 页也完成截图检查，未见内容缺失、越界
或重叠。生成器检查 791/791 源 spans 恰好分配一次，546 个可编辑文本框、
47 个原生三角项目符号，背景可提取文字为 0。不声称在 PowerPoint 或 Google Slides
原生客户端完成实测。当前摘要见 `research-revision-validation.json`；原始构建与
截图证据位于 `.codex-tmp/proposal-two-designs-20260910/`。

Context Mode active-Spec health 因 plan/tasks 索引摘要过期未通过，使用仓库文件
作为权威来源；Context Mode 仅用于辅助文本检查。没有修改实现、目标设计或实验
结论，Spec182 的产品与研究验收门保持原状态。

首次本地 commit 被既有全索引助手引用扫描拒绝；原始诊断保存于
`.codex-tmp/proposal-two-designs-20260910/checkpoint-hook.log`。检查钩子及已有失败
记录后，使用其显式支持的 `NDNSF_LOCAL_CHECKPOINT=1` 本地入口，保留禁止路径
检查，不修改钩子、不自动 push。仅提交本轮明确文档路径，不纳入既有 DI 合约修改。

## 2026-09-09 Authorization Argument Follow-up

按用户确认改成“安全需求 → 架构收益”两条理由，再说明机制、生命周期与成本。
更新共享中英文授权正文、相关工作摘要及共享 slides，保持原有研究问题和实验数字。
签名验证／执行授权与内容解密继续区分；Trust Schema 与 NAC 的一手来源补入共享书目。
第 11、12 页连续介绍两条理由，第 13 页比较完整替代方案，第 15 页说明生命周期。
属性策略聚合不等于信息压缩，不将服务权限示例泛化为已实现的多维动态属性检查。

首个补丁因缺少原文中的 `In` 而未匹配，未修改文件；修正精确上下文后通过。
首次渲染在导入 `fitz` 时失败，边界为文档工具依赖，尚未生成渲染结论。
独立临时环境的 pip 安装遇到 DNS 解析失败，停止该安装；随后找到上一轮保留的
`.codex-tmp/proposal-research-revision-20260909/pydeps`。其中扩展为 CPython 3.8，
Python 3.10 探测不能加载 lxml；使用系统 Python 3.8 加命令局部 PYTHONPATH
导入 PyMuPDF/Pillow/python-pptx 成功，再重试渲染，不修改产品 Python 环境。
完整构建与渲染记录位于
`.codex-tmp/proposal-two-reasons-20260909/`；最新验证 JSON 替代旧文件摘要。
下文保留前次研究重组的历史审计，不把当时的页数／检查自动套用到本次修改。

本轮最终文档检查 PASS：根／分章英文均 29 页，中文均 21 页，同语言入口逐页
提取文本一致；两个 slides 入口均 38 页且文本一致；两份讲稿均 7 页，含 38 页
讲述内容。所有最终 LaTeX 日志无未定义引用、缺字或 Overfull。与修改前 PDF
逐页比较，仅 slides P11/P12/P13/P15 文本改变，实验页及其数字保持原样。
LaTeX PDF 的普通内容页均低于 100 词；参考文献页 P36/P37 不适用此限。
PPTX 导出后的分词会受页脚和断词影响，不能将其页数／词数机械等同于源 PDF。

逐页 contact sheet 检查中英文正文、全部 slides 和讲稿，未见越界或重叠。
PPTX 生成器验证 797/797 源文本 spans 恰好分配一次，551 个可编辑文本框、
47 个原生三角项目符号，背景可提取文字为 0；38 页均有 notes。
LibreOffice 重导出 38 页，逐页检查并复核 P11--15，未见缺失或重叠，文本均在页内。
未宣称在 Microsoft PowerPoint／Google Slides 原生客户端实测。
研究和实现资格门保持 OPEN；本轮没有重测运行时撤销或其他产品行为。

本地 checkpoint 的首次独立暂存被既有 `docs/PAPER` ignore 规则拒绝。
核对所有指定路径均已存在于 HEAD 后，仅对这些明确文档路径使用 `git add -f`；
共享 tasks.md 只纳入本轮新增的文档旁路记录，不混入其他会话的任务行。
原始记录：`.codex-tmp/proposal-two-reasons-20260909/checkpoint.log`。

Date: 2026-09-09. Scope: Proposal EN/CH, research slides, editable PPTX, and presenter notes. No application implementation or experiment was run for this revision.

## Decision and Source Authority

本轮按 `review.md` 重建主叙事，而不是给原有功能清单加研究问题标签。英文语义母版为 `en/chapters/research-revision.tex`；中文对应稿为 `ch/chapters/research-revision.tex`；两个语言各自的根入口与子目录入口加载同一正文。授权段落继续共享各语言的 `authorization-rationale.tex`。Slides 以 `slides/research-slides.tex` 为正文；`slides/main.tex` 和 `main_35min.tex` 使用同一精简内容。旧 `01-...07-...` 章节不再被入口加载，作为历史材料保留；不能以其旧 RQ 判断当前稿。

用户授权的是学术文档修改。没有修改 NDNSF 协议、设计目标或运行验收，不因文档完成关闭 Spec182 的 native/runtime gates。英文压缩并重组旧教程/API内容；中文是语义对应稿而非逐字长度相同的译文。历史实验未改数值。原教师批注 PPTX 未修改。

## Claim Classes and Audit Rule

- **SOURCE**：关于 NDN、相关框架或当前实现的事实，指向一手文献、规范或具体源码。
- **DESIGN**：本 Proposal 的协议、规则、威胁假设和实验设计，用 proposed/required/conditional 区分未验收目标。
- **MEASURED-BOUNDED**：对应指定实验记录；不能推广为所有当前代码、配置或应用的结论。
- **INFERENCE**：由条件与机制推导的理由，不能替代性能/管理成本实测。
- **OPEN**：需要补文献、原始证据、实验或导师确认的事项，不标为已证明。

逐段及逐页审查按以上类别进行；程序检查仅负责遗漏引用、溢出、页数、文本覆盖与源一致性，不是“每句永真”的自动认证。未完成全部最近工作逐特性排查，因此明确保留 novelty 边界，而不是宣称全领域 first。ARS 的修订/证据原则用于本次 LaTeX 修订；Markdown 行锚补丁格式不适用于该输入，采用原生 apply_patch、Git diff 和源码摘要替代，不宣称执行了未运行的完整 ARS pipeline。

## Claim-to-Evidence Ledger

| Claim group | Location / class | Source and bounded interpretation |
| --- | --- | --- |
| Named Data, signatures, Interest retrieval, caching | Introduction; slides NDN overview / SOURCE | NDN architecture paper and current Data packet specification: https://docs.named-data.net/NDN-packet-spec/current/data.html . Authentication requires trust validation; no automatic freshness, execution permission, persistent storage or deadline guarantee. |
| Sync/SVS | Introduction, message model / SOURCE | Li et al. MILCOM 2018; Moll et al. NDN Sync survey and SVS report. Announces dataset/name state; applications retrieve named Data. No unsolicited Data-push claim. |
| Existing service frameworks | Related Work / SOURCE | NFN, NFaaS, RICE and NSC original references retained. NSC primary report: https://named-data.net/wp-content/uploads/2021/05/ndn-tr-0074-1-nsc.pdf . Signed invocation and application trust rules are acknowledged. |
| DNMP role authorization | Related Work, shared authorization rationale / SOURCE + DESIGN | Nichols, ICN 2019: https://named-data.net/wp-content/uploads/2019/10/kathleen.pdf . Role/command signing is reported fact. The proposed status/expiry-based command revocation alternative is explicitly our comparison design, not attributed to DNMP as implemented online withdrawal. |
| gRPC discovery | Related Work, baseline/transition / SOURCE | https://grpc.io/docs/guides/custom-name-resolution/ . Fixed-list test limitations do not imply the framework cannot integrate dynamic discovery. |
| KP-ABE behavior | Authorization / SOURCE | Enhanced NAC-ABE: https://arxiv.org/abs/2311.07299 . Policy belongs in DKEY, attributes on ciphertext; no claim that original NAC-ABE always used KP-ABE. |
| Service entitlements and withdrawal | Shared authorization rationale / SOURCE, runtime boundary only | `ndn-service-framework/ServiceController.cpp`: `grant`, `revoke`, `rotateAbeGenerationAndReissuePolicies`; current revoke calls authorization-epoch advancement and ABE rotation. `/SERVICE/<service>` and `/PERMISSION/<service>` are kept distinct. Revoke is Controller-wide rotation; granting another service is not asserted to rotate every member's key. |
| Selected input/result protection | Protocol and selected-exchange slide / DESIGN | Request-scoped symmetric Content encryption with authenticated recipient key delivery; precise User + selected Provider secrecy assumption. Not backfilled to all v0.1 paths. No assertion that the producer cannot read its own result. |
| Plan, role and dependency admission | Protocol, UAV/DI / DESIGN | Proposed contract checks same-request wrong role/edge, not an invented request-ID defect. Local commitment assumes trusted User; signatures alone do not prove global uniqueness. Separate execution-time admission, segmented-object validation, cancellation, restart and replacement obligations. |
| UAV task | Introduction and application chapter / DESIGN | Derived from user's ground-area multi-view case. GPS is insufficient for visibility/depth/correspondence. Original producer namespaces and image reuse retained. Recognition/localization improvement not claimed without ground truth. |
| DI and KV state | Application chapter / DESIGN + model/runtime semantics | Partitioning distinguished from framework increment; ONNX IR and distributed-DNN references retained. Weight residency does not eliminate next-step KV/state dependencies. Specific tensor/hybrid/LLM performance remains OPEN. |
| Current native qualification | Application/status sections / SOURCE | `specs/182-native-di-python-bindings/tasks.md`; R7-B2 local replacement evidence and R6-B9 freshness evidence. No conversion of a local PASS into cross-process/multi-device qualification. |
| Mobility overall controls | Overall-completion slide / MEASURED-BOUNDED | Exact `results/spec171-burnin300-50m-2ms-seeds62-71-20260808/aggregate.json` and `results/spec171-burnin300-100-150m-2ms-seeds62-71-20260808/aggregate.json`. Ten seeds per system/condition; table is the mean per-seed success percentage, rounded to two decimals. Obsolete three-seed image was caught and removed during rendering review. |
| Conditional switching result | Evidence chapter and switching slide / MEASURED-BOUNDED | `specs/171-four-provider-mobility-advantage/evidence/opportunity-holdout-results-20260809/holdout-summary.json`; seeds 72–81, 1,312 requests. Rechecked rounded mean seed-p95 differences and intervals; intervals concern NDNSF minus baseline, not a positive reduction variable. Preserve separate overall controls and preregistration. |
| Work efficiency | Evidence chapter/slide / MEASURED-BOUNDED | `results/four_provider_work_efficiency_confirmatory_20260806/combined-six-seed-aggregate.json`; seeds 20–25. Rechecked 1,798/1,800 versus 1,800/1,800 and 1,800 versus 7,180 starts. Corresponding Spec171 evidence README also reports NSC 1.002 starts/request; the revision explicitly includes this and makes no superiority claim over NSC. |
| Provider transition | Evidence chapter/backup / MEASURED-BOUNDED | `specs/171-four-provider-mobility-advantage/evidence/provider-transition-results-20260809/transition-summary.json` and boundary-audit/registration. 357-request test is a discovery-configuration observation, not inability of other frameworks. |
| Historical loss and DI diagnostic | Evidence chapter/backups / recorded historical evidence | Values preserved from pre-revision Proposal source. The loss result 539/600 is explicitly a best recorded run. The ten-successful-run DI diagnostic is not a new campaign; raw model/run provenance has NOT been re-established in this revision. No causal or current V3 qualification claim is made. These need provenance closure before final-defense use as principal evidence. |
| Academic schedule | Plan chapter and timeline / SOURCE + conditional target | CS official program information: https://www.memphis.edu/cs/pdfs/forms_grad_phd_programinfo.pdf . Advisor-approved complete draft to committee two weeks before defense. Graduate School https://www.memphis.edu/gradschool/current_students/graduation.php lists March 26, 2027 for defended final ProQuest submission. February defense is internal conditional target; university instructions require recheck before filing. |
| CaSCON novelty boundary | Related Work / OPEN | Publication existence identified by primary RIT institutional record; not used as evidence that CaSCON lacks a particular mechanism. Full paper feature-level comparison, plus broader workflow/capability related work, remains necessary before strong novelty wording. |

## Teacher Comments: Disposition

`TEXT-ADDRESSED` means this revision changes the argument, not that the committee has approved it or experiments are complete.

| IDs | Disposition in the revised artifacts |
| --- | --- |
| C01, C02 | TEXT-ADDRESSED: remove host/channel-as-defect reasoning and universal NDN superiority. Start with task-dependent costs and complete alternatives. |
| C03 | TEXT-ADDRESSED: one ground-location multi-view task introduces actors, runtime uncertainty and downstream processing. |
| C04 | TEXT-ADDRESSED within its ambiguous wording: replace system-name-as-contribution with three bounded research outcomes. |
| C05, C15 | TEXT-ADDRESSED: explicitly managed trust domain and selected workload scope; no claim to cover every service-framework function. |
| C06 | TEXT-ADDRESSED: provisioned trust anchors, identity validation and Controller material are explicit prerequisites. |
| C07, C08 | TEXT-ADDRESSED: remove the disconnected telemetry-name tutorial; immediately connect NDN mechanisms to remaining task requirements. |
| C09 | TEXT-ADDRESSED: Sync describes dataset/name knowledge, not shared memory or identical execution state. |
| C10 | TEXT-ADDRESSED: discovery confidentiality requirement precedes ABE/key mechanics. |
| C11 | TEXT-ADDRESSED: distinguish candidate alternatives from assigned roles; positive ACK does not execute the task. |
| C12 | PARTIAL: related work acknowledges existing invocation/authorization/composition; exhaustive nearest-work feature comparison remains OPEN. |
| C13 | TEXT-ADDRESSED: decryption, authentication, execution authorization and request-state checks separated; DNMP-inspired authorization plus encryption compared with NDNSF ABE-backed authorization. |
| C14 | TEXT-ADDRESSED: completion by deadline, successful-response latency, execution starts, object retrieval and recognition accuracy are not conflated. Remaining campaigns must implement the stated metric definitions. |
| C16, C17 | TEXT-ADDRESSED: one scenario derives the RQs; UAV/DI are validation workloads. RQ wording does not prescribe ABE or ACK as a foregone answer. Advisor confirmation of narrowed scope remains required. |
| C18, C19 | TEXT-ADDRESSED: protocol roles may share hardware; organization enrollment differs from User runtime discovery; trust bootstrap is stated. |
| C20, C21, C23 | TEXT-ADDRESSED: participant overview, four named message types, shared protocol figure, and explicit Sync/Interest retrieval replace API details. |
| C22 | PARTIAL: the single example is carried through collection, processing and image reuse, with positive/negative ACK semantics. A measured complete multi-UAV trace is not claimed. |
| C24 | TEXT-ADDRESSED: remove the main-deck API/configuration inventory; keep mechanism choice, alternatives, costs and evidence. |
| C25 | TEXT-ADDRESSED: organization grants service entitlement; Provider states willingness; User selects invocation roles. Selection does not create missing long-term service permission. |
| C26 | TEXT-ADDRESSED: challenges reuse service decryption capability in addition to signatures/state; extra User DKEY and processing costs are acknowledged. Schema-plus-encryption remains valid. |

## Final-Defense Findings: Disposition

| IDs | Action / remaining obligation |
| --- | --- |
| FD01–FD05 | Argument reorganized around task, RQs, bounded contributions and related alternatives. Originality is a research obligation, not certified by rewriting. |
| FD06–FD08 | Explicit threat assumptions, conditional invariants and complete comparison requirements. No fabricated security proof or equivalence result. |
| FD09–FD12 | Seed/run units, success conditioning, full controls, provenance and cost separation added. Existing limited data preserved. Historical raw DI provenance, final scaling and full alternative campaigns remain open. |
| FD13, FD14 | Scope restricted to one UAV graph and one supported DI partition; optional features need their own evidence. |
| FD15 | Shared semantic sources replace divergent build-entry text. Alternate-entry rendered text is checked, including the previously inconsistent Chinese cover wording. |
| FD16 | Proposal tense preserved; final-defense gate requires actual RQ answers, not future-work slides relabeled as results. |
| FD17–FD20 | Published CS/Graduate School standards inform evidence, author contribution and readiness distinctions. They do not certify committee acceptance or completion of every catalog requirement. |

## Remaining Research Work (Not Closed by This Revision)

1. Advisor confirmation of contribution/RQ scope; explicit personal/coauthor ownership record.
2. Full nearest-work feature comparison, particularly collaboration/security mechanisms and complete alternatives.
3. One qualified UAV and one qualified DI execution with role/edge, direct/cache, cancellation/replacement and failure observations.
4. Matched authorization lifecycle and cost comparison; scale only dimensions needed by final claims.
5. Original provenance for historical DI/loss figures before using them as principal final evidence.
6. Replace each planned RQ answer with a measured bounded answer before scheduling final defense.

## Validation Record

Builds, page/text checks, final hashes and editable-text coverage are recorded in `research-revision-validation.json` after final rendering. Visual review checks all pages, then rechecks changed pages. Raw work is retained in `.codex-tmp/proposal-research-revision-20260909/`; PPTX intermediate export uses a dedicated `/tmp/ndnsf-proposal-revision-*/ndnsf-build` directory. No old raw experiment directory was changed.

Tools: Context Mode health passed at start; maintained repository and exact evidence paths remained authoritative. CodeGraph supplied real source locations but also returned `.codex-tmp` copies, which were rejected as authority. Missing renderer dependency was installed only in task-local `pydeps`, not into system Python. Bibliography lookup, exporter ownership guards and notes-parser failures were resolved at their first boundary and recorded in `docs/failure-log.md`. The notes parser's escaped-percent handling received focused regression checks so success-rate text is not silently truncated.

NDN Slides Review 文本预检的逐项处置：NAC 首次引用已改为紧跟 `NAC [4]`；KP-ABE 警告是将原版 CP-ABE 与 enhanced library 混淆的规则候选，本文明确引用增强版 [19]。`first`/`guarantee` 的命中多为否定声明，保留。`matched` 的两项实测有精确汇总来源，其余是未来比较要求或历史非匹配声明。receiver 是加密接收集合，不是错误的 NDN 目的主机。admission 通过具体角色／边检查及执行前资源检查定义。引用页允许高于普通页的约 100-word 目标；普通页最长仅略超 100（按含标题的简单切词计数），经渲染保留，没有以缩小字体强塞内容。

LibreOffice 回读发现 RQ2/RQ3 被 PDF-to-PPTX 段落分组合并；改用三条独立 itemize 项并重新导出。论文流程图脚注与节点曾过近，调整坐标后重查。以上问题只有修正并复查后才算排版完成。
