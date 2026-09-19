# DI / Repo Repair Design Analysis

## Conclusion

**Status: PROPOSED / ANALYSIS_ONLY**。本文是对[静态审查](di-repo-design-static-audit-20260919.md)的修复设计分析，不是已经接受的目标设计，也不代表源码修复或验收完成。

建议不是九个独立补丁，也不是重写 DI/Repo，而是：**先修小范围确定性缺陷，再让材料读取、存储提交、预算和执行所有权各自只有一套明确契约。** 复用现有实现，避免出现新旧两套长期并行的状态机。

“优雅”应体现为减少需要同时成立的隐含条件；“高效”应体现为有界内存、少读不需要的材料、可解释的锁和持久化开销；“长远”应体现为重试、升级、撤销、重启和多个调用者仍遵守同一契约。现在不能宣称这些方案已获得性能收益，收益需要测量。

## Current Baseline Correction

本次复核发现并行开发已改变 F01 的现状：

- `NativeCanonicalOnnxAssembler.cpp` 已加入 `materialMetadataPresent` 分支，读取经过身份/摘要/长度绑定的 material manifest，并按照 `projection.assembly.nodeIndices` 选择节点及其 initializer；显式 material 格式无效时拒绝，不退回完整 source。
- legacy root 仍走原有 source 路径。这是版本兼容路径，不能和 material 格式失败后的降级混为一谈。
- [最新材料 consumer 证据](b189-material-consumer-20260919.md)报告 C++ material-only 正负例及 assembly suite 8/8 通过，状态是 `LOCAL_MATERIAL_CONSUMER_PASS`；本轮未重跑这些测试。
- 因此，**F01 的“尚未接入”已被后续局部实现取代**。剩余工作是真实生产授权入口、实际材料传输、两 Provider 执行和资源回收的完整验证，以及进一步的有界读取设计。
- `fetchPlainObject()` 仍先取得完整 Buffer，再检查实际大小，并复制成 vector；prepare 的 `deriveNativeCanonicalMaterialManifest()` 仍积累独立 payload。累计声明字节预算已改善，但不等于所有网络分配、复制和并发峰值都已受同一预算控制。

复核时 assembler SHA-256 为 `f825b9c070ee9443745c6be955296df0560d5d4802c9f57655d8cac09a7a1408`；RepoCore、RepoClient 和 Conversation 摘要与上一轮一致。工作树仍在并行变化，实施前要再次冻结边界。本轮 Context Mode health 找不到对应项目 ContentDB，使用仓库源码和证据作为依据，不据此操作或重建其索引。

## 1. Material Access: Fix the Data Boundary, Not Each Model

### Recommended design

保留现有 material manifest 和 protected fetch 路径，逐步把“返回完整 bytes 的 source”收敛为一个内部材料读取契约。它描述：不可变模型身份、材料身份、长度、摘要，以及有 owner 的读取方法。**不是立即再发明一套公开 wire schema。**

职责分开：

| Layer | Owns | Must not own |
| --- | --- | --- |
| Core | 已有请求/Selection/grant 验证、通用受保护 Data 读取 | ONNX 节点、模型算子语义 |
| Repo | 精确命名对象、不可变提交、持久性、容量 | 判断哪些模型节点属于某个推理角色 |
| DI adapter | 根据已经验证的 placement 计算所需材料集合、核对模型身份、组装 | 绕过 Core 授权，或让 Repo 理解模型执行图 |

一次执行的顺序保持：验证本次授权与分配 → 验证 manifest → 求所需材料集合 → 申请预算 → 按精确名称读取并验证 → 组装 → 执行。缓存命中也经过本次授权检查；内容摘要相同只能支持字节复用，不能证明当前调用有权使用这些字节。

当前 manifest 直接列节点所需 initializer 时，按该格式解析即可；若未来支持嵌套依赖，再明确闭包、重复引用和环检测规则。不要现在引入通用工作流求解器。

### Efficiency and cost

令全部模型材料大小为 S，Provider i 真正需要的材料大小为 Sᵢ，manifest 大小为 M。忽略协议/加密开销、首次读取且无缓存时：完整模型读取约为 P×S；选择性读取约为 ΣSᵢ + P×M。共享参数可能被多个 Provider 读取，因此不能保证 ΣSᵢ≤S；极端情况下选择性收益很小。

源码中多次 `find_if` 查 references，可在验证 manifest 后建立一次不可变索引，避免选择每个节点或 payload 时线性扫描。哈希索引的预期代价可接近 O(N+E) 建索引、O(Nᵢ+Eᵢ) 查询；有序索引则有对数查找成本。是否优化先看实际 manifest 规模，不把所有 map 重写作为接线前置条件。

保留语义材料粒度，但不必“一份材料一个磁盘文件”。小材料可在后续合并为 pack；pack 的 offset/length/digest 必须经过验证，加密时还要有可独立认证的 chunk 边界。不能把明文 byte range 直接套到整对象 AEAD 密文上。

### Acceptance

必须测到选中角色读取的实际名称/字节、缓存命中后的授权检查、损坏材料拒绝，以及旧格式兼容性。不能只看最终 output 正确，因为“完整模型都读了但结果正确”仍可能违反设计目标。

## 2. Preparation Memory: Use Bounded Ownership, Not Just Move Semantics

把一个 `const vector` 的复制改成 move 可以减少一次临时分配，但所有 payload 仍一起驻留，所以它是低风险局部优化，**不是长期解决方案**。

建议两步推进：

1. 在现有 schema 下先逐个物化、计算摘要并发布材料；保留 receipt/descriptor，不在准备目录累计已提交 payload。源文件由明确的生命周期 owner 保持稳定，结束或取消后释放。路径本身不是不可变保证，需使用受控快照/只读 spool，并核对大小、摘要与读取范围，避免发布期间源文件被修改。
2. 对特别大的 initializer 增加 bounded reader/writer，使序列化、加密、存储共用有界窗口。若暂时仍依赖整 TensorProto 序列化，必须明确峰值下界还包含最大单个 tensor，不能声称内存与模型完全无关。

设 G 为 graph/索引内存，W 为单次物化工作区，B 为传输窗口，c 为同时在途窗口数；可以将准备阶段的额外内存目标写为 **O(G+W+cB)**，但只有真正打通流式序列化/加密/写入后才成立。初期 W 可能等于最大 tensor 大小。模型最终由 ORT 持有的权重和 KV cache 另计，不能藏进准备预算之外后宣称总内存有界。

推荐“有界文件暂存 + 完整验证后交给 ORT”，而不是为了零拷贝让 ORT 直接消费尚未认证的网络内容。必要时 mmap 可以减少复制，但仍消耗地址空间、页缓存和物理内存，不能把 mmap 等同于不占内存。

## 3. Repo Catalog: First Make the Snapshot Correct, Then Make Recovery Explicit

### F03: minimal repair first

先把 inventory 读取和 epoch 读取放在同一个一致性边界内，并核对 Core→store 锁顺序。对现有单进程、所有写入经 Core 的实现，这是比引入 MVCC 更小、更容易验证的修复。

代价是列出 N 个对象期间写入可能等待 O(N) 的扫描时间。只有这个锁成为实测瓶颈后，才使用后端 snapshot/read transaction；分页不能每页重新读“最新目录”，必须固定同一 snapshot/cursor。若后端允许绕过 Core 修改，单个 Core mutex 本来就不够，必须明确禁止或由后端统一事务。

### F04: durable state plus bounded history

长期契约应把三类内容分开：

- 当前对象目录及删除状态；
- 有界变更历史；
- 表示目录来源和位置的 cursor。

cursor 需要区分逻辑目录身份和 sequence。持久 journal 连续时，普通进程重启不必改变逻辑目录身份；数据库重建、回滚或历史连续性丢失才需要新 incarnation。不要让每次重启都无条件触发全量同步，也不要把 bootId 当成足以解决历史缺口的机制。

delta 返回明确的可用历史下界；cursor 太旧或来源不匹配时返回 snapshot-required。接收方应**替换该来源的目录视图**，不是只把新 entries 合并进去，否则全量快照中缺失的旧对象仍可能残留。其他来源的同名对象不受此次替换影响。

最重要的提交约束是：对象可见性、目录记录和 revision 之间必须可恢复地一致。文件系统与数据库不是一个原子事务，不能简单写一句“放进同一事务”就认为解决：先持久化不可变 payload，再在目录/journal 的同一个数据库事务中发布引用；崩溃后清理未引用 payload。没有数据库的后端也需等价的 durable commit record 和恢复流程。尽量复用现有 store，不另建竞争的权威目录。

代价：多一次可解释的 journal/metadata 持久化和有界历史空间；收益：正常增量同步保持小成本，落后太久时明确付出 O(N) snapshot 成本，而不是静默漏数据。

## 4. Capacity: One Accounting Contract, Different Backend Implementations

F05 不应只在普通 put 中随手减一次 `m_reservedRangeBytes`；那容易与 staging 已占空间重复扣减。

统一定义逻辑预算：已提交且仍占用的字节 C、已物化暂存字节 T、已预留但未物化额度 R。新增长预算 Δ 的准入条件为 **C+T+R+Δ≤Q**。实现可以采用不同表示，但三个集合不得重叠。另查真实磁盘可用空间和安全余量，这与逻辑配额是两个约束。

reserve → write → commit 应是预算所有权转移：写入消耗 reservation，提交把 staging 变为 committed；不能每走一步都重新加一遍。替换旧对象时，旧版本未被释放前仍计入 C。ftruncate/稀疏文件的逻辑长度与实际分配块大小不同，必须声明 Q 控制哪一种，不能混用。

使用一个内部、可移动的 reservation owner 表达预算所有权；不是每个 API 各自维护一个计数器。取消和析构只释放自己尚未转移的额度；崩溃恢复根据 durable staging/commit records 重建，不能依赖析构函数在掉电时运行。

收益是所有 vector/range/packet 写入使用同一预算规则，后续并发写入更容易推理。代价是需梳理全部 mutation 入口，属于中等规模修复，不建议与整个存储后端重写混在一起。

## 5. Immutable Publication: Share Commit Semantics, Not a Giant Transaction Framework

F06/F07 的共同原则是“先准备不可变对象，再发布可见引用”，但不需要给整个 Repo 引入分布式原子事务。

- segment 名称使用明确的版本/内容标识；最终 manifest 精确列出 segment 名称、长度和摘要。NDN 缓存中的旧 segment 因名称不同不会变成新对象的一部分。
- 新版本对象全部验证且持久后，CAS 更新当前引用；固定别名如需保留，只作为可更新入口，不把原地覆盖的 Data 当作不可变对象。
- 相同 operation ID 加相同内容应返回原提交结果；相同 operation ID 加不同内容必须冲突。operation、内容版本和目录 revision 是不同概念，不必为了统一名字而混成一个整数。
- Python 本地后端补齐文件/目录 fsync 和 crash recovery，或准确收窄其 durability 声明；不能由原子 rename 推导掉电持久。
- 删除/GC 必须尊重读 lease 和活跃提交；只有失去引用、没有本地使用者且越过清理条件的对象才能回收。失败清理只能删除本次创建且未被引用的对象。

这会增加短期新旧版本并存空间，但换来失败时旧版本仍可读、缓存身份稳定和重试可解释。配置低存储设备时必须预留该空间，不能承诺零额外磁盘成本。

F09 的 fd 重复 close 则应立即局部修复：采用已有风格的 RAII owner，确保所有权只释放一次，保留原始错误；不必等待上述提交架构。对于 rename 已成功但目录 fsync 失败的情况，保留“提交可见但持久性不确定”的结果并进行 reconciliation，不把它当成可安全随意回滚的普通失败。

## 6. Execution Ownership: Make an Old Turn Unable to Modify a New Turn

F08 首先采用小修复：`activeHandle` 安装、异常清理、subscription 安装以及 terminal 清理全部核对同一个 generation。不要为这个问题立即重写 Conversation。

随后可把本轮状态收敛到一个内部 turn owner：generation、handle、取消状态和资源 lease 有一个共同 owner。旧回调只操作捕获的本轮 owner，不直接清空共享的“当前请求”。close 与“handle 尚未返回”的竞态必须由启动中的 owner 承接取消，待 handle 到达后执行，而不是遗漏取消。

mutex 只保护状态转移；不在锁内执行用户回调、阻塞等待或复杂 cancel 回调，以防死锁和重入。terminal 只发布一次，取消与完成竞争也必须有确定的赢家和清理规则。

暂不建议仅为修这个竞态全面改成 actor/strand：那会改变调度、阻塞和回调契约。只有整个 runtime 明确采用统一 executor 时，再把该方案作为单独迁移评估。

## 7. What Not to Do

- 不通过提高超时、降低内存保护门限来把资源失败变成 PASS。
- 不为每个模型增加独立的材料读取和预算特例。
- 不通过缓存完整模型回避当前角色的按范围获取目标。
- 不把内容去重等同于跨身份授权共享；共享密文字节也不意味着共享解密密钥。
- 不为了解决 snapshot 竞态直接引入分布式共识。
- 不把 Python 网络编排全面改成 C++ 作为本批前置工作；先统一契约，native 行为仍以 C++ 生产测试验收。
- 不一次性删除 legacy 路径；先隔离格式入口、记录实际调用，完成迁移后再删除。

## 8. Sequence and Acceptance

| Priority | Unit | Why / exit |
| --- | --- | --- |
| P0 | F03 snapshot 锁边界、F09 fd owner、F08 generation 反例与局部修复 | 修改面小；确定性 C++ 反例通过。F08 的并发支持契约先核实 |
| P1 | 当前材料 consumer 的生产接线验收；F02 准备峰值和 fetch 预算贯通 | 主资格链直接依赖；使用当前已有 consumer，不重复实现 |
| P1 | F05 统一预算 | 在并发大材料写入前建立安全配额；测试混合写入和失败释放 |
| P2 | F04 历史缺口与持久目录；F06/F07 兼容写入与 durability | 长时间运行、离线恢复、崩溃恢复的必要能力；不强行作为单次无故障推理的全部前置条件 |
| P3 | 材料索引优化、pack、独立 SDK 边界、清理过时入口 | 有测量/外部调用需求再投入，避免为“优雅”扩大范围 |

每个单元的验收应包括：一个违反契约的反例、修复后相同反例、兼容正常路径、取消/失败清理，以及适用时的重启恢复。C++ 行为由 C++ production target 验证；Python 仅测试其自身网络编排/持久化边界，不能替 native 实现出具 PASS。

最终仍需实际观察 prepare、Repo commit、ACK、Selection、按分配材料读取、assembly、执行、terminal 和资源回收。组件测试通过后只运行受影响范围；完整主链需要的能力尚未准备好时，不重复跑昂贵模型试错。

## Validation Boundary

本轮核对新旧源码身份、material consumer 证据及关键读取/所有权分支；未改产品代码，未新运行编译或实验。没有把建议写入冻结目标 API 或把它们标为已实现。旧记忆只用作复核线索，不作为当前行为依据。

文档链接检查和限定文件的 `git diff --check` 通过。审查记录保持未提交，避免把已有安装脚本暂存改动、并行任务记录与尚未接受的设计建议混为一个 checkpoint；没有 push。

**推荐下一步**：先接受内部边界与修复优先级，再为 P0 和 P1 各建立小而独立的实施单元。真正长期有效的是减少状态与所有权的重复，而不是增加类名或设计图数量。
