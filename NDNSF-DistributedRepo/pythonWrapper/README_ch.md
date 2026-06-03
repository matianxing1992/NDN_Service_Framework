# py-repoclient

`py-repoclient` 是可复用 NDNSF-DistributedRepo client/protocol API 的 Python binding。它保持 application neutral：暴露 repo manifest、placement decision、request encoder，以及一个可以与 NDNSF Python `ServiceUser` 一起使用的小型 Python `RepoClient` helper。

在构建 NDNSF 和 NDNSF-DistributedRepo 后安装：

```bash
cd NDNSF-DistributedRepo/pythonWrapper
python3 -m pip install -e .
```

从仓库根目录，也可以在 DistributedRepo build 后一起安装：

```bash
./waf
python3 -m pip install -e NDNSF-DistributedRepo/pythonWrapper
```

最小用法：

```python
from ndnsf_distributed_inference import DistributedRepo

repo = DistributedRepo.from_repo_config(
    controller="/example/repo/controller",
    user="/example/repo/user",
    group="/example/repo/group",
    trust_schema="examples/trust-schema.conf",
)
manifest = repo.put(
    "APP/Generic/BinaryBlob/demo",
    b"...",
    object_type="binary-blob",
    replication_factor=2,
    policy_epoch="/Policy/app/v1",
)
payload = repo.get(manifest.object_name, manifest)
```

高层 API 会把相对 object suffix 展开到 publisher namespace 下，例如 `/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo`。Repo 存储 app 创建并签名后的 Data segments，把它们当作 opaque bytes；它不会对已加密或明文 application data 再加第二层加密。这些 Data packet names 必须保持在 publisher identity 下，这样 hierarchical trust schema 才能用 trust-root certificate anchor 验证它们。

Namespace 和 trust 规则是：

- `/NDNSF/DistributedRepo` 是共享 service name，不是 stored-data prefix；
- stored objects 和 payload segments 位于 publisher identity 下；
- deployment config objects 位于 controller publisher identity 下；
- project root 应该是 `/example/repo` 这样的父 namespace；
- child certificates 必须在这个 root namespace 下签发，例如 `/example/repo/user/KEY/.../ROOT/...`。

`DistributedRepo.from_config("repo_policy.yaml")` 仍然适合已经在本地有 policy file 的测试和 deployment-side 工具。

只有当 application 已经拥有 NDNSF `ServiceUser` 时，才需要使用 lower-level `py_repoclient.RepoClient`：

```python
from ndnsf import ServiceUser
from py_repoclient import RepoClient

user = ServiceUser(...)
repo = RepoClient(user, "/NDNSF/DistributedRepo")

manifest = repo.insert(
    object_name="/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Model/Shard/0",
    payload=b"...",
    object_type="model-shard",
    replication_factor=2,
    policy_epoch="/Policy/app/v1",
)
payload = repo.fetch(manifest.object_name)
```

对于 large objects 或 APP 自己签好的 `Data` segments，NDNSF-DistributedInference 等高层包仍然可以使用 `ndnsf` 的 lower-level segmented Data APIs，并只把 `py-repoclient` 用于 manifest/protocol 和 repo service operations。
