# Sentence-level Proposal and Slides Review

## Scope and Verdict

本轮逐句阅读英文／中文共享正文、四入口摘要、40 页 slides 和协议图文字，
检查命题、依据、位置、前后逻辑和证据范围；修改源文件后同步构建 PDF、讲稿和
可编辑 PPTX。实验与产品代码未修改，未重跑产品测试或实验。

采用 NDN Slides Review 的主体—操作—对象—条件检查，以及 ARS 的主张／证据
区分。维持原有研究结构，使用原生 LaTeX apply_patch 和 Git diff；未运行完整 ARS
pipeline，也未将 Markdown block markers 插入 LaTeX。

逐句导航见 [sentence/claim ledger](sentence-claim-ledger-20260911.json)，可由
[inventory script](review_sentence_claims.py) 重建。清单包含正文句子、表格行、标题、
图中标签和重复入口，自动分句不是语法完整性证明；证据类别用于导航，不代表
每句都获得独立实验验证。讲稿从最终 slides 生成，不作为第二份科学依据。

结论：文字与论证可作为修订后的 Proposal；**不能据此声称完整创新性比较、
协作安全证明、所有运行路径验收或历史实验原始来源已经闭环**。

## Findings and Revisions

| ID / location | 原问题 | 修订与判断 |
| --- | --- | --- |
| S01 / 双语摘要 | “NDN 分布式服务需要运行时发现”概括所有部署 | 限定为参与者变化、任务具有依赖的分布式应用，再引出 NDN/NDNSF。 |
| S02 / Introduction、P2–P5 | 把“不是两项贡献”等内部评审提醒置于主线 | 改成具体任务为何需要发现、授权、依赖检查；说明 UAV 与 DI 分别检验何种依赖。 |
| S03 / Contribution | 反复比较旧 v0.1、假想旧 attempt 缺陷、个人归属提醒 | 集中交代已有调用证据与拟议协作扩展；承认已有 request ID 隔离，不发明历史漏洞。个人贡献归属仍须作者最终确认，未虚构。 |
| S04 / Related Work | “不是 insecure straw-man”“不是首个”等防御性表达 | 正面描述已有机制，再明确本研究比较分配时机、对象使用与恢复语义；强 novelty 保持待证。 |
| S05 / Authorization、P11–P12 | 两条 ABE 理由都在重复密文共享分发 | 理由一为选择前描述保密；理由二为权限聚合、身份凭证独立及调用管理复用。注明 User DKEY/challenge 额外成本。 |
| S06 / DNMP、P14–P16 | 容易把 Trust Schema 无加密推成整个替代方案不可能 | 保留原文角色密钥选择与 NOD 验证；配套组加密／ABE、调用状态和撤销是 NDNSF 对照设计的补足，不倒填为 DNMP 原有功能。 |
| S07 / Protocol、P9–P10 | “Input After Selection”容易与 Selection 本身交付输入冲突 | 改成 Request 提供 descriptor、Selection 提供任务输入／引用及密钥；按用途而非字段名划界。 |
| S08 / Message table | 简略检查清单标题像完整充分接受条件 | 改为 Principal checks／主要检查，保留后文完整计划／状态约束。 |
| S09 / P13 | challenge response 与 Response Data 易混淆 | 用“User 在 Selection 返回该值，Provider 核对”明确消息与主体。 |
| S10 / P17 | “not a third advantage”像写作提醒 | 改为传播、拒绝生效和换钥的测量目标，保留离线撤销限制。 |
| S11 / Object admission、P19–P21 | 关闭窗口动作主体缺失；依赖检查未明确执行期限 | User 关闭窗口冻结已验证合格 positive ACK；接受依赖前检查活动执行身份和 deadline。保持拟议契约，不宣称所有路径已验收。 |
| S12 / DI、P26 | “解码需要保留 KV”可误读为缓存是生成的必要条件 | 改成“使用 KV caching 时”复用 K/V；可共享缓冲区，不等于每步重新复制。驻留权重不替代请求特定 KV。 |
| S13 / Current status、Limits | “本地测试不证明跨进程”不准确，本地也能多进程 | 改为“仅限单进程的测试”；内部 V3 术语改为实际验证边界。Spec184 局部通过不提升为完整资格。 |
| S14 / P28–P29、Evidence | 两页过渡易误以为同批母样本和子集 | 明确整体覆盖 controls 为 seeds 62–71，switching holdout 为 seeds 72–81。 |
| S15 / P29、Evidence | 成功条件化 p95 没有邻近呈现各系统失败量 | 增加既有记录导出的完成数：1,311／1,297／1,296，各分母 1,312；明确每系统只对自身成功请求取 seed-p95。原差值及 CI 不变。 |
| S16 / Work efficiency | “Selection avoids”超出观测给出单因素因果 | 改为相对该并行配置 observed fewer starts；不外推消息量、能量或相对 NSC 优势。 |
| S17 / DI diagnostic | 原始模型／运行来源尚未在当前审查重新建立 | 正文与 P31 显式标明 retained historical diagnostic、provenance 待复核；不得作为新设计的主证据。 |
| S18 / Timeline、P33–P34 | 只写最终提交遗漏早期申请；内部答辩提示代替结论 | 补 Spring 2027 Feb 5 申请／candidacy；保留 Mar 26 ProQuest 和提前两周送稿。P34 改为 Expected Research Outcomes。 |
| S19 / References | Trust Schema 用项目组织名代替论文作者，且 slides/body 引用版本不一致 | 统一至 Yu 等 ACM ICN 2015，DOI 10.1145/2810156.2810170；KV 复用加官方 ORT 文档，不采用该网页有问题的内存大小算例。 |
| S20 / English protocol figure | 浮动图表出现在所属小节标题之前；改为固定位置后继承正文双倍行距，产生图内重叠 | 固定相关图表位置并显式恢复图表单倍行距；重新渲染检查，不把无 Overfull 当成无重叠证明。 |
| S21 / P37 PPTX | LibreOffice 回渲中公式的 ell 字符变为反引号 | slides 改为易读文字说明逐链路 loss 不等于重试后的服务失败率；论文保留正常显示的公式及假设，历史实验数字不改。 |

## Evidence Boundaries

1. **NDN_PRIMARY_SEMANTICS**：NDN 架构、[Data packet specification](https://docs.named-data.net/NDN-packet-spec/current/data.html)、正文已有 Sync/SVS、NAC 原文。签名须与信任规则结合；不自动给出执行权限、语义正确性、持久存储或任务期限保证。
2. **DNMP_PRIMARY_OR_EXPLICIT_ADAPTATION**：[Nichols 2019](https://named-data.net/wp-content/uploads/2019/10/kathleen.pdf)，§2.4、§3.2–3.3。角色密钥和 Schema 调用约束、least-privileged matching、NOD 验证与 publication 时限属于原文；NDNSF 四消息流程适配与在线撤销状态属于本比较的设计要求。
3. **AUTHORIZATION_SOURCE_AND_ASSUMPTIONS**：`ServiceController.cpp::addAttributesForUsersAccordingToServicePolicy` 的 `/SERVICE`、`/PERMISSION` 和 OR policy；`revoke`、`rotateAbeGenerationAndReissuePolicies` 的当前状态／参数轮换。`ServiceUser.cpp`、`ServiceProvider.cpp` 的 challenge／签名／请求状态检查；相关 controller-revocation-flow 测试及历史定向证据，不构成本轮新 PASS。增强 [NAC-ABE](https://arxiv.org/abs/2311.07299) 支持 KP-ABE。密钥独占与恶意分享不由 challenge 值的返回证明。
4. **RECORDED_EXPERIMENT_SCOPED**：本轮读取 Spec171 `opportunity-holdout-results-20260809/holdout-summary.json`，核对 seeds、各系统成功数、seed-p95 差值和区间；读取 `results/four_provider_work_efficiency_confirmatory_20260806/combined-six-seed-aggregate.json` 及两个 `spec171-burnin300-*-seeds62-71-20260808/aggregate.json`。保留原数字，仅新增现存记录导出的成功数和条件说明。不重做统计方法独立验证，不声称每条原始 packet log 均被复算。
5. **HISTORICAL_PROVENANCE_OPEN**：539/600 loss 最佳运行、小模型 259.4/186.8 ms 等历史值保持原貌；当前未重新建立完整模型、运行与统计来源。须完成 provenance 后才能作为 final-defense 主证据。
6. **MODEL_SEMANTICS_AND_PROPOSED_DI**：ONNX IR、既有模型切分参考；[ORT past/present buffers](https://onnxruntime.ai/docs/genai/howto/past-present-share-buffer.html) 仅支持 buffer reuse/copy 行为。特定 NDNSF-DI 性能与恢复仍取决于实际模型／切分验收。官方网页的内存计算例子未使用。
7. **PROPOSAL_DESIGN_SCOPE_OR_RESEARCH_METHOD**：请求方可信、授权状态传播、角色／边验证、重分配与取消是明示假设或拟议契约；不是已完成安全定理。UAV 为用户确认的地图地面区域案例，不冒充实飞或精度测量。单进程与多进程验证区分据当前 Spec184 T007 的 PARTIAL 记录及 I01 r6 原始日志核对；不把局部 selector PASS 当总验收。
8. **SCHEDULE_SOURCE_AND_INTERNAL_TARGET**：[CS 指南](https://www.memphis.edu/cs/pdfs/forms_grad_phd_programinfo.pdf) 与 [Graduate School](https://www.memphis.edu/gradschool/current_students/graduation.php) 于本轮重新核验。February defense 是内部目标，非学校强制日期；申请和提交日期须提交前复核。
9. **LITERATURE_COMPARISON_OPEN**：CaSCON 仅有可确认的出版线索，不据此判定缺少某项机制。对服务组合、workflow 和 capability 系统的完整逐特性比较尚未完成；不声称全领域 first 或重大突破。

## Document Validation and Limits

最终实际构建、逐页渲染、双入口、PPTX 可编辑文字与 LibreOffice 回渲结果记录于
[validation](research-revision-validation.json) 的 `sentence_review_revision`。
不得把此处的文档检查解释成重新运行的协议测试。

工具边界：首次补丁因中文行上下文未匹配而未应用，核对原文后重试；首次文档构建
启动因 outdir 的相对 cwd 错误在 LaTeX 启动前失败，改用绝对路径。均未触及产品运行。
PPTX 最后一轮的 `final-build` 目录名也被转换器安全检查拒绝，发生于清理／生成前；
改用符合已有契约的 `ndnsf-final-build` 目录，不放宽保护规则。原日志保留为
`pptx-build-final.log`，重试日志为 `pptx-build-final-r2.log`。
原始构建与截图目录：`.codex-tmp/proposal-sentence-review-20260911/`。

下一步：补齐完整授权替代方案成本实验、协作反例／恢复端到端证据，以及历史 DI
原始模型／运行来源；这些是研究证据工作，不应靠进一步强化措辞代替。
