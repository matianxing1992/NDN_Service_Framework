# Proposal Origin Coverage Restoration

## Scope

用户要求参考完整 Origin Proposal，补齐当前双语论文遗漏的重要内容。本工作只修改学术文档，不运行产品实验，不改变 T007/T008 验收状态。

## Attempts

- 首次文档构建驱动失败：临时目录由相对 `__file__` 派生，传给切换工作目录后的 latexmk，未在预期位置产生 `main.log`。首次边界是构建脚本路径解析，不是 LaTeX 正文或协议失败。原记录保留在 `.codex-tmp/proposal-origin-expansion-20260911/root-en/driver.log` 及同级三个入口目录。
- 修复方案：使用绝对临时路径，并用新的 `r2/` 目录保留重试产物。最终结果和文档覆盖记录在完成检查后追加。

## Authority Boundary

Context Mode active health 报告索引中的 tasks.md hash 过期；采用仓库 tasks.md 和当前源文件，不从索引推断实现完成。目标协作及完整模型资格保持原有边界。

## Final Document Exit

- `DOCUMENT_CHECKS_PASS`：英文 root/en 两入口均 49 页，中文 root/ch 两入口均 38 页；两种语言的镜像分别文本一致。
- 四次 latexmk 构建无 Overfull、undefined reference/citation、missing character；14 个新增双语模块全部且只被包含一次，新增引用键均可解析。PDF 文本无越过页面边界。
- 逐页 contact sheets 和新增架构／DI 图、API／UAV 表的细图检查；DI 框间距扩大，使箭头可辨。中文覆盖表固定位置，避免被单独漂移到整张浮动页。
- 对比修订前主文，去除新增 input 并归一化一个中文 table placement 后 byte 相同：已有 RQ、实验数值、授权段落及时间表未改。Bibliography 修正 CFEC 第一作者，三入口内容一致。
- 持久检查：[validation](../../../docs/PAPER/proposal-defense/origin-expansion-validation.json)；逐节去向与来源：[coverage review](../../../docs/PAPER/proposal-defense/origin-coverage-review.md)。原始驱动、构建和截图留在 `.codex-tmp/proposal-origin-expansion-20260911/`。
- 本轮仅论文文档；无产品编译或实验，slides 不变。不声称近邻工作 novelty、所有模型和协作恢复或 final-defense 资格已闭环。
