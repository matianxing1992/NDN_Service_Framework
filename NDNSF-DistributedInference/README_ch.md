# NDNSF-DistributedInference

NDNSF-DistributedInference 是构建在通用 NDNSF Python API 之上的 Python 库。它为分布式 AI 推理提供更高层 API，但不会把 AI-specific semantics 加入 NDNSF Core。

分层结构：

```text
APP
  calls model/inference APIs

NDNSF-DistributedInference
  understands model plans, roles, stages, shards, runtime artifacts,
  backend requirements, and inference dependencies

NDNSF-DistributedRepo
  stores model/runtime/intermediate objects with controlled replication,
  repo manifests, storage capabilities, and placement policy

NDNSF Python Wrapper
  exposes generic service invocation, collaboration, encrypted artifacts,
  segmented large data, and provider callbacks

NDNSF Core
  handles Face, SVS, NAC-ABE, signing, permissions, selection, workers,
  and wire protocol behavior
```

## Application-Level API

推荐应用开发者使用 `APPClient`、`APPProvider`、`APPController` 和 `APPDeployment`。这些名字隐藏了 Face、SVS、trust schema files、permission Interests 和 artifact Data names 等 NDN-specific concepts。APP developer 只需要描述：

```text
service name
model parts / roles
runtime backend
local handler logic for each provider role
```

Distributed-inference 层会把这些描述映射到 NDNSF service invocation、artifact publishing、role assignment、encrypted shared scopes 和 provider callbacks。

每个 service name 都是唯一的，并且对应一个确定的 model layout：一个 model identity、一个 role set 和一个 dependency graph。如果同一个模型用不同方式切割，应发布为不同 service name。因此 dependency graph 位于 deployment config 中，而不是放在每个 request 中。

在下面的例子里，`APPClient` 是应用侧的高层 client facade。它不是手工创建 Face、SVS group 或 permission fetcher；这些底层 NDNSF runtime 对象都由配置文件派生出来。

`yolo_policy.yaml` 是 deployment policy。它可以由 model splitter 生成，也可以由应用部署工具生成。这里的 model splitter 指部署前运行的模型切分工具：它读取原始模型或模型描述，决定模型要被拆成哪些 stage/shard、每个 role 需要哪个 artifact、role 之间按什么 dependency graph 交换中间结果，然后输出 NDNSF-DI 能读的 policy YAML。YOLO 示例提供了这样的 splitter 脚本；如果是新的模型家族，应用或框架开发者可以写自己的 splitter，只要输出标准的 service、roles、dependencies 和 artifacts 描述即可。

`APPClient.from_config("yolo_policy.yaml")` 会读取这个 YAML，生成对应的 trust schema、controller policy 和 service manifest，然后用这些生成物连接到底层 `DistributedInferenceClient`。

Client 侧：

```python
from ndnsf_distributed_inference import APPClient

def encode_image_for_yolo(image_tensor) -> bytes:
    # Application-defined encoding that follows client.input_contract(service).
    return serialize_npz(images=preprocess_for_yolo(image_tensor))

client = APPClient.from_config("yolo_policy.yaml")
service = "/AI/YOLO/SplitInference"
print(client.input_contract(service))

client.register_input_encoder(service, encode_image_for_yolo)
result = client.infer_service_object(service, image_tensor)

# 可以同时提交多个请求。每个请求仍然使用 NDNSF runtime 完成
# Face/SVS/NAC-ABE 工作；APP 线程收到的是 Future。
future = client.infer_service_object_async(service, image_tensor)
result = future.result(timeout=30)
```

推荐的用户侧入口是 `infer_service_object(...)`：调用者给出 service name
和应用对象，例如 `image_tensor`，NDNSF-DI 通过注册到该 service 的 encoder
把它转换为请求 bytes。service name 是一个有约束的应用契约，而不是随便起的字符串：
`/AI/YOLO/SplitInference` 在 `yolo_policy.yaml` 中绑定到一个确定的模型
identity、模型版本、输入/输出编码、role set、dependency graph、provider
identity 和 security policy。换句话说，client 不需要在每次请求里重新
说明模型怎么切、有哪些 stage、谁依赖谁；这些都由 service name 查到。

`client.input_contract(service)` 返回 policy 中记录的输入契约，例如 codec、
字段名、shape、dtype 和推荐 encoder 名称。`register_input_encoder(...)`
把应用对象到 bytes 的转换函数注册到这个 service 上。这个 encoder 是
应用代码的一部分，可以由 model package、SDK 或示例库提供，也可以由
应用开发者按 `input_contract` 自己实现；之后用户可以调用
`infer_service_object(...)`，直接传 `image_tensor` 这样的应用对象。如果
调用者已经有编码好的 bytes，才需要直接使用较低层的
`infer_service(service, encoded_payload)`。

`encoded_payload` 本质上是这个 service 契约要求的请求 bytes。它由应用层
编码函数生成。仓库里的 YOLO 示例实现叫
`yolo_split_lib.encode_initial_request(image_tensor)`，但这只是示例的
具体 helper；通用 API 只要求注册一个“应用对象 -> bytes”的 encoder。
NDNSF-DI 不解释这些 bytes 的语义，只负责把它们安全送到对应 service
的分布式执行流程。Provider handler 和 client 必须对同一个 service 使用
同一套 payload schema；如果输入 shape、dtype、预处理方式或模型切分发生
不兼容变化，应发布为新的 service name 或新的模型版本，而不是复用旧名字。
这个路径适合模型和 runtime 已经部署在 provider 上的服务。

Model/runtime artifacts 属于部署阶段。Splitter 或 deployment tool 应先把
artifact paths 写入 service policy；provider 启动后按自己的 role 从
policy 找到本地 artifact。Client 请求阶段不再发布模型 shard。

Provider 侧：

```python
from ndnsf_distributed_inference import APPProvider

provider = APPProvider.from_config("yolo_policy.yaml", provider_id="A")
provider.serve(
    service="/AI/YOLO/SplitInference",
    roles="all",
    handler=handle_assigned_role,
    backends=["onnxruntime"],
    temp_dir="/tmp/ndnsf-yolo-provider-A",
    has_model=True,
    can_provision=False,
)
provider.run()
```

Provider 使用 service policy 中记录的 artifact paths。没有本地 artifact
的 provider 不应声明 `has_model=True`；模型发布和放置应在 provider 启动前
由 splitter/deployment 流程完成。

Provider-side Python handler 也可以使用单独 worker pool：

```python
provider = APPProvider.from_config(
    "yolo_policy.yaml",
    provider_id="A",
    handler_workers=4,
)
```

NDNSF callback 会等待 worker result，以保证 collaboration context 仍然有效；昂贵的 Python model logic 会在 NDNSF callback 函数外运行。

Controller 侧：

```python
from ndnsf_distributed_inference import APPController

controller = APPController.from_config("yolo_policy.yaml")
controller.run()
```

Deployment-only utilities：

```python
from ndnsf_distributed_inference import APPDeployment

deployment = APPDeployment.from_config("yolo_policy.yaml")
print(deployment.trust_schema)
print(deployment.policy_file)
```

多服务部署中，对每个 service 调用一次 `provider.serve(...)`，并使用 `client.infer_async(...)` 并发请求一个或多个 service。service name 仍然决定每个 request 使用的固定 role set 和 dependency graph。

## Example Families

当前仓库在 `examples/python/NDNSF-DistributedInference/` 下包含三组 Python 示例：

```text
yolo_split/
  Two-stage real Ultralytics YOLO split inference over ONNX Runtime.

yolo_2x2/
  Four-provider real Ultralytics YOLO 2x2 split inference，使用独立 repo 节点。
  Splitter 导出四个真实 ONNX chunks：Stage 0 内两个顺序 shards，Stage 1
  内两个顺序 shards。同一 stage 内的 shards 会先交换 activation references，
  再由下一个 shard 继续计算。Controller-side deployer 在推理前把
  model/runtime artifacts 存入 repo。User 只携带 repo manifest references；
  providers 在第一个 command 获取被分配 role 需要的 artifacts，第二个
  command 复用 provider artifact cache。

pytorch_eager_2x2/
  Four-provider PyTorch eager example. It splits a random fully connected
  network into two stages and two shards per stage, then verifies the
  distributed result against a local full-model reference.
```

当模型可以导出为 ONNX 时，ONNX 示例代表推荐的 portable deployment path。PyTorch eager 示例代表最灵活的 Python runtime path，适合 dynamic control flow、custom Python logic 和 research prototypes。

较低层的 `DistributedInferenceClient`、`DistributedInferenceProvider` 和 `DistributedInferenceController` 仍然保留，供 framework developer 和需要直接控制的实验使用。

## DistributedRepo Integration

Model shard、runtime bundle 和 intermediate data 不应随意 push 到任意节点。NDNSF-DI 可以携带由 NDNSF-DistributedRepo 生成的 repo object manifests：

```python
from ndnsf_distributed_inference import (
    LocalDistributedRepo,
    PlacementPolicy,
    StorageCapability,
)

repo = LocalDistributedRepo([
    StorageCapability("/repo/A", free_bytes=4_000_000_000,
                      recent_load=0.1, failure_domain="rack-a"),
    StorageCapability("/repo/B", free_bytes=4_000_000_000,
                      recent_load=0.2, failure_domain="rack-b"),
])

manifest = repo.put(
    object_name=(
        "/example/hello/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/"
        "NDNSF-DI/ARTIFACT/AI/YOLO/2x2/Stage/0/Shard/0/model"
    ),
    payload=model_bytes,
    object_type="onnx-model",
    policy=PlacementPolicy(replication_factor=2),
    policy_epoch="/Policy/yolo-2x2/v1",
)

payload = repo.fetch_object(manifest.object_name, manifest)
```

Manifest 记录 object hash、size、replication factor、selected repo nodes，以及保存 object segments 的 signed Data names。Object name 仍然按发布者命名空间组织：controller 发布的 artifacts 使用 controller object namespace，user 发布的 inputs/intermediates 使用 user namespace，provider 输出使用 provider namespace。实际服务 stored segments 的 Data name 可以是 repo-owned prefix，例如 `/repo-node/NDNSF-DISTRIBUTED-REPO/DATA/<object-hash>`，这样 fetcher 可以直接路由到被选中的 repo node。当前 networked path 中，controller-side deployer 会在推理前把 model/runtime artifacts 存入 repo node。User request 只携带 execution specs 和 repo manifest references；被选中的 providers 自己从 repo 获取被分配 artifacts，并在本地缓存。

DI 代码读取 model artifacts、runtime bundles、images 或 activation objects 时，应该优先使用 manifest-aware object API，也就是 `fetch_object()` / `get_object()`。这个 API 返回一个经过 size/hash 校验的逻辑对象，并隐藏 repo 内部到底是一个 Data packet、多个 segmented Data packets，还是 replicated object。Planner 和 dependency graph 因此只需要表达 object references 和 manifests，不应该混入 repo segment names。

## Lower-Level API Sketch

User 侧：

```python
from ndnsf_distributed_inference import (
    DistributedInferenceClient,
    load_or_generate_deployment,
)
from ndnsf import CollaborationRole

deployment = load_or_generate_deployment("yolo_policy.yaml", "/tmp/yolo-policy")
service = deployment.service_policy("/AI/YOLO/SplitInference")
client = DistributedInferenceClient.connect(
    group=deployment.group,
    controller=deployment.controller,
    user=deployment.user,
    trust_schema=deployment.trust_schema,
)
request_payload = encode_image_for_yolo(image_tensor)
graph = deployment.dependency_graph_for_service(service.name)
result = client.infer_deployed_service(
    service.name,
    request_payload,
    roles=[CollaborationRole(role=role, service=service.name)
           for role in service.roles],
    key_scopes=graph.key_scopes(),
    dependencies=list(service.dependencies),
    role_scopes=graph.role_scopes(),
)
```

Provider 侧：

```python
from ndnsf_distributed_inference import (
    DistributedInferenceProvider,
    load_or_generate_deployment,
)

deployment = load_or_generate_deployment("yolo_policy.yaml", "/tmp/yolo-policy")
inference = DistributedInferenceProvider.create(
    provider_id=deployment.provider_id_for_role("/Stage/0"),
    group=deployment.group,
    controller=deployment.controller,
    provider_prefix=deployment.provider_prefix,
    trust_schema=deployment.trust_schema,
)
inference.add_role("/AI/YOLO/SplitInference", "/Stage/0", handle_stage0)
inference.run()
```

Controller 侧：

```python
from ndnsf_distributed_inference import (
    DistributedInferenceController,
    load_or_generate_deployment,
)

deployment = load_or_generate_deployment("yolo_policy.yaml", "/tmp/yolo-policy")
controller = DistributedInferenceController.create(
    controller_prefix=deployment.controller,
    policy_file=deployment.policy_file,
    trust_schema=deployment.trust_schema,
    bootstrap_identities=deployment.bootstrap_identities,
)
controller.run()
```

APP、model publisher 或 model-splitting tool 拥有 semantic service definition：模型如何切割、有哪些 roles、每个 role 发布或等待什么、需要什么 runtime/backend。NDNSF-DistributedInference 不要求所有模型都用同一种 dependency generation 机制。它可以接受手写 splitter、PyTorch-specific splitter、ONNX analyzer、container-bundle planner 或未来 optimizer 生成的 dependency graph。运行时承载 service config 中记录的 dependency graph，并把 plan 转换为通用 NDNSF collaboration calls 和 artifact provisioning。

## Dependency Graph Generation Roadmap

分布式推理部署中需要区分三种图：

```text
Model dependency graph
  原始模型内部的 operator/tensor DAG，例如 ONNX graph。

Chunk collaboration graph
  模型切分后的 provider-role graph。每条边记录哪些 activation tensors
  从一个 chunk 传给另一个 chunk。

Deployment plan
  roles 到 providers 的映射、runtime artifacts、repo manifests、security
  policy 和 NDNSF service names。
```

当前 policy format 把 chunk collaboration graph 放在
`services[].dependencies` 中。对于非 ONNX 模型，这仍然可以由
model-specific splitter 或 application planner 提供。对于 ONNX 模型，
NDNSF-DI 提供了可选的 `onnx_graph` helper module：

```text
ONNX graph
  -> tensor/operator dependency DAG
  -> candidate split points and boundary tensor costs
  -> exported ONNX chunks
  -> chunk-level dependencies with tensor names
  -> NDNSF-DI collaboration plan
```

这个 helper 是可选的。它不会替代 `SplitterOutput`，也不会把 NDNSF-DI
变成只支持 ONNX 的框架。它只是为 ONNX deployment 提供自动规划的共同起点，同时保留同一套 policy format 给 PyTorch eager、model-specific 和 containerized workloads。

YOLO 2x2 splitter 现在会在导出的 chunks 旁边写一个 ONNX graph summary
JSON。这个文件有三个顶层部分：

```text
fullModel
  原始导出的 ONNX graph：inputs、outputs、initializers、nodes、
  tensor producers、tensor consumers，以及静态 tensor shape/size metadata。

splitCandidates
  按 unknown boundary tensors、known boundary bytes、boundary tensor 数量和
  cut 位置排序的候选顺序切割点。这些只是 planning hints；当前 YOLO splitter
  仍然使用 model-specific 逻辑选择并导出实际 chunks。

plannerRecommendations
  根据 provider compute score、估计 RTT/bandwidth、activation size 和
  compute-balance cost 排序的 candidate/provider assignment。这样 graph
  analysis 会变成 planning input，但不会改变 `SplitterOutput`。

chunkGraph
  实际导出的 chunks，以及每个已选 chunk boundary 上传递的 tensor 名字。
```

当前 YOLO 2x2 示例的默认 planner 会有意把 providers 当成同质节点。这样重点
放在分布式推理机制本身：真实 graph analysis、activation boundaries、artifact
provisioning 和 multi-role execution。运行时 provider profiling 是后续扩展。

如果要做实验，splitter 也可以接受可选的粗粒度 provider profile JSON：

```json
{
  "providers": [
    {
      "name": "/example/hello/provider/A",
      "compute_score": 1.0,
      "uplink_mbps": 200,
      "downlink_mbps": 200,
      "rtt_ms": 20
    },
    {
      "name": "/example/hello/provider/B",
      "compute_score": 1.0,
      "uplink_mbps": 200,
      "downlink_mbps": 200,
      "rtt_ms": 20
    }
  ]
}
```

这只是估计，不是硬性能保证。后续 profiling 可以把这些粗略值替换成真实测量的
provider throughput、model-layer latency、memory pressure 和 link quality。

### 走向真正分布式计算还差的关键工作

当前 NDNSF-DI prototype 已经在做真实的网络级分布式推理：模型可以被切成 ONNX
stages/chunks，providers 之间会交换具名 activation objects，MiniNDN 回归也能验证
端到端结果。下一步不应该继续堆更多互不相关的 demo，而应该集中在三项 framework
level 工作：

1. 从 ONNX tensor DAG 生成更真实的 dependency graph。Analyzer 应该保留 branch、
   skip connection、concat、多输入、多输出等 tensor dependencies，让 chunk
   collaboration graph 反映真实模型图，而不是手写 pipeline approximation。
   当前 `build_chunk_dependencies(...)` helper 已经会把每个导出 chunk 的 ONNX
   outputs 和其它所有 chunks 的 ONNX inputs 做匹配，因此只要 boundary tensor
   names 被保留下来，fan-out/fan-in 依赖就能直接体现在 chunk graph 中。

2. 用 planner 自动生成 2-stage 和 2x2 policies。现有 hand-tuned YOLO policies
   应该逐步变成示例或 fallback；主路径应该是：

   ```text
   ONNX tensor DAG -> candidate split points -> chunk graph -> NDNSF-DI policy
   ```

3. 做对比实验。关键对比包括 single-node inference、2-stage split inference、
   2x2 split inference、不同 activation size、不同 RTT/bandwidth 设置，以及不同
   provider 数量。
   当前 comparison harness 先提供 local full-ONNX baseline，并且可以选择调用
   MiniNDN split 回归：

   ```bash
   python3 Experiments/NDNSF_DI_Compare_Yolo_Plans.py \
     --iterations 5 \
     --output results/yolo_di_comparison/result.json

   sudo -E python3 Experiments/NDNSF_DI_Compare_Yolo_Plans.py \
     --include-minindn-auto-split \
     --output results/yolo_di_comparison/result-with-minindn.json
   ```

Provider scheduling 暂时不是眼前的主要研究瓶颈。Planner 暴露
`ProviderProfile` 和 `homogeneous_provider_profiles(...)` 作为兼容接口。默认情况下，
providers 被当作同质节点处理，这样当前实验可以聚焦分布式推理机制本身。未来 runtime
profiling 可以用真实测量的 compute、memory、latency、bandwidth 和 RTT 值替换这些
默认值，同时不改变 policy format。

生成的 policy 中，dependency 可以包含 `tensors` 字段：

```yaml
dependencies:
  - producers: [/Stage/0/Shard/1]
    consumers: [/Stage/1/Shard/0]
    key_scope: stage0-to-stage1
    topic_prefix: /activation
    tensors: [x, saved_4]
```

这表示 role-level edge 携带一个 large activation object，里面包含列出的 tensors。Request 本身只应携带小 reference。Images、activations、model shards 和 runtime bundles 都通过 segmented NDN Data 传输，可以使用 NDNSF encrypted large-data helper，也可以使用 NDNSF-DistributedRepo manifests。压缩或降低精度可以作为应用层模型质量/带宽 tradeoff，但不应成为大对象传输机制本身。

## Splitter Output Contract

真实 dependency graph 应来自 model splitter。Splitter 通常是 model-family 或 backend specific：YOLO ONNX splitter、transformer pipeline splitter、tensor-parallel LLM splitter 或 container-bundle splitter 可能都有不同逻辑。NDNSF-DistributedInference 只要求 splitter 输出标准 `SplitterOutput`：

```python
from ndnsf_distributed_inference import InferenceDependency
from ndnsf_distributed_inference.splitter import (
    SplitArtifact,
    SplitServiceSpec,
    SplitterOutput,
)

split = SplitterOutput(
    application="yolo-split-demo",
    controller="/example/hello/controller",
    group="/example/hello/group",
    user="/example/hello/user",
    provider_prefix="/example/hello/provider",
    trust_app_roots=["/example"],
    services=[
        SplitServiceSpec(
            name="/AI/YOLO/SplitInference",
            model_name="/Model/Ultralytics/YOLO/Split",
            roles=["/Stage/0", "/Stage/1"],
            dependencies=[
                InferenceDependency(
                    producers=["/Stage/0"],
                    consumers=["/Stage/1"],
                    key_scope="stage0-to-stage1",
                    topic_prefix="/activation",
                ),
            ],
            artifacts=[
                SplitArtifact(
                    role="/Stage/0",
                    path="yolo-stage0.onnx",
                    artifact_name="/Model/Ultralytics/YOLO/Stage/0",
                    kind="onnx-model",
                    backend="onnxruntime",
                ),
            ],
        ),
    ],
)
split.write_policy_config("yolo_policy.yaml")
```

生成的 YAML 因此是由 split 派生的 deployment policy。这个 split 可以来自 ONNX tensor DAG、PyTorch/model-specific splitter，也可以来自手写 application planner。同一个 service name 应始终映射到同一个 model layout 和 dependency graph。如果模型用不同方式切割，就应发布为不同 service name。

Provider handler 会在 `ctx.dependencies` 中收到 role-local dependency view，因此 handler 代码可以询问当前 role 应该发布或等待什么，而不是手写重复 topic strings：

```python
def handle_assigned_role(ctx):
    if ctx.dependencies.outputs:
        activation = run_local_stage(ctx.execution.path("model"), ctx.request)
        large_name = ctx.publish_output_large(activation)
        edge = ctx.dependencies.output()
        ctx.ndnsf.publish(edge.key_scope, edge.topic("ref"), large_name.encode())

    if ctx.dependencies.inputs:
        edge = ctx.dependencies.input()
        ref = ctx.ndnsf.wait_one(edge.key_scope, edge.topic("ref"), 10000)
```

对于有多个 inputs 或 outputs 的 role，可以显式传入 `key_scope`，例如 `ctx.dependencies.input("stage0-to-stage1")` 或 `ctx.publish_output(payload, key_scope="stage1-internal")`。

## User-Facing Security Config

应用开发者不需要手写 NDN validator trust schema。他们使用 YAML 或 JSON 描述部署：

```yaml
application: yolo-split-demo
controller: /example/hello/controller
group: /example/hello/group

identities:
  user: /example/hello/user
  provider_prefix: /example/hello/provider

trust:
  app_roots: [/example]
  # 生产部署应使用显式 trust anchor。
  # anchor_file: /path/to/root.cert

artifact_security:
  # executable artifacts 只有在 trust.anchor_file、artifact_security.allowlist
  # 和 sandbox 全部配置时才允许；否则会被拒绝。
  allowlist: []
  sandbox:
    kind: ""

services:
  - name: /AI/YOLO/SplitInference
    model: /Model/Ultralytics/YOLO/Split
    roles: [/Stage/0, /Stage/1]
    dependencies:
      - producers: [/Stage/0]
        consumers: [/Stage/1]
        key_scope: stage0-to-stage1
        topic_prefix: /activation
    users: [/example/hello/user]
    providers:
      - identity: /example/hello/provider
        roles: all
      - identity: /example/hello/provider/A
        roles: all
```

Distributed-inference 层会把 config 编译为 NDNSF controller policy 和 NDN trust schema。生成的 Data 和 certificate 规则使用层次化验证：Data name 必须在 signer identity 名下，child certificate 必须在 parent certificate namespace 名下。生产部署中，`trust.anchor_file` 必须指向 trust-root certificate；本地示例 fallback 只用于临时 self-signed demo identities。

```bash
ndnsf-di-policy \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-yolo-policy
```

生成文件：

```text
/tmp/ndnsf-di-yolo-policy/trust-schema.conf
/tmp/ndnsf-di-yolo-policy/controller.policies
/tmp/ndnsf-di-yolo-policy/service-manifest.json
/tmp/ndnsf-di-yolo-policy/service-manifest.json.sha256
```

Service manifest 是 service-to-model 和 service-to-dependency 映射的 canonical JSON 形式。`.sha256` 文件只是部署工具的本地 fingerprint，不是安全签名。安全性来自把 manifest 作为 NDN Data 发布并验证 Data signature。

Client 可以通过 NDNSF 发布 manifest：

```python
client = APPClient.from_config("yolo_policy.yaml")
result = client.publish_service_manifest("/AI/YOLO/SplitInference")
print(result.encrypted_data_name)
```

这使用与 model shards 和 runtime artifacts 相同的 NDNSF encrypted large-Data path：payload 被放入由本地 NDN identity 签名的 NDN Data packets；当 service policy 要求 confidentiality 时会加密。Model files、ONNX shards、runner scripts 和 executable bundles 都遵循同样规则：只有发布为 signed NDN Data 后，它们才是 artifacts。

Role scripts 会自动调用 `load_or_generate_deployment()`，因此已提交的 YOLO 示例可以直接从高层 config 运行。

默认禁用 executable artifacts。如果要允许下载的 binary 或 script 被标记为 executable，deployment 必须同时配置：

```yaml
trust:
  anchor_file: /path/to/root.cert

artifact_security:
  allowlist:
    - /NDNSF/Runtime/TrustedBackend/v1
  sandbox:
    kind: container
    image: registry.example/ndnsf-runtime:latest
```

如果缺少其中任何一项，provider code 在请求 `allow_executables=True` 时会在开始服务请求前失败。

## YOLO ONNX Split Example

该示例把一个小型 Ultralytics YOLO 模型导出为两个 ONNX stages。Splitter
会把 ONNX shard paths 写入生成的 policy，providers 按自己的 role 加载
本地 shard，user 只按 service name 发起请求。

安装 Python 依赖：

```bash
python3 -m pip install -e ./pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedInference
python3 -m pip install "ultralytics>=8.4" "onnx>=1.16" "onnxruntime>=1.18"
```

通过 YOLO splitter 生成 ONNX shards 和 policy：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/split_model.py \
  --model yolo26n.pt \
  --auto-split \
  --out-dir /tmp/ndnsf-yolo-split \
  --policy /tmp/ndnsf-yolo-split/yolo_policy.yaml
```

使用 `--auto-split` 时，splitter 会先导出 full ONNX model，运行可选的 graph
analyzer 和同质 provider planner，把推荐的 ONNX cut 映射回 YOLO module
boundary，然后导出两个 ONNX stages。不加 `--auto-split` 时，示例仍保留固定的
YOLO-specific split，方便重复实验。两条路径最终都输出同一种
`SplitterOutput` policy format。

安装后，应用代码可以从任意工作目录导入 distributed inference 层：

```python
from ndnsf_distributed_inference import DistributedInferenceClient
```

每个 shell 或 MiniNDN node 运行一个 role：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/controller.py \
  --config /tmp/ndnsf-yolo-split/yolo_policy.yaml
python3 examples/python/NDNSF-DistributedInference/yolo_split/provider.py \
  --config /tmp/ndnsf-yolo-split/yolo_policy.yaml \
  --temp-dir /tmp/ndnsf-yolo-stage0
python3 examples/python/NDNSF-DistributedInference/yolo_split/provider.py \
  --config /tmp/ndnsf-yolo-split/yolo_policy.yaml \
  --provider-id A --temp-dir /tmp/ndnsf-yolo-stage1
python3 examples/python/NDNSF-DistributedInference/yolo_split/user.py \
  --config /tmp/ndnsf-yolo-split/yolo_policy.yaml
```

如果要运行端到端 MiniNDN 回归，让脚本先生成 auto-split policy，再把
controller、Stage 0 provider、Stage 1 provider 和 user 分别放到不同
MiniNDN 节点上运行：

```bash
sudo -E python3 Experiments/NDNSF_DI_YoloSplit_Minindn.py
```

只有 user 日志里出现下面结果时，smoke test 才算通过：

```text
YOLO_SPLIT_RESULT ... ok=true
YOLO_SPLIT_MININDN_OK ...
```

同一个 smoke test 也可以通过统一的 DI 回归入口运行：

```bash
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case auto-split
```

## YOLO 2x2 Split API Example

`yolo_2x2` 示例展示同一套 APP API 如何表达更通用的 layout：两个 pipeline stages，每个 stage 内有两个顺序 shards。它现在包含真正的分布式推理路径，而不只是 repository smoke test：

`split_model.py` 会导出四个 ONNX chunks，并用 ONNX chunk 的实际 input/output
名字生成 `yolo_policy.yaml` 中的 dependency edges。当前小型 YOLO split
的跨 role tensor set 是：

```text
/Stage/0/Shard/0 -> /Stage/0/Shard/1: x
/Stage/0/Shard/1 -> /Stage/1/Shard/0: x, saved_4
/Stage/1/Shard/0 -> /Stage/1/Shard/1: x, saved_4, saved_10, saved_13
```

每条边发布一个包含这些 tensors 的 activation large object，consumer 在继续
执行自己的 ONNX chunk 前先 fetch 这个对象。这是从真实模型 tensor boundary
派生出的 chunk-level collaboration graph；YOLO 内部复杂 operator graph 仍然
在每个 chunk 内本地执行。
生成的 `*-2x2-onnx-graph-summary.json` 也会记录 full-model candidate split
points，因此后续 planner 可以比较不同 cut positions，而不需要改变
NDNSF-DI policy interface。

由于已经编译好的 distributed-inference plan 会让 dependency scope、topic
prefix、producer role 和 consumer role 都变得可预测，provider 也可以对计划内
输入做预取。`ProviderRuntimeContext.prefetch_input_large(...)` 会在后台等待并
获取某个 role-local dependency reference 指向的 large object；
`wait_prefetched_input_large(...)` 则在 handler 真正需要时返回已经取到的
activation object。这个优化是泛化的：它只依赖声明好的 dependency edge 和 topic
suffix，不依赖 YOLO。只有当 plan 能给出确定的 dependency name 时才应该使用；
否则 handler 仍然可以继续显式调用 `wait_one(...)` 和 `fetch_large(...)`。

对于 ONNX chunks，推荐的 provider-side 路径是
`execute_onnx_dependency_chunk(...)`。它会使用当前 role 的 dependency view
自动收集所有 input-edge tensor bundles，按 tensor name 合并，运行被分配的 ONNX
chunk，然后为每条声明的 output edge 发布一个 tensor bundle。YOLO 2x2 provider
现在已经使用这个 dependency-driven executor：YOLO-specific 代码只负责准备第一块的
image input，以及编码最后的 prediction response。这样 runtime path 就不再把一条
pipeline chain 写死在 provider 中，而是更适合未来 fan-in/fan-out ONNX DAG。

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --model yolo26n.pt \
  --input-size 32 \
  --auto-split \
  --out-dir /tmp/ndnsf-yolo-2x2 \
  --policy /tmp/ndnsf-yolo-2x2/yolo_policy.yaml
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-2x2/yolo_policy.yaml --role /Stage/0/Shard/0
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-2x2/yolo_policy.yaml --provider-id A \
  --role /Stage/0/Shard/1
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-2x2/yolo_policy.yaml --provider-id B \
  --role /Stage/1/Shard/0
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-2x2/yolo_policy.yaml --provider-id C \
  --role /Stage/1/Shard/1
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/user.py \
  --config /tmp/ndnsf-yolo-2x2/yolo_policy.yaml
```

使用 `--auto-split` 时，2x2 splitter 会用 ONNX planner recommendation
选择 pipeline stage boundary，然后把每个 stage 再切成两个顺序 chunks。不加这个
参数时，示例保留原来的 YOLO-specific split 作为稳定 fallback。

MiniNDN 中运行：

```bash
sudo -n python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py
```

期望输出包含：

```text
YOLO_2X2_RESULT ... ok=true
YOLO_2X2_DYNAMIC_PROVISIONING_MININDN_OK ...
```

MiniNDN 脚本会在第一个 command 前清空 provider artifact cache。它在 `neu`
启动 repo node，在 `csu` 启动 controller，然后运行 controller-side deployer
把 model shards 和 runner 写入 repo。Provider logs 随后会在 cold command 中
为每个 role 的 `model` 和 `runner` artifacts 打印
`NDNSF_EXECUTION_ARTIFACT_CACHE_MISS ... source=repo`，并在 warm command 中
打印 `NDNSF_EXECUTION_ARTIFACT_CACHE_HIT`。

如果要用一个入口同时运行 2-stage auto-split smoke 和 YOLO 2x2 smoke：

```bash
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case all
```

policy/repo inspection helper 仍然保留：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/plan_example.py
```

它构建四个可分配 roles：

```text
/Stage/0/Shard/0
/Stage/0/Shard/1
/Stage/1/Shard/0
/Stage/1/Shard/1
```

和三个 dependency scopes：

```text
stage0-internal   activation transfer inside stage 0
stage0-to-stage1  activation transfer between pipeline stages
stage1-internal   activation transfer inside stage 1
```

`split_model.py` 会把 per-role ONNX artifacts 写入生成的 deployment policy。
每个 role 都有自己的 ONNX chunk。因此这个 sharding 是真实 YOLO layers 上的
execution plan，而不是 synthetic NumPy model：每个 provider fetch 前一个
shard 的 activation reference，继续 ONNX computation，然后发布下一个
activation reference。最后一个 Stage1 shard 发布 response。User 会把这个
response 与本地完整 YOLO forward pass 比较，只有数值一致时才打印 `ok=true`。

Provider 可以在不理解 NDN internals 的情况下 advertise 四个 roles：

```python
provider = APPProvider.from_config("yolo_policy.yaml", provider_id="A")
provider.serve(
    service="/AI/YOLO/2x2Inference",
    roles="all",
    handler=handle_yolo_role,
    backends=["onnxruntime"],
    temp_dir="/tmp/provider-A",
    has_model=False,
    can_provision=True,
    allow_executables=True,
)
provider.run()
```

在 `handle_yolo_role(ctx)` 中，APP 使用普通 Python model logic 和提供的 collaboration context：

```python
if ctx.role == "/Stage/0/Shard/0":
    hidden = run_stage0_shard0(ctx.execution.path("model"), ctx.request)
    ctx.publish_output(hidden, key_scope="stage0-to-stage1",
                       topic_suffix="Stage-0-Shard-0")
```

对于更复杂 layout，APP 改变 role names 和 dependency scopes；NDNSF-facing deployment、artifact 和 security mechanics 保持不变。

`Experiments/NDNSF_DI_Yolo2x2_Repo_Minindn.py` 仍然有用，但它是
DistributedRepo storage smoke test。需要验证 end-to-end split inference
和结果一致性时，应运行 `Experiments/NDNSF_DI_Yolo2x2_Minindn.py`。
