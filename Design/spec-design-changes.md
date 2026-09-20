# Spec 设计变更记录

## Spec189 protected source-staging cleanup boundary — 2026-09-20

- **Status**: `PARTIAL` / internal native resource-ownership repair。受保护的
  Post-Selection assembly 在 worker 已消费并校验 canonical source 后，先释放
  `canonical.onnx` staging 文件；该早期释放现在复用受保护目录的 pinned fd，
  对普通文件执行覆盖、`fsync`、`unlink`，再进入 ciphertext sealing 和
  request-scoped plaintext materialization；删除失败显式报错，不静默假设资源已
  回收。受保护
  artifact directory 以 runner-spec lifetime 转交 Provider cache，覆盖 assembler
  返回到 cache owner 安装之间的异常窗口；首个异常仍保留，zeroization/cleanup
  失败写入稳定诊断 marker。
- **Design boundary**: 只减少 protected assembly 的磁盘 working set；不改变
  source/initializer/graph/recipe digest、grant、ACK、Selection、placement、
  runner、terminal 或 ciphertext schema。非 protected plaintext 路径保持原有
  finalization cleanup。r139 首个生产边界仍是 host `diskFree`，该变更待受影响
  C++ selector、安装身份和新的 guarded MiniNDN run 验证。
- **Source / evidence**: `NativeCanonicalOnnxAssembler.cpp`、`Provider.cpp`、
  `ProviderArtifactCache.cpp`、`NativeModelRunner.hpp`；r139 raw run 和
  resource summary 记录于 Spec189 evidence/failure log。v2 read-only review
  报告了早期 `std::filesystem::remove` 绕过 secure erase 的 P1；当前修复把
  eraser 绑定到同一目录 lease fd。受影响 C++ selector、build/install identity
  和新 guarded MiniNDN run 仍待验证；此次变更不能把 r139 或 focused selector
  结果提升为产品资格 PASS。
- v3 read-only review 进一步发现 early eraser 与最终 lease drain 共享 fd
  但没有共享锁，以及 range-backed material 校验仍直接读取空 `data()`。
  当前修复为目录 lease 的早期擦除和最终 drain 增加同一互斥，并让模板/节点
  校验先生成 `copyBytes()`。v4 immutable snapshot 已获 `STATIC_PASS`；仍需
  focused selector、安装身份和 guarded run。

## Spec189 worker post-parse source release boundary — 2026-09-20

- **Status**: `PARTIAL` / internal worker working-set repair. After OA04
  `ownedSourceModel()` has parsed, validated, and copied the authenticated
  source into the child-owned ONNX model, the worker may scrub and release its
  request model/initializer buffers before continuing S4-S7. The in-process
  OA01 entry remains non-destructive.
- **Design boundary**: this changes only child-local plaintext lifetime and
  resident memory. It does not change S1-S7 checks, recipe/identity digests,
  ORT loading, worker framing, grant, ACK/Selection, placement, or terminal
  contracts. The release is after the source-consuming call returns; failure
  before that point retains normal exception cleanup.
- **Source / evidence**: `NativeOnnxRecipeAssembler.cpp` and
  `NativeOnnxAssemblyWorker.cpp`; r131 remains a
  `RESOURCE_BOUNDARY:ownedSwap` with no protocol or qualification result.
  The r132c affected build `538/538`, material selector `2/2`, and
  ONNX/Repo selector `30/30` passed; installed-runtime identity must still
  pass before the next guarded MiniNDN retry.

## Spec189 worker certified-chain move boundary — 2026-09-20

- **Status**: `PARTIAL` / internal native working-set repair。OA04 certified
  chain 在 S5 shape inference 前保留经过认证 node 的 deterministic bytes，
  然后 move authenticated `original` into `inferred`，不再深拷贝包含完整
  external initializer 的 protobuf model。
- **Design boundary**: 只减少 worker 内部重复 source/model allocation；S1–S7
  checks、recipe/node coverage、identity digest、ORT session、worker framing、
  grant/ACK/Selection/placement/terminal contract 不变。r129 仍在真实
  MiniNDN assembly 前被 host `MemAvailable` guard 停止，不能提升资格。
- **Source / evidence**: `NativeOnnxRecipeAssembler.cpp`；r130 targeted
  build `538/538`、material selector `2/2`、ONNX/Repo selector `30` cases
  are the focused gates. 下一步是安装受影响 worker/DI targets 并以新 run root
  做 guarded MiniNDN retry。

## Spec189 worker parent-source release boundary — 2026-09-20

- **Status**: `PARTIAL` / internal native API ownership adjustment。OA02
  worker transport 新增可选 `sourceToReleaseAfterWrite` 参数；生产
  `NativeCanonicalOnnxAssembler` 在请求帧完全写入 anonymous pipe 后 scrub
  并释放父 Provider 的 model/initializer/material source，既有默认调用保持
  non-destructive。该写入完成屏障之后子 worker 已拥有唯一仍需的请求副本。
- **Design boundary**: 只改变 assembly parent/child 的 resident working-set
  与明文生命周期，不改变 recipe digest、grant、ACK、Selection、placement、
  terminal 或 worker framing。若写入未完成，source 仍保持有效；失败路径继续
  由 worker transport 和 assembler cleanup 负责回收。r121 的宿主资源边界尚未
  通过，不能提升 Spec189 资格。
- **Source / evidence**: `NativeOnnxAssemblyWorker.{hpp,cpp}`、
  `NativeCanonicalOnnxAssembler.cpp`；详见 [r121 resource evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r119-range-source-focused-check-20260920.md)。
  下一步是 affected C++ selectors、installed identity check 和全新 guarded
  MiniNDN retry。

## Spec189 native runner/artifact cleanup boundary — 2026-09-20

- **Status**: `PARTIAL` / internal native API change。`NativeModelRunnerSpec` 新增
  `std::shared_ptr<const void> lifetime`，由活动 runner 持有请求/缓存材料；cache
  publication 会清除 metadata template 的 owner，避免缓存条目把自己的 lease 永久
  固定。`NativeCanonicalOnnxAssembler` 暴露进程内
  `withNativeArtifactDirectoryFinalization(directory, action)`，Provider owner cleanup
  与 assembler finalization 共享同一收尾锁，并在 canonical physical path 下执行。
- **Design boundary**: 该修复只约束 native artifact 的所有权、淘汰和同进程目录收尾，
  不改变 grant、ACK、Selection、placement、Repo wire 或模型输出协议。跨独立进程共享
  cache directory 仍未提供锁，不能宣称跨进程并发安全。
- **Source / evidence**: `NativeModelRunner.hpp`、`Provider.cpp`、
  `ProviderArtifactCache.cpp`、`NativeCanonicalOnnxAssembler.{hpp,cpp}`；详见
  [B189 r8/r91 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-memory-lifecycle-r8-and-r91-20260920.md)。
  冻结范围官方 `review-agent` 返回 `STATIC_PASS`，DI target closure 与 focused C++
  selectors 通过；真实 Qwen r91 在两个 Provider ready 后因宿主 disk-free guard 停止，
  因此仍为 `PARTIAL`。
- **Documentation boundary**: API Markdown/JSON/PDF 的完整生成必须在干净文档
  checkpoint 重新运行；本条不能把工作树中的旧 API snapshot 视为已同步。

## Spec189 native memory release boundary — 2026-09-20

- **Status**: `PARTIAL` / internal API ownership adjustment。prepare publication 在
  material manifest 已认证后以不可变 `shared_ptr` 快照替换 full source；Post-Selection
  role model 物化后释放 selected material payloads/manifest。`sourceRefFor` 返回 owning
  immutable view，避免 catalog 原地清空造成并发读者失效。
- **Design boundary**: 这只改变 native working-set 和 source ownership，不改变模型摘要、
  grant、ACK、Selection、placement 或 terminal 协议；必须保留 PreparedModel/provider
  lease、ORT session/conversation state 和 protected Repo lease 到终态。
- **Source / evidence**: `NativeCanonicalPreparationCatalog.{hpp,cpp}`、
  `NativeCanonicalOnnxAssembler.cpp`、`ModelPreparationCache.cpp`、`Runtime.cpp`；详见
  [memory lifecycle audit](../specs/189-qwen-two-provider-minindn/evidence/b189-memory-lifecycle-static-20260920.md)。
  DI/selector compile-link 与两个定向 C++ selector 通过；冻结范围的官方
  `review-agent` 复审为 `STATIC_PASS / TESTS_DEFERRED`。完整 selector 仍有既有
  cancellation/staging fixture 失败，不提升 Spec189 产品验收。

## Object handles and replica locators — 2026-09-19

- **Status**: DOCUMENTATION_ONLY / NO_PUBLIC_API_CHANGE。用户接受 R6；适用 G3–G5、R1–R6、D2–D3。Repo 管理引用与副本定位，网络部署提供可达路由，DI 保持材料身份及执行范围。
- **Boundary**: handle 不授予权限、不延长 lease、不默认包含明文密钥；位置迁移不改变对象摘要。具体序列化、签名者规则、位置刷新和真实 NFD 取数尚待下层设计与验收；未更改源码、API 清单或冻结 PDF。
- **Validation**: 高层文档链接和限定 diff 检查；没有构建或运行实验。下一步定义 handle/位置记录契约及源离线、副本切换、过期位置的验收。

## Repo modes and DI ownership — 2026-09-19

- **Status**: DOCUMENTATION_ONLY / NO_PUBLIC_API_CHANGE；用户接受两模式方向，新增高层 R5，适用 G1–G6、R1–R5、D1–D4。
- **Owner**: Repo 管理存储、模式准入与读取生命周期；DI 管理准备材料、placement、缓存和 KV 语义。应用主动获取后本地缓存不视为远端 INSERT；显式 server 归档不视为 in-app 自动复制。
- **Evidence**: [两模式审查](repo-two-mode-analysis-20260919.md)，本轮核对 PreparedModel.cpp 的 repository input 和 ModelPreparationCache.cpp 的持有/退役 lease。高层链接、diff 与模式/DI 边界检查通过；没有运行测试或提升 Spec189 资格。
- **Remaining**: 统一写入/恢复/修复入口门控及服务路径验收待后续实施；当前 API/PDF 和冻结目标不因高层原则调整而改写为已实现。

## Reusable base and complete runtime SIF — 2026-09-19

- **Status**: DOCUMENTATION_ONLY / NO_PUBLIC_API_CHANGE。用户要求将可复用 base 与后续 NDNSF 更新流程纳入 [高层设计 S1–S3](highlevel-design.md#sif-构建与复用约束)，采用已声明外部依赖/SDK base 加容器内构建的 NDNSF，输出新的完整 SIF。
- **Boundary**: 不原地修改 base；依赖/工具链/ABI 变化重新验证 base，普通源码更新复用依赖。最终运行不依赖外置应用包或相邻 base；沿用维护入口和 Apptainer 1.5.3。本轮未改源码/API/冻结目标 PDF、未启动构建或实验。

## Build/install and MiniNDN constraints — 2026-09-19

- **Status**: DOCUMENTATION_ONLY / NO_PUBLIC_API_CHANGE。用户新增 [B1–B3、M1–M3](highlevel-design.md#编译安装与-minindn-实验约束)，明确 NDNSF 根 Waf 与外部依赖构建系统的责任，以及所有 MiniNDN 运行的系统安装边界。
- **Impact**: 使用构建树被测程序的现有启动器必须迁移后才能满足新约束；本轮仅更新文档，不启动构建、安装或实验。原始记录保留，运行目录仍可保存配置和数据。

## High-level design baseline — 2026-09-19

- **Status**: DOCUMENTATION_ONLY / NO_PUBLIC_API_CHANGE。按用户要求提炼 [highlevel-design.md](highlevel-design.md)，为四模块确立可引用的 G1–G6 与模块原则；当前摘要与必须保持的约束分开。
- **Scope**: README、MANAGEMENT 和架构阅读入口增加原则检查。保留现有未验收项和冻结目标，不将建议修复写为当前已实现；当前/目标 PDF 输入未变，本轮不重建。
- **Validation**: 四模块各 500–1000 字，链接及限定 diff 检查通过；后续代码修改须说明适用原则和证据。本条是文档治理交付，不关闭 Spec189 的任何产品任务。

## Spec189 authenticated assembly progress handoff — 2026-09-19

- **Status**: `PARTIAL`。B189-3 为 post-Selection assembly 增加了生产 C++ 进度回报和精确消费绑定：`NativeCanonicalOnnxAssemblerOptions::reportProgress` 在已认证里程碑回调，`StreamEventConsumer::observeAuthenticatedProgress` 只接受同一 request/provider/service、Selection digest、terminal-role operation、严格 epoch/sequence 和有效期限；`ServiceUser::initializeStreamConsumer` 传递 expected operation ID。
- **设计原因**：r53 的真实运行已到 grant verification 和 assembly staging，但 requester 因 stream event gap 超时，日志没有可验证的中间阶段。generic heartbeat 不能证明 assembly；进度必须由生产 assembler 的真实 root/material/source/worker 里程碑产生，并由 requester 绑定到本次 Selection。
- **当前/目标边界**：本地 C++ lifecycle、same-provider multi-role streamed fixture、D2b 和 worker-backed assembly selectors 已通过；初次 fixture 契约错误已由只读复审后的 test-only metadata 修复消除，完整 assembly suite 现为 9/9。真实 Qwen/MiniNDN、terminal/output/drain 和跨 service 负例未完成。不得将本条提升为资格 PASS。
- **源码与证据**：`ndn-service-framework/InvocationStream.{hpp,cpp}`、`ServiceUser.{hpp,cpp}`、`NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.{hpp,cpp}`、`Provider.cpp`、`examples/DI_NativeProviderExecutable.cpp`；详见 [B189-R4 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r4-progress-heartbeat-20260919.md) 和 v6 review snapshot。
- **文档同步边界**：本条登记当前源码行为和剩余验证；API Markdown/PDF 的完整生成与源码摘要刷新仍须在同一干净文档 checkpoint 按 `Design/MANAGEMENT.md` 完成，不能把工作树中的旧 API reference 当作已同步。

## Spec189 pre-root assembly admission correction — 2026-09-19

- **Status**: `PARTIAL`。r56 的真实候选已到 ACK/Selection 和 `BEFORE_ASSEMBLY` grant verification，但 requester 在 Provider 产生可观察 assembly 事件前因 stream gap 终止；这不是 ORT、Repo 或模型 PASS/FAIL 结论。
- **Design change**：将第一个 authenticated `ASSEMBLY_STARTED` milestone 前移到 root-material fetch 之前，覆盖 post-grant 的静默 admission 窗口。该变化不新增授权决策、不提高 timeout，也不改变 Selection digest/operation binding。
- **Evidence**：冻结修复快照获官方只读 `STATIC_PASS`；受影响 C++ closure 以 Waf `-j3` 重建，`Spec175InvocationStreamLifecycle` 15/15、`Spec175NativeAssembly` 9/9 通过。真实重跑及 terminal/output/drain 仍未完成，详见 [r56 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-real-qwen-r56-20260919.md)。
- **Documentation boundary**：B189-3 的下一个且唯一优先出口是用重建候选做一次真实 retry；在观察新的第一生产边界前，不扩张组件职责、不盲目增加 timeout，也不把本地回归提升为资格 PASS。

## Spec189 provider-specific collaboration progress binding — 2026-09-19

- **Status**: `PARTIAL`。r70 真实运行已到 authenticated assembly admission 和 selected-material fetch，但 terminal stream consumer 在非 terminal Provider 继续组装时仍因 stream gap 终止。
- **Design change**: streamed collaboration progress now binds exact `{providerName, providerSelectionDigest, operationId}` tuples. Selection digests are Provider-specific because each key envelope is recipient-bound; freshness `(epoch, sequence)` is tracked per tuple so worker-to-terminal progress handoff is accepted while stale duplicates remain rejected. Legacy single-Provider consumers retain their exact operation binding.
- **Evidence**: r70 boundary is recorded in [native-r70 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-native-r70-progressed-stream-boundary-20260919.md). The repair requires the reviewed C++ selector and a new candidate run; no qualification PASS is claimed.
- **Documentation boundary**: this entry records the current repair contract and its open validation. Generated API Markdown/PDF remains a separate MANAGEMENT checkpoint and must not be inferred from this working-tree entry.

## Spec189 Selection-scoped admission sequence — 2026-09-19

- **Status**: `PARTIAL` / `NO_PUBLIC_API_CHANGE`。为修复 assembler/rebuild 重建时重复从 sequence 2 开始的问题，`NativeSelectionProjectionV3` 现在携带不参与 JSON/canonical digest 的 runtime-only shared atomic counter；同一 authenticated Selection 的 admission 与每次 runner factory copy 共享它。
- **Design change**：`GRANT_VERIFIED` 后先报告 epoch 1、sequence 1 的 `ASSEMBLY_ADMISSION`，随后 assembler milestones 使用同一 counter 产生 sequence 2、3……。这保持 operation identity、授权和 Selection wire 不变，只闭合 Core 严格非零/单调序列约束。
- **Evidence**：v4 冻结快照获官方只读 `STATIC_PASS`；受影响 closure 真实编译链接成功；C++ lifecycle 1→2→3 与 `Spec175NativeAssembly` 9/9 通过。r58 在 ACK/Selection 前触发 `RESOURCE_BOUNDARY:diskFree`，因此没有真实 admission/assembly 结论。见 [admission sequence/r58 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-admission-sequence-r58-20260919.md)。
- **Documentation boundary**：该内部状态不构成 Qwen、MiniNDN、ORT、output 或 reuse PASS；T006/T007/T009 继续 `PARTIAL`/blocked。

## Spec189 local MiniNDN runtime boundary — 2026-09-19

- **Status**: `NO_DESIGN_CHANGE`。本机 Spec189 验收直接使用宿主机已安装的全局 NDNSF/Core/Repo/DI 依赖和本机构建的 C++ APP；SIF/Apptainer/TigerCluster 不属于本地 MiniNDN 前置条件。
- **Evidence boundary**：r58 的 `RESOURCE_BOUNDARY:diskFree` 只表示宿主机工作区/运行时文件系统低于安全门；本轮没有启动或读取 SIF，也不能把该边界归因于 SIF 构建或容器内容。
- **Delivery boundary**：SIF/Tiger 保留给后续独立交付和远端资格，不改变 Spec189 的本地 C++/MiniNDN 完成门。

## Spec189 causal evidence checker — 2026-09-19

- **Status**: NO_PUBLIC_API_CHANGE / PARTIAL。仅增加 Provider 内部日志 `preparationId`（request/role owner 下每次 runner preparation 调用身份），不改变公开签名、授权、模型数据、operation status sequence 或 wire。
- **Evidence**: [causal oracle evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-causal-oracle-20260919.md)。C++ checker 按 ID 配对多 epoch 的 assembly/ready，并让正常 CLI 必经材料与末段终态门；这不是模型正确性或 MiniNDN 资格。
- **Documentation boundary**: 不将日志判据视为新目标 API，不覆盖冻结当前/目标 PDF；完整 Spec189 交付仍按原设计同步门验收。

## Spec189 DI/Repo repair analysis — 2026-09-19

- **Status**: NO_DESIGN_CHANGE。本轮仅评估静态审查问题的修复选项，建议为 PROPOSED，未修改产品源码、公开 API 或冻结目标。
- **Evidence**: [repair design analysis](../specs/189-qwen-two-provider-minindn/evidence/di-repo-repair-design-analysis-20260919.md)。已区分新 material-only consumer 的局部验证与真实生产链资格，未将旧的“尚未接入”结论延续为当前事实。
- **Next**: 用户接受边界后再形成实施契约；本轮不重新生成当前/目标 PDF，不宣称设计同步或产品验收完成。

## Spec189 execution and ownership audit — 2026-09-18 14:50 -0500

- **Status**: PARTIAL；本次仅修正 [Spec189 任务与批次](../specs/189-qwen-two-provider-minindn/batch-execution.md)，保留 7 个能力任务，T003 分 protected storage/atomic preparation 两个验证出口。
- **Target clarification**: Core 保留 protected serving、Repo 提供身份绑定范围存储；publication lease 服从 preparation cache 预算/淘汰，避免 publisher 第二强 owner。重型 commit 不阻塞 Core I/O，取消/drain 与同名替代 fence 为 B189-1a 必要出口。
- **Validation**: 请求 active owner 与 idle cache 分账；host guard 前置，native counters 随实际组件交付，真实重复验收前齐备。详见 [审计证据](../specs/189-qwen-two-provider-minindn/evidence/spec189-static-audit-20260918.md#execution-and-ownership-follow-up)。
- **Current boundary**: 已有 protected-store 源码草稿 STATIC_FAIL，当前/目标 PDF 与 API 不在本审计中宣称已同步或通过；B189-1a 实现交付须按 MANAGEMENT.md 同步，T009 最终文档门保留。本轮没有修改产品源码、构建或运行模型。

本文件回答“哪个 Spec，为什么，把哪个模块的什么设计从什么改成了什么，代码实现到哪里”。
这里记录设计影响；Spec 的 tasks.md 与契约继续负责具体任务和验收。CHANGELOG.md 记录文档版本，两者互相引用。

## 记录规则

1. 从本基线起，每个新 Spec 必须登记一条；旧 Spec 后续继续修改设计时也登记。没有设计变化写“无设计变化”并说明依据，不留空白让读者猜测。
2. Spec 建立时记录目标与受影响章节；目标调整时补充前后差异；实施和验收时分别更新实现状态与证据。状态使用 PLANNED、PARTIAL、VERIFIED、NO_DESIGN_CHANGE，不以 Spec 编号或代码提交代替验收。
3. 当前设计只写已核对的实际行为；未实现的目标留在目标设计。部分实现分别列出已接通和未接通部分。
4. 每条保存 Spec 路径、任务/契约 ID、模块、章节标题、变更前后行为、源码提交、文档提交定位和证据。章节标题为主要定位，页码仅辅助。
5. 源码提交使用完整哈希或明确范围；工作树快照必须附逐文件摘要和补丁。文档提交由 `git log -- Design/spec-design-changes.md` 定位，避免在提交内填写自身哈希。
6. 更新两份 PDF、正文、记录和必要的 Spec 进度，同一文档单元核对后提交。回退或取代某项设计时追加记录，不删除历史条目。

## 索引

### 2026-09-18 — Spec189 architecture and progress correction

- **Status: PLANNED / PARTIAL**。本轮只修正 [Spec189](../specs/189-qwen-two-provider-minindn/plan.md) 的目标边界与任务，不修改生产代码或公开 API。prepare 从固定两段最终模型改为拓扑无关原子层/shared 材料与 Repo 可达性；ACK 后规划、Selection 后范围物化。复用现有 Runtime/PreparedModel/Repo，不新增 Qwen API。
- **Current evidence**：基线 `76b26e2c` 加已有工作区实现；Repo 层 payload 与 PreparedModel 复用 selector 是组件证据，实际 requester/assembler 接线未闭合。r25 已到执行/组装入口，未通过完整模型运行。详见 [audit correction](../specs/189-qwen-two-provider-minindn/evidence/spec189-static-audit-20260918.md#architecture-and-progress-correction)。
- **Tasks**：旧 T002/T004 合并 T003、T010 合并 T009；T008 资源门前移。当前/目标 PDF 的冻结 API/源码快照本轮不覆盖；本条登记的是 Spec 内部目标修正，不宣称 PDF 已包含未实现设计。后续 T003/T006 实现改变 API/行为时按 MANAGEMENT 同步中文契约、API 参考与双 PDF，T009 文档交付检查不得跳过。
- **Validation boundary**：仅文档一致性与只读审查，无 native build、模型运行或 MiniNDN PASS；纯文档 checkpoint 不使未变二进制和模型证据失效。

### 2026-09-17 — Spec188 Model Preparation and Disk-Backed Artifact Memory Control

- **2026-09-17 progress gate reset / PARTIAL**：进度审计把本地产品出口进一步限定为 T005、T006、T008、T009、T010、T011；T014–T016 只在 native core 通过后作为 delivery gate，T002/T003/T004/T007/T012/T013 继续作为 follow-up。当前 `RepoSourceProvider::load()` 的完整 vector 组装、缺少默认 owner 注入和 current-candidate zero-republication 仍是开放证据缺口；没有新增 API、生产机制或 PASS。详见 [progress audit r3](../specs/188-model-preparation-disk-backed-memory/evidence/progress-audit-20260917-r3.md)。

- **PLANNED / NDNSF-DI + NDNSF-Repo**：依据当前源码和 Spec185/187 evidence，登记模型制品从 request-time publication 改为 `Runtime::prepare(ModelRef)` 阶段的目标边界。当前 `NativeCanonicalPreparationCatalog`/`NativeCanonicalArtifactPublisher` 仍可能在请求准备中持有和发布完整 source/initializer；`RepoCore`/`RepoNode`/`RepoClient`/`TieredRepoStore` 仍有 vector、BLOB、concat 和 cache copy 路径。目标增加 typed `ArtifactReference`、file-backed/range Repo、bounded hot windows、lease/pin 计量和 authenticated Selection 后 Provider assembly。
- **Before/after**：before，模型 bytes 可在 request-time publisher、encrypted envelope、NDN segment/IMS、Repo `StoredObject::payload`、runtime executor 和 Provider runner 多处同时存活；after（target only），prepare 幂等落盘并返回 reference/lease，request 不再携带或重复发布模型，Repo/Provider 只按范围读取并分别计量 immutable artifact、transfer、materialization、runner 和 input 内存。应用输入 `Input::repository(DataRef)` 保持独立，不被改成模型传输接口。
- **Contracts/tasks**：[Spec188](../specs/188-model-preparation-disk-backed-memory/spec.md)、[model reference](../specs/188-model-preparation-disk-backed-memory/contracts/model-reference.md)、[file-backed Repo](../specs/188-model-preparation-disk-backed-memory/contracts/file-backed-repo.md)、[memory budget](../specs/188-model-preparation-disk-backed-memory/contracts/memory-budget.md)、[tasks](../specs/188-model-preparation-disk-backed-memory/tasks.md)。Spec 状态为 `IN_PROGRESS`；T005 当前为 `PARTIAL`，已接入 `RuntimeConfig::repositorySourceProvider` 与 RepoCore-backed `RepoSourceProvider`，但 provider 仍返回完整 `NativeCanonicalSource` vector，lease 计量、当前 caller 注入和完整请求链仍未闭合。B188-1 r7 发现的 erase 顺序和 physical-usage recovery 缺陷已修复，并在 r8/r9 任务及组合静态门和 C++ selector 中通过；T002/T003 仍为 `PARTIAL`，扩展故障和 TSan 未观测。
- **Source/evidence**：当前事实来自 `NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.cpp`、`NativeCanonicalArtifactPublisher.cpp`、`NativeRequestPreparation.cpp`、`Runtime.cpp`、`Runtime.hpp`、`ServiceUser.cpp`、`NDNSFMessages.cpp` 以及 `NDNSF-DistributedRepo` 的 RepoCore/Node/Client/TieredStore。当前 provider 的 C++ selector、matching host dependency closure 和复审记录见 [B188-3 current evidence](../specs/188-model-preparation-disk-backed-memory/evidence/b188-preparation.md#current-t005-provider-implementation-and-static-gate--2026-09-17)；r14 YOLO publication 行属于旧候选，当前 provider-bound YOLO 尚未重跑。API 清单已用 `build-api-reference.py --changed-only` 生成，但当前 source snapshot/PDF provenance 因并行工作树漂移保持未验证，不能宣称文档门禁通过。
- **Scope correction / PARTIAL**：本轮审计确认核心目标仍是 prepare→reference request→authenticated assembly/drain→YOLO 两轮本地链路；Qwen 完整推理、1.5 GB OS 级压力、TSan/parser-fuzz、远端/跨进程故障和 broad mutation matrix 从核心完成门移为 `FOLLOW_UP`/`UNOBSERVED`。保留原任务编号、历史证据和失败边界，不改变当前/目标设计快照，也不把任何未执行验证写成 PASS。详见 [scope audit](../specs/188-model-preparation-disk-backed-memory/evidence/scope-audit-20260917.md)。
- **Progress/code-reality correction / PARTIAL**：复核确认 RepoCore callback 只属于测试注入边界，`PreparedModelPackage` 仍通过 `NativeCanonicalPreparationCatalog` 保留 source，且 YOLO 两轮仍记录 `LARGE_DATA_PUBLISH_*`；下一批只推进真实 Repo owner/reference/lease 与 zero republication。Provider/Memory/YOLO selector 已按实际 Waf target/suite 修正，未新增 API 或核心机制。详见 [progress audit](../specs/188-model-preparation-disk-backed-memory/evidence/progress-audit-20260917.md)。
- **2026-09-17 r7/r8/r9 Repo repair and validation**：B188-1 r7 发现 manifest-first erase 与 physical-usage recovery 缺陷；当前已修复并由 r8/r9 冻结快照的任务及组合 `review-agent` 复审为 `STATIC_PASS`。r9 canonical affected closure 和三个 Repo C++ selector 在 ASan/UBSan 下通过；真实 ENOSPC/short-write/cancel、erase/fsync ambiguous fault 与 TSan 仍未观测，T002/T003 继续为 `PARTIAL`，不得将 focused PASS 写成产品完成。

- **2026-09-17 T005 provider wiring / PARTIAL**：当前工作区把 prepare-time Repo owner 从测试 callback
  扩展为 `RuntimeConfig::repositorySourceProvider` 与 RepoCore-backed `RepoSourceProvider`。
  Before，Runtime 只能通过兼容 `RepositorySourceLoader` 或本地 file 取得 source；after，首选 provider
  以 typed presence、manifest-first bounded range read/write、一次 miss ingest、digest/size/deadline
  校验接入 `Runtime::prepare`，并在带 receipt 的 publication 后释放 catalog transient source。
  旧 loader 与无 receipt source owner 保留为兼容边界。此为实际 API/ownership 变化，已完成 v4
  `review-agent` `STATIC_PASS`；随后 canonical ASan/UBSan `spec185-runtime` build exit `0`，
  provider miss-ingest/hot-hit、typed lifecycle-error 和完整 Runtime selectors 分别为 1/1、1/1、11/11。
  lease 计量、真实 request/YOLO zero-republication 和完整动态故障矩阵仍未验收；Spec188/T005 保持
  `PARTIAL`。受影响契约见
  [model reference](../specs/188-model-preparation-disk-backed-memory/contracts/model-reference.md)。

- **2026-09-18 T005/T011 code-reality reclassification / PARTIAL**：复核确认 `RepoSourceProvider::load()`
  虽使用 bounded range I/O，仍将块组装为完整 `NativeCanonicalSource` vector；因此不能把 range
  selector 当作 prepare 内存上界证明。当前 `Runtime::open` 和 `spec188-yolo-repeat` 也没有默认注入
  Repo owner，r14 的 `LARGE_DATA_PUBLISH_*` 只能作为旧候选历史证据。该条只修正当前/历史文档分界，
  不改变 API 或生产代码；下一步是 provider-bound current-candidate selector、lease/copy counter
  和两轮 YOLO zero-republication 验证。

- **2026-09-17 scope reset / PARTIAL**：再次审计确认 Spec188 的最短产品目标是当前
  `User::prepare(modelKey, PrepareOptions)` → Repo owner/receipt/lease → reference-only request
  → authenticated Provider assembly/drain → 两轮当前候选 YOLO。原先把通用
  `RepoCore`/`RepoNode`/`RepoClient` 大对象重构、完整 segmented serving、8 路 prepare、GB 级
  RSS/swap、Qwen 数值推理、扩展故障和 SIF/Tiger 放在同一完成门，已改为 `FOLLOW_UP`/外部门；
  T002/T003/T004/T007 的 focused 结果保留为复用基础。`prepare(ModelRef)` 修正为当前真实公开
  入口，不新增第二套 API。此为 Spec 范围和契约文档修正，没有把任何运行结果提升为 PASS；证据见
  [scope audit r2](../specs/188-model-preparation-disk-backed-memory/evidence/scope-audit-20260917-r2.md)。

- **2026-09-18 scope-convergence review / PARTIAL**：在 T005–T011 和 T014 的 bounded local
  exits 已有证据后，Spec188 的活动计划收敛为 Core、Delivery、Follow-up 三层，删除重复的
  scope-reset 叙述，保留历史 evidence 和 task ID 以便追踪。当前 `RepoSourceProvider::load()`
  仍把 range 块组装成完整 `NativeCanonicalSource`，所以本 Spec 只声明 receipt/lease/owner、
  reference-only request、authenticated assembly/drain 和本机 YOLO zero republication；不声明
  GB 级内存上界。T015 使用 Spec188-scoped source/API/profile manifest；全仓库 Design/source
  baseline/PDF 若受其他并行 Spec 影响，记录为 `UNOBSERVED`，不吸收无关改动。当前/目标设计继续
  分离，T015 的 scoped convergence 和 T016 本地 handoff 已完成，SIF/Tiger 保持外部边界与
  `WAITING_EXTERNAL_INPUT`。详见 Spec188 的
  [plan](../specs/188-model-preparation-disk-backed-memory/plan.md)、[tasks](../specs/188-model-preparation-disk-backed-memory/tasks.md)
  和 [convergence evidence](../specs/188-model-preparation-disk-backed-memory/evidence/b188-convergence.md)。

### 2026-09-15 — Spec187 Two-layer Packaging

- **NO_DESIGN_CHANGE / product APIs; PARTIAL / packaging**：用户确认 `base SIF + NDNSF`，外部依赖和 SDK 在现有 base 上增建，本仓库各模块归统一 NDNSF 层。此次改动仅构建脚本、技能与交付归属，不改变 C++/Python 产品 API、请求协议或对象生命周期，因此不刷新产品 API/PDF 快照。实现和验收见 [two-layer delivery](../Experiments/TigerCluster/docs/two-layer-delivery.md)、[Spec187 tasks](../specs/187-yolo-minindn-sif-app/tasks.md) 和 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)；base SDK 已真实验收并封存，NDNSF candidate 仍保持 PARTIAL。

### 2026-09-12 — Spec185 Prepared Model Runtime

- **2026-09-12 21:07 -05:00 / T003+T004 B2 `VERIFIED`**: C-02 preparation design is now implemented for the bounded local Runtime path. Before, `User::prepare` had no verified Package/cache implementation; after, `ModelPreparationCache` owns canonical source inspection, independent graph identity, immutable `PreparedModelPackage`, single-flight/refresh generations, waiter cancellation/deadlines, leases, LRU/byte budget and exactly-once completion. Core `OperationRuntime` remains the generic owner and has no DI dependency. Source range is the frozen B2 snapshot from base `9bdde3cf` with tracked patch SHA `2d1f7772efef5a0cdc689e340599a2b752262abb673de698427eda3b52ff7d9a`; bounded C++ normal/TSan evidence is recorded in [B2 evidence](../specs/185-prepared-model-runtime/evidence/b2-preparation.md). Request, conversation, Provider, Python and cross-process qualification remain `PLANNED`.

- **2026-09-12 17:00 -05:00 / NO_DESIGN_CHANGE**: 逐任务静态门改为按依赖阻塞，允许单主会话编码与独立快照审查重叠；不改产品API、任务依赖或验收，双PDF无需重建。[验证](../specs/185-prepared-model-runtime/evidence/dependency-scoped-dispatch-20260912.md)。

- **2026-09-12 16:24 -05:00 / NO_DESIGN_CHANGE**: tasks Updated及新checkpoint要求分钟与UTC offset；仅进度元数据格式修订，不改API/行为/状态或双PDF。[验证](../specs/185-prepared-model-runtime/evidence/progress-timestamps-20260912.md)。

- **NO_DESIGN_CHANGE / batch execution**: [批次执行表](../specs/185-prepared-model-runtime/batch-execution.md)补齐18任务12批的静态门/共享验证范围，任务卡改为实际顺序；API、owner及产品行为沿C-01–C-09，本轮不改TeX或双PDF。[证据](../specs/185-prepared-model-runtime/evidence/batch-execution-20260912.md)。

- **Core/App revision**: [C-09](../specs/185-prepared-model-runtime/contracts/core-app-boundary.md)确认协议/流/注册已有Core机制，通用运行时/等待/订阅仍需提取。新增T017/T018前置B0C，现18任务12批；模型/会话/KV语义保留DI。PLANNED，源码未迁移。[证据](../specs/185-prepared-model-runtime/evidence/core-boundary-20260912.md)。

- **Implementation design**: [C-08](../specs/185-prepared-model-runtime/contracts/code-design.md)及逐任务Design binding补齐内部类/函数/字段/流程。T016前移到B1后、T003前；具体路径/完整类型/策略来源/continuation/重载问题已修订。[证据](../specs/185-prepared-model-runtime/evidence/implementation-design-20260912.md)。产品仍PLANNED。

- **T016/B2E implementation and validation**: C-08 `CD08/CD09`, `F16`,
  `FN08`, `FLOW03`, and `PO08` are now implemented in the DI planning,
  adapter, catalog, placement, and runner boundaries. Cooperative strategy
  ports carry `ExtensionControl`; registries use explicit replace-before-freeze
  and read-only lookup; the legacy vtable remains advanced compatibility. The
  normal and clang/TSan C++ selectors plus the installed extension consumer
  pass; full packaging and later request/preparation exits remain unobserved.
  Source/evidence: T016 working-tree range from B1 base `76e656c4`,
  [B2E evidence](../specs/185-prepared-model-runtime/evidence/b2e-extensions.md),
  and [tasks](../specs/185-prepared-model-runtime/tasks.md). Status:
  `VERIFIED` for the bounded B2E exit; Spec185 remains `PLANNED` pending B2–B9.

- **Complete API revision**: [C-07](../specs/185-prepared-model-runtime/contracts/api-catalog.md)统一64组稳定入口、Python直接绑定/便利映射与值/生命周期；Subscription及局部异步等待、read取消、析构和关闭边界归原任务owner。[验证记录](../specs/185-prepared-model-runtime/evidence/api-lifecycle-20260912.md)。PLANNED，不是产品完成。

- **API revision**: [全表面审计](../specs/185-prepared-model-runtime/api-review.md)、[C-05](../specs/185-prepared-model-runtime/contracts/api-usability.md)、[C-06](../specs/185-prepared-model-runtime/contracts/cpp-first.md)。独立C++ SDK及原生异步/Provider入口，Python仅包装；任务扩为16项11批，T013完整C++资格先于T012。
- **Revision evidence**: [API修订记录](../specs/185-prepared-model-runtime/evidence/api-review-20260912.md)；源码未改，当前/冻结目标API快照不覆盖。

- **Status**: PLANNED；[Spec185](../specs/185-prepared-model-runtime/spec.md)、[任务](../specs/185-prepared-model-runtime/tasks.md)。
- **Before/after**: 已有 NativeRequestCatalog 在构造期验证冻结模型，请求期仍需model/splitter等接线；目标提供Runtime/User/PreparedModel和有界准备缓存，NativeInferenceClient继续唯一执行。
- **Contracts**: C-01公开API、C-02缓存身份、C-03请求/会话/Provider、C-04验收；详见[审计](../specs/185-prepared-model-runtime/audit.md)。
- **Design/API**: 目标roadmap新增Spec185章节；新增签名在独立中文契约，不写入已实现API inventory。当前PDF提示历史基线漂移54文件；未刷新并行源码事实。
- **Source/evidence**: 源码审计基线575b43cc93bbed29932303caf3d09974f1585af7；[规划证据](../specs/185-prepared-model-runtime/evidence/planning-20260912.md)。本轮没有native行为或资格测试，184未完成项不变。

### 2026-09-12 — Spec184 portable MiniNDN environment inputs

- **Status**: PARTIAL（environment-input unit `CLOSED_FOR_VALIDATION`）；T007 qualification remains IN_PROGRESS / PARTIAL。
- **Before/after**: MiniNDN runner 的 topology、Provider node、MiniNDN root、content store、app state 和 native executable 依赖分散在默认值与硬编码中；现在由 `ndnsf-di-minindn-environment-v1` profile 提供机器差异，显式 CLI 覆盖，启动前完成节点/binary preflight，并记录解析 profile identity。协议、ACK/Selection、Provider assembly 和 C++ API/wire 未改变。
- **Source / evidence**: source checkpoints `689cca00d40b1e2bf241aff4e7d14b0204a7c892`, `90dc8785` and `15c8abb1`; [Spec184 profile evidence](../specs/184-native-di-closure/evidence/minindn-environment-profile-20260912.md)、[environment profile guide](../docs/ndnsf-di-minindn-environment.md)。
- **PDF boundary**: no current/target API or architecture change; PDFs were not regenerated. Actual remote/Tiger and Qwen3.6-27B evidence remains external。

### 2026-09-12 — Spec184 Native MiniNDN requester route

- **Status**: PARTIAL（caller route `CLOSED_FOR_VALIDATION`）；T007 qualification remains IN_PROGRESS / PARTIAL。
- **Before/after**: MiniNDN runner previously appended `--native-cpu-provider` even when
  `--native-requester-config` was present, making the legacy per-token diagnostic branch win over
  `APPClient.request_native_reference`. The runner now selects mutually exclusive native requester
  and compatibility diagnostic arguments; User rejects the conflicting pair and normalizes the native
  final `tokenIds` response for the existing first-token oracle.
- **Current C++ boundary**: `NativeInferenceClient` still plans after `ACK_CLOSED`; the Provider keeps
  metadata-only startup slots and assembles canonical ONNX after authenticated Selection. No C++ API,
  wire contract, or Provider state-machine change was made.
- **Source / evidence**: checkpoint `7488ac08`; [Spec184 caller evidence](../specs/184-native-di-closure/evidence/native-minindn-post-ack-routing-20260912.md),
  [caller matrix](../specs/184-native-di-closure/contracts/caller-matrix.md), and [tasks](../specs/184-native-di-closure/tasks.md)。
- **PDF boundary**: no Design/API signature or target/current architecture change; current/target PDFs
  were not regenerated. The caller route and qualification limits are recorded in the active Spec evidence.

### 2026-09-11 — Spec184 Native DI Closure / Spec182 Transfer

- **Status**: NO_DESIGN_CHANGE；184 implementation NOT_STARTED；182 TRANSFERRED / qualification INCOMPLETE。
- **Before/after**: 将182剩余14个父任务、四项源码 finding 与两类闭合缺口迁入184，保留已完成实现和原始证据；
  不改变 API/wire、架构目标或当前行为，不复制历史时间线。
- **Baseline**: `94c1e644`；[184 spec](../specs/184-native-di-closure/spec.md)、
  [transfer matrix](../specs/184-native-di-closure/contracts/transfer-matrix.md)、
  [migration evidence](../specs/184-native-di-closure/evidence/migration-20260911.md)。
- **PDF boundary**: 仅文档治理/执行归属变化，双 PDF 与独立源码快照不重新生成；后续修复仍按 MANAGEMENT.md 同步。

### 2026-09-11 — Spec182 Request Chain Audit / R12 Replan

- **Status**: NO_DESIGN_CHANGE（产品实现 PARTIAL）；模块 NDNSF-DI，关联 T005/T010/T011/T013–T017。
- **Source baseline**: `72b9e388cc3920b0bdcd4c36d302d63c71e7f15a`。本轮不修改产品源码、API 或冻结设计快照。
- **Before/after**: 原执行计划累积多个旧 dispatch；现按当前请求链审计的四项缺陷和两类缺口，统一为
  R12-A–E。修复线程、既有会话提交契约和导出行为是后续 PLANNED 工作，不写为当前已修复行为。
- **Evidence**: [静态审计](../specs/182-native-di-python-bindings/evidence/request-chain-static-audit-20260911.md)、
  [R12 调度](../specs/182-native-di-python-bindings/contracts/audit-driven-execution.md)、[tasks](../specs/182-native-di-python-bindings/tasks.md)。
- **PDF boundary**: 没有 API/目标设计变更，不重新生成双 PDF，不覆盖独立冻结的当前/目标快照；
  修复实施时仍须按 MANAGEMENT.md 同步实际行为与源码摘要。本次文档 checkpoint 由 Git 历史定位。

## Spec182：Native-First Execution（2026-09-10）

- 状态 PLANNED；模块 DI；TG-02 增补独立 artifact authority 和 N1–N5 顺序。原先允许生产 requester 内置 issuer、将 process tests 统一留给 T016 的规则被取代。
- R11-B1 至 B9 与 T005/T009–T017 映射见 [执行契约](../specs/182-native-di-python-bindings/contracts/native-first-execution.md)；任务状态见 [tasks.md](../specs/182-native-di-python-bindings/tasks.md)。
- 本轮无源码/API 签名变更；当前快照和冻结目标 API 均保留。目标新增边界仍待实现，不宣称当前 requester 已移除 authority 私钥。
- 核对起点源码提交 `89932fb5b91f488eec6d4dc8756abba3fdaa40bd`；文档提交由本文件 Git 历史定位；验证见 [本轮证据](../specs/182-native-di-python-bindings/evidence/native-first-replan-20260910.md)。

## D-005：四模块图解（2026-09-08）

- 工作单元 D-DESIGN-DIAGRAMS；用户授权补充模块图、类图、时序/状态图。
- 新增图解章节与 G1--G9；API/实现行为无新增，属于现有设计的图形说明。
- 当前 G7 对照 NativeInferenceClient::dispatchOperation 的未就绪终点；目标 G7 仅表达 TG-02/TG-03，PLANNED，不声明完整原生链已实现。
- 当前源码身份与 API 清单同步，目标快照保持冻结；共享对象图核对两侧关系。
- 文档与渲染证据见 [diagram evidence](../specs/182-native-di-python-bindings/evidence/design-diagrams-20260908.md)；状态以该记录为准，不关闭 Spec182 产品任务。

## D-004：逐章修订与关键 API 行为（2026-09-08）

- 工作单元 D-DESIGN-R3；依据用户授权审计并修正 Design。
- 前：第 53 章只有 cancel 声明；grant/流/目录存在错述；BC 重复；目标正文含 R0 过时声明。
- 后：23 组 API 先解释行为，再列带所属符号的准确声明；补字段、状态表、返回/错误及调用示意。
  第 53 章覆盖 epoch/Qwen/KV/journal；BC 合入 AC，当前/目标 58/63 章；五项目标补兼容和验收。
- 新发现：Drone Execute handler 直接转交 backend，未见独立 lease/readiness 重验；
  Qwen cancel 与 handle cancel 不同；journal abort/耐久性限制按实际代码记录，目标显式承接。
- 工具：修复长方法名断行，生成两侧可读 Markdown 声明并逐字节检查，目标使用冻结 inventory。
  维护规则加入原章勘误、逐章阅读和示意/运行证据区分；不把计数当语义通过。
- 当前采样身份见 source-baseline.json 与精确补丁；目标身份继续保留独立冻结记录。
  当前新增模型/候选字段仅写当前契约，未修改目标基线或产品代码。
- 逐项位置：[67 个原主题的修订记录](reviews/chapter-revision-r3-20260908.md)。
- 验证及状态：[R3 evidence](../specs/182-native-di-python-bindings/evidence/design-r3-20260908.md)。
  文档修订不关闭 Spec182 产品任务；未定稿目标继续 PLANNED。

## D-003：逐章可理解性审阅（2026-09-08）

- 工作单元 D-DESIGN-CHAPTER-AUDIT；[审阅清单](reviews/chapter-audit-20260908.md)、
  [证据](../specs/182-native-di-python-bindings/evidence/design-chapter-audit-20260908.md)。
- 当前/目标 129 个章节位置按 67 个主题逐项阅读：KEEP 7、EXPAND 36、REWRITE 20、CORRECT 4。
- 第 53 章只展开 cancel，生成/KV/会话方法和流程缺失；第 59 章外部 grant 参数与内部 ABE
  完整策略物化混淆；目标历史说明与 TG 章节冲突。报告区分内容错误、完整性和可理解性。
- 审阅工作完成；文档内容 NEEDS_REVISION。未改产品 API、目标决策、源码、快照和 PDF。
  D-002 的技术检查为历史事实，不解释为逐章语义验收；后续修订按本清单收敛。

R2 新增 D-002（文档校验与行为补充）及 TG-01 至 TG-05（PLANNED）。目标批准来自用户
“先修复设计基线和校验机制，再补关键 API 行为契约，最后将架构改进逐项纳入目标设计”。

## D-002：基线、校验与行为契约

- 工作单元：Spec182 D-DESIGN-R2；[证据](../specs/182-native-di-python-bindings/evidence/design-r2-20260907.md)。
- 变更前：350 文件快照遗漏关键 .cpp；目标渲染共享当前 API；PDF 未绑定全部生成输入。
- 变更后：460 文件实现/配置快照；独立目标 API/源码基线；输入/PDF 构建身份验证；
  全函数 API ID 的保守覆盖状态；新增文件、源码漂移与过期生成内容均检查。
- API 行为：AC-13 修正精确 lookup；BC-01 至 BC-04 补授权失效、句柄异步行为、Repo 和 UAV 边界。
- 当前源码：ca585ab5365189203325a8462d8726d2ea32c98f 加 source-baseline.json 登记的工作区补丁；
  未提交实现只作为字节基线，不随文档暂存，不因此获得产品资格。
- 目标源码：保留 e9fe33994a6ca3ff81893591bd24c3fae43f933f 的 R1 冻结 API/源码快照。
- 文档状态和精确检查结果见证据；剩余 SIGNATURE_ONLY 项不计行为审查完成。

## R2 Target Changes

| ID | 模块 / 目标章节 | 前后变化 | API / 兼容边界 | 状态 / 后续验收 |
|---|---|---|---|---|
| TG-01 | Core / 授权版本与影响范围 | 全局版本失效 → 按权限和密钥变化区分影响 | grant/revoke/getPolicyStatus/install；旧客户端保守处理，线格式待 Spec | PLANNED；撤销、乱序、离线及无关节点刷新证据 |
| TG-02 | DI / 原生请求链 | Python/native 分担运行状态 → 原生唯一状态所有者 | APPClient/NativeInferenceClient/Handle；保留签名和错误兼容，关联 Spec182 原任务 | PLANNED；真实 requester 路径、oracle、失败及取消 |
| TG-03 | 四模块 / 异步 API | 分散描述 → 显式线程、deadline、取消、终态和背压契约 | streaming/handle/transfer/mission；逐 API 迁移 | PLANNED；竞争、重入、资源释放和远端取消 |
| TG-04 | Repo / 能力与恢复 | 模糊跨层能力 → 明确能力表和操作幂等/恢复 | lookup 精确语义不变，组合查询单独契约；格式版本迁移 | PLANNED；崩溃、重复提交、目录/数据不一致 |
| TG-05 | UAV / 类型和状态机 | Fields 与交织控制 → 类型验证、独立状态机与适配器 | command/sendMavlink/mission/job；兼容适配器明确拒绝非法输入 | PLANNED；Mock、真实飞控、失联和迟到结果分阶段 |

仅 TG-02 关联当前 Spec182 的既有迁移任务；其余尚未分配 Spec，不虚构编号。
这些记录不改变当前产品 API 或任何功能验收门；未来修改须同步对应 Spec 的 plan/contracts/tasks。

| 记录 | Spec / 工作单元 | 模块 | 设计影响 | 状态 |
|---|---|---|---|---|
| D-000 | 四模块设计 R0 建档；Spec182 的 D-DESIGN-R0 文档单元 | Core / UAV / DI / Repo | 建立当前/目标一致的 35 章基线；新增版本管理与追踪规则 | VERIFIED（文档） |
| D-182-BASE | [Spec182](../specs/182-native-di-python-bindings/spec.md) 基线观察 | DI | 记录原生迁移当前边界；尚未逐项追溯该 Spec 全部设计差异 | PARTIAL（历史映射） |
| D-001 | [D-DESIGN-API](../specs/182-native-di-python-bindings/evidence/design-api-guide-20260907.md) | Core / UAV / DI / Repo | 从组件级细化到 API 契约与准确声明，增加 AGENTS 管理要求 | VERIFIED（文档；结果见证据） |
| D-182-CC3B | [Spec182 R4-B4](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md#cc-3b-requester-provider-transaction-wiring) | DI | 配置化 native requester 接入会话 owner、认证 receipt、Provider COMMIT/ROLLBACK/FINALIZE 与终态清理；真实跨进程两轮仍待验收 | PARTIAL（实现与局部验证） |

## D-001：API 开发者指南与维护规则

- 依据：用户要求 API 级细节并参考 NFD Developer’s Guide；本次不实现新产品 API。
- 变更前：35 章组件设计，API 主要作为源码定位入口。
- 变更后：增加第 36–58 章 API 契约、295 文件声明参考、完整签名/默认值/类型字段与契约映射；MANAGEMENT.md 和本机 AGENTS.md 规定每个 Spec/API 变化的同步、验证与提交流程。
- 当前/目标：本轮两份一致；目标契约独立维护，允许后续加入明确标识的计划接口。
- 实现状态：文档实现；原生 DI requester 等产品未完成项保持原状态。5738 个函数条目不代表 5738 个接口均已运行验证。
- 源码身份：R1 source-baseline.json 与精确补丁，可从 Git 还原 350 个文件；API inventory 逐文件绑定其中 295 个文件。API ID 列表见 contract-map.json。
- 验证与下一步：[API 指南证据](../specs/182-native-di-python-bindings/evidence/design-api-guide-20260907.md)。后续每个 Spec 按 MANAGEMENT.md 同步，不自动覆盖目标。

## D-000：四模块设计建档

- 日期：2026-09-07。
- 依据：用户要求建立当前/目标设计，并追加要求按 Spec 追踪、完整纳入 Git。
- 章节：两份文档第 1–35 章；本次维护修订涉及“文档范围与源码基线”和“文档维护与后续目标变更”。
- 变更前：没有统一的四模块双份设计基线；首版生成后暂时本地排除。
- 变更后：统一保存两份中文 PDF、独立可编辑正文、模块清单、源码摘要和本文件，全部纳入 Git；初始目标技术正文等于当前正文。
- 运行行为变化：无；本单元不修改产品代码。
- 源码基线：`d7fa9c8edef924a6cf29c12d3793049aa794a6be` 加 [快照补丁](evidence/source-baseline-worktree.patch)，对应 [94 文件摘要](source-baseline.json)。不把工作树快照误写成已提交实现。
- 上述 R0 的摘要/补丁指提交 9c019a17 中的同名文件；当前路径已在 R1 推进，回溯时须从该历史提交读取。
- 验证：[文档验证](validation.md)、[Spec 文档工作单元](../specs/182-native-di-python-bindings/evidence/design-pdf-baseline-20260907.md)。文档状态 VERIFIED 不代表四模块运行资格已全部通过。

## D-182-BASE：原生 DI 迁移的当前设计边界

- Spec：[182-native-di-python-bindings](../specs/182-native-di-python-bindings/spec.md)；任务依据：[tasks.md](../specs/182-native-di-python-bindings/tasks.md)。
- 章节：第 18–23 章 DI，第 31 章“实现状态与验证口径”。
- 已核对现状：Python 应用规划与 C++ Provider 执行组件并存；原生 NativeInferenceClient dispatch 返回 NATIVE_REQUEST_PIPELINE_NOT_READY，准备/放置/封存组件的存在不等于完整请求链已接通。
- 当前/目标差异：本次用户指定 R0 两份一致，因此没有把 Spec182 尚未完成的迁移目标自动写入目标 PDF；Spec182 的目标仍以其 plan/contracts 为准。
- 历史变更前后与提交范围：尚未完整追溯，不能把 R0 快照当作整个 Spec182 的变更清单。下一次该 Spec 设计更新时新增具体条目，并逐项补齐任务 ID、行为差异、源码提交与验证。
- 状态：PARTIAL 仅指本文件的历史映射；不替代或降低 Spec 自身验收状态。

## D-182-CC3B：会话请求事务接线

- 日期 / Spec / 任务：2026-09-08；Spec182 R4-B4 CC-3B；T011-C 保持 PARTIAL。
- 原设计：`NativeInferenceClient` 仅保存会话 coordinator，公开 stream final 没有 receipt
  收集、Provider promotion 或 durable checkpoint 提交。
- 当前变化：配置了 `NativeRequestRuntime` 的请求在 final 阶段创建 owner turn，验证每个角色的
  receipt，发送加密 COMMIT/ROLLBACK/FINALIZE 控制并等待 canonical commit ACK；coordinator
  在 durable gate 中执行 parent/journal 晋升，取消、deadline 和 replacement 清理有明确边界。
  未配置 runtime 的兼容/组件构造仍保留结构化 `NATIVE_REQUEST_PIPELINE_NOT_READY`。
- 兼容与目标边界：不改变既有 collaboration wire；Provider 仍使用现有 request-scope 加密
  端口。真实两轮、跨进程 control/receipt、恢复和 T016 qualification 未完成，因此不能把局部
  build/test 结果写成完整原生请求链。
- 源码与证据：NativeInferenceClient、NativeConversationCoordinator、NativeProviderHandler；
  当前批次命令、日志和剩余出口见 [R4-B4 证据](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md#cc-3b-requester-provider-transaction-wiring)。
- Design PDF：当前/目标 PDF 继续保持原设计基线；未将未验收的请求链写入目标行为，下一次设计
  PDF 修订须在真实两轮/恢复验收后同步。
- 状态：PARTIAL；下一步补 C++ integration harness，再执行 T012/T013 与 T015/T016。

## D-184：Spec184 原生 YOLO 请求与候选绑定收敛

- 日期 / Spec / 任务：2026-09-11；[Spec184](../specs/184-native-di-closure/spec.md)；T007-A0/A1/A2。
- 模块 / 当前章节：NDNSF-DI User/Provider request-scope、native ONNX assembler、qualification harness。
- 原设计与变化：请求作用域输入原先可由多个 Provider 共用同一逻辑路径；当前实现为每个 Provider
  建立独立的 key/binding/input name 状态，并在清理时逐项失效。`COMPONENT_SET` assembler
  原先按完整图节点数拒绝子集；当前按声明的 role boundary 与 extracted graph 验证 certified
  subset，同时保留 deterministic node bytes 与 node-index cover 检查。
- 当前实现 / 目标边界：Y-A 单 Provider、Y-B 多 Provider、Y-N 七个拒绝边界已由当前候选的
  C++ 生产路径运行验证；Python 只编排 MiniNDN 和收集证据。Qwen3.6-27B、继承 negative/
  retirement、I05、Python retirement 与 SIF/Tiger 仍未实现或未资格化，不能写入当前行为。
- 兼容性 / 安全：保留现有 collaboration wire；request-scoped input name 增加 Provider
  绑定，防止跨 Provider key/state 复用；ONNX subset 只允许 recipe 声明和图输入/role boundary
  可达的节点，非法 cover 继续拒绝。
- 源码范围：`ServiceUser.hpp/.cpp`、`ServiceProvider.hpp/.cpp`、
  `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp`；验证 harness
  为 `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`。
- 验证与证据：[T007 process qualification](../specs/184-native-di-closure/evidence/t007-process-qualification-20260911.md)、
  [remainder audit](../specs/184-native-di-closure/evidence/remainder-audit-20260911.md)、
  `.codex-tmp/spec184-yolo-Y-A-output-20260911-r51/`、
  `.codex-tmp/spec184-yolo-Y-B-output-20260911-r37/`、
  `.codex-tmp/spec184-yolo-Y-N-output-20260911-r50/`；candidate receipt verify exit `0`，
  Y-A/Y-B/Y-N `PASS`，`git diff --check` 和 Python syntax check 通过。
- 状态：`PARTIAL`；本地 C++/YOLO 行关闭为 `PASS_FOR_ROW`，Qwen3.6-27B 明确
  `WAITING_EXTERNAL_INPUT`。下一步仅处理 A4 继承行与外部模型/实验机，不把 0.6B smoke 代替 27B。

## 新记录模板

### D-185-B7R：runner identity 与 exact-forward cache 生命周期契约

- 日期 / Spec / 任务与契约 ID：2026-09-15；[Spec185](../specs/185-prepared-model-runtime/spec.md)；T021 / C-03、C-08。
- 模块 / 当前与目标章节：NDNSF-DI `NativeModelRunner` 与 `ProviderRoleWorker`；当前 API reference 的 `NativeModelRunner` 条目。
- 原设计 / 新设计 / 修改原因：exact-forward cache 原先用 runner 原始地址区分实例；生产两次独立授权请求中 allocator 复用地址会错误命中已销毁 runner 的输出。当前改为进程内外部 registry 分配单调 runner identity，并在 base destructor 移除登记；cache key 使用该 identity。公共多态基类不增加数据成员，保持对象布局不变。
- 当前已实现部分 / 目标未实现部分：normal 与 ASan/UBSan C++ protected Provider selector 观察两次独立 grant 各自 source fetch、assembly、runner creation 和 execution；生产 grant issuance 与真实 ONNX Runtime 模型仍由 fixture/后续资格批次覆盖。
- 兼容性、迁移或撤回影响：新增构造、析构和 identity accessor 已登记到当前 DI API inventory/reference；无 wire 字段变化，旧 runner 地址不再作为 cache identity。
- 源码提交或范围 / 文档提交定位：`NativeModelRunner.hpp/.cpp`、`ProviderRoleWorker.cpp`、T021 C++ selector；本地 checkpoint 待组合门通过后提交。
- 验证命令、结果与持久证据：`.codex-tmp/spec185-t021-runtime/production-independent-grants-normal-v5.log` 与 `production-independent-grants-asan-v3.log` 均 `RC=0`；详见 [B7R T021 evidence](../specs/185-prepared-model-runtime/evidence/b7r-lifecycle-fixes-20260915.md#t021-follow-up-production-protected-independent-grant-matrix)。
- 状态：PASS（T021）；T013、T012、T014 仍按各自验收边界保持未完成。

### D-185-B4：会话追加输入的 native adapter token 契约

- 日期 / Spec / 任务：2026-09-13；[Spec185](../specs/185-prepared-model-runtime/spec.md)；T007/T008 B4。
- 设计变化：取消调用者可写的 `RequestOptions.canonicalTokenIds`，改由已验证 native adapter
  的 `conversationInputTokens(Input)` 生成当前输入 suffix；native `Conversation` 在首轮建立
  token prefix，在 `APPEND_DELTA` 接到 durable parent 后再由 coordinator 校验严格增长。
- 原因与边界：此前公开包装没有把本轮输入的 canonical token 谱系传到 coordinator，第二轮只能在
  `beginTurn` 被正确拒绝；直接增加公开 vector 会允许调用者伪造 lineage，因此改为 adapter-owned
  encoder。该机制不是新的 parent receipt、role map、plan 或 Provider 参数；没有 pinned encoder
  的 adapter 必须显式报 `UNSUPPORTED_CAPABILITY`。
- 源码与契约：`NativePlanning.hpp`、`NativeCatalogModelAdapter.*`、
  `NativeCanonicalPreparationCatalog.*`、`NativeRequestCatalog.cpp`、`PreparedModel.*`、
  `Conversation.hpp/.cpp`、B4 C++ fixture；对应
  `contracts/public-api.md`、`execution.md`、`api-catalog.md`、`code-design.md`、`api-usability.md`。
- 验证边界：旧 B4 r5 失败证据保留于 [b4-conversation](../specs/185-prepared-model-runtime/evidence/b4-conversation.md)；
  新字段仅完成静态复审准备，normal/sanitizer selector 尚未重跑，B4 仍 `PARTIAL`。当前/目标 PDF
  在 B4 通过并进入文档交付时按 MANAGEMENT.md 统一刷新，未把本次未验收行为写成资格 PASS。

### D-185-B8/B9：PreparedModel 原生入口与设计交付快照

- 日期 / Spec / 任务与契约 ID：2026-09-15；[Spec185](../specs/185-prepared-model-runtime/spec.md)；T012/T014 / C-05、C-06、C-07、C-08。
- 模块 / 当前与目标章节：NDNSF-DI `Runtime`、`User`、`PreparedModel`、`NativeInferenceClient`、pybind facade；Design 当前/目标 API、G7 图与双 PDF。
- 设计变化：当前设计从“原生 dispatch 尚未接通”的历史快照更新为区分两条实际路径：`Runtime::open → User::prepare → PreparedModel → NativeInferenceClient` 已由 C++ 过程矩阵和 Python 薄绑定验收；没有 preparation/request contract 的兼容构造仍返回 `NATIVE_REQUEST_PIPELINE_NOT_READY`。目标设计继续独立保留 TG-01--TG-05 的 `PLANNED` 内容。
- 修改原因：T013/T012 已形成当前候选的原生过程与包装边界，旧当前书会把已验证实现误报为未实现；API 清单、绑定映射、行为覆盖和源码快照也需要绑定同一工作树身份。
- 当前已实现 / 目标未实现：B7 C++ 过程矩阵、B8 Python 绑定和 lifecycle 修复有各自证据；subinterpreter、wheel packaging、Spec184 外部模型/retirement、I05 与 SIF/Tiger 仍保持未观测或 `PARTIAL`。
- 兼容性 / 撤回：保留旧 `APPClient` 编排和无 runtime 的兼容失败语义；未新增 wire 字段。若回退文档，只能回退当前设计说明，不得覆盖目标快照或降低已记录的 C++ 资格证据等级。
- 源码范围 / 文档提交定位：`Design/api/*`、`Design/current-api.tex`、`Design/current-content.tex`、`Design/current-design.tex`、`Design/current-diagrams.tex`、`Design/diagrams/di-current-flow.tex`、`Design/api-contracts.json`、`Design/validation.md` 与 `specs/185-prepared-model-runtime/evidence/b9-handoff.md`；源码身份由 `Design/source-baseline.json` 和 `evidence/source-baseline-worktree.patch` 绑定。
- 验证命令、结果与持久证据：`test_design_state.py`、`verify-api-reference.py`、`verify-source-baseline.py` 均 `PASS`；双 PDF 构建和 `verify.py` 均 `PASS`，构建目录 `.codex-tmp/design-pdf-20260915T130303244487Z/`，详见 [B9 handoff](../specs/185-prepared-model-runtime/evidence/b9-handoff.md)。
- 状态：`PASS`（文档交付）；这不升级 Spec184 的外部资格，也不把文档门替代 native C++ 运行验收。

### D-编号：设计变化名称

- 日期 / Spec 链接 / 任务与契约 ID：
- 模块 / 当前与目标章节标题：
- 原设计 / 新设计 / 修改原因：
- 当前已实现部分 / 目标未实现部分：
- 兼容性、迁移或撤回影响：
- 源码提交或范围 / 文档提交定位：
- 验证命令、结果与持久证据：
- 状态 / 剩余验收 / 下一步：

### D-187：YOLO MiniNDN 原生 User selector 接线

- 日期 / Spec / 任务与契约 ID：2026-09-15；[Spec187](../specs/187-yolo-minindn-sif-app/spec.md)；T002/T006 / FR-005、FR-006、FR-012。
- 模块 / 当前与目标章节：`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`、`tests/integration-tests/di-prepared-request.t.cpp`、`tests/wscript`；YOLO MiniNDN local acceptance。
- 原设计 / 新设计 / 修改原因：原接线在 Python MiniNDN case 完成后另起 `DummyClientFace` C++ fixture，且 selector 可缺省，不能证明同一网络调用链。当前在显式 `SPEC187_NATIVE_MODE=1` 下由 runner 在网络启动前校验 selector/config/input/output，并把注册的 C++ `NativeRequesterThroughMiniNdn` 作为 MiniNDN User 子进程启动；C++ 直接调用 `Runtime::open → User::prepare → PreparedModel::request`，通过继承的 `NDN_CLIENT_TRANSPORT` 连接节点 NFD。
- 当前已实现部分 / 目标未实现部分：C++ target、source closure、fail-closed 输入门和独立 served-provider selector 已通过；Requester 的 ACK/Selection 与 Provider 的 Selection/execution 阶段记录按 request/attempt/plan 关联并由 C++ selector 校验顺序；真实 candidate-bound config、两次 local MiniNDN run、SIF/Tiger promotion 尚未观测。
- 兼容性、迁移或撤回影响：未改变 NDNSF wire 或默认旧 Python harness；只有显式 Spec187 native mode 使用 C++ User，缺失 selector/config/input/output 不回退到 Python PASS。
- 源码提交或范围 / 文档提交定位：本地 T002 工作区改动；[B187-LOCAL-YOLO evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-local-yolo.md)、[convergence report](../specs/187-yolo-minindn-sif-app/evidence/convergence-20260915-r1.md)。
- 验证命令、结果与持久证据：`spec187-yolo-minindn` normal compile/link `rc=0`（`-j4`，1m6.118s）；独立 served-provider C++ selector `rc=0`；缺失 native input selector 以 rc=201 fail-closed；官方 review-agent r7 `STATIC_PASS`，阶段证据详见 B187-LOCAL-YOLO。
- 状态 / 剩余验收 / 下一步：`PARTIAL`；等待 regular base SIF、host-gate manifest 和 native requester config/input 后执行两次 local gate，再决定 TigerCluster promotion。

历史 Spec 的完整回溯是后续独立核对工作，本轮不虚构它们的变更记录。

### D-187-BASE: Stable dependency image repair

- 2026-09-15；Spec187 T001 prerequisite；`NO_DESIGN_CHANGE`：仅修复部署脚本及稳定依赖镜像，不修改 Core/UAV/DI/Repo API、wire 或目标设计。
- 保持现有稳定 base / 外置 APP 边界；NumPy wheel 私有库完整性与最终镜像 C++ SDK smoke 见 [base repair](../specs/187-yolo-minindn-sif-app/evidence/b187-base-repair.md)。本机最终镜像 `BASE_SMOKE_ONLY PASS`，缺库反例被拒绝；不以此刷新生产 API 或将 APP/MiniNDN 状态升级为 PASS。

## D-187-SEGMENT：request-scoped large input 的标准 segmented Data 绑定

- 日期 / Spec / 任务与契约：2026-09-16；[Spec187](../specs/187-yolo-minindn-sif-app/spec.md)；T002；request-scoped input transport。
- 原设计与变化：6.55 MB 输入曾走单个 request-scoped Data，超过 8,800-byte transport bound。当前大输入按 4,096-byte encrypted chunks 发布为 `base/version/segment` Data，User 与 Provider 用 `.appendVersion(attempt)` 共享基础名；每段独立 AEAD AAD，统一 `FinalBlockId`，Provider 以 `SegmentFetcher` 组装后才解密/派发。小输入保留单 Data 路径。
- 原因与兼容：Segmenter/SegmentFetcher 的 NDN object contract 需要 version component；attempt 派生版本不增加 wire 字段，也不改变 request/Selection 语义。旧单 Data 失败记录保留，不能把旧路径作为当前行为。
- 源码与证据：`ndn-service-framework/ServiceUser.cpp`、`ServiceProvider.cpp`、`tests/integration-tests/request-scoped-selection.t.cpp`；[segmented regression](../specs/187-yolo-minindn-sif-app/evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-c-segmented-request-regression)。
- 验证：官方 `review-agent` snapshot `review-segmented-input-20260916-r3` 返回 `STATIC_PASS`；`build-spec187-local-nac-r1` integration target `-j4` `rc=0`；`RequestScopedSelection/*` 与 `RequestScopedResponseConfidentiality/*` 第二次整套均 `rc=0`。长输入断言 1,601 segments、统一 FinalBlock、最大 wire <8,800 bytes 和完整组装后 handler。缺段/乱序/错误 FinalBlock/超时负例仍未运行。
- 当前/目标边界：C++ DummyFace 分段边界已通过；当前源码 MiniNDN r42 Controller native crash、SIF/APP 与 Tiger qualification 仍独立 `PARTIAL`，不因本条升级。

## D-189：Qwen two-provider MiniNDN full-path gate

- 日期 / Spec / 任务与契约：2026-09-18；[Spec189](../specs/189-qwen-two-provider-minindn/spec.md)；T001–T010；`Q189-PREP`、`Q189-REPO`、`Q189-WIRE`、`Q189-ASSEMBLY`、`Q189-HANDOFF`。
- 原设计与变化：Spec188 的 bounded core 以 YOLO 两轮作为本地出口，Qwen 只保留为 follow-up resource probe，不能证明最初的多 Provider CPU 目标。Spec189 独立规定真实 Qwen3-0.6B 的 `prepare → Repo manifest/layer publication → reference-only request → ACK → Selection → placement-bound Provider fetch/assembly/execute → terminal → drain` 全链；prepare 一次发布、后续 request 复用 reference，不能使用 stage export、单 Provider ORT、预置 runner 或 synthetic ACK/Selection 替代。
- 当前已实现部分 / 目标未实现部分：本机已生成并校验两阶段 ONNX、external canonical graph 和 initializer，哈希及大小见 [B189-1 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-prepare.md)。native prepare/Repo commit、真实 ACK/Selection、两个 Provider 的 C++ handoff/execute、MiniNDN 和 repeat 尚未运行，Spec189 保持 `IN_PROGRESS`。
- 兼容性、迁移或撤回影响：不修改 Spec188 已关闭的 bounded core 结论；`.specify/feature.json` 和 `AGENTS.md` 的 active pointer/rules 指向 Spec189。任意模型、split、ABI、profile、selector 或 handoff 变化都使后续证据失效。
- 源码范围 / 文档定位：`specs/189-qwen-two-provider-minindn/{spec,plan,tasks,batch-execution,contracts,quickstart}.md`；通用实验规则写入 `AGENTS.md` 的 `Real multi-provider model experiment contract`。
- 验证：`verify-spec-kit-sync.py --require-entrypoints` 返回 `PASS: 11/11`；ONNX checker 和 ORT CPU session 通过；这些仅是制品准备证据，不是 native/MiniNDN PASS。原始导出日志保留在 `.codex-tmp/spec189-qwen-two-provider-20260918/`。
- 状态 / 下一步：`PARTIAL` / `IN_PROGRESS`；先执行 B189-0 的 CodeGraph/ABI/handoff 冻结，再按 T002/T003 让 native prepare 把已验证制品发布到 Repo，随后才可运行双 Provider MiniNDN。

### D-189-PROGRESS：跨 Provider progress 与 Qwen/YOLO 验收边界

- 日期 / Spec / 任务与契约 ID：2026-09-20 04:34 -0500；[Spec189](../specs/189-qwen-two-provider-minindn/spec.md)；T006/T007/T009；FR-030、SC-007。
- 模块 / 当前与目标章节：Core `ServiceUser`/`InvocationStream` collaboration stream consumer、NDNSF-DI Provider assembly progress；Spec189 当前资格边界。
- 原设计 / 新设计 / 修改原因：原运行时只按 terminal Provider 的 progress 续命，Qwen 的非 terminal Provider 在 selected-material assembly 或 hidden-state 等待期间会触发 stream gap。现行契约按 committed Selection 接受每个 `{providerName, providerSelectionDigest, role operationId}`，并分别检查 `(epoch, sequence)` 新鲜度；YOLO 的轻量 `NATIVE_POSTPROCESS` 结果不再作为 Qwen 两 Provider 资格证据。
- 当前已实现部分 / 目标未实现部分：r47/r70 已证明 Qwen 进入 ACK、Selection、grant verification 和 selected-material fetch；progress binding 的 C++ 状态机与 focused selectors 已通过。新安装候选的真实 MiniNDN runner、hidden-state handoff、terminal output、cleanup 和重复请求仍未完成。
- 兼容性、迁移或撤回影响：单 Provider 继续使用一个精确 operation binding；不改变 ACK/Selection wire 或授权语义。宿主机 `swapIo`/`diskFree` 仍作为独立 `RESOURCE_BOUNDARY`，不能与协议失败合并。
- 源码范围 / 文档提交定位：`ndn-service-framework/ServiceUser.cpp`、`ndn-service-framework/InvocationStream.cpp/.hpp`、`specs/189-qwen-two-provider-minindn/{spec,plan,tasks,batch-execution,traceability}.md`；证据为 [r4 progress heartbeat](../specs/189-qwen-two-provider-minindn/evidence/b189-r4-progress-heartbeat-20260919.md) 与 [r70 boundary](../specs/189-qwen-two-provider-minindn/evidence/b189-native-r70-progressed-stream-boundary-20260919.md)。
- 验证命令、结果与持久证据：C++ progress/lifecycle selectors 已通过并获只读 `STATIC_PASS`；r70 仍使用旧候选并保持 `PARTIAL`，必须用新安装候选重跑，不能由静态或 YOLO 结果升级。
- 状态 / 剩余验收 / 下一步：`PARTIAL`；先完成 affected Core/DI 安装身份核对，再进行一次新的真实 MiniNDN run；只有越过 stream gap 后才继续材料、runner、handoff 或 terminal 边界。
