# DNMP Authorization Throughline Review

## Scope and Claim Boundary

按用户要求，将 DNMP 作为 Trust-Schema-based 命令授权的具体先例贯穿当前
Proposal 与 slides，而非只在相关工作表中列出名字。保留初版“多候选选一”与
多角色协作扩展的区别。历史未加载章节不修改，产品源码和实验数据不修改。

## Source and Revision Map

原始依据：[Nichols, ACM ICN 2019](https://named-data.net/wp-content/uploads/2019/10/kathleen.pdf)，
Sections 2.4、3.2、3.3 和 Figure 8；DOI：10.1145/3357150.3357397。
本轮重新核对角色密钥选择、命令约束和接收侧验证，并补齐 bibliography DOI。
Trust Schema 的一般定义引用 [Yu et al., ACM ICN 2015](https://named-data.net/wp-content/uploads/2015/11/schematizing_trust_ndn.pdf)。

| Location | Revision / preserved boundary |
| --- | --- |
| EN/CH Introduction | NDN 提供命名获取和以数据为中心的安全机制；应用可以用 Trust Schema 表达验证规则，不暗示所有 NDN 应用必须使用它。NFN/NFaaS/RICE/NSC 的计算或调用先例与 DNMP 的命令授权先例分句说明。 |
| RQ1 explanation | 明确对照是适配 NDNSF 工作流、配合内容加密的 DNMP-inspired 方案，不是原版 DNMP 与完整 NDNSF 的不对等比较。 |
| Related work | 保留 DNMP 原有角色密钥、命令构造／验证、防重放说明；为 NDNSF 适配小节新增可引用编号。 |
| ABE rationale | 权限聚合和凭证复用段落回指同一个 DNMP-inspired 对照；保留角色可覆盖多权限、两类设计均有更新成本等限定。 |
| Evaluation / conclusion | RQ1 的表格和结论明确命名对照方案，仍将匹配成本实验列为待完成。 |
| Slides P4/P6 | 区分一般信任策略与 Trust Schema；明确 NSC 的应用 Schema 和 DNMP 的角色／测量命令授权。 |
| Slides P12/P14–16 | P12 将权限复用理由连接到 DNMP；既有 P14–16 的机制、完整对照和适用条件保留。 |
| Slides P32/P34 | 待完成评价与预期 RQ1 答案使用同一 DNMP-inspired 对照名称。 |

不在摘要、每一条通用信任规则、无关实验页反复加入 DNMP。它是具体命令授权
先例，不是通用服务编排系统，也不是已测量优劣的性能基线。Role credential 的
有效期和 publication 防重放不等于即时撤销；完整对照的加密和状态检查仍明确
标为本研究的适配要求。没有恢复“每服务必须新证书”等已否定的绝对断言。

## Review and Tool Boundaries

NDN Slides Review 用于先例分类、机制—性质区分、完整对照和渲染审查；ARS
用于原文核验和限定性修订。沿用 LaTeX 原生 apply_patch／Git diff，不向 TeX
注入 Markdown block anchors，未运行完整 ARS pipeline 或声称投稿认证。
Context Mode project health 通过；active-Spec 索引过期，改读 feature pointer、
Spec184 文件和 tasks.md，不把索引结果当最新状态。此轮无产品行为变更，故不
运行 Spec 实现流程、CodeGraph 源码行为审计、C++ 构建或模型实验。

首轮 P6 表格增加术语后出现 5.79 pt 的竖向溢出，已精简重复文字。中文引言
增加对照说明后产生过疏续页，已合并重复段落。早期 PDF／日志保留在原始目录。
首轮入口一致性检查发现 en/ch 的 `ref.bib` 是独立副本，缺少根目录新加的 DNMP
DOI；已同步这三处并重新构建。此失败只涉及文献元数据，不是产品协议失败。

## Validation

当前机器结果记录在 `research-revision-validation.json` 的
`dnmp_throughline_revision`，检查脚本为 `validate_dnmp_throughline.py`。
原始目录：`.codex-tmp/proposal-dnmp-throughline-20260911/`。
检查八个 LaTeX 入口、双语对应入口文本、全页文字边界、普通 slides 密度、
实验页数字不变、PPTX 文字分配和 notes、LibreOffice 回渲。逐页联系表及修改页
放大检查不等于安全证明；没有运行 Microsoft PowerPoint 或 Google Slides。

最终结果：英文 30 页、中文 23 页、slides 40 页、讲稿 8 页。八入口构建和
同语言文本一致性通过；无 Overfull、缺字或未定义引用，保留 320 条 Underfull
警告，不能称零 warning。普通 slides 最多 100 词，P38–40 参考页豁免。
PPTX 的 828/828 源文字片段恰好分配一次；574 个可编辑文本框、40 页 notes，
背景无可提取正文。PDF／LibreOffice 全页文字边界检查通过；讲稿解析 2/2
测试通过，六张历史实验页数字不变。1000 个主张导航单元更新源摘要；未改部分
沿用上轮语义审查，本轮重点核验 DNMP 相关增量，不声称重新验证所有产品行为。

下一步：将 DNMP-inspired 完整对照的配置和成本指标落到 RQ1 实验方案，才能
判断 ABE 权限复用何时减少总管理成本；本轮不启动这些实验或改变产品资格状态。
