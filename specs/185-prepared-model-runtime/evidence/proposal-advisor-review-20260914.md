# Proposal Advisor Review — Documentation Checkpoint

## Scope

按用户要求审查并修改 Proposal 与对应 slides。使用原生 LaTeX 局部 patch，保留重要原版内容及旧实验数字；不是 Spec185 native implementation batch，不改 API、源码设计、运行时资格或任务勾选。与并行 Core/DI 开发独立。

## Evidence

- [批注逐项处理与研究边界](../../../docs/PAPER/proposal-defense/advisor-review-20260914.md)：19 条 PDF 与26条 PPTX 批注。
- [验证记录](../../../docs/PAPER/proposal-defense/advisor-review-validation-20260914.json)：英文50页、中文38页、slides49页、notes10页，双入口文本一致。
- [句子／主张导航](../../../docs/PAPER/proposal-defense/sentence-claim-ledger-20260914.json)：1,758单元，人工审查导航，不是真值认证。
- [可重复检查脚本](../../../docs/PAPER/proposal-defense/validate_advisor_review.py)：模块、章节、六张实验页保持；八份LaTeX日志、页边界、PPTX编辑文字与notes一致性。
- Raw：`.codex-tmp/proposal-advisor-review-20260914/`；PPTX intermediate：`docs/PAPER/proposal-defense/slides/build/advisor-review-20260914-r3/`。

## Observed Results

`DOCUMENT_CHECKS_PASS_RESEARCH_GAPS_REMAIN`。1,006/1,006 PDF文字spans恰好转换一次，692可编辑文本框，49页LibreOffice回读检查；notes parser 2/2 PASS。没有新实验、native build或产品测试。老师原件只读提取，不覆盖。

## Tool and Failure Boundaries

Context Mode project health PASS；CodeGraph查找先返回临时源码副本，canonical node查询未返回有效内容，改以生产文件精确文本核对。ARS用于局部修订和证据边界，NDN Slides Review用于用语、顺序、引用与渲染审查；未运行ARS全流水线。

新增流程页首轮overfull 24.76523pt，改成紧凑流程图后通过；PPTX首轮被build-directory guard拒绝，使用独立获准目录后通过。失败均为文档工具边界，不归为协议结果。PPTX失败日志保留为`pptx-build.log`，后续成功为`pptx-build-r3.log`；最初排版日志曾被增量构建替换，其首错按工具输出在人工报告记录，最终r3/r4日志独立保留，不声称首轮原日志仍完整存在。

## Remaining Work

## Local Checkpoint Boundary

首次普通 commit 被 `.git/hooks/pre-commit` 的全索引开发工具引用检查拒绝，HEAD 未改变。该 hook 明确提供 `NDNSF_LOCAL_CHECKPOINT=1`，用于本地 checkpoint 跳过正文引用扫描，同时保留禁止路径检查；按此模式提交，不禁用 hook、不推送远端。本轮索引只含32个文档路径，tasks／failure-log 仅含本轮局部记录，并行源码及进度改动未暂存。

## Remaining Research Work

完整授权替代方案成本、CaSCON／工作流安全特性比较、协作及恢复端到端证据、历史DI模型/run provenance仍未闭合。文档完成不意味着final-defense readiness；下一步导师确认范围，再推进相应研究验收。
