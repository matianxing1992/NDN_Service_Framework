# NDNSF 四模块设计

本目录保存中文设计，覆盖 NDNSF Core、NDNSF-UAV、NDNSF-DI 和 NDNSF-Repo 的完整子系统视图。

- [当前设计](current-design.pdf)：按源码核对职责、接口、控制与数据流程、状态、安全、恢复及实现边界。
- [目标设计](target-design.pdf)：R0 技术正文与当前设计完全一致，后续按用户指示独立修改。
- [覆盖矩阵](coverage-matrix.md)：章节与模块、核对入口的对应关系。
- [验证记录](validation.md)：构建、版面和正文一致性检查。
- [源码基线](source-baseline.json)：94 个关键文件的摘要、采样时间、提交与本地源码存档。
- [模块清单](module-inventory.json)：606 个已跟踪文件的范围盘点；清单登记不等于逐行审计。
- [Spec 设计变更记录](spec-design-changes.md)：每个 Spec 的设计差异、实现状态、源码提交及验证证据。

## Git 版本管理

用户已改为要求完整纳入 Git。两份 PDF、可编辑正文、布局、构建脚本、设计变更记录、清单和精简验证证据一起提交到 Experimental，便于逐次比较；不自动推送。原始构建日志、预览图和源码压缩包仍在 .codex-tmp/。
原始源码基线包含未提交文件，其相对基线提交的精确差异保存在 [源码快照补丁](evidence/source-baseline-worktree.patch)，可与 source-baseline.json 联合核对，不把其他工作单元的源码修改纳入本次提交。

## 构建

从仓库根执行 `python3 Design/build.py`。依赖 XeLaTeX、ctex、TikZ、Noto CJK、TeX Gyre 和 DejaVu 字体。
每次双份各编译两遍，日志保留在独立的 `.codex-tmp/design-pdf-<时间>/`；两份均成功才更新本目录 PDF。

## 后续维护

1. 用户提出目标变化时，修改 `target-content.tex` 与修订记录，重建目标 PDF，并检查与当前正文的差异。
2. 当前设计仅在重新核对实际源码后更新；目标提议不会自动成为实现事实。
3. 核对源码变化后可运行 `python3 Design/refresh-snapshot.py` 刷新已登记文件的快照，再重建 PDF；该脚本只记录字节，不代替源码审查。
4. 四模块增加新入口时同步覆盖矩阵和模块清单。保留目标侧已接受的差异，不自动覆盖目标正文。
5. 文档不替代 active Spec，也不改变功能或运行资格验收。
6. 每个 Spec 开始、目标变更和实现验收时同步 spec-design-changes.md；无设计变化也记录“无”，部分实现保持 PARTIAL。更新 PDF、正文和记录须属于同一文档提交。

下一步：根据用户后续要求修改目标设计，保持当前设计作为可核对的实现基线。
