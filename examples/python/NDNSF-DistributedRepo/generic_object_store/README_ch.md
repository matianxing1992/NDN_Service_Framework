# Generic NDNSF-DistributedRepo Example

这个示例验证 NDNSF-DistributedRepo 是通用 object store，而不是 AI artifact 专用 API。它通过同一个共享 service name `/NDNSF/DistributedRepo` 存储和获取三种互不相关的对象类型：

- JSON configuration
- telemetry log
- binary blob

每个对象都按自己的 replication factor 进行复制。Client 使用高层 generic object API。在一个运行中的 NDNSF 部署里，repo node 会把部署配置作为普通 repo object 预加载。应用用户只需要 repo service 的 bootstrap 参数，然后先通过同一个 NDNSF API 从 repo 获取配置：

```python
repo = DistributedRepo.from_repo_config(
    controller="/example/repo/controller",
    user="/example/repo/user",
    group="/example/repo/group",
    trust_schema="examples/trust-schema.conf",
    config_object_name="/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml",
)
manifest = repo.put("APP/Generic/BinaryBlob/demo", payload,
                    object_type="binary-blob", replication_factor=3)
payload = repo.get(manifest.object_name, manifest)
```

`repo.put(...)` 接收 application-relative suffix，并把它发布到 user namespace 下面，例如 `/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo`。Repo 存储的是应用创建并签名后的 Data segments。它不会额外再加一层加密；无论 app payload 已加密还是明文，repo 都把它当作 opaque bytes，然后分段并签名。

Config object 由 deployment side 在 `repo_node.py` 启动时创建。它也是普通 repo object，所以 object name 位于 controller publisher namespace 下。应用用户在调用 API 前不需要手写这个配置文件。对于本地测试和离线工具，`DistributedRepo.from_config("repo_policy.yaml")` 仍然可用。

这个示例故意不包含 model 或 artifact 语义。

## Namespace Design

应用数据由发布者命名，而不是由 repo service 命名。Repo service name 保持共享和稳定：

```text
/NDNSF/DistributedRepo
```

Object name 之所以全局唯一，是因为高层 API 会把相对应用 suffix 展开到 publisher identity 下面：

```text
repo.put("APP/Generic/BinaryBlob/demo", payload)
  -> /example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo
```

部署配置对象遵循同一条规则。它发布在 controller identity 下面：

```text
/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml
```

Payload segments 是原始 publisher namespace 下的 Data packets，例如：

```text
/example/repo/user/NDNSF-DISTRIBUTED-REPO/UPLOAD/DATA/<digest>
```

Repo 不会把数据改名到 `/NDNSF/DistributedRepo` 下，也不会包一层新的加密。如果 application 已经加密 payload，repo 存的是加密 bytes；如果 application 给的是明文，repo 存的是明文 bytes。两种情况下，repo client 都只负责把 Data packets 分段并签名，然后 repo nodes 存储这些 segments。

## Trust Schema Design

MiniNDN 部署使用 project root identity：

```text
/example/repo
```

它签发这些 child identities：

```text
/example/repo/controller
/example/repo/user
/example/repo/provider/repoA
```

这样满足 hierarchical parent-child trust，因为所有被签发的证书都仍然位于 project root namespace 下。像 `/example/repo/root` 这样的 root identity 去签 `/example/repo/user` 并不是 user namespace 的父节点，所以示例不使用这种模式。

生成的 trust schema 遵循这些规则：

- stored object Data 必须位于 publisher identity 下；
- NDNSF runtime Data 和 SVS sync Data 必须位于 signer identity 下；
- child certificates 必须位于 parent certificate namespace 下；
- production deployment 应该把 validation anchor 放在 project trust-root certificate 上，而不是使用 `type any`。

从仓库根目录在 MiniNDN 中运行：

```bash
sudo -E PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  python3 Experiments/NDNSF_DistributedRepo_Generic_Minindn.py
```

期望成功标记：

```text
GENERIC_DISTRIBUTED_REPO_MININDN_OK
```
