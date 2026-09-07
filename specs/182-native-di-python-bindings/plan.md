# Implementation Plan: Native NDNSF-DI with Optional Python Bindings

**Branch**: Experimental | **Revision**: 7 | **Date**: 2026-09-06
**Status**: DRAFT / NOT_STARTED
**Spec**: [spec.md](spec.md)

## Summary

完整C++ DI复用Core协作/安全原语、Provider runtime和原生model adapters；
Python只作同库兼容绑定，旧默认控制路径退出。O-001源码基线已核对关闭；O-002--005关闭前不开始迁移。
详细接口与字段仅在contracts定义；本文件安排实施和验证阶段。

## Technical Context

建立可安装ndnsf-distributed-inference库与独立C++ consumer，Python绑定可选。
沿用Waf、C++/Boost/ndn-cxx/ORT；ONNX/tokenizer依赖及ABI锁由T001冻结。
原生构建使用核对后的system compiler/binutils、匹配Boost headers/libs、
NAC-ABE prefix与NDN-SVS source/build pair，并包含直接消费SVS ABI的NDNSD；ABI变化重建全部传递消费者与绑定并核对实际加载路径/hash。四库版本及旧证据失效边界见[integrated baseline](contracts/integrated-baseline.md#current-source-identity)。
默认至多-j2，不并发操作同一Waf树。T001允许有界依赖探针；产品构建按设计门和各任务的验证范围执行。

## Constitution Check

沿用Core协作与安全边界、同库绑定和旧路径退出契约；保留全部真实运行验收。
流程只调整记录方式与执行阶段，不缩减权限、协议、数值或隔离要求。

## Gate Order

1. G0 / T001：复用已关闭O-001的源码身份与181承接，关闭O-002--005，冻结schema/调用方/依赖与单测、集成、实验选择器。181旧完整资格不作为前置门；源码基线关闭不表示新依赖组合运行PASS。
2. G1 / T002--009：库、策略、sealer/grant、assembler/tokenizer、准备/admission和Provider host；每任务实现→静态审查→相关单测及必要构建。
3. G2 / T010--012：requester、会话/恢复与绑定；完成接线、相关单测，同时编写注册后续集成用例。
4. G3 / T013--014：迁移旧入口、实现隔离gate和MiniNDN harness/collector；完成静态审查与本地单测，真实跨进程/no-Python用例尚不运行。
5. G4 / T015：全部实现与测试工具完成后，补审跨任务调用链、effective config、测试/oracle/harness和依赖；复用有效局部审查，控制性缺陷修复后进入T016。
6. G5 / T016：统一执行完整unit→integration→MiniNDN/no-Python及既定负例，核对实际证据和最终diff；不另写测试后报告。
7. G6 / T017：交付已验证版本、说明与示例；外部SIF/Tiger由实验机器接手。

审查内容、最小诊断例外、变化/失败处理和唯一结果记录见
[validation workflow](contracts/pre-test-static-review.md)。
实现任务[x]表示实现/审查/单测完成；完整PO与feature验收直到T016才关闭。
任务开始本身不会使前项单测失效；实际变化决定重审和回归范围。

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

在本地候选版本中将Python默认入口一次切换到同库；独立消费者的完整运行与迁移正确性由T016统一验收后交付。
旧实现仅在迁移窗口保留；T013 关闭时删除无生产消费者的运行实现，离线对照不进入运行包。
无长期双默认路径。公开 API 未实现的兼容项由 inventory 显式 BLOCK，不静默 fallback。
旧 journal/model/contract 格式使用原版本规则；字节不一致先修订设计，不能改 oracle 消除差异。
具体 mixed-version、数据备份/原子转换/回退和旧路径 owner 见
[migration contract](contracts/runtime-boundaries.md#migration-and-rollback-contract)。
GUI、离线训练/导出与实验 Python 保留；其业务调用转向 binding，禁止在工具层藏 runtime owner。

## Evidence Reuse

按源码、接口、依赖、配置、oracle/harness的实际变化判断影响；
只更新受影响契约、重读相关调用方并重跑相关检查。没有变化或具体缺口不重复全量验证。
未运行、环境失败、局部通过和完整资格分开，原始失败结果保留。
最终版本必须有全部既定本地运行证明；文档改动不触发模型重跑。

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

revision 7源码对照审计已完成；用户已授权在Experimental完成182，当前T001 IN_PROGRESS，产品实现与最终运行验证仍NOT_STARTED。
O-001源码核对已关闭；T001关闭O-002--005，依赖可行性及实际探针见[native dependency design](contracts/native-dependency-design.md)。实验机器已接收上一轮交付，本轮不继续管理它；不恢复旧交付构建，后续按182任务重新固定并验证所需依赖身份。
后续182开发仍按G1--G6执行本地开发验证；上一轮delivery-only的TRANSFERRED不是永久移走182的T016义务。
使用仓库[shared design skill](../../skills/speckit-code-design/SKILL.md)及其相对引用，避免依赖开发机个人技能路径。T017复用现有[handoff tooling](../../Experiments/TigerCluster/docs/source-handoff.md)，另生成182身份与依赖清单；旧锁包含Python运行包且不含planned原生DI库，不能直接称为182 no-Python交付。
