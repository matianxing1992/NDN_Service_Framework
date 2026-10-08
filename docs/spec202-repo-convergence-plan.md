# Spec202 DistributedRepo Convergence Planning Checkpoint

2026-10-08，用户要求通过静态审计比较当前Repo与已接受的理想边界，纳入新缺陷，停止Spec192/193并建立Spec202。活动入口为[Spec202](../specs/202-distributedrepo-convergence/spec.md)，旧两个Spec标记`SUPERSEDED_STOPPED`，历史结果保留，停止不等于完成。

当前产品readiness **BLOCK**。静态源码确认：fetch覆盖目的文件后directory fsync/最终deadline失败不能恢复旧文件（HIGH）；owner source失败会把已尝试且结果未知的target改为NOT_ATTEMPTED/SKIPPED（HIGH）；legacy import发布后parent fsync失败的幂等重放缺少耐久屏障（MEDIUM）。另外存在source overlay闭合、完整资格、API/parser覆盖、A19派生文档跨object plane与A21签名packet oracle缺口。详见[差距审计](../specs/202-distributedrepo-convergence/gap-audit.md)。

[计划](../specs/202-distributedrepo-convergence/plan.md)、[修复契约](../specs/202-distributedrepo-convergence/contracts/architecture-and-api.md)、[任务](../specs/202-distributedrepo-convergence/tasks.md)和[完整验收矩阵](../specs/202-distributedrepo-convergence/validation-matrix.md)保留全部59个稳定用例及子场景、逐条继承Spec192 FR001–025/Spec193 FR001–007，并增加7类native故障回归。native fault验证与两轮完整MiniNDN资格分开；Original Data保持metadata授权与生产者加密边界，不添加没有需求的新公开API。

本次仅文档与静态审计，无产品代码修改/build/test/network run。CodeGraph已核对主要native路径；Context Mode active authority index修复后health通过，但project host capture失败、MCP retrieval为空，使用仓库fallback。静态检查不代表产品通过。[审查记录](../specs/202-distributedrepo-convergence/review.md)明确范围与限制。

下一步是T001完整入口/源码/target inventory，然后T002/T003/T004局部native修复；只有affected门和fresh convergence关闭后，才进入T009/T010统一candidate的全部资格。不自动push，不继续旧Spec任务。
