# Implementation Plan: Native NDNSF-DI with Optional Python Bindings

**Branch**: Experimental | **Revision**: 5 | **Date**: 2026-09-06
**Status**: DRAFT / NOT_STARTED
**Spec**: [spec.md](spec.md)

## Summary

本机建立完整 C++ DI，Python 只作同库绑定。保留现有 Core 协作/安全原语、
Provider native runtime 和模型 adapter 边界；消除默认 Python 控制实现及 helper 依赖。
本计划基于正在修复的合并工作区。182为当前设计；本轮更新文档，不实施迁移。181完整旧资格不再是前置门，合并稳定基线和承接表仍必须确认。

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
| Cohesive tasks | 每任务包含实现、S0静态审查/修复复审、focused检查和证据；大单元先修订边界 |
| Convergence | 每单元S0 PASS前不运行对应测试；T015整体PASS前不执行正式T016 |
| Immutable delivery | 源、库、adapter、依赖、工件、config、harness 同一身份 |
| Language / code design | 中文叙述、英文 markers；技术细节归规范性 CD 附件 |

## Gate Order

1. G0 / T001：确认合并修复与181承接后刷新 baseline，关闭 O-001--005，冻结类型/调用方/依赖锁，
   自审达到相关范围 READY_FOR_IMPLEMENTATION。当前尚不满足。
2. G1 / T002--009：独立库、策略/sealer/grant、ONNX/tokenizer、请求准备/admission 和 Provider host；每单元实现后S0 PASS，再focused proof。
3. G2 / T010--012：完整 C++ requester、会话/恢复与同库 Python binding；S0覆盖调用链及测试后unit→focused integration。
4. G3 / T013--014：maintained callers 切换、旧路径退出、无 Python gate、MiniNDN harness/collector/fixture；先S0审查其逻辑再反例自检，正式用例尚不运行。
5. G4 / T015：整体静态/convergence audit，覆盖跨单元生产路径、test/oracle/harness、effective config与依赖；不替代此前各单元S0。
6. G5 / T016：每层核对有效S0 subject/scope；同源完整unit PASS → integration PASS → MiniNDN/no-Python资格，保留失败首边界；规定的mutation/anti-fake证据齐全后完成整体S1和final diff才结束T016。
7. G6 / T017：核对T016整体S1/final diff及同源行为证据后，本地开发交付、调用示例、迁移说明与外部实验交接。

### Per-Unit Static Review

每单元实施/测试编写 → compile-oriented读码 → S0三层审查/5风险检错映射 → 修复控制性finding/复审PASS → Waf/native构建 → unit → adjacent → integration → 适用运行/mutation证明 → S1对抗复审 → final diff。规则与报告见 [static review](contracts/pre-test-static-review.md)。转入更广测试范围前检查审查覆盖；身份未变且已覆盖可复用，有行为修复则先标STALE、重审再重跑。具名RED/mutant也先接受独立受控范围审查，不能借此放行产品测试。T001只检查设计/已合并基线，未实现的迁移无产品静态PASS。

### Dependencies

~~~text
Merged baseline closure and Spec181 handoff -> T001
T001 -> T002 -> T003 -> T004 -> T005
T002 -> T006
T002 -> T007
T003/T006/T007 -> T008
T006/T007 -> T009
T003/T004/T005/T006/T007/T008/T009 -> T010 -> T011 -> T012
T012 -> T013 -> T014 -> T015 -> T016 -> T017
~~~

T006/T007 的原生依赖设计必须先由 T001 关闭，不能一边猜 ABI 一边并入 requester。
T003--011 中的大算法迁移为设计批次，超过工作单元阈值时按 work-units 先沿稳定行为接口细分，
不机械按文件拆分、不授权并行 agent 自动实施。
当前授权仅文档，不运行以上实现/构建/实验。Static review PASS != Behavior PASS。

## Migration and Compatibility

新原生路径在独立消费者中验证后，Python 默认入口一次切换到同库；
旧实现仅在迁移窗口保留；T013 关闭时删除无生产消费者的运行实现，离线对照不进入运行包。
无长期双默认路径。公开 API 未实现的兼容项由 inventory 显式 BLOCK，不静默 fallback。
旧 journal/model/contract 格式使用原版本规则；字节不一致先修订设计，不能改 oracle 消除差异。
具体 mixed-version、数据备份/原子转换/回退和旧路径 owner 见
[migration contract](contracts/runtime-boundaries.md#migration-and-rollback-contract)。
GUI、离线训练/导出与实验 Python 保留；其业务调用转向 binding，禁止在工具层藏 runtime owner。

## Invalidation Matrix

| Changed plane | Invalidated evidence | Earliest gate |
| --- | --- | --- |
| Merged source / inherited capability baseline | CD inventory、迁移假设、下游全部 | G0 |
| strategy/sealer/contract | 对应S0、wire/placement及integration/qualification | 相关G1的S0 + G4 |
| grant/assembly/tokenizer | 对应S0、安全/字节/token结果及下游 | 相关G1的S0 + G4 |
| lifecycle/binding/default routing | 对应S0、状态/兼容/no-Python及下游 | G2或G3的S0 + G4 |
| compiler/native dependency/ABI/build recipe | 对应S0、installed consumer、runtime identity及下游 | S0及G1 build closure + G4 |
| config/model/oracle/harness | 对应S0、受影响的proof和正式运行 | S0及源/输入preflight + G4 |
| 纯文档无行为变化 | 结构/追踪/链接检查 | 文档检查；不得自动重跑模型 |

## Delivery

T017 的 evidence/development-handoff.md 包含 exact commit、clean source closure、
库/可选绑定/依赖/adapter/model/config/oracle/harness hashes、可复现命令、
全部本地 verdict、失败处置和已知限制。私钥不入 Git。
另附原生库头/链接说明与最短 C++/Python 示例；示例均调用同一 public API。

外部 owner=实验机器：消费该提交构建 SIF，验证 exact runtime dependency closure，
运行 Tiger 并返回该身份的日志和 verdict。TRANSFERRED 只表示责任移交，
不得把未运行的 SIF/Tiger/GPU/性能写为 PASS。
既有容器内 ABI/build boundary 仍适用，不把 host .so 或 venv 装入镜像充当构建。

## Current Planning Result

设计revision 5已规定符号/字段契约、S0前置审查、架构不变量、工作边界与PO；
完整实现就绪受 O-001--005 控制；O-005 设计由 T001 关闭，T014 再实现隔离并证明检错能力。
下一步待合并修复提交稳定后执行T001，核对源码漂移、关闭叶子schema/ABI与兼容清单，再开始原生迁移。

## Symbol and Toolchain Gates

FR-017/SC-009 的设计审查执行 [symbol contract](contracts/symbol-design.md) 与 [field contract](contracts/value-contracts.md)：任务逐项列 SymbolContracts、Documentation、Usage；公开接口注释和使用示例随同实现验收。局部实现细节只有不改变外部行为且无资源/安全/状态责任时才可列 LOCAL_DETAIL。

后续原生构建使用已核对的 system compiler/binutils closure，匹配 Boost headers/libs、NAC-ABE prefix 与 NDN-SVS build；ABI变更后重建全部依赖对象和绑定，核对实际加载路径/hash。默认至多-j2，不并发操作同一Waf树。当前文档检查不触发任何构建；合并记录中的依赖PASS不能替代新库验收。

## Review Budget and Post-Test Closure

SR-010--013规定三层审查与风险映射；无已确认阻塞则立即测试，动态假设不要求静态证明。一次初审后仅复查影响面，同一问题两轮仍不收敛回明确设计问题或经审查的最小诊断，不自动放行、不无限重构。测试失败先读实际失败路径再修复。

S1在各任务计划测试通过后、完成勾选/提交前执行；T015不替代T016的测试后整体审查。修复使对应S0/S1及下游测试证据失效；无行为改变可说明后复用。T017核对最终diff、风险检错证据和交付身份，未验证的必要行为阻止完成。
