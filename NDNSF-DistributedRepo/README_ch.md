# NDNSF-DistributedRepo

NDNSF-DistributedRepo 是 NDNSF 的 C++ Repo 与不可变制品存储模块。Repo 的
写入、持久化、授权服务、receipt、location、availability 和恢复状态由 C++ 实现
并作为唯一事实来源。

## 当前运行边界

| 用途 | 当前入口 |
|---|---|
| Persistent 网络 Repo 服务 | C++ `DistributedRepoServer`；生产 service handler 注册在 C++ `RepoNode` |
| 可信进程内的 generic object Repo | C++ `RepoNode::registerLocalServices` 和 `LocalServiceRegistry` |
| 应用拥有的本地制品存储 | C++ `ArtifactRepositoryApi::makeInApp`，或 Python `ArtifactRepositoryApi.make_in_app` |
| 授权 Persistent 制品操作 | C++ `ArtifactRepositoryApi::makePersistent`，或 Python `ArtifactRepositoryApi.make_persistent` |
| 旧数据库迁移 | 显式运行 `ndnsf-repo-import`；服务启动不会自动迁移 |

Persistent 网络服务的默认 service prefix 是
`/NDNSF/DistributedRepo/Object/v1`。服务使用独立的 Repo Provider identity、
Controller authority 和 trust schema。generic object manifest 与 payload range
保存在数据库基准路径追加 `.store` 得到的文件系统目录中；制品 receipt、
location 和 operation 记录保存在追加 `.artifact-v2.sqlite` 的 SQLite 数据库中。
例如 `--database /var/lib/ndnsf-repo/repo-A` 会使用
`/var/lib/ndnsf-repo/repo-A.store` 和
`/var/lib/ndnsf-repo/repo-A.artifact-v2.sqlite`。`--cache-bytes` 必须为正数，
服务端会在配置校验时拒绝零缓存预算。Python 不提供 Repo server、network backend、
replica placement、receipt store 或 availability/retry engine。

Generic object 操作由 C++ `RepoNode` 提供。In-App 与 Persistent 是不同的生命周期：
应用可以先在自己的 durable root 中提交本地副本，再显式把同一个
`ArtifactHandle` archive 到选定的 Persistent Repo。仅有本地副本时不会向远端
发送 STORE，也不会生成 Persistent receipt。

## Python 制品 API

Python 包 `py_repoclient` 是 C++ API 的薄绑定。它只做路径转换、native 返回值
投影和异常映射；reference、handle、receipt、提交状态、持久化与网络处理都来自
同一个 C++ owner。

```python
import hashlib
from pathlib import Path

from py_repoclient import ArtifactReference, ArtifactRepositoryApi

source = Path("model.onnx")
payload = source.read_bytes()

reference = ArtifactReference(
    "/example/models/model-v1",
    hashlib.sha256(payload).hexdigest(),
    len(payload),
    "/example/manifests/model-v1",
    "/example/provider",
    "policy-epoch-1",
)

api = ArtifactRepositoryApi.make_in_app(
    durable_root="/var/lib/ndnsf-artifacts",
    owner_id="/example/provider",
)
published = api.publish_file(source, reference)
assert published.handle is not None
assert published.receipt is not None

api.fetch_file(published.reference, Path("model.onnx.copy"))
```

`ArtifactPublishResult`、`ArtifactHandle`、`ArtifactCommitReceipt`、
`ArtifactFetchResult` 和其他 Repo value 都是 C++ native types。Python wrapper 不
维护第二份状态。Network calls 使用 Persistent API：

```python
api = ArtifactRepositoryApi.make_persistent(service_user, client_config)
```

Persistent API 借用 `ServiceUser` 的 native owner，并在 API 存活期间保留该 owner。
Face event loop 必须在另一个线程运行；同步等待会释放 Python GIL。超时限制调用方的
等待时间，不代表取消网络操作。销毁顺序是先 API，再停止并销毁 ServiceUser。

可执行示例位于：

- [Python In-App 发布](../examples/python/NDNSF-DistributedRepo/artifact_api/publish_file.py)
- [Python In-App 获取](../examples/python/NDNSF-DistributedRepo/artifact_api/fetch_file.py)
- [C++ Persistent 服务](apps/DistributedRepoServer.cpp)

## 兼容性边界

Spec 192 已移除 Python-owned Repo runtime。`RepoClient` 保留为
`ArtifactRepositoryApi` 的薄 Persistent facade；它要求调用方提供原生
`ArtifactReference` 和显式 `SelectedRepoSet`，并沿用 `ack_timeout_ms=1000`、
`timeout_ms=30000` 默认值。以下其他旧 API 保留 import 名称，并在调用时抛出带有
`SPEC192_COMPATIBILITY_V1` 的 `RepoCompatibilityError`：

- `RepoNodeApp`、`NetworkDistributedRepoClient`
- `FilesystemArtifactApiBackend`、`CollaborationArtifactApiBackend`
- Python upload/fetch session、async callback、Python server handler
- Python placement、receipt/location/status、journal、availability 和 retry state

请迁移到 `ArtifactRepositoryApi` 的 In-App/Persistent factory，以及 C++
`DistributedRepoServer`。这个错误明确标记已移除的 Python 行为，不会提供备用的
Python 实现。

## Legacy importer

`ndnsf-repo-import` 是离线、显式、一次性的迁移工具。它只接受受支持的固定 schema
fingerprint；未知 schema、损坏的 signed Data、digest 不匹配或目标冲突都会 fail closed。
导入成功后可从 native In-App store 或 C++ RepoCore 重新读取对象。服务启动不会运行
importer，源数据库和 payload 也会保留供审计。

更多 C++ API、namespace/trust schema 和历史实验说明见英文
[README](README.md)。历史 MiniNDN 或 Python campaign 记录只描述当时的候选版本，不
代表 Spec 192 当前验收结果。
