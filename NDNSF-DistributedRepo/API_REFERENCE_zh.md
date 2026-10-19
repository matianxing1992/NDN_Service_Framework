# DistributedRepo API 参考（当前 Repo 工作树）

本文按 2026-10-10 的 `Repo` 工作树整理：记录基线为 `HEAD 2be0e98b`，Repo 源码对应 Spec202 候选 `aeb815ea2b362b857c398d910bb1464426e580b1121eee241b2206002d1c26e2`（688 个候选输入）。本文是接口使用说明，不表示 Spec202 的 T007/T009/T010 或网络资格已完成。精确 C++ 类型、参数与默认值以所链接的头文件为准。

## API 分层

| 场景 | 首选入口 | 说明 |
| --- | --- | --- |
| 不经过网络，在一个 durable root 发布、读取 artifact | `ArtifactRepositoryApi::makeInApp` | 本地 artifact store；适合打包、缓存和本地阶段。 |
| 通过 NDNSF ServiceUser 访问一个或多个 Repo Provider | `ArtifactRepositoryApi::makePersistent` | artifact 级 API；持久化发布、选定 Provider、回执、位置和可用性。 |
| 操作 RepoNode 上的传统命名对象、分段对象或签名 Data wire | `RepoClient` | C++ 对象/packet API；与 artifact handle API 是不同的数据平面。 |
| 提供 Repo 服务或嵌入进程内服务 | `RepoNode` | 注册网络服务、注册 LocalServiceRegistry、实现本地对象处理。 |
| 读写存储事务、实现后端、适配 DI 内容源 | `RepoCore`、backend 与 source-provider 头文件 | 高级组件接口；普通应用优先使用上面的 API。 |

`ArtifactReference` 标识不可变 artifact 内容（名称、摘要、字节数、格式、root manifest、发布者和 policy epoch）。`ArtifactHandle` 携带已发布位置与提交回执，供持久化读取、receipt 查询和可用性探测。`RepoObjectManifest` 则描述对象名、对象摘要、分段、复制和生命周期信息。两套类型不可互换。

## ArtifactRepositoryApi：artifact 发布与取回

完整声明见 [`ArtifactRepositoryApi.hpp`](include/ndnsf-distributed-repo/ArtifactRepositoryApi.hpp) 和 [`ArtifactTypes.hpp`](include/ndnsf-distributed-repo/ArtifactTypes.hpp)。C++ 方法的 Python 投影使用 snake_case，见 [`artifact_api.py`](pythonWrapper/py_repoclient/artifact_api.py) 与 pybind 实现。

| C++ API | 用途与用例 |
| --- | --- |
| `makeInApp(InAppRepoConfig)` | 创建本地 durable artifact store；配置 `durableRoot`、非空 `ownerId`、`limits` 和 availability freshness。 |
| `makePersistent(ServiceUser&, PersistentRepoClientConfig)` | 创建网络客户端；配置 `servicePrefix`、可选 `providerScope`、ACK 收集时间、请求/操作等待超时、fetch 失败冷却和健康项上限。 |
| `publishFile(path, reference)` | In-App 发布文件并返回 `ArtifactPublishResult`；持久化发布使用下一重载。 |
| `publishFile(path, reference, selected, timeoutMs)` | 将文件发布到应用明确选择的目标 Repo；结果包含 replica 状态、receipt/handle 和耗时。 |
| `beginPublishFile(path, reference, selected, idempotencyKey, timeoutMs)` | 以幂等键启动可查询的持久化发布；返回 `PublishOperationHandle`。 |
| `getPublishStatus(handle, timeoutMs=0)` | 按操作 handle 读取状态、目标尝试和 receipt 信息。 |
| `discoverRepoCandidates(requirements, timeoutMs)` | 询问满足 `ArtifactCapabilityRequirements` 的 Repo；返回候选和观察时间。候选发现不会替应用决定写入目标。 |
| `archive(inAppSource, handle, selected, timeoutMs)` | 将 In-App 已有 artifact 归档到应用选择的持久化目标。 |
| `beginArchive(inAppSource, handle, selected, idempotencyKey, timeoutMs)` | 幂等启动 archive 并返回可查询的操作 handle。 |
| `fetchFile(reference, destination)` | 从 In-App store 按 reference 取回；适用于本地阶段。 |
| `fetchFile(handle, destination, timeoutMs)` | 使用持久化 handle 从当前 Repo 位置取回；记录每次 attempt、所选身份、字节数和耗时。 |
| `lookupLocalCommit(reference)` | 检查该 In-App store 是否已有本地 commit，返回可选 `ArtifactLocalCommitRecord`。 |
| `getReceipt(receiptId, optionalHandle, timeoutMs)` | 读取不可变提交回执。In-App 的 handle 可选；Persistent 必须传 handle，查询会校验 receipt ID、reference 和 seed Repo 身份。 |
| `checkAvailability(handle, probeKind="metadata", timeoutMs=0, maxAgeMs=0)` | 查询当前位置。`metadata` 检查元数据/存储表示；`verify_content` 还验证内容字节。返回 `AvailabilityReport`，不是新的 commit receipt。当前实现每次都发起新 probe，`maxAgeMs` 暂不用于缓存命中。 |

### 本地 Python：发布和取回

此例与仓库中的 `examples/python/NDNSF-DistributedRepo/artifact_api/` 用法一致。`ArtifactReference` 构造参数次序为 logical name、SHA-256、size、root-manifest name、publisher identity、policy epoch；算法和格式使用默认值。

```python
import hashlib
from pathlib import Path

from py_repoclient import ArtifactReference, ArtifactRepositoryApi

source = Path("model.bin")
publisher = "/example/artifact/publisher"
digest = hashlib.sha256(source.read_bytes()).hexdigest()
reference = ArtifactReference(
    "model-v1", digest, source.stat().st_size,
    f"{publisher}/NDNSF/ARTIFACT/{digest}/root",
    publisher, "policy-epoch-1",
)

api = ArtifactRepositoryApi.make_in_app(
    durable_root="./repo-store", owner_id=publisher
)
published = api.publish_file(source, reference)
assert published.handle is not None

api.fetch_file(reference, "./copy.bin")
```

`fetch_file(reference, ...)` 是 In-App 用法。持久化取回必须传入提交结果中的 `ArtifactHandle`；不要只用一个 reference 假定 Repo 位置。完整可运行的文件参数处理见 [publish_file.py](../examples/python/NDNSF-DistributedRepo/artifact_api/publish_file.py) 和 [fetch_file.py](../examples/python/NDNSF-DistributedRepo/artifact_api/fetch_file.py)。

### 持久化 Python：候选发现、应用选择、发布

`ServiceUser` 和它持有的 Face 由调用方创建。下面展示 API 顺序；选择策略应由应用决定，示例只选第一个已发现候选，不代表生产放置策略。

```python
from py_repoclient import (
    ArtifactCapabilityRequirements, ArtifactRepositoryApi,
    PersistentRepoClientConfig, RepoWriteConsistency,
    SelectedRepoSet, SelectedRepoTarget,
)

config = PersistentRepoClientConfig()
config.service_prefix = "/NDNSF/DistributedRepo/Object/v1"
api = ArtifactRepositoryApi.make_persistent(service_user, config)

requirements = ArtifactCapabilityRequirements()
requirements.artifact = reference
candidates = api.discover_repo_candidates(requirements, timeout_ms=30_000)
candidate = candidates.candidates[0]

target = SelectedRepoTarget()
target.repo_node = candidate.repo_node
target.provider_identity = candidate.provider_identity
target.service_prefix = candidate.service_prefix
selected = SelectedRepoSet()
selected.targets = [target]
selected.seed_repo_id = candidate.repo_node
selected.consistency = RepoWriteConsistency.ALL

result = api.publish_file(source, reference, selected, timeout_ms=30_000)
assert result.receipt is not None and result.handle is not None
receipt = api.get_receipt(result.receipt.receipt_id, result.handle)
report = api.check_availability(result.handle, "verify_content")
```

需要按幂等键启动并轮询时，改用 `begin_publish_file(...)`，再把返回的 `PublishOperationHandle` 传给 `get_publish_status(...)`。

实际调用要求 Face event loop 已在另一线程运行；阻塞式 API 不可从该 Face 线程调用，也不会自行启动 event loop。`ServiceUser`/Face 必须比 API 活得久。销毁顺序是先停止并 join event loop，再销毁 API，最后销毁 `ServiceUser`。超时只限制等待时间，不会停止 Face loop。

Python `RepoClient` 是轻量持久化兼容包装：`RepoClient(user, repo_service_name, ack_timeout_ms, timeout_ms)` 提供 `artifact_api`、`publish_file`、`fetch_file`、`begin_publish_file`、`begin_archive`、`discover_repo_candidates` 和 `archive`；`check_availability`、`get_publish_status`、`get_receipt` 转交给 `artifact_api`。持久化 `publish_file` 必须给 `ArtifactReference` 和显式 `SelectedRepoSet`；`fetch_file` 必须给 `ArtifactHandle`。

`py_repoclient` 还重导出若干支持型 Python API，按职责分组如下；这些不是另一套 Repo wire/client 实现：

| 模块 | 导出 API |
| --- | --- |
| `artifact_lifecycle` | `AuthenticatedReplicaReceipt`、`ArtifactReplicaSession`、`AtomicArtifactDestination`、`HmacReceiptAuthenticator`、`resolve_active_artifact`、`retrieve_to_atomic_destination`。 |
| `artifact_transfer` | `ArtifactStoreAssignment/Offer`、`PendingReplicaLeaseCollaboration`、`PendingReplicaTaskCollaboration`、`ReplicaLeaseCollaborationClient`、`ReplicaTaskCollaborationClient`、`ReplicaTaskControlSnapshot`，以及 store offer/assignment/upload lease 的 encode/decode helpers。 |
| backend/orchestration | `FilesystemArtifactApiBackend`、`CollaborationArtifactApiBackend`、`ArtifactControlMetrics`、`install_artifact_collaboration_service`、`NetworkDistributedRepoClient`、`RepoNodeApp`。 |
| persistence | `ArtifactCapacityStatus`、`ArtifactFinalizationRecord`、`ArtifactStorageIdentity`、`ArtifactTransferSessionRecord`、`FilesystemCasPayloadStore`、`LifecycleTransitionError`、`MetadataStore`、`PayloadStore`、`PersistenceOwnershipError`、`RepoLifecycleEvent`、`SqliteRepositoryPersistence`。 |
| service naming | `DEFAULT_REPO_SERVICE_ROOT`、`canonical_repo_operation`、`is_internal_repo_service`、`repo_service_for_operation`、`repo_versioned_services`。 |

### Artifact 值类型、结果和错误

| 类型 | 用途 |
| --- | --- |
| `ArtifactReference` | 内容身份；支持 `validate`、`sameBytes`、`toJson`/`fromJson`。 |
| `ArtifactCapabilityRequirements`、`RepoCandidate(Set)` | 声明需求与发现候选；候选带 provider、service prefix、能力和签名 ACK 摘要信息。 |
| `SelectedRepoTarget`、`SelectedRepoSet`、`RepoWriteConsistency` | 应用选择具体目标、seed 和 `One`/`Quorum`/`All` 一致性。 |
| `ArtifactHandle` | 发布后读取/查询的定位凭据；支持校验和 JSON codec。 |
| `ArtifactPublishResult`、`ArtifactReplicaResult` | 同步发布结果、每副本状态、handle、receipt、去重/恢复标记和阶段耗时。 |
| `PublishOperationHandle`、`PublishOperationStatus` | 幂等 operation ID 和轮询状态。 |
| `ArtifactCommitReceipt`、`ArtifactReplicaCommit` | 已提交证据和每个 Repo 的提交/位置版本信息；receipt 是历史提交记录。 |
| `ArtifactFetchResult`、`ArtifactFetchAttempt` | 成功取回所用身份、传输/验证字节、耗时以及逐 Repo 尝试。 |
| `AvailabilityReport`、`AvailabilityLocationReport` | 按当前位置报告可达性、元数据/内容检查和状态。 |
| `ArtifactLimits`、`ArtifactValidationError` | 输入/codec 资源限制；验证异常可读取稳定 `code()`。 |

C++ fetch 失败抛 `ArtifactFetchError`，可通过 `result()` 检查已完成的 attempts。Python 将它包装为 `ArtifactApiError`，结构化结果在 `.result`。其他 native validation/runtime 异常保持其原始类别。

## RepoClient：对象、range、分段和签名 Data

完整声明见 [`RepoClient.hpp`](include/ndnsf-distributed-repo/RepoClient.hpp)。`RepoClient` 是一组静态 C++ operation；它不拥有 `RepoNode` 或 `LocalServiceRegistry`。

| API | 用途 |
| --- | --- |
| `makeManifest` | 由对象名、类型、payload、复制/副本和 policy epoch 构造 `RepoObjectManifest`。 |
| `put` / `get` | 写入或读取一个完整 opaque object。`put` 返回 manifest。 |
| `getManifest` / `list` / `remove` | 查对象 manifest、枚举 catalog、删除对象。`remove` 返回是否有对象被删除。 |
| `putRange` / `commitRanges` / `abortRanges` / `getRange` | 分段上传、显式提交/中止和按 byte range 读取；未 commit 的 staging 不应被当成完整对象。 |
| `putSegmented` / `getSegmented` / `getObject` | 对大 opaque object 分块存储并在调用端重组；`getObject` 按 manifest 读取完整对象。调用端校验 manifest 中的 size/hash。 |
| `putDataPacket` / `getDataPacket` / `getDataPackets` | 保存/取回 NDN signed Data 的原始 wire；按完整原名及 manifest 顺序取回，不把 packet 名改成 Repo 自造别名。 |
| `insert` / `insertPayload` / `status` | 将既有 Data reference 插入 Repo，或用 KeyChain/SigningInfo 分段签名 payload 后插入；按 operation ID 查询状态。 |
| `fetchOriginalDataPacket` | 先检查副本的 receipt/当前位置，再通过原 Data 名取 wire；调用方必须提供 producer-signature verifier 和预期 wire SHA-256。 |
| `cacheStatus`、`catalogStatus`、`catalogSnapshot`、`catalogDelta(sinceEpoch)`、`catalogLookup(name)` | 查询缓存和 catalog epoch、snapshot、增量及单对象条目。 |

对象写入的最小用法（`node` 是已配置的 `RepoNode`）：

```cpp
using namespace ndnsf_distributed_repo;

std::vector<uint8_t> payload{'r', 'e', 'p', 'o'};
StoreOptions options;
options.objectType = "model";
options.policyEpoch = "/Policy/demo/v1";

const auto manifest = RepoClient::put(node, "/models/model-v1", payload, options);
const auto restored = RepoClient::get(node, manifest.objectName);
const auto current = RepoClient::getManifest(node, manifest.objectName);
```

大对象用 `putSegmented(node, name, payload, options, maxSegmentPayload)`，之后用 `getSegmented(node, manifest)` 或 `getObject(node, manifest)`。这些接口保存 opaque bytes；Repo 不替应用验证模型签名或应用层授权。

同进程 local 调用沿用相同 object manifest，但明确传 `LocalServiceRegistry` 和 service prefix：

```cpp
node.registerLocalServices(registry);
const auto localManifest = RepoClient::localPut(
  registry, node.servicePrefix(), "/models/local-only", payload, options);
const auto localBytes = RepoClient::localGet(
  registry, node.servicePrefix(), localManifest.objectName);
```

对象、分段、packet、catalog 和删除组合示例见 [`DistributedRepoSmoke.cpp`](examples/DistributedRepoSmoke.cpp)。

### RepoClient 的进程内 LocalServiceRegistry API

以下方法与上面对象操作对应，但目标是调用方提供的 registry + service prefix，适用于同进程组合，不发送网络 Interest：

`localPut`、`localGet`、`localPutRange`、`localCommitRanges`、`localAbortRanges`、`localGetRange`、`localGetManifest`、`localList`、`localRemove`、`localInsert`、`localStatus`、`localPutSegmented`、`localGetSegmented`、`localGetObject`、`localCacheStatus`、`localCatalogStatus`、`localCatalogSnapshot`、`localCatalogDelta`、`localCatalogLookup`。

range/object 用法与远端同名方法相同，仅首两个实参改为 `LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix`。不要把 local 调用结果当成网络路径或远端 Provider 权限的证明。

### RepoClient 的底层 ServiceUser 请求 API

这些接口接受 `ServiceUser`、service/provider 名称、timeout 和 timeout/response callbacks，直接暴露 request/ACK/selection/response 流程。高层应用通常应使用 `ArtifactRepositoryApi`，只有自定义协议调用、测试或 Repo 组件适配器才直接使用。

| 请求入口 | 操作 |
| --- | --- |
| `requestCapability`、`requestCacheStatus` | 查询 Provider 能力与 cache status。 |
| `requestStore`、`requestStoreRange`、`requestCommitRanges`、`requestAbortRanges` | 传统对象写入与 range 生命周期。 |
| `requestArtifactStore`、`requestArtifactStoreRange`、`requestArtifactCommitRanges` | artifact payload/range 写入。 |
| `requestFetch`、`requestFetchRange`、`requestManifest`、`requestInventory` | 读取对象、byte range、manifest 和 inventory。`requestFetchRange` 有普通 service-prefix 与显式 provider identity 两种 overload。 |
| `requestInsert`、`requestDelete`、`requestStatus` | 插入 Data reference、删除对象、读操作状态。 |
| `requestLocation`、`requestReceipt`、`requestAvailability` | 查询 artifact 的位置、回执和可用性。 |
| `requestSelectedService` | 通用的单 Provider 请求：限制 ACK 收集与最终选择到指定 Provider identity。 |

请求构造辅助项：`makeRequest(payload)` 将字节封装为 NDNSF `RequestMessage`。`DEFAULT_SERVICE_NAME` 为 `/NDNSF/DistributedRepo/Object/v1`。

## RepoNode：Provider/本地服务端 API

完整声明见 [`RepoNode.hpp`](include/ndnsf-distributed-repo/RepoNode.hpp)。构造时提供 service prefix、`StorageCapability`、共享 `RepoStoreBackend`，以及可选 Repo/provider 身份、artifact metadata 路径、limits 和 freshness。

| API | 用途 |
| --- | --- |
| `servicePrefix()`、`core()` | 读取 service prefix 或访问底层 Core。直接修改 `core()` 会绕过 RepoNode 的存储事务锁，调用方必须自行串行化。 |
| `registerLocalServices(registry)` | 把 local handler 安装到进程内 registry。 |
| `registerServices(provider)` | 将当前 Repo 操作注册到 NDNSF `ServiceProvider`。节点需先由 `std::shared_ptr` 管理。 |
| `registerServices(provider, publisherUser, ackCollectionTimeMs, requestTimeoutMs)` | 注册服务并启用发布操作 worker/回执路径。 |
| `registerOriginalDataServing(face)` | 显式让该 Repo 在原 Data name 下提供已提交 packet；Face 必须比 RepoNode 活得久。 |
| 对象方法 `put`、`putManifest`、`get`、`putRange`、`commitRanges`、`abortRanges`、`getRange`、`getManifest`、`list`、`remove` | 对 RepoCore 对象存储的直接方法。 |
| Data 方法 `putDataPacket`、`getDataPacket`、`hasDataPacket`、`importLegacyDataPacketReceipt`、`setDataReferenceFetcher`、`insertWirePackets` | 保存或查询原始 signed Data、显式导入旧回执，或插入应用取回的 wire packet。 |
| 报告方法 `cacheStatus` | 返回底层缓存状态。 |

Provider handler 入口由 `registerServices` 注册；名称是 `handleStore`、`handleStoreRange`、`handleCommitRanges`、`handleAbortRanges`、`handleInsert`、`handleStoreManifest`、`handleFetch`、`handleFetchRange`、`handleManifest`、`handleDataReceipt`、`handleDataLocation`、`handleInventory`、`handleCapability`、`handleCacheStatus`、`handleCatalogStatus`、`handleCatalogSnapshot`、`handleCatalogDelta`、`handleCatalogLookup`、`handleDelete` 和 `handleStatus`。这些是服务端适配器入口，不是消费者直接请求 API。

典型初始化形状：

```cpp
auto node = std::make_shared<RepoNode>(
  ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), capability, store,
  repoNodeId, providerIdentity);
node->registerServices(serviceProvider);
```

具体 storage backend 和 Provider/Face 生命周期由应用装配。RepoNode 的服务回调保留 shared owner；不要在注册前用栈对象构造后传入回调。

## 低层 RepoCore 与后端扩展接口

这些头文件是 Repo 实现、存储适配器和 DI 内容源会使用的公开组件接口，普通客户端不需要逐个调用。

| 头文件 / 类型 | API 范围 |
| --- | --- |
| [`RepoCore.hpp`](include/ndnsf-distributed-repo/RepoCore.hpp) · `RepoCore` | Object：`put`、`get`、`has`、`getManifest`、`probeObjectMetadata`、`list`、`remove`、`putManifest`。Range：`putRange`、`commitRanges`、`abortRanges`、`getRange`、`getRangeIfCurrent`、`removeIfCurrent`、`abortRangesIfOwned`、`reapExpiredRangeReservations`、`putRangeIfAbsent`、`commitRangesIfOwned`。Data：`putDataPacket`、`importLegacyDataPacketReceipt`、`getDataPacket`、`getDataPacketReceipt`、`hasDataPacket`。报告/协议：`cacheStatus`、`catalogStatus`、`catalogSnapshot`、`catalogDelta`、`catalogLookup`、`handleStore`/`handleStoreRange`/`handleCommitRanges`/`handleStoreManifest`/`handleFetch`/`handleFetchRange`/`handleManifest`/`handleInventory`/`handleCapability`/`handleCacheStatus`/`handleCatalogStatus`/`handleCatalogSnapshot`/`handleCatalogDelta`/`handleCatalogLookup`/`handleDelete`；另有 `acquirePublicationLock`。 |
| [`ArtifactTypes.hpp`](include/ndnsf-distributed-repo/ArtifactTypes.hpp) | `ArtifactReference`、`ArtifactLimits`、`ArtifactCapabilityRequirements/Capability/Ack`、location/receipt/availability request/response、publish intent/handle/status、root/page/chunk manifest、upload lease 和 replica receipt。对应 records 暴露 `validate` 与 `toJson`/`fromJson` codec（按类型定义）；通用 validators 为 `isHex`、`isKnownFormat`、`isKnownDigestAlgorithm`、`isPublicRootSignatureAlgorithm`、`validateName`、`validateIdentifier`、`validateDigest`、`validateUniqueStrings`。 |
| [`RepoTypes.hpp`](include/ndnsf-distributed-repo/RepoTypes.hpp) | `RepoObjectManifest`、`RepoByteRange`、`RepoDataReference`、`RepoOperationStatus/Metrics`、`RepoWriteReceipt`、`StorageCapability`、catalog/cache 记录和 artifact store/range request records；`RepoWriteConsistency`/`RepoDeploymentMode`；`toString`、`parseRepoWriteConsistency`、`requiredWriteAcks`、`normalizeRepoOperationState`。该头文件也声明 `RepoStoreBackend`，其实现契约是 `put`/`putManifest`/`get`/`has`/`erase`/`size`/`listManifests`/`usedBytes`/`cacheStatus`、`putRange`/`commitRanges`/`getRange`/`abortRanges`、`getManifest`/`probeObjectMetadata`、`findRetainedDataReceipt`/`attachImportedDataReceipt`、`supportsRange`/`fullCopyFallbackCount`/`supportsManifestLookup` 和 `pin`/`unpin`。 |
| [`RepoStoreBackend.hpp`](include/ndnsf-distributed-repo/RepoStoreBackend.hpp) | `ArtifactLifecycleState` helpers：`toString`/`parseArtifactLifecycleState`/`isAllowedArtifactTransition`。`PayloadStore` 的 `begin`/`writeRange`/`readRange`/`markVerified`/`verifiedRanges`/`flush`/`finalize`/`isCommitted`/`abort`；`MetadataStore` 的 `schemaGeneration`/`appendLifecycleEvent`/`lifecycleEvents`/`currentLifecycleState`；`BackendOwnershipLease` 的 `ownerId`/`lockPath`/`ownsBackend`；`RepositoryStoreFacade` 的 `payload`/`metadata`/`ownership`/`transition`。 |
| [`FilesystemRepoStoreBackend.hpp`](include/ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp) | `FilesystemRepoStoreBackend` 实现 object 方法 `put`/`putManifest`/`get`/`has`/`erase`/`size`/`listManifests`/`usedBytes`，range 方法 `putRange`/`commitRanges`/`getRange`/`abortRanges`/`getManifest`，receipt/metadata 查询 `probeObjectMetadata`/`findRetainedDataReceipt`/`attachImportedDataReceipt`，能力与诊断 `supportsRange`/`fullCopyFallbackCount`/`supportsManifestLookup`/`recoverOrphans`/`rootPath`/`vectorCompatibilityThreshold`。manifest 以 sidecar 形式发布，payload 由文件系统 range store 管理。 |
| [`FilesystemArtifactStore.hpp`](include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp) | `FilesystemArtifactPayloadStore` 实现 `begin`/`writeRange`/`readRange`/`markVerified`/`verifiedRanges`/`flush`/`finalize`/`isCommitted`/`abort`，并提供 `inspectCommitted`/`rootPath`/`committedPath`/`stagingPath`。`SqliteArtifactMetadataStore` 提供 `schemaGeneration`/`artifactWritesEnabled`/`migrationDiagnostics`、生命周期读写、`recordLocalCommit`/`lookupLocalCommit`/`lookupAggregateReceipt`、`recordPersistentCommit`/`commitPersistentArtifact`/`lookupPersistentLocation`/receipt existence checks、`reservePublishOperation`/`lookupPublishOperation`/`recoverablePublishOperations`/`updatePublishOperationTarget`/`finalizePublishOperation` 和 `databasePath`。工厂函数为 `makeFilesystemArtifactRepositoryStore(rootPath, ownerId)`。 |
| [`ArtifactManifest.hpp`](include/ndnsf-distributed-repo/ArtifactManifest.hpp) | `SignedArtifactRoot`、`ArtifactManifestTrustPolicy`、`ArtifactManifestVerificationResult`；编码/解码 `encodeSignedArtifactRoot`/`decodeSignedArtifactRoot`、`encodeArtifactManifestPage`/`decodeArtifactManifestPage`，名称派生 `deriveManifestPageName`/`deriveArtifactDataName`，校验 `verifySignedArtifactRoot`/`verifyArtifactManifestGraph`/`verifyArtifactChunkPayload`/`verifyArtifactPayload`/`validateArtifactResumeIdentity`，摘要 `artifactSha256Hex`，canonical 编码 `canonicalRootManifestBytes`/`canonicalManifestPageBytes`。信任锚由调用方解析并放入 trust policy。 |
| [`ArtifactTransfer.hpp`](include/ndnsf-distributed-repo/ArtifactTransfer.hpp) | `AdaptiveArtifactTransfer`：`poll`/`receive`/`markVerified`/`reject`/`expire`/`fail`/`snapshot`/`missingSegments`。`ArtifactResumeSession`：`restoreVerified`/`markVerified`/`missingChunks`/`renewLease`/`resume`/`cancel`/`expire`/`complete`/`fail`/`snapshot`/`identity`。`ReplicaLeaseControlFlow`：`beginCollaboration`/`closeAcks`/`commitPlan`/`fail`/`snapshot`。这些是传输组件状态机，不是文件发布入口。 |
| [`RepoEncryptedLargeDataStore.hpp`](include/ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp) | `RepoEncryptedLargeDataStore` 实现 Core `EncryptedLargeDataRangeStore` 的 `commitFile` 两种 overload、`lookupDurable` 和 `supportsDurableRetention`；内部 `Source` 提供 `isDurable`/`release`/`size`/`read`。用于已加密大数据持久化适配，不管理密钥或解密。 |
| [`RepoSourceProvider.hpp`](include/ndnsf-distributed-repo/RepoSourceProvider.hpp) | `RepoSourceProvider`：`lookupPrepared`、`load`（range-source 与 vector fallback overload）、`publish`、`rollback`、`loadMaterialSelection`、`stats`；为 Core/DI 提供 Repo-backed 数据读取和发布。 |
| [`RepoProtocol.hpp`](include/ndnsf-distributed-repo/RepoProtocol.hpp) | `makeRepoServiceName`、`toBytes`/`toString`；`encodeStoreRequest`、`encodeRangeWriteRequest`、`encodeRangeReadRequest`、`encodeRangeAbortRequest`、`encodeManifestRequest`、`encodeDataReferenceRequest`、`encodeStatusRequest`、`encodeCatalogDeltaRequest`、`encodeCatalogLookupRequest`；对应 `decodeStoreRequest`/`decodeRangeWriteRequest`/`decodeRangeReadRequest`/`decodeRangeAbortRequest`；`parseDataReferenceJson`/`parseOperationStatusJson`/`parseManifestJson`/`parseCatalogEntryJson`/`parseCatalogStatusJson`/`parseCacheStatusJson`/`parseCatalogDeltaJson`/`parseInventoryJson`/`encodeInventory`。线上交互由 RepoClient/RepoNode 调用；业务调用方一般无需自行拼协议字节。 |
| [`PersistentRepoServerConfig.hpp`](include/ndnsf-distributed-repo/PersistentRepoServerConfig.hpp) | `RepoServerMode`/`parseRepoServerMode`；`PersistentRepoServerConfig::fromArguments`/`validate` 与 service、identity、DB、trust schema、cache、阈值、ACK/timeout、availability freshness 配置。用于 C++ standalone server 启动，不是 client API。 |

## LegacyRepoImporter：显式的一次性迁移

[`LegacyRepoImporter.hpp`](include/ndnsf-distributed-repo/LegacyRepoImporter.hpp) 暴露 `LegacyRepoImporter::import(const LegacyRepoImportConfig&)`。用 `sourceDatabase`、`sourcePayloadRoot`、`destinationRoot` 和唯一 `importId` 指定来源与目的；可降低 `maxRangeBytes`/`maxDataWireBytes`，并通过 `onItemCommitted` 观察 durable staging checkpoint。结果 `LegacyImportManifest` 包含 source/destination inventory digest、每项 payload/manifest hash、状态和 legacy row claims。

```cpp
LegacyRepoImportConfig config;
config.sourceDatabase = "./old-repo.sqlite3";
config.sourcePayloadRoot = "./old-payloads";
config.destinationRoot = "./repo-v2";
config.importId = "migration-2026-10-10";
const auto manifest = LegacyRepoImporter::import(config);
```

迁移是显式入口，不会因普通 `RepoClient` 读取而隐式执行。相同 import ID 用于对已写入的目标做验证/恢复；不要覆盖来源或目的目录中的现有数据。

生产 CLI 使用相同的 native importer，适合在维护窗口显式迁移旧数据库和 payload tree：

```sh
build/NDNSF-DistributedRepo/ndnsf-repo-import \
  --source-db ./old-repo.sqlite3 \
  --source-payload-root ./old-payloads \
  --destination ./repo-v2 \
  --import-id migration-2026-10-10 \
  --max-range-bytes 4194304 \
  --max-data-wire-bytes 16777216
```

成功时 stdout 输出 `LegacyImportManifest` JSON；参数、源数据或目标检查失败时以非零状态退出。先在独立目标上验证输出，不要把 CLI 当作普通读 API 或覆盖式升级工具。

## 退役的 Python 兼容名称

`py_repoclient` 当前把 native pybind 类型作为事实来源，并保留少数旧 import 名供调用方得到版本化错误。退役项包括 `RepoDataPlaneProducer`、`artifact_capability_from_ack`/`capability_from_ack`/`ready_capability_from_ack`/`discovery_record_from_ack`、Python-owned artifact backend/session/driver、`negotiate_artifact_capabilities`、`begin_upload`/`begin_fetch`、`publish_file_async`/`fetch_file_async`，以及 `RepoClient` 的 `artifact_capabilities`、`capability`、`delete`、`fetch`、`fetch_object`、`get`、`get_object`、`insert`、`inventory`、`list`、`lookup_local_commit`、`publisher_namespace`、`publisher_object_name`、`publish_file_async`、`remove` 和 `store`。它们会明确报 `RepoCompatibilityError`；迁移到 native `ArtifactRepositoryApi` 或 C++ `DistributedRepoServer`。不要把这些名字按当前 API 编写新代码。

退役的 Python import 类型还包括 `ArtifactDescriptor`、`ArtifactApiBackend`、`ArtifactCancellationToken`、`ArtifactCapabilityNegotiation`、`ArtifactCapabilityRejection`、`ArtifactControlMode`、`ArtifactControlOptions`、`ArtifactErrorCode`、`ArtifactFetchDriver`、`ArtifactFetchSession`、`ArtifactProgress`、`ArtifactPublishDriver`、`ArtifactSessionStatus` 和 `ArtifactUploadSession`。仍可用的兼容别名是 `SelectedRepoBinding`（`SelectedRepoTarget`）、`LocalArtifactCommit`（`ArtifactLocalCommitRecord`）和 `WriteConsistency`（`RepoWriteConsistency`）。

## 使用边界速查

| 需求 | 选用 |
| --- | --- |
| 本地 immutable artifact、跨进程持久化 reference/handle | `ArtifactRepositoryApi.make_in_app` |
| 通过 NDNSF selection 把 artifact 写入/读出 Repo Provider | `make_persistent` + discover/应用选择 + `SelectedRepoSet` + handle |
| 通用对象、range、chunked bytes | C++ `RepoClient` / `RepoNode` |
| 存储和复用已签名原始 Data packet | `putDataPacket`、`getDataPacket(s)`；producer trust 仍由应用负责 |
| 同一进程内 handler 调用 | `RepoClient::local*` 或 `registerLocalServices`；不能据此声称网络覆盖 |
| 旧 Repo 数据搬到当前后端 | 显式 `LegacyRepoImporter::import` |

## API 与测试用例对应

下面列的是当前 Repo 候选已有的主要 native/Python 验证入口，便于按使用场景定位回归测试。组件测试和 authenticated in-process ServiceUser/Provider 测试不等于 MiniNDN 网络资格。

| API / 使用场景 | 代表性测试与覆盖用例 |
| --- | --- |
| `ArtifactRepositoryApi::makeInApp`、`publishFile`、`fetchFile`、`lookupLocalCommit`：本地 durable artifact 发布、重开、取回 | [`distributed-repo-inapp-artifact-api.t.cpp`](../tests/unit-tests/distributed-repo-inapp-artifact-api.t.cpp)：`PublishesReopensFetchesAndMatchesIndependentDiskOracle`、`FullReferenceIsTheCatalogKeyNotDigestAlone`、配置/损坏/恢复/原子 destination 用例；目标 `spec192-repo-inapp-api`。 |
| `ArtifactRepositoryApi::makePersistent`、候选发现、显式目标选择、发布状态、receipt、availability、fetch health：通过 NDNSF Provider 操作 artifact | [`spec192-persistent-artifact-api.t.cpp`](../tests/integration-tests/spec192-persistent-artifact-api.t.cpp)：`PersistentApiSelectsProvidersAndCountsOnlyDurableReceipts`、`PersistentApiFetchHealthStateSerializesConcurrentCalls` 等；目标 `spec192-repo-persistent-api` 使用 authenticated in-process transport。 |
| `RepoClient` / `RepoNode` 的对象、range、manifest、insert/status、catalog 和 handler：传统 Repo object 与服务端操作 | [`tests/wscript`](../tests/wscript) 中的 `spec202-repo-conformance` 汇总 `distributed-repo-*.t.cpp` 和 original-data receipt 用例；ACK-window 用例由 `spec190-ack-window` 和 `unit-tests` 目标执行；持久化 handler、注册生命周期、配置及身份约束另见 [`distributed-repo-persistent-service.t.cpp`](../tests/unit-tests/distributed-repo-persistent-service.t.cpp) 与 [`distributed-repo-artifact-receipt.t.cpp`](../tests/unit-tests/distributed-repo-artifact-receipt.t.cpp)。 |
| `ArtifactTypes`、manifest/transfer 状态机、receipt/availability codec、range 后端与 SQLite schema：协议值和存储组件 | [`distributed-repo-artifact-types.t.cpp`](../tests/unit-tests/distributed-repo-artifact-types.t.cpp)、[`distributed-repo-artifact-manifest.t.cpp`](../tests/unit-tests/distributed-repo-artifact-manifest.t.cpp)、[`distributed-repo-artifact-transfer.t.cpp`](../tests/unit-tests/distributed-repo-artifact-transfer.t.cpp)、[`distributed-repo-filesystem-artifact-store.t.cpp`](../tests/unit-tests/distributed-repo-filesystem-artifact-store.t.cpp)。 |
| Python `py_repoclient`、native bindings 与 `RepoClient` compatibility facade：应用侧调用 native Repo | 当前 Python conformance 执行 33 个文件、117 个测试通过；入口和 API-specific cases 可从 [`tests/python`](../tests/python) 中 `test_spec192_repo_*.py`、`test_ndnsf_repo_*.py` 和 `test_spec164_public_artifact_api.py` 定位。结果摘要保存在 Spec202 `tasks.md` 的 T009 记录。 |
| `LegacyRepoImporter::import` 和 `ndnsf-repo-import`：受控迁移、恢复、source immutability、CLI/native parity | [`distributed-repo-legacy-import.t.cpp`](../tests/unit-tests/distributed-repo-legacy-import.t.cpp)、[`spec192-legacy-repo-import.t.cpp`](../tests/integration-tests/spec192-legacy-repo-import.t.cpp)；`productionCliMatchesNativeImportAndReplayWithoutSourceMutation` 检查生产 CLI、重放、目标 parity 与源不变。 |
| 编译后 native/Python API smoke 与动态链接 | Spec202 T009 当前记录为：7 个 native targets、33-file/117-test Python suite、production CLI parity 均在候选 `aeb815...` 通过；详细日志见 [`tasks.md`](../specs/202-distributedrepo-convergence/tasks.md)。ASAN/UBSAN 和 fresh convergence 仍 OPEN。 |
| 网络 API、Provider admission/selection、原名 Data packet 与跨节点行为 | 由 Spec202 T010 MiniNDN campaign 验证；T010 仍 IN_PROGRESS，旧 candidate 的 `FAIL_STOPPED` 状态没有被上述本地测试改写。不要用 `local*`、native component 或 in-process 测试声称网络 API 已通过。 |
