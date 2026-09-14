# Dissertation Proposal Structure and Positioning Review

## Scope and Review Standard

本轮承接 `d0164b1c` 的逐句／批注修订，专查论文和slides的组织及叙述定位，不重新宣称完成全部文献、代码或实验验收。采用ARS的局部修订与论证核对方式，以及NDN Slides Review的结构、状态和渲染检查；原生LaTeX使用小范围patch，不执行Markdown全文重生成或ARS全流程认证。

目标是 **dissertation proposal**：提出值得研究的问题、解释已有工作与预期贡献、给出可执行的研究和评价方案，并让委员会判断范围及可行性。已有结果说明研究基础；尚未完成的实验作为明确计划，而非伪装成最终结论。Proposal也不能仅罗列未来功能：新颖性依据、对照设计、失败条件及范围仍需能接受委员会质询。

本系[PhD Program Information](https://www.memphis.edu/cs/pdfs/forms_grad_phd_programinfo.pdf)（2024-07-26版，2026-09-14在线核对，第2页）将proposal defense列为comprehensive examination，要求事先准备proposal，提前两周由导师批准并送委员会，进行口头报告及委员会提问。其第3页另列最终dissertation defense。该文件未规定统一的七章标题或唯一slide结构；下表是针对本研究的组织建议，不冒充学校强制模板，也不以final-defense评价表要求proposal已完成所有实验。

## Argument Structure

| 论文部分 | 要回答的问题 | Slides对应组织 |
|---|---|---|
| 1 Introduction | 谁遇到什么问题；为什么研究；RQ和预期贡献是什么 | P2–8：调用基础、协作扩展、NDN原语、需求、RQ、相关工作及已有基础 |
| 2 Related Work and Design Alternatives | 已有什么方法；真正待比较或待解决的差异是什么 | P7、P19–21：前例和完整授权替代方案，先解释再比较 |
| 3 Research Approach: Invocation and Collaboration | 如何研究；为何选择机制；哪些已有、哪些拟议 | P9–28：调用与授权基础、威胁假设、RQ2计划／依赖／恢复规则 |
| 4 Application Scenarios and Validation Plan | 用哪些工作负载检验框架，观察什么 | P29–35：UAV与DI的完整验证覆盖；协作仅为其中一类 |
| 5 Preliminary Evidence and Proposed Evaluation | 已有证据支持什么；每个RQ还需何种实验 | P36–41：基线、初步测量、RQ评价表；历史备份P45–46 |
| 6 Research Plan, Feasibility, and Risks | 剩余研究能否完成；哪些风险需缩减范围或调整时间 | P42：2026/2027明确年份的排期，February 2027为最终学位答辩目标 |
| 7 Expected Outcomes and Proposal Summary | 预期获得什么知识，当前请求委员会评估什么 | P43–44：预期答案、范围、评价标准和完成计划 |

Slides以口头论证组织，不逐章机械复制论文。技术细节保留，用于说明机制选择和实验可行性；不把API清单、原生测试通过数或功能数量当作博士贡献。

## Changes and Preserved Scope

1. 保留摘要已有的proposal口径，不把基础能力改写为全部待开发，也不将协作实效写成已有结论。
2. 将 `Contribution and Evidence Boundary` 改为 `Expected Contributions and Current Foundation`；新增短篇章导航，说明委员会需要评估的问题与研究方法。
3. 统一第3–7章的proposal标题，去除开发进度式 `Decision Gates`，将 `Conclusion` 改为预期成果与提案总结。中英文本同步。
4. 将slides贡献、应用、评价页标为Proposed／Planned，将四张结果页明确标为Preliminary Evidence／Diagnostic。它们的实验内容和数值保持不变。
5. 时间线明确February 2027是 **final dissertation defense**，不是本次proposal defense；结尾请求委员会评估意义、范围、验证标准和可行性。
6. 三个RQ、七章范围、两套应用的完整功能覆盖、16个中英文被包含模块、机制段落、实验数字及老师19条PDF／26条PPTX批注记录保持，不因改成proposal而删除技术内容或研究问题。

## Claim and Tense Rules

| 状态 | 使用方式 | 不应使用的推论 |
|---|---|---|
| 已有基础 | present tense描述已说明的协议；明确初版单Provider边界 | 实现存在不等于效果已验证 |
| 拟议方法 | proposed／under the proposed rules，或解释“研究将检验” | 设计中的接受规则不是完整系统证明 |
| 初步证据 | past/present描述已记录的限定结果及其条件 | 不扩展到未测协作、模型、硬件和故障配置 |
| 拟开展评价 | will evaluate／planned tests；明确对照、观测及成本 | 不预定一定优于替代方案 |
| 预期贡献 | intended／expected outcomes | 不把proposal总结写成研究问题已全部回答 |

## Validation and Remaining Decisions

当前机械检查见[validation](proposal-structure-validation-20260914.json)。检查章节／模块保留、RQ不变、六张实验／备份页正文不变、双入口一致、最终日志、可编辑PPTX和渲染。此报告评价组织和口径，不认证科学结论真值或委员会通过。

最终检查通过：英文50页、中文38页、slides49页、两份notes各10页；八入口最终LaTeX日志无overfull、缺字或未定义引用；1,006/1,006源文字spans保留为可编辑文字，49页PPTX及49页notes存在，notes parser 2/2通过。查看了英文／中文全稿contact sheets、49页LibreOffice回读contact sheets，并放大复查P42时间线；未见明显重叠或截断。未运行Microsoft PowerPoint或Google Slides客户端，也未重新验证产品行为或重跑实验。

导师批注逐项处置仍见[advisor review](advisor-review-20260914.md)。完整CaSCON／工作流相关工作比较、同需求授权方案成本、协作与恢复端到端证据、历史DI模型／run provenance仍待补齐。它们作为有明确方法的剩余研究符合proposal阶段；其范围与完成标准仍须委员会认可。下一步优先确认RQ2的可区别贡献与最小验证任务，而非继续增加可选功能。
