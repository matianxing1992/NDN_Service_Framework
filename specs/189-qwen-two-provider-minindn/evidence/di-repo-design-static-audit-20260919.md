# DI / DistributedRepo Design-to-Code Static Audit

> 后续复核：并行开发已加入 material-only consumer 并记录局部 C++ 测试通过；F01 的“未接入”描述保留为本次审查时点的事实，不再代表最新源码。最新边界及建议见 [repair design analysis](di-repo-repair-design-analysis-20260919.md)，真实生产链验收仍未关闭。

## Decision

**Status: PARTIAL / NOT_STATIC_PASS / OPEN_FOR_NEXT_BATCH**

本轮只读审查生产源码、设计契约和已有证据，没有修改产品代码，没有重新编译，也没有运行模型或 MiniNDN。静态审查发现了值得优先修复的具体问题，但不能据此宣布两个子系统已经完成全量验收。

最重要的结论：

1. **DI 的原子材料发布端已存在，但当前 Provider 组装入口仍读取完整 canonical initializer。** “存在分层材料接口”不等于“生产执行已经按分配范围取数”。
2. **准备阶段仍有随整个模型大小增长的并存内存。** 后续释放 source 的修复有效，但不能消除发布前的峰值。
3. **Repo 的目录快照、增量历史恢复和容量核算有独立的正确性缺口。** 这些问题不能仅靠 DI 的授权检查解决。
4. 当前实现已有授权、所有权和生命周期方面的重要改进；不能照搬旧审查，把已经修复的问题重新认定为当前缺陷。

本报告给出修复建议，不授权或启动这些修复。没有把任何未完成 Spec 任务改为 `[x]`。

## Follow-up reconciliation — 2026-09-19

本报告的主体结论保留为当时工作树的历史只读审计；后续源码和 selector 已改变其中
一项边界。`NativeCanonicalOnnxAssembler` 现在在受保护 Selection 后读取认证的
material manifest 和 selected payload，且已有 C++ material-only/aggregate-budget
selector。因而 F01 的“当前 Provider 组装仍读取完整 initializer”只适用于本报告的
旧 snapshot，不再描述最新局部实现；它仍未满足真实 Core→Provider ingress、两
Provider assembly/execute、独立输出和 cleanup 的资格要求。

当前 Spec189 的修订分流如下：

| Finding | Revised status | Spec189 action |
| --- | --- | --- |
| F01 | `LOCAL_REPAIR_PRESENT / QUALIFICATION_OPEN` | T003/T006 保留真实 protected ingress、名称/字节审计和 full-path evidence；不重做已通过 consumer selector。 |
| F02 | `OPEN / HIGH` | T003-R1 / B189-1c；测量 source、initializer、material、encryption、ORT preparation owners 和取消/重试清理。 |
| F03/F04 | `DEFERRED / CONDITIONAL` | 当前 Qwen native protected qualification 不调用 generic catalog snapshot/delta；若调用方进入 candidate，再创建 snapshot-required/incarnation repair。 |
| F05 | `FOCUSED_CXX_PASS / QUALIFICATION_OPEN` | T003-R2 / B189-1c 已通过 mixed range/vector/Data admission selector；完整 replacement、失败回滚和 protected candidate 仍开放，见 [F05 evidence](b189-f05-quota-20260919.md)。 |
| F06/F07 | `DEFERRED / LEGACY` | 当前 native protected path 不使用 segmented compatibility helper 或 Python power-loss backend；保留后续维护边界，不授予 Spec189 PASS。 |
| F08 | `OPEN / IN-SCOPE` | T009-R1；C++ barrier 控制旧 terminal/exception 与新 turn owner 安装。 |
| F09 | `FOCUSED_CXX_PASS / QUALIFICATION_OPEN` | T003-R3 / B189-1c 已通过 fsync/close injected failure、单一 owner 与可见性 selector；rename/crash recovery 和完整候选资格仍开放。 |

这次对账不改变原始审计的 `PARTIAL / NOT_STATIC_PASS` 结论，也不把后续局部 selector
改写成真实资格证据。对应需求、依赖和未完成出口已写入 [Spec189](../spec.md)、
[plan](../plan.md)、[tasks](../tasks.md) 与 [batch register](../batch-execution.md)。

## Baseline and Coverage

- 工作目录：`/home/tianxing/NDN/ndn-service-framework`。
- 分支：`Experimental`；检查时 HEAD：`13b79ad1f11929ecc88c0e61d86f6dbcc07e5af3`。
- 工作树已有约 537 项改动；本报告针对检查时的工作树，不是仅针对 HEAD。
- 清单范围：两个子项目中 `.cpp/.hpp/.h/.cc/.py` 文件，排除 `.git/build/__pycache__/vendor/.venv` 目录。共 **372 个文件、152,163 行**；其中 **173 个 Python 文件通过 AST 语法解析**。这些数字不是逐行语义审查覆盖率。
- 清单摘要：按路径排序，对每个文件记录 `path + NUL + sha256(bytes) + LF` 后计算 SHA-256，结果为 `86c0eb3c078987f6cb0c35473a6a5c4e91b377ed6d441097e8f44fd1e91b2081`。
- 深读范围：Runtime/Conversation、模型准备与发布、canonical ONNX 组装、受保护 Repo 读写、RepoCore 目录及容量、分段对象、本地持久化、Python 网络目录同步，以及对应构建和测试入口。
- **未完成的范围**：全部算子/切分算法、每个 tokenizer/ORT 分支、全部 Python 兼容 API、网络修复调度的所有状态组合，以及全部并发交错的逐行证明。因此不使用“所有代码已审完”或“安全性已证明”的表述。

Context Mode 项目级健康检查通过，但 active-Spec 索引中的 `tasks.md` 摘要已过期；检查点以仓库文件为准。CodeGraph 检索包含历史快照，且精确文件查询未给出完整结果，因此源代码结论使用实际文件复核。Spec 结构检查通过但有任务编号不连续警告；它不证明实现正确。

## Design Authority

以 [Spec 189](../spec.md) 的 FR-002/003/004/007–010/015/019、[当前任务](../tasks.md)、[设计管理规则](../../../Design/MANAGEMENT.md) 和 [目标路线](../../../Design/target-roadmap.tex) 的 TG-02/03/04 为主要比较依据。

| Desired property | Current assessment |
| --- | --- |
| prepare 发布拓扑无关、可按分配范围获取的不可变材料 | 原子材料已生成并发布；真实 Provider consumer 尚未闭合 |
| ACK/Selection/授权先于本次分配材料读取 | 存在受保护执行路径；仍需真实两 Provider 链路验收，不能由本地发布测试代替 |
| PreparedModel 保留引用而非永久持有全部模型字节 | 发布后已有释放；准备峰值与临时材料仍是问题 |
| Repo snapshot 和 cursor 描述同一目录状态 | C++ snapshot 存在读列表与读版本之间的竞态 |
| 目录历史裁剪/重启不导致漏同步 | 当前 delta 缺少明确的历史缺口协议 |
| 存储预算覆盖 committed、staging 和 reservation | 不同写入接口核算不一致 |
| 一次执行的取消、结束和下一次执行互不干扰 | 已有 generation 检查，但 handle 安装和异常清理仍有条件性风险 |

目标文档中较旧的 NOT_READY、source 持有等描述不能覆盖当前代码事实。反过来，已完成局部单元测试也不能覆盖仍未走通的端到端目标。

## Findings

### F01 — HIGH — Atomic-material consumer is not connected to the current Provider assembly path

**证据**：`NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp:361–453` 从 root metadata 取 `canonicalSourceDataName` 和 `canonicalInitializerDataName`，调用 `fetchEncryptedLargeData()` 获取完整对象，随后检查大小并将缓冲交给组装。该入口没有使用 `loadMaterialSelection()`。

与此同时，`NativeCanonicalPreparationCatalog.cpp:60–98` 会派生 material manifest；`NativeCanonicalArtifactPublisher.cpp:737–781` 发布完整 source、完整 initializer、原子材料和材料索引。`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoSourceProvider.hpp` 已有选择性材料读取接口，对应单元测试不能证明上述生产入口已经使用它。

**影响**：违反 FR-010 的按分配范围取数目标；两个 Provider 可能分别承担不属于自身分配范围的完整 initializer 传输和内存。对象通过授权和摘要验证不等于读取范围已被限制。

**建议**：在 Selection/grant 验证后，把受保护的 material-index reader 接入真实组装路径。将 root digest、model identity、角色、分配节点及共享依赖与本次 grant 绑定；对原子材料格式禁止静默退回完整 initializer。不要为了复用现有 helper 绕过受保护 Repo 路径。读取 API 同时接受可信的大小上限，不能等完整分配后才检查长度。

**验收**：C++ 生产 Provider 测试记录实际获取的名称和字节；未分配材料和完整 initializer 请求必须失败，未选中 Provider 不得进入材料读取；两个真实角色完成 assembly、执行、terminal 和 cleanup 后才可关闭该项。

### F02 — HIGH — Preparation retains multiple model-sized representations

**证据**：`Runtime.cpp` 的本地 source 路径读取完整文件；`NativeOnnxRecipeAssembler.cpp:1946–2040` 的 `deriveNativeCanonicalMaterialManifest()` 逐个物化 TensorProto，序列化后保存在拥有独立缓冲的 `payloads` 中。准备目录同时持有原始 source/initializer 和派生材料。发布器还发布完整表示及原子表示。

**影响**：发布前存在完整 initializer、派生 payload 及序列化临时数据并存。`ModelPreparationCache.cpp` 已在成功发布后调用 `releaseTransientSource()`；这解决长期保留的一部分问题，但不是有界准备峰值。

已有 [r36 证据](b189-onnx-memory-r36-20260919.md) 显示 `RESOURCE_BOUNDARY:MemAvailable`，最低可用内存 1,557,188,608 bytes，门限 1,610,612,736 bytes，峰值 RSS 4,591,411,200 bytes。它支持资源问题仍未关闭，**不能证明全部峰值只由本项导致**。

**建议**：采用有明确 owner 的文件/range source，material manifest 保存 offset/length/digest；按有界窗口读取、加密、提交单个材料。分别统计 source、material、加密缓冲和 ORT 内存预算。完整 source 是否继续发布应由兼容目标决定，不能默认保留双份大对象而不记成本。

**验收**：C++ 生产准备链测量随 initializer 大小增长的额外驻留内存，并检查取消、失败和重试的 source/临时材料清理。提高超时或降低内存保护门限不是本项修复。

### F03 — HIGH — Catalog snapshot can pair an old inventory with a new cursor

**证据**：`NDNSF-DistributedRepo/src/RepoCore.cpp:637–650` 先调用 `m_store->listManifests()`，之后才取得 `m_mutex` 并读取 `m_catalogEpoch`。写入路径在 Core 锁内更改 store 和目录版本。

**反例**：snapshot 读到 E 版本列表；写入者提交对象 B 并推进到 E+1；snapshot 随后返回旧列表和 E+1 cursor。客户端继续请求 `catalogDelta(E+1)` 时不会再收到 B。本报告没有执行该线程交错，但源代码中的窗口明确存在。

**建议**：在既有 Core→store 锁顺序内获取同一时刻的列表和版本。若允许绕过 Core 修改后端，则需要后端事务快照/版本契约，单靠 Core 锁不足。

**验收**：可控 C++ backend 在列表读取与版本获取之间设置屏障并并发写入；snapshot 加 subsequent delta 必须包含 B，且没有把 cursor 提前到未包含的状态。

### F04 — HIGH — Catalog delta lacks an explicit history-gap recovery contract

**证据**：C++ `RepoCore.hpp` 的 epoch 和 change vector 为进程内状态；`RepoCore.cpp:654–669` 只筛选大于 sinceEpoch 的条目，没有重启身份或可用历史下界。

Python 网络路径并非没有持久化：`pythonWrapper/py_repoclient/orchestration.py` 使用 SQLite journal 和 peer watermark。但是 `_compact_catalog_locked():3448` 删除超出保留数量的数据库历史，`_restore_catalog_state():3457` 重启后只恢复保留日志；`_catalog_delta():3812` 和 `_catalog_sync_loop():5386` 没有在 cursor 早于保留历史时要求完整快照。正常运行时 `_catalog_changes` 又持续累积，数据库裁剪不等于内存列表裁剪。

**反例**：离线 peer 的 cursor 落后于已裁剪历史；server 重启后只返回剩余 delta，peer 却将 watermark 推进到最新版本，可能漏掉仍有效的旧对象或删除记录。Python 的 snapshot 能从 inventory 补充当前对象，并不意味着同步循环会在这个条件下调用它。

**建议**：分开维护当前目录、删除记录和有界变化日志；返回 source/incarnation、sequence 和 oldest-available sequence。发现缺口时明确返回 snapshot-required；完成经过验证的快照替换后才更新 watermark。C++ 和 Python 的实际持久化能力分别声明，不能用一个实现的能力替另一个背书。

**验收**：将历史上限设为 2，执行至少 3 次增删、重启、让旧 peer 和新 peer 同步，验证不漏对象、不复活删除项、内存历史有界。

### F05 — HIGH — Ordinary writes do not account for other range reservations

**证据**：`RepoCore.cpp:155–165` 为 range 写入使用 `freeBytes - m_reservedRangeBytes`；普通 `handleStore():443–474` 使用 `freeBytes + oldSize`，没有扣除其他对象的 range reservation。`putDataPacket()` 也需要纳入统一检查。底层磁盘可用空间检查不能替代 Repo 配置的逻辑配额。

**反例**：Repo 配额 100 MiB，A 的 range staging 预留 80 MiB，在缓存 capability 尚未重新核算时，B 的普通写入 30 MiB 仍可能被接受。磁盘本身足够大并不能使这次逻辑配额超额合理。

**建议**：统一 committed/staged/outstanding-reservation 口径，各写入 API 共享准入函数，避免漏算和重复扣除。替换旧对象需要另计提交前新旧并存的物理空间；失败、取消和同对象替换必须释放正确的 reservation。

**验收**：C++ 混合 range/vector/Data-packet 写入测试，覆盖低配额、覆盖旧对象、失败重试和 reservation 清理。

### F06 — MEDIUM — Segmented compatibility writes are not atomic replacements

**证据**：`NDNSF-DistributedRepo/src/RepoClient.cpp` 的 `putSegmented()` / `localPutSegmented()` 使用固定 `objectName/seg/i`，先逐段写入，再更新父 manifest。

**反例**：覆盖已有对象时，新 segment 0 已写入，segment 1 写入失败，旧 manifest 仍可见但引用的内容已改变。后续完整性检查通常会拒绝混合对象；问题是旧的有效版本也不可读，并不是签名验证会接受错误结果。

**边界**：这是本地公开兼容 helper 的问题，不能泛化为新的 protected range-commit 路径或所有网络写入都没有事务。

**建议**：使用 generation/digest 限定的不可变 segment 名称，最后原子提交/CAS 父 manifest；失败只清理本次未引用对象。或者明确禁止该 helper 覆盖，迁移到事务 API。

**验收**：在第 k 段注入写失败，旧对象必须仍完整可读；测试相同操作重试和冲突写入。

### F07 — MEDIUM — Local Python atomic replacement is not power-loss durability

**证据**：`pythonWrapper/py_repoclient/local_artifact_backend.py` 将实现称为 crash-safe，但 `_atomic_json()` 使用 `write_text()` 和 `os.replace()`，没有 metadata 文件及父目录 fsync。`_LocalPublishDriver.commit()` 在 payload/metadata rename 后返回 COMMITTED；network backend 的本地 receipt 写入也需检查同类边界。

**影响**：进程异常下的原子替换不等于断电后目录和 metadata 都持久。不能因为 payload 本身 fsync 过，就把后续元数据持久性视为已完成。

**建议**：明确 COMMITTED 的故障模型；若要求掉电持久性，使用唯一临时文件、文件 fsync、rename、目录 fsync，并定义 payload 与 metadata 的提交顺序和 orphan 恢复。

**验收**：故障注入验证每个持久化边界；本轮没有进行掉电测试。原生 C++ 后端已有 fsync，不能把此结论套用于所有后端。

### F08 — MEDIUM / CONDITIONAL — Conversation handle installation can race with a newer turn

**证据**：`NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.cpp:252–299` 的 terminal 回调和部分完成清理检查 generation，但 `requestInternal()` 返回后的 `activeHandle = handle` 只检查 closed；catch 清理也未检查 generation。

**条件性反例**：A 很快结束，terminal 回调释放 turn gate；另一线程启动 B 并安装 B handle；A 的调用栈随后返回并把 activeHandle 覆盖成 A。close 可能取消错误的 turn。异常清理也可能覆盖较新的状态。若公开契约禁止这种并发或重入，必须明确并强制该契约；本轮没有运行反例，因此不宣称已复现。

**建议**：handle 安装及异常清理统一要求 activeGeneration 等于本次 generation；过期 turn 不能修改新 turn 的 owner。保留内部 terminal-before-user-callback 的现有改进。

**验收**：C++ 屏障控制 A terminal、B start、A handle installation 顺序；close 必须取消 B，且只有一次终态。不要通过放宽 close/取消语义掩盖竞态。

### F09 — MEDIUM — Metadata fsync failure can close the same descriptor twice

**证据**：`NDNSF-DistributedRepo/src/backends/FilesystemRepoStoreBackend.cpp:92–122` 在 fsync 失败分支先 `close(fd)`，没有将 fd 置为 -1，抛异常后 outer catch 再次 `close(fd)`；close 自身报错路径也需要考虑 fd 已释放的情况。正常成功 close 后已有 `fd = -1`，因此并非所有 rename 失败都会触发这个问题。

**影响**：在错误处理与其他线程打开文件交错时，同一整数描述符可能被复用，第二次 close 有机会关闭无关资源。这是错误路径资源所有权缺陷，不是已经观测到的实验根因。

**建议**：采用 RAII fd owner 或单次 close helper，在交出/关闭所有权前使本地 fd 失效；清理时仅关闭仍拥有的 fd，并保留原始 errno。参考同目录 `FilesystemArtifactStore.cpp` 的失效处理，但仍检查其具体边界。

**验收**：对 fsync/close 错误注入，验证 close 次数和新分配描述符不被误关；避免仅测试正常提交。

## Secondary Design / Packaging Risks

以下需要保留，但不应阻断主问题的修复次序：

- `NativeArtifactMaterializer.cpp` 的 filename 合成缺少完整的绝对路径、`..` 和 symlink containment 检查。实际 serving 模式拒绝 `--artifact-references`，因此目前证据支持本地/兼容工具的路径风险，**不支持直接称为网络攻击者可任意写文件**。明确文件名契约并加负例，或隔离旧入口。
- Repo 的 `wscript` 静态库 `install_path=None`，缺少完整原生 SDK 安装闭包；`RepoSourceProvider.hpp` 又依赖 DI 内部头。如果目标包含外部 C++ Repo consumer，应拆分稳定 Repo API 与 DI 集成 adapter，并测试安装后独立编译。当前树内构建成功不证明可独立安装；这不是已确认的树内运行缺陷。

## Important Non-findings

1. `NativeInferenceClient` 已有满足上下文条件时进入真实 Core request 的路径；NOT_READY fallback 不能代表所有 native 调用。
2. Conversation 已有内部 terminal 通知，旧的“只能靠用户异步完成回调释放 gate”结论不能原样复用。
3. 发布成功后释放 transient source 的机制已存在；本次 F02 指准备峰值，而不是声称永远不释放模型。
4. 受保护 Repo 写入已使用 operation ownership、commit-if-owned、read-if-current 和 remove-if-current。不能仅凭旧兼容 API 推断新路径完全没有隔离。
5. `RepoNode` 中的旧服务注册不是当前 Python versioned network adapter 的完整授权机制；后者存在 request ownership enforcement。本轮没有依据宣布生产 Repo 无授权。
6. Protected Runtime 已有 grant 绑定和阻塞读取后的时间复核。KV 状态也有 session/conversation scope 和缺失状态检查。这些是实际机制，不等于已完成全部安全性或真实模型验收。

## Proposed Repair Order

| Batch | Scope | Exit evidence |
| --- | --- | --- |
| A | F03/F04/F05/F09：Repo 快照、历史缺口、预算和错误路径 fd 所有权 | C++ 生产 Repo 反例测试；Python 网络目录本身的持久化/同步测试。Python 测试不能替代 C++ Repo 行为验收 |
| B | F01/F02：protected 原子材料 consumer、有界准备与读取 | 生产路径材料名称/字节审计、内存 owner 及取消证据；随后才跑真实模型 |
| C | F08、F06/F07：turn owner、兼容分段提交和本地 durability | 确定性并发/失败注入；按当前实际使用的路径分配优先级 |
| D | 安装闭包与兼容入口收敛 | 干净 consumer 编译、独立安装/导入检查及明确 API 边界 |

每批先冻结源码和静态反例审查，再构建受影响目标并运行对应测试。跨层行为变更时同步中文契约、当前/目标设计和 Spec 任务；不能为了匹配当前实现而降低用户接受的目标。

真正的两 Provider 资格验收仍必须观察 `prepare → Repo publication → ACK → Selection → placement-bound fetch → assembly/execute → terminal → cleanup`。F01/F02 与这条主链直接相关；其余发现是独立正确性风险，不自动解释 r36 或先前的 stream-gap。

## Validation and Handoff

- 本轮实际执行：文件清单/摘要、Python AST 解析、Spec 结构检查、源码交叉追踪、已有失败证据复核。
- 没有执行：新的原生编译、运行时反例、SIF 构建、MiniNDN、GPU 推理或全量资格测试。
- 没有独立 review-agent 复核，不授予正式 STATIC_PASS。
- 报告与任务记录的 `git diff --check` 通过；结束时复核的 `NativeCanonicalOnnxAssembler.cpp`、`RepoCore.cpp`、`RepoClient.cpp` 和 `Conversation.cpp` 摘要与读取时一致。
- 本报告与任务检查点作为审查记录保留；工作树有并行改动，引用行号和文件摘要必须在实施前重新核对。
- 本轮记录未提交：索引已有上一工作单元的安装脚本改动，`tasks.md` 同时包含已暂存与未暂存改动；本次审查保持 PARTIAL，不将不同工作单元混合提交。没有 push。
- 下一步：用户确定修复范围后，先实现 Batch A 的确定性反例和局部修复，同时为 Batch B 形成明确的 protected material-reader 接线契约；不要先重复启动昂贵模型实验。
