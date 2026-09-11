# Invocation Foundation and Collaboration Extension

## Scope

用户指出初版 NDNSF 从多个候选中选择一个 Provider，而不支持多角色协作。
本轮据此调整中英文 Proposal 的摘要、Introduction、相关工作定位、协议表、
评价边界与结论，并同步 40 页 slides、讲稿及可编辑 PPTX。
不修改旧论文快照、保留的历史章节、产品协议、实验数字或实现验收状态。

## Argument Revision

| Location | Revised purpose / boundary |
| --- | --- |
| Abstract | 先说明服务需求和初版多候选选一基础，再说明本 Proposal 评价该基础并研究协作扩展；摘要与 keywords 保持一页。 |
| Introduction 1.1–1.2 | 从 NDN 服务发现与获准调用切入；明确单 Provider 执行同样需要发现、授权与保密性，不依赖协作才能成立。 |
| Introduction 1.3 | 多角色、互补服务和命名依赖是扩展需求；UAV 作为这一需求的例子，而非整个框架或初版能力的定义。 |
| Thesis / RQ1–RQ3 / Contribution | RQ1 评价调用基础，RQ2 研究协作扩展，RQ3 分别衡量两者收益／成本；保留已有 request ID 隔离。 |
| Related Work | role assignment 与 dependency enforcement 明确属于 collaboration extension；DNMP 比较与已有引用保留。 |
| Chapter 3 | 先写被评估的调用／授权设计，再写拟议扩展；后续保密性和生命周期改进不被倒填为所有初版路径。 |
| Four-message table | 基础表去掉 role／plan 接受条件；协作计划、依赖与恢复在后续扩展小节说明；主要检查仍非完整 wire-format 或充分谓词。 |
| Evaluation / Conclusion | 调用实验不验证多角色计划；早期 DI 诊断也不证明初版框架具有通用角色协作；数字与既有证据限制不改。 |
| Slides P2–P3 | P2 为候选 A/B/C 中选 B 执行的选择过程；P3 对照已有调用基础与协作扩展，不再以多 UAV 任务定义开场。 |
| Slides P5–P9, P19, P22–P23, P35 | 同步 RQ、贡献定位、User 职责和单 Provider 四消息；明确协作规划与恢复属于扩展；UAV 例子在扩展之后引入。 |

## Evidence and Tool Boundaries

这是按作者确认的初版范围和已有文档记录进行的叙事修订，不是重新定位 May-20
精确 Git tree 的取证任务。当前 CodeGraph 可检索现行 ServiceUser 及协作调用方，
不能用现行代码反推初版已有能力。正文已保留历史／现行／拟议设计的区别。

使用 NDN Slides Review 的全篇逻辑、版本边界和渲染检查，以及 ARS 的主张—证据
修订原则。沿用 LaTeX apply_patch／Git diff；未向 LaTeX 注入 Markdown anchors，
未运行完整 ARS pipeline 或对文稿作 ready-to-submit 认证。
Context Mode project health 通过，当前 Spec 状态以仓库 pointer／tasks 为准。

首轮补丁因段落尾部的精确上下文不匹配而整体拒绝，未发生部分修改；去掉该独立
hunk 后重试，并以精确段落替换完成剩余位置。第一轮摘要 keywords 溢至下一页、
Introduction 末页过疏，均在渲染审查中发现并通过删减重复说明修复。

默认提交 hook 在创建 commit 前拒绝全索引的历史开发助手引用。已核对 hook 及
Spec182 `integration-20260906.md` 中既定的 `NDNSF_LOCAL_CHECKPOINT=1` 模式，
本地 checkpoint 使用该模式并保留禁止路径检查；不修改 hook、不用 `--no-verify`、
不 push。默认检查的只读复现日志保存在原始目录 `checkpoint-hook-default.log`。

## Validation

本轮实际结果：英文 30 页、中文 23 页；slides 40 页、讲稿 8 页。
八入口构建无 Overfull／缺字／未定义引用，同语言入口提取文本相同。
保留 314 条 Underfull 排版警告，不声称零 warning。
普通内容页最多 100 词；参考页按参考文献用途豁免。
PPTX 826/826 source spans 分配一次，572 个可编辑文本框，40 页 notes；
PDF 与 LibreOffice 回渲均未检测到页面外文字，所有 contact sheets 已检查。
P28–P31、P36–P37 实验数值相对本轮开始前保持一致。
讲稿解析测试 2/2 通过。文本预检作为人工复核候选：P4 在不同分词规则下为
100／103 词；否定性的 prove／guarantee 不作为过度主张；DNMP 标题下的 [12]
支持其具体机制，Trust Schema 通用比较另附 [16]。这些不是未解决的协议错误。

实际检查见 [validation](research-revision-validation.json) 的
`invocation_scope_revision`；复查脚本为 [validate_invocation_scope.py](validate_invocation_scope.py)。
旧 `sentence_review_revision` 保留为历史记录，不代表本轮新执行的产品测试。
源句导航已按本轮源码重新生成；单元数量不是独立科学主张或正确性证明的数量。

原始构建／渲染记录：`.codex-tmp/proposal-invocation-scope-20260911/`。
渲染检查覆盖 PDF 与 LibreOffice 回渲；未运行 Microsoft PowerPoint／Google Slides 客户端。
不把 PDF 边界和文字检查等同于完整视觉或语义形式证明。

## Next Step

与导师确认 RQ1 的完整授权替代方案成本评价及 RQ2 的最小协作任务图；随后补齐
端到端反例／恢复证据及历史 DI provenance。进一步强化措辞不能代替这些研究证据。
