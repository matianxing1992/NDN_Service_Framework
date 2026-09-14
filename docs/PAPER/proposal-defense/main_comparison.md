# Proposal PDF Comparison

## Deliverable and Baseline

[main_comparison.pdf](main_comparison.pdf) 为110页的文字修订阅读副本：第1页说明与主题页码对照，第2–51页当前稿，第52–110页完整原稿。

- 原稿：`../reference-pdfs/Tianxing_Dissertation_Proposal_Origin.pdf`，59页，SHA-256 `3e1bd2e9ae323ad919917d64c0d6a003646ddc27e7a4ca137c7e9618c9bc6277`。
- 当前稿：`main.pdf`，50页，文档checkpoint `3b3f29f5`，SHA-256 `f3b6d952ed723fa0b81b6b0600434f0c37d5230a92b2b5c60a13c6f3ca90fcae`。
- 不采用11页的老师批注稿 `proposal 2.pdf`，也不复用旧 `revision-comparison/` 的另一版源码快照。此前指定的完整Origin稿作为本次基准；本次未改写任何论文或slides正文。

## Reading and Limits

当前稿蓝色底纹／下划线表示新增或改写的文字；原稿红色底纹／删除线表示文字匹配中被删除或替换的文字。两版章节书签完整保留，页面顶部注明源PDF页码。

使用ARS格式转换的内容保留边界：直接复制原PDF页面，再添加矢量标记，不重新排版正文。算法为精确词序比较，并恢复跨章节移动的至少12词连续相同片段；它不是语义审查。剩余移动、改写、引文编号及抽取差异仍可显示为删加，不能据此认定研究内容被丢弃。两版正文已经大幅改写，标记较密集，不将标记词数当作语义变更比例。

封面、目录、页脚页码不参加文字匹配；图表保持原位置。可抽取的图中文字可能被标记，但矢量图形和图片内部变化没有自动比较。引用保持原样，本次不重新验证文献或科学结论。

## Validation

最终r3：110页渲染；109页源文字逐页精确保留、页面尺寸和嵌入图片数量一致，原始PDF哈希不变，文字越界0页；6项词匹配回归通过。查看全部10张contact sheets，放大检查说明页及文字／图表混合页。目录当前起始页2、原稿起始页52。完整定位报告见 [main_comparison-report.json](main_comparison-report.json)。

原始构建与截图保留于 `.codex-tmp/proposal-pdf-comparison-20260914/r3/`（仓库根相对路径）；r1说明页分页失败亦保留，详见Spec证据。仅DOCUMENT_PASS，不产生产品或实验资格。

## Reproduction

需要 `pdflatex`、Python和PyMuPDF；以下路径从仓库根执行。选择一个不存在的输出文件，脚本拒绝覆盖既有对比稿。此说明页对应上述50／59页冻结基准；换基准必须同步说明页和页码表。

```bash
mkdir -p .codex-tmp/proposal-comparison-rebuild
pdflatex -interaction=nonstopmode -halt-on-error \
  -output-directory=.codex-tmp/proposal-comparison-rebuild \
  docs/PAPER/proposal-defense/comparison-guide.tex
python3 docs/PAPER/proposal-defense/build_pdf_comparison.py \
  --original docs/PAPER/reference-pdfs/Tianxing_Dissertation_Proposal_Origin.pdf \
  --current docs/PAPER/proposal-defense/main.pdf \
  --guide .codex-tmp/proposal-comparison-rebuild/comparison-guide.pdf \
  --output .codex-tmp/proposal-comparison-rebuild/main_comparison.pdf \
  --report .codex-tmp/proposal-comparison-rebuild/comparison-report.json
```

本机PyMuPDF环境可使用 `PYTHONPATH=.codex-tmp/proposal-research-revision-20260909/pydeps /usr/bin/python3`。下一步使用说明页主题索引按章节审阅；本阅读副本不替代干净提交稿。
