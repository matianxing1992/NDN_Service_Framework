# NDNSF 四模块设计

本目录保存中文设计，覆盖 NDNSF Core、NDNSF-UAV、NDNSF-DI 和 NDNSF-Repo 的完整子系统视图。

- [当前设计](current-design.pdf)：按源码核对职责、接口、控制与数据流程、状态、安全、恢复及实现边界。
- [目标设计](target-design.pdf)：R2 独立冻结 API 基线，并纳入 TG-01 至 TG-05 五项 PLANNED 改进。
- [覆盖矩阵](coverage-matrix.md)：章节与模块、核对入口的对应关系。
- [验证记录](validation.md)：构建、版面和正文一致性检查。
- [源码基线](source-baseline.json)：维护实现、配置及 API 输入的摘要、采样时间、提交与精确补丁；具体数量以清单为准。
- [模块清单](module-inventory.json)：R0 的 606 文件历史盘点；R2 当前漂移范围以 source-baseline.json 和 design_state.py 为准，登记不等于逐行审计。
- [Spec 设计变更记录](spec-design-changes.md)：每个 Spec 的设计差异、实现状态、源码提交及验证证据。
- [API 查询入口](api/README.md)：295 文件的声明参考和 23 组中文行为契约，保留类型、重载、默认值及源码位置。
- [管理规则](MANAGEMENT.md)：AGENTS.md 执行的设计/API/Spec 同步流程。
- [参考文献](references.md)：NFD Developer’s Guide 的版本与采用范围。

## Git 版本管理

用户已改为要求完整纳入 Git。两份 PDF、可编辑正文、布局、构建脚本、设计变更记录、清单和精简验证证据一起提交到 Experimental，便于逐次比较；不自动推送。原始构建日志、预览图和源码压缩包仍在 .codex-tmp/。
原始源码基线包含未提交文件，其相对基线提交的精确差异保存在 [源码快照补丁](evidence/source-baseline-worktree.patch)，可与 source-baseline.json 联合核对，不把其他工作单元的源码修改纳入本次提交。

## 构建

从仓库根执行 `python3 Design/build.py`。依赖 XeLaTeX、ctex、TikZ、Noto CJK、TeX Gyre 和 DejaVu 字体。
每次双份各编译三遍以稳定目录页码，日志保留在独立的 `.codex-tmp/design-pdf-<时间>/`；两份均成功才更新本目录 PDF。

## 后续维护

1. 用户提出目标变化时，修改 `target-content.tex`、`target-api-contracts.json` 与修订记录；运行 `python3 Design/render-api-contracts.py --target`，再重建 PDF。实际目标出现差异时将 document-contract.json 的 expect_equal 改为 false，并登记预期差异。
2. 当前设计仅在重新核对实际源码后更新；目标提议不会自动成为实现事实。
3. 核对源码变化后运行 `python3 Design/build-behavior-coverage.py` 和 `python3 Design/refresh-snapshot.py`，检查全部维护源文件范围，再重建 PDF；快照只记录字节，不代替语义审查。
4. 四模块增加新入口时同步覆盖矩阵和模块清单。保留目标侧已接受的差异，不自动覆盖目标正文。
5. 文档不替代 active Spec，也不改变功能或运行资格验收。
6. 每个 Spec 开始、目标变更和实现验收时同步 spec-design-changes.md；无设计变化也记录“无”，部分实现保持 PARTIAL。更新 PDF、正文和记录须属于同一文档提交。

下一步：根据用户后续要求修改目标设计，保持当前设计作为可核对的实现基线。

## API 更新命令

源码 API 变化后执行 `python3 Design/build-api-reference.py`，只生成当前声明参考和当前契约 TeX；审查 diff 并补全 api-contracts.json 的中文语义后，再独立渲染。
生成器使用 Node 和 web-tree-sitter/C++ wasm；本机复用 CodeGraph 安装，其他机器用 NDNSF_TREE_SITTER_ROOT 指向包含 web-tree-sitter 与 tree-sitter-wasms 的 node_modules。Python AST 不导入产品模块。
运行 `python3 Design/test_design_state.py` 检查遗漏/旧 PDF/目标耦合回归；运行 `python3 Design/verify-api-reference.py` 检查源码全集、快照、声明、行为覆盖和双侧生成内容；运行 `python3 Design/verify-source-baseline.py` 与加 `--target` 的命令检查两侧可还原身份。
PDF 构建后运行 `python3 Design/verify.py <本次构建目录>` 并人工检查版面。生成命令不是行为审计，完整最低要求见 MANAGEMENT.md。
