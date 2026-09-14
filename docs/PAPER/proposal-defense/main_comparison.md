# Proposal PDF Comparison

## Deliverable and Baseline

[main_comparison.pdf](main_comparison.pdf) 现为79页左右对照版：左侧完整Origin原稿，右侧当前稿；第1页为可点击的19主题索引。每个源页面按1:1尺寸嵌入，不加红蓝标记，保留原字号、图表和排版。

- 原稿：`../reference-pdfs/Tianxing_Dissertation_Proposal_Origin.pdf`，59页，SHA-256 `3e1bd2e9ae323ad919917d64c0d6a003646ddc27e7a4ca137c7e9618c9bc6277`。
- 当前稿：`main.pdf`，50页，文档checkpoint `3b3f29f5`，SHA-256 `f3b6d952ed723fa0b81b6b0600434f0c37d5230a92b2b5c60a13c6f3ca90fcae`。
- 不采用11页的老师批注稿 `proposal 2.pdf`，也不复用旧 `revision-comparison/` 的另一版源码快照。此前指定的完整Origin稿作为本次基准；本次未改写任何论文或slides正文。

## Reading and Limits

按摘要、研究问题、NDN背景、相关工作、消息流程、授权、协作、UAV、DI、评价及时间线等19个主题组织。每侧标注源PDF页码；横向宽页允许放大阅读，不把两张原页面压缩到一张Letter纸上。首页索引和主题书签均可跳转。

使用ARS格式转换的内容保留边界：直接嵌入源PDF的矢量页面，不重新排版正文。对照按主题分组，组内按各自源页顺序阅读，不声称逐段／逐句严格对应。一个页面可能涉及多个主题，故允许重复并明确标注。某侧没有更多页面时留白并说明；不能据此认定该版缺失整个主题。

全部59页原稿和50页当前稿均被包含，没有删除图表、参考文献或正文。引用保持原样，本次不重新验证文献或科学结论。嵌入页内的原PDF超链接不复制；本对照版提供新的主题导航。

## Validation

最终r4：79页、130个完整页面面板（包含跨主题重复），源59/50页全覆盖；各面板文字精确相同、源文件哈希不变，单位缩放。光栅差异检查显式允许微小字形边缘／一级灰度取整差异，不声明逐像素完全一致；每个非零差异及容差记录在 [main_comparison-report.json](main_comparison-report.json)。全部79页渲染、首页索引／书签、图表与页眉边界经检查。

本轮构建／截图位于 `.codex-tmp/proposal-side-by-side-20260914/r4/`。前三轮排版或像素门禁边界保留，见[Spec证据](../../../specs/185-prepared-model-runtime/evidence/proposal-side-by-side-20260914.md)。旧110页标记版保留在本地提交 `ab9ee6f7` 及 `.codex-tmp/proposal-pdf-comparison-20260914/r3/`，不再作为推荐阅读副本。仅DOCUMENT_PASS，不产生产品或实验资格。

## Reproduction

需要Python、PyMuPDF和Pillow；从仓库根运行。脚本验证两份冻结源的哈希，换基准必须先复核主题映射。选择新输出目录，脚本拒绝覆盖既有对比稿。

```bash
python3 docs/PAPER/proposal-defense/build_side_by_side.py \
  --original docs/PAPER/reference-pdfs/Tianxing_Dissertation_Proposal_Origin.pdf \
  --current docs/PAPER/proposal-defense/main.pdf \
  --out-dir .codex-tmp/proposal-side-by-side-rebuild
```

本机PyMuPDF环境可使用 `PYTHONPATH=.codex-tmp/proposal-research-revision-20260909/pydeps /usr/bin/python3`。下一步使用说明页主题索引按章节审阅；本阅读副本不替代干净提交稿。
