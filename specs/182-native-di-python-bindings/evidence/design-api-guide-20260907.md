# API Design Guide

## Work Unit D-DESIGN-API

用户要求四模块 Design 细化到 API 级别，并在 AGENTS.md 说明管理规则；后续指定参考 NFD Developer’s Guide。
范围：中文 API 行为契约、准确声明参考、双份 PDF、覆盖/验证、Spec 变更映射及本机 AGENTS 规则。
无产品源码修改，不改变 Spec182 功能验收门。

## Current Status — PASS

采用源码语法树清单加人工中文契约，覆盖 C++ public/protected、Python 公共命名定义/字段与显式导出；区分应用内部和测试 helper。不以扫描数量宣称运行资格。
NFD 参考的是官方维护 77 页 PDF，Revision 12 标为 TBD；仅采用组织方式。
AGENTS.md 已增加管理要求，可交付规范在 Design/MANAGEMENT.md；AGENTS 本来被本地排除，保持其现有跟踪策略。

## Retrieval and Tool Boundaries

Context Mode stats 已检查，project health PASS；active health rc=4（tasks.md source hash 过期），按仓库文件读取 active 状态，不重建或清空共享索引。
CodeGraph status 报告 up to date；先用 explore 定位当前 Core/Repo/DI/UAV 入口，再核对规范文件。
Python 环境没有 tree_sitter；使用已安装 CodeGraph 附带 web-tree-sitter 0.25.10 及 C++ wasm，不安装依赖或修改产品环境。Python 只做 AST 静态读取，不导入运行模块。
已读最新 publication fixture failure 及 recertification evidence；其第一边界是 fixture 的 // 名字，后续 r2 定向 PASS，不重跑原生构建。

## Validation Plan

完整 API PDF r1 位于 `.codex-tmp/design-pdf-20260908T034744723269Z/`，双份各 79 页并编译成功，但一段 options/objective/constraints/strategy 导致 38.46106pt 横向溢出；这是排版失败。对长标识符采用可断行 code，再建立独立 r2。

r2 `.codex-tmp/design-pdf-20260908T034944909852Z/`：双份 79 页，无警告/溢出/缺字；295 文件/16558 声明核对 PASS，350 文件基线还原 PASS。视觉检查发现按契约强制分页产生多处过短尾页，改为连续 API 排版并精简目录到章节级，建立 r3。

r3 `.codex-tmp/design-pdf-20260908T035230725421Z/`：连续排版 63 页，文字/字体/无溢出和源码检查 PASS；人工目录检查发现两遍编译仍显示前一遍页码，改为三遍编译，独立 r4 确认目录与章节实际页码。

首轮契约渲染在 ExecutionLease::release 选择器处失败；实际 owner 是 ProviderExecutionLeaseTable，已按头文件修正。语法清单生成成功（无解析错误），此为文档定位失败，不是产品构建失败。拆出独立渲染脚本，避免仅修改契约时重复抽取全部源码。

核对 API 解析与当前字节、选择器解析、签名/参数/模板完整性、四模块计数、当前/目标预期一致；检查双 PDF 编译、文字、字体、版面与可重建源码快照。保存精简持久证据。
完成后同步 tasks.md，仅暂存本单元明确文件和进度 hunk；原始日志/预览/源码压缩包不入 Git，不推送。

## Final Validation — PASS

Raw：`.codex-tmp/design-pdf-20260908T035534860722Z/`，双份各三遍 XeLaTeX rc=0。
两份各 63 页、58 章；逐章目录页码对应实际标题，字体嵌入，无警告/溢出/缺字；正文与 API 技术文本一致。
已检查章节目录、API 部分全部缩略图及放大的目录/长参数/源码位置；原有结构章节保留，API 改为连续排版。
295 文件/16558 声明（5738 函数条目）、23 契约/53 签名选择器核对 PASS。
另有两个 pybind11 源文件/830 项绑定操作，其字节身份与 Git 提交相同；8 项动态类名 helper 明确标示，不虚构导出 owner。
350 文件的 Git 基线加补丁还原 PASS；验证时无登记源码漂移。Python 文档脚本语法检查 PASS。
精简证据：[综合结果](../../../Design/evidence/r1-guide-verification.json)、[API 结果](../../../Design/evidence/r1-api-verification.json)。
AGENTS 本机规则已核对，对应可交付规则 Design/MANAGEMENT.md；未改变 AGENTS 的本地忽略策略。
本单元没有产品源码修改、原生构建或运行资格结论；后续按 Spec/API 变化维护文档，当前/目标不自动互相覆盖。
