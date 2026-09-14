# Proposal PDF Comparison

## Scope

仅制作完整原版 `Tianxing_Dissertation_Proposal_Origin.pdf`（59页）与当前英文proposal（50页）的PDF文字差异阅读副本。不是11页老师批注稿，也不采用旧 `revision-comparison/` 中另一份57页源码快照。不得改变正文、slides、模型或实验结果。

## First Boundary

首轮 `.codex-tmp/proposal-pdf-comparison-20260914/r1/` 生成的说明页意外排成两页，导致说明中预写的页码比实际偏移一页；111页候选未晋升。正文109页的文字、页面尺寸与嵌入图片数量均通过保留检查。已添加说明页必须为一页的门禁，缩减说明页段距与边距后另开 `r2/`，不覆盖首轮产物。原始证据为 `r1/guide-build.log`、`r1/comparison-guide.pdf`、`r1/comparison-build.log` 与 `r1/comparison-report.json`。这是文档布局错误，不是协议结果。

## Current Checkpoint

DOCUMENT_PASS：r2修复说明页分页；r3增加跨章节连续12词相同片段恢复后，110页构建／渲染通过。109页源文字逐页精确相同、页面尺寸及嵌入图片数量保持、文字越界0页、两份输入PDF哈希不变；6项词匹配回归通过。已查看全部10张contact sheets并放大说明页及混合图表页。比较页2开始当前稿、52开始原稿，目录和说明页一致。

交付 [comparison PDF](../../../docs/PAPER/proposal-defense/main_comparison.pdf)、[方法与限制](../../../docs/PAPER/proposal-defense/main_comparison.md)、[逐页报告](../../../docs/PAPER/proposal-defense/main_comparison-report.json)。最终原始证据目录为 `.codex-tmp/proposal-pdf-comparison-20260914/r3/`，包含guide/build日志、PDF、JSON与逐页PNG；前两轮完整保留。仅增加文字标记，不改变干净论文、slides或科学主张；Spec185 native任务勾选保持原状态。下一步按主题页码对照人工审阅文字和图形差异。
