# Implementation Plan: Prepared Model Runtime

**Branch**: Experimental | **Date**: 2026-09-12 | **Status**: COMPLETE / B0–B9 delivered; external qualification boundaries remain scoped to Spec184 and the recorded unobserved limits
**Authority**: [spec](spec.md) · [contracts](contracts/public-api.md) · [tasks](tasks.md) · [audit](audit.md)

## Constitution Check

沿用 C++17、ndn-cxx/Core、现有 native DI catalog/adapter/planner/Provider 和 Waf；
绑定为 pythonWrapper 下现有 pybind11 模块。DI公开领域包装；通用运行时、订阅和等待机制在Core实现，Core不引入模型语义，详见[C-09](contracts/core-app-boundary.md)。
目录名固定185，不建长期功能分支；实现、编译和实验按本执行表完成，未观测边界保留在对应 evidence 中。
所有新示例避免 C++20 designated initializer；使用构造后赋值。
源码基线见 audit，开始执行时读取最新 HEAD、dirty diff 和184资格证据，不能用本轮 hash 永久冻结实现。

## Dependency Gate

B1/B2可完成纯本地owner与模型准备；B3前须核对184的请求/授权/终态相关依赖并运行定向回归，
以同树候选记录结果。原有未完成184外部模型资格保留其owner，不把185局部通过标成父Spec完成。
同一源码文件被其他工作修改时重新审查受影响diff，不并发改同一owner。
第一次接入 Core 的行为门和最终资格明确保留，不将静态检查当运行结果。

## Logical Batch Quality Plan

执行细则见[Batch Execution Schedule](batch-execution.md)：01–12调度顺序、逐任务静态门、五lane、共享构建与受影响复测。保留原Batch ID以维持证据链接。

下表是默认拓扑顺序。依赖前项的任务只可在相应**逐任务静态门**通过后编码；无依赖、文件边界清晰且自身前置已满足的任务可在固定快照异步审查期间继续，调度记录遵循执行表和共享 Dependency-Scoped Dispatch；
跨批依赖要求前批声明出口的实际验证通过。每批稳定出口达到后立即验证，不继续吸收下一批职责。

| Batch | Units | Shared boundary | Planned C++ selector | Dynamic profile | Stable observable exit |
| --- | --- | --- | --- | --- | --- |
| B0 | T015 | Installed SDK/ABI | Spec185InstalledApi | asan | 安装prefix独立包含/链接/构造析构通过 |
| B0C | T017,T018 | Core runtime primitives and DI delegation | Spec185CoreOperation / Spec185DiCoreOperation | tsan | Core-only consumer可用，DI委托同一owner，既有协议回归通过 |
| B1 | T001,T002 | Runtime/config/owner lifecycle | Spec185Runtime | tsan | open → user → close/drain，无活动owner |
| B2E | T016 | Extension lifecycle/control | Spec185ExtensionRegistry | tsan | freeze、协作控制及runner隔离通过 |
| B2 | T003,T004 | Preparation catalog/cache/lease | Spec185Preparation | tsan | 8并发prepare单次生产者，失败不发布READY |
| B3 | T005,T006 | Prepared request/handle projection | Spec185PreparedRequest | asan-ubsan | 两次请求复用包，独立授权/终态 |
| B4 | T007,T008 | Conversation/recovery wrapper | Spec185Conversation | asan-ubsan | 两轮durable checkpoint与handle一致 |
| B5 | T009,T010 | Provider artifact/template lifecycle | Spec185ProviderAssembly | asan-ubsan | 认证后准备及安全复用，stop后lease=0 |
| B6 | T011 | Native callers/export | Spec185Compatibility | none | native进程入口与旧签名对照通过 |
| B7 | T013 | Complete C++ qualification | Spec185Process | asan-ubsan | 安装SDK和独立生产进程全部原生模式闭合 |
| B7R | T019,T020,T021 | Lifecycle and identity audit repairs | affected C++ request/provider selectors | asan-ubsan | 审计缺口修复有C++证据，未观测项不冒充资格完成 |
| B8 | T012 | Thin Python wrapper | wrapper-only checks | none | 基于B7完整原生资格的映射通过 |
| B9 | T014 | Design handoff | document checks | none | 源码/API/资格交付一致 |

B6/B8的none仅适用于不改变native行为的导出/薄封装；若引入owner或状态变化，先归入相应native批次并增加动态卡。
实际依赖为B0→B0C→B1；B0C具有C-09 PO-C1–C4五lane覆盖，逐任务只读静态门后做组合审查，再共享编译/测试。
Core头/ABI改变须重建受影响消费者并记录真实链接闭包；初始规划阶段原计划仅修订规划、不执行
native构建，实际 B0–B9 执行结果以 `tasks.md` 和各批次 evidence 为准。
B7只重跑受185改变的链路及最终process范围，不重复全部184历史实验。
B6–B9各自单任务有独立出口：不能要求先完成Python再运行其依赖的C++进程验证，也不能先交付再验收。

## Batch Growth Decision

每批分配依据为同一生产入口、共同契约、独立oracle、共享source closure和稳定出口。
B1 owner、B2 cache、B3 request、B4 conversation、B5 provider不能仅为少一次构建合并。
T001–T018及B7R的T019–T021均含对应测试编写、静态审查、证据更新；不拆出“写测试/跑测试/写报告”的行政任务。
批内新增任务必须先登记ID及出口；已有稳定出口不得继续扩张。

## Build and Validation Order

生产库实际名称 `ndnsf-distributed-inference`，root `wscript:543–623`；
单元/集成注册 `tests/wscript`；C++ examples `examples/wscript`。
原生测试fixture/driver/oracle均为C++，直接调用库或C++进程。
先 `C++ production → C++ unit/integration/process → Python wrapper checks`。
逐小任务只读review-agent → 批末五lane组合审查 → 共享构建和定向测试 → 动态卡。
参见 [C-04](contracts/validation.md)；selector 的注册与实际运行状态以 tasks/evidence 为准，不能用未注册名称或文档检查声称产品资格。

普通DI修改复用核验过的build tree，按实际target选 `-j4`，系统g++/binutils/Boost/NAC-ABE闭包；
观察vmstat，不启动竞争构建。ABI/共享头变化只重建必要transitive consumers；
sanitizer使用独立同ABI构建。记录target→实际binary→SHA，不猜historical build-system路径。
最终C++跨进程需独立authority、requester、provider，真实 ACK/Selection/execution/Response；
启动成功或CLI help不是行为PASS。MiniNDN/SIF/Tiger不因文档变更自动启动。

## Implementation Design Authority

[C-08](contracts/code-design.md)冻结CD/FIELD/FN/FLOW/PO；每任务Design binding引用该权威，不只给出公开API。
T016的cooperative接口/catalog字段先于Package构造，因此B2E排在B1之后、B2之前；不是新增批次，不能按数字自行排序。
实现前核对源码基线和关键签名，影响契约的缺口先修设计；普通局部实现为LOCAL_DETAIL。以readiness语义审查为准，不把结构检查当就绪。

## Native Public API Authority

[C-07全API清单](contracts/api-catalog.md)是签名/Python对应/生命周期完整性检查入口。当前执行队列为21任务、12个产品批次及B7R修复门；T019–T021保留审计发现的独立C++出口，不与Python或文档验收混合。
T015建立每行exposure及缺失项；T002/T004/T006/T009完成生命周期；T011完整C++例子，T013原生矩阵；T012验证直接binding及有限便利层；T014核对实际全部导出。

[C-05](contracts/api-usability.md)定义六层API、model key、能力、结果/错误、可靠流和扩展freeze；[C-06](contracts/cpp-first.md)定义独立C++入口及native异步owner。
执行顺序：T015 → T001,T002 → T016 → T003–T011 → T013 → T019,T020,T021 → T012 → T014。入口api.hpp/provider.hpp；Provider-only不要求User目录或requester私钥。配置、规划、状态机、恢复由C++提供，Python不得补缺。

## Migration and Rollback

新增facade additive接入，旧入口继续调用同一NativeInferenceClient，禁止隐式Python fallback。
T011生成当前维护caller矩阵，按共享后端/模式分组；不得机械照抄历史16调用点。
每行包含path/symbol、mode、owner、旧→新route、证据、retention/removal条件。
本Spec只移除已证明无维护调用方且有完整替代证据的façade专属compat shim；
Spec184尚未关闭的Python retirement不自动消失。
回退按批次本地checkpoint恢复调用入口选择，保留既有journal/wire格式，不迁移数据库或改写旧证据。

## Delivery

T014同步当前API/中文契约/源摘要及目标PLANNED→实际状态、三类图和双PDF，
记录未覆盖外部资格的准确owner；无完整验收不勾任务。仅提交显式paths，local checkpoint不push。
