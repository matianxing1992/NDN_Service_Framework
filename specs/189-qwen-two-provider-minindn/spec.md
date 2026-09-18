# Feature Specification: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Feature Branch**: `189-qwen-two-provider-minindn`

**Created**: 2026-09-18

**Status**: IN_PROGRESS

**Input**: 用户要求把最初的本机目标独立出来：用真实 Qwen3-0.6B、两个 CPU Provider 和 MiniNDN 验证完整 prepare/Repo/ACK/Selection/分层执行链，不能用导出、单 Provider 或伪造状态代替。

## Purpose and scope

Spec189 只处理一个可判定目标：在当前 6-core/12-GB 主机上，使用真实 Qwen3-0.6B 和两个 native CPU Provider，完成一次可重复的完整请求，或在第一个明确边界停止并保留可复核证据。它不重做 Spec188 已验证的 bounded prepare/request/YOLO 工作，也不自动构建 SIF、上传 TigerCluster 或运行 QWEN 集群资格。

目标调用链固定为：

```text
Qwen snapshot
  → native prepare: graph/initializer identity + layer split
  → Repo manifest and placement-addressable layer packages
  → payload-free request
  → Core ACK
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

**Independent Test**: C++ `DI_NativeArtifactAuthority`/Repo selector 由真实 Qwen candidate 驱动，检查 manifest、graph identity、initializer digest、两个 layer ranges、commit count 和 source ownership；失败时检查 staging cleanup。

**Acceptance Scenarios**:

1. **Given** a local Qwen snapshot and an immutable source identity, **When** `prepare` runs, **Then** one committed manifest references canonical graph/initializer and exactly two placement-addressable layer packages.
2. **Given** the same identity is prepared again, **When** the second prepare runs, **Then** Repo returns the existing manifest/reference without a second full publication and creates only the request lease.
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

1. **Given** a valid two-provider Selection, **When** one request executes, **Then** both providers fetch only their assigned layer packages, each runner is created once, and a terminal result is returned.
2. **Given** the first provider returns a hidden-state handoff, **When** the second provider receives it, **Then** it validates attempt/model/placement identity before execution and returns a terminal result with a named output digest/top-token oracle.
3. **Given** assembly, ONNX, NDN handoff or CPU execution fails, **When** the request terminates, **Then** the C++ oracle reports the first failure boundary and both providers drain without stale runners.

### User Story 4 - Stay within a measured local resource envelope (Priority: P1)

实验在每个阶段采集 RSS、MemAvailable、swap、disk usage、Repo resident bytes、materialization bytes 和 child process state；资源 guard 在不可安全继续时停止请求并保留 `RESOURCE_BOUNDARY`，不伪造协议失败或 PASS。

**Why this priority**: 当前主机只有 12 GB RAM，必须区分“协议接通”和“内存峰值仍不可运行”。

**Independent Test**: C++ counters are authoritative for Repo/runner ownership; Python only samples host/process metrics and enforces a bounded stop. At least one normal run and one reduced-input boundary are recorded under the same candidate identity.

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
| US1 / FR-001..FR-005 | `Runtime::prepare`, `NativeCanonicalPreparationCatalog`, `NativeCanonicalArtifactPublisher`, Repo owner | one manifest plus two layer references; no payload in handle | `DI_NativeArtifactAuthority`, Repo C++ selector | digest/range mismatch, staging abort, duplicate prepare | asan-ubsan; commit/source-owner counts | `evidence/b189-prepare.md` | B189-1 |
| US2 / FR-006..FR-009 | `DI_NativeRequester`, Core controller/ACK/Selection path | payload-free request and two signed placements | C++ ACK/Selection oracle in `spec189-qwen-two-provider` | no offer, stale epoch, unselected provider, overlap/out-of-range | none; placement and wire invariants | `evidence/b189-placement.md` | B189-2 |
| US3 / FR-010..FR-015 | `di-native-provider`, `DI_NativeOnnxAssemblyWorker`, Core handoff | two assigned fetches, two runners, hidden-state handoff, terminal result | C++ `di-native-provider`/assembly worker plus MiniNDN selector | assembly/ORT failure, handoff mismatch, cancellation, provider stop | asan-ubsan where feasible; runner/lease/drain counts | `evidence/b189-execution.md` | B189-3 |
| US4 / FR-016..FR-019 | experiment runner and native ownership counters | measured peak, safety stop, zero post-drain residue | C++ counters; Python host sampler only | memory floor, swap pressure, disk floor, child leak | resource guard; post-drain zero invariant | `evidence/b189-resource.md` | B189-4 |
| US5 / FR-020..FR-025 | maintained scripts, evidence checker and global dependency preflight | immutable tuple, event sequence, first boundary, verdict and one installed native closure | C++ oracle output plus evidence checker and dependency identity checks | stale identity, timeout, partial logs, missing/ABI-incompatible global dependency | host `/usr` + `/usr/local` closure; no checkout or temporary prefix | `evidence/b189-convergence.md`, `evidence/b189-build-20260918.md` | B189-5 |

## Functional Requirements

- **FR-001**: The candidate MUST use the pinned local Qwen3-0.6B snapshot and record model revision, config digest, tokenizer digest and file hashes.
- **FR-002**: Native `prepare` MUST validate canonical graph identity, initializer digest/size, layer count and exactly two non-overlapping layer ranges before creating a READY reference.
- **FR-003**: Repo publication MUST commit a manifest and placement-addressable layer packages; request-time publication of canonical graph/initializer MUST be zero on a warm prepared handle.
- **FR-004**: The prepared handle MUST contain reference/lease identity only; it MUST NOT retain the complete model payload solely for a future request.
- **FR-005**: Duplicate prepare MUST be idempotent for immutable identity and MUST expose commit/lookup counters to the C++ oracle.
- **FR-006**: The request envelope MUST contain model reference, manifest digest, epoch, input reference and options only; it MUST reject embedded full-model bytes.
- **FR-007**: ACK MUST be emitted by the real Core path and MUST precede Selection and Provider layer fetch.
- **FR-008**: Selection MUST bind two provider identities, layer ranges, manifest digest, authorization epoch and attempt identity.
- **FR-009**: Unselected, stale, overlapping or out-of-range placement MUST fail before Repo layer fetch and runner creation.
- **FR-010**: Provider-0 and Provider-1 MUST fetch only their assigned packages and MUST verify package digest, role, range and manifest identity before ORT load.
- **FR-011**: The native assembly path MUST create runners from the selected layer packages, not from a preloaded whole-model fixture.
- **FR-012**: The two-provider path MUST carry a real hidden-state handoff or an explicitly named production pipeline equivalent between providers.
- **FR-013**: The final response MUST include a terminal status and a C++-computed output digest/top-token oracle for the test input, or a classified execution failure.
- **FR-014**: Cancellation, assembly failure, provider stop and Core close MUST drain Face/io_context/timer/callback dependencies before fixture destruction.
- **FR-015**: Runner, lease and temporary materialization counters MUST return to baseline after terminal response or classified stop.
- **FR-016**: The experiment MUST sample RSS, MemAvailable, swap, disk free, Repo resident bytes and child states at named lifecycle points.
- **FR-017**: A resource guard MUST stop before unsafe host exhaustion and MUST classify the result as `RESOURCE_BOUNDARY` with the first observed metric.
- **FR-018**: A successful run MUST record peak resource values and post-drain values under the same candidate tuple.
- **FR-019**: No run may claim `QWEN_TWO_PROVIDER_PASS` unless prepare, Repo commit, ACK, Selection, both provider executions, terminal response and cleanup are all observed.
- **FR-020**: The candidate tuple MUST include source commit/digest, ABI/library hashes, model files, canonical/initializer/layer digests, profile, selector binary hashes, build tree and run id.
- **FR-021**: The dispatch gate MUST reject stale or mismatched candidate inputs before creating MiniNDN nodes or mutating NDN state.
- **FR-022**: Python MUST only orchestrate external facilities, process lifecycle and host sampling; native behavior assertions MUST be C++.
- **FR-023**: Every failed or stopped run MUST preserve raw logs and a durable evidence record with `static`, `compile/link`, `runtime/test` and `unobserved` miss classes.
- **FR-024**: Spec189 MUST not start SIF/Tiger work automatically; local evidence is a prerequisite for a later immutable delivery candidate.
- **FR-025**: Host native builds MUST consume one installed global dependency closure: Boost 1.71 from the verified system pair and all other NDNSF direct libraries/archives from their declared global roots. Missing or ABI-incompatible dependencies MUST be installed and verified before configuration; checkout, `/tmp`, `.codex-tmp` and per-run dependency prefixes MUST be rejected as build or runtime inputs.

## Success Criteria

- **SC-001**: A fresh candidate produces one verified Repo manifest with exactly two non-overlapping placement ranges and zero request-time full-model publication.
- **SC-002**: The real Core path emits ACK and Selection that the C++ oracle binds to both provider identities and the manifest digest.
- **SC-003**: Both native CPU Providers fetch, assemble and execute their assigned ranges, and one terminal result reaches the requester, or the first boundary is classified with raw evidence.
- **SC-004**: Successful completion leaves no active runner, lease, callback or temporary layer window beyond the declared baseline.
- **SC-005**: Resource evidence includes at least 1-second samples around prepare, ACK, Selection, provider fetch, execution, terminal and drain; a safety stop is deterministic.
- **SC-006**: Replaying the same immutable candidate with a new run id either reproduces the same terminal class or exposes a newly classified boundary without reusing a prior PASS.

## Non-goals and assumptions

- SIF creation, TigerCluster upload, Slurm execution, remote qualification and production deployment are Spec189 non-goals.
- Qwen text-generation quality beyond the named one-input C++ oracle is not claimed; numerical parity is a follow-up after the transport/placement chain closes.
- The existing two-stage split (layers 0–14 and 14–28) is the initial experiment shape; changing the split invalidates the candidate tuple.
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

After protected-grant verification, each Provider must expose the ordered
runtime exits `EXECUTION_ENTERED`, `DEPENDENCY_FETCH`, `ASSEMBLY_STARTED`,
`RUNNER_READY`, `EXECUTION_COMPLETED` and `TERMINAL`. A requester-side
`stream event gap` alone is not a first-failure classification; when the next
Provider exit is absent, the result remains `UNOBSERVED` until a C++ selector or
fresh run identifies that boundary. ACK, Selection and grant verification do
not count as native execution.

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
