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
CATALOG_QUERY
DELETE
```

Python/NDNSF-DI helper 应逐步转向使用 `INSERT` 处理 APP 自己创建的 segmented
Data packets。如果 APP 手里只有原始 payload bytes，应先把 payload 转成同一套 Core
large-data 表达：hybrid AES-GCM 加密、签名并分段的 Data，加上一个很小的 reference /
manifest。Repo 只保存 opaque Data packets 和 metadata；它不是另一套大 payload transport。

对象由 `RepoObjectManifest` 描述，其中包含 object name、object type、SHA-256、size、segment count、replication factor、selected replica nodes 和 policy epoch。manifest 是把 application object name 映射到 stored Data segments 与 hash 信息的元数据；它不是第二套 payload transport。Placement 和 replica selection 使用 NDNSF service discovery 与 ACK metadata。这样 repo 保持通用：存储的 Data 可以代表 model shard、runner、ONNX file、PyTorch artifact、activation tensor、payment-workflow record、telemetry log、JSON configuration，或其它任何 NDNSF application object。

## APP 自有分段 Data 引用

对于大对象，更符合 NDN 的路径是：APP 或 Core runtime 在自己的 namespace 下发布
hybrid-encrypted、已签名并分段的 Data。Repo request 只携带一个 `RepoDataReference`：
object name、Data prefix、可选 segment range/final segment hint、forwarding hint、
expected size 和 expected SHA-256。repo 通过注入的 SegmentFetcher adapter 拉取这些
Data packets。每个 packet 只被解码以取得并验证完整 Data name，例如
`/data/model/v=42/seg=0`；这个完整名字就是权威存储键，完整签名 wire 按字节原样保存。
logical object manifest 只保存有序的 `packetNames` 索引。packet 路径不会生成
`<objectName>/ndn-data/<N>` 或 `<objectName>/seg/N` alias。

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
/NDNSF/DistributedRepo/CATALOG_QUERY
```

`CATALOG_STATUS` 返回 repo node、mode、当前 catalog epoch、object count，以及该 repo
是否接受 backup replicas。`CATALOG_SNAPSHOT` 返回用于恢复的完整 object-level
snapshot。`CATALOG_DELTA` 返回 caller 指定 epoch 之后的变化。`CATALOG_LOOKUP` 返回
某个 object name 的 catalog entry。`CATALOG_QUERY` 返回按 object class、object type、
publisher、state、metadata tags、精确 metadata 字段以及 created/updated time range
过滤后的 object summaries。这些过滤只作用于 manifest/catalog metadata；repo node
仍然把 payload bytes 和已签名的 NDN Data packets 当作 opaque application data。

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
repair planning。部署可以在 `repo_control_plane.object_classes` 中覆盖 object class
默认值；repo 会把这些生命周期字段记录到 object manifest 中，但 payload bytes 仍然是
opaque application data：

```yaml
repo_control_plane:
  object_classes:
    uav-recording:
      minReplicationFactor: 2
      maxReplicationFactor: 3
      ttlMs: 604800000
      repairAllowed: true
      autoDelete: false
```

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
    metadata={"tags": ["demo", "binary"], "workflow": "example"},
)
payload = repo.get(manifest.object_name, manifest)
objects = repo.list()
matches = repo.catalog_query(
    manifest.replica_nodes[0],
    {"objectClass": "binary-blob", "tags": ["demo"]},
)
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

对于明确要把任意 opaque bytes 切成 object-level chunks 的调用方，旧 C++
segmented helper 会把每个 chunk 作为独立 repo object 存到
`<object>/seg/<N>`，并额外保存一个 manifest-only parent object。调用方仍应通过
manifest-aware object helper 获取；它会自动重组
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
storage-backend tiered
storage-path /tmp/ndnsf-distributed-repo/repo-node-A.sqlite3
memory-cache-bytes 67108864
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

Repo service 不定义额外的 certificate-selection policy。远程 repo provider/client
继承 NDNSF runtime 行为：certificate role 在启动时解析一次，RSA 保留给
NAC-ABE/permission unwrap；如果安装了 EC/ECDSA certificate，则优先用于签名。

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

## SQLite 权威存储与有界热点缓存

Repo node 初始化时带有逻辑容量，例如 Python `RepoNodeApp` 的 `free_bytes` 参数。Node 在 ACK metadata 中广播剩余容量，方便 client 选择 storage replicas：

```text
repoNode=/example/repo/provider/repoA
freeBytes=...
usedBytes=...
memoryCacheBytes=...
memoryCacheUsedBytes=...
storageBackend=tiered
authoritativeBackend=sqlite
cachePolicy=lru
```

所有实际部署的 Repo node 都以 SQLite 作为 manifest、opaque payload 和已存 Data
packet 的唯一权威来源。一个按字节限制容量的 LRU memory tier 用于加速刚刚提交和
重复读取的内容；普通 object 与完整 packet set 共用同一个预算。写入时先提交
SQLite，再加入 cache；读取时先查 memory，miss 后回退 SQLite；delete/overwrite
也只在权威事务成功后更新 cache。大于整个预算的 entry 会绕过 memory，但仍能从
SQLite 读取。

C++ repo library 通过 `RepoStoreBackend` 和 `makeTieredRepoStore(...)` 暴露这套策略：

```cpp
RepoCore core(capability, makeTieredRepoStore(
  "/var/lib/ndnsf/repo/repo.sqlite3",
  64 * 1024 * 1024));
```

C++ in-memory backend 只保留为 direct unit test 的内部 test double，不是受支持的
Repo-node 部署模式。独立 APP 会明确拒绝 `storage-backend memory`，不会静默失去
持久性。

`DistributedRepoNodeApp` 从配置中读取：

```text
storage-backend tiered
storage-path /tmp/ndnsf-distributed-repo/repo-node-A.sqlite3
memory-cache-bytes 67108864
```

设置 `memory-cache-bytes 0` 可以关闭热点 cache admission，但 SQLite 权威存储和
object API 保持不变。这是“SQLite-only”运行，不是 memory-only 兼容模式。

direct、embedded、remote NDNSF 和 Python client 都可以调用 `CACHE_STATUS`。返回值
包括 backend/authority/policy、当前 `budgetBytes`/`usedBytes`/`entryCount`，以及累计
`hits`、`misses`、`admissions`、`evictions`、`invalidations`、
`oversizedBypasses`、`backingReads` 和 `backingWrites`。Cache 内容和计数器属于当前
进程，重启后会清零；SQLite object 不会丢失。

Python persistent Repo 遵循相同顺序；没有显式 storage directory 时，会在
`/tmp/ndnsf-distributed-repo/` 下生成确定性的 SQLite 路径。它不会再把所有持久
payload 复制到无界 Python dictionary。Repo 重启后，network fetch 会先向目标 Repo
发送 `FETCH_PREPARE`；Repo 通过 hot cache/SQLite 恢复完整 object 或 packet set，
启动有生命周期限制的 Data producer，然后 client 使用正常 NDN segmented fetch。
临时 producer 只是传输资源，不属于权威存储或热点 cache entry。

Packet API 与旧的 opaque-object chunking helper 是两条不同路径。已经签名的 Data 在
`data_packets` 中以完整 Data name 为键只存一份；`object_packet_refs` 把 logical manifest
映射到这些名字。两个 manifest 可以安全引用同一个 immutable packet。同名同 wire 的写入
是幂等操作，同名不同 wire 会被拒绝。`FETCH_PACKET_PREPARE` 可以激活一个已知完整名字，
packet-backed `FETCH_PREPARE` 会返回原始 versioned Data name 和 `packetNames`。Producer
原样返回 wire，不重新签名、不改名、不重新分段，也不重建 packet。

`RepoClient::putSegmented/getSegmented` 仍作为 arbitrary opaque bytes 的兼容 helper，
它使用 logical `<object>/seg/N` chunk，但不是规范的 NDN Data packet 路径。
SegmentFetcher 是 consumer 端基于原始名字进行获取和重组的工具，并不允许 Repo 替换名字。

Logical manifest name 仍位于已认证 publisher namespace 下，但 packet name 可以使用
`/data/...` 这样的 application-owned namespace。Packet API 会验证每个 wire 内的名字都在
声明的原始 Data prefix 下；是否允许执行 insert 仍由 NDNSF service permission 决定。

Packet-backed consumer 必须使用 manifest 索引，不能自行推导 packet name：

```cpp
auto wires = RepoClient::getDataPackets(node, manifest);
```

```python
manifest = repo.put_signed_packets(
    object_name, packets,
    object_type="video-segment-set",
    object_size=len(payload),
    object_sha256=sha256)
packets = repo.get_signed_packets(manifest.object_name, manifest)
```

两种操作都会检查 `packetNames` 非空、不重复、数量与 `segmentCount` 一致，然后按顺序
使用每个完整名字获取并解码 wire；只要实际 wire name 不匹配就拒绝。一次 replica 尝试必须
完整成功才会返回；Python 在失败后会从下一个声明的 replica 重新获取完整 packet set。
普通 `get(...)` 仍可作为 payload view，从同一组原始 packet 重组 content，但不会改变存储。

Replica preparation 还会返回一个短生命周期、Repo 专属的 forwarding hint。这个 hint 用于
选择实际提供数据的 Repo，但每个 Interest name 仍是 application 原始 Data name。因此，即使
多个 replica 保存相同 packet prefix，也不需要 Repo alias。Failover 以整个 packet set 为
原子边界：失败尝试已经取得的 packet 会被丢弃，下一个 replica 从 manifest 第一个名字重新
开始；client 不会把 primary 的部分结果与 secondary 的结果拼接起来。

双 replica MiniNDN 验收会把同样的四个 signed packet 存入 Repo A 和 Repo B，在 Repo A
返回 packet zero 后终止它，并验证 Repo B 从 packet zero 开始收到全部四个名字：

```bash
sudo -n -E python3 Experiments/NDNSF_DistributedRepo_Generic_Minindn.py \
  --exact-packet-failover-smoke \
  --nlsr-wait-s 10 \
  --repo-start-wait-s 15 \
  --output-dir results/distributed_repo_exact_packet_failover_minindn
```

结果写入 `exact-packet-failover-summary.json`，其中记录每次 Repo/name 尝试、exact-name 与
wire-identity 检查、总延迟和 failover 延迟。2026-07-10 验收总耗时 50,772 ms，其中
failover interval 为 42,735 ms；正确性全部通过，但 failed-primary 的长 timeout 仍是后续
延迟优化点。

当前应用统一使用下面的边界：

| Caller data | API | 原因 |
|---|---|---|
| Application 已签名的 NDN Data | `put_signed_packets` / `get_signed_packets` | 完整名字、签名和 wire 属于 application identity |
| DI model/runtime file 和 activation blob | object `put` / `get` | caller 拥有的是 bytes，不是编码后的 Data packet |
| UAV 加密 recording chunk | object `put` / `get` | 加密结果是 opaque byte object |
| 未来已经编码成 signed Data 的 UAV/DI output | packet API | 保留 producer 的原始 packet representation |

专用 MiniNDN restart/cache 验收命令是：

```bash
sudo -n python3 Experiments/NDNSF_DistributedRepo_Generic_Minindn.py \
  --tiered-cache-smoke \
  --tiered-cache-bytes 8192 \
  --tiered-cache-object-bytes 4096 \
  --output-dir results/distributed_repo_tiered_cache_minindn
```

它存储三个确定性 object，在同一 database 上重启 Repo A，然后依次读取
`A, A, B, C, A`。规范结果写入 `tiered-cache-summary.json`；通过条件包括 cold
backing read、第二次读取 hit 且不增加 backing read、LRU eviction、SQLite fallback、
digest 一致，以及 `usedBytes <= budgetBytes`。

## 大文件公开 Python API（`artifact-manifest-v2`）

应用通过 `ArtifactRepositoryApi`（或 `RepoClient` 上的同名方法）发布和获取
不可变文件，不再自行构造 packet batch、访问私有字段选择副本或调用副本内部控制
操作。最小的本地持久化示例如下：

```python
from py_repoclient import ArtifactRepositoryApi, FilesystemArtifactApiBackend

api = ArtifactRepositoryApi(
    FilesystemArtifactApiBackend("/var/lib/ndnsf-artifacts"),
    publisher_identity="/example/publisher",
)
published = api.publish_file(
    "model.onnx",
    name="/models/example",
    expected_sha256="0123456789abcdef" * 4,
    replicas=1,
)
fetched = api.fetch_file(published.reference, "cache/model.onnx")
```

`FilesystemArtifactApiBackend` 是供开发和本地运行使用的单副本、崩溃安全后端。
部署后的 NDNSF runtime 应安装 `ArtifactApiBackend` adapter，并默认使用普通
NDNSF Collaboration；只有已知 provider 的显式优化才使用
`ArtifactControlOptions(mode=TARGETED, targeted_provider="/repo/A")`。Targeted
不会绕过 NDNSF 授权、一次性 token、重放保护、manifest 验证或持久 receipt 检查。

维护中的命令行应用位于
`examples/python/NDNSF-DistributedRepo/artifact_api/publish_file.py` 和
`examples/python/NDNSF-DistributedRepo/artifact_api/fetch_file.py`。二者支持同步、
`--async` 和 `--advanced` session 模式。高级流程分别是
`begin_upload -> upload_file -> commit` 与
`begin_fetch -> transfer -> commit`；调用
`abort(preserve_progress=True)` 会保留经过验证的 staging 状态，供同一个
idempotency key 精确续传。

### 发布参数

| 参数 | 含义 |
|---|---|
| `path` | 已存在的普通源文件。 |
| `name` | 绝对逻辑 NDN 名称；完整身份还包括 SHA-256、大小、格式、publisher 和 policy epoch。 |
| `expected_sha256` | 必需的 SHA-256 断言；不匹配时在传输前失败。 |
| `replicas` | 正整数的目标持久副本数。 |
| `verification` | `artifact-manifest-v2` 必须使用 `signed-manifest`。 |
| `resume` | 中断后保留并验证属于同一 operation 的 staging 前缀。 |
| `on_progress` | 可选、有界、单调的 `ArtifactProgress` observer；慢 observer 不会阻塞传输。 |
| `idempotency_key` | 稳定的重试身份；省略时由逻辑名和 digest 推导。 |
| `policy_epoch` | 绑定到 `ArtifactReference` 的信任策略版本。 |
| `timeout_ms` | 正数的总 operation deadline；省略时使用 API 默认值。 |
| `control` | `ArtifactControlOptions`：默认 Collaboration，或显式 Targeted provider。 |
| `cancellation` | 可选 `ArtifactCancellationToken`；异步 task 取消会传播到该 token。 |

`ArtifactPublishResult` 返回 `reference`、`operation_id`、
`requested_replicas`、`achieved_replicas`、逐副本 state/receipt、
`deduplicated`、`resumed`、`total_duration_ms` 和各阶段耗时。实际 durability
始终等于不同且通过认证的 `COMMITTED` receipt 数量。

### 获取参数

| 参数 | 含义 |
|---|---|
| `reference` | 完整、不可变的 `ArtifactReference`；仅有逻辑名称不够。 |
| `destination` | 最终本地路径；只有完整验证并原子 rename 后才可见。 |
| `resume` | 只复用属于同一 operation/reference 且已经验证的前缀。 |
| `verify` | 验证重组后的不可变 digest；部署的 v2 backend 还必须验证签名 manifest。 |
| `replace` | 允许原子替换冲突的已有 destination；默认明确失败。 |
| `on_progress` | 可选、有界、单调的进度 observer。 |
| `idempotency_key` | 稳定的重试身份；默认由 digest 和 destination 绝对路径推导。 |
| `timeout_ms` | 正数的总 operation deadline。 |
| `control` | 默认 Collaboration，或显式 Targeted provider。 |
| `cancellation` | 可选取消 token。 |

`ArtifactFetchResult` 返回 `reference`、`operation_id`、`destination`、
`reused_bytes`、`transferred_bytes`、源副本、总耗时和各阶段耗时。
`reused_bytes + transferred_bytes` 必须等于不可变大小。同步和异步调用返回相同
类型的结果：

```python
published = await api.publish_file_async(
    source, name=name, expected_sha256=digest)
fetched = await api.fetch_file_async(published.reference, destination)
```

所有公开错误都是 `ArtifactApiError`，稳定 `code` 只能是：
`INVALID_ARGUMENT`、`UNSUPPORTED_CAPABILITY`、`AUTHORIZATION_FAILED`、
`TRUST_VALIDATION_FAILED`、`MANIFEST_INVALID`、`CONTENT_DIGEST_MISMATCH`、
`LEASE_EXPIRED`、`CAPACITY_UNAVAILABLE`、`TRANSFER_TIMEOUT`、`CANCELLED`、
`REPLICA_COMMIT_FAILED`、`DURABILITY_NOT_ACHIEVED`、
`DESTINATION_CONFLICT`、`RECOVERY_REQUIRED` 或 `INTERNAL_ERROR`。异常还携带
有界 message、operation ID、已知时的 artifact reference 和已实现副本数，不会
暴露由 peer 控制的原始诊断文本。

`exact-packet-v1` 继续作为显式兼容 API，供必须保留应用签名 Data 原始 wire bytes
的调用方使用。上述文件 API 不会静默选择或重新解释它。

### Artifact schema 迁移与回滚

Artifact 元数据使用持久 schema generation 12。启动时只执行增量
roll-forward，并在 active catalog、认证 receipt 和 GC claim 中显式保存
`formatVersion` 与 `digestAlgorithm`；不会重写已提交 payload bytes，也不会
改写旧的 exact-packet wire。

`RepoNodeApp(..., artifact_writes_enabled=False)` 可显式禁止新的 v2 发布。
`artifact_max_write_schema_generation=N` 在数据库代际高于 `N` 时也会使节点进入
只读回滚模式；数据库代际高于当前 runtime 时同样自动 fail closed。已提交 v2
对象仍可读取，但新的 v2 lifecycle、resume、finalization、reservation 和 GC
修改都会以 `repo-artifact-writes-disabled` 失败。

ACK 和 `CAPABILITY` payload 会提供有界的 `artifactMigration` 诊断。只读节点会
从能力广告中撤回 `artifact-manifest-v2`、resume 和 durable receipt 支持，使
能力协商在传输前拒绝该节点。`exact-packet-v1` 仍独立广告，其信任语义不变。

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
# 高可用运行时契约

持久 Repo 使用 SQLite 作为权威存储，并使用有容量上限的内存 LRU 作为加速层。
写请求携带可幂等重放的 operation ID，并返回每个副本的持久 write receipt；
`ONE`、`QUORUM` 和 `ALL` 决定必须收集的 receipt 数量。正 ACK 只是当时的容量
与队列快照，不预留任何字节；Selection 才分配任务，Provider 随后将任务加入有界
执行队列，并在真正开始执行时完成最终容量检查。placement cache 会过期；过载、
超时、容量不足或完整性失败会使对应节点的缓存选择失效，并让节点进入短暂的健康
冷却期。

Repo Data 由每个进程一个长期运行的 producer 提供。应用签名的精确 Data wire
按原始完整名称存储和返回，不改名、不重新签名。普通大对象仍使用
SegmentFetcher；连续发布属于 stream 语义，不属于 Repo 对象 API。

Catalog journal、tombstone、peer watermark、membership heartbeat 和 repair job
都能跨重启恢复。旧版本升级后的数据库可以为回滚保留只读
`capacity_reservations` 表；当前 ACK、placement 和 write 路径既不读取也不修改它。
Bucket digest 用于有界 anti-entropy。
同一 generation 的在线副本若 content digest 不同，会明确报告 `CONFLICT`，
repair scheduler 不会静默选择其中一个。Repair job 支持幂等、lease、退避重试和
周期扫描，不再依赖 sidecar 进程生命周期内的抑制集合。

C++ 库是对象和协议契约的权威实现；Python 负责 NDNSF 网络编排、SQLite
catalog/repair 持久化、placement telemetry 和 MiniNDN campaign。`repo-ng`
command wire 兼容属于未来 adapter，不应成为第二套内部策略实现。

## Targeted 并行控制面

当副本集合已知时，Repo 控制操作在完成标准认证 token bootstrap 后使用 NDNSF
Targeted 调用。副本存储分配通过同一个 `ServiceUser` 异步提交并共享一个总截止
时间，之前不再有 reserve/release 往返。如果某个副本失败，其他副本已经返回的
成功 receipt 仍会保留；最终写入仍必须满足请求的 `ONE`、`QUORUM` 或 `ALL`
一致性级别。

Targeted 只是优化，不是安全绕过。权限检查、NAC-ABE 保护、一次性 provider
token、重放保护、operation ID、receipt 验证和写一致性检查都继续生效。可选的
有界 fallback 使旧的 Normal-only provider 仍可使用，并在控制指标中单独统计。

`NetworkDistributedRepoClient` 接受 `control_mode="normal"` 或
`control_mode="targeted"`，并提供 `control_metrics()`。当前 campaign lifecycle
记录使用 `planningMs`、`queueWaitMs`、`sessionStartMs` 和 `storeMs`；读取器只为
兼容旧证据把历史 `reserveMs` 归一化为 `sessionStartMs`，不会恢复 reservation
协议。summary JSON 记录 Targeted、normal、timeout、fallback、fan-out 和最大
并发计数。Targeted token batch 可通过 `NDNSF_TARGETED_TOKEN_BATCH_SIZE`
调整，范围 1--256，默认值为 8。

RF=2、W=ALL 的匹配 60 秒 MiniNDN 实验结果如下：

| 工作负载 | Normal write p95 | Targeted write p95 | Targeted 完成数 |
|---|---:|---:|---:|
| c16、2 RPS、90% read | 39,838.543 ms | 243.134 ms | 120/120 |
| c4、0.5 RPS、10% read | 10,583.983 ms | 192.855 ms | 30/30 |

第一次 RF=2 并行实验还暴露了 OpenABE/RELIC backend 的跨线程不安全问题。
NAC-ABE 现在把所有 OpenABE 操作串行调度到同一个进程级专用线程，并在该线程
完成初始化。这样既保证正确性，也明确保留了每个进程内 ABE 操作串行化的边界。

精确复现命令见 `specs/078-repo-targeted-control-plane/quickstart.md`，验收证据见
`specs/078-repo-targeted-control-plane/results.md`。

### Provider 掉线时的 RF=3 quorum

期望副本数和写确认阈值是两个独立概念。RF=3/W=QUORUM 在一个期望 Repo
不可用时，可以凭两个经过验证的持久 receipt 提交；manifest 仍保留
`replicationFactor=3` 供后续 repair 使用，`confirmedReplicaNodes` 只列出真正
返回 receipt 的节点。W=ALL 仍然要求三个 receipt。

Selection assignment 给出期望副本，而 receipt 阈值决定结果能否提交；两者之间
不创建容量 reservation。Targeted 和 fallback 结果会更新 provider health；如果
Targeted 和 Normal fallback 都失败，该 provider 会进入比“Targeted 暂态失败但
fallback 成功”更强的 cooldown。

在匹配的 60 秒 RF=3/W=QUORUM MiniNDN 实验中，RepoA 在第 20 秒被停止。
故障后的 19 个请求全部成功，其中 17 个 write 都恰好获得两个 receipt；故障后
write p50/p95 为 178.457/1,649.912 ms。唯一失败的 write 发生在注入故障之前，
原因是三个 Targeted 投递同时超时，因此总结果如实记录为 29/30，而没有错误地
归因于 RepoA 掉线。详见 `specs/079-repo-targeted-quorum-failure/results.md`。

### Provider 恢复后的在线 repair

故障 Repo 和它的 catalog sidecar 被视为一个恢复单元。Repo 进程重启后，sidecar
也使用相同 identity、peer、policy 和持久存储重新启动。它先发布新的 membership，
合并 peer catalog delta，再扫描和领取持久 repair job，并通过现有、经过验证的
NDNSF repair service path 补副本。Campaign harness 只观察和统计该过程，不直接
复制 SQLite 文件或 payload。

在 60 秒 RF=3/W=QUORUM 恢复实验中，30 个请求全部完成。RepoA 严格离线期间有
5 个 write 完成；12 秒后 RepoA 重启，它的 sidecar 创建了 10 个持久 repair job，
并在测量窗口结束前完成 3 个 repair。其中 1 个属于严格离线期间的 5 个对象，
所以本轮有界窗口 coverage 为 20%；首次 repair 在重启后 15.015 秒完成。RepoA
持久存储，以及 A/B/C 三个相同 digest 的 AVAILABLE catalog entry，确认该对象
真正恢复到 RF=3。其余 4 个离线期对象仍明确保留为 repair backlog，而没有被错误
报告为已恢复。详见 `specs/080-repo-online-repair-recovery/results.md`。

### 有界 repair worker 与 quorum finalization

Repair job 现在持久记录风险、优先级、对象年龄和缺失副本数。Claim 先选择可用
副本更少的对象，再按更高优先级和更早更新时间排序。Sidecar 可以并发执行 1--8
个独立对象传输，但 scan、claim、complete 和 fail 仍通过同一个 `ServiceUser`
串行执行；生产默认值仍为 1 个 worker。

多副本本地写入现在先进入 `STAGED`。只有 user 验证至少 W 个持久 receipt 并发送
受保护的 `FINALIZE_WRITE` 后，provider 才发布 `AVAILABLE`。Staged generation
不会进入公开 inventory、read、Data-plane prefix、repair source 或 repair job，
因此低于 quorum 的失败写入不会在恢复时被 repair 复活。

两组匹配的 60 秒 MiniNDN 实验都是 30/30 成功，receipt floor 为 2，错误修复失败
写入的事件数为 0。Workers=1 修复 2/4 个严格离线对象（50%），请求 p50/p95 为
239.371/5,371.381 ms；workers=3 修复 1/4（25%），为
318.392/5,660.957 ms。Worker pool 的实现正确且可配置，但本轮没有吞吐收益，
瓶颈是 catalog/control path 产生可领取 job 的节奏，而不是 transfer capacity。
详见 `specs/081-repo-bounded-parallel-repair/results.md`。

### Repair fast path 与阶段可观测性

Durable repair 不再对 catalog 已确认缺失的 target 发送 `FETCH_PREPARE`。
这个 negative ACK 以前会在单一 client owner thread 上变成固定 selection timeout，
从而串行阻塞 worker 启动。现在 repair 直接从 source `FETCH_PREPARE` 开始，但仍然
强制执行精确 Data 获取、packet/object hash、repair authorization、target 持久化、
lease 和 completion 检查。

`REPAIR_SCAN` 现在报告持久 job state 和本地 target 的 claimable 数量。Sidecar
日志记录 peer merge 的 batch/耗时，以及 repair cycle 的 scan、claim、transfer 和
总耗时；MiniNDN summary 会解析这些指标以定位瓶颈。

唯一一次预先计划的匹配 workers=3 实验仍为 30/30、W=2、错误 repair 数为 0。
严格离线对象 coverage 从 1/4 提升到 4/4，首次 repair 从 20.248 秒降到
10.587 秒，请求 p95 从 5,660.957 降到 1,814.117 ms。首次 cycle 显示 9 个
claimable job，并在 0.838 秒内完成 6 个。当前下一个可测瓶颈是 catalog merge
分批。详见 `specs/082-repo-repair-fast-path-observability/results.md`。

### 使用精确分段 Data 合并大 catalog

较大的 anti-entropy delta 现在只发送一个受保护的 `CATALOG_MERGE_PULL`
控制请求，不再串行发送许多小型 inline merge batch。Source 发布一个不可变、
已签名的 segmented object；请求绑定它的精确名称、schema version、字节数、
entry 数和 SHA-256 digest。Target 使用 SegmentFetcher 获取完整对象，验证所有
绑定字段后才合并 catalog entry。6,000 字节以内的 payload 仍走 inline；pull
上限为 16 MiB；pull 失败时回退到原有的有界 batch 路径。

匹配的 workers=3 MiniNDN treatment 仍保持 30/30、W=2、错误 repair 为 0，
并恢复全部 4/4 个严格离线对象。Recovered sidecar 使用 6 次 pull 和 2 次
inline merge，没有 fallback。最初两个 37/39-entry delta 都只使用一个控制请求，
而不是各 16 个 batch。Merge 总耗时从 5,200.463 ms 降至 3,038.567 ms；重启后
首次 repair 从 10.587 秒改善到 9.033 秒；请求 p95 基本稳定在 1,779.222 ms。
详见 `specs/083-repo-catalog-merge-large-data/results.md`。

## Spec 164 可扩展制品路径与当前验收边界

可扩展路径已经实现为 `artifact-manifest-v2`，其责任边界是：

```text
应用 / NDNSF-DI
  -> ArtifactRepositoryApi
  -> 普通 NDNSF Collaboration 控制
  -> 选中的 Repo providers
  -> 分段 NDN Data 传输
  -> 权威 payload store 与 metadata store
```

NDNSF 负责 Request/ACK/Selection 授权、NAC-ABE 路由、一次性 token、重放保护和
Collaboration 生命周期。Repo 负责制品能力协商、lease、manifest、分段传输、副本
receipt、原子可见性、断点续传、恢复与垃圾回收。NDNSF-DI 提供模型相关制品身份并
调用公开 Repo API；模型分割和推理调度不属于 Repo。

公开应用表面同时支持同步、异步与可恢复 session：

```python
published = repo.publish_file(
    source,
    name="/models/qwen/stage-0",
    expected_sha256=digest,
    replicas=3,
)

fetched = repo.fetch_file(
    published.reference,
    destination,
    verify=True,
)

upload = repo.begin_upload(descriptor)
upload.upload_file(source)
published = upload.commit()
```

`ArtifactReference` 是不可变复用身份：逻辑名称、完整内容 digest、字节数、已签名
root manifest 名称、publisher identity 和 policy epoch。只有逻辑名称不够。部署
默认控制模式是普通 Collaboration；Targeted 只是已知 provider 优化，不绕过授权、
token、manifest 校验或持久 receipt 检查。

信任组合有意避免为每个 Data packet 做一次公钥验证。一个 publisher 签名的 root
绑定制品身份和内容寻址 manifest 层级；Data packet 与 chunk 使用有界 digest 工作
校验；激活前校验完整 payload digest；每个已提交副本返回认证 receipt。HMAC
receipt 是同一个 NDNSF 授权域内的高效证据，不是相互不信任管理域之间的公开
不可否认性证明。

Payload 以流方式写入内容寻址文件；metadata、journal、lease、receipt 和 catalog
保持事务化。临时与 verified 数据在持久 metadata commit 和 activation 之前都不可见。
启动恢复、精确身份 resume、generation fence、容量预留和 GC ownership 规则共同防止
部分内容、迟到 retry 或无主临时文件被公开。`exact-packet-v1` 继续作为显式兼容格式，
不会被静默解释为 v2。未来 schema generation 或操作员禁止写入时进入只读模式，而不
重写已有 bytes。

### 冻结的 MiniNDN 结果

第一轮冻结 campaign 保留在：

```text
results/spec164-artifact-campaign-20260730T050211Z
campaignId: spec164-artifact-20260730T050307Z
manifestSha256: 1ec7305d0b2f6b563ac7a65bf3858a82d3e64fdee57fab91c3202ec36f3636b2
```

Preflight 的 96 个候选 cell 中 24 个可接受；24 次 warmup 和 120 次 measured 全部
保留；物理瓶颈为 659.613 Mbit/s。120 次 measured 全部通过；一个 warmup 副本超时
仍保留在 ledger 中。

这些证据**没有**关闭 Spec 164 性能验收：

- SC-002 结论不足：64 MiB digest/raw median 为 0.992，但五样本 bootstrap 区间为
  `[0.364, 1.191]`。
- SC-003 失败：r1/c16 与 r3/c4 的 signed/digest median 分别为 0.893 和 0.825。
- SC-007 结论不足，因为没有测量 cold retrieval/read amplification。
- Campaign 测量 NDN Data 传输、信任和持久化，但没有走完整公开 Collaboration 发布
  路径，因此不能建立 SC-004 或完整 request latency。

不可变分析复现命令：

```bash
python3 Experiments/analyze_distributed_repo_artifact.py \
  --campaign results/spec164-artifact-campaign-20260730T050211Z \
  --output-json results/spec164-artifact-campaign-20260730T050211Z/derived-results.json \
  --output-markdown \
    specs/164-distributed-repo-large-artifact-transport/evidence/performance-report.md
```

下一轮 MiniNDN campaign 必须先使用公开 Collaboration 控制路径，消除已测得的
signed-manifest 并发瓶颈，加入 matched cold fetch，并分别统计 payload store、
metadata store、Data wire 与 Interest wire bytes。现有 campaign 是不可变证据，
不得调参、覆盖或删除负面样本。

TigerCluster 只是在本地阻塞项关闭之后的外部有效性门禁。它的大 Qwen 测试必须复用
相同的不可变 `ArtifactReference`、普通 NDNSF Collaboration assignment、Repo
publish/fetch 与持久证据绑定。当前任何 Spec 164 制品结果都不是 TigerCluster 或
大模型验收结果。

### 2026-08-01 TigerCluster Qwen3.6 运维经验

Spec 162 的首次明确授权运行已经在三台 RTX 5000 节点上走过真实的
NDNSF-DI/DistributedRepo 发布路径。Qwen3.6-27B 的三个内容寻址 stage 共
53,792,308,358 bytes；manifest 为
`sha256:cd9bb9c37dd2b7780cf76a2b3080d2b58fa27a4e16b22e5b6f377ee70e50e787`。
这组运行保留了 181527、181528、181530、181531、181532 五个失败 identity；
181532 在第一 token 的 Selection 阶段因 Provider/Core 的 boot epoch 不一致而被
拒绝，不能表述为完整生成 PASS。

这次运行明确了 Repo 观测必须分成四段：

1. NDN chunk 发布；
2. root-manifest 注册与 catalog `ACTIVE` 提交；
3. Provider 获取、校验、内存/GPU 准备与缓存命中；
4. NDNSF-DI 协作、依赖数据流和推理响应。

stage 文件全部出现并不代表 catalog 已经可用。53.79 GB 冷发布在观测中还要
等待约 19–22 分钟才出现 registration record；这是包含调度、控制消息和提交延迟
的诊断观察，不是 Repo 吞吐率。Spec 167 必须用受控 payload、匹配的节点对、冷
发布/冷获取/热复用和独立物理带宽上限来测吞吐，不能把 Qwen 作业总耗时当成 Repo
性能。

故障经验也属于协议边界：容器内只能使用 `/shared/<basename>` 映射后的 campaign
路径；tokenizer 必须来自绑定的 artifact 目录；`user.py` 与
`llm_pipeline_lib.py` 必须是同一封存版本；原生 SIF 缺少符号时必须先修复运行时
闭包。live-003 的 Provider 日志还记录了 Repo STORE 控制选择期间的多次
`ProviderToken mismatch`；即使发布最终完成，也必须把它当作控制面安全/关联诊断，
不能静默忽略。详细 identity、证据路径和修复规则见
`docs/NDNSFDI/tigercluster-qwen36-operational-lessons.md`。

live-006 和 live-007 两个诊断 identity 保留不变：live-006 证明终态
Selection fail-fast 不能对通用 Repo STORE 协作默认启用；live-007 发现封存
运行时缺少 `DISelectionAssignmentV2` 的显式导入。Repo ACK 现在会按 artifact
大小设置有界的 `pending_state_ttl_ms`（保守按 8 MiB/s 估计并增加五分钟，最长一
小时），避免大对象拉取、校验和 finalize 期间 Core 提前清理已消费的
ProviderToken。这只保护请求状态生命周期；Repo 仍然以队列执行，不引入存储锁或
容量 reservation。fix-009 source manifest 为
`ec8212dbe219f492d0e4474837bdb0208fdf2a079ab42c2409a0c0393e65850d`。

后续 identity 仍分别保留：live-009（181538）发现 `AckDecision` 是冻结对象，
`pending_state_ttl_ms` 必须在构造时传入；live-011（181539）证明修正后的 Repo
ACK 和 DI assignment 已覆盖，但必须绑定当前 DI provider 模块；live-012
（181541）暴露错误的 `DIRoleAssignmentV2.artifact` cache-key 访问；live-013
（181543）随后暴露 Selection 阶段重复进行 DI GPU admission。fix-014 对相同
request/attempt/model/roles/backends 绑定复用未过期 offer；live-014（181544）
是该修复后的复验。这些是源码/运行时闭包与准备修复，不是 Repo 吞吐率结论。
