# DNMP Comparison Review

## Scope

按用户要求补充 DNMP 作为 Trust-Schema-based 命令授权的具体实例，同步中英文
Proposal、共享 slides、讲稿与可编辑 PPTX。本轮使用 NDN Slides Review 的
mechanism/claim 与完整方案比较准则，以及 Academic Research Suite 的原文核验准则。
这是文献与文档修订，不重新核定产品功能或实验结论。

## Primary Evidence

引用 [Nichols, ACM ICN 2019, Sections 2.4, 3.2, 3.3 and Figure 8](https://named-data.net/wp-content/uploads/2019/10/kathleen.pdf)，
DOI [10.1145/3357150.3357397](https://doi.org/10.1145/3357150.3357397)。
Section 2.4 支持命名／签名层级及有效期约束；Section 3.3 支持从现有 role keys
选择允许的密钥、按 Schema 构造与验证命令；Section 3.2 支持 publication 生命周期
和近期状态防重放，前提为时钟大致同步。

直接下载返回 HTML，不能作为 PDF 解析；保留收到的原始字节，采用浏览器索引的
原论文文本核验，未反复重试。此工具边界见 `docs/failure-log.md`。
旧稿“DNMP 通过加密密钥轮换限制后续测量访问”在本次所核验的引用中未获支持，
已删除。该删除不等于声称所有 DNMP/VerSec 版本都没有加密或撤销机制。

## Claim and Page Ledger

| Location | Finding | Revision / evidence boundary |
|---|---|---|
| Related work | 已有 DNMP 名称与比较行，但缺少机制说明 | 新增 DNMP 与 NDNSF 适配两个小节，先说明先例，再比较。 |
| EN/CH mechanism | 有效签名易被误读成角色授权 | 明确已有角色密钥、Schema 允许的命令、接收侧验证及执行。 |
| Slide 14 | 缺少可直接讲解的 DNMP 实例 | 新增角色密钥选择、publication 验证和防重放说明，[12] 就近引用。 |
| Slide 15 | 原比较将 DNMP 压缩成抽象 roles | 改成 DNMP-inspired 与 NDNSF 直接对照，保留共同的 NDNSF 工作流。 |
| Slide 16 | 是否暗示 NDNSF 普遍更好 | 保留 DNMP 命令参数约束、稳定组加密、已知 Provider 的适用情形。 |
| Lifecycle paragraphs | 原文未支持的 DNMP 换钥归因 | 改成原文已述的有效期／防重放，与本比较提出的撤销状态检查分开。 |
| Comparison costs | 是否按证书数量断言成本优势 | 保留角色复用；ABE DKEY 更新和策略复杂度仍有成本，未宣称实测胜出。 |
| All other slides | 新插页是否改变实验数据／后文口径 | 原 1–13 页不移动；原 14–39 页移动至 15–40；实验数字独立核对。 |

## Fair Alternative

DNMP 原型不是 NDNSF 的 Request–ACK–Selection–Response 实现。本比较提出将其
角色约束签名思路适配到同一工作流：分别判断 User 调用权限与 Provider 参与权限，
检查活动请求和 Selection。选择前的 descriptor 仍需加密，选择后的输入与结果
仍需接收者范围保护。对照可采用服务组加密或 ABE；不能把不带加密的签名方案与
包含加密的 NDNSF 作安全性优劣比较。

NDNSF 的理由是复用属性权限管理与身份签名凭证，而非 ABE 独占多播、缓存保护、
防重放或撤销能力。当前权限粒度与成本限制保留。两类完整配置尚无匹配实测成本
结论；产品代码和历史实验值均未修改。

## Validation

当前机器检查、页数和文件摘要以 [validation](research-revision-validation.json)
的 `dnmp_comparison_revision` 为准。原始材料保留于
`.codex-tmp/proposal-dnmp-example-20260910/`。
检查范围包括六个正文／slides 入口、两份讲稿、普通 slide 密度、引用／缺字／越界、
同语言入口一致性、原实验数字、PPTX 可编辑文字分配及 LibreOffice 导出。
旧审计中的 39 页为历史范围，本轮增加 DNMP 机制页后为 40 页。
不把排版通过称为安全证明，也不声称在 PowerPoint 或 Google Slides 客户端实测。

最终结果：八个 LaTeX 入口通过；英文双入口各 29 页、中文各 22 页，slides 各
40 页，讲稿各 8 页；同类入口提取文本一致。最终日志无 Overfull、缺字或未定义
引用；Underfull 警告对应的表格已渲染审查。普通 slides 最多 100 词，参考文献页
豁免。除原比较页外，旧 slides 的主体文本逐页不变（排除页脚页码），因此六页
实验数据不变。PPTX 的 835/835 源文字片段恰好分配一次，569 个可编辑文本框，
背景可提取文字为 0，40 页 notes 齐全；LibreOffice 导出 40 页无文字越界。
讲稿转换单测 2/2 通过。所有最终页面完成 contact-sheet 检查，新增两页及正文
比较表放大检查；首次英文构建的两行孤页通过精简重复解释消除。

静态文字预检给出 16 项候选，已人工判断：P14 的 [12] 直接支持该 DNMP Schema
实例，标题中的 Trust-Schema 不另重复 [16]；其余为已限定的 evidence 词、后文
已展开的简称或参考文献密度。LibreOffice 文本提取的词切分不同，100 词检查以
LaTeX 最终 PDF 为准，不将不同分词结果误报为文字新增。

下一步：若要论证管理或运行开销优势，需实现同等保密与撤销要求的 DNMP-inspired
对照，并测量凭证／策略维护、挑战处理、传播与换钥成本；本轮不启动这些实验。
