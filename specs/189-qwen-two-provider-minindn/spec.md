# Feature Specification: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Development Branch**: `Experimental` | **Feature**: `189-qwen-two-provider-minindn`

**Created**: 2026-09-18

**Status**: IN_PROGRESS

**Input**: 用户要求把最初的本机目标独立出来：用真实 Qwen3-0.6B、两个 CPU Provider 和 MiniNDN 验证完整 prepare/Repo/ACK/Selection/分层执行链，不能用导出、单 Provider 或伪造状态代替。

## Purpose and scope

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
| US4 / FR-016..FR-019 | experiment runner and native ownership counters | measured peak, safety stop, zero post-drain residue | C++ counters; Python host sampler only | memory floor, swap pressure, disk floor, child leak | resource guard; post-drain zero invariant | `evidence/b189-resource.md` | B189-4 |
| US5 / FR-020..FR-025 | maintained scripts, evidence checker and global dependency preflight | immutable tuple, event sequence, first boundary, verdict and one installed native closure | C++ oracle output plus evidence checker and dependency identity checks | stale identity, timeout, partial logs, missing/ABI-incompatible global dependency | host `/usr` + `/usr/local` closure; no checkout or temporary prefix | `evidence/b189-convergence.md`, `evidence/b189-build-20260918.md` | B189-5 |

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

## Success Criteria

- **SC-001**: A fresh prepare produces one verified Repo manifest of topology-independent materials. The same live handle serves two independent requests with zero additional model publication; ACK-driven plans bind two non-overlapping ranges.
- **SC-002**: The real Core path emits ACK and Selection that the C++ oracle binds to both provider identities and the manifest digest.
- **SC-003**: Both native CPU Providers fetch, assemble and execute their assigned ranges, and one independently validated successful terminal result reaches the requester; classified failure does not meet this criterion.
- **SC-004**: Successful completion leaves no active runner, lease, callback or temporary layer window beyond the declared baseline.
- **SC-005**: Resource evidence includes at least 1-second samples around prepare, ACK, Selection, provider fetch, execution, terminal and drain; a safety stop is deterministic.
- **SC-006**: Two independent runs of the same immutable candidate, each with prepare-once/two-request reuse in one live requester, MUST both pass output, resource and cleanup checks. Failed repeats remain incomplete.

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
