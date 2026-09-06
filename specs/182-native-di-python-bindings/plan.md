# Implementation Plan: Native NDNSF-DI with Optional Python Bindings

**Branch**: Experimental | **Revision**: 1 | **Date**: 2026-09-06
**Status**: DRAFT / NOT_STARTED
**Spec**: [spec.md](spec.md)

## Summary

本机建立完整 C++ DI，Python 只作同库绑定。保留现有 Core 协作/安全原语、
Provider native runtime 和模型 adapter 边界；消除默认 Python 控制实现及 helper 依赖。
本计划是后续规划，不激活 Spec182，不改变仍在执行的 Spec181。

## Technical Context

当前 native 以 Waf core/adapter objects 和 executable 为主；Python setup 也直接编译部分 DI 源。
目标为可安装的 ndnsf-distributed-inference 库和独立 C++ consumer；Python 绑定可选。
使用仓库既有 C++/Boost/ndn-cxx/ORT 工具链，ONNX/protobuf/tokenizer 精确锁尚待 O-002/O-003。
不在此声称具体原生 tokenizer 依赖已安装、可链接或能通过向量。

## Constitution Check

| Principle | Planned compliance |
| --- | --- |
| Canonical runtime | 复用 ServiceUser deferred collaboration，不引入平行 wire |
| Security | 保留 grant/AEAD/lease/fencing 和 Provider 独立授权 |
| CodeGraph | 精确主工作区路径，排除临时比较副本 |
| Cohesive tasks | 每任务同时包含实现、focused 检查、证据；大单元先修订边界 |
| Convergence | T013 PASS 前不执行正式 T014 |
| Immutable delivery | 源、库、adapter、依赖、工件、config、harness 同一身份 |
| Language / code design | 中文叙述、英文 markers；技术细节归规范性 CD 附件 |

## Gate Order

1. G0 / T001：Spec181 关闭/交付后刷新 baseline，关闭 O-001--004，冻结类型/调用方/依赖锁，
   自审达到相关范围 READY_FOR_IMPLEMENTATION。当前尚不满足。
2. G1 / T002--007：独立库和原生策略、sealer、grant、ONNX、tokenizer 行为闭合；focused proof。
3. G2 / T008--010：完整 C++ requester、会话/恢复与同库 Python binding；focused integration。
4. G3 / T011--012：maintained callers 切换、旧路径退出、无 Python gate 和反例自检。
5. G4 / T013：code-aware convergence audit，检查真实生产路径、effective config、源与运行依赖。
6. G5 / T014：同源完整 unit/integration + MiniNDN + no-Python 资格，保留所有失败首边界。
7. G6 / T015：本地开发交付、调用示例、迁移说明与外部实验交接。

### Dependencies

~~~text
Spec181 local closure -> T001
T001 -> T002 -> T003 -> T004 -> T005
T002 -> T006
T002 -> T007
T003/T004/T005/T006/T007 -> T008 -> T009 -> T010
T010 -> T011 -> T012 -> T013 -> T014 -> T015
~~~

T006/T007 的原生依赖设计必须先由 T001 关闭，不能一边猜 ABI 一边并入 requester。
T003--009 的大算法迁移为设计批次，超过工作单元阈值时按 work-units 先沿稳定行为接口细分，
不机械按文件拆分、不授权并行 agent 自动实施。
当前授权仅文档，不运行以上实现/构建/实验。

## Migration and Compatibility

新原生路径在独立消费者中验证后，Python 默认入口一次切换到同库；
旧实现保留在测试对照用途直至所有真实调用方有明确去向。
无长期双默认路径。公开 API 未实现的兼容项由 inventory 显式 BLOCK，不静默 fallback。
旧 journal/model/contract 格式使用原版本规则；字节不一致先修订设计，不能改 oracle 消除差异。
GUI、离线训练/导出与实验 Python 保留；其业务调用转向 binding，禁止在工具层藏 runtime owner。

## Invalidation Matrix

| Changed plane | Invalidated evidence | Earliest gate |
| --- | --- | --- |
| 181 final source / capability baseline | CD inventory、迁移假设、下游全部 | G0 |
| strategy/sealer/contract | wire/placement + integration/qualification | 相关 G1 + G4 |
| grant/assembly/tokenizer | 安全/字节/token oracle 对应结果 + 下游 | 相关 G1 + G4 |
| lifecycle/binding/default routing | 状态/兼容/no-Python/下游 | G2 或 G3 + G4 |
| compiler/native dependency/ABI/build recipe | installed consumer、runtime identity、下游 | G1 build closure + G4 |
| config/model/oracle/harness | 受影响的 proof 和正式运行 | 源/输入 preflight + G4 |
| 纯文档无行为变化 | 结构/追踪/链接检查 | 文档检查；不得自动重跑模型 |

## Delivery

T015 的 evidence/development-handoff.md 包含 exact commit、clean source closure、
库/可选绑定/依赖/adapter/model/config/oracle/harness hashes、可复现命令、
全部本地 verdict、失败处置和已知限制。私钥不入 Git。
另附原生库头/链接说明与最短 C++/Python 示例；示例均调用同一 public API。

外部 owner=实验机器：消费该提交构建 SIF，验证 exact runtime dependency closure，
运行 Tiger 并返回该身份的日志和 verdict。TRANSFERRED 只表示责任移交，
不得把未运行的 SIF/Tiger/GPU/性能写为 PASS。
既有容器内 ABI/build boundary 仍适用，不把 host .so 或 venv 装入镜像充当构建。

## Current Planning Result

设计 revision 1 已有范围、架构不变量、CD、工作边界和 PO；
完整实现就绪仍受 O-001--004 控制，native isolation O-005 控制正式无 Python gate。
下一个执行动作是继续 Spec181；其关闭后从 T001 刷新/补齐设计开始。
