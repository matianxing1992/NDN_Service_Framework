# Feature Specification: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Development Branch**: `Experimental` | **Feature**: `189-qwen-two-provider-minindn`

**Created**: 2026-09-18

**Status**: IN_PROGRESS

**Input**: 用户要求把最初的本机目标独立出来：用真实 Qwen3-0.6B、两个 CPU Provider 和 MiniNDN 验证完整 prepare/Repo/ACK/Selection/分层执行链，不能用导出、单 Provider 或伪造状态代替。

## Purpose and scope

**Resident runner decision — 2026-09-22**: 已加载ONNX session应由Provider以有界
内存缓存短期保留，允许后续匹配请求复用，不因一次request结束立即销毁。闲置超时、
容量/替换需求或显式eviction可释放无活跃lease的session；关闭Provider先停止准入、
drain/cancel，再释放内存owner。磁盘模型缓存与会话KV独立管理，缓存不替代本次
授权、Selection和兼容性校验。当前为接受的目标，跨request驻留尚未实现；
[r260](evidence/b189-r260-runner-reuse.md)先修逐token重载，再验证跨request生命周期。

**Conversation locality decision — 2026-09-22**: 多轮对话通过已有placement策略子类
优先沿用上一轮合法角色—Provider分配，避免不必要的KV迁移；本轮授权、ACK、资源
与精确KV恢复校验不省略。模型缓存命中不能代替KV有效性。显式保留期限须贯通
配置、Provider实际store和receipt，续轮不能复活过期状态。当前实现/验证进度见
[r259](evidence/b189-r259-affinity-retention.md)，不得将计划写为多轮验收PASS。

Spec189 只处理一个可判定目标：在当前 6-core/12-GB 主机上，使用真实 Qwen3-0.6B 和两个 native CPU Provider，完成可重复的完整成功请求。失败时在第一个明确边界停止并保留证据，但失败分类不满足完成条件。它不重做 Spec188 已验证的 bounded prepare/request/YOLO 工作，也不自动构建 SIF、上传 TigerCluster 或运行 QWEN 集群资格。

目标调用链固定为：

```text
Qwen snapshot
  → native prepare: graph/initializer identity + topology-independent atomic layer/shared materials
  → Repo manifest and reusable atomic material references
  → payload-free request
  → Core ACK → partition planning
  → authenticated Selection with two placement entries
  → Provider-0/Provider-1 fetch only assigned layer ranges
  → C++ assembly and CPU execution
  → hidden-state handoff / terminal logits
  → cleanup, lease drain and resource evidence
```

Stage export is preparation input only. A generated ONNX file, an ORT session, a preloaded provider runner, a static selector, or a synthetic ACK/Selection is never a product PASS.

## Explicit acceptance target

连续生成要求（r256）：下一次两节点诊断使用 `maxNewTokens=1024` 和有效聊天输入，
生成至配置的 EOS/EOT 或 token 预算耗尽，不设置字符上限。必须核对多token输出、
终止原因、最后token的KV finalization及checkpoint；单token成功不替代该要求。
缓存诊断仍不等于以下完整Repo资格。

本 Spec 的唯一主线固定为：**MiniNDN + Qwen/Qwen3-0.6B + 2 个执行节点 + NDNSF-DI
原生 C++ 推理**。这里的“2 个节点”专指两个实际执行模型分片的 native CPU
Provider（当前 profile 为 `ucla` 和 `arizona`）；`memphis`、`neu` 以及 Repo/Core
属于支撑该请求的基础设施角色，不计入两个执行节点。

只有同一个新鲜 candidate 在 MiniNDN 中走完真实
`prepare → Repo → ACK → Selection → placement-bound fetch → Provider-0/Provider-1
assembly/execute → hidden-state handoff → terminal output → drain/cleanup`，并由
C++ oracle 验证输出、两 Provider 的选中材料和资源/清理结果，才可写入
`QWEN_TWO_PROVIDER_PASS`。stage export、单 Provider ORT、Python-only runner、缓存
命中、静态 selector、伪 ACK/Selection 或局部事件均不是该目标的完成替代物。

## Current planning checkpoint — 2026-09-21

本轮已将旧 Qwen stage exporter 的边界语义移入 Provider 侧原生 materializer：已认证
的 role input/output contract 现在用于重建内部 handoff（例如
`hidden_states_out`）的 ONNX graph boundary，selected node/dependency 仍保持有界。
新增 stage-boundary C++ selector 与既有 15-case canonical publisher regression 均通过，
受影响 native target 已安装并核对 `/usr/local` 身份。安装日志同时记录了
`py_repoclient` host-binding 环境缺口；这不改变 native install 结果，但在 runtime
preflight 前必须显式处理或确认其不属于 launcher 的加载闭包。

该 checkpoint 只关闭 stage materialization 的 focused compile/test/install 单元；尚未
重跑新的 MiniNDN 请求，仍没有 terminal output、独立 oracle、两次 repeat 或
`QWEN_TWO_PROVIDER_PASS`。下一门是安装闭包与实际加载路径的静态确认，然后以新 raw
run ID 运行一次完整两 Provider Qwen candidate。

## Current qualification boundary — 2026-09-20

Spec188 的 YOLO 通过结果只证明其已关闭的轻量本地路径；它不能替代本 Spec 的
Qwen 两 Provider 资格。两者共享 prepare、Repo、request、ACK、Selection、授权和
Provider 接线，但 Qwen 还会触发分阶段材料读取、Provider-0 到 Provider-1 的
hidden-state handoff、ORT runner 建立和较长的 assembly 等待窗口。

当前真实 Qwen 证据已越过共同入口和资源边界：r155 首次证明 system-wide
content-addressed source cache 写入并命中准备路径；r156 通过释放 source protobuf
越过先前的 `MemAvailable` assembly 边界，进入 Provider-0 `RUNNER_READY`。r159–r161
继续在真实 MiniNDN 观察到两个 Provider 的执行入口、Provider-0 assembly/runner 和
Provider-1 dependency fetch，但 Provider-0 在 generation lineage 首个使用点失败，
Requester 随后返回 `NATIVE_STREAM_FAILED`。这些运行没有 terminal response、模型输出、
oracle、repeat 或资格 PASS。

r161 的源码边界已经确认：初始 lineage 含有合法的 authenticated core generation
state，但在本地 ONNX causal-position materialization 时尚未绑定 edge-local
`producerRole`/`consumerRole`；此前误用了要求完整 edge routing 的 `validate()`。当前
已加入 `validateCore()`，并完成受影响 DI closure 的编译与全局安装；wire
encode/decode、edge publication 仍保持 full `validate()`。这不是 Qwen ONNX
input/KV/output contract mismatch，因此不新增 adapter。

随后 r162 使用该已安装修正进入真实 MiniNDN，并在两个 Provider 已完成授权入口后
按计划受控停止，避免继续执行普通 Repo material fetch/assembly；这不是新的协议失败。
当前已实现一个显式、默认关闭的 temporary cache-compatibility mode：它只在已认证
Selection/grant/placement 通过后，从已校验的 system-wide plain source cache 读取
graph/initializer，跳过该阶段的 Repo material fetch；它不绕过 prepare、manifest、
ACK、Selection、授权或 placement。material-backed source 仍不支持；若 Selection 已
建立既有 `ProtectedRuntime`，protected role 可在该显式诊断模式下使用 verified local
source，并按 `assembled/<role>/<sha256>/model.onnx` 的实际 SHA-256 复用已组装模型，
但普通 protected Repo qualification 仍必须走加密 Repo/ciphertext 路径。旧的 Provider-only
r163 版本已通过 source-cache identity/hash 校验，但在 ACK/Selection 之前仍由 host
guard 的 `RESOURCE_BOUNDARY:diskFree` 停止，因为 requester prepare 仍产生约 1.4 GiB
的 protected Repo payload。现在 requester-side metadata-only publication seam 已实现，
并已通过静态门、受影响 C++ 编译和全局安装；它在兼容模式下不创建 run-scoped encrypted
Repo，只交付经过 source-cache namespace 绑定的 metadata receipt，普通 authenticated
prepare/manifest/ACK/Selection/授权/placement 仍保留。r164 已用全新 run ID 尝试验证
materialization，但在 requester/Provider 启动前被 host guard 的
`RESOURCE_BOUNDARY:diskFree` 停止：`diskFreeBytes=4232839168` 低于 4 GiB 门限；
没有创建新的 run-scoped encrypted Repo，也没有进入 ACK、Selection、assembly、NDN
handoff、terminal output、drain、warm cache 或 repeat。获得足够磁盘空间后才可用新的
run ID 继续。T003/T005/T006/T007/T009 继续保持 `PARTIAL`；不得用 source-cache hit、
cache-compatibility diagnostic、focused selector、YOLO 结果或单次局部事件把它提升为
`QWEN_TWO_PROVIDER_PASS`。
本次边界与 closure 记录见 [boundary revision evidence](evidence/b189-spec-revision-20260920.md)
及 [r161 evidence](evidence/b189-r161-lineage-validation-diagnostic-20260920.md)。

## User Scenarios & Testing

### User Story 1 - Prepare and publish a split Qwen candidate (Priority: P1)

应用用真实 Qwen3-0.6B snapshot 调用 native prepare。prepare 验证 canonical graph、initializer、模型配置、层范围和 digest，把 immutable layer packages 及 manifest/reference 写入 Repo；返回 handle 不携带 1.5-GB payload。

**Why this priority**: 没有 prepare-time、可复用的分层制品，request 会再次复制或发布整模型，正是原始多 Provider CPU 失败的来源。

**Independent Test**: C++ `DI_NativeArtifactAuthority`/Repo selector 由真实 Qwen candidate 驱动，检查 manifest、graph identity、initializer digest、原子层/shared 索引、commit count 和 source ownership；失败时检查 staging cleanup。

**Acceptance Scenarios**:

1. **Given** a local Qwen snapshot and an immutable source identity, **When** `prepare` runs, **Then** one committed manifest references topology-independent atomic layer/shared materials; final partitioning occurs after ACK.
2. **Given** the same identity is prepared again, **When** the second prepare runs, **Then** Repo returns the existing manifest/reference without a second full publication or binding a Provider placement; execution leases are owned by their requests.
3. **Given** graph, initializer, config, layer-range or digest mismatch, **When** prepare validates, **Then** it rejects before ACK and leaves no READY manifest or staging residue.

### User Story 2 - Bind ACK and Selection to two placements (Priority: P1)

请求只携带 model reference、input reference 和 request options。Core 根据真实 provider offers 返回 ACK，随后 Selection 明确绑定 Provider-0/Provider-1、layer ranges、manifest digest、authorization epoch 和 attempt identity。

**Why this priority**: placement 是决定“谁能取哪些层”的安全和内存边界；未确认 placement 不能提前拉取大制品。

**Independent Test**: C++ `DI_NativeRequester` 加 `App_ServiceController`/Core path；C++ oracle 解析真实 ACK/Selection Data，确认两个 provider identity 和 ranges 与 manifest 一致，并确认 request wire 没有 model bytes。

**Acceptance Scenarios**:

1. **Given** two valid offers, **When** ACK and Selection complete, **Then** both providers receive a signed placement-bound selection with the same manifest digest and non-overlapping layer ranges covering the model.
2. **Given** a provider is not selected or a range is outside the manifest, **When** it receives a candidate, **Then** fetch count and runner creation remain zero and the request is rejected.
3. **Given** an authorization epoch or manifest digest changes between ACK and Selection, **When** the plan is checked, **Then** Selection is rejected before layer fetch.

### User Story 3 - Execute the split model on two native CPU Providers (Priority: P1)

两个真实 Provider 只从 Repo 读取各自 layer package，使用 native ONNX Runtime assembly/runner 执行；Provider-0 的 hidden state 通过真实 Core/NDN handoff 传给 Provider-1，末段返回 terminal logits 或明确的 execution error。

**Why this priority**: 这是“本地多 Provider Qwen 能跑”的唯一直接证据；组件 selector 或单 Provider ORT session 都不能证明它。

**Independent Test**: native `di-native-provider`, `DI_NativeOnnxAssemblyWorker` and the maintained MiniNDN runner execute the same candidate. The C++ oracle checks layer fetch, assembly, execution, hidden-state transfer, terminal response and child exit.

**Acceptance Scenarios**:

1. **Given** a valid two-provider Selection, **When** one request executes, **Then** both providers fetch only their assigned layer packages, each selected range has a valid runner (a verified warm cache may reuse it), and a terminal result is returned.
2. **Given** the first provider returns a hidden-state handoff, **When** the second provider receives it, **Then** it validates attempt/model/placement identity before execution and returns a terminal result with a named output digest/top-token oracle.
3. **Given** assembly, ONNX, NDN handoff or CPU execution fails, **When** the request terminates, **Then** the C++ oracle reports the first failure boundary and both providers drain without stale runners.

### User Story 4 - Stay within a measured local resource envelope (Priority: P1)

实验在每个阶段采集 RSS、MemAvailable、swap、disk usage、Repo resident bytes、materialization bytes 和 child process state；资源 guard 在不可安全继续时停止请求并保留 `RESOURCE_BOUNDARY`，不伪造协议失败或 PASS。

**Why this priority**: 当前主机只有 12 GB RAM，必须区分“协议接通”和“内存峰值仍不可运行”。

**Independent Test**: C++ counters are authoritative for Repo/runner ownership; Python only samples host/process metrics and enforces a bounded stop. A controlled small-fixture stop validates the guard before full-model work; the final real run records normal peak/drain metrics. Any changed profile/threshold is recorded with its own identity.

**Acceptance Scenarios**:

1. **Given** a run below the configured memory floor, **When** it reaches terminal response, **Then** peak and post-drain metrics are stored with the exact candidate identity.
2. **Given** MemAvailable or swap safety floor is crossed, **When** the guard stops the run, **Then** children are drained/killed deterministically and the result is `RESOURCE_BOUNDARY`, never `QWEN_TWO_PROVIDER_PASS`.
3. **Given** request completion, **When** cleanup finishes, **Then** active leases, runners, temporary layer windows and Repo request counters return to their declared baseline.

### User Story 5 - Produce reproducible evidence and a clear verdict (Priority: P2)

每次实验使用唯一 run id 和 immutable candidate tuple，保存 source/build/ABI/profile/model/manifest/selector identity、原始日志、C++ oracle、资源样本和 first-failure classification。只有完整链路成功才写 `QWEN_TWO_PROVIDER_PASS`。

**Why this priority**: 结果要能区分 export、preflight、Repo、ACK、Selection、execution、resource 和 cleanup 边界，避免重复几天前的误判。

**Independent Test**: evidence checker compares hashes and event sequence against the candidate manifest and C++ selector output; a deliberate stale/mismatched artifact is rejected.

**Acceptance Scenarios**:

1. **Given** a fresh run, **When** evidence is finalized, **Then** all required event markers and child exit statuses agree.
2. **Given** a stale stage, manifest, binary or profile hash, **When** dispatch preflight runs, **Then** it exits before MiniNDN/NDN mutation and records `CANDIDATE_IDENTITY_MISMATCH`.
3. **Given** an incomplete run, **When** the report is written, **Then** it remains `PARTIAL`, `BLOCKED`, `RESOURCE_BOUNDARY`, `PROTOCOL_BOUNDARY` or `UNQUALIFIED` with a next gate.

## Acceptance Evidence Contract

| Story / FR | Production entry / callers | Observable outcome | Independent oracle / C++ selector | Negative / recovery boundary | Dynamic profile / invariant | Evidence owner / path | Batch / Coverage reference |
| --- | --- | --- | --- | --- | --- | --- | --- |
| US1 / FR-001..FR-005 | `Runtime::prepare`, `NativeCanonicalPreparationCatalog`, `NativeCanonicalArtifactPublisher`, Repo owner | one manifest plus topology-independent material references; no payload in handle | `DI_NativeArtifactAuthority`, Repo C++ selector | digest/range mismatch, staging abort, duplicate prepare | asan-ubsan; commit/source-owner counts | `evidence/b189-prepare.md` | B189-1 |
| US2 / FR-006..FR-009 | `DI_NativeRequester`, Core controller/ACK/Selection path | payload-free request and two signed placements | production-ingress C++ assertions plus existing `spec189-two-provider-oracle` | no offer, stale epoch, unselected provider, overlap/out-of-range | none; placement and wire invariants | `evidence/b189-placement.md` | B189-2 |
| US3 / FR-010..FR-015 | `di-native-provider`, `DI_NativeOnnxAssemblyWorker`, Core handoff | selected/shared material fetch and assembly, measured cold/warm runner reuse, hidden-state handoff and valid terminal result | existing assembly/handoff C++ selectors plus corrected Spec189 event checker and independent output assertions | assembly/ORT failure, handoff mismatch, cancellation, provider stop | small-fixture asan-ubsan where feasible; runner/lease/drain counts | `evidence/b189-execution.md` | B189-3 |
| US4 / FR-016..FR-019 | experiment runner and native ownership counters owned by T003/T006 and consumed by the final runner | measured peak, safety stop, zero post-drain residue | C++ counters; Python host sampler only | memory floor, swap pressure, disk floor, child leak | resource guard; post-drain zero invariant | `evidence/b189-resource.md` | B189-1/B189-3/B189-5 cross-cutting gate |
| US5 / FR-020..FR-025 | maintained scripts, evidence checker and global dependency preflight | immutable tuple, event sequence, first boundary, verdict and one installed native closure | C++ oracle output plus evidence checker and dependency identity checks | stale identity, timeout, partial logs, missing/ABI-incompatible global dependency | host `/usr` + `/usr/local` closure; no checkout or temporary prefix | `evidence/b189-convergence.md`, `evidence/b189-build-20260918.md` | B189-5 |
| Audit gates / FR-027 | Runtime/ONNX preparation owner | category-level peak and cleanup evidence | named C++ preparation counter selector | coexistence, cancel, failed retry | source/material/encryption/ORT owner counters | `evidence/b189-prepare.md` | B189-1c / T003-R1 |
| Audit gates / FR-028 | RepoCore range/vector/Data packet admission | one committed+staged+reservation quota | C++ mixed-write/replacement selector | quota overflow, replacement failure, reservation leak | old identity remains readable | `evidence/b189-prepare.md` | B189-1c / T003-R2 |
| Audit gates / FR-029 | Conversation/PreparedModel handle owner | generation-safe installation and close | C++ terminal/next-turn barrier selector | stale turn cannot overwrite/cancel newer turn | one active owner and one terminal state | `evidence/b189-convergence.md` | B189-5 / T009-R1 |

## Functional Requirements

- **FR-001**: The candidate MUST use the pinned local Qwen3-0.6B snapshot and record model revision, config digest, tokenizer digest and file hashes.
- **FR-002**: Native `prepare` MUST validate canonical graph identity, initializer digest/size, layer count, atomic material coverage and shared dependencies before creating a READY reference; it MUST NOT fix the final Provider partition.
- **FR-003**: Repo publication MUST durably commit a versioned manifest and topology-independent atomic materials reachable through the normal Repo path; request-time publication of canonical graph/initializer MUST be zero on a warm prepared handle.
- **FR-004**: The prepared handle MUST contain reference/lease identity only; it MUST NOT retain the complete model payload solely for a future request.
- **FR-005**: Duplicate prepare MUST be idempotent for immutable identity and MUST expose commit/lookup counters to the C++ oracle.
- **FR-006**: The request envelope MUST contain model reference, manifest digest, epoch, input reference and options only; it MUST reject embedded full-model bytes.
- **FR-007**: ACK MUST be emitted by the real Core path and MUST precede Selection and Provider layer fetch.
- **FR-008**: Selection MUST bind two provider identities, layer ranges, manifest digest, authorization epoch and attempt identity.
- **FR-009**: Unselected, stale, overlapping or out-of-range placement MUST fail before Repo layer fetch and runner creation.
- **FR-010**: Provider-0 and Provider-1 MUST fetch only their selected atomic materials and explicitly declared shared dependencies and MUST verify package digest, role, range and manifest identity before ORT load.
- **FR-011**: The native assembly path MUST create runners from the selected layer packages, not from a preloaded whole-model fixture.
- **FR-012**: The two-provider path MUST carry a real hidden-state handoff or an explicitly named production pipeline equivalent between providers.
- **FR-013**: A successful final response MUST pass a C++ assertion against an independent fixed-input reference (shape/finite and frozen tolerance or top-token); a digest alone is identity evidence. Execution failures MUST be classified and MUST NOT satisfy success.
- **FR-014**: Cancellation, assembly failure, provider stop and Core close MUST drain Face/io_context/timer/callback dependencies before fixture destruction.
- **FR-015**: Active request/worker leases and temporary materialization MUST drain after terminal response or classified stop. Retained prepared materials and idle runners MUST have an explicit bounded cache owner and separate counters; after final handle release plus eviction, or Runtime close/drain, their counters MUST return to the declared baseline. Cache retention MUST NOT create an unbounded second ownership path.
- **FR-016**: The experiment MUST sample RSS, MemAvailable, swap, disk free, Repo resident bytes and child states at named lifecycle points.
- **FR-017**: A resource guard MUST stop before unsafe host exhaustion and MUST classify the result as `RESOURCE_BOUNDARY` with the first observed metric.
- **FR-018**: A successful run MUST record peak resource values and post-drain values under the same candidate tuple.
- **FR-019**: No run may claim `QWEN_TWO_PROVIDER_PASS` unless prepare, Repo commit, ACK, Selection, both provider executions, terminal response and cleanup are all observed.
- **FR-020**: The immutable candidate MUST bind source content, ABI/library hashes, model/material digests, profile/topology and binary/oracle hashes. Commit/build path are provenance; run id and request/key/path identities MUST be recorded separately and bound to that candidate.
- **FR-021**: The dispatch gate MUST reject stale or mismatched candidate inputs before creating MiniNDN nodes or mutating NDN state.
- **FR-022**: Python MUST only orchestrate external facilities, process lifecycle and host sampling; native behavior assertions MUST be C++.
- **FR-023**: Every failed or stopped run MUST preserve raw logs and a durable evidence record with `static`, `compile/link`, `runtime/test` and `unobserved` miss classes.
- **FR-024**: Spec189 MUST not start SIF/Tiger work automatically; local evidence is a prerequisite for a later immutable delivery candidate.
- **FR-025**: Host native builds MUST consume one installed global dependency closure: Boost 1.71 from the verified system pair and all other NDNSF direct libraries/archives from their declared global roots. Missing or ABI-incompatible dependencies MUST be installed and verified before configuration; checkout, `/tmp`, `.codex-tmp` and per-run dependency prefixes MUST be rejected as build or runtime inputs.
- **FR-026**: Qwen-specific material names, layer maps, experiment profiles, resource thresholds and oracle fields MUST remain candidate-local. They MUST NOT become global Core/Repo defaults or public API fields unless a separate design change records the cross-Spec contract and migration.
- **FR-027**: Native preparation MUST account for every model-sized representation it creates or retains, including source, initializer, material payloads, encryption buffers and ORT preparation buffers. The C++ evidence MUST expose the owner and peak contribution of each category; post-publication source release does not prove that the preparation peak is within the resource envelope.
- **FR-028**: Every Repo publication path used by this candidate MUST apply one logical quota over committed bytes, staging bytes and outstanding range reservations. Replacement, cancellation and failed retries MUST release only the reservation/object identity owned by that operation; a successful disk-space check alone is insufficient.
- **FR-029**: Conversation/PreparedModel handle installation and exceptional cleanup MUST be generation-guarded. A completed older turn MUST NOT overwrite or close a newer active turn, and the C++ regression MUST control the terminal/next-turn interleaving.
- **FR-030**: A streamed two-provider request MUST accept only fresh authenticated progress from every committed selected Provider/role, bound by `{providerName, providerSelectionDigest, operationId}` and monotonic `(epoch, sequence)`; terminal-Provider-only progress MUST NOT qualify the request while another selected Provider is assembling or publishing an upstream tensor.
- **FR-031**: The temporary cache-compatibility diagnostic mode MUST be explicit and default-off; after authenticated Selection/grant/placement it MAY read only a verified system-wide plain source cache and a hash-verified content-addressed assembled model cache, MUST fail closed on identity/hash/size mismatch or missing `ProtectedRuntime` for a protected role, MUST NOT be used for normal protected Repo qualification, and MUST NOT satisfy the normal Repo or two-provider qualification requirements.

## Success Criteria

- **SC-001**: A fresh prepare produces one verified Repo manifest of topology-independent materials. The same live handle serves two independent requests with zero additional model publication; ACK-driven plans bind two non-overlapping ranges.
- **SC-002**: The real Core path emits ACK and Selection that the C++ oracle binds to both provider identities and the manifest digest.
- **SC-003**: Both native CPU Providers fetch, assemble and execute their assigned ranges, and one independently validated successful terminal result reaches the requester; classified failure does not meet this criterion.
- **SC-004**: Successful completion leaves no active runner, lease, callback or temporary layer window beyond the declared baseline.
- **SC-005**: Resource evidence includes at least 1-second samples around prepare, ACK, Selection, provider fetch, execution, terminal and drain; a safety stop is deterministic.
- **SC-006**: Two independent runs of the same immutable candidate, each with prepare-once/two-request reuse in one live requester, MUST both pass output, resource and cleanup checks. Failed repeats remain incomplete.
- **SC-007**: During a streamed two-provider request, authenticated progress from each committed selected Provider/role MUST keep the bounded stream window alive; a terminal-only progress path is an execution-liveness failure and cannot satisfy SC-003.

## Non-goals and assumptions

- SIF creation, TigerCluster upload, Slurm execution, remote qualification and production deployment are Spec189 non-goals.
- Qwen text-generation quality beyond the named one-input C++ oracle is not claimed; a bounded independent correctness reference is required here; broad numerical/quality campaigns remain out of scope.
- The initial ACK-stage policy constrains ranges [0,14) and [14,28). Changing placement policy invalidates affected execution evidence, not compatible topology-independent prepared material.
- The host may fail the resource gate; that is a valid diagnostic result and does not become a protocol PASS.
- Existing Spec188 evidence remains historical/bounded evidence and cannot be reused as Spec189 full-path evidence.

## Static audit and runtime-boundary rules

Static review is an early defect gate, not a substitute for the native run.
Before starting a new MiniNDN candidate, the plan must statically and in
preflight verify the service-scoped role policy, protected-grant registry/key
closure, ONNX dynamic-KV state contract, graph-size planning budget, privileged
Python module closure, run-scoped publication space and candidate-derived
digests. The production review must also compare authenticated V3 dependency
endpoint digests with the edges consumed by the generation coordinator.

Provider events MUST satisfy the [causal contract](contracts/placement.md), not a total log-line order. Model assembly and upstream tensor fetch can interleave; the first stage has no upstream fetch. Execution requires verified authorization, runner and input; the terminal role returns the final result and all participants drain. A requester stream gap is a symptom, not a root-cause classification. Missing markers remain UNOBSERVED. ACK/Selection/grant verification alone never proves execution.

Generation lineage has two validation scopes: `validateCore()` is permitted only
inside the local adapter path before edge-local producer/consumer binding, while
full `validate()` remains mandatory for encoded/decoded and published dependency
edges. This split does not alter the wire schema or permit an edge with missing
routing identity to be published.

## Audit reconciliation — 2026-09-19

The [DI/Repo static audit](evidence/di-repo-design-static-audit-20260919.md) was taken
against an earlier dirty-tree snapshot. Its findings remain durable evidence, but their
status must be reconciled with the later source and selectors before another run:

| Finding | Current Spec189 disposition | Scope and required proof |
| --- | --- | --- |
| F01 material consumer | `LOCAL_REPAIR_PRESENT / QUALIFICATION_OPEN` | The native consumer now reads an authenticated material manifest and selected payloads after Selection, with C++ bounds tests. Real protected Core→Provider ingress, two-provider assembly and cleanup are still required. |
| F02 preparation peak | `FOCUSED_CXX_PASS / QUALIFICATION_OPEN` | The C++ selector records source/initializer/material/publication categories and post-publication cancellation cleanup. `ortPreparationBudgetBytes` is a configured budget, not an ORT allocator/RSS measurement; real Qwen peak attribution and T009's unchanged host gate remain open. See [F02 evidence](evidence/b189-f02-memory-20260919.md). |
| F03 snapshot/cursor race | `DEFERRED / CONDITIONAL` | Applies if this candidate uses catalog snapshot/delta synchronization. The current qualification path is protected material publication/read; no evidence may claim the generic catalog contract is repaired. |
| F04 history-gap recovery | `DEFERRED / CONDITIONAL` | Same applicability boundary as F03; catalog sync would need an explicit snapshot-required/epoch-incarnation gate. |
| F05 reservation accounting | `FOCUSED_CXX_PASS / QUALIFICATION_OPEN` | The C++ mixed range/vector/Data admission selector now uses one logical quota; replacement/failure rollback and the protected publication candidate remain open. See [F05 evidence](evidence/b189-f05-quota-20260919.md). |
| F06 segmented compatibility replacement | `DEFERRED / LEGACY` | Not used by the native protected qualification path. The compatibility helper is not atomic evidence. |
| F07 Python local durability | `DEFERRED / LEGACY` | Python orchestration is not evidence for native Repo durability or Qwen qualification. |
| F08 turn owner race | `OPEN / IN-SCOPE` | Same-handle two-request qualification must include a C++ terminal/next-turn interleaving and generation-guarded close check in T009. |
| F09 descriptor error path | `FOCUSED_CXX_PASS / QUALIFICATION_OPEN` | The backend now has one owning fd and the injected fsync/close selector passes; rename/crash recovery and the complete publication candidate remain open. |

This reconciliation changes task ordering and evidence requirements; it does not lower
the two-provider acceptance chain or turn a focused selector into `QWEN_TWO_PROVIDER_PASS`.

The fresh r56 candidate confirms the next design boundary: both Providers can reach
authenticated `BEFORE_ASSEMBLY` grant verification while the requester still expires on a
silent stream gap. The accepted repair is a provider-authenticated
`ASSEMBLY_ADMISSION` followed by `ASSEMBLY_STARTED`, emitted before the first
root-material fetch. Both statuses share one Selection-scoped monotonic operation
sequence so runner rebuilds cannot restart the operation at sequence two. This is an
observability/liveness handoff, not a new authorization decision, timeout increase, or
qualification result. The rebuilt C++ selectors pass locally; one fresh real candidate
run must observe the marker before any additional component work is admitted. See
[admission/r58 evidence](evidence/b189-admission-sequence-r58-20260919.md).

For a streamed collaboration, the terminal user-side consumer may wait while a
nonterminal Provider fetches selected material or publishes an upstream tensor.
Its authenticated progress allowlist therefore binds each committed participant
as an exact tuple `{providerName, providerSelectionDigest, operationId}`. The
selection digest is Provider-specific because the Selection key envelope is
recipient-bound; a status must match both the outer Provider/digest and its
member Provider/operation identity. Freshness `(epoch, sequence)` is tracked per
tuple, so worker progress followed by terminal progress can each re-arm the
bounded stream gap. The legacy single-Provider path retains its one exact
operation binding. This contract is still subject to the rebuilt C++ selector and
fresh host-native candidate run; it does not advance qualification by itself.

Every bounded retry follows the shared
[experiment static re-review loop](../../skills/speckit-code-design/references/experiment-static-review-loop.md):
freeze the candidate and raw attempt, classify the first missing production
boundary, record a real `Changed gate`, freeze an immutable review snapshot,
obtain read-only static re-review, and only then rebuild and rerun the affected
scope. A changed run id, timeout, log level or copied digest is not a Changed
gate.

## Revision History

- 2026-09-18: Created as a separate Spec after the original Qwen multi-provider CPU goal was found to be broader than Spec188's bounded YOLO closure. Full prepare/Repo/ACK/Selection/placement/Provider/terminal/cleanup evidence is mandatory.
- 2026-09-18: r01-r21 audit: the V3 endpoint projection bug was corrected, but the first post-grant execution boundary remains unobserved. Added mandatory candidate preflight, provider-side execution markers and C++ endpoint-preservation regression before the next MiniNDN retry. Current status remains `IN_PROGRESS`.
- 2026-09-18: Added the shared experiment static re-review loop to every bounded retry; Spec189 now requires a real Changed gate and immutable review snapshot before rebuilding or rerunning.
- 2026-09-18: 固化本机原生构建依赖规则：缺失或 ABI 不匹配的库先安装到全局声明根并记录真实路径与摘要，禁止临时 checkout、`/tmp`、`.codex-tmp` 和每次运行的依赖前缀进入构建或运行闭包。

- 2026-09-18: 架构/进度复审：prepare 改为拓扑无关原子材料，ACK 后规划；资源门前移，十任务合并为七个活动任务，保留所有必要负例；修正事件总序、candidate/run 身份与失败完成条件。r25 仍 FAIL，文档修订不构成功能 PASS。
- 2026-09-18 19:16 -0500: Spec189 收敛审计将资源 guard 从独立 T008 改为跨批次门，T008/T010 合并到 T009；同 handle 重用只在最终真实运行收口；Qwen profile/material/oracle 明确为候选局部契约，避免临时实验设计替代全局 API。当前仍无 `QWEN_TWO_PROVIDER_PASS`。
- 2026-09-19: DI/Repo design audit reconciliation retained the historical F01–F09 findings, marked the later material-only consumer as a local repair rather than qualification, and added explicit preparation-peak, quota-accounting and generation-guard requirements. F03/F04/F06/F07 remain conditional or legacy follow-up items and are not silently treated as fixed.
- 2026-09-19: r56 reached ACK/Selection/grant verification but failed the post-grant stream-liveness boundary before assembly. The design now freezes B189-3 at the authenticated pre-root `ASSEMBLY_STARTED` gate; no blind timeout increase or further component expansion precedes one rebuilt-candidate retry.
- 2026-09-19: r70 crossed authenticated assembly admission and selected-material fetch, then exposed that a terminal stream consumer could not use progress from a nonterminal Provider. The repair contract now requires Provider-specific Selection digests and per-provider/operation freshness; r70 remains `PARTIAL` until static review, C++ validation and a new candidate run complete.
- 2026-09-20 04:34 -0500: 对 r47/r70 与 Spec188 YOLO 结果重新分层：共同 ACK/Selection 链已在 Qwen 运行中观察到，宿主机 `swapIo` 是独立资源门，当前主要生产阻断是跨 Provider progress 聚合。新增 FR-030/SC-007，明确 YOLO 或 terminal-only progress 不能替代两 Provider LLM 资格；progress 修复尚待新安装候选真实重跑。
- 2026-09-20: r155 建立并验证 system-wide canonical source cache；r156 的 source protobuf release 使真实运行越过 assembly memory boundary。r159-r161 将失败边界收敛到 local ONNX causal-position materialization 对 edge-less initial lineage 误用 full validation；新增 `validateCore()` 设计与实现，保持 wire/edge full validation，affected DI closure 已编译并安装，r162 runtime 仍待验证。
- 2026-09-21: 按最新计划将唯一验收目标明确为 `MiniNDN + Qwen/Qwen3-0.6B + 2 execution Provider nodes + NDNSF-DI native C++ inference`；旧 stage exporter 的显式 role boundary 语义已由 C++ materializer 重建，focused selector、15-case publisher regression 和 native install 通过。`py_repoclient` host-binding 缺口与完整 MiniNDN runtime 仍未关闭，产品状态保持 `IN_PROGRESS`。
