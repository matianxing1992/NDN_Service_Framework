# Specification Quality Checklist

**Revision**: 6
**Feature**: [spec.md](../spec.md)

## Design And Workflow

- [x] 用户目标、C++ DI职责与Core边界明确；Python只作兼容绑定。
- [x] 保留类/方法/重要字段、注释、调用方式与迁移契约。
- [x] 共用规则只定义一次，任务引用具体CD/PO。
- [x] 实现任务按实现→静态审查→相关unit完成；集成/实验集中T016。
- [x] 测试与harness随实现编写，unit与跨进程用例分开选择。
- [x] 保留独立oracle、实际生产路径和既定负例/反事实。
- [x] 单份结果记录；不要求固定风险数、独立S0/S1或逐层放行报告。
- [x] Static review PASS != Behavior PASS；局部任务完成不代表完整PO闭合。

## Implementation And Acceptance

- [ ] T001关闭O-001--005，冻结实际基线、依赖与具体selector。
- [ ] T002--T014实现、静态审查、相关unit及必要构建完成。
- [ ] T015整体生产接线、测试/oracle/harness审查无控制性缺陷。
- [ ] T016完整unit→integration→MiniNDN/no-Python和既定负例全部通过。
- [ ] T017核对最终diff、交付身份与同源证据；外部实验单独TRANSFERRED。

本轮文档检查见 [workflow simplification](../evidence/workflow-simplification.md)，
以上产品检查尚未执行。没有新变化、失败或具体缺口时不追加重复全量验证。
