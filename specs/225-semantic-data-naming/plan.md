# Implementation Plan: Semantic NDNSF-DI Data Naming

**Status**: IN_PROGRESS
**Progress Timestamp**: 2026-11-20 14:57 -06:00
**Baseline**: `spec253@cd9246fdc18007b32e45f0c742a9ed98f33f8976`
**Design binding**: spec FR-001–FR-009 / SC-001–SC-006；naming N1–N6；integration C1–C6。

## Activation Boundary

用户已激活实现，`.specify/feature.json` 指向 `specs/225-semantic-data-naming`。
上面的 Spec 253 提交是设计基线。实现以 `spec225` 当前源码和各批次记录的构建候选为准，
逐项核对命名 owner、Spec252 endpoint 投影和实际测试闭包。

## Source-Backed Delta

| Existing source at baseline | Gap against this design | Smallest change / task |
| --- | --- | --- |
| `ProducerDataNaming`、`PublishedContentName`、`ProtectedBlobManifest` | 截断标签、多段摘要和单组件旧 artifact coordinate。manifest 按现行 schema 重建名字 | 复用通用组件/digest helper，C1/C2 同步 codec、表示与原包接纳；T001 |
| `NativeCanonicalRolePreparer`、`NativeOnnxAssemblyWorker`、`NativeCanonicalOnnxAssembler` | 源图节点、配方、shard 与 I/O 已绑定，碎片在 Provider 本地生成。旧 N4 的坐标遗漏若干切分方式 | N4/C1 使既有独立制品 API 可表达实际范围。沿原配方辨识本地制品；T001 |
| `NativeCanonicalArtifactPublisher`、`model-object-keys` | 普通材料名不显示实际 bundle 编号，authorization/capsule 分别沿用 model/assignment | 从现有准备和使用链投影语义字段，原子更新发布与读取；T001 |
| `stored-model-capability`、`stored-model-preparation`、`RepoClient` | 包内对象/根和原引用参与 key/AAD，与 journal/index 绑定 | 新发布的 name/seal/read/journal 同改，按既有 exact handle 重开持久原包；T002 |
| `NativeExecutionPlan`、`NativePlanProjectionBuilder`、`NativeExecutionPlanJson`、`NdnsfCollaborationDependencyIo` | 已接线共享 partial、接收端归约及普通/inline-first；现行 wire 缺公开 phase/step/layers，反馈名字使用接收轮次 | C4 复用生产 slot、完整受众和原 IO，新增源侧语义，保留兼容和保护边界；T003 |
| `ServiceUser::makeRequestScopedInputDataName`、`InvocationStream::makeInvocationEventName` | input 和 event 由对应绑定精确重建 | 同步引用/AAD/读取，保留 input versioned prefix 和事件序列；T003 |
| `protected-kv-checkpoint`、`protected-kv-packets` | 先 seal 后构造 packet descriptor | 保留 header AAD，更新表示承诺、引用和恢复；T004 |
| `utils`、`ServiceUser`、`ServiceProvider` | Core 已有调用名，Offer V4 和控制记录嵌入既有载体 | 保留载体，验证冻结原包与终态；有证实缺口由原 owner 修补；T003/T005 |

以上是静态已核实的接线/差异，不是运行失败记录。新 category、metadata 或18组件 parser
是直接套新名称会首先遇到的拒绝边界；实施必须从这些 owner 同步，不在实验脚本绕过。

## Concrete Walkthrough

用模型 `qwen3/0.6b/int4/revision/r3`、User `/lab/user1`、Providers `/lab/provider0`、
`/lab/provider1`，公开 request `request7`、attempt1；标签只是示例，完整 SHA 来自实物。

| Hop | State/input → owner/action → next observation | Existing vs planned |
| --- | --- | --- |
| W01 准备 | 已验证源模型/配方 → canonical publisher 构造 semanticRole/ModelPath/bundle12 → C2 固定 reservation、Name/AAD 后发布原包 | 原链 existing，名称与字段 planned；T001 |
| W01 派生分支 | 同一源图以 PP 整段和 hybrid 加权切分 → role preparer 绑定实际节点、层/入口、rank、KV/FFN 范围 → Provider 按 recipe 组装。若既有 API 发布碎片，则从已验证配方投影 N4 坐标 → C2 的 manifest 绑定准确文件及其源配方 | 本地组装和制品类型 existing，N4 独立发布命名 planned；T001 |
| W02 持久复用 | 准备方存 journal/handle 后退出 → 新 User 用 exact root pin 验证 index/object → 当前 capsule 绑定 admitted Provider/boot/expiry | StoredModel existing，C3 新名字/读回 planned；T002 |
| W03 请求选择 | Core Request/ACK/Selection 不改名 → 当前 User 接纳 Provider 证书/能力，投影 input 与 invocation seed → 两端得到同一 source-side facts | 既有控制链 existing，C4 新字段 planned；T003 |
| W04 密封/预取 | 先算 output facts commitment/`<RepresentationId>`，再密封包含名字的 projection → consumer 提前发 MANIFEST/有界 segments → Producer 无需等待这些 Interest 到达才计算 | exact IO existing，C4 去循环和新语义 planned；T003 |
| W05 输出接纳 | prefill stage0输出 → 原 publishOwnedExactData 冻结普通或 inline-first 包 → stage1验证 plan/Producer/crypto/layout/count 后交 runner | 保留 existing 验证/调度，更新 Name/descriptor；T003 |
| W06 TP 分支 | 此分支增加下游 `/lab/provider2`。`/lab/provider0`、`/lab/provider1` 分别按实际 layer/step/output 发布 partial → 下游按两个源 exact Name 获取，校验各自 source 与 target slot 后本地 SUM。若同个 partial 另有获准读者，共用原包；能力不足沿兼容计划 | Spec252 的规划和 IO existing，N5 表示名 planned；T003 |
| W07 warm/终态 | warm新request/attempt1；decode与早停finalize使用预知continuation名字，实际转换由认证lineage判定 → End/Response/digest一致 → drain cleanup | 原生命周期existing，C4语义投影planned；T003/T005 |
| W08 移交 | 旧 Provider seal KV header → 计算 `<CheckpointPrefix>`/`<RepresentationId>`并存 Repo → 新 Provider 验证原签名/epoch/recipient后恢复 → 新输出使用新 Producer | KV existing，语义字段与 name/reference 同改；T004 |
| W09 失败 | estimate差异只改窗口；超硬界/取消终止 → 取消原 fetch handles，晚到包不推进终态；原包丢失须原恢复流程新接纳，不能同名改 wire | 复用原 cancellation/fencing，C2–C5约束；各任务负例 |

没有新的 readiness Data、ACK/`<RepresentationId>` 发现往返或模型名查询服务。新名字均通过原引用/投影传递。

## T001 Model-Row Migration: Blocking Consumers (measured 2026-11-20)

Migrating the four model rows (`source graph`, `initializers`, `prepared metadata`, root `manifest`) to semantic
names was attempted and measured on `build/unit-tests`, then reverted because three consumers depend on the old
shape and belong in the same unit:

| Case | Boundary |
| --- | --- |
| `LongModelLabelsRetainCompleteSemanticIdentity` | the case asserts that two model identities give different `contentPrefix`; after the migration the stem is identity-free by design and only the representation differs, so the case must compare `objectIdentity()` or the representation tail |
| `OwnerRecoverableReceiptRetainsExactCoreReferencesWithoutOperationalState` | `DI_PROTECTED_PREPARATION_PUBLICATION_REFERENCE_INVALID` — `protected-preparation-index.cpp` validates the recorded `manifestName`/`dataName` positionally, so the index and its durable receipt need the same migration |
| `UsesRealCoreIoAndEncryptedPublicationWithLocalMockKey` | `DI_NATIVE_ENCRYPTED_PUBLICATION_FAILED: cannot create std::vector larger than max_size`, from the Core encrypted-publication path consuming the new name shape |

With the model rows migrated and the other two consumers untouched, the suite went from 3074 passing assertions to
3003/3006, so the migration is not separable: T001 must move the publisher, `protected-preparation-index` and the
Core publication callback together. The material rows were also attempted and reverted for the same reason — their
labels and bundle numbering are a separate sub-unit.

## T002 End-to-End Boundary (measured 2026-11-20)

The journal now allocates and persists an N4 package prefix when it is given the verified facts, but a publication
driven from a hand-built `NativeStoredModelInput` list is rejected with
`DI_STORED_MODEL_REJECTED: invalid capability or object` **even with the legacy prefix and role-shaped ids**
(`prepared-metadata`, `business-root`, `catalog-<64 hex>`). The rejection is shared by every `require()` in the
stored-model path, so the first failing condition still has to be identified, and the supported entry for a real
package is `nativePublishStoredPreparedModel`, which derives the ids, the material manifest and the preflight plan
from an admitted catalogue rather than from a caller-supplied list.

`Runtime` already calls `nativePublishStoredPreparedModel`, which passes the
semantic context before journal allocation. The remaining factory defect is in
that context: every splitter other than `LLAMA` currently maps to `qwen` and
an exception leaves the whole context empty. Derive the family from admitted
model or adapter facts, retain explicit unknown coordinates, and reject invalid
facts before publication. Exercise the factory with an admitted catalogue so
the check covers the real IDs and the encrypted business root as well as the
outer package root.

The explicit-prefix path also has a source-confirmed validation gap.
`nativeStoredModelSemanticPrefix` reads `[-3]` after a two-component minimum
check and recognizes only the `stored/rep/<digest>` tail. The publish entry
uses that predicate to admit an explicit prefix with verified coordinates only
when a journal is configured. The same predicate chooses object names on the
read/write path. Check the full `NDNSF-DI/model/<ModelPath>/stored/rep/<id>`
shape and compare an explicit prefix's ModelPath with the verified facts
before journal allocation, source access or packet publication, including the
no-journal path. Keep the old object naming branch available to recorded
legacy packages. Use the admitted catalogue and a reopened Repo for the
independent package check.

The maintained Repo integration already supplies most of that transport
fixture. `OwnerClientCommitsAndReadsOriginalPacketsThroughAuthenticatedRepoServices`
in `persistent-artifact-api.t.cpp` publishes via
`nativeStoredModelRepositoryIo`, reads as a separate identity, and compares
the root's exact wire from a fresh filesystem Repo. Its package is currently
assembled from a legacy `NativeStoredModelInput` list. Reuse the fixture with
the admitted catalogue entry used by `Runtime.cpp` and check the object reads
from the reopened Repo as well as the root wire.

The configured `nativePublishStoredPreparedModel` path constructs naming facts
before calling the package/journal owner. Its current `LLAMA ? llama : qwen`
mapping mislabels the `YOLO` and `AUTOMATIC` routes already recognized by
`NativeRequestCatalog`. Its catch also leaves every field empty, which selects
the legacy prefix despite the comment promising `unspecified` components.
Use the admitted route and descriptor facts for new packages. For a route
with no verified family, keep the explicit `unspecified` value. Ready legacy
journals must continue to return their original handle without sourcing the
model again.

## T003 Progress (2026-11-20)

Done: `9483a820` carries the five N5 coordinates through the endpoint JSON, the endpoint digest and the object
commitment, and builds the fixed N5 tensor name when a phase is set; `d0a47bb0` copies the coordinates onto the
dependency edge and lets `validateTensorReference` select the legacy or N5 schema from the first suffix component.

The remaining slice is the projection: `NativePlanProjectionBuilder.cpp`'s `endpointBase` lambda (around line 400)
is where a per-endpoint phase, step id, layer interval and port must be filled from the verified plan and step
facts — generation contract for `prefill`/`continuation`, the real step/layers for TP, `execute` plus `none/none`
for a generic DAG, and `role-execution` for a non-step PP. The N5 admission branch then needs one test that drives
an actual V3 fetch/publish with those coordinates, because `validateTensorReference` lives in an anonymous
namespace and cannot be called directly.

## T003 Measured Delta (2026-11-20)

`tensorObjectNamePrefix` currently emits the legacy schema

    <producer>/NDNSF-DI/tensor/REQUEST/<label>/ATTEMPT/<n>/ROUND/<n>/POSITION/<label>/OP/<op>
        /INDEX/<n>/RANK/<n>/BATCH/<n>/<tensor label>/<key digest>

against N5's fixed schema

    <producer>/NDNSF-DI/tensor/request/<RequestId>/attempt/<Attempt>/round/<Round>/phase/<Phase>
        /role/<RoleId>/step/<StepId>/layers/<LayerBegin>/<LayerEnd>/op/<Operation>/rank/<Rank>
        /batch/<Microbatch>/port/<PortId>/rep/<RepresentationId>

So the migration needs, in one unit: the endpoint fields N5 reads (`phase`, `stepId`, layer interval,
`portId`), their projection from the verified adapter plan, the projected/sealed binding, and the
`validateTensorReference` rewrite that stops hard-coding the old 18-component suffix. The `ROUND` value and
the packed `INDEX` already follow the stride rule, but the index must stay bound in step/metadata rather than
appear as a separate name component.

## T003 Sub-Units (planned 2026-11-20)

T003 is not separable into smaller commits that leave the tree green, so it is split into two ordered sub-units.
Both need the projected/sealed binding to carry facts that today live only in the plan.

**U1 — tensor object schema.** `NativeTensorEndpointV3` gains `phase`, `stepId`, `layerBegin`, `layerEnd` and
`portId`; they are projected from the verified adapter plan (PP stage range, TP real step/layers, generic DAG
`execute` + `none/none`, `role-execution` for a non-step PP) and sealed into the endpoint. `tensorObjectNamePrefix`
is rewritten to the N5 fixed schema, `validateTensorReference` stops hard-coding the 18-component suffix, and every
producer of `DependencyEdge` plus the fixtures that assert the shape move in the same commit. Touch points measured
here: `NativeExecutionPlan.{hpp,cpp}` (builder, serializer, digest), `NativeExecutionPlanJson.cpp` (jsonValue/parser),
`NativePlanProjectionBuilder.cpp`, `NativePlanSealer.cpp`, `NdnsfCollaborationDependencyIo.cpp` (validator and the
two admission call sites), plus the tensor/placement suites.

**U2 — request-scoped input name.** `ServiceUser::makeRequestScopedInputDataName` emits
`<user>/NDNSF/DI/REQUEST-INPUT/<service>/<requestId>/<attempt>/PROVIDER/<provider>/v=<attempt>` today; N5 wants
`<User>/NDNSF-DI/input/request/<RequestId>/attempt/<Attempt>/recipient/NameComponent(<Provider>)/rep/<R>/v=0` with
the representation commitment from the publication instance. The caller at `ServiceUser.cpp:10553` is where the
key capsule is chosen, so the seed and protection facts must be threaded there, and the Provider's reconstruction
from the Selection binding must be migrated with it.

## U2 Input-Name Design Constraint (measured 2026-11-20)

The request-scoped input name is built twice from the same facts: `ServiceUser::makeRequestScopedInputDataName`
before encryption, and `ServiceProvider.cpp` while validating the Selection binding. N5 wants

    <User>/NDNSF-DI/input/request/<RequestId>/attempt/<Attempt>/recipient/NameComponent(<Provider>)/rep/<R>/v=0

Two constraints decide what `<R>` may contain:

- **No plaintext digest.** The Provider rebuilds the name before it fetches or decrypts, so a commitment over the
  plaintext would make the name unreconstructable. N3 already places an input's content digest in the protected
  message/manifest, not in `<R>`.
- **No self-reference.** `RequestSecurityBinding::inputDataName` is itself part of the binding and of the AEAD AAD,
  so `<R>` must not include it.

The facts both sides hold before publication are therefore: user identity, service name, request id, attempt,
controller version, the complete Provider name, the user and Provider encryption certificate names and digests,
and the selection digest. `<R>` is a tagged digest over exactly those, and the migration must move both builders
together while the legacy shape stays readable for a Selection issued before this row.

## T004 Remaining Boundary (measured 2026-11-20)

Naming, the receipt projection and the grant/restore wiring are in place and the whole KV family passes
(`Spec221KvSave` 157, `Spec221KvRestore` 73, `Spec221KvStateLifecycle` 232, `Spec221PortableKv` 133,
`Spec221PrivateKvCheckpoint` 236, `Spec221ProtectedKvCheckpoint` 704, `Spec221ProtectedKvPackets` 174,
`Spec221RepoUserStorageJournal` 444, `Spec221RepoUserStorageWire` 84).

What is still missing is a fixture that drives the publication owner itself. `publishProtectedKvCheckpoint` is
installed by `nativeKvPublicationRepo(runtime, producer)` and captures a real
`ndn_service_framework::ServiceProvider` for `signProducerDataUntil`, so the end-to-end case needs a provider
fixture with a durable storage owner: a receipt with the N5 coordinates, a Repo target, a grant resolver that
returns a matching grant, and the `onGrantRequired`/`onDurablePacketSetBoundary` callbacks. The smaller open items are
the capsule object reference (it must point at the granted `<CheckpointPrefix>`) and recomputing the prefix on
restore from the saved root plus signing certificate instead of trusting the saved root's shape.

The existing `OwnerClientCommitsAndReadsOriginalPacketsThroughAuthenticatedRepoServices`
fixture already provides the real Source signing bridge, authenticated Owner
storage, persistent Repo and a fresh store for original-wire comparison. Its
KV branch currently calls packet preparation, materialization and Owner-key
recovery directly. Use its actors and storage to drive
`bindNativeProviderDurableKvPublisher` and `nativeRestoreKvForInvocation` for
the remaining B4 check. The acceptance path must consume the durable Saved
record, issue the target recipient capsule, reopen Repo, and read the restored
tensor through the target Provider. The existing direct Owner recovery remains
evidence for the lower-level packet and ciphertext boundaries.

## Whole-Profile Evidence (2026-11-20)

With T001–T004's code in place, the default `unit` profile on the maintained `build/` tree reports

    status PASSED, discovered 2903, selected 2831, executed 2831, failed 0, unavailable 0

covering the tensor, naming, canonical-publication, stored-model, KV and request-scoped suites together. One earlier
run of the same profile aborted after ten cases with `SIGINT`-visible kernel identity failure
(`NDNSF_IDENTITY_CHECK_FAILED: Supplied TPM locator ... does not match TPM locator in PIB`), and an immediate rerun
passed cleanly, so that abort is a fixture-level flake in the Spec217 identity helper rather than a naming
regression; it is recorded here rather than silently dropped.

## Model-Profile Run (2026-11-20)

The tracked recipe's asset id matches the SEALED package already on this host
(`/var/tmp/ndnsf-spec251-qwen-assets`, recipe `qwen3-0.6b-int4.json`), so the `model` profile can run with
`--asset-manifest`. Result: **52 selected, 52 executed, 2 failed, 0 unavailable**, i.e. fifty of the fifty-two
asset-backed cases pass with the migrated names.

| Case | Failure | Attribution |
| --- | --- | --- |
| `TensorParallel/PlanAndPlacement/ProviderAcceptsATensorParallelPreparedSpec` | `DI_PROVIDER_ASSEMBLY_STEP_CONTRACT_MISMATCH` | **pre-existing**: reproduced identically with the worktree checked out at the base commit `1d8434a6` |
| `TensorParallel/PlanAndPlacement/TwoWorkersRunAllLayersAndSampleAToken` | `worstExcess <= 0` fails by 2.5406208e-05 and 7.2917119e-07 | **pre-existing**: the same two values reproduce at `1d8434a6` |
| `FrozenShardedCatalogLoadPreparesTwoRankRoles` | reported as assertion-free | the case returns before asserting in this environment; recorded, not counted as a pass |

The A/B was run by checking the worktree out at `1d8434a6`, rebuilding `unit-tests` (13m45s) and running the two
cases with the same asset environment, then restoring this branch and rebuilding (11m10s); the restored tree passes
`PublishedContentNaming` 191, `Spec182V3Placement` 3377 and `Spec221ProtectedKvPackets` 174.

## Cross-Shape Reuse: Key vs Comparison (measured 2026-11-20)

The durable hit path now compares verified facts instead of name bytes (`43d95e18`), but a cross-shape hit still
cannot happen: `durablePublicationBase` is a digest over `options.publicationIdentity`, `contentName.objectIdentity()`
and the manifest suffix (`ServiceUser.cpp:7843`), and `objectIdentity()` changes when coordinates are added. A record
published before the coordinate row therefore lives under a different key and is never found, so changing the
comparison alone has no observable effect across shapes.

Reusing such a record needs the durable store to carry a shape-independent semantic key (publication identity +
shared business facts, without coordinates or the adapter alias) as a second index entry, with the hit resolved by
that key and the verified-fact comparison deciding reuse. That is an index change on the Core durable store, not a
comparison change, and it is the remaining half of audit item 3(a).

## T003 Phase: Closed by Removing the State (2026-11-20)

The gap is closed in `791e5c5b` by deleting the stored `namingPhase` field rather than by deciding it somewhere.
`round` is the packed operation index and the epoch is `round / stride`, so the phase is computed where the name is
built (`tensorObjectNamePrefix`) and where an admitted reference is checked (`validateTensorReference`): a generation
plan's first epoch is `prefill`, every later epoch is `continuation`, and a plan without an operation stride is
`execute`. Nothing stores it, so a cached projection and a freshly derived one cannot disagree — the failure mode
that blocked the three earlier attempts. The N5 selector became `namingStepId`, which every N5 endpoint carries.

`Spec182V3Placement` now asserts, per epoch, that every published output actually names `phase/continuation` for
epochs beyond the first (3410 assertions). That assertion is what exposed the earlier attempt as inert.

## T003 Phase Derivation: Attempted and Reverted (2026-11-20)

The audit's item-2 gap — the naming phase is decided when the base endpoint is built, so later epochs of a
generation keep `prefill` — was attempted twice and reverted in `bf592b51`:

- Deriving the phase from the raw round inside `deriveGenerationEndpoint` fails the plan validator, which requires
  `deriveGenerationEndpoint(endpoint, range, 0)` to reproduce the base digest and manifest byte for byte (the base
  is already the epoch-0 endpoint).
- Deriving it from `round / stride` satisfies that check, but a fresh projection and a cached or rebound one then
  disagree on 217 endpoint digests, because the cache stores derived endpoints while the fresh path re-derives them.

A third attempt (`f636b305`) decides the phase in `applyNamingCoordinates` from `round / stride` and passes the
placement suite, but a mutation probe — forcing every generated endpoint to `prefill` — also passes 3377, so that
change is not exercised by any assertion. Adding the missing assertion (every epoch>0 output must name
`phase/continuation`) then exposed the real state: production still emits `prefill` for later epochs, because the
epoch expansion happens through `deriveGenerationEndpoint`, which copies the base endpoint.

Changing the phase inside that helper for `sequence != 0` satisfies the base no-op requirement but再次 breaks the
placement suite on 217 endpoints: the fresh and the cached or rebound projection disagree on `plannedDataName`,
`endpointDigest` and `executionObjectDigest`. The epoch-expansion path and the cached layout therefore have to be
reconciled together — the cache stores endpoints that a later step re-derives — which is a projection-owner change
with its own test, not a one-line phase decision.

The tree is back to `f636b305` with `Spec182V3Placement` at 3377 passing assertions and no pending changes; the phase
gap remains open and is listed in `tasks.md`.

## Audit AUD-72c8ff5b Findings (2026-11-20)

Accepted and still open after `7ba91851`:

1. The tensor representation now binds the framing facts (segment count, bundle selection, inline rule), but the
   accepted protection scope beyond the profile — capability and epoch-key identity — is still absent. The plan-derived
   group identity must stay out, because reusing it would make the name depend on the plan digest again.
2. The durable cross-shape reuse chain has a second break beyond the lookup key: the ordinary and rebound paths match
   the current semantic stem against the recorded `/PROTECTED/.../DATA` before the manifest is read, so
   `DURABLE_LOOKUP_NAME_MISMATCH` fires before `sameSemanticFacts` can decide. Old-candidate location, per-shape
   verification and returning the original reference belong in one batch.
3. The public logical request id is still per-attempt, and the label helper truncates silently at 64 bytes.
4. The feedback producer publishes the `e+1` projection, so the first packet currently names `round/1` with
   `phase/continuation` where the contract wants `round/0` with `prefill`; both sides agree, which is exactly why the
   suite cannot see it. The projection also copies the role layer interval into a step, where C4 wants the verified
   step interval.

## Cross-Shape Fallback Attempted and Reverted (2026-11-20)

A candidate-enumeration fallback was implemented and reverted: `EncryptedLargeDataRangeStore` gained an optional
`listDurableCandidates`, `RepoEncryptedLargeDataStore` implemented it from `m_repo->list()`, and `ServiceUser` used it
on a primary miss, filtering candidates by payload digest, plaintext size and a shape-agnostic protected prefix.

The new integration case did show a legacy-shaped record being reused by a semantic request, but the fallback is not
admissible: `PersistentWorkerPublicationRetainsAndServesEveryDurableObject` publishes 672 objects with the **same**
payload, and the filter collapsed them to one record (`publicationCount 1 != 672`). Owner-recoverable paths also hit
`DURABLE_MANIFEST_METADATA_MISMATCH`. Payload identity is therefore not evidence that a record describes the same
object: without a stored semantic key, a candidate scan cannot distinguish two objects that happen to share content.

The correct route is the one the audit names: a shape-independent semantic key written into the durable record (a
record-format addition with its own migration), used for lookup, after which the existing fact comparison decides
reuse. The experimental code is reverted; `NamingOriginalPacketPublication` is back to 1900 passing assertions.

## Cross-Shape Lookup: What Landed (2026-11-20)

`c576faf8` resolves the lookup half without a record-format change. A durable record published before the coordinate
row is stored under the identity its coordinate-free description produced, so `publishEncryptedLargeData` now derives
that identity too — the same formula over a copy of the content name with the coordinates cleared — and tries it when
the primary lookup misses. The hit is then addressed and verified under the identity the record actually has, keeps
its recorded prefix and manifest, and the restored manifest decides reuse through `sameSemanticFacts`.

Two candidate designs were rejected on evidence: a payload-identity scan collapsed the 672 same-payload objects of
`PersistentWorkerPublicationRetainsAndServesEveryDurableObject` into one record, and a scan-based "any protected
prefix" match also let owner-recoverable paths reach `DURABLE_MANIFEST_METADATA_MISMATCH`.

The remaining gap is narrow and explicit: a record whose facts were only expressible *without* coordinates (a material
row carrying model facts, for instance) has no coordinate-free counterpart to reconstruct, so recognising it needs the
stored semantic key the audit names. Every other documented case — a model, weights or artifact row — is now found.

## Owner-Recoverable Cross-Shape Reuse: Not Landed (2026-11-20)

The coordinate-free fallback wraps its key in the owner-recoverable domain, as the primary key does. That wrap was
implemented and then reverted: a mutation probe that skipped the wrap left every assertion passing, so no test covers
it. The only boundary that exercises an owner-recoverable durable publication needs a ready NAC producer, a worker
thread and a real Repo, which is not a cheap fixture, and adding unverified code is worse than leaving the gap named.

At the end of that attempt, an owner-recoverable record was not cross-shape reusable. The audit identified three reasons: the primary
key carries the owner domain while a reconstructed legacy key does not (and, per the probe, nothing would catch the
omission); a canonical or material receipt may have had a non-empty adapter alias that the current empty suffix cannot
reproduce; and a material row whose model facts are only legal with coordinates cannot be rebuilt from its
coordinate-free description at all. Recognising such a record needs the stored semantic key — a durable record-format
addition with a migration — which is the honest remaining work on T001.

### Owner domain verified with a real durable publication

The current candidate applies the same owner-domain digest to the primary key
and the reconstructed legacy key. The existing real NAC, worker and filesystem
Repo fixture publishes a coordinate-free model, republishes it as a source
graph with coordinates, and requires the original publication identity and
manifest name and an unchanged Repo record count. The focused case passed all
126 assertions against the candidate library in `build/`, with isolated
`HOME` and `TMPDIR`. Reproduce with `bash scripts/build-safe.sh build
--targets=naming-original-packet-publication`, then run
`build/naming-original-packet-publication
--run_test=NamingOriginalPacketPublication/OwnerRecoverablePublicationRewrapsAfterUserReconstruction`
with the candidate build dir on `LD_LIBRARY_PATH`.
The full `NamingOriginalPacketPublication` suite then passed 18 cases and
1,909 assertions with isolated `HOME` and `TMPDIR`, including retention of all
672 distinct publications with identical payloads. An earlier overlapping run
shared a PID-based temporary path across isolated command processes and failed
at the fixture's Repo directory. The independent temporary roots eliminated
that collision on rerun.

The remaining alias lookup has a concrete historical source. The publisher
immediately before `3101a36d` derived the canonical root alias from the
verified model name and digest. The publisher before `afe07599` also used
different material labels and represented the receipt as `Material`. The DI
durable owner seed includes service, catalog, exact application label, payload
digest and byte count in both historical and current code. Check whether each
historical row can be reconstructed from its verified publication inputs and
seek that exact old key before considering any record-format change. For rows
with different business fields, require the applicable model and recipe facts
before accepting the old record. A match on payload alone is insufficient.

### Historical canonical root alias in the next candidate

The DI durable publisher now derives the historical canonical root suffix from
its root JSON and verified model identity and passes it to Core as a read-only
lookup input. Core still tries the current key first. Its fallback derives the
coordinate-free key with the exact legacy suffix, checks that suffix in the
original manifest, and leaves the current publication suffix empty. The
owner-recoverable regression checks the original manifest wire as well as the
publication ID and Repo record count. The alias candidate passed the Core
original-packet suite (18 cases, 1,911 assertions) and the DI canonical-publisher
suite (50 cases, 3,077 assertions), both linked to the candidate in `build/`.

The historical material descriptor before `afe07599` used `Material` for all
three rows and constructed each label from the model name. The application
label handed to `durablePublicationIdentity` stayed unchanged, including the
exact pack number for bundles. For the catalog path the seed commits the
catalog, label, verified payload digest and byte count. The Owner key capsule
also binds the resulting publication identity, original manifest and service.
Use those bindings together with the exact old descriptor and suffix. With
the direct publisher's source-digest-only configuration, verify an independent
catalog or receipt binding before treating a historical material record as the
same model and recipe.

When a service-shared legacy hit cannot be reauthorized, the existing rebind
path looks up a version-derived identity from the current key. On that path
the suffix and descriptor must be checked against the current shape again.

The following candidate passes the historical descriptor and alias together
from DI to Core. DI reconstructs the three historical material rows on its
catalog-bound path. Core admits an explicit historical descriptor only for
owner-recoverable durable publication, checks the exact historical shape on
fallback, and clears that expectation when rebinding to a version-derived
current identity. The real Repo fixture exercises a historical material
receipt whose current kind and label both differ, then changes the catalog
binding with the same payload and requires a separate record. The combined
candidate built successfully. `NamingOriginalPacketPublication` passed 18
cases and 1,920 assertions, including 137 in the real owner-recoverable case.
`Spec182CanonicalPublisher` passed 50 cases and 3,077 assertions. Both used
isolated `HOME` and `TMPDIR` and the candidate libraries in `build/`.
The remaining B1 acceptance includes the native Provider fetch/admission and
the bounded sanitizer checks in C6.

The broader B1 run passed static publication (38 cases, 10,477 assertions),
encrypted Repo (16 cases, 444 assertions) and transparent Repo (106 cases,
3,175 assertions). `naming-codec` failed the pre-`e4ad5093` assertion for a
63-byte prefix followed by a multibyte UTF-8 character. The updated oracle
uses the independently calculated digest of the full label and still checks
that the visible prefix ends at a UTF-8 boundary. The source review also found
that the artifact manifest stem was appending the file label and the object
identity omitted artifact entry, partition and manifest page number. The
candidate fixes both and rejects oversized public labels at the N4 boundary.
The refreshed candidate is building for the native codec, original packet,
model contract and Provider fetch tests.

The refreshed native runs passed `naming-codec` (9 cases, 121 assertions),
`naming-static-publication` (38 cases, 10,483 assertions) and the canonical
publisher (50 cases, 3,077 assertions). Three generated/owned model partition
cases for Qwen, hybrid boundary rejection and YOLO passed (276 assertions).
The Provider test reached the real capsule publisher and exposed the next
boundary: the producer fed the 71-byte `sha256:<hex>` diagnostic digest into
the 64-byte `UseId` label. The publisher now emits the full 64 hex digits as
intended. The subsequent fixed-source test in the same process terminated
during fixture setup after the capsule exception. Rerun these Provider cases
individually against the corrected candidate before attributing that secondary
crash. No Provider acceptance claim is made from this run.

With the 64-digit UseId, the standalone fixed-source cold/warm case passes
147 assertions and the canonical assembly fetch case passes 26. The stored
capsule case advances beyond publication but fails before any capsule or
stored model payload fetch, at admission of the outer external Selection
assignment. The requester uses a label containing Provider, role and payload
commitment. The Provider classifier still caps the rendered component at 69
bytes and cannot recognize the bounded digest suffix for a long label. The
manifest classifiers have the same problem when they reconstruct from the
already shortened component. Separate component syntax recognition at the
early classification boundary from full-label verification against the
decoded signed manifest. Preserve the current Producer, shape, digest and
authorization checks. The bounded DI sanitizer build is running separately.

The next candidate shares one naming-component syntax check across Selection
admission and manifest classification. It recognizes the bounded UTF-8
prefix and lower-hex suffix, while manifest validation still compares the
component against the full recorded label. Capsule admission also recomputes
the model path and UseId against the admitted projection before any table
fetch. The Provider fixture now includes wrong-model and wrong-use Names and
checks that neither fetches a table or stored model payload. These changes
still require a refreshed native build and the isolated Provider replay.
The two-job diagnostic build was interrupted through its own launcher after
the source changed. It produced no sanitizer test result. The queued ordinary
build acquired the project lock and is compiling the complete Provider fix
with the project's default six jobs. Refresh the diagnostic candidate after
the ordinary Provider regression so it investigates the same source.

The Python publication entry previously accepted only the legacy metadata
dictionary. It now also accepts the C++ `PublishedContentName` and partition
types through pybind. The wrapper forwards the native metadata to the existing
Core publication method. The binding fixture compares actual metadata and
Name bytes with manually assembled TLV and checks that the publication entry
admits those coordinates and rejects an oversized one. Python syntax checks
pass. The `naming-python-binding` build and runtime fixture remain pending.

The long-label candidate built and passed the naming codec (9 cases, 125
assertions), static publication (38 cases, 10,483 assertions), original
packet publication (18 cases, 1,920 assertions), canonical publisher (50
cases, 3,077 assertions) and two Provider fetch/assembly cases (173
assertions). The authenticated stored capsule case progressed to table
Interests. The old plaintext verifier still expected a digest at `DATA[-4]`,
which is absent from the new `/rep/<R>/DATA` form. The next candidate follows
the deterministic exact signed `MANIFEST` for semantic DATA references and
checks the authenticated plaintext digest and size from that record. The
Provider mutations for wrong model and UseId now get their own current grants
so they reach capsule admission. In the Python build the installed ndn-cxx
header exposed `Block::data()`, so the binding now uses that public accessor.
The combined build is running. Capsule and binding runtime checks remain open.

The C2 transcript still needs an independent acceptance check. The producer
currently hashes the raw publication nonce into the representation identity.
The signed manifest records the nonce commitment in the protection descriptor,
and the envelope carries the nonce itself. The manifest validator checks the
stem and representation-tail shape. The complete reader chain needs to
recompute the same transcript using the admitted nonce and descriptor, and
the signer context in tag 7 must come from the authorized signer used for
publication. Keep this B1 requirement open while testing the new exact
manifest fetch and plaintext check.

The first combined build found two existing direct decrypt callers that pass
an optional plaintext length. The reader now keeps that overload and adds a
signed-manifest overload for the semantic path. Both use the same decryption
implementation. The rebuilt batch again includes Provider, original packets,
static publication and Python naming. Runtime verdicts remain pending.

The rebuilt batch completed. The Core library exports both decrypt overloads.
The real stored capsule Provider case passes all 323 assertions, including the
wrong-model and wrong-use cases before table I/O, the wrong-recipient case
before model payload reads, and the accepted stored payload path. Static
publication passes 38 cases and 10,483 assertions. The actual Waf-built Python
extension passes five source-binding tests, including byte-for-byte metadata
and Name oracles. The original-packet suite passes all 18 cases and 1,920
assertions against the same candidate. C2 transcript rederivation and the
bounded sanitizer gate remain open.

The C2 candidate now builds and verifies its representation Name through one
transcript implementation. Semantic manifests carry the preallocated public
nonce and signing certificate Name. The decoder derives the complete DATA
prefix from the admitted Producer, object identity, plaintext digest,
reservation, wire and protection descriptors, and signer context. Core
compares the certificate's key with the validated packet signer and the
reservation nonce with the envelope nonce. The fixture includes an independent
fixed digest vector and negatives for changing nonce, signer and content
identity while retaining the earlier Name. The combined native build is
completed. The updated naming codec passes 9 cases and 125 assertions.
Static publication passes 38 cases and 10,488 assertions, including the new
fixed vector and descriptor negatives. The stored capsule Provider case
passes 323 assertions, the DI canonical publisher passes 50 cases and 3,077
assertions, and all five Python binding tests pass. The original-packet suite
passes 18 cases and 1,920 assertions. The six-job diagnostic build is running the maintained
`model-contracts-asan` and `uav-large-fetch-lifetime-asan` targets.
Those targets instrument DI and the Provider fetch owner. The focused
`naming-static-publication-asan` target also compiles the changed Core naming,
manifest and hybrid crypto sources with ASan/UBSan and reuses the existing
codec and publication fixtures. Its build is queued behind the ordinary replay.

The alternate immutable input path uses `dependency-transfer.cpp` to verify
the fetched originals before passing their assembled envelope to the decrypt
owner. It now checks every segment's signer against the admitted manifest,
as the ordinary Provider fetch does. The maintained `input-admission` case for
an authorized unselected Producer now publishes the semantic manifest form
and exercises this boundary. The refreshed ordinary Waf build finished in
48.7 seconds. Three input admission cases pass 28 assertions, the real
stored capsule Provider case passes 341 assertions including expiration
before table or model payload I/O, and all 18 original-packet cases pass
1,920 assertions. The previous diagnostic run has no terminal result. Its
replacement reuses the same build tree and GCC 9.4 toolchain with `-O0 -g1`
and six jobs. The diagnostic targets retain ASan/UBSan instrumentation.

The ordinary C2 candidate also passes the fixed-role Provider handler restart
with a current grant (181 assertions) and rejects substitution of the Provider
epoch or grant at Selection binding (6 assertions). Neither case exercises
the updated immutable input prefetch path.

The existing native planning and partition fixtures pass four selected cases
with 68 assertions. They check both weighted shard layouts, rejection of an
incomplete hybrid candidate and the actual YOLO ONNX partition input. The
independent naming codec covers the profile component at identical degree and
rank. These are local assembly and API checks for the API-only N4 artifact
forms, which have no network publisher in the current source.

## Stored Semantic Key: Designed, Not Landed (2026-11-20)

A V4 form of the Core durable key-reference record was implemented and reverted before any test could drive it. The
field would have carried a shape-independent key so a record could be found without reconstructing either name shape.
Two facts decided the revert:

- Every record written by the current code carries coordinates, and every request for it carries coordinates, so the
  primary key already matches; no second shape exists to exercise the stored key, and a change no test can drive is
  speculative.
- A key over *business facts only* — the only form that can match a coordinate-free record — cannot distinguish two
  objects that differ solely by coordinates, which is exactly the collision that collapsed the 672 same-payload
  objects when candidate scanning was tried.

So the durable-format change stays out until a shape transition exists that it must bridge. The coordinate-free
lookup in `publishEncryptedLargeData` remains the supported path. The
catalog-bound Owner regression above covers the explicit historical alias and
material descriptor without adding a new durable record format.

## Feedback Round Off-By-One: Why It Is Not a Local Fix (2026-11-20)

The audit is right that a generation's first feedback packet is named `round/1` with `phase/continuation` where N5
wants the producing epoch (`round/0`, `prefill`). The code states why the label currently follows the consumer:
`NativeEpochCoordinator` takes the `epoch + 1` projection deliberately, so the feedback's DATA_V1 operation index and
exact name equal the input edge the upstream role will fetch on its next epoch.

Changing only the name would make the producer publish a name the peer never fetches; changing only the fetch would
break the sealed edge the peer already admitted. The two sides must move together: the published name should carry the
producing epoch, and the consumer's next-epoch fetch projection must be derived with the same producing epoch for that
edge. That is a coordinated change across the coordinator, the projection builder and the admission check, and it needs
a streaming test that inspects the first feedback packet's name on both sides — not a one-line round adjustment.

## Work Units and Batch Allocation

每项一个有独立行为出口的实现/验证单元，完成原生验证后立即按 AGENTS 提交该项；不
把编码、测试注册和该项文档同步拆成行政任务。不把未来激活或本轮文档审查算成任务 DONE。

| Batch / task | Common entry and closure | Independent exit / oracle |
| --- | --- | --- |
| B1 / T001 | Core codec/保护publisher→manifest→canonical assembler/capsule install；Core+DI | 手工TLV与真实发布/验证/解密一起闭合，旧canonical原包仍可复用 |
| B2 / T002 | StoredModel factory/context→journal/seal/index/read；现有Repo API | prefix前元数据已到位，新旧ready/partial包重开可读，错pin/key拒绝 |
| B3 / T003 | Catalog/fixed tuple→projection/edge→exact IO→runner，input/event/attempt | 提前Name与真实包一致；早停止、feedback epoch、两种framing和取消/错误binding生产断言 |
| B4 / T004 | KV seal→certificate-aware packet root→grant/Repo restore | byte/packet identity及两张证书不混同，恢复与错epoch/recipient拒绝 |
| B5 / T005 | 安装部署和native oracle；T001–T004冻结组合候选 | 真正两Provider CPU cold/warm、原包复用和cleanup；报告实际边界 |

T003 依赖 T001，不依赖 StoredModel 完成后才能做 local canonical 路径；T002 与
T003 不互借未完成验收。T004 依赖 T001/T003 的公共执行坐标。默认按表顺序推进，
dependencies 只表达真实硬前置。每项测试闭包在其出口验证，不把验收欠账留给 T005。

T005 的实机入口使用 `Experiments/profiles/README.md` 记录的安装版 `ndnsf-di cluster`
与 Qwen 原生 verifier。当前主机能发现 `minindn` Python 模块。仓库的
`qwen06b-tp-cpu.json` 固定三轮 24/24/24 和 56/112/168，它对应另一项验收。
Spec 225 的部署配置须固定两轮 25/24，首轮生成 32，第二轮从 context 81
生成 32，并以相同的模型、tokenizer 和 BOS 规则生成独立 oracle。
使用安装版 `multiturn-oracle --deployment-state <state.json> --model-reference
--require-exact-conversation 32 2` 核对该候选。此模式要求 MiniNDN、显式放置、同一
连续会话及每轮 32 个实际 token。两个 Provider 的 `actor.parameters` 均需启用
`capture-complete-kv-state: true`，生成各自统计路径对应的 `.kv-complete`，供独立未切分
ONNX 参考比较；普通 `--model-reference` 不会加载完整 KV 快照。另核对配置和回执恰好
对应 Spec 225 的两轮输入，记录捕获开启条件下的耗时。

两轮输入已用本机 native tokenizer 和资产配方固定的 tokenizer SHA 重新生成，并与
历史 G2 输入逐字节一致。问题依次为 “Explain how Named Data Networking can support
distributed neural network inference.” 和 “Give a concrete example and explain the
main tradeoff.”。采用 `qwen06b-tp-cpu.json` 的 user/assistant 模板，关闭自动 special
tokens，首轮无前缀，次轮只加 `\n`。默认三轮 profile 的 `<|im_end|>\n` 会给次轮多加
token 151645。两份 tensor bundle 的 SHA-256 分别为
`278f9eeedf242fc047635699df132dcd89ec488584cce3038de3553d7526e310` 和
`48fbc9e02a20e9709f94d393037373dcd8ba80815cd02aab85a2c4a93dca7e34`。

## Requirements and Qualification Mapping

| Requirements | Contracts / tasks | Final observations |
| --- | --- | --- |
| FR-001/002/003；SC-001/002 | N1/N2/N4–N6，C1；T001–T004 | 目录每行的来源分类、字段/Producer/组件wire，绑定一致性 |
| FR-004/005；SC-003/004 | N3/N5，C2/C4/C5；T001/T003/T004 | 名字预计算、无循环、actual vs estimate、signature/AEAD与终态负例 |
| FR-006；SC-005 | N4，C3/C5；T002/T004 | 原包持久重开读、离线准备方、当前授权、KV原Producer |
| FR-007/008；SC-003/006 | C1–C6；T001–T005 | shared owner接线、原Core名字、PP及已有TP负例、drain |
| FR-009；SC-006 | C6；T005 | code-aware组合审查、安装后请求/独立oracle与具体耗时证据 |

## Validation Boundaries

执行前冻结每项实际 source/config/target 输出路径和 ABI 闭包；Waf正常安装，MiniNDN
消费已安装程序。source seal/manifest 引用变更使关联旧证据失效；没有行为变化且依赖
闭包相同的已有检查不重跑。正式安装验收前审查 writer→reference→reader 与所有负例。

动态分析只覆盖改动的 native codec/IO/取消边界，沿 C6 复用有界 ASan/UBSan 卡；纯文档
不运行。GPU/27B/性能 campaign、通用版本框架、session优化及未完成 TP 功能不属本 Spec。
不设没有配对基线支撑的提速百分比；记录实测包数/Name开销，不能把缩短Name直接写成提速。
