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

## Persistent Repo Catalog Gossip

MiniNDN smoke 会启动三个 Persistent repo node，并在每个 repo 旁边启动一个小型
`catalog_sync.py` sidecar。Sidecar 会周期性向 peer Persistent repos 请求
`CATALOG_DELTA`，然后通过 `CATALOG_MERGE` 把返回的 entries 合并到本地 repo。

这个 smoke 有意使用 10 秒同步周期。更短的全互联周期会产生太多 NDNSF service
requests，并拖慢正常 repo 操作；这正是 catalog exchange 应该保持 object-level 和
delta-based，而不是广播每个 segment 或完整目录的原因。

在存储 JSON config、telemetry log、binary blob 和 app-signed Data packet objects
之后，client 会等待 catalog 传播，并向每个 Persistent repo 请求
`CATALOG_SNAPSHOT`。只有每个 snapshot 都包含所有已存对象时，smoke 才算通过。

Tombstone 也是同一个 catalog control plane 的一部分。当某个 repo 删除 object 时，它会发布
带有更新 catalog epoch 的 `DELETED` entry。Peer repos 必须保存这个 tombstone，并让它
shadow 更旧的 `AVAILABLE` entries，这样旧 catalog delta 不会把已删除对象复活。MiniNDN
smoke 包含一个专门的 tombstone gossip 检查来验证这件事。它还会在 tombstone 传播后
故意注入一个 catalog epoch 更高、但 object update time 更旧的 `AVAILABLE` entry；对象
必须继续保持删除状态，因为 tombstone 排序依赖 object 更新时间和删除语义，而不只是
peer catalog sequence。

## Object Classes 和 Retention Policy

Repo object 除了 application `objectType` 之外，还携带 `objectClass` 和生命周期元数据。
当前默认 class 是：

```text
temporary-activation  min=1 max=1 repair=false ttl=10min
model-artifact        min=2 max=3 repair=true  ttl=none
uav-recording         min=2 max=3 repair=true  ttl=7d
telemetry-log         min=1 max=2 repair=true  ttl=7d
mission-log           min=2 max=3 repair=true  ttl=30d
```

这些默认值只描述 catalog 和 repair 行为，不改变 NDN object name、签名、加密或
segmented Data 存储方式。应用需要不同策略时，仍然可以给 generic object 显式设置
replication 参数。Catalog lookup 会把过期对象标记为 `EXPIRED`；过期对象即使所在
class 默认允许 repair，也不会进入 repair plan。这样短生命周期 activation 或临时数据
产品不会在有效期结束后又被复制。

Generic MiniNDN regression 还会存储 UAV 风格的数据产品：

```text
uav-recording
telemetry-log
mission-log
```

测试会确认它们以预期 object class metadata 进入 catalog，并且 client 能 lookup 和 fetch
回原始 payload。这是 UAV recording/log 产品的 repo-level browsing prototype；完整 GS UI
仍然和这个 repo control-plane 测试分开。

## Repair Plan 和手动 Repair Action

Catalog lookup 和 snapshot response 会暴露每个 object 的控制面健康状态。当某个 object
的 live replicas 少于配置的 `minReplicationFactor` 时，catalog 会把它标记为
under-replicated，并附带一个 `repairPlan`。这个 plan 会列出保守的候选 action：

- `schemaVersion: 1`；
- `actionType: copy-replica`；
- object name、object hash 和 manifest hash；
- live source Persistent repo；
- target Persistent repo；
- 配置的 min/max replication factors。

Python control path 会先用 `RepoRepairAction` 校验这个 schema，然后 sidecar 才能执行
repair。旧 action dict 如果没有 `schemaVersion` 和 `actionType`，仍会按 version-1
`copy-replica` action 兼容处理；但新生成的 catalog response 会显式包含这两个字段。

默认情况下，sidecar 不会执行这些 action。它只会在某个 repair action 的 target 是本地
repo 时打印 warning。这样 catalog synchronization 在部署测试阶段更安全，也避免系统
悄悄进行后台复制。

如果希望某个 sidecar 执行指向自己的 repair actions，可以在配置中设置：

```yaml
repo_control_plane:
  repair:
    auto_execute: true
```

也可以启动 `catalog_sync.py` 时加入 `--auto-repair`。如果配置中启用了 repair，但本次
运行希望强制只报警，可以使用 `--no-auto-repair`。

Repair execution 由 client/sidecar path 编排，而不是让 provider 在处理请求时递归调用另一个
repo provider。Sidecar 会先通过 `FETCH_PREPARE` 准备 source object，校验 object hash，
发布 packet manifest，然后让 target repo 通过 `STORE_PACKET_PULL` 拉取并保存已签名的
Data packets。Target repo 只保存 opaque signed Data packets 并更新 catalog entry；
它不会解密或重新解释 object。

MiniNDN health smoke 会用下面两个 marker 验证这条路径：

```text
GENERIC_DISTRIBUTED_REPO_CATALOG_REPAIR_OK
GENERIC_DISTRIBUTED_REPO_CATALOG_HEALTH_OK
GENERIC_DISTRIBUTED_REPO_AUTO_REPAIR_OK
```

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
GENERIC_DISTRIBUTED_REPO_CATALOG_GOSSIP_OK
GENERIC_DISTRIBUTED_REPO_OBJECT_POLICY_OK
GENERIC_DISTRIBUTED_REPO_TOMBSTONE_GOSSIP_OK
GENERIC_DISTRIBUTED_REPO_TOMBSTONE_EPOCH_CONFLICT_OK
GENERIC_DISTRIBUTED_REPO_UAV_DATA_PRODUCT_OK
GENERIC_DISTRIBUTED_REPO_CATALOG_REPAIR_OK
GENERIC_DISTRIBUTED_REPO_AUTO_REPAIR_OK
GENERIC_DISTRIBUTED_REPO_OK
GENERIC_DISTRIBUTED_REPO_MININDN_OK
```
