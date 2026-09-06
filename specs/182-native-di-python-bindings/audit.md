# Spec182 Design Audit

**Revision**: 6 | **Mode**: workflow simplification / pre-implementation
**Verdict**: DRAFT / BLOCK for implementation
**Evidence**: [workflow simplification](evidence/workflow-simplification.md)

## Current Findings

本轮将重复审查、固定风险配额和独立前后报告合并为一次读码审查与一份结果记录。
实现任务只做相关unit，全部实现后由T016统一执行integration/MiniNDN；
具体PO、负例、模型oracle和C++职责边界保留。阶段完成不等于完整PO或feature验收。
文档结构检查只能验证ID、链接和分层约定，不能确认产品逻辑正确。

| Open item | Controlling gap | Owner |
| --- | --- | --- |
| O-001 | 合并修复稳定基线与181承接尚待确认 | T001 |
| O-002 | ONNX原生装配字节契约与依赖锁 | T001 |
| O-003 | tokenizer原生依赖、ABI及固定向量 | T001 |
| O-004 | 完整旧能力、调用方与测试selector清单 | T001 |
| O-005 | 无Python隔离方案可行性与边界 | T001 |

上述未决项沿用 [code-design](contracts/code-design.md#open-questions)，本轮没有关闭。
T002--T014的各项实现、测试工具编写、静态审查、局部单测完成后，T015补审整体接线，
T016收齐真实运行证据，T017交付。验收标准满足即结束；变化或具体缺陷才触发受影响回归。

## History

既有审计发现的历史理由与证据保留在Git及
[revision 2](evidence/audit-revision2.md)、
[revision 3](evidence/skill-and-design-revision3.md)、
[revision 4](evidence/static-review-gate-revision4.md)、
[revision 5](evidence/adversarial-review-revision5.md)。
旧revision中固定五项风险、独立S0/S1报告与逐任务integration要求由revision 6替代。
历史源码快照与运行结果不回填为当前实现或资格证明。

## Next Action

T001核对最终合并源码，关闭O-001--005并冻结unit/integration的独立选择器。
产品实现 **0/17**，产品审查及unit/integration/MiniNDN **NOT_RUN**。
