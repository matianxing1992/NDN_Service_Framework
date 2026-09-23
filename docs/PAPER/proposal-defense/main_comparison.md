# Proposal Revision Comparison

## Format and Reading Guide

[main_comparison.pdf](main_comparison.pdf) 现在采用单栏“彩色标记修订稿”格式，便于连续阅读。
它不是干净论文，也不是逐条 rebuttal letter；它用于让导师在同一页上阅读当前正文并定位旧稿措辞。

- 第1页：当前 Proposal 封面。
- 第2页起：黑色为当前 Proposal 正文；深金色括号为 Origin 中的旧措辞或相关旧材料。
- 短小修改以内联旧词标记；大段重写用 `[Old; Origin PDF p. ...]` 注释，不强行制造逐词替换。

这种格式不使用左右面板、彩色背景或大表格，减少版式噪声；旧稿仍完整保留在定位注释中。

- 同主题配对是审阅定位，不等于逐句替换证明。
- 没有直接对应段落的旧文仍保留；它不表示该内容必然被删除或新增。
- 旧文中的引用编号、章节号和页码属于Origin；新版中的编号属于当前稿。
- 图表、表格和代码保留为原PDF片段；它们使用源页码定位，不做逐词颜色标记。
  需要检查图形细节时，应回到对应的 Origin 或当前 Proposal PDF。
- 独立公式保留原PDF矢量外观；普通段落由PDF文字重排，行内数学的斜体、上下标和
  重音位置不保证原样。精确数学排版以正式稿或左右对照的原PDF片段为准。

本文件是AI辅助重排的审阅副本，不是正式提交稿。干净的[main.pdf](main.pdf)
已同步当前 Proposal；加入对照内容后自然重新分页，不承诺与正式稿逐页相同。

## Baselines and Preserved Version

- 原稿：完整59页 `../reference-pdfs/Tianxing_Dissertation_Proposal_Origin.pdf`。
  SHA-256 `3e1bd2e9ae323ad919917d64c0d6a003646ddc27e7a4ca137c7e9618c9bc6277`。
- 新稿：72页 `main.pdf`。
  SHA-256 `240c39a2628cc7dfa7494845ad732123bbb6e1eb9be0b0092b2b6ddff2d41a13`。
- 老师批注依据为11页 `../reference-pdfs/proposal 2.pdf`，与完整Origin不是同一份PDF。
  具体意见的对应关系另见[批注审计](comment-change-audit-20260916.md)。
- 当前彩色单栏对照（102页）：[main_comparison.pdf](main_comparison.pdf)。
- 原132页主题左右对照审计副本：[main_comparison_topic_matrix.pdf](main_comparison_topic_matrix.pdf)。
- 旧的左右对照副本（103页）：[main_comparison_side_by_side.pdf](main_comparison_side_by_side.pdf)。
- 机器验收记录：[main_comparison-report.json](main_comparison-report.json)。

## Validation

2026-09-17 当前彩色单栏构建为102页：原稿349个单元、当前稿350个单元。
黑色当前文字的完整性、顺序、旧文金色覆盖和页面边界检查均通过；生成脚本报告为 `TEXT_AND_LAYOUT_PASS`。
这些是格式和覆盖验收，不是对新颖性、安全性或实验结论的科学认证。

当前 Proposal 从70页增至72页的原因属于正文后续修订；对照已重新读取当前72页 PDF，未沿用旧版70页哈希。

以下前轮审查仅为历史依据，不代表它们已在当前版本全部重新执行。

- 输出与检查报告：[main_comparison-report.json](main_comparison-report.json)。
- 前轮全文语言修订：[clarity-review-20260917.md](clarity-review-20260917.md)。
- 前轮成本模型精简：[cost-model-audit-20260917.md](cost-model-audit-20260917.md#latest-concise-revision)。
- 初次成本模型与三组配置：[cost-model-review-20260917.md](cost-model-review-20260917.md)。
- 前轮三项贡献修订：[contribution-revision-20260917.md](contribution-revision-20260917.md)。
- 前轮标准与必要性复审：[proposal-criteria-review-20260917.md](proposal-criteria-review-20260917.md)。
- 前轮评价计划同步：[tiger-evaluation-plan-20260916.md](tiger-evaluation-plan-20260916.md)。
- 前轮研究论证检查：[proposal-skill-review-20260916.md](proposal-skill-review-20260916.md)。
- 前轮语言检查：[language-review-20260916.md](language-review-20260916.md)。
- 前轮proposal口径检查：[proposal-register-review-20260916.md](proposal-register-review-20260916.md)。
- 前轮设计同步：[design-sync-20260916.md](design-sync-20260916.md)。
- 前轮论文与slides同步：[audit-sync-20260916.md](audit-sync-20260916.md)。
- 必要性分类与局部修订：[necessity-review-20260916.md](necessity-review-20260916.md)。
- 前轮需求追踪与风险修订：[comment-fixes-20260916.md](comment-fixes-20260916.md)。
- 初次格式转换记录：[inline-comparison-review-20260916.md](inline-comparison-review-20260916.md)。
- 生成源：[build_inline_comparison.py](build_inline_comparison.py)，使用审阅后的对齐清单生成。
- 当前彩色单栏源：[main_comparison.tex](main_comparison.tex)。
- 原主题对照由：[build_paragraph_comparison.py](build_paragraph_comparison.py) 生成；其 PDF 已保存在审计副本中。

## Reproduction

从仓库根运行，输出目录必须尚不存在；不得覆盖正式论文或已有失败目录。

```bash
alignment_dir=$(mktemp -d /tmp/ndnsf-paragraph-current-XXXXXX)
/usr/bin/python3 - "$alignment_dir" <<'PY'
import pathlib, sys
sys.path.insert(0, 'docs/PAPER/proposal-defense')
import build_paragraph_comparison as b
b.EXPECTED = [b.sha(p) for p in b.SOURCES]
b.build(pathlib.Path(sys.argv[1]))
PY
out_dir=/tmp/ndnsf-inline-current-$(date +%s%N)
/usr/bin/python3 docs/PAPER/proposal-defense/build_inline_comparison.py \
  --alignment "$alignment_dir/main_comparison-report.json" --out-dir "$out_dir"
```

依赖 PyMuPDF 和 XeLaTeX。两份源 PDF 变化后，必须重新生成对齐清单并运行
`validate_inline_comparison.py`；不能沿用旧的源哈希或把未通过的构建当作发布稿。
