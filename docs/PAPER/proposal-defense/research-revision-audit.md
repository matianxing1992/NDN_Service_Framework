# Research-first Proposal Revision and Evidence Audit

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
| C13 | TEXT-ADDRESSED: decryption, authentication, execution authorization and request-state checks separated; three complete alternatives compared. |
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
