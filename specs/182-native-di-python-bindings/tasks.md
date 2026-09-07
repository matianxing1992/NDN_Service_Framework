# Tasks: Native NDNSF-DI with Optional Python Bindings

**Revision**: 8 | **Status**: DRAFT / T001 IN_PROGRESS
**Input**: [spec](spec.md), [code design](contracts/code-design.md),
[proof](contracts/proof-design.md), [work units](contracts/work-units.md)

## Execution Progress

本表是所有执行者共同维护的**当前子任务进度唯一入口**；点击任务查看 Read、Write、Steps、Verify。
父任务清单保留阶段验收；Current Checkpoint 保存摘要与历史，不作为第二张状态表。
状态：NOT_STARTED（无独立执行记录）、READY（依赖及门禁满足）、IN_PROGRESS（正在执行）、
PARTIAL（已有工作但验收不全）、BLOCKED（已确认阻塞）、DONE（该卡完整验收通过）。
PARTIAL 不表示依赖放行；T001 release 及 plan Gate Order 继续约束执行。
本次按持久 checkpoint 保守登记，未逐卡重跑验收，不以文件存在或结构检查计算完成百分比。
每个工作单元成功/失败/阻塞后、commit 和回复前更新对应行及证据；新增工作先补卡和进度行。
维护规则见 [task progress](../../skills/speckit-code-design/references/task-progress.md)。

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001-A Identity and Dependency Closure](contracts/execution-units.md#t001-a-identity-and-dependency-closure) | PARTIAL | — | [baseline](evidence/task-progress-registry-20260907.md)；已有身份设计；完整关闭待核对 | 2026-09-07 |
| [T001-B Lifecycle and Capability Closure](contracts/execution-units.md#t001-b-lifecycle-and-capability-closure) | PARTIAL | — | [baseline](evidence/task-progress-registry-20260907.md)；已有生命周期设计；O-004 公开 API 映射仍 OPEN | 2026-09-07 |
| [T001-C Dispatch and Selector Freeze](contracts/execution-units.md#t001-c-dispatch-and-selector-freeze) | BLOCKED | T001-A, T001-B | [baseline](evidence/task-progress-registry-20260907.md)；T001-A/B 与 O-004 未关闭；selector release 待完成 | 2026-09-07 |
| [T002-A Installed Library Boundary](contracts/execution-units.md#t002-a-installed-library-boundary) | BLOCKED | T001-C | [baseline](evidence/task-progress-registry-20260907.md)；已有库/consumer 边界；NAC-ABE API 闭包阻塞构建 | 2026-09-07 |
| [T003-A Qwen Split Candidates](contracts/execution-units.md#t003-a-qwen-split-candidates) | PARTIAL | T002-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T003-B Yolo Split Candidates](contracts/execution-units.md#t003-b-yolo-split-candidates) | PARTIAL | T003-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T003-C Placement and Registry](contracts/execution-units.md#t003-c-placement-and-registry) | PARTIAL | T003-A, T003-B | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T004-A Canonical Plan Sealing](contracts/execution-units.md#t004-a-canonical-plan-sealing) | PARTIAL | T003-C | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T005-A InProcess Authority](contracts/execution-units.md#t005-a-inprocess-authority) | NOT_STARTED | T004-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T005-B Requester Grant Publication](contracts/execution-units.md#t005-b-requester-grant-publication) | PARTIAL | T005-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T006-A Canonical Source Identity](contracts/execution-units.md#t006-a-canonical-source-identity) | PARTIAL | T002-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T006-B Certified Extraction and Wire](contracts/execution-units.md#t006-b-certified-extraction-and-wire) | PARTIAL | T006-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T006-C Bounded Native Worker](contracts/execution-units.md#t006-c-bounded-native-worker) | NOT_STARTED | T006-B | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T006-D Protected Provider Activation](contracts/execution-units.md#t006-d-protected-provider-activation) | PARTIAL | T006-C | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T007-A Full Tokenizer Ownership](contracts/execution-units.md#t007-a-full-tokenizer-ownership) | PARTIAL | T002-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T007-B Stable Text Decoder Pair](contracts/execution-units.md#t007-b-stable-text-decoder-pair) | PARTIAL | T007-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T008-A Native Input and Artifact Preparation](contracts/execution-units.md#t008-a-native-input-and-artifact-preparation) | PARTIAL | T003-C, T006-D, T007-B | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T008-B Authenticated Offer Admission](contracts/execution-units.md#t008-b-authenticated-offer-admission) | NOT_STARTED | T008-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T009-A Core Scoped Registration](contracts/execution-units.md#t009-a-core-scoped-registration) | NOT_STARTED | T006-D, T007-B | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T009-B Shared Execution Lease State](contracts/execution-units.md#t009-b-shared-execution-lease-state) | NOT_STARTED | T009-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T009-C Shared Provider Host Wiring](contracts/execution-units.md#t009-c-shared-provider-host-wiring) | PARTIAL | T009-B | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T010-A Request Operation Terminal State](contracts/execution-units.md#t010-a-request-operation-terminal-state) | PARTIAL | T005-B, T008-B, T009-C | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T010-B Complete Request Orchestration](contracts/execution-units.md#t010-b-complete-request-orchestration) | PARTIAL | T010-A | [baseline](evidence/task-progress-registry-20260907.md)；request 仍返回 NATIVE_REQUEST_PIPELINE_NOT_READY；编排未接通 | 2026-09-07 |
| [T010-C Stream Acceptance and Replacement](contracts/execution-units.md#t010-c-stream-acceptance-and-replacement) | NOT_STARTED | T010-B | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T011-A Sampling Parity Repair](contracts/execution-units.md#t011-a-sampling-parity-repair) | PARTIAL | T010-C | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T011-B Stable Epoch Emission](contracts/execution-units.md#t011-b-stable-epoch-emission) | PARTIAL | T011-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T011-C Conversation Journal and Continuation](contracts/execution-units.md#t011-c-conversation-journal-and-continuation) | PARTIAL | T011-B | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T012-A Native Binding Types and Lifetime](contracts/execution-units.md#t012-a-native-binding-types-and-lifetime) | PARTIAL | T011-C | [baseline](evidence/task-progress-registry-20260907.md)；已有 Python focused 17/17 记录；native 验收未完成 | 2026-09-07 |
| [T012-B Compatible Python Facades](contracts/execution-units.md#t012-b-compatible-python-facades) | NOT_STARTED | T012-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T013-A Maintained Caller Migration](contracts/execution-units.md#t013-a-maintained-caller-migration) | NOT_STARTED | T012-B | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T013-B Legacy Runtime Retirement](contracts/execution-units.md#t013-b-legacy-runtime-retirement) | NOT_STARTED | T013-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T014-A Isolation Collector Semantics](contracts/execution-units.md#t014-a-isolation-collector-semantics) | PARTIAL | T013-B | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T014-B Qualification Harness Registration](contracts/execution-units.md#t014-b-qualification-harness-registration) | PARTIAL | T014-A | [baseline](evidence/task-progress-registry-20260907.md)；已有相关源码切片；完整卡验收未完成 | 2026-09-07 |
| [T015-A CrossTask Convergence](contracts/execution-units.md#t015-a-crosstask-convergence) | NOT_STARTED | T014-B | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T016-A Local Qualification](contracts/execution-units.md#t016-a-local-qualification) | NOT_STARTED | T015-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |
| [T017-A Development Handoff](contracts/execution-units.md#t017-a-development-handoff) | NOT_STARTED | T016-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |

## Current Checkpoint

2026-09-07 Native boundary follow-up / **T001 IN_PROGRESS**：补齐 ONNX
protobuf 生成源到 provider、fault-provider、assembly-parity、smoke 和
integration 的 Waf 闭包（`76d074af`），移除装配器废弃 JSON/路径 helper
（`e0a4e248`）；Python 扩展不再重复编译 `NativeGrantVerifier.cpp`，只链接
安装的 DI 库（`2eb259f7`）。嵌套 graph external initializer 的绑定/内联和
超大输入上限已由 `b4f02615` 覆盖。相关静态检查及 Spec182 Python 门禁仍
通过；Cargo 缺失、NAC-ABE ABI mismatch 和完整请求编排未解决，产品仍 **0/17**。
下一步在匹配工具链上完成 bridge/全量链接，再推进 T010/T011；不把本地
focused 结果写成 T016 qualification。

2026-09-07 Spec Kit default / **PASS**：通用 tasks-template 默认包含 Execution Progress 和
Current Checkpoint；任务生成/执行技能增加覆盖检查、增量更新及保留既有状态规则。
仅工作流文档更新，不改变上表产品状态。检查见 [registry evidence](evidence/task-progress-registry-20260907.md#spec-kit-default)。
下一步所有新任务清单沿此模板生成，Spec182 继续按现有 T001 缺口推进。

2026-09-07 Progress registry / **PASS**：36 个执行单元统一登记到上表，详细卡改为通用执行契约。
旧 Spark 路径保留历史入口；本次仅整理进度和规则，未实现或验收产品，不改变父任务勾选。
检查和状态来源见 [registry evidence](evidence/task-progress-registry-20260907.md)。
下一步关闭 T001 公开 API/设计 release 缺口，并处理已记录的构建依赖边界，再按 Gate Order 推进。

2026-09-07 Native component and binding implementation slice / **T001 IN_PROGRESS**：
已加入原生 ONNX recipe assembler（protobuf structural assembly、external
initializer digest binding、deterministic serialization）、grant issue/publish
ports、request preparation/admission ports、conversation journal coordinator、
Provider host registration seam，以及 `pythonWrapper` 的单一薄 pybind11 DI
入口。新增 C++ focused tests 均完成语法检查；Python binding/build-boundary
测试 **17/17 PASS**。绑定只映射 native DTO/错误/句柄/策略类型，`NativeServiceUser`
通过生命周期保持创建 native client，不在 Python 复制 planner 或状态机。
显式 `NDNSF_LIBRARY_DIR` 现在同时要求 Core 和 DI shared library，避免链接回退。
ONNX protobuf 生成源已纳入 Waf；全量 Waf/native qualification 仍受既有
NAC-ABE ABI mismatch 阻断，不能据此关闭 T002/T006/T009/T010 或 T012。
新增 installed-consumer Waf 目标（checkpoint `aba90194`），其公共头语法检查
通过；Spec182 相关 Python 门禁 **29/29 PASS**，设计校验 `errors=[]`。
Rust tokenizer bridge 的本机 release 构建在 `cargo: command not found`
（exit127）处未开始，原始边界见 `.codex-tmp/spec182-tokenizer-r1/`，不能
替代真实 bridge/ABI 验证。T001/O-004保持OPEN，产品任务仍 **0/17**；下一步
在可用同源 Cargo 工具链上完成 bridge/consumer 检查，并继续 T010/T011 的真实
request/stream 接线后再做 T015 静态收敛审查。

2026-09-07 Tokenizer identity guard / **T001 IN_PROGRESS**：修正
`NativeTokenizer` 在加载动态 bridge 前校验 tokenizer 文件摘要，并拒绝空输出
缓冲区；新增 3 个身份/工厂负例单测，独立 Boost.Test **3/3 PASS**（commit
`54595eee`）。Cargo 缺失仍使 Rust bridge 的真实构建保持 **NOT_RUN**，不关闭
T007/O-004。

2026-09-07 Public export inventory / **T001 IN_PROGRESS**：新增[API migration review](contracts/public-api-migration-review.md)和可复现AST snapshot，覆盖api27/sdk76/root174，共277导出，264定义/10assignment/3外部owner。发现正式api中23个名称尚无四份主契约的精确映射；部署catalog、请求handle和provenance不能由现有request概述替代。snapshot逐条UNREVIEWED，动态wildcard/继承/实例字段仍需核对；不是迁移完成。下一步逐行为完成正式api映射及动态层清单，O-004/T001保持OPEN，产品0/17。未修改产品源码、未运行native产品测试。

### Prior Provider Lifetime

上述inventory单元277个导出键无重复，36个定义文件SHA256与当前源码一致；strict structure、design validator（214本地链接）及diff whitespace检查PASS。AST扫描不导入运行依赖，分namespace生成均exit0；初次全集工具输出截断后改为分namespace读取并合并，未把截断结果作为snapshot。

2026-09-07 Spark execution preparation / **DISPATCH_DESIGNED**：用户指定Spark为后续实现执行者。
已增加[execution cards](contracts/spark-execution.md)，保留17个父任务，补齐定向Read、精确Write、步骤、依赖和planned selector；
共享设计技能及本机tasks/implement入口支持bounded-executor。T001设计关闭与selector release仍是产品前置门，
本轮不替其他设计工作宣告关闭O项，也不勾选任何产品任务。36卡/17父任务覆盖、依赖、211链接、strict structure及三项validator拒绝反例PASS；
技能通用校验器与既有Spec Kit metadata的schema差异及替代检查见[spark preparation](evidence/spark-execution-preparation.md)。
下一步由T001-C汇总已有设计关闭证据、冻结实际选择器和构建入口，再从T002-A按依赖交给Spark执行。
**Spark trial NOT_RUN；产品仍0/17。**下方Provider等记录保留各自设计范围与历史验证事实。

### Spark Checkpoint

历史准备见 [preparation checks](evidence/spark-execution-preparation.md)。
当前状态统一到 [Execution Progress](#execution-progress)，此处不再维护第二张表。

### Prior Provider Lifetime Design

2026-09-07 Provider lifetime design / **T001 IN_PROGRESS**：在[lifecycle contract](contracts/native-provider-lifecycle-design.md#provider-lifetime-control)定义受mutex保护的RegistrationControl、close/post/析构互斥、锁外释放capture及post失败后续清理；补齐ACK同步/异步、Selection、普通lease执行fallback、完成发布检查。源码确认pending cleanup可能早于兄弟role结束，因此Selection将代次转入协作生命周期，work fence不依赖pending表。M47同步指向新增Core scoped接口；不是包装不存在的unregister。Provider生命周期设计范围已明确，O-004其余公开类型/调用及整体T001仍未完成，产品0/17。下一步归并设计清单并核对剩余缺项，不重启已闭合算法研究。

### Prior Registration Generation

上述lifetime设计单元strict structure、design validator（186本地链接）及diff whitespace检查PASS。未构建/运行native产品；新增生命周期单测仍NOT_RUN，文档检查不计T009完成。

2026-09-07 Registration generation / **T001 IN_PROGRESS**：源码确认ACK复制旧handler异步执行、Selection重新查当前service handler，单独DI closed包装不能隔离重注册旧请求。已在[lifecycle contract](contracts/native-provider-lifecycle-design.md#registration-generation-decision)定义Core scoped registration、pending代次绑定、ACK完成/Selection/work fence检查及幂等close清理范围；不增加wire或授权owner，不删除legacy API。尚需补齐Provider存活控制与同步fallback位置，T009仍BLOCK、T001/O-004未完成、产品0/17。本轮无native产品构建/测试；下一步沿新增Core范围完成可实施设计，不重新讨论已确定的代次机制。

### Prior Shared Lease Design

上述registration设计单元strict structure、design validator（183本地链接）和diff whitespace检查PASS；只支持文档checkpoint，不表示Core扩展已实现或生命周期测试通过。

2026-09-07 Shared Provider lease design / **T001 IN_PROGRESS**：核对Core addService/addCollaborationHandler会覆盖同名handler，且没有公开unregister；当前单服务CLI为固定lease入口创建单target私有表，不能直接逐serve复制。新增[Provider lifecycle contract](contracts/native-provider-lifecycle-design.md)，定义一个host共享lease表/prepare mutex、单入口target路由、跨target绑定验证、关闭期间只清理旧lease及精确类/字段/constructor迁移。此为拟议多服务集成风险的源码推导，没有运行native产品；现有单服务结果不被改判失败。

本单元strict structure、design validator（182本地链接）及diff whitespace检查PASS；无产品构建/测试。T001/O-004及T009仍未完成：下一步关闭registration generation、晚到ACK/Selection及Core入口生命周期，再完成其余公开类型清单。产品0/17，SIF/Tiger不介入。

### Prior Stream Caller Closure

2026-09-07 Stream caller/recovery closure / **T001 IN_PROGRESS**：补齐[token stream contract](contracts/native-token-stream-design.md#production-caller-inventory)的paired factory、认证摘要绑定、唯一生产CLI注入点及三个integration consumers；定义NativeInferenceOperation的accept/replacement/final检查。源码确认现有AutomaticStreamingHandle接受前缀只在内存，runtime journal不在逐token接受链；保留进程内replacement与已提交conversation恢复各自边界，禁止终止token后重启生成，新增final text一致性义务。A7-08相关设计已定义，产品修复/测试仍OPEN；O-004其余公开API/schema/Provider注册生命周期仍需关闭，T001未完成、产品0/17。

本轮仅源码核对与契约修订，没有native构建或运行测试。strict structure、design validator（180本地链接）及diff whitespace检查PASS。下一步统一收口O-004剩余注册生命周期及完整公开类型/调用清单，避免再重复已关闭的tokenizer算法问题。

### Prior Qwen Stream Design

2026-09-07 Qwen stream design / **T001 IN_PROGRESS**：读取交付清单固定revision的tokenizer.json，12,807,982 bytes及SHA256完全匹配，确认ByteLevel decoder；本地另一Qwen工件身份单独记录，不混用。新增[token stream design](contracts/native-token-stream-design.md)，定义stable API/第六私有ABI、所有权、ByteLevel与ByteFallback不同算法、终止flush及epoch候选/提交接线。A7-08剩余完整调用方和journal接受边界仍OPEN，T001未完成、产品0/17。

独立`/usr/bin/python3 tests/fixtures/spec182/dependency-probes/check-bytelevel-stream.py`实际exit0：65,536个two-byte序列、9个长/非法/截断序列和3个whole-token fallback检查PASS；Python标准库增量UTF-8结果与固定HF0.20.3完整decode对照。strict structure、design validator（178本地链接）和diff whitespace检查PASS。仅reference诊断，不执行新增ABI/native产品。下一步关闭事件接受/恢复与全部factory调用方，随后统一收口O-004。

### Prior Stream Boundary

2026-09-07 Stream boundary / **T001 IN_PROGRESS**：新增可移植[reference checker](../../tests/fixtures/spec182/dependency-probes/check-stream-boundaries.py)，固定tokenizers0.20.3及既有fixture哈希，7个边界输入实际PASS/exit0。确认完整UTF-8的byte run仍会被后续无效byte改写；合法U+FFFD不能删除，skip special不构成run边界。算法与新版HF原生stream能力比较写入[generation contract](contracts/native-generation-design.md#verified-boundary-and-planned-bytefallback-algorithm)。本轮没有native产品构建/测试。

A7-08 的本轮收口原则：完整decode仅用于完整文本验收，不可替代stream边界；`decodeStable`与`eventSink`恢复边界仍是T007/T011/O-004闭环项。

A7-08仍OPEN：ByteFallback适配选择已明确，但真实Qwen decoder pipeline、完整调用/字段与事件接受后失败的恢复边界尚未关闭。源码确认eventSink接受后仍执行反馈发布与runtime commit，不能承诺靠decoder局部rollback撤回事件。T001未完成、产品0/17；下一步从实际Qwen工件和既有journal/commit路径关闭这些剩余设计，不重跑已固定reference用例。本单元strict structure、design validator（173本地链接）及diff whitespace检查PASS；参考运行命令为`/usr/bin/python3 tests/fixtures/spec182/dependency-probes/check-stream-boundaries.py`。

### Prior Generation Design

2026-09-07 Generation design / **T001 IN_PROGRESS**：[native generation contract](contracts/native-generation-design.md)补齐GenAI/HF/ORT复用比较与现有sampler的参数、double精度、去重惩罚、截断后归一化及旧会话兼容处置。A7-10 已按复用边界清单闭环：GenAI仅作能力对照，不作为默认生产路径；A7-11 CLOSED。A7-08 的完整decode/stream边界与A7-09 的采样数学均为设计约束，不等于实现/通过；A7-08/A7-09与O-004仍OPEN，T001未完成、产品0/17。修订plan旧授权句；不新增生成引擎或产品依赖，不操作实验机器。

本单元检查 **PASS**：prerequisites、strict structure、design validator和diff whitespace。独立Python reference源SHA256 `3affb6f438a4134bb8e69222d79b3f2ec5a6b256bf0022af636b065627397c11`；将log概率按`struct.pack/unpack('<f')`量化后输入`[-0.5108256340026855,-1.2039728164672852,-2.3025851249694824]`，seed8/top_k3/top_p0.8/temperature1/step0实际选0；重复惩罚例也实际选0，assert通过/exit0。仅运行标准库reference，不构建native或运行产品测试。已有ONNX normalization草稿与failure-log修改保留，不在本单元宣称O-002关闭。

下一步关闭stream decoder的preview/commit/flush/restore算法和逐调用方清单，再完成O-004及T001。下方审计检查点为历史事实，不覆盖本段状态。

### Prior Native Reuse Review

2026-09-06 Native reuse review / **BLOCK for implementation**：核对原生库复用与当前源码，ONNX/ORT及HF Rust tokenizer方向合理；新增A7-08流式decode前缀不稳定、A7-09已有C++ Top-P归一化及重复惩罚与Python reference不一致，A7-10缺GenAI复用对照、A7-11计划旧授权语句。详见[native reuse review](evidence/native-reuse-review-20260906.md)。reference诊断exit0：多byte-token文本展示prefix重写，两个采样输入Python实际返回0而native源逻辑推导为1；未运行native产品。raw `.codex-tmp/spec182-native-reuse-review-20260906-r1/`。T001/O-002/O-004保持OPEN、产品0/17；下一步在T001冻结复用/stream/采样兼容处置，再由T007/T011/T016实现和证明。本轮仅审计记录，不改产品源码或既有oracle。

本审计记录检查 **PASS**：Spec Kit prerequisites、strict structure、design validator（163本地链接、17任务/0完成）、`git diff --check`；reference诊断exit0及具体结果见同一review/raw。这些只允许保存审计记录，不关闭A7-08/A7-09、O-002/O-004或T001。

checkpoint首轮被本地pre-commit全索引引用检查拒绝（exit1）；已核对hook提供`NDNSF_LOCAL_CHECKPOINT=1`专用模式，后续本地提交使用该模式且保留禁止路径检查，原始`commit-r1.json`不覆盖。未修改hook、未push；其他工作正在追加的typed-complex诊断不纳入本审计提交。

### Prior ONNX Contract Checkpoint

2026-09-06 T001 ONNX contract / IN_PROGRESS：补齐[native ONNX assembly design](contracts/native-onnx-assembly-design.md)的owned类型、9个函数、8步算法、recipe/manifest字节与native worker生命周期；修复直接进程内替换会丢失硬超时回收的设计缺口。原始Python实现仍未修改；独立reference提取24个普通numeric raw/typed向量PASS。额外诊断确认BFLOAT16 raw摘要内容错误、STRING跨进程摘要不稳定，见[identity evidence](evidence/identity-reference-20260906.json)及failure index。**O-002/O-004仍OPEN，T001未完成，产品实现0/17**。下一步冻结这两种表示的稳定身份/兼容处置，并继续完整能力和Provider注册清单；不复制错误oracle、不以I/O dtype代替initializer能力范围。

本单元检查 **PASS**：strict structure、design validator（160本地链接、17任务/0完成）、diff whitespace；24个model hash、12对identity、6条诊断及未修改reference源hash核对一致。generator锁定原onnx/numpy/source版本，补锁后typed诊断仍通过。Context Mode健康检查PASS，但relevance检索返回了较早的Dependency Design子节，checkpoint以实际tasks顶部为准；CodeGraph的临时副本结果仍剔除，算法按精确生产路径核对。未构建/测试产品，不把发现旧缺陷或补全设计计作T006完成。

### Isolation Design Checkpoint

2026-09-06 T001 isolation design：**O-005 CLOSED**。已冻结[native isolation design](contracts/native-isolation-design.md)的最小root/namespace、权限与服务白名单、逐进程exec/映射/endpoint观测、harness函数/字段和I01--I08反例。bwrap0.4.0/strace5.5工具正例exit0且CapEff=0/NoNewPrivs=1，缺解释器反例在exec边界ENOENT/exit1，均符合预期；raw `.codex-tmp/spec182-t001-isolation-r1/`。没有运行NDNSF、MiniNDN、SIF/Tiger，也未实现T014 detector。当前O-001/O-003/O-005按各自设计范围CLOSED，**O-002/O-004仍OPEN，T001仍未完成**；下一步补完整ONNX算法和迁移/注册/状态清单。

本隔离设计单元检查 **PASS**：strict structure、design validator（149本地链接、17任务/0完成）、`git diff --check`。已静态核对工具选项、namespace/文件根与外部harness分界；最小工具正反例不外推NFD/Repo/Controller或全部后代观测的运行证明。此前依赖设计单元已本地提交`5187e733`，tracked tree随该单元清理，原始构建/日志未入Git；未push。

### Dependency Design Checkpoint

2026-09-06 T001 / IN_PROGRESS：用户已授权在Experimental完成Spec182，按原目标实施与本地验收；上一轮只审计的范围已结束。先关闭O-002--005，不提前迁移业务或执行最终集成/MiniNDN。已补齐tokenizer特殊token参数，建立[依赖设计与探针契约](contracts/native-dependency-design.md)。T001保持未勾选；SIF/Tiger继续外部负责。

T001运行边界：ONNX1.17.0原生依赖-j2构建及四向量探针 **PASS / exit0**，四个模型字节与原Spec181 oracle完全一致。tokenizers0.20.3/Rust1.90.0静态ABI与C++consumer构建 **PASS**，84个完整ids/text对照和14个拒绝检查 **PASS**；两探针ldd均无Python。ABI、生产桥接路径/字段/所有权、许可及依赖锁已冻结，**O-003 CLOSED**。O-002的完整算法/叶子契约、O-004完整兼容与状态设计、O-005隔离方案仍OPEN；T001和T007未完成。Rust工具链R1/R2下载TLS失败在R3更换HTTPS实现后恢复。详见同一依赖设计记录及failure index；不计产品任务完成。

本依赖设计单元检查 **PASS**：prerequisites、strict structure、design validator（143本地链接、17任务/0完成）、`git diff --check`；lock与两个oracle及三个tokenizer JSON的hash一致，Cargo.lock中72个registry依赖均有精确checksum。probe静态审查已在运行前完成，输入/expected来自旧版本，未改变产品源或历史oracle。恢复时Context Mode提供的`probe.cpp write` timeline查询未通过高熵identifier/source guard，改用持久tasks/contracts与实际日志核对，不从被拒绝的session检索推断状态。下一步补O-002/O-004/O-005；不重跑已通过且输入未变的探针。

### Prior Audit Checkpoint

2026-09-06 revision 7 / SOURCE_ALIGNMENT_COMPLETE：用户确认另一台机器已接收，本轮只审计/修订Spec182。以Experimental `81e251a4ef1d8e6a394dc5f0c38bc44e44bfc973`核对代码，修正未提交合并/旧integration失败的过时表述；固定NAC/SVS/NDNSD身份与旧运行证据失效范围。O-001按源码身份和181承接范围CLOSED；O-002--005仍OPEN，T001未勾选，实现仍 **0/17**。当前SVS/NDNSD组合 **UNQUALIFIED**，接收不等于实验通过。审计发现与关闭责任见[audit](audit.md)，具体版本见[integrated baseline](contracts/integrated-baseline.md)。

本轮未构建或执行产品unit/integration/MiniNDN/SIF/Tiger，未管理接收机器，未恢复已停止的ABI构建。下一步只继续T001的ONNX/tokenizer依赖、兼容/字段/注册寿命及隔离设计关闭；后续182开发验证仍按下方任务分工，上一轮delivery-only移交不取消T016义务。

本轮实际文档检查 **PASS**：Spec Kit prerequisites、strict structure、`checklists/validate_design.py`（136链接、17任务/0完成、依赖无环、19 FR/11 SC/14 CD/16 PO）、`git diff --check`；12类137字段AST名称/类型/默认值一致，19 FR/11 SC/16 PO/14负例/48方法/137字段条目相对审计输入未删改。以上不计产品验收；O-002--005保持OPEN。改动仅12份Spec182文档，历史evidence及产品源码不变。

## Historical Checkpoints

以下为各时点事实；其旧“下一步”、失败、未push及验证结果均不覆盖上方当前checkpoint。旧结果只适用于当时身份。

2026-09-06 Source Handoff COMPLETE / SOURCE_READY：用户明确本轮只交付，编译与测试在另一台机器执行。本机额外构建已停止并确认无遗留编译进程；后续NDNSD/全部ABI消费者构建、unit/integration、两wrapper验证、MiniNDN、SIF/Tiger全部TRANSFERRED，不再作为本机交付条件。D001范围修订为固定版本及ABI要求移交，D001--D004已完成；四库Experimental、可迁移输入/模板与root skills已发布。已有检查与中断事实保留，不冒称完整验证PASS。接收步骤及证据见 [source handoff](../../Experiments/TigerCluster/docs/source-handoff.md)。Spec182仍0/17，不计T001--T017完成。

2026-09-06 Existing Presentation Checkpoint：补存此前未跟踪的`docs/NDNSF-UAV/slides/UPDATES.tex`及对应4页PDF；标题和全部命名frame的PDF文本核对PASS。旧版`UPDATES_UAV.pdf`、LaTeX缓存、原始实验输出及本地助手状态继续保留为本地产物，不计源码或182进度。活动指针/managed plan均指182，最终索引刷新后project与active健康检查PASS。

2026-09-06 Branch Closure：`Experimental` 已快进至整合提交 `e91ecc91`，本地只保留 `main`、`Experimental`；原生验证工作树改为detached并保留二进制/raw，临时分支已删除。主工作区原426项源码/文档状态保存在具名恢复stash `4bb5e0a5`，实际成果已归并，不能直接pop旧测试覆盖修复。未push。另将用户既有UAV slides源文件/PDF单独checkpoint：30个frame与30页PDF、标题/日期和两个新增Geo-Capture页的文本对应检查PASS；这是既有演示文档保存，不计182产品验收。

2026-09-06 Experimental Consolidation：原生合并修复已成为真实merge commit `c770f18bb7bf42c3b8a8274b5c029b4883141f60`，包含远端UAV历史；同步本机 `2e7865c7` 的revision6和Tiger目录布局。最终工作分支统一为Experimental，main保持稳定基线。原生生产代码与已验证基线逐字节一致；unit **759/759**、integration **154/154**、current Python **2171 passed / 22 skipped**、三个真实MiniNDN授权/撤销场景 **PASS**，见 [integration closure](evidence/integration-20260906.md)。此前152/154是已修复的历史失败，不再控制合并状态。

新增 [integrated baseline](contracts/integrated-baseline.md) 固定Core/UAV复用接口、生命周期修复与181承接。O-001已提供实际commit/证据/承接表，T001仍须核对最终Experimental差异并关闭O-002--005；182实现仍 **0/17**，其最终产品验收 **NOT_RUN**。不再独立续跑181最终资格，下一步是T001设计关闭。

2026-09-06 revision 6：合并重复审查与报告；实现任务只做静态审查、相关unit及必要构建，集成与MiniNDN在全部实现后由T016统一运行。类/方法/字段设计和既定真实运行用例保留。

本轮只改技能与文档；实现 **0/17**，产品STATIC_REVIEW与unit/integration/MiniNDN均 **NOT_RUN**。
文档结构、107链接、依赖及技能引用检查PASS；既定PO-001--014/负例/运行用例与符号字段表的保留检查PASS。范围见 [workflow simplification](evidence/workflow-simplification.md)。
O-001--005和合并修复状态未被本轮关闭。下一步执行T001确认基线与设计就绪。

## Tiger Directory Checkpoint

2026-09-06 Consolidation Review Closure：当前Tiger源码一并归入Experimental。静态复审修复ACK/选择/请求关联、原生错根拒绝证据组合、有限应用进程组清理；新增负例先复现19项失败，修复后新工具目录 **58/58 PASS**，旧目录兼容及关联工具 **162 passed / 3 skipped**。B001/B002开发检查更新，B003实际SIF/双节点/复用验收仍未完成，Local R8 FAIL保留。用户已停止实验，本轮未运行SIF/Tiger；详见 [baseline checkpoint](../../Experiments/TigerCluster/docs/two-node-baseline.md#usage)。

2026-09-06 Two-node baseline IN_PROGRESS：新增[基础实验契约与进度](../../Experiments/TigerCluster/docs/two-node-baseline.md)，独立B001--B003负责共享runtime、双节点profile、真实NDN/NDNSF探针及实际运行复用；不改变Spec182原生迁移任务状态。B001/B002实现与静态审查完成，相关unit **24/24 PASS**；本地/远端SIF哈希一致。B003尚待精确SIF集成与实际双节点/复用运行，尚无实验PASS。

2026-09-06 Usage Guidance：本地工作约定已补充Tiger唯一入口、共享owner、旧路径同步和双机分工；可随Git交付的说明见[Tiger README](../../Experiments/TigerCluster/README.md#compatibility-and-review-boundary)。路径/链接及文档一致性检查PASS；本轮只改说明，不运行产品测试、不改变实现进度或验收状态。

2026-09-06 Follow-up R2：64个迁移文件的哈希/权限/旧新路径、29个共享文件与审查工作树对比、Tiger文档本地链接均PASS，无新增同步差异。两份已同步测试未变，沿用R1的58/58工具单测证据，本轮不重复执行。审查工作树完整integration日志为152/154 PASS、2 failed；整合未完成，目录迁移无需追加修改，等待该owner交付最终基线。详见 [follow-up evidence](evidence/tiger-directory-migration-20260906.md#follow-up-r2)。

2026-09-06 Review Sync R1：64个迁移文件及共享lib/bin无新差异；两份测试修正已从审查工作树同步，相关工具/collector单测 **58/58 PASS，exit0**，关闭上轮原因码断言失败。源码迁移与原生实现状态不变；证据见下方migration evidence。

2026-09-06：Tiger目录迁移完成，64文件内容/权限与路径静态检查PASS；工具单测50 PASS / 1 FAIL，独立原布局已复现相同原因码失败，留给原工具owner。纯路径checkpoint保留49个已跟踪文件原HEAD内容，已有修改及15个未跟踪文件在新路径继续保留，不混入迁移提交。详见 [migration evidence](evidence/tiger-directory-migration-20260906.md)。本工作不计T001--T017实现或产品验收。

## Validation Standard

唯一规则见 [validation workflow](contracts/pre-test-static-review.md)。
T002--T014的任务[x]仅代表本任务实现、静态审查、相关单测和必要构建完成；
其Proof行引用完整行为义务，跨组件/跨进程与MiniNDN证明登记给T016，不要求各任务提前运行。
T015在全部实现后补审整体接线；T016执行完整unit→integration→MiniNDN并关闭全部必需PO。
不得将真实集成重命名为unit/smoke提前执行。Static review PASS != Behavior PASS。
每任务只保留简短结果或一份evidence链接，最终核对diff与证据，不另建S0/S1报告。

## Phase 1: Design and Native Components

- [ ] T001 [US5] **Successor Baseline and Design Closure**。冻结合并基线与181承接表、所有 schema/公开调用方/能力清单及原生依赖，关闭 O-001--005；修订叶子签名与任务至可执行。Dependencies: Merged baseline closure and Spec181 handoff。
  Design: FR-015,FR-016,FR-017,FR-018; CD-001--014。Proof: PO-012。
  [T001 contract](contracts/work-units.md#t001-successor-baseline-and-design-closure)。

- [ ] T002 [US1] **Installable Native Library Contract**。existing Provider runtime 可独立安装/链接；planned requester 公开声明先冻结，完整request由T010实现、T016运行验收。Dependencies: T001。
  Design: FR-001,FR-012; CD-001,CD-009。Proof: PO-001。
  [T002 contract](contracts/work-units.md#t002-installable-native-library-contract)。

- [ ] T003 [US2] **Native Split and Placement Decisions**。两个原生模型 splitter 与默认 placement 对固定输入生成合法且确定的方案。Dependencies: T002。
  Design: FR-003,FR-009,FR-016; CD-002。Proof: PO-002。
  [T003 contract](contracts/work-units.md#t003-native-split-and-placement-decisions)。

- [ ] T004 [US1] **Canonical Native Plan Sealing**。合法 proposal 转成可被真实 Core/Provider 接受的规范计划；非法投影在首边界拒绝。Dependencies: T003。
  Design: FR-002,FR-004; CD-003。Proof: PO-003。
  [T004 contract](contracts/work-units.md#t004-canonical-native-plan-sealing)。

- [ ] T005 [US1] **Native Requester Grant Path**。原生 requester 签名/申请/发布 grant，实际 Provider 验证并消费密钥。Dependencies: T004。
  Design: FR-005; CD-004。Proof: PO-004。
  [T005 contract](contracts/work-units.md#t005-native-requester-grant-path)。

- [ ] T006 [US1] **Native Cold ONNX Assembly**。Selection后原生装配与既有固定bytes一致；原生worker保留有界取消/清理，删除Python helper及其文件IPC。Dependencies: T002；O-002 closed。
  Design: FR-006,FR-016; CD-005。Proof: PO-005。
  [T006 contract](contracts/work-units.md#t006-native-cold-onnx-assembly)。

- [ ] T007 [US4] **Native Tokenizer Execution**。原生 encode/decode 完整文本，与固定 tokenizer oracle 一致，无子进程解释器。Dependencies: T002；O-003 closed。
  Design: FR-007; CD-006。Proof: PO-006。
  [T007 contract](contracts/work-units.md#t007-native-tokenizer-execution)。

- [ ] T008 [US1] **Native Request Preparation and Admission**。原生输入/认证模型/工件准备与 offer policy 校验闭合，GraphAdapter/TaskAdapter 端口由 native 实现。Dependencies: T003/T006/T007。
  Design: FR-001,FR-002,FR-004,FR-009,FR-016; CD-013。Proof: PO-013。
  [T008 contract](contracts/work-units.md#t008-native-request-preparation-and-admission)。

- [ ] T009 [US3] **Shared Native Provider Host**。CLI/C++/Python 共用服务注册、准备/执行接线与停止语义。Dependencies: T006/T007。
  Design: FR-001,FR-009,FR-010,FR-012; CD-014。Proof: PO-014。
  [T009 contract](contracts/work-units.md#t009-shared-native-provider-host)。

## Phase 2: Invocation and Compatibility

- [ ] T010 [US1] **Complete Native Request Lifecycle**。独立 C++ requester 从模型/输入到真实 Response，cancel/deadline/late callbacks 保持单一终态。Dependencies: T003/T004/T005/T006/T007/T008/T009。
  Design: FR-001,FR-002,FR-008; CD-001,CD-013,CD-014。Proof: PO-001,PO-003,PO-007,PO-013,PO-014。
  [T010 contract](contracts/work-units.md#t010-complete-native-request-lifecycle)。

- [ ] T011 [US4] **Native Conversation Continuation**。原生 requester 续接/有限恢复与既有 epoch/state runtime 协作，文本/lineage 正确。Dependencies: T010。
  Design: FR-008,FR-016; CD-007。Proof: PO-007,PO-008。
  [T011 contract](contracts/work-units.md#t011-native-conversation-continuation)。

- [ ] T012 [US3] **Thin Python Native Bindings**。支持的 Python 调用转发同一 native 库，无 Python strategy trampoline/业务状态机。Dependencies: T011。
  Design: FR-010; CD-008,CD-009。Proof: PO-009。
  [T012 contract](contracts/work-units.md#t012-thin-python-native-bindings)。

- [ ] T013 [US3] **Default Route and Legacy Retirement**。所有 maintained callers 默认原生；旧运行时退出默认 import/调用图。Dependencies: T012。
  Design: FR-011,FR-016; CD-010。Proof: PO-010。
  [T013 contract](contracts/work-units.md#t013-default-route-and-legacy-retirement)。

## Phase 3: Proof and Delivery

- [ ] T014 [US5] **Runtime Dependency Exclusion Gate**。harness 将被测 native scope 与 Python harness 隔离，能拒绝已知 interpreter/libpython/helper 旁路；交付正式 MiniNDN harness/collector 并完成本地单测；真实隔离反例在T016运行。Dependencies: T013。
  Design: FR-001,FR-011,FR-012,FR-014; CD-011。Proof: PO-001,PO-010,PO-012。
  [T014 contract](contracts/work-units.md#t014-runtime-dependency-exclusion-gate)。

- [ ] T015 [US5] **Design-code Convergence Audit**。整体静态读码核对FR/CD/INV/PO、生产接线、test/oracle/harness与依赖身份，控制性发现清零；记录整体审查结论并进入T016。Dependencies: T014。
  Design: FR-013,FR-017,FR-018; CD-001--014。Proof: PO-001--016。
  [T015 contract](contracts/work-units.md#t015-design-code-convergence-audit)。

- [ ] T016 [US5] **Local Native Qualification**。全部实现和T015审查完成后，同源完整unit→integration→YOLO/Qwen MiniNDN/no-Python及必要检错证明通过，核对最终diff与证据。Dependencies: T015 PASS。
  Design: FR-001,FR-005,FR-006,FR-007,FR-008,FR-010,FR-011,FR-012,FR-013,FR-016; CD-011。Proof: PO-001--016。
  [T016 contract](contracts/work-units.md#t016-local-native-qualification)。

- [ ] T017 [US5] **Native Development Handoff**。唯一开发交付版本、维护文档与两个入口示例；外部实验单列 TRANSFERRED。Dependencies: T016 PASS。
  Design: FR-014,FR-015; CD-012。Proof: PO-012。
  [T017 contract](contracts/work-units.md#t017-native-development-handoff)。

## Dependencies & Execution Order

Merged baseline closure and Spec181 handoff → T001 → T002；T003/T004/T005 依次收口；
T006/T007 依赖 T002 和已关闭 native dependency design；
T003/T006/T007 → T008；T006/T007 → T009；
T003--009 → T010 → T011 → T012 → T013 → T014 → T015 PASS → T016 → T017。
所有任务遵循FR-018/019和统一验证规则；每任务具体范围见work-units。只有最早未关闭门可进入其对应实施。每次失败先保留新 raw/evidence、更新本 tasks/failure index。
最终 checkpoint 前核对 task 状态与实际 diff/PO；不得 blanket stage 预存修改。
