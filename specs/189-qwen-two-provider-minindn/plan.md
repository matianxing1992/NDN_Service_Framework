# Implementation Plan: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Multi-turn execution — r258**: 在r257单轮缓存诊断基础上，同一次MiniNDN保持两个
Provider存活，连续三轮，各轮1024输出token预算/EOS终止。每轮独立checkpoint，
下一轮APPEND_DELTA引用直接前驱；C++验证逐轮请求、恢复及prefix/epoch接续。
先修正launcher/oracle和必要原生接线，冻结只读审查→C++回归→一次有界运行。
设计、覆盖和结果统一见 [r258](evidence/b189-r258-multiturn-kv.md)，不扩大Repo/SIF范围。

**Continuation affinity and retention — planned**: r258第二轮真实KV复用成功，第三轮
因继承首轮5分钟截止时间而过期，并未更换Provider。下一批在本轮授权、ACK、资源
和有效状态约束内优先上一轮role→Provider分配，保留选后身份校验；明确有界保留
期限语义，不取消过期检查，不新增KV迁移协议。先补C++偏好/不可用/过期反例，
冻结静态审查后统一测试，再运行新三轮实例。r259已实现并通过定向C++及三轮缓存
诊断，完整Spec仍PARTIAL；常驻C++ Conversation同handle多轮仍待验证。
用户性能复核后优先执行[r260](evidence/b189-r260-runner-reuse.md)：消除同一request/role
每token重复创建runner，先C++生命周期/创建次数回归与静态门，再同配置性能对比；
不同时调整ACK或收尾预算，以便隔离效果，然后继续常驻Conversation。
用户随后接受已加载ONNX session短期跨request驻留：后续独立批次在Provider owner
下实现有界idle缓存/lease/evict/shutdown，先覆盖CPU；加载证据与本次plan/profile
及KV状态分离。现有artifact缓存不等于session缓存，不将该目标写作已实现。

执行批次为[r259](evidence/b189-r259-affinity-retention.md)：复用现有placement子类，
本地journal保留并认证角色映射，Provider显式有界retention与receipt/store一致。
不改网络checkpoint格式，不新增KV迁移。共享header变更只重编受影响DI消费者；
原生测试在构建树执行，MiniNDN只运行安装并核对过的候选。部分实现不升级验收状态。

**Multi-token execution — r256**: 使用1024输出token预算，有效Qwen聊天输入；EOS/EOT或
预算耗尽终止，不引入字符限制。先C++双角色反馈/收尾和1024 projection编码回归，再
安装受影响目标并运行一次MiniNDN。显式`--require-multi-token`检查不得弱化原scope门；
缓存模式只产生`CACHE_DIAGNOSTIC_PASS`，完整Repo验收保持PARTIAL。

**Installed-runtime constraint — 2026-09-19**: 用户明确要求后续全部 MiniNDN 使用系统已安装的库、应用、worker 和被测模块，见 [高层设计 B/M 约束](../../Design/highlevel-design.md#编译安装与-minindn-实验约束)。下文 build tree 继续用于构建及定向 selector；涉及 MiniNDN 软件来源时以本约束为准。重跑前须补齐必要目标的安装及真实加载路径校验，不从临时目录或 build tree 启动被测程序；运行配置、日志和数据仍使用隔离目录。既有实验事实不回写。

**Branch**: `Experimental` | **Feature**: `189-qwen-two-provider-minindn` | **Date**: 2026-09-18
**Spec**: [spec.md](spec.md) | **Status**: IN_PROGRESS

## Summary

目标仍是在本机 12 GB 上完成 Qwen3-0.6B 两 CPU Provider 的真实 MiniNDN 请求。
本轮只收敛本机真实 Qwen 两 Provider 请求的剩余闭环：选中材料消费、assembly、
generation lineage、跨 Provider handoff、独立输出、drain、warm cache 和 repeat。
保留已验代码，不重做 Spec185/188，不扩张 SIF/Tiger。

当前已完成的局部修复是两项：第一，canonical graph/initializer 等未加密 source
对象按 candidate identity 写入 system-wide content-addressed cache，命中时复用并
校验 hash/size，run-scoped encrypted Repo 与 protected grant-bound material 不跨
请求复用；第二，assembler 在 extraction 后释放完整 source protobuf，减少 source
与 assembled graph/runtime 的重叠 working set。r155/r156 已分别给出 source-cache
写入/命中和 assembly memory boundary 推进证据。

r161 又确认了 local ONNX adapter 在 edge-local role 尚未绑定时误用 full lineage
validation 的生产边界。`validateCore()` 修正已完成，affected DI closure 已用
`-j4` 编译并安装到全局 `/usr/local`；r162 已越过该修正所在的入口，但为避免普通
Repo material fetch/assembly 的无必要工作集增长而受控停止，所以真实 Provider-1
assembly、handoff、terminal、output、drain 和资格仍未闭合。当前增加一个显式、默认关闭
的 temporary cache-compatibility mode：在 authenticated Selection/grant/placement
之后验证 system-wide plain source cache，并跳过该阶段的 Repo material fetch。见
[audit correction](evidence/spec189-static-audit-20260918.md#architecture-and-progress-correction)、
[r161 evidence](evidence/b189-r161-lineage-validation-diagnostic-20260920.md) 和
[r162 stop evidence](evidence/b189-r162-cache-compatibility-stop-20260920.md)。

审计对账已将 F01 标为局部修复、资格仍开放；F02 已通过准备 owner/cancel 的 focused
C++ selector、但实际 ORT/RSS 和完整候选资格仍开放；F08 仍是当前候选的开放生产门，
F05/F09 已有 focused C++ selector 但仍需完整候选边界，分别由 T003/T009 收口。F03/F04 只在采用 catalog snapshot/delta 时适用，
F06/F07 属于未用于本机 native protected 资格路径的兼容/本地 Python 后端；它们保留
为后续维护项，不能写成已修复，也不应把本次 Qwen 任务扩成通用 Repo 重构。

## Latest refinement — 2026-09-21

### Acceptance target (唯一主线)

本计划只服务于一个结果：`MiniNDN + Qwen/Qwen3-0.6B + 2 execution Provider nodes
+ NDNSF-DistributedInference native C++ inference`。当前两个执行节点是 topology
profile 中的 `ucla` 与 `arizona`；Controller/User/Repo 只提供请求、授权、材料和
网络基础设施，不被计作第三个模型执行节点。任何 stage export、单 Provider、预加载
runner、Python-only 推理、cache hit 或 focused selector 只能作为前置证据，不能关闭
Spec189 的真实请求任务。

### Stage preparation implementation boundary

旧 Python stage exporter 只提供候选输入/输出契约和离线校验依据。Provider 侧 C++
路径按以下顺序接收并验证它：

`authenticated role contracts → selected node/dependency materialization → explicit
ONNX role graph input/output rebuild → ONNX checker/shape and dynamic-KV validation →
ORT runner → Provider handoff/terminal output`。

这条路径不复制整模型，也不把 stage 文件变成绕过 Repo/ACK/Selection 的 runner；
每个 Provider 只 materialize 自己的 selected range/shared dependencies。若契约、
dtype、shape、KV 或 boundary 名称不一致，必须在 Provider native boundary fail closed。

### Assembled-cache identity refinement — 2026-09-21

组装前的 cache lookup 必须使用 authenticated `recipeDigest`，因为 assembled
`model.onnx` 的结果 SHA 只有 worker 完成后才产生。当前 stable entry 是
`assembled/<safe-role>/<recipeDigest>/model.onnx`；`manifest.json` 再记录并校验
`assembledModelDigest` 及 root/graph/initializer/role identity。recipe 保留
backend/ABI/precision 等 runner compatibility fields；这不否认同一 ONNX bytes
在不同 backend 间可能复用，而是避免当前 assembly/runner contract 跨 backend
错误命中。cache hit 仍要求本次 authenticated Selection/grant，普通 protected
Repo 不改为跨请求明文复用。该修正已有 focused C++/Python 证据，但未改变真实
MiniNDN qualification gate。

### Model-preparation TDD slice — 2026-09-21

新增 focused C++ selector 用固定种子生成一个 4-node fully-connected ONNX（文件
不存在时生成，随后重新读取），再调用生产 `NativeCanonicalRolePreparer` 按
`X -> H2 -> Y` 拆成 3+1 两个 role。两个 role 都通过同一 native assembly chain 的
full checker、shape inference 和 CPU ORT session load；测试使用 RAII 删除生成目录。
该 slice 证明准备、拆分、边界契约和组装能力，不替代 Qwen 0.6B 的真实 Repo/ACK/
Selection/两 Provider/terminal 验收，故不改变 `RUNTIME_UNQUALIFIED`。

### Gate order for the next candidate

1. 静态审查：确认 Spec189 candidate tuple、role policy、grant/key closure、dynamic-KV
   contract、graph budget、installed Python/native closure 和 run-scoped raw root。
2. C++ focused selector：复核内部 stage boundary 与 canonical publisher regression。
3. Affected build/install：核对 build receipt、`/usr/local` binary/library hashes、
   RUNPATH 和实际 loader closure；`py_repoclient` 若仍缺 host-binding 必须记录为
   preflight gap，不得静默忽略。
4. 一次新 raw run：只运行 MiniNDN + Qwen 0.6B + 两个 native Provider 的完整链路，
   保留首个生产边界和 cleanup 证据。
5. 若失败：先更新 `tasks.md`、failure log 和 immutable evidence，再做只读静态复审；
   只有真实 Changed gate 才能进入下一次 build/runtime。
6. 只有 terminal/oracle、两 Provider execution、drain/resource 和 repeat 全部满足，
   才评估 SC/T 项勾选；局部 stage PASS 不推进产品验收。

本轮不做 SIF/Tiger、通用 Repo 重构、无关 git 清理或 push。

## Technical Context

- C++ owns preparation/publication/authorization/assembly/execution and behavior assertions.
- Python owns MiniNDN orchestration/host sampling/offline conversion only.
- 全局依赖按 [declared closure](../../docs/native-dependency-closure.md)；
  本机 Boost 为 /usr/include + /usr/lib/x86_64-linux-gnu 1.71，其余按声明全局根（含 ORT）。
  缺失/不兼容才安装，禁止 checkout/.codex-tmp 前缀覆盖。
- 本地 MiniNDN 直接运行宿主机已安装的全局依赖和本机构建的 C++ APP；SIF/Apptainer/Tiger
  只属于后续交付或远端资格，不是 Spec189 本地运行前置条件。`RESOURCE_BOUNDARY:diskFree`
  指宿主机文件系统安全门，不是 SIF 构建或 SIF 内容失败。
- 复用已验的仓库根 Waf tree（与 NDN-CXX 相同的 `waf`/`wscript` 入口），受影响 target
  增量构建默认 -j4，swap 压力按仓库规则降并发；构建树 selector 的 target-local RPATH
  必须优先于已安装的同 SONAME 库。
  不为每个新 selector 复制整套 DI 编译闭包。
- 根 NDNSF Waf 只编译 NDNSF 自有的 Core、Repo、DI、examples 和 tests；NAC-ABE 的源码、
  构建和安装由 NAC-ABE 自己负责（当前 canonical 路径为 NAC-ABE 的 CMake，仓库内
  Waf 入口仅为 deprecated 兼容入口）。NDNSF 只消费已安装且已核对的 NAC-ABE SDK，
  不在 NDNSF Waf 中递归构建或把 NAC-ABE checkout 当作临时依赖前缀。
- 两 Provider，固定短输入与少量 token；独立正确性判据必需，广泛质量评测不在范围内。

## Audit correction — local candidate first

本 Spec 的成功条件是一次本机可重复的真实请求；候选专用的 Qwen layer map、
profile、resource budget、MiniNDN topology 和 `spec189-two-provider-oracle`
都是本地验收输入，不是新的 NDNSF 全局 API、Core/Repo 默认值或长期协议。除非
实际修改公开签名、跨模块数据契约或安全语义，否则不更新全局设计来承载这些
临时字段；需要跨 Spec 的 API 变化必须另开设计变更，而不是在本 Spec 中顺手
泛化。

原生 C++ 负责 source identity、material manifest、prepare、授权、assembly 和
行为断言。实验脚本中的 ONNX helper 只能做候选预检、资源采样和进程编排；它的
digest 不是 native identity 的权威来源，native prepare 必须再次独立检查同一
source。r27 的 Python identity 资源边界已修复，下一步先以新的受控 run 观察
native prepare 的第一边界；若仍是 source 内存峰值，新增的是一个有界修复单元，
不借机重写全局 Repo 或 DI 缓存架构。

资源 guard 是每个 full-model run 的前置/伴随门，不再作为独立能力任务。小型
guard/lifecycle 证据保留，native resident/runner counters 由其实际 owner
（T003/T006）交付，最终在 T009 收口。

## Constitution Check

设计修订遵循 C++ ownership、真实调用链、全局依赖与静态门；产品仍 PARTIAL。
文档一致性 PASS 不能替代 native/MiniNDN runtime。

## Architecture Decisions

### AD-01: topology-independent preparation

native prepare 验证 canonical graph/config/initializer，生成原子层和 shared tensor
内容索引，发布可经正常 Repo 路径读取的材料，commit 后返回现有 PreparedModel。
prepare 不固定最终 Provider 分区，不构造两段 runner；其他分区计划可复用同一材料。
本 Spec 只验两个 Provider，不增加其他拓扑验收。

维护导出器可以预先生成 stage ONNX 作为输入/输出和 top-token 契约校验；这些
exporter-side 文件不是 Provider 启动 runner，也不是本次请求已经选定的分区。

复用 Repo manifest/payload owner/文件后端/事务，最小版本化扩展表达 layer→
对象或受验证 byte-range、digest/size/shared dependencies。
不新增 PreparedQwenModel 公开 API、Qwen 专用 Repo 或第二 serializer。

### AD-02: durable reference and availability

requester 经 Runtime 注入通用 ciphertext range store；Core 保持加密、签名与
NDN serving，Repo 负责持久范围存储。不能将 plain Repo publisher 直接替代
生产 encrypted fetch 的对端，见 [binding](contracts/model-preparation.md#protected-repo-integration-binding)。
prepare 返回前证明持久提交与可读性；模型源和临时 buffer 可释放，handle 持有 reference/lease。
Repo service 在请求期间有明确 owner，缓存符合预算；不可达/丢失明确报错，
request 不隐式重新发布或携带模型 payload。
publisher receipt cache 不得无界持有 serving lease；保留决策归现有
ModelPreparationCache 预算与淘汰，活动 package/request 持有必要 owner。

### AD-03: ACK constrains placement; Selection authorizes materialization

request 是独立阶段，只携带已准备模型的 reference、digest、epoch 和输入 reference，
不携带整模型字节或预组装 stage 路径。当前实现从 catalog 读取一个已验证的
候选 layer-range 集合，在真实 ACK offers 返回后只选择可行的 Provider placement；
最终 signed Selection/grant 绑定 manifest、role/range、attempt/epoch 和 plan digest。
因此当前候选是“预声明范围 + ACK 后 placement”，还不是“ACK 在多个切分候选中动态决定
范围”。初始 profile 可约束 [0,14)/[14,28)，不改变 prepare 材料。无效/未选
placement 在重型 fetch/runner 前拒绝。若要实现真正的 ACK-driven partition，必须
提供多个带独立 range/成本/能力约束的候选，并让 planner 在 ACK offers 上选择其中一个；
这项能力不能由 exporter stage 文件代替。fixture 使用已有 canonical typed identity
与签名入口，不手拼替代授权。

### AD-04: selected materialization and bounded cache

Selection 成功后 Provider 才读取选中原子层及显式 shared tensors，有界读/文件物化后组装 ONNX；
exporter 产生的 stage manifest 仅提供契约摘要和校验 digest，不得绕过这一步。
禁止整 initializer 下载后切片与预加载整模型 runner。
不可变内容可共享，授权每请求验证；Provider 使用系统唯一、非 run-scoped 的
content-addressed artifact root。Selection/grant 验证后先扫描当前 role 目录，
只接受目录名与其中 `model.onnx` 流式 SHA-256 自洽的 entry；命中后直接复用已有
路径，跳过 layer fetch/copy/assembly。这里的 hash-only admission 是本地可信优化，
不是第二套 manifest/signature 身份协议；缺失、篡改、半写入或 protected
grant/ciphertext entry 均回到正常 authenticated cold path。不因文件名或 cache hit
绕过授权、Selection、KV/会话契约或 runner owner。KV/会话/runner owner 与 Repo
持久材料分离。cold/warm runner 创建数可以不同，但资源回收与计费必须可解释。

canonical source ownership 另有一个 system-wide content-addressed root，由模型、
manifest/source/graph/initializer、node-mapping 和 tokenizer digest 派生 identity。
launcher 在运行前只接受 size/hash 完全匹配的 immutable graph/initializer 对象；命中
时 requester 可通过 hardlink/引用复用，避免每个 run 再复制完整 source。该 source
cache 不绕过 native prepare、Repo manifest、ACK、Selection 或 grant；run-scoped
encrypted Repo、wrapped-key/ciphertext 和 active request lease 仍保持运行期边界。

本轮为资源诊断增加 temporary cache-compatibility mode，默认关闭且不改变正式
Repo contract。兼容模式的 requester 在 prepare 阶段交付一个绑定 system-wide plain
source-cache namespace 的 metadata-only publication receipt，因此不创建 run-scoped
encrypted Repo 或复制大 payload；普通 authenticated prepare、manifest、ACK、Selection、
grant 和 placement 仍必须执行。Provider 只有在本次 authenticated Selection/grant/
placement 已验证后，才可从该 cache 读取 graph/initializer；identity schema、model/
manifest digest、大小和逐文件 SHA-256 均须通过，缺失或不匹配即 fail closed，不得
fallback 到未认证本地文件。该模式跳过 post-Selection Repo root/source material fetch，
但仍执行现有 native assembly、ONNX input/KV/output、lineage 和 runner 校验；
	material-backed source 仍不支持，必须 fail closed；protected role 只有在既有
	ProtectedRuntime 已由 authenticated grant 建立时才可在该诊断模式下使用 verified
	local source 和 hash-only assembled model cache，不能把该路径当作 protected Repo
	material/ciphertext 的正式替代。它只用于 r164 之后的 diagnostic，不能计入
	FR-003/FR-019 或 `QWEN_TWO_PROVIDER_PASS`。

### AD-05: causal events and independent output

使用已有 Core/NDN hidden-state dependency；模型材料 fetch 与 upstream tensor
fetch 不同，assembly 和等待输入可交错，首段没有 upstream。
按 [causal contract](contracts/placement.md) 验证，不假设跨 Provider 日志全序。
已注册 Waf target `spec189-two-provider-oracle`（源码
`examples/Spec189TwoProviderOracle.cpp`，输出 `<build>/examples/spec189-two-provider-oracle`）是 C++ 日志/身份 checker，
需修正事件假设并配合独立输出 reference。计算 digest 本身不能证明推理正确。

### AD-09: shared ingress, model-specific execution gate

YOLO 与 Qwen 共用 prepare、Repo、request、ACK、Selection、授权和 Provider ingress；
YOLO 的 `NATIVE_POSTPROCESS` 或小型输出不覆盖 Qwen 的分阶段材料读取、hidden-state
handoff、ORT runner load、跨 Provider stream liveness 或资源峰值。Qwen 资格必须在
同一真实链上观察每个 committed selected Provider/role 的 authenticated progress，
并按 `{providerName, providerSelectionDigest, operationId}` 和 `(epoch, sequence)`
验证新鲜度。任何只监听 terminal Provider 的实现都视为 liveness 缺陷；静态 selector
通过后仍必须以新安装候选重跑 MiniNDN。

### AD-10: core lineage before edge binding

Initial generation lineage is authenticated request/generation state and is
edge-less until the executing Provider binds the output edge. Local causal-position
materialization may call `validateCore()` at this boundary. Any encoded, decoded,
or published dependency edge must call full `validate()` and contain non-empty
validated producer/consumer roles. This is a validation-scope correction only; it
does not add a Qwen adapter or change the lineage wire format.

### AD-06: safety before expensive work

Host guard/受控 stop 前置所有真实模型准备/发布/MiniNDN；其 safety entry 已可
独立复用，但不再创建行政任务，剩余 counters 由 T003/T006 的真实 owner 交付。
新 native counters 随 T003/T006 的 owner 接入，不能倒过来阻断其小 fixture。
T009 前核对完整采样，运行中记录峰值。RESOURCE_BOUNDARY 是诊断，不是协议失败或完成。

### AD-07: candidate separate from run identity

不可变 candidate 包含 source content、global ABI、binaries、model/manifest、
profile/topology/oracle；commit/build path 是 provenance。
run-id/request id/运行期 key/path 独立记录并绑定 candidate。
只重验变更影响的后继证据，文档改动或新 run-id 不使有效 build/model 证据失效。

### AD-08: audit findings have explicit applicability and owners

审计发现按当前 candidate path 分流：material-only consumer 的接线属于 T003/T006
并必须经真实 protected ingress 验收；准备峰值和混合 quota 属于 T003 的生产反例；
Conversation generation owner 属于 T009 的同 handle 两请求反例；filesystem fd 错误
路径属于 T003 的发布错误门。catalog snapshot/history、segmented compatibility 和
Python power-loss durability 不属于当前 native qualification 的默认路径，只有实际
调用方进入候选后才提升依赖；不得用本 Spec 的局部 selector 代替这些通用契约。

所有这些门都必须记录 `static`、`compile/link`、`runtime/test`、`unobserved` 四类
状态；未完成的 audit follow-up 不得通过降低资源阈值、增加 timeout 或复制 digest
来关闭。

## Production paths and design-to-code binding

| Binding | Actual source / caller | Remaining proof |
| --- | --- | --- |
| Q189-PREP | Runtime::prepare, NativeCanonicalPreparationCatalog/Publisher, DI_NativeRequester | 原子 Qwen 材料发布与真实 Repo 可达性 |
| Q189-REPO | NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoSourceProvider.hpp; RepoCore | receipt/事务/文件对象接入实际 producer/consumer |
| Q189-WIRE | PreparedModel, NativeRequestPreparation/Envelope, Core ACK/Selection, NativeProviderHandler | 同 handle 复用、ACK 后规划、生产 ingress no-fetch |
| Q189-ASSEMBLY | NativeCanonicalOnnxAssembler, NativeOnnxAssemblyWorker, provider factory | 选定材料替代整 initializer fetch |
| Q189-HANDOFF | NativeEpochCoordinator, NativeProviderHandler, Core dependency path | endpoint/attempt/tensor 绑定与 terminal/drain |
| Q189-LINEAGE | GenerationEpochLineageV1, OnnxRuntimeModelRunner, NativeEpochCoordinator | local core validation before edge binding; full validation at wire/edge publication |
| Q189-ORACLE | examples/Spec189TwoProviderOracle.cpp + existing native selectors | 因果与独立输出校验；CLI 不等于 oracle |
| Q189-EVIDENCE | Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py and maintained launcher | guard、同 handle 两请求、新 run-id 重复成功 |
| Q189-AUDIT-OWNER | RepoCore/FilesystemRepoStoreBackend、Runtime/Conversation、T003/T009 C++ selectors | F02/F05/F08/F09 的真实 owner、失败/并发反例和 resource accounting；F03/F04/F06/F07 仅在适用调用方接入后提升 |

## Logical Batch Quality Plan

每批 native selector 运行前检查 `readelf`/`ldd` 实际加载的 DI/Core 库及 SHA-256。
本机 global-first RUNPATH 会优先使用 `/usr/local`；若本批生成的共享库与全局库不同，
先安装对应目标并核对一致，再测试。头文件结构变化但 SONAME 未变也必须执行此门。
不通过临时 LD_LIBRARY_PATH 绕过，不因单一 DI 安装变化重编未变 Core/Repo。
Waf 原生 install 与附带 Python editable hook 分开记录；后者失败不能写 binding PASS。

唯一成员注册表见 [batch-execution.md](batch-execution.md)；
顺序 B189-0 → B189-1 → B189-2 → B189-3 → B189-5。
6 个活动任务及旧 ID 合并映射见 [tasks.md](tasks.md)。资源门随 B189-1/B189-3
交付并在 B189-5 最终收口。
B189-1a protected storage、B189-1b atomic preparation 各有独立验证出口，
在同一能力任务/证据内顺序执行，不等全部 T003 完成才首次构建。

B189-1c 是 T003 的审计 follow-up 出口：准备峰值分类计数、混合 reservation quota
和 filesystem fd error ownership。它必须在真正的 protected candidate 之前完成；
不能用旧 B189-1a/B189-1b 组件结果替代。T009 增加 generation-guarded same-handle
交错门；F03/F04/F06/F07 只有实际调用方进入本候选时才加入依赖图。

每小任务完整编码/fixture/调用方/build registration 后冻结五 lane review，
修复复审，再同批组合审查，最后一次增量构建和规定 C++ 测试。
等待期间不实现依赖该门的下一任务；批次达稳定出口即测试，不无限扩张。
T006 的 small-fixture owner/cancel 适用 ASan/UBSan；
依赖不支持时记录限制并运行具体 owner/counter 反例，不能省略生命周期测试。
12 GB full-model run 不强制 sanitizer。每次 full-model run 仍必须通过 guard，
但不为 guard 单独建立批次或重复构建。

## Evidence reuse and invalidation

| Change | Retain | Recheck |
| --- | --- | --- |
| Docs/task order | 有效组件/build/model 证据 | 文档链接、任务依赖与验收一致性 |
| Material schema | 全局依赖与无关 Core | producer/consumer/schema negatives/受影响 MiniNDN |
| Placement/grant | 兼容 Repo 内容 | ingress/no-fetch/两 Provider 绑定 |
| Assembly/handoff | 未变 prepare 和依赖证据 | 原生 selector、输出/因果/lifecycle/full path |
| Global ABI | 无关文档与模型源 | 受影响目标/绑定/loader 及 downstream |
| New run-id | 同一 candidate | 新运行证据，不重编或重导出 |

## Formal validation order

T001 收敛接线 → T003 prepare/Repo/request → T005 选择门 →
T006/T007 范围组装/handoff 组合验证 → T009 同 handle 两请求及独立重复。
每次失败保留原始边界，遵守
[experiment retry loop](../../skills/speckit-code-design/references/experiment-static-review-loop.md)；
日志级别/timeout 改变不算功能修复，诊断运行必须注明目的。

当前 B189-3 的稳定出口包含三层：Selection-scoped authenticated admission sequence、
每个 committed Provider/role 的 progress binding，以及 local ONNX core lineage
validation 与 edge publication full validation 的边界。前两层已有 focused C++
证据，r161 已定位并修正第三层，affected DI targets 已安装；旧 r163 Provider-side
skip 仍被 prepare-time protected publication 的 disk boundary 先挡住。requester-side
metadata-only seam 已通过静态审查、受影响 DI 编译和安装；r164 新 run ID 在 host
guard admission 阶段因 `diskFreeBytes=4232839168` 低于 4 GiB 门限而停止，尚未观察
后续生产边界。获得足够磁盘空间后再以新的 run ID 继续，不加入组件职责、扩大全局设计
或盲目提高 timeout。只有新的 run 越过 local materialization，才开启 handoff/terminal
方向的下一项局部修复。

## Closure rule

所有真实验收与文档交付完成才结束 Spec189；分类失败仍 PARTIAL。
实现中的 API/行为变化按 Design/MANAGEMENT.md 同步当前/目标契约及 PDF；
本轮审计只修正文档；已有未验证 range-store/lease API 草稿保持 PARTIAL，
其源码/API/中文契约与 PDF 同步属于 B189-1a 交付，不冒充已验当前设计。
