# Implementation Plan: Native NDNSF-DI with Optional Python Bindings

**Branch**: Experimental | **Revision**: 8 | **Date**: 2026-09-07
**Status**: IN_PROGRESS / 当前实现与验收状态见 tasks.md 的 Task Progress Registry 和 Current Checkpoint
**Spec**: [spec.md](spec.md)

## Summary

完整C++ DI复用Core协作/安全原语、Provider runtime和原生model adapters；
Python只作同库兼容绑定，旧默认控制路径退出。O-001源码基线已核对关闭，
O-002--O-005 已于 2026-09-07 全部关闭（code-design Open Questions 无 OPEN 项）；
实现按 T001-C 冻结的 build identity 与 case-manifest selector 从 T002-A 起执行。
详细接口与字段仅在contracts定义；本文件安排实施和验证阶段。

## Technical Context

建立可安装ndnsf-distributed-inference库与独立C++ consumer，Python绑定可选。
沿用Waf、C++/Boost/ndn-cxx/ORT；ONNX/tokenizer依赖及ABI锁由T001冻结。
内部 typed JSON 使用固定 nlohmann/json 源与许可证；版本/哈希见 native-dependencies.json。
T004 已接入完整 projection codec 与 canonical core/final identity；实际 planner metadata 和
requester 接线仍按未完成任务推进，定向 PASS 不代表整体资格验收。
原生构建使用核对后的system compiler/binutils、匹配Boost headers/libs、
NAC-ABE prefix与NDN-SVS source/build pair，并包含直接消费SVS ABI的NDNSD；ABI变化重建全部传递消费者与绑定并核对实际加载路径/hash。四库版本及旧证据失效边界见[integrated baseline](contracts/integrated-baseline.md#current-source-identity)。
本开发机默认-j4（6逻辑CPU/12GB RAM，2026-09-07用户授权），不并发操作同一Waf树或叠加原生构建。
持续换页或桌面卡顿时下次降为-j2；其他机器及容器builder另核资源，见[build policy](../../docs/native-build-parallelism.md)。
T001允许有界依赖探针；产品构建按设计门和各任务的验证范围执行。

## Constitution Check

沿用Core协作与安全边界、同库绑定和旧路径退出契约；保留全部真实运行验收。
流程只调整记录方式与执行阶段，不缩减权限、协议、数值或隔离要求。

## Gate Order

2026-09-07 implementation audit：T004-A 因真实 Selection wire/identity 不兼容重开，
见 [A8-01](evidence/t004-wire-reopened-20260907.md)。在继续 T010 完整请求提交前，
先修复 T004 的完整 canonical wire、真实工件和 grant 输入，并复核受影响前置值契约；
不得将七字段片段包装为可执行计划。其余阶段及 T016 正式运行顺序保持。

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

### B-G1-YOLO-SEMANTIC

2026-09-08：当前生产适配器接线的直接前置是 T003-B 尚缺的真实 catalog semantic
partition 消费。先完成该既有 PARTIAL 单元的修复批次，再推进 T008-A；不以共享
ONNX helper 的局部 PASS 放行 T003-A 或 T003-C 的完整验收。

| Member | Behavior boundary | Implementation dependency |
| --- | --- | --- |
| YS-1 / T003-B | 将注册 partition 的语义节点名映射到实际 inspection 的 planning node ID；核对完整 cover | 已通过定向测试的 owned ONNX inspection API；保持其完整输入/graph identity 绑定 |
| YS-2 / T003-B | 对照实际图与 metadata 验证 tensorInterfaces、dependencyEdges、safeCuts、roleInterfaces | YS-1 静态门；不得只有 node cover 就接受 catalog |
| YS-3 / T003-B | 注册 catalog 消费入口接入 NativeYoloComponentSplit，并编写实际 Python splitter 对照及篡改负例 | YS-1/YS-2 静态门；不能只测试独立转换 helper |

Owner：当前执行者。共享测试选择器：Spec182YoloSplit、Spec182NativePlanning、
Spec182Preparation、Spec182CanonicalPublisher、Spec182V3Placement；同一批次最后
统一 `waf build --targets=unit-tests -j4`。ABI 变化仍按 toolchain preflight 选择
fresh tree，不能复用不兼容对象。静态门及测试结果统一记录于
[batch evidence](evidence/t003-yolo-semantic-batch-20260908.md)。
T003-A/T003-C/T008-A 的既有 acceptance dependencies 保留；本批不运行 requester、
integration、MiniNDN 或 Tiger，不把 catalog 声明当作 Core 来源认证。

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
用户已授权在Experimental完成Spec182；当前按T001关闭设计，再按上述门执行实现与本地验证。SIF/Tiger仍由实验机器负责。Static review PASS != Behavior PASS。

## Bounded Executor Profile

所有执行者统一使用 [execution cards](contracts/execution-units.md)
展开现有17个父任务，按一张就绪卡的 Read/Write/Steps/Verify 分派，沿稳定行为边界拆分。
T001由设计者关闭未决契约与选择器；设计冻结后的实现按依赖分派，ABI、安全、生命周期和整体收敛由相应审查者复核。
每卡记录实际源码身份与证据；依赖或设计变化只重新检查受影响卡。卡完成不提前关闭父任务或T016。
执行卡覆盖与链接检查不证明产品完成；当前子任务状态统一记录在 tasks.md 的 Execution Progress。
原 G0--G6 顺序与上方父任务依赖保持。本文件引用的共享设计技能为本仓库版本。

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

T001-A/B/C 设计关闭全部完成（2026-09-07，evidence
[t001-ab-closure](evidence/t001-ab-closure-20260907.md) 与
[t001-c-freeze](evidence/t001-c-freeze-20260907.md)）；用户已授权在Experimental
完成182，产品实现与最终运行验证仍 NOT_STARTED，按任务门 T002 起执行。
O-001--O-005 全部关闭：O-002 由 [native ONNX assembly design](contracts/native-onnx-assembly-design.md)
设计收口，O-003/O-005 见 [native dependency design](contracts/native-dependency-design.md) 与
[native isolation design](contracts/native-isolation-design.md)，O-004 静态映射收口
于 compatibility-manifest（62157804/6fa1f756）；运行期 build identity、L0 命令与
每卡 planned suite/case selector 冻结于 [case-manifest](../../tests/fixtures/spec182/case-manifest.json)。
CD-005 新增具名原生装配 worker，以保留不可中断ONNX调用的取消/超时清理；库、worker
安装、父子协议及隔离白名单见 [native ONNX assembly design](contracts/native-onnx-assembly-design.md)。
产品 native 构建的当前工具链边界为 NAC-ABE ABI 5 符号缺口
（[failure-log](../../docs/failure-log.md) 2026-09-07），T002-A 匹配后重跑首次 L0。
后续182开发仍按G1--G6执行本地开发验证；上一轮delivery-only的TRANSFERRED不是永久移走182的T016义务。
使用仓库[shared design skill](../../skills/speckit-code-design/SKILL.md)及其相对引用，避免依赖开发机个人技能路径。T017复用现有[handoff tooling](../../Experiments/TigerCluster/docs/source-handoff.md)，另生成182身份与依赖清单；旧锁包含Python运行包且不含planned原生DI库，不能直接称为182 no-Python交付。
