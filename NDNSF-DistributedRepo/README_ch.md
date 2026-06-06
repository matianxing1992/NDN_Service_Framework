# NDNSF-DistributedRepo

NDNSF-DistributedRepo 是一个实验性 C++ 子项目，目标是为 NDNSF 应用提供 distributed、NDN-native、policy-controlled 的 artifact 和 intermediate-data storage layer。

当前实现有意保持较小，但已经同时组织为可复用 C++ API 和 NDNSF service layer：

- `RepoObjectManifest`：可准备签名的一个 stored object metadata。
- `StorageCapability`：repo node 广播的容量、负载和 mode 信息。
- `RepoCatalogStatus`、`RepoCatalogEntry` 和 `RepoCatalogDelta`：用于副本发现
  和恢复的 logical object 级目录元数据。
- `PlacementPolicy`：可控 replication requirements。
- `selectReplicas`：在候选 storage nodes 上进行 deterministic placement。
- `InMemoryRepoStore`：示例使用的本地 smoke-test store。
- `RepoCore`：不依赖 NDNSF networking 的可复用进程内存储逻辑。
- `RepoNode`：可以在 `ServiceProvider` 上注册 NDNSF services 的 repo node，并把 storage operations 委托给 `RepoCore`。
- `RepoClient`：高层 `put/get/list/remove` helper，以及需要直接控制 `ServiceUser` 的应用可用的 lower-level request helper。
- `py_repoclient`：可复用 repo client/protocol API 的 Python binding。

Service name 可以由应用配置。默认情况下，所有 repo nodes 共享：

```text
/NDNSF/DistributedRepo
```

C++ repo node 会在这个 prefix 下面暴露 operation-specific services。repo
层面的语义操作是 `INSERT`：APP 在自己的 namespace 下发布已签名、可选已加密的 NDN
Data segments，repo 按名字保存这些 opaque Data packets。`STORE` 和
`STORE_MANIFEST` 是当前 payload-accepting adapter，供已有 helper 和测试使用。它们
只是输入便利：调用方可以把 payload bytes 交给 API，但 repo/client adapter 仍应按
object name 对这些 bytes 命名、分段、签名为 NDN Data，并进入同一套 segment storage
语义。它们不是“小对象”的另一套 repo 模型。当前 C++ services 包括：

```text
CAPABILITY
STORE
INSERT
STORE_MANIFEST
FETCH
MANIFEST
INVENTORY
STATUS
CATALOG_STATUS
CATALOG_SNAPSHOT
CATALOG_DELTA
CATALOG_LOOKUP
DELETE
```

Python/NDNSF-DI helper 应逐步转向使用 `INSERT` 处理 APP 自己创建的 segmented
Data packets。

对象由 `RepoObjectManifest` 描述，其中包含 object name、object type、SHA-256、size、segment count、replication factor、selected replica nodes 和 policy epoch。manifest 是把 application object name 映射到 stored Data segments 与 hash 信息的元数据；它不是第二套 payload transport。Placement 和 replica selection 使用 NDNSF service discovery 与 ACK metadata。这样 repo 保持通用：存储的 Data 可以代表 model shard、runner、ONNX file、PyTorch artifact、activation tensor、payment-workflow record、telemetry log、JSON configuration，或其它任何 NDNSF application object。

## APP 自有分段 Data 引用

对于大对象，更符合 NDN 的路径是：APP 在自己的 namespace 下发布已签名、可选已加密的
segmented Data。Repo request 只携带一个 `RepoDataReference`：object name、Data
prefix、可选 segment range/final segment hint、forwarding hint、expected size 和
expected SHA-256。repo 通过注入的 SegmentFetcher adapter 拉取这些 Data packets，把每个
wire packet 作为 opaque bytes 存到 `<objectName>/ndn-data/<N>`，同时为父对象只保存
manifest。

Repo 不解密、不解释、不重新授权 application data。它只保存 opaque Data wire packets，并
提供 operation status：

```text
/NDNSF/DistributedRepo/INSERT
/NDNSF/DistributedRepo/STATUS
```

`STATUS` 返回 `RepoOperationStatus`，状态包括 `FETCHING`、`STORING`、`DONE` 和
`FAILED`。如果 standalone repo node 尚未配置 SegmentFetcher adapter，
`INSERT` 会明确失败，而不是假装已经拉取了远端 Data。

## In-App 和 Persistent Repo 模式

NDNSF-DistributedRepo 区分两种部署角色。**In-App Repo** 嵌入在 NDNSF 应用的同一个
trusted process 内，适合低延迟本地 cache、临时 intermediate artifact、recording
metadata，以及快速 same-process helper access。它不是长期备份数据的默认目标。
**Persistent Repo** 作为独立 repo node 或 server 运行，负责 backup、durable
storage、global catalog maintenance，以及节点迟到加入或错过目录更新后的恢复。

代码为了兼容旧配置仍然接受 `embedded` mode 字符串，但设计术语是 In-App Repo。当前
接受的 mode 字符串是：

```text
remote          独立 Persistent Repo service path
embedded        In-App Repo 的兼容旧拼写
in-app          In-App Repo 的推荐拼写
both            同时暴露 remote 和 in-process 路径
```

`StorageCapability` 现在携带 `repoMode` 和 `acceptsBackupReplica`。创建 backup
replica 时，placement logic 会过滤 `acceptsBackupReplica=false` 的候选节点。
In-App Repos 也可以广播 cached objects，但除非应用明确标记为 durable，它们在
catalog 中应被看作 local/ephemeral 角色。

## Catalog 与副本发现

Catalog 借鉴 HDFS 中“对象位置元数据”和“实际存储字节”分离的思想，但保持 NDN 风格的
去中心化设计。系统中没有单一 NameNode。每个 repo 维护自己的 local catalog，
Persistent Repos 可以交换 object-level catalog delta，从而维护 replicated global
catalog。

Catalog entry 描述 logical object 和 manifest，而不是列出每个 segment：

```text
objectName
manifestSha256
objectType
size
segmentCount
sourceRepo
repoMode
state
catalogEpoch
replicaNodes
manifest
```

当前 service/API surface 是：

```text
/NDNSF/DistributedRepo/CATALOG_STATUS
/NDNSF/DistributedRepo/CATALOG_SNAPSHOT
/NDNSF/DistributedRepo/CATALOG_DELTA
/NDNSF/DistributedRepo/CATALOG_LOOKUP
```

`CATALOG_STATUS` 返回 repo node、mode、当前 catalog epoch、object count，以及该 repo
是否接受 backup replicas。`CATALOG_SNAPSHOT` 返回用于恢复的完整 object-level
snapshot。`CATALOG_DELTA` 返回 caller 指定 epoch 之后的变化。`CATALOG_LOOKUP` 返回
某个 object name 的 catalog entry。

Catalog 不应该周期性广播完整目录，也不应该广播每个 segment 的完整列表。可扩展部署应
周期性发布很小的 delta，例如每 10 秒一次；当 Persistent Repo 迟到加入或落后太多时，
再通过 snapshot/diff 主动恢复。大数据本身仍然是已签名、可选已加密的 NDN segments；
catalog metadata 只帮助 client 判断哪个 repo 可以服务或复制该 object。

删除也应该体现为 catalog metadata，而不是静默删除本地文件。Repo 会为 object 发布
`DELETED` tombstone entry，peer repos 必须保存这个 tombstone，使更旧的 `AVAILABLE`
catalog entries 不能把对象复活。因此冲突处理除了 peer 本地 catalog sequence，还要考虑
object 的更新时间和删除语义。

Retention 也属于 catalog-level metadata。Object 可以携带 `ttlMs` 和 `repairAllowed`
字段。一旦 object 过期，lookup 会报告 `EXPIRED`，并且即使它副本不足，也不会把它放进
repair planning。

推荐的 Python-facing generic object API 会隐藏大部分 NDNSF setup 细节。在运行中的部署里，repo nodes 可以把部署配置作为普通 repo object 预加载。应用用户从 repo service bootstrap 参数开始，通过 repo 获取配置，然后执行普通 `put/get` 操作：

```python
from ndnsf_distributed_inference import DistributedRepo

repo = DistributedRepo.from_repo_config(
    controller="/example/repo/controller",
    user="/example/repo/user",
    group="/example/repo/group",
    trust_schema="examples/trust-schema.conf",
    config_object_name="/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml",
)
manifest = repo.put(
    "APP/Generic/BinaryBlob/demo",
    payload,
    object_type="binary-blob",
    replication_factor=2,
    policy_epoch="/Policy/example/v1",
)
payload = repo.get(manifest.object_name, manifest)
objects = repo.list()
repo.remove(manifest.object_name)
```

高层 `repo.put(...)` API 接收 application-relative suffix，并展开到 publisher namespace 下，例如 `/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo`。这让 app data name 全局唯一，也更容易验证。通过 repo 获取的 deployment config 遵循同一条规则；默认示例配置对象位于 controller publisher namespace 下。NDNSF-DistributedRepo 把传入 payload 当作 opaque application data：app 已经加密也好、明文也好，repo client 只负责分段和签名 Data packets，然后把这些 segments 存到 repo nodes。

## Namespace and Trust Schema Design

NDNSF-DistributedRepo 把 service namespace 和 stored-data namespace 分开。`/NDNSF/DistributedRepo` 只是用于 CAPABILITY、STORE、FETCH、MANIFEST 等操作的 invocation service name。Stored objects、deployment configs、manifests 和 payload segments 都命名在 application publisher identity 下。

推荐的 object namespace 形状：

```text
/<publisher>/NDNSF-DISTRIBUTED-REPO/OBJECT/<app-suffix...>
/<publisher>/NDNSF-DISTRIBUTED-REPO/UPLOAD/DATA/<digest>
```

Generic MiniNDN 示例中：

```text
/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo
/example/repo/user/NDNSF-DISTRIBUTED-REPO/UPLOAD/DATA/<digest>
/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml
```

Repo 不额外添加加密层。Application payload 对 repo 是 opaque：输入已加密就继续是加密 bytes，输入是明文就继续是明文 bytes；repo client 只负责分段并签名 repo nodes 存储的 Data packets。

Trust root 应该是 project namespace，而不是叶子名字。对于 root 位于 `/example/repo` 的项目，trust-root identity 就是 `/example/repo`，它签发 `/example/repo/controller`、`/example/repo/user`、`/example/repo/provider/repoA` 等 child identities。用 `/example/repo/root` 去签 `/example/repo/user` 并不满足 hierarchical trust，因为 `/example/repo/root` 不是 `/example/repo/user` 的父前缀。

对应的 trust schema 应该强制：

- application object Data names 位于 publisher identity 下；
- NDNSF runtime 和 SVS Data names 位于 signer identity 下；
- certificate names 保持 parent-child hierarchy；
- production validation anchor 使用 trust-root certificate，而不是宽松的 `type any` anchor。

`DistributedRepo.from_config("repo_policy.yaml")` 仍然可用于已经在本地拥有 policy file 的测试和部署工具。

当 framework 已经拥有 NDNSF `ServiceUser` 时，可以使用 lower-level Python binding：

```python
from py_repoclient import RepoClient

repo = RepoClient(user, "/NDNSF/DistributedRepo")
manifest = repo.insert(
    object_name="/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo",
    payload=payload,
    object_type="binary-blob",
    replication_factor=2,
    policy_epoch="/Policy/example/v1",
)
payload = repo.fetch(manifest.object_name)
```

如果调用方已经拿到 manifest，并且想获取一个逻辑 object，推荐使用
manifest-aware helper：

```python
payload = repo.fetch_object(manifest)
```

它会根据 manifest 的 size/hash 校验返回 payload。这样 application code
不需要知道 repo 内部到底是单 payload 存储，还是 object-level segmented 存储。

推荐的 C++ object API 也是 object-oriented：

```cpp
using namespace ndnsf_distributed_repo;

RepoNode node(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), capability);
StoreOptions options;
options.objectType = "binary-blob";
options.replicationFactor = 2;

auto manifest = RepoClient::put(
  node,
  "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo",
  payload,
  options);
auto fetched = RepoClient::get(node, manifest.objectName);
```

对于更大的 payload，使用 C++ segmented helper 存储。它把每个 chunk 作为独立
repo object 存到 `<object>/seg/<N>`，并额外保存一个 manifest-only parent
object。调用方仍应通过 manifest-aware object helper 获取；它会自动重组
segmented object，并验证 size/hash metadata。Repo 只存 opaque bytes；存储时
不做 APP trust、signature 或 hash 验证。APP 在 fetch 后根据 manifest/hash
自己验证。

```cpp
auto manifest = RepoClient::putSegmented(
  node,
  "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/model/yolo-stage0",
  payload,
  options,
  6000);
auto verifiedPayload = RepoClient::getObject(node, manifest);
```

`RepoClient::getSegmented(...)` 仍保留给测试和明确需要验证 segmented-object
路径的低层代码使用。

## C++ 独立 Repo Node

`DistributedRepoNodeApp` 使用同一个 `RepoNode` adapter，把 repo 作为独立
NDNSF provider 运行。它通过一个简单 key/value 配置文件启动：

```bash
./build/NDNSF-DistributedRepo/DistributedRepoNodeApp \
  --config NDNSF-DistributedRepo/configs/repo-node.conf
```

默认示例配置包含：

```text
service-prefix /NDNSF/DistributedRepo
identity /example/repo/repo/A
controller-prefix /example/repo/controller
repo-node /example/repo/repo/A
deployment-mode remote
repo-mode persistent
accepts-backup-replica true
```

`deployment-mode remote` 是独立 Persistent Repo 的正常模式。APP 也接受
`embedded`/`in-app` 和 `both`，这样独立进程和 service container 可以共用同一套
配置语义；但 local invocation 只对同一个 trusted process 内部的代码有意义。

如果只想验证构建和配置，不启动 NFD 或 controller，可以运行：

```bash
./build/NDNSF-DistributedRepo/DistributedRepoNodeApp \
  --config NDNSF-DistributedRepo/configs/repo-node.conf \
  --deployment-mode embedded \
  --dry-run \
  --local-smoke
```

## In-App Repo Plan

NDNSF-DistributedRepo 正在被组织成同一套 repo implementation 既可以作为独立
repo application 运行，也可以嵌入到另一个 trusted NDNSF service container
内部作为进程内服务使用。

目标分层是：

```text
RepoCore
  纯存储逻辑：put/get/manifest/list/delete/capability/catalog

RepoNode
  NDNSF service adapter：在 ServiceProvider 上注册
  STORE/FETCH/MANIFEST/INVENTORY/CAPABILITY/DELETE/STATUS/CATALOG_*，
  并把所有操作委托给 RepoCore

Repo app 或 embedding container
  决定远程暴露 RepoNode、注册 local services，或两者同时启用
```

当前第一步：`RepoCore` 拥有 object store、manifest validation 和 capacity/catalog
accounting。`RepoNode` 保留现有 public API 和 standalone service registration
行为，但把实际存储和 catalog 工作委托给 `RepoCore`。
`RepoNode::registerLocalServices(LocalServiceRegistry&)` 会把同一套
STORE/FETCH/MANIFEST/INVENTORY/CAPABILITY/DELETE/STATUS/CATALOG_* handlers 暴露给 trusted
in-process invocation。
进程内 repo 路径仍使用普通的 `LocalServiceRegistry::localInvokeRawInto(...)`
Request/Response message path；关键实现细节是 NDNSF message payload 内部由
payload TLV `ndn::Block` 承载，而不是通过额外的 local API 直接传 raw byte
vector。应用仍应调用 typed `RepoClient::local*` helper，而不是自己拼 repo
service name 或手写 message wrapper。
`RepoDeploymentMode` 用来选择启用哪一种 exposure path：

```cpp
RepoNode node(servicePrefix, capability);
LocalServiceRegistry localRegistry;

node.registerDeploymentServices(
  nullptr,
  &localRegistry,
  RepoDeploymentMode::Embedded);
```

嵌入式 caller 应该使用 `RepoClient` 的 local helper，而不是自己拼 repo
service name 或 raw `RequestMessage`：

```cpp
StoreOptions options;
options.objectType = "model";
options.replicationFactor = 1;
options.replicaNodes = {"/repo/embedded"};

auto manifest = RepoClient::localPut(
  localRegistry,
  servicePrefix,
  "/app/user/NDNSF-DISTRIBUTED-REPO/OBJECT/model/yolo-stage0",
  payload,
  options);
auto payloadAgain = RepoClient::localGet(
  localRegistry,
  servicePrefix,
  manifest.objectName);
```

支持的 mode 字符串是 `remote`、`embedded`、`in-app` 和 `both`。空配置保持默认
`remote` 行为，因此现有 standalone repo deployment 不受影响。

后续步骤：

1. 让 NDNSF-UAV-APP、NDNSF-DistributedInference 等高层应用在需要快速本地
   artifact/frame/intermediate-data access 时，从自己的 service container
   config 读取 deployment mode，并把 embedded registry 接到自己的组件中。
2. 远程 caller 继续走正常 NDNSF service path，保留 permissions、signatures、
   NAC-ABE 和 token/replay protection。

Local invocation 只是 trusted same-process composition 的优化。它不应该成为
wire-protocol mode，也不能让 remote caller 绕过 NDNSF access control。

`store_artifact(...)` 等 AI-specific helper 属于 NDNSF-DistributedInference。NDNSF-DistributedRepo 只存储 opaque named objects。

## 应用集成状态

NDNSF-DistributedRepo 目标上要同时支撑 UAV 和分布式推理 workload，但它应该保持为
storage 与 catalog control-plane component，而不是把任一应用的 domain logic 吸收到
repo 内部。

对于 **NDNSF-UAV-APP**，repo 是 recording、telemetry log、mission log 和其它持久数据
产品的 backing layer。当前 generic MiniNDN smoke 已经会存储并查询代表性的
`uav-recording`、`telemetry-log` 和 `mission-log` objects。剩余 UAV 工作属于应用集成：
Ground Station browsing、replay/download UI，以及 drone/mission identifier 到 repo
object name 的映射。

对于 **NDNSF-DistributedInference**，repo 是 model artifact、runtime artifact 和需要
跨越单个 service packet 生命周期的 activation bundle 的 backing layer。DI planner/executor
应该消费统一的 `largeDataReference` abstraction，再根据 reference source 选择
repo-backed fetch 或 direct NDN fetch。Model dependency planning、ONNX graph analysis、
tensor naming 和 role scheduling 仍然是 DI 的责任，不是 repo 的责任。

Repo 共享职责有意保持更窄：

- 把 opaque application objects 存成 segmented、signed Data products；
- 发布并合并 object-level catalog entries 和 tombstones；
- 报告 liveness、stale replicas、object class、retention 和 repair eligibility；
- 生成保守的 repair plan，并且只在 deployment policy 允许时可选执行 repair action；
- 暴露 In-App 和 Persistent deployment modes，但不改变 NDNSF remote invocation semantics。

这个边界让 repo 能同时服务两个应用，而不会变成 UAV log browser 或 AI artifact planner。

## Storage Backend

Repo node 初始化时带有逻辑容量，例如 Python `RepoNodeApp` 的 `free_bytes` 参数。Node 在 ACK metadata 中广播剩余容量，方便 client 选择 storage replicas：

```text
repoNode=/example/repo/provider/repoA
freeBytes=...
usedBytes=...
memoryCacheBytes=...
memoryCacheUsedBytes=...
storageBackend=sqlite
```

当前 persistent backend 是 SQLite。每行存储 object manifest 和 payload bytes，`payload_size` 列用于计算剩余容量。每个 repo node 还维护一个 in-memory LRU cache，用于最近存储或最近 fetch 的对象。Cache 只是优化；SQLite 是 source of truth。

C++ repo library 也暴露同样的选择：`RepoStoreBackend` 是存储后端接口。
`RepoCore` 默认使用 in-memory store，适合测试和临时 embedded 用法；
`makeSqliteRepoStore(path)` 则提供持久化 backend：

```cpp
RepoCore core(capability, makeSqliteRepoStore("/var/lib/ndnsf/repo/repo.sqlite3"));
```

`DistributedRepoNodeApp` 从配置中读取：

```text
storage-backend sqlite
storage-path /tmp/ndnsf-distributed-repo/repo-node-A.sqlite3
```

通过 SQLite backend 写入的 object，在 repo app 或 embedding process 重启后仍然可以 fetch。

Object API 会把 stored bytes 视为 manifest 后面的 opaque application object 或 opaque
segment records。应用不应该依赖 SQLite row layout 或 cache internals。当应用已经发布
signed segmented Data 时，repo-facing reference path 会把拉取到的 segment records 当作
opaque bytes 保存，并报告对应 manifest/catalog metadata。直接按匹配 Interest 返回 raw
Data wire packets 可以在 object API 下方继续优化，但不应该改变公开的
`put/get/insert/fetch` object semantics。

## Python Binding

`NDNSF-DistributedRepo/pythonWrapper` 安装一个名为 `py_repoclient` 的可导入包。它暴露与 C++ API 相同的通用概念：

```python
from py_repoclient import (
    RepoClient,
    RepoObjectManifest,
    StorageCapability,
    PlacementPolicy,
    select_replicas,
)
```

该 binding 会在 `./waf install` 中通过 `python3 -m pip install -e NDNSF-DistributedRepo/pythonWrapper` 自动安装。源码树开发时也可以手动安装：

```bash
python3 -m pip install -e NDNSF-DistributedRepo/pythonWrapper
```

NDNSF-DistributedInference 等高层框架应该使用 `py_repoclient` 进行 repo service operation 和 placement metadata 管理，同时把 `store_artifact(...)` 这类 AI-specific helper 保留在自己的包中。

长期服务设计：

```text
NDNSF-DistributedInference
  decides model roles, dependency graph, and runtime needs

NDNSF-DistributedRepo
  stores model/runtime/intermediate objects with controlled replication

NDNSF Core
  provides service discovery, selection, signing, NAC-ABE, and SVS transport
```

这个子项目不是 repo-ng 的替代品。它是一个 policy-controlled NDNSF storage layer：NDNSF 应用可以询问 repo cluster 某个 object 应该存放在哪里，用有界 replication 存储它，并在之后通过 NDNSF service invocation 和标准 NDNSF authentication/authorization 路径 fetch 它。
