# Implementation Plan: Multi-turn Token Generation Latency

**Branch**: `Experimental` | **Date**: 2026-09-22 | **Spec**: [spec.md](spec.md)
**Status**: PLANNED — 文档规划，不是已实现优化。

## Summary

优先消除每轮60秒ACK固定窗口，再打通FINALIZE及时收尾、同handle多轮/实时token，最后复用CPU已加载session。
分段测量先行，不把总时长当decode时间，不引入自适应调度/新模型/通用Repo重构。
新增核心范围：stage传输预算、每节点固定Repo根、重启恢复、查询命中免重复发布及受保护缓存安全接线。
现有r260基线、已修复70→6次runner准备、r259 affinity/KV均保留，详见[research](research.md)。
Spec189待做的跨请求驻留转入本Spec T007，旧Spec资格不关闭。

在 T003 的 live-turn 实现之前增加一个同任务前置门：只验证固定摘要的预构建
weight-only INT8 ONNX 是否能沿现有 `prepare → publication → Repo → Selection →
assembly → ORT` 契约运行。该门不做本地量化、不把 `precision=float32` 重命名为“全 INT8”，
也不为尚未证明的 IO/KV mismatch 添加 adapter；失败时停在 T003 并保留候选为
`UNQUALIFIED`。

## Technical Context

- Language/Version: C++17；Python仅MiniNDN/进程/资源编排。
- Primary Dependencies: 已安装NDN-CXX/Core、NAC-ABE、ONNX full protobuf、ORT CPU、tokenizer bridge；不新增外部库。
- Target Platform: 当前Linux开发机，两个MiniNDN执行节点ucla/arizona，Qwen3-0.6B；保留CPU缓存诊断，新增真实Repo-enabled范围。
- Storage: 复用FilesystemRepoStoreBackend和固定内容缓存；每deployment/node单owner持久根，run工作区/日志/临时staging分离；密钥不进入Repo元数据或Git。
- Testing: C++ `spec190-latency-tests`、`spec190-multiturn-oracle`（planned），现有spec189原生回归；真实MiniNDN最终验证。
- Constraints: 系统安装闭包；受影响目标增量-j4；Core/公共头变更时重编真实消费者，不凭批号重建全树。
- Performance Goals: spec SC-001–009；1000ms ACK目标、实时首token、三轮中位端到端降低至少50%，合法热缓存零材料payload、重启不重复STORE。

## Constitution Check

I/D1：原生Conversation/既有协议权威；II/G3：授权/验证/Selection不变；III：CodeGraph后核对源；
IV/VI：每task一个行为出口，测试/实现/证据不拆行政任务；V：匹配真实MiniNDN，至少60秒累计真实请求观察；
VII：候选冻结及拒绝零启动；VIII：T010收敛PASS后才运行T011。G4/D3是驻留及drain硬约束。
G1–G6、C1–C3、D1–D4、R1–R6、B1–B3、M1–M3映射[design contract](contracts/design.md)与[material contract](contracts/material-reuse.md)。
无原则豁免，当前/目标PDF不因本计划自动刷新为实现事实。

## Architecture Decisions and Ownership

| Owner | Responsibility | Must not own |
| --- | --- | --- |
| Core ServiceUser/OperationRuntime | ACK时间、认证集合、通用收束/传输 | 模型/role可行性、KV或ONNX缓存 |
| DI NativeInferenceClient/Conversation | 当前轮规划、commit/finalize、续轮状态 | 第二套Core ACK机制 |
| DI Provider + ORT adapter | session identity/lease/新请求执行证据、KV独立owner | Python缓存或未经Selection预加载 |
| RepoCore/file backend/RepoNode | 固定根、事务/范围/manifest、恢复与已授权存储/读取 | 理解模型、解密密钥、赋予推理权限 |
| DI publication + Core crypto owner | 查询匹配的材料/receipt，当前授权及合法密文serving恢复 | 复用旧grant、把存储hit当执行授权 |
| Native requester CLI | 实时消费、同对象多轮、一次close/drain | Python模拟对话、伪终态 |
| MiniNDN launcher | 拓扑、配置、启动/停止、资源采集 | C++业务oracle和轮次状态机 |

关键签名/字段/失败流程以 [CD-01–05](contracts/design.md)及[CD-06–09](contracts/material-reuse.md)为单一设计源。
T009首个FINALIZE失败点尚未完全归因；轮到 T009 后先执行其只读/仪器诊断子步骤，确认生产修复点并修订CD-03，
再编码该修复。不得将未知触发条件改写成“已经定位stop睡眠”。在 T003 完整 DONE 前，T004 及所有后续批次均不派发。
T006的durable key-reference/新grant绑定/serving恢复和protected缓存retention接口尚未冻结；轮到 T006 后才可做其
源码核查/设计闭合，该生产编码与真实protected warm资格不能凭文档结构通过释放；在此之前保持 NOT_STARTED。

## T003 model-source compatibility recovery gate

该 gate 是 T003 内的前置批 `B190-03A`，必须在 live-turn 代码或新的 MiniNDN run 之前完成，
不能并行派发 T004。固定生产调用链和修复顺序如下：

1. `source inspection`：由 Python 只读取固定 ONNX，记录文件摘要、graph/initializer identity、
   model format、quantization subtype、输入/输出/KV/position/backend contract；C++ 重新解析同一
   candidate，建立 Python/C++ 双向 identity oracle。
2. `catalog and identity`：把 weight-only INT8 subtype 和 public FP32/INT64 activation contract
   作为同一不可变 model identity 的字段；不同 precision、quantization subtype、graph、initializer、
   IO/KV/backend ABI 必须 cache miss，当前授权/Selection 不因命中而跳过。
3. `bounded material publication`：保留现有 1 MiB bundle 上限，扩展 canonical inline
   initializer 的有序分块、digest、range/reassembly 和 ownership；禁止简单提高上限，禁止把
   754 MB inline raw initializer 作为单个 payload。
4. `native assembly/runner gate`：用真实 C++ assembler、Provider runner 和 ORT CPU 验证
   selected-layer materialization、FP32 activation/KV/output、INT64 control input、一次 token
   和至少一个 continuation；只有出现真实 IO/KV mismatch 才设计 adapter。
5. `real chain`：上述 focused C++ gates、static review、受影响目标 build/install 和 candidate
   preflight 全部通过后，才允许一次新的 normal Repo；r6 原始失败不覆盖。

每一步都记录 `static`、`compile-link`、`runtime-test`、`unobserved`，并由 C++ production
target/oracle 证明 native behavior；Python 只做 candidate 读取、编排和资源记录。

## Per-Change Static Review and Batch Gate

每个逻辑修改单元在构建或运行前必须走一次只读静态复审：固定baseline、完整diff、CodeGraph
入口/caller核对、精确源码与配置查询、测试/fixture/oracle及Waf/安装闭包检查；跨库新符号
还须记录definition TU→target并用`nm -C`/`readelf`核对。复审结果写入当前task的checkpoint
或evidence，包含五lane coverage、四类miss（`static`、`compile-link`、`runtime-test`、
`unobserved`）、首个失败边界、review trace和closure decision。

任何生产源码、测试、launcher/profile/config、候选/证据元数据或本Spec/plan/tasks/contract
的后续修改都会使受影响范围的旧复审失效；先复审再构建/测试，发现控制性问题则停在当前
task内修复并复审。纯文档修改只执行结构/交叉一致性复审，不冒称产品行为验证；不因文档
审查通过改变T003的`PARTIAL`状态或派发T004。

## Logical Batch Quality Plan

| Batch / Members | Stable exit | Implementation / acceptance dependencies | Caller + contract + selector + source closure | Owner / result |
| --- | --- | --- | --- | --- |
| B190-01 / T001 | 每阶段可按request/attempt关联，不伪造缺失指标 | none / C++ PhaseTiming | Core/DI/CLI timing；CD-01；spec190-latency-tests；DI/Core库+requester | DI/Core / evidence/b190-01.md planned |
| B190-02 / T002 | Qwen profile实际1000ms、原认证和冻结行为不变 | T001 DONE / AckWindow+原Core回归 | launcher→requester→Core；CD-01；同测试target；Core仅实际修改时构建 | DI admission / evidence/b190-02.md planned |
| B190-03A / T003 precondition | fixed prebuilt INT8 candidate either passes native compatibility or remains explicitly unqualified | T002 DONE / source identity + bounded material + runner gate | Python inspection→C++ catalog/assembler/ORT；CD-INT8；native C++ selectors | DI model compatibility / evidence/b190-03.md |
| B190-03 / T003 | token终态前输出、同Conversation三轮无需逐轮进程退出 | B190-03A PASS / LiveTurns | requester→Conversation→EventReader；CD-02；DI/requester | DI API / evidence/b190-03.md planned |
| B190-09 / T004 | 节点固定根重开可读，cleanup不删committed对象 | T003 DONE / RepoRestart | RepoNode/Core/file backend+launcher；CD-07；Repo/原生fixture | Repo lifecycle / evidence/b190-09.md planned |
| B190-10 / T005 | User查询完整匹配后不再STORE，缺失才精确补齐 | T004 DONE / RepoLookupReuse | Runtime→RepoSourceProvider/RepoClient；CD-08；DI+Repo | DI prepare / evidence/b190-10.md planned |
| B190-11 / T006 | 新授权下protected持久/本地缓存合法复用，无旁路 | T005 DONE / ProtectedMaterialReuse | crypto owner→Repo backing/assembler；CD-09；Core+DI+Repo | protected material / evidence/b190-11.md planned |
| B190-05 / T007 | 两请求一次ORT load，身份/证据/lease与退出正确 | T006 DONE / ResidentSession，接入T003在T011验收 | Provider factory→ORT；CD-04；DI/provider+真实ORT fixture | DI adapter / evidence/b190-05.md planned |
| B190-08 / T008 | 按实际tensor/wire预算传输，无模型/KV/logits额外跨stage | T007 DONE / StageTransferBudget | worker→dependencyIo；CD-06；DI/Core观察+原生fixture | DI dataflow / evidence/b190-08.md planned |
| B190-04 / T009 | 正常FINALIZE及时处理，无30秒补偿满窗；异常补偿不丢 | T008 DONE / TerminalDrain | NativeInferenceClient↔ProviderHandler；CD-03；DI/provider/requester，必要Core | DI lifecycle / evidence/b190-04.md planned |
| B190-06 / T010 | 全真实路径收敛PASS，候选错误在启动前拒绝 | T009 DONE / all focused selectors | Waf注册/安装、C++oracle、launcher preflight；CD-05–09 | validation / evidence/b190-06.md planned |
| B190-07 / T011 | 三组配对同handle实测正确、提速、资源收束 | T010 DONE / all SC | 已安装MiniNDN+oracle；CD-05；不修改candidate | experiment / evidence/b190-07.md planned |

Batch growth decision：每行不同独立行为出口，不为少编译合并为一大批；达到出口立即限定验证。
每task写测试与实现→只读冻结审查；批末组合审查→受影响增量编译/测试。实测前T010 convergence。
STRICT_SERIAL下任何后项都必须等待紧邻前项DONE；T002/T003/T007不可提前分派或只做静态通过后暂缓测试。
调度器每次只允许一个 ACTIVE_TASK_ID，必须等当前行的完整 implementation→review→build→native
test/dynamic validation→evidence closure 全部完成后才切换到下一行。`PARTIAL`、`BLOCKED`、
`STATIC_PASS`、`TESTS_DEFERRED`、单项 selector PASS 或 cleanup PASS 都不是解锁状态；后续行
在未轮到前保持 `NOT_STARTED`，不使用并行任务或并行示例。失败只生成当前任务内的 recovery/Changed
gate，保留原始证据并停在当前行；不得把前项欠账转交 T010/T011。
任务已按依赖重新编号，正文/表格均T001至T011；仅B190批次和历史证据ID保留，映射见tasks checkpoint。
T004存储生命周期、T005 prepare命中、T006安全边界有不同caller/独立出口，因而分批，不按写文件或写测试拆分；
它们仍严格等待前项 DONE，不因不同 caller 而提前执行。

## Coverage Matrix

以下是规划的定位覆盖，不代表实现静态PASS；每批在自己的单一evidence中更新。B190-03A
新增 model-source/identity/material/runner 四个子边界，任何一个未闭合都保持 T003 `PARTIAL`。

| Lane | Coverage / files / verification |
| --- | --- |
| production entry/callers | covered for planning: DI_NativeRequester、NativeInferenceClient→ServiceUser、NativeProviderHandler→factory→ORT；CodeGraph + 当前源码 |
| implementation and wire | proposed: contracts/design.md CD-01–05；现有ACK wire不变、profile证据可选扩展；FINALIZE根因保持T009诊断门 |
| test/harness/oracle | gap until implemented: tests/unit-tests/spec190-*.t.cpp；C++独立断言、共享Spec189 oracle规则；不以Python行为断言关闭 |
| build/source closure | planned tests/wscript/examples/wscript注册；新符号definition TU→target，nm/readelf；已安装路径/摘要比对 |
| migration/evidence | covered for planning: 保留单轮/disabled-cache控制组、旧checkpoint/失败边界、原Spec189资格；变化须重新审计 |

新增五lane：production为RepoNode/RepoSourceProvider/Runtime与worker/dependencyIo，
implementation为文件backend/受保护range-store与CD-06–09，tests为planned RepoRestart/RepoLookupReuse/
ProtectedMaterialReuse/StageTransferBudget，build为Repo/DI/Core真实target+tests/wscript安装接线，
migration为固定root排除cleanup、原transient清理不变、旧key/grant禁止复用、Repo-enabled与诊断分开。
CodeGraph定位后核对实际源码，T006实现接线仍为gap，不以已有helper声明全路径支持。

## Dynamic Validation

T001/T002：none（时间/认证语义由C++可控时钟及现有安全selector证明）；
T003/T007/T009：以 C++ callback/owner/并发lease selector 和真实 production runtime 证明。
原计划的 ASan/UBSan 独立构建门在 2026-09-23 经用户明确取消，当前 Spec190 执行不把 sanitizer
compile/runtime 作为任务 Exit；这不是 sanitizer-clean 声明，若后续重新纳入必须建立新的 scope
decision、独立 build identity 和对应 evidence batch。
T007另有有界TSan selector `ResidentSessionConcurrency`，仅对生产cache owner的
acquire/release/evict/close/single-flight竞争做至少20次、总预算120秒的调度交错回归；
不把ASan无报告当race证明。该selector可用受控loader隔离ORT内部线程，不能替代另行真实ORT输出/加载复用测试，
不能声称未插桩的第三方ORT内部已TSan验证；首次可比toolchain/输出身份按本批dynamic gate card冻结。
共同参数矩阵：正常；边界999/1000/1001ms、TTL前/后；非法digest/ACK；取消/close与完成竞态；
加载失败重试、两会话同key、正在使用时换key、迟到FINALIZE/重复控制。
每native selector至少3次；异步生命周期定向case至少20次，预算每case90秒（正常不应睡满）。
Freeze→Sample→Run→Classify；Face/io/scheduler owner晚于worker销毁，或显式join/drain证明安全。
不能降低生产close契约迁就fixture；T008：计数/序列化C++反例；T004/T005：真实文件backend恢复、
双进程flock、部分提交/断电边界故障注入；T006复用现有C++授权/crypto负例，新增重启/lease动态门。
不能用目录存在、Python配置
检查、缓存标志或r260未采集的零日志行来证明material bytes=0。

## Pre-Qualification Design-Code Convergence

T010核对spec/contract→生产默认接线→CPP fixture/oracle→安装闭包；未解决语义、安全、owner、
wire或证据缺口BLOCK。记录static、compile-link、runtime-test、unobserved四类首边界，
Build measurement记录targets/-j/toolchain/elapsed/exit；无运行填NOT_RUN，不补造PASS。
审查记录包括review-agent profile路径/SHA、固定候选摘要、完整diff范围及五lane；结论仅适用该候选。
本次文档审计在[audit.md](audit.md)，不能代替实施后收敛审计。

## Immutable Candidate and Invalidation

候选元组：含新增未跟踪文件的源码快照、系统库/二进制与依赖hash、launcher/oracle/fixtures、
有效配置、输入/模型/权重digest、验收契约。复用现有安装校验和preflight，在T010补必要mutation。
源码/ABI改变→静态+受影响构建+安装+preflight+实测；harness/oracle/配置/输入改变→受影响静态+
preflight+实测；单纯描述更新→文档审计，不自动重跑模型。每candidate/gate一个active subject。
不复用旧run id，不覆盖r260；公共不可变缓存可重用，临时小ONNX fixture退出自动删除。

## Project Structure and Delivery

本目录：spec、research、plan、data-model、contracts/design、tasks、quickstart、traceability、audit/checklist。
生产修改限CD-01–09明确路径。维护Design/spec-design-changes.md，实施API变化随对应task同步中文契约和双PDF。
Git使用Experimental显式路径checkpoint，不push；混合既有源码/暂存区不得打包进本Spec文档提交。

## Strict Serial Execution

用户要求严格串行：T001–T011按数字顺序逐项完成，上项全部本项验收DONE后才开始下项。
以tasks.md的单链完成依赖为准；本表技术依赖是额外前置，不允许独立派发/静态后延期测试。
任务与独立验收批次一一对应，B190历史ID保留；T010候选收敛、T011真实系统验收置末。
T009未知FINALIZE首边界和T006安全设计门在各自任务内闭合；被阻塞时不得跳项。局部回归不能转交末项补欠账。
