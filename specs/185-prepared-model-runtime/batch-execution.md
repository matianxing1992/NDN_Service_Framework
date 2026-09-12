# Batch Execution Schedule

**Status**: PLANNED | **Baseline**: c5ad5f49 | **Date**: 2026-09-12

## Dispatch Order

本表细化plan已有12个批次，保留18个任务ID与既有验收要求；不是新增产品范围。
执行状态仍只在[tasks.md](tasks.md)登记，不能把本表当第二份进度表。
`DI/`指`NDNSF-DistributedInference/cpp/ndnsf-di/`；Core指`ndn-service-framework/`。
测试文件及完整Design binding见任务卡，suite全部PLANNED，必须先核对注册再运行。

| Order / batch | Coding order | Shared production boundary and build scope | Shared tests / stable exit | Result record |
| --- | --- | --- | --- | --- |
| 01 / B0 | T015 | 现有安装头、ONNX PImpl、根wscript与DI pkg-config；只构建受影响库及安装消费者 | Spec185InstalledApi；现有接口独立include/link/构造析构闭合 | evidence/b0-installed-api.md |
| 02 / B0C | T017 → T018 | Core OperationRuntime/State → DI NativeInferenceClient委托；Core+DI及ABI受影响消费者 | Spec185CoreOperation + Spec185DiCoreOperation；Core-only消费者和真实DI适配同批闭合，旧协作/流/注册定向回归 | evidence/b0c-core-operation.md |
| 03 / B1 | T001 → T002 | DI Runtime/bootstrap/Core owner组合；DI库及Runtime fixture | Spec185Runtime；open→user→close/drain，依赖释放完整 | evidence/b1-runtime.md |
| 04 / B2E | T016 | registry/合作strategy/control及catalog端口；DI+受影响adapters/consumers | Spec185ExtensionRegistry；freeze、控制与旧端口兼容，Package构造依赖可用 | evidence/b2e-extensions.md |
| 05 / B2 | T003 → T004 | Package/preparation/cache/waiter；DI及准备fixture | Spec185Preparation；verified READY、8并发单flight与失败/refresh隔离 | evidence/b2-preparation.md |
| 06 / B3 | T005 → T006 | PreparedModel→NativeInferenceClient→Core；DI及请求fixture | Spec185PreparedRequest；两次独立授权请求复用包，结果/取消/事件一致 | evidence/b3-request.md |
| 07 / B4 | T007 → T008 | Conversation→既有coordinator/journal；DI及会话fixture | Spec185Conversation；两轮持久提交、恢复/替换/导出与handle一致 | evidence/b4-conversation.md |
| 08 / B5 | T009 → T010 | Provider facade→认证后组装→artifact lease；DI+provider可执行文件及fixture | Spec185ProviderAssembly；冷/热请求安全隔离，stop后清理闭合 | evidence/b5-provider.md |
| 09 / B6 | T011 | native caller、公开SDK消费、CLI兼容；DI+受影响examples及安装consumer | Spec185Compatibility；安装后例子及最小unary/stream真实接线通过 | evidence/b6-migration.md |
| 10 / B7 | T013 | 已完成native链的当前候选；复用同身份binary，仅因源码/依赖变化增量构建 | Spec185Process；全部C++模式/反例、no-Python及依赖身份资格通过 | evidence/b7-cpp-qualification.md |
| 11 / B8 | T012 | 既有pybind TU与Python薄封装；只构建受影响extension及ABI依赖 | wrapper-only checks；C-07映射、GIL/async/异常边界通过，不推进native资格 | evidence/b8-python.md |
| 12 / B9 | T014 | Design/API/交付文件；文档生成器与双PDF，无native构建 | 文档/源码/证据一致，184未完成项保留 | evidence/b9-handoff.md |

上表为默认拓扑顺序；依赖硬门仍按任务卡。按共享 [Dependency-Scoped Dispatch](../../skills/speckit-code-design/references/pre-test-static-review.md#dependency-scoped-dispatch)，依赖该任务的工作必须等待通过；无依赖、文件边界清晰且自身前置已满足的任务可继续。单主会话内主代理编码、只读 review-agent 子代理异步审查固定快照，派发后优先推进合格独立工作。
当前 registry 的 T003→T004、T005→T006 等仍有显式依赖，B5仍依赖B4出口，本修订不解除这些硬门。没有已登记独立任务时允许等待；若细分独立子任务，先记录 Design binding、依赖、写入范围和单一共享文件owner，再开始重叠工作。批次 evidence 记录审查快照/摘要、派发与返回时间、重叠任务或等待原因；不另建进度权威。B0C前就须核对184同树Core/DI接线，不能等B3才第一次检查。
B0只关闭当时存在的接口与ABI；不能要求尚未编码的Runtime/PreparedModel先可链接。未来公开符号在所属批次检查安装导出，B6汇总消费；B7不再重新设计SDK。

## Per-Task Static Gate

1. 批次开始冻结base、成员、设计契约、预期出口、受影响source closure和测试selector；先解决契约级OPEN项。
2. 编码一个任务及其C++fixture、反例、注册和必要调用方；一段关键逻辑写完即检查参数/状态/owner/错误传播。
3. 任务完成后明确调用`/home/tianxing/.codex/skills/review-agent/SKILL.md`做只读静态门，读取完整任务diff和足够周边代码、全部受影响caller/test/build接线。
4. 审查期间可推进已登记独立任务；依赖工作等待没有控制性缺陷且五lane无gap。修复发现后以新快照复审受影响范围及关联不变量，失败阻塞依赖闭包；共享契约变化重新判断独立性。代码静态通过但未运行验收时保持PARTIAL、任务不勾选。
5. 同批全部成员完成，再审查从批次base至工作树的组合diff：类型/字段/错误码对应、状态转换、权限校验顺序、同步异常清理、回调寿命及测试是否真正进入生产路径。
6. 组合门通过才进入批末共享构建与测试；测试失败先定位首边界，修复→受影响静态复审→必要增量构建/复测，不能直接跳批或把失败收进下一批。

T017内部按调度/ticket→完成/订阅→reader三个逻辑段自查，但仍是一个带Core消费者的实现任务；不为每段建立独立编译循环。
静态门不能证明链接与运行时安全；批末构建/测试不可省略，也不承诺本流程必然减少总耗时。

## Five-Lane Review Matrix

每批证据必须填以下五个精确lane；安全、并发和错误是各lane中的检查维度，不能替代lane本身。

| Lane | Batch-specific binding |
| --- | --- |
| production entry/callers | 上表生产入口；B0/B6为安装consumer，B0C为NativeInferenceClient及Core-only consumer，B1–B5为各领域owner，B7为独立authority/requester/provider，B8为pybind实际入口，B9为Design生成入口 |
| implementation and wire | 各任务C-08/C-09类/字段/函数delta；B0C必须核对Core传输终态与DI领域终态分层；其他批次wire无修改时写N/A及diff依据，不伪造协议审查 |
| test/harness/oracle | 上表suite→任务卡实际C++fixture/独立oracle→tests/wscript注册；B7逐模式进程oracle；B8只wrapper断言；B9为文档校验与证据引用，native N/A |
| build/source closure | 根wscript、tests/wscript、examples/wscript及实际改变的安装/pybind入口；登记definition TU→target→binary/hash，B9只文档构建输入和PDF身份 |
| migration/evidence | C-07兼容/暴露、C-09 Core依赖方向、调用方矩阵、上表唯一结果文件与tasks状态；B0C不得保留重复executor，B8不得新增Python领域owner |

本表是coverage计划。执行时每lane填实际path:symbol、查询与covered/N/A(reason)/gap；泛称目录或只抄计划不能STATIC_PASS。
review trace记录官方skill路径/SHA、base与工作树diff摘要、发现、修复及复审。每批一个结果文件容纳所有任务静态门和批末结果，不另造每任务报告。

## Shared Build and Retest Policy

“集中测试”指在一个逻辑批次内集中，不是等18任务全部写完才首次编译。正常情况下任务之间不编译、不跑全suite。
批末按源/ABI闭包形成一次普通增量构建计划，随后运行本批selector并复用binary；sanitizer按C-04独立配置统一构建，不能与普通构建混成同一次或虚称只需一个binary。
使用已核验build tree、默认-j4及内存观测；不同时启动竞争构建。不同profile的必要构建不是冗余测试。
跨批不默认重跑所有已过suite；改动共享头/ABI、授权、终态或清理契约时追加受影响旧suite，记录为何受影响。
静态阶段若存在必须通过编译才能消除的具体模板/ABI不确定性，可登记一次有明确问题与目标的提前compile-only探针；不能借此恢复每次小修改编译。
运行时失败不得靠静态推理宣称修复，必须复跑失败用例及受影响回归。各动态矩阵/重复次数仍遵守C-04，不擅自删减。
B7是跨批组合资格，必须保留；已验证同身份binary可复用，不能因换批次ID强制全量重建。

## Allocation and Closure Decision

保留12批不是按文件数凑组：B0C两个成员共享“通用Core实现被真实DI消费”的迁移出口，两个selector共同构成该出口；
B1生命周期、B2缓存、B3调用、B4持久会话、B5 Provider有不同状态机和反例，不合并为长时间无验证的大批。
B2E必须先独立提供Package所需合作策略；B6最小迁移冒烟与B7完整资格各有出口；Python和文档不与native实现混合计完成。
如B0C实现发现Core primitive已可独立交付而DI迁移出现新的硬依赖，先在plan/tasks登记拆分、保留证据，再继续；不能以节省一次构建无限扩批。
每批审查时写Batch growth decision；达到稳定出口即验证。写Closure decision及static/compile-link/runtime-test/unobserved漏检复盘，未观察项明确保留。
只有对应验收全部通过才勾选任务；文档结构检查不等于源码STATIC_PASS或产品完成。
