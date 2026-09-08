# Four-Module Diagram Guide

## Work Unit

D-DESIGN-DIAGRAMS；用户授权补充 Design 三类图，当前/目标各 G1--G9。图源为 TikZ，保持矢量与可编辑；共享关系与 DI 当前/目标专属流程分开维护。来源索引见 Design/diagrams/README.md。

## Source And Review

CodeGraph status 可用，定位实际类后读取 canonical 源文件；查询含临时副本时不使用副本作为权威。核对 Core 容器共享/借用重载，DI handle/operation/provider 状态，Repo 值成员与 backend 共享持有，UAV unique/shared 成员。目标冻结 archive 核对保留的对象关系，G7 PLANNED 复用 TG-02/TG-03。

Context Mode active health 因 spec/plan/tasks 源哈希过期失败，使用仓库指针/文档及真实源码，不修改检索库。当前 API 增量刷新 295 文件、16616 条声明，最终校验登记 5755 函数；冻结目标不覆盖。源码基线详见 source-baseline.json；字节身份不表示整仓语义或运行资格。

## Validation Attempts

R1：`.codex-tmp/design-pdf-20260908T074510261378Z`。双 PDF 构建成功，但 verify.py exit 1：di-classes.tex 第 22--23 行图边界 overfull 70.85669pt。首个失败边界是两侧弯曲返回箭头及标签扩展图宽，不是字体、源码或产品测试失败。保留该次完整日志，下一版改用固定宽度折线路径。

## Results

R3：`.codex-tmp/design-pdf-20260908T074740205470Z` 双 PDF 构建完成且版面门通过；API 校验 exit 1，首个边界是并发修改 `Experiments/TigerCluster/README.md` 和 `docs/sif-build.md`，与刚采样的 supporting-source 摘要不一致。未放宽漂移门；保留日志，刷新当前源码身份后重建。五项 design_state 工具回归 PASS；该检查与产品运行无关。

R2：`.codex-tmp/design-pdf-20260908T074639196287Z` 构建成功。静态复核发现图中将等待动作误写为 wait 方法，实际两侧 NativeInferenceHandle API 均为 result(waitTimeout)，已在图、说明和来源表修正后重新生成；不把动作名当作不存在的 API。

最终 R5：`.codex-tmp/design-pdf-20260908T075110290934Z`。双 PDF 构建、verify.py 均 exit 0：当前 91 页/59 章，目标 96 页/64 章，无 warnings，字体嵌入、目录标题/页码与构建输入身份 PASS。API 验证 PASS；当前 460、目标 350 文件按各自提交与补丁还原 PASS，当前源码漂移为空。五项 design_state 工具回归 PASS；git diff --check PASS。

视觉检查：R4 两侧各九页 contact sheets（当前 83--91，目标 88--96）全部查看；R5 仅修正共享 G4 的 getManifest 名称并放大检查最终当前第 86 页，其余 TeX 图源不变。原始预览保留在各 run，不入 Git。采用已读取官方 review-agent 的只读审查规则核对图、引用与相关源码；实现者修复 API 名称和边界后复核。未把当前组件“已持有”推断成默认链已调用。

当前源码基线 `d6165c5365f29e957d0056182402a833426aa18a` 加精确工作树补丁；目标保持 `e9fe33994a6ca3ff81893591bd24c3fae43f933f` 加原冻结补丁。并发产品修改仅作为可重建快照输入记录，不随文档提交为产品实现。产品构建/测试 NOT_RUN；图解不关闭产品任务。

## Next

完成图解验证并 checkpoint；以后接口所有权或关键流程改变时同步对应图及源索引。
