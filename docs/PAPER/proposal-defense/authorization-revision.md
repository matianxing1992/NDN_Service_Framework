# Proposal Authorization Revision

## Scope

按用户确认的邮件口径修改 Proposal 中英文安全论证及对应演示文稿。
仅改文档，不改变 Core/DI/UAV API，不重跑协议或集群实验。
既有未完成 Spec182 验收状态保持不变。

## Current Checkpoint

PASS (documentation only)：完成中英文正文、长/短版 slides、讲稿及长版可编辑 PPTX。
本记录不关闭 Spec182 实现或资格验收任务，不把历史定向测试当成本轮重测。

## Delivered Changes

- 根目录 `main.pdf` / `main_ch.pdf` 新增 3.7.1 授权选择理由和 3.7.2 运行时撤销。
  中英文使用独立共享 snippet，同步至 `en/`、`ch/` 分章入口；本轮不统一两种入口既有的其他章节差异。
- `slides/main.pdf` 共 64 页：P20 保密发现，P21 challenge 验证，P22 三方案凭证比较，
  新 P23 撤销及成本，P24 发现描述和应用输入边界；P59 比较 ABE 加角色证书方案。
- `slides/main_35min.pdf` 保持 44 页，更新授权摘要及相应备份页；两份讲稿同步。
- `slides/NDNSF_proposal_hybrid_editable.pptx` 重新生成 64 页并附 64 页讲稿。
- 补充增强 NAC-ABE / KP-ABE 原文引用；不再用普通 NAC 仓库引用代替 KP-ABE 证据。

## Claim Boundaries

保密对象是缺少相应解密能力的观察者；获准但未被选中的 Provider 可读取发现描述。
内容加密不隐藏外层名称或流量模式。Selection 限定输入的描述明确针对 request-scoped 路径。
KP-ABE 聚合限定于同一授权机构和参数代，不声称任意策略固定大小或固定成本。
DNMP 的角色可以涵盖多项权限；每服务角色证书为本文对照设计，并非所有 Trust Schema 的要求。
DNMP 测量数据密钥轮换与命令权限撤销分开表述；证书撤销要求验证方检查状态，不是物理收回证书。
NDNSF Controller-wide ABE 轮换可能要求其他仍获授权成员刷新 DKEY；不宣称全网瞬时撤销或更低总成本。

## Editing Boundary

首次 apply_patch 在修改文件之前因同一 patch 重复指定 main.tex 被拒绝。
边界为 patch 结构验证，不是 LaTeX 构建或协议失败；重试将同一文件的 hunks 合并。
原始操作输出：`apply_patch verification failed: invalid patch: multiple operations target .../main.tex`。

第二次因同一文件 hunks 未按源码先后排序而未找到前文锚点，仍未写入正文。
已重新读取原文确认锚点存在，重试将早期安全概述修改放在后续插入操作之前。

后续 slides 限定语补丁再次遇到同类 hunk 顺序错误；该次未修改 slides，
此前已完成的 notes 更新保留。以下重试严格按源码位置排序。

## Evidence Sources

- DNMP: https://doi.org/10.1145/3357150.3357397, Sections 2.2 and 3.3.
- KP-ABE extension: https://arxiv.org/abs/2311.07299.
- Current source: `ndn-service-framework/ServiceController.cpp`, OR-policy aggregation and Controller-wide ABE rotation.
- Historical focused tests: [runtime revocation lifecycle](../../../specs/179-request-scoped-confidentiality/evidence/runtime-revocation-lifecycle-20260904.md).

## Validation

通过 `latexmk -norc -pdf/-xelatex -bibtex -interaction=nonstopmode -halt-on-error`
在独立目录构建：根英文 63 页，根中文 54 页，分章英文 64 页，长版 slides 64 页，
短版 slides 44 页，讲稿 15/10 页。最终日志均无未定义引用及 LaTeX error。
正文、短版及讲稿无 Overfull；长版保留非新增页的 3 条既有 vbox 警告
（0.109/1.327/8.319 pt），全 64 页 contact sheet 检查未见内容越出页面。
新增正文 PDF33--35/中文29--30 与授权 slides 重点检查，未发现文字重叠。

PPTX 生成检查：1944/1944 原始文本 spans 恰好分配一次，去文字背景包含 0 个可提取字符，
1151 个可编辑文本框；LibreOffice 重导出 64 页，P20--24 截图复核无缺失或重叠。
这不是对 Google Slides 或 Microsoft PowerPoint 原生客户端渲染的实测。

文字审查工具报告 62 个启发式提示，包括既有页面及标题重复触发的引用提示；
新增 P20/P22/P23 的 102/102/101 词密度提示经渲染检查接受，不表示全稿所有提示已关闭。
旧 P30--35、P49、P55--57 对应的新页，剔除页脚后数字 token 逐项一致；未改变实验数字。

原始构建、PPTX 生成及审查日志、截图与旧交付物备份持久保存在
`.codex-tmp/proposal-authorization-20260908-docs/`（不入 Git）。
本目录内 Markdown/TeX/PDF/PPTX 为文档交付；不包含原始运行日志、密钥或模型。

首次正文 latexmk 虽 exit0，但载入既有 build/ 辅助文件并留下新增文献 undefined，
因此不计为通过。原始日志保留在 `/tmp/ndnsf-authorization-docs-VhY0xT/proposal-en-build.log`
和 `proposal-zh-build.log`；重试使用 `-norc`、独立输出目录和显式 BibTeX。
Slides 初次编译 exit0，无未定义引用；存在非新增页的既有 vbox 警告，需渲染核对。

PPTX 首次生成被脚本的临时目录保护拒绝：末级目录必须以 `ndnsf-` 开头。
日志 `/tmp/ndnsf-authorization-docs-VhY0xT/pptx-build.log` 保留；未覆盖交付文件。
重试使用新的 `ndnsf-pptx-build-r2` 末级目录，不绕过安全检查。
