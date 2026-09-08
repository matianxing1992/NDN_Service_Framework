# Design PDF Baseline

## Work Unit D-DESIGN-R0

用户要求在仓库根 `Design/` 保存当前设计与目标设计 PDF；初始技术正文一致。
本单元范围为 NDNSF Core、NDNSF-UAV、NDNSF-DI、NDNSF-Repo 全部设计领域。
Read：当前架构、Controller/User/Provider 源码及版本/刷新 owner。
Write：`Design/`、本证据、tasks.md 的独立附加文档行，必要的 failure index。
Steps：逐项源码核对，建立可编辑双份正文和源码摘要，编译并检查 PDF。
Verify：正文一致、源码 SHA-256、双份 PDF 页数/文本/字体/布局、git diff --check。
不执行协议测试、不改变 runtime、不调整 Spec182 功能验收门。

## Current Status — PASS

用户要求 NDNSF Core、UAV、DI、Repo 全部中文设计，后续明确改为 Design 完整纳入 Git。
四模块完整双份中文 PDF 已交付，各 35 页；技术正文一致。新增 Spec 设计追踪记录，同步 PDF 后创建本地文档 checkpoint。
不代表 Spec182 功能或运行资格验收。验证记录见
[Design validation](../../../Design/validation.md)，后续维护见 [Design README](../../../Design/README.md)。

## Attempt r1 — PARTIAL

Raw：`.codex-tmp/design-pdf-20260908T020519732600Z/`。
两份 XeLaTeX 编译 rc=0，但布局检查发现第 7 页长测试符号造成两个 Overfull hbox
（211.92155pt、66.07983pt）；这是 PDF 排版失败，不是协议或原生构建失败。
修复方向：code inline 使用允许断行的 xurl/nolinkurl，再创建新目录重建。
当前不计文档完成，不计运行资格验收。

## Attempt r2 — PASS

Raw：`.codex-tmp/design-pdf-20260908T020637509900Z/`。
改 code inline 为 xurl/nolinkurl 断行；两份 PDF 各两遍 XeLaTeX rc=0，7 页 A4，
无 Overfull/Missing character/Error。正文 TeX 字节相同；PDF 提取文本仅页眉身份不同。
12 个源码摘要与磁盘及基线提交均匹配，字体全嵌入；逐页检查 current 7 页、target 首页面貌。
git diff --check PASS。未执行 runtime 测试，无功能任务由本单元关闭。

## Attempt r3 — FAIL

Raw：`.codex-tmp/design-pdf-20260908T022959579009Z/`。
完整中文版在 Repo 章节裸下划线触发 `Missing $ inserted`，XeLaTeX rc=1，
未替换已生成 PDF。此为文档语法失败；修复标识符转义、描述列表和中文强调后独立重建。

## Attempt r4 — PARTIAL

Raw：`.codex-tmp/design-pdf-20260908T023513717283Z/`。
双份完整 PDF 各 35 页，编译成功；长 abort 标识符有 24.15834pt 横向溢出。
改为可断行 code 并设置中文等宽字体后独立重建。r3 语法错误已消除。

## Attempt r5 — PASS

Raw：[final run](../../../.codex-tmp/design-pdf-20260908T023607184577Z/)。
双份各两遍 XeLaTeX rc=0；各 35 页，无警告/溢出/缺字，字体全部嵌入。
TeX 技术正文逐字节相同；PDF 去身份页眉后文本一致。检查全部 current 页面缩略图、
放大制品接口/源码索引页及 target 首页，无裁切或重叠。
94 文件存档哈希通过，验证时无源码漂移；606 文件模块范围另有清单。
期间三个 DI cpp 文件的 canonical graph 身份变化已核对并刷新字节快照；
源基线是 d7fa9c8edef924a6cf29c12d3793049aa794a6be 加 manifest 标记的工作树内容。
机器结果见 [verification](../../../.codex-tmp/design-pdf-20260908T023607184577Z/verification.json)。
git diff --check PASS；Design 未跟踪且本地排除。本单元仅文档验收，不关闭功能任务。

## Git Tracking Revision — PASS

用户后续授权 Design 完整纳入 Git；新增 spec-design-changes.md，记录 Spec、前后设计、章节、实现状态、源码提交与证据；历史映射未核对部分明确保留 PARTIAL。
同步两份 PDF 的版本维护说明；`.codex-tmp/design-pdf-20260908T024454038889Z/`
双份各两遍 XeLaTeX rc=0、35 页、正文一致、无警告/溢出/缺字且字体嵌入。
精简结果见 [tracked verification](../../../Design/evidence/r0-git-verification.json)。
源码基线可用 Git 提交加 Design/evidence/source-baseline-worktree.patch 还原并检查 94 文件摘要。
只提交 Design、本证据和 tasks/failure index 的文档单元内容；并行 Spec182 源码及其进度改动不纳入本提交。

## Git Patch Whitespace Check

首次 staged diff --check rc=2：源码补丁第 29 行是 unified diff 的空白上下文行，被外层文档 diff 识别为行末空格；未提交。
改用零上下文补丁及 git apply --unidiff-zero；重验 94 文件源码还原、PDF 摘要和 staged diff 均 PASS。此为文档补丁格式检查，不是产品测试失败。

## Retrieval Diagnostics

Context Mode project health PASS；active health rc=4，原因是 tasks.md 文件哈希过期。
使用仓库文件读取 active Spec 进度；project lane 仅作架构检索。
query guard 首两次因缺 require、以及 query 不包含 require 被拒绝；补齐精确标识后
project query PASS。未用 timeline auto-memory 当当前任务 authority，未修改共享索引。
CodeGraph 返回 compare-old/compare-new 同名副本时拒绝这些副本，按 canonical
文件行号和磁盘内容核对。未将临时副本作为设计基线。
