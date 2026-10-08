# Implementation Plan: DistributedRepo Contract and Implementation Convergence

**Branch**: `Experimental` | **Date**: 2026-10-08 | **Spec**: [spec.md](spec.md)
**Progress Timestamp**: 2026-10-08 12:24 -05:00
**Status**: PLANNING_ONLY; product readiness BLOCK

## Summary

停止Spec192/193独立推进，将其产品目标、未完资格、三个实际缺陷与两处派生契约/oracle问题纳入Spec202。主路径已经native化；实现策略是修复现有owner、消除设计/代码/证据差异并完成统一资格，不重写Repo或添加隐式replication。

本轮仅审计/文档；新任务的product实施与validation均NOT_RUN。详细差距唯一记录为[gap-audit.md](gap-audit.md)，规则在[contracts/architecture-and-api.md](contracts/architecture-and-api.md)与[candidate-and-validation.md](contracts/candidate-and-validation.md)，避免逐任务复制契约。

## Technical Context

**Language/Version**: C++17、Python3薄绑定/外部编排；当前Waf/native targets。
**Primary Dependencies**: ndn-cxx、固定Experimental ndn-svs/system Boost1.71、NDNSF Core/NAC、SQLite、OpenSSL、pybind。
**Storage**: 现有filesystem payload/manifest、native SQLite journal/receipt/location；不新增schema for本轮三个局部修复。
**Testing**: 真实native API/worker/import fixtures，C++ unit/生产integration，Python conformance/parity，风险匹配dynamic，MiniNDN signed packet/storage oracle。
**Target Platform**: 本机Ubuntu20.04；system Boost1.71，build≤j4、单tree；无Tiger/SIF/模型benchmark。
**Project Type**: 现有native library/service与bindings；配置profile明确分离。
**Constraints**: 两种mode/native ownership不变；Core授权边界不变；post-sync deadline不移除；结果与failed destination语义明确；旧原始证据不修改。
**Scale/Scope**: 59稳定case及全部子场景，five/six/seven独立NFD hosts，two完整rounds。
**Open checks**: T001 inventory/source closure、legacy parser兼容边界、destination特殊类型/同路径并发前置；对应任务编码前检查，不隐式作已实现假设。

## Constitution Check

| Principle | Design decision / gate |
| --- | --- |
| native ownership/security | DB001/DB007；Framework负责Core grant，Repo只验证已有identity，不创建第二Python状态机 |
| CodeGraph/source authority | 本轮当前Source hash和调用路径已核对；T001逐入口补齐，旧checkbox不覆盖新缺陷 |
| cohesive tasks | 10个行为task；red/patch/focused regression/evidence属于同一任务，动态门按batch冻结 |
| immutable promotion | DB candidate合同、变更失效矩阵、profile组合、mutation closure，先convergence后昂贵运行 |
| design-code convergence | unresolved G001/G002使product BLOCK；本轮未启动formalvalidation |
| language/state | 中文叙述、English structural IDs/status；Spec202 registry，GSD不另建权威 |
| environment separation | 本机功能资格，无统计/ARS研究主张，无Tiger操作，无push |

## Pre-Qualification Design-Code Convergence

T001固定实际baseline/API与需求，T002–T008完成适用repair/conformance后，T009重新对source/runtime/config/oracle做五lane组合审查。未解决semantic/security/wiring/evidence HIGH保持BLOCK；允许focused red/repair，禁止在BLOCK下启动完整验收suite或MiniNDN资格。任何行为影响变化按invalidation matrix重审受影响范围。

## Logical Batch Quality Plan

| Batch | Members / shared boundary | Static/closing oracle | Dynamic / exit |
| --- | --- | --- | --- |
| B1 | T001现状/目标/入口清单 | definition TU/target/source ownership和59case双向映射 | none/docs；baseline captured |
| B2 | T002 consumer destination事务 | 两public fetch、真实postrename faults/old inode/absence/recovery/stopretry | asan-ubsan；闭合destination安全 |
| B3 | T003 server publication ledger | 真实B请求→timeout→C source read failure、immutable receipts | asan-ubsan；concurrency ownership变化才加tsan |
| B4 | T004 importer publication recovery | postpublish parent-sync failure/replay，同源inventory/no-replace | asan-ubsan；closing retrydurability |
| B5 | T005–T008已有surface/metadata/oracle/binding | 各入口保留各自contract与独立closing evidence；无共同出口不强行合批 | 每适用子batch风险确定；signed Python oracle可none |
| B6 | T009最终source/runtime/tests/convergence | exactsource/artifact/loadeddeps/wholeaffectedsuite，fresh convergence | 所有适用dynamic rows结果明确 |
| B7 | T010完整matrix two rounds | 全case/subscenario、storage/wire/children/cleanup及reproduction | qualification；单轮/局部不升级 |

**Batch growth decision**: 本次只形成cohesive文档集与旧Spec停止记录；不吸收任何product patch/build/run。后续成员稳定出口即进入批末验证，不为少build继续吸收无关职责。
每批遵循canonical work-unit/static-review/batch-quality references；source审查记录actualqueries、reviewer path/SHA、完整delta、symbol definition map、artifactoutputhash、首失败/Changed gate、四类retrospective。尚未执行的build/dynamic/runtime为NOT_RUN。

## Project Structure

### Documentation (this feature)

`spec.md, gap-audit.md, plan.md, research.md, data-model.md, tasks.md, validation-matrix.md, traceability.md, supersession.md, quickstart.md, checklists/requirements.md, contracts/architecture-and-api.md, contracts/candidate-and-validation.md, review.md`。

### Source Code (repository root)

- Repo owner：`NDNSF-DistributedRepo/src/ArtifactRepositoryApi.cpp`、`RepoNode.cpp`、`LegacyRepoImporter.cpp`；既有types/protocol/stores和headers。
- Runtime/security：`ndn-service-framework/ServiceUser.*`及Core；只修实际违约owner，不为fixture改变授权/时间。
- Thin binding/DI：Repo pythonWrapper、`pythonWrapper/src/ndnsf/_ndnsf.cpp`、DI native/adapter；没有第二PythonDB。
- Regression：既有API/worker/import C++ fixtures及`tests/wscript`；test-only syscalls seam不成为生产环境开关。
- Network/oracle：`tests/minindn/spec193_packet_oracle.py`、`run_spec192_real_cpp_repo.py`、`spec193_preflight.py`；文件名带192/193不代表恢复旧Spec推进，也不为改名造成churn。

## Phases and Migration

1. B1先统一baseline/intent、检查特殊路径与parser compat；对未提交source按owner审阅，禁止broad staging build目录。
2. B2–B4依次修复两个HIGH与MEDIUM，各自本任务保留首次red和closingnativeoracle；publicABI/schema不改变。
3. B5关闭A19文档跨plane要求、A21 signed mapping oracle与完整OriginalData/object/Python/DI适用边界；existing已正确source不无故重写。
4. B6稳定共同source与每profile runtime composition；rebuild所有受影响static消费者包括nativePython模块，核对实际加载hash；完成所有受影响测试/dynamic和finalaudit。
5. B7同最终candidate完整two-roundmatrix/cross-run persistence，failed/unknowncase保留，最后完成公开文档与reproduction。

**Rollback**: 修复按现有owner显式commit可回退；不改schema无需数据降级。destination异常保留恢复材料；import retry不删除完整target。rollback不得丢弃先前用户source/历史证据。旧Spec停用记录保留，Spec202 pointer是唯一活动计划。

## Complexity Tracking

没有批准新publicAPI、全局事务框架、Python catalog、默认replication、observer或统计benchmark。局部destination rollback helper必须仅服务两个nativefetch entry；若实现要求新增ABI/schema或并发机制，先记录实际原因/替代方案并修订contract、审查再编码。
