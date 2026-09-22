# Implementation Plan: Multi-turn Token Generation Latency

**Branch**: `Experimental` | **Date**: 2026-09-22 | **Spec**: [spec.md](spec.md)
**Status**: PLANNED — 文档规划，不是已实现优化。

## Summary

优先消除每轮60秒ACK固定窗口，再打通FINALIZE及时收尾、同handle多轮/实时token，最后复用CPU已加载session。
分段测量先行，不把总时长当decode时间，不引入自适应调度/新模型/通用Repo重构。
新增核心范围：stage传输预算、每节点固定Repo根、重启恢复、查询命中免重复发布及受保护缓存安全接线。
现有r260基线、已修复70→6次runner准备、r259 affinity/KV均保留，详见[research](research.md)。
Spec189待做的跨请求驻留转入本Spec T005，旧Spec资格不关闭。

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
VII：候选冻结及拒绝零启动；VIII：T006收敛PASS后才运行T007。G4/D3是驻留及drain硬约束。
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
T004首个FINALIZE失败点尚未完全归因；先执行其只读/仪器诊断子步骤，确认生产修复点并修订CD-03，
再编码该修复。不得将未知触发条件改写成“已经定位stop睡眠”。其余批次不被该诊断阻塞。
T011的durable key-reference/新grant绑定/serving恢复和protected缓存retention接口尚未冻结，
只可先做其明确的源码核查/设计闭合；该生产编码与真实protected warm资格BLOCK，不能凭文档结构通过释放。

## Logical Batch Quality Plan

| Batch / Members | Stable exit | Implementation / acceptance dependencies | Caller + contract + selector + source closure | Owner / result |
| --- | --- | --- | --- | --- |
| B190-01 / T001 | 每阶段可按request/attempt关联，不伪造缺失指标 | none / C++ PhaseTiming | Core/DI/CLI timing；CD-01；spec190-latency-tests；DI/Core库+requester | DI/Core / evidence/b190-01.md planned |
| B190-02 / T002 | Qwen profile实际1000ms、原认证和冻结行为不变 | T001 / AckWindow+原Core回归 | launcher→requester→Core；CD-01；同测试target；Core仅实际修改时构建 | DI admission / evidence/b190-02.md planned |
| B190-03 / T003 | token终态前输出、同Conversation三轮无需逐轮进程退出 | T001 / LiveTurns | requester→Conversation→EventReader；CD-02；DI/requester | DI API / evidence/b190-03.md planned |
| B190-04 / T004 | 正常FINALIZE及时处理，无30秒补偿满窗；异常补偿不丢 | T001,T003 / TerminalDrain | NativeInferenceClient↔ProviderHandler；CD-03；DI/provider/requester，必要Core | DI lifecycle / evidence/b190-04.md planned |
| B190-05 / T005 | 两请求一次ORT load，身份/证据/lease与退出正确 | T001 / ResidentSession，接入T003在T007验收 | Provider factory→ORT；CD-04；DI/provider+真实ORT fixture | DI adapter / evidence/b190-05.md planned |
| B190-06 / T006 | 全真实路径收敛PASS，候选错误在启动前拒绝 | T001–T005,T008–T011 / all focused selectors | Waf注册/安装、C++oracle、launcher preflight；CD-05–09 | validation / evidence/b190-06.md planned |
| B190-07 / T007 | 三组配对同handle实测正确、提速、资源收束 | T006 PASS / all SC | 已安装MiniNDN+oracle；CD-05；不修改candidate | experiment / evidence/b190-07.md planned |
| B190-08 / T008 | 按实际tensor/wire预算传输，无模型/KV/logits额外跨stage | T001 / StageTransferBudget | worker→dependencyIo；CD-06；DI/Core观察+原生fixture | DI dataflow / evidence/b190-08.md planned |
| B190-09 / T009 | 节点固定根重开可读，cleanup不删committed对象 | none / RepoRestart | RepoNode/Core/file backend+launcher；CD-07；Repo/原生fixture | Repo lifecycle / evidence/b190-09.md planned |
| B190-10 / T010 | User查询完整匹配后不再STORE，缺失才精确补齐 | T009 / RepoLookupReuse | Runtime→RepoSourceProvider/RepoClient；CD-08；DI+Repo | DI prepare / evidence/b190-10.md planned |
| B190-11 / T011 | 新授权下protected持久/本地缓存合法复用，无旁路 | T005,T009,T010 + CD-09设计门 / ProtectedMaterialReuse | crypto owner→Repo backing/assembler；CD-09；Core+DI+Repo | protected material / evidence/b190-11.md planned |

Batch growth decision：每行不同独立行为出口，不为少编译合并为一大批；达到出口立即限定验证。
每task写测试与实现→只读冻结审查；批末组合审查→受影响增量编译/测试。实测前T006 convergence。
T002与T003/T005可在T001通过后分派不同文件；共享NativeInferenceClient/Provider接线修改串行合并复审。
追加T008–T011保留旧ID，执行顺序按依赖，不按数字大小；T006/T007仍最后。
T009存储生命周期、T010prepare命中、T011安全边界有不同caller/独立出口，因而分批，不按写文件或写测试拆分。

## Coverage Matrix

以下是规划的定位覆盖，不代表实现静态PASS；每批在自己的单一evidence中更新。

| Lane | Coverage / files / verification |
| --- | --- |
| production entry/callers | covered for planning: DI_NativeRequester、NativeInferenceClient→ServiceUser、NativeProviderHandler→factory→ORT；CodeGraph + 当前源码 |
| implementation and wire | proposed: contracts/design.md CD-01–05；现有ACK wire不变、profile证据可选扩展；FINALIZE根因保持T004诊断门 |
| test/harness/oracle | gap until implemented: tests/unit-tests/spec190-*.t.cpp；C++独立断言、共享Spec189 oracle规则；不以Python行为断言关闭 |
| build/source closure | planned tests/wscript/examples/wscript注册；新符号definition TU→target，nm/readelf；已安装路径/摘要比对 |
| migration/evidence | covered for planning: 保留单轮/disabled-cache控制组、旧checkpoint/失败边界、原Spec189资格；变化须重新审计 |

新增五lane：production为RepoNode/RepoSourceProvider/Runtime与worker/dependencyIo，
implementation为文件backend/受保护range-store与CD-06–09，tests为planned RepoRestart/RepoLookupReuse/
ProtectedMaterialReuse/StageTransferBudget，build为Repo/DI/Core真实target+tests/wscript安装接线，
migration为固定root排除cleanup、原transient清理不变、旧key/grant禁止复用、Repo-enabled与诊断分开。
CodeGraph定位后核对实际源码，T011实现接线仍为gap，不以已有helper声明全路径支持。

## Dynamic Validation

T001/T002：none（时间/认证语义由C++可控时钟及现有安全selector证明）；
T003/T004/T005：asan-ubsan，callback/owner/并发lease风险；匹配ABI的独立定向构建，不借旧失配目录。
T005另有有界TSan selector `ResidentSessionConcurrency`，仅对生产cache owner的
acquire/release/evict/close/single-flight竞争做至少20次、总预算120秒的调度交错回归；
不把ASan无报告当race证明。该selector可用受控loader隔离ORT内部线程，不能替代另行真实ORT输出/加载复用测试，
不能声称未插桩的第三方ORT内部已TSan验证；首次可比toolchain/输出身份按本批dynamic gate card冻结。
共同参数矩阵：正常；边界999/1000/1001ms、TTL前/后；非法digest/ACK；取消/close与完成竞态；
加载失败重试、两会话同key、正在使用时换key、迟到FINALIZE/重复控制。
每native selector至少3次；异步生命周期定向case至少20次，预算每case90秒（正常不应睡满）。
Freeze→Sample→Run→Classify；Face/io/scheduler owner晚于worker销毁，或显式join/drain证明安全。
不能降低生产close契约迁就fixture，sanitizer未跑就保留该验收缺口。
T008：计数/序列化C++反例+适用asan；T009/T010：真实文件backend恢复、双进程flock、部分提交/断电边界
故障注入及asan；T011复用现有C++授权/crypto负例，新增重启/lease动态门。不能用目录存在、Python配置
检查、缓存标志或r260未采集的零日志行来证明material bytes=0。

## Pre-Qualification Design-Code Convergence

T006核对spec/contract→生产默认接线→CPP fixture/oracle→安装闭包；未解决语义、安全、owner、
wire或证据缺口BLOCK。记录static、compile-link、runtime-test、unobserved四类首边界，
Build measurement记录targets/-j/toolchain/elapsed/exit；无运行填NOT_RUN，不补造PASS。
审查记录包括review-agent profile路径/SHA、固定候选摘要、完整diff范围及五lane；结论仅适用该候选。
本次文档审计在[audit.md](audit.md)，不能代替实施后收敛审计。

## Immutable Candidate and Invalidation

候选元组：含新增未跟踪文件的源码快照、系统库/二进制与依赖hash、launcher/oracle/fixtures、
有效配置、输入/模型/权重digest、验收契约。复用现有安装校验和preflight，在T006补必要mutation。
源码/ABI改变→静态+受影响构建+安装+preflight+实测；harness/oracle/配置/输入改变→受影响静态+
preflight+实测；单纯描述更新→文档审计，不自动重跑模型。每candidate/gate一个active subject。
不复用旧run id，不覆盖r260；公共不可变缓存可重用，临时小ONNX fixture退出自动删除。

## Project Structure and Delivery

本目录：spec、research、plan、data-model、contracts/design、tasks、quickstart、traceability、audit/checklist。
生产修改限CD-01–09明确路径。维护Design/spec-design-changes.md，实施API变化随对应task同步中文契约和双PDF。
Git使用Experimental显式路径checkpoint，不push；混合既有源码/暂存区不得打包进本Spec文档提交。
