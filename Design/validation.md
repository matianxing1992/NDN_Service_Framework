# 文档验证记录

## R1 API 开发者指南

两份中文 PDF 各 63 页、58 章，包含四模块结构说明和 23 组 API 契约、53 个准确签名示例。当前/目标技术正文相同，两套可编辑契约独立保存。

最终构建目录：`.codex-tmp/design-pdf-20260908T035534860722Z/`。各执行三遍 XeLaTeX，目录分页稳定；无警告、缺字或溢出，字体全部嵌入。
机器逐章核对 58 个目录条目的页码与实际标题；正文与 API 部分字节一致，PDF 去身份页眉后文字相同。
检查目录和 API 部分全部页面缩略图，放大检查长签名、密集参数与源码定位；原先的短尾页已改为连续排版。

## API 与源码身份

- [API 检查结果](evidence/r1-api-verification.json)：295 个规范文件、16558 个声明条目，其中 5738 个函数条目；无记录的解析错误，签名与源码及契约 ID 核对通过。
- pybind11 映射另有两个源文件、830 项操作（含属性/枚举）；文件摘要与登记 Git 提交一致。8 项动态类名 helper 明确保留人工定位边界。
- [源码基线](source-baseline.json)：UTC 2026-09-08 03:47:37 采样，基线 e9fe33994a6ca3ff81893591bd24c3fae43f933f 加已登记工作树补丁，覆盖 350 文件及全部 295 个 API 清单来源。
- Git 提交加补丁还原全部 350 文件并通过 SHA-256 核对，无需本机压缩包。基线是明确时刻的快照，后续并行修改不会自动获得审查。
- [综合验证](evidence/r1-guide-verification.json) 保存 PDF 哈希、页数、目录、字体、文本、API 与源码还原结果。

## 管理规则与证据边界

本机 AGENTS.md 已增加 API/设计同步规则，可随 Git 交付的规范在 MANAGEMENT.md。AGENTS.md 本来被本地 exclude 排除，保持该跟踪策略。
Design 的 PDF、正文、契约、声明参考、脚本和精简证据入 Git；原始日志、预览与源码压缩包不提交、不推送。
本轮未回溯全部旧 Spec；没有 runtime 修改、协议测试、原生构建、模型性能或硬件资格结论。

## 失败、工具与历史

错误的 ExecutionLease owner 已修正为 ProviderExecutionLeaseTable；长标识符溢出改用可断行 code；目录增加第三遍编译并核对页码。
各次独立目录及第一失败边界见 [Spec 证据](../specs/182-native-di-python-bindings/evidence/design-api-guide-20260907.md)。
Context Mode project health PASS、active health rc=4（tasks.md 哈希过期），改用仓库和 CodeGraph。Python tree_sitter 不可用，复用 CodeGraph 的 web-tree-sitter/C++ wasm，未修改产品依赖。
R0 的 35 页设计与 94 文件快照在提交 9c019a17，旧验证为 evidence/r0-git-verification.json；当前 source-baseline 已推进到 R1。
后续按 README.md 和 MANAGEMENT.md 同步当前/目标、Spec 与 API。
## R2 Baseline and Contracts

当前 PDF 66 页、62 目录项；目标 PDF 69 页、67 目录项。最终独立构建目录：
`.codex-tmp/design-pdf-20260908T043646760539Z/`。
两份字体嵌入、目录页码、构建输入/PDF 身份检查 PASS，无警告/溢出/缺字；技术正文按计划不同。
当前新增行为页和目标 TG 页面已渲染抽查。完整机器结果与失败历史见
[R2 证据](../specs/182-native-di-python-bindings/evidence/design-r2-20260907.md)。
工具回归 4 PASS；295 API 文件、16586 声明、5744 函数记录（5743 唯一 API ID）、852 绑定 PASS；
当前 460 文件、目标 350 文件 Git+patch 还原 PASS，检查时当前无源码漂移。
当前源码基线保留采样提交与未提交补丁；后续源码修改须重新核对，不自动继承本次 PASS。
产品测试、MiniNDN、SIF、Tiger 均 NOT_RUN；目标五项 PLANNED 不计实现完成。
