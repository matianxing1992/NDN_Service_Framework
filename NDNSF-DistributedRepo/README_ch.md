# NDNSF-DistributedRepo

NDNSF-DistributedRepo 是一个实验性 C++ 子项目，目标是为 NDNSF 应用提供 distributed、NDN-native、policy-controlled 的 artifact 和 intermediate-data storage layer。

当前实现有意保持较小，但已经同时组织为可复用 C++ API 和 NDNSF service layer：

- `RepoObjectManifest`：可准备签名的一个 stored object metadata。
- `StorageCapability`：repo node 广播的容量和负载信息。
- `PlacementPolicy`：可控 replication requirements。
- `selectReplicas`：在候选 storage nodes 上进行 deterministic placement。
- `InMemoryRepoStore`：示例使用的本地 smoke-test store。
- `RepoNode`：可以在 `ServiceProvider` 上注册 NDNSF services 的 repo node，并提供给 C++ app 和测试使用的本地 `put/get/list/remove` helper。
- `RepoClient`：高层 `put/get/list/remove` helper，以及需要直接控制 `ServiceUser` 的应用可用的 lower-level request helper。
- `py_repoclient`：可复用 repo client/protocol API 的 Python binding。

Service name 可以由应用配置。默认情况下，所有 repo nodes 共享：

```text
/NDNSF/DistributedRepo
```

操作类型放在 request payload 中，而不是放在 service name 中。当前操作包括：

```text
CAPABILITY
STORE
STORE_FROM_NDN
STORE_MANIFEST
FETCH
FETCH_PREPARE
MANIFEST
INVENTORY
DELETE
```

对象由 `RepoObjectManifest` 描述，其中包含 object name、object type、SHA-256、size、segment count、replication factor、selected replica nodes 和 policy epoch。Large object transfer 通过 NDNSF Python binding 直接使用 ndn-cxx Segmenter 和 SegmentFetcher；placement 和 replica selection 使用 NDNSF service discovery 与 ACK metadata。这样 repo 保持通用：payload 可以是 model shard、runner、ONNX file、PyTorch artifact、activation tensor、payment-workflow record、telemetry log、JSON configuration，或其它任何 NDNSF application object。

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
manifest = repo.store(
    object_name="/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo",
    payload=payload,
    object_type="binary-blob",
    replication_factor=2,
    policy_epoch="/Policy/example/v1",
)
payload = repo.fetch(manifest.object_name)
```

`store_artifact(...)` 等 AI-specific helper 属于 NDNSF-DistributedInference。NDNSF-DistributedRepo 只存储 opaque named objects。

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

当前实现存储的是重新组装后的 object payload。未来较低层优化可以存储收到的 `ndn::Data` segments 的 wire encoding，并在匹配 Interest 时原封不动返回这些 Data packets。这样可以在 cache hit 时避免重新分段，但需要在当前 object API 下方暴露 packet-level segmented-data API。

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
