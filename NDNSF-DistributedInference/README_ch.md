# NDNSF-DistributedInference

NDNSF-DistributedInference 是构建在通用 NDNSF Python API 之上的 Python 库。它为分布式 AI 推理提供更高层 API，但不会把 AI-specific semantics 加入 NDNSF Core。

分层结构：

```text
APP
  calls model/inference APIs

NDNSF-DistributedInference
  understands model plans, roles, stages, shards, runtime artifacts,
  backend requirements, and inference dependencies

NDNSF Core
  handles Face, SVS, NAC-ABE, signing, permissions, selection, workers,
  and wire protocol behavior
```

## 从零开始的端到端使用指南

这一节面向第一次接触 NDNSF-DI 的开发者。最短流程是：

```text
1. 选择或生成一个 service policy。
2. 审查 identities、users、providers、roles 和 artifacts。
3. 启动一个 controller 进程。
4. 启动一个或多个 provider 进程。
5. 启动 user/client 进程并调用 distributed_inference(...)。
6. 检查结果和 provider 日志。
```

应用开发者应该尽量停留在 APP 层，不需要手写 NDN Interest name、Data name、
SVS topic、segment name、NAC-ABE attribute 或 permission Interest name。
这些都由 service policy 推导，并由 NDNSF-DI 和 NDNSF Core 处理。

### 1. 安装 Python API

在仓库根目录运行：

```bash
python3 -m pip install -e ./pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedInference
```

如果运行 ONNX 示例，还需要安装该示例使用的模型/runtime 依赖，例如 `numpy`、
`onnx`、`onnxruntime`，以及用于导出 ONNX graph 的模型包。MiniNDN 脚本假设
当前 checkout 中已经有可用的仓库构建结果和 NDNSF native runtime。

### 2. 理解主要对象

公开的 APP API 有四个主要入口：

```text
APPDeployment   读取 policy 并生成部署文件
APPController   运行这个 deployment 的 NDNSF service controller
APPProvider     发布 provider 能力并执行被分配的 roles
APPClient       提交 inference 请求并接收最终输出
```

`yolo_policy.yaml` 这样的 service policy 是核心契约。它描述：

```text
提供的是哪个 service name
哪些 users 可以调用这个 service
哪些 providers 可以提供这个 service
模型有哪些 roles/stages/shards
哪个 role 依赖哪个前置 role
每个 role 需要哪些 model/runtime artifacts
request 和 response payload 如何编码
```

对用户来说，service 调用保持简单：

```python
result = client.distributed_inference("/AI/YOLO/SplitInference", image_tensor)
```

剩下的由 policy 决定：provider 选择、role 分配、artifact 获取、activation 交换和
最终 response 收集。

### 3. 创建或审查 Policy

Policy 可以手写，可以由 model-specific splitter 生成，也可以由 ONNX-assisted
planner 生成。当前 YOLO 示例已经带有可运行的 policy 文件和 splitter 脚本。
YOLO layout splitter 可以把真实 Ultralytics YOLO 模型导出成用户指定的
stage-by-shard layout，例如 `1x3`、`2x3`、`3x2` 或 `3x3`；生成出的 policy
部署后仍由同一个 dependency executor 驱动执行。这是 YOLO ONNX 路径上的自定义
layout 支持，还不是对所有 ONNX topology 和所有 partitioning strategy 的完全通用
planner。

两阶段 YOLO split：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/split_model.py
```

四角色 YOLO 2x2 示例：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py
```

自定义 YOLO ONNX layout：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --layout 3x2 \
  --out-dir /tmp/ndnsf-yolo-3x2 \
  --policy /tmp/ndnsf-yolo-3x2/yolo_policy.yaml
```

Splitter 会为每个生成 role 导出一个 ONNX chunk，根据 ONNX chunk 的实际
input/output tensor 写入 chunk-level dependencies，并在成功输出前运行一次本地
chunk pipeline 验证。

部署前先审查 policy：

```bash
PYTHONPATH="NDNSF-DistributedInference:$PYTHONPATH" \
python3 -m ndnsf_distributed_inference.policy \
  --config examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-review \
  --print-summary
```

摘要会显示：

```text
User permissions      每个 user 可以调用哪些 services
Provider permissions  每个 provider 可以运行哪些 services 和 roles
Role coverage         每个 role 是否至少有一个 provider
Artifact coverage     每个 role 是否有 model/runtime artifacts
Artifact security     executable artifacts 是否被允许
```

这是部署 sanity check。真正授权仍然来自每个 service 下精确的 `users` 和
`providers` 列表。

### 4. 启动 Controller

Controller 读取同一个 policy，并为这个 deployment 发布 NDNSF permission 和 trust
材料：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/controller.py \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml
```

在 MiniNDN 脚本中，这个进程运行在某个节点上。真实部署中，它运行在拥有
service-controller identity 和 trust root 的节点上。

### 5. 启动 Providers

Provider 注册 service，并声明自己能运行哪些 roles：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/provider.py \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml \
  --provider-id A \
  --temp-dir /tmp/ndnsf-yolo-provider-A
```

对于同质 provider pool，通常使用 `roles="all"` 就够了。Provider 不一定一开始就
本地安装所有 model shards。如果 policy 中包含 artifacts 或 artifact references，
provider 可以先被选中承担某个 role，然后下载这个 role 需要的 model/runtime
artifact。

### 6. 启动 User

User 进程使用 `APPClient` 并调用 service name：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/user.py \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml
```

应用内部的用户侧代码类似：

```python
client = APPClient.from_config("yolo_policy.yaml")
service = "/AI/YOLO/SplitInference"
print(client.describe_input(service))
print(client.describe_output(service))
result = client.distributed_inference(service, image_tensor)
```

如果 service input 声明 `codec: npz`，NDNSF-DI 可以自动编码常见 numpy tensor
输入。只有模型需要自定义预处理时，才需要注册 custom input encoder。

### 7. 运行完整 MiniNDN Smoke Tests

最简单的端到端网络路径测试是 MiniNDN：

```bash
sudo -E python3 Experiments/NDNSF_DI_YoloSplit_Minindn.py
sudo -E python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py
sudo -E python3 Experiments/NDNSF_DI_PyTorch2x2_Minindn.py
```

成功输出应包含：

```text
YOLO_SPLIT_RESULT ... ok=true
YOLO_2X2_RESULT ... ok=true
PYTORCH_2X2_RESULT ... ok=true
```

统一 runner 也可以运行指定 cases。`yolo-layout` 是快速的非 MiniNDN
layout/policy smoke，用来检查自定义 YOLO layout；当前稳定的网络级 YOLO
回归仍然是 `yolo-2x2`：

```bash
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case all
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case yolo-layout --layout 3x2
```

### 8. 常见部署错误

如果部署失败，先检查这些问题：

```text
runtime.user_identity 没有出现在任何 service users 列表里
provider identity 没有出现在 service providers 列表里
service roles 提到了没有任何 provider 能承担的 role
artifact path 指向不存在的文件
repo manifest 指向未发布的 model/runtime object
生产部署缺少 trust.anchor_file
input tensor shape 或 dtype 与 describe_input(service) 不一致
provider 使用 can_provision=False 但本地没有 model shard
```

使用下面命令可以在启动 controller、providers 或 users 前抓出大多数 policy-level
问题：

```bash
python3 -m ndnsf_distributed_inference.policy \
  --config yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-review \
  --explain
```

## 图形化部署工具

对于不熟悉 YAML、NDN name 和证书命令的用户，NDNSF-DI 还提供一个轻量级
Python GUI：

```bash
PYTHONPATH="NDNSF-DistributedInference:$PYTHONPATH" \
python3 Experiments/NDNSF_DI_GUI.py
```

如果希望先做 GUI preflight，或者在打开 GUI 前顺便跑一次 MiniNDN regression，
可以用这个面向 GUI 试用的入口：

```bash
python3 Experiments/NDNSF_DI_GUI_Minindn.py
python3 Experiments/NDNSF_DI_GUI_Minindn.py --run-minindn --case app-api --no-gui
python3 Experiments/NDNSF_DI_GUI_Minindn.py --run-minindn --case yolo-2x2 --no-gui
```

第一条命令会检查 `tkinter`、导入 GUI、验证默认 policy，然后打开 GUI。第二条
命令通过同一个 launcher 运行快速的非 MiniNDN API smoke。第三条命令运行完整的
YOLO 2x2 MiniNDN distributed-inference diagnostic path。

第一版使用 Python 标准库里的 `tkinter`，因此普通 Ubuntu 桌面不需要额外引入
Qt 依赖。它提供：

```text
Service Project Wizard
  导入 ONNX、PyTorch 或已有 policy 文件，选择 service/controller/group/user
  名字，选择 provider identities 和 roles，并生成 policy skeleton。

Policy Editor
  加载和编辑 YAML，浏览 users/providers/services，保存前运行 policy
  validation，并显示与 ndnsf-di-policy --explain 相同的 summary。

Model Split
  导入 ONNX 模型，显示 graph summary 和 candidate split points，并把推荐
  split 写入两段式 policy skeleton。

Certificate / Identity Manager
  调用 ndnsec list，显示本机 identities/certificates，选择
  runtime.user_identity，生成 key request，并导入 safebag。GUI 不直接随意
  签发证书；证书签名仍应遵循 deployment trust process。

Controller / User / Provider certificate tools
  各个 role tab 也包含部署证书流程。User 或 Provider tab 可以在本机生成自己的
  private key 和 key request，然后把 request 文本复制到 Controller tab。如果当前
  节点是 root/controller 节点，Controller tab 可以生成 root certificate，并对粘贴
  进来的 request 或 request file 进行签名。签好的 certificate 再复制回 User 或
  Provider tab，通过 ndnsec cert-install 安装。这样 private key 始终留在申请节点，
  只有 certificate request 交给 root/controller 签名。

Deployment Runner
  启动 example controller/provider/user，显示 logs，并运行统一 DI regression
  runner。默认 YOLO 2x2 regression 会启动 MiniNDN，执行分布式推理，并检查
  YOLO_2X2_RESULT ... ok=true。auto-split 两段式 regression 也作为可选 case
  保留。
```

同一个 GUI 里也有独立的 `Controller`、`User` 和 `Provider` tabs。真实节点可以
同时启用任意组合：例如一台桌面机器可以同时运行 controller 和 user，另一台 worker
节点运行一个或多个 provider roles。这些 tabs 从同一个 policy 文件配置和启动
APP-level role process，并把日志发送到 Deployment Runner pane。

这个 GUI 只封装现有 APP-level APIs 和 `ndnsf-di-policy` validation path。
它不引入新的 policy format，也不改变 NDNSF 的 authorization 机制。

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
Service package 应该随服务自带默认 policy config，就像 YOLO 示例自带
`yolo_policy.yaml`。用户只有在修改身份、trust root、provider pool、
artifact path、repo manifest 或模型切分方式时，才需要传入自定义 config。

Client 侧：

```python
from ndnsf_distributed_inference import APPClient

client = APPClient.from_config("yolo_policy.yaml")
service = "/AI/YOLO/SplitInference"
print(client.describe_input(service))
print(client.describe_output(service))

# 如果 policy 声明 codec=npz 并且只有一个 tensor field，NDNSF-DI 可以
# 自动把 numpy tensor 编码成请求 bytes。只有需要模型专用预处理时，才注册
# 自定义 encoder。
result = client.distributed_inference(service, image_tensor)

# 可以同时提交多个请求。每个请求仍然使用 NDNSF runtime 完成
# Face/SVS/NAC-ABE 工作；APP 线程收到的是 Future。
future = client.async_distributed_inference(service, image_tensor)
result = future.result(timeout=30)
```

推荐的用户侧入口是 `distributed_inference(...)`；异步形式是
`async_distributed_inference(...)`。这个命名是有意的：APP 层暴露的是
distributed inference，而不是一个泛化的 NDN service invocation API。调用者给出
service name 和应用对象，例如 `image_tensor`，NDNSF-DI 根据 service 输入说明
把它转换为请求 bytes。service name 是一个有约束的应用契约，而不是随便起的字符串：
`/AI/YOLO/SplitInference` 在 `yolo_policy.yaml` 中绑定到一个确定的模型
identity、模型版本、输入/输出编码、role set、dependency graph、provider
identity 和 security policy。换句话说，client 不需要在每次请求里重新
说明模型怎么切、有哪些 stage、谁依赖谁；这些都由 service name 查到。

对用户来说，这应该就是完整的请求 API。Provider 选择、role 分配、
artifact 发布、model shard 下载、scope key 分发、dependency 交换和
结果收集都由运行时隐藏在 service call 之后。默认部署假设是：Provider
一开始是同质化的 service worker。如果 service policy 中记录了 artifacts，
`distributed_inference(...)` 会自动构造动态 provisioning plan：被选中的 provider
根据自己被分配到的 role 获取 executable/runtime bundle 和 model shard。
如果 service policy 没有 artifact 描述，同一个调用会退回到预部署模型路径。

`client.describe_input(service)` 返回 policy 中记录的输入说明，例如 codec、
字段名、shape、dtype 和推荐 encoder 名称。`client.describe_output(service)`
返回输出说明，例如 response codec 和 tensor layout。对于常见 tensor payload，
现在有内置 NPZ encoder：如果 policy 声明 `codec: npz`，调用者可以直接传 numpy
tensor、字段名到 tensor 的 mapping，或者和字段列表对应的 tuple/list。只有 generic
NPZ encoder 不知道的模型专用预处理，才需要 `register_input_encoder(...)`。
如果调用者已经有编码好的 bytes，也直接传给同一个 `distributed_inference(...)`
或 `async_distributed_inference(...)` 入口即可。

编码后的请求 bytes 由这个 service 契约定义。它由应用层或内置编码函数生成。
NDNSF-DI 编码之后不解释这些 bytes 的语义，只负责把它们安全送到对应 service
的分布式执行流程。Provider handler 和 client 必须对同一个 service 使用同一套
payload schema；如果输入 shape、dtype、预处理方式或模型切分发生不兼容变化，
应发布为新的 service name 或新的模型版本，而不是复用旧名字。

Model/runtime artifacts 属于 service definition，而不是用户临时手写代码。
Splitter 或 deployment tool 应把 artifact paths 或 artifact references 写入
service policy。普通应用调用者不需要手工构造 `DistributedInferencePlan`；
他们调用 `distributed_inference(...)`，APP 层会从 service policy 推导 plan。
高级部署工具如果需要检查或复用生成的 plan，可以调用
`client.service_plan(service, ...)`。`distributed_inference(...)`、
`async_distributed_inference(...)` 和 `service_plan(...)` 的可选
`artifact_references` 参数指的是保存在 NDNSF-DistributedRepo 中的 model/runtime
artifacts，而不是输入图片或 activation tensors。旧名 `repo_manifests` 仍然为已有
脚本保留兼容，但新代码应使用 `artifact_references`，因为同一个 entry 同时携带
`repoManifest` 和 `largeDataReference` metadata。输入和中间 tensor 使用
service payload contract，以及 NDNSF large-data 或 dependency-object helper。
Repo-backed artifacts 实际取数仍走 manifest-aware repo path，但 execution spec
也会携带与 input 和 activation 相同形状的 large-data reference metadata。新的
planner 或 executor 代码应该读取这类 reference metadata，而不是继续传裸 Data name
字符串。
APP plan builder 现在也会优先把 reference 放进每个 artifact spec；内嵌的 repo
manifest 只作为 repo-backed provider 的 fetch metadata 和旧脚本兼容 fallback。
metadata 中包含 `source` 字段：`repo-manifest` 表示 provider 应该走 repo
manifest-aware object fetch path；`ndn-large-data` 表示 provider 可以直接按名字
抓取加密 large Data。
生成的 repo deployment manifest 文件会为每个 artifact 显式写出两个字段：
`repoManifest` 用于 manifest-aware fetch path，`largeDataReference` 便于人和
planner 直接审查 source、Data name、hash 和 size。Runtime execution spec 也会携带
这些 camelCase 字段，同时保留旧 snake_case alias 兼容旧 provider。新的 provider
代码应优先读取 `largeDataReference`，只在兼容场景下回退到 `repoManifest` 或
`repo_manifest`。

Provider 侧：

```python
from ndnsf_distributed_inference import APPProvider

provider = APPProvider.from_config("yolo_policy.yaml", provider_id="A")
provider.serve_service(
    service="/AI/YOLO/SplitInference",
    roles="all",
    handler=handle_assigned_role,
    backends=["onnxruntime"],
    temp_dir="/tmp/ndnsf-yolo-provider-A",
    has_model=False,
    can_provision=True,
)
provider.run()
```

Provider 使用一个 service-level 注册。在常见的同质 worker 场景中，每个
provider 都可以用 `roles="all"` 和 `can_provision=True` 发布能力。被选中
承担某个 role 的 provider 会从 assignment 中获取该 role 的 artifact，
然后执行该 role。已经在本机安装好 model shards 的部署可以改用
`has_model=True, can_provision=False`。

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

多服务部署中，对每个 service 调用一次 `provider.serve_service(...)`，并使用
`client.async_distributed_inference(...)` 并发请求一个或多个 service。service name
仍然决定每个 request 使用的固定 role set 和 dependency graph。

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
  model/runtime artifacts 存入 repo。User 只携带 artifact references；
  providers 在第一个 command 获取被分配 role 需要的 artifacts，第二个
  command 复用 provider artifact cache。

pytorch_eager_2x2/
  Four-provider fully connected ONNX example。模型先用 PyTorch 定义，
  splitter 再导出 full ONNX reference graph、分析 candidate cut points，
  并生成四个 ONNX shards：两个 hidden-layer shards 和两个 output-layer
  shards。它会检查分布式输出是否与本地完整模型一致。
```

当模型可以导出为 ONNX 时，ONNX 示例代表推荐的 portable deployment path。
全连接示例说明了为什么某些模型 family 仍然需要 model-specific splitter：
通用 ONNX sequential cut 可以发现 graph boundary，但 dense layer 内部的
horizontal split 需要理解 weight rows、activation offsets 和 output merge
order。

较低层的 `DistributedInferenceClient`、`DistributedInferenceProvider` 和 `DistributedInferenceController` 仍然保留，供 framework developer 和需要直接控制的实验使用。

## DistributedRepo Integration

Model shard、runtime bundle 和 intermediate data 不应随意 push 到任意节点。NDNSF-DI 可以携带从 NDNSF-DistributedRepo object manifests 生成的 artifact references：

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
        "/NDNSF-DistributeInference/example/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/"
        "NDNSF-DI/ARTIFACT/AI/YOLO/2x2/Stage/0/Shard/0/model"
    ),
    payload=model_bytes,
    object_type="onnx-model",
    policy=PlacementPolicy(replication_factor=2),
    policy_epoch="/Policy/yolo-2x2/v1",
)

payload = repo.fetch_object(manifest.object_name, manifest)
```

Manifest 记录 object hash、size、replication factor、selected repo nodes，以及保存 object segments 的 signed Data names。Object name 仍然按发布者命名空间组织：controller 发布的 artifacts 使用 controller object namespace，user 发布的 inputs/intermediates 使用 user namespace，provider 输出使用 provider namespace。实际服务 stored segments 的 Data name 可以是 repo-owned prefix，例如 `/repo-node/NDNSF-DISTRIBUTED-REPO/DATA/<object-hash>`，这样 fetcher 可以直接路由到被选中的 repo node。当前 networked path 中，controller-side deployer 会在推理前把 model/runtime artifacts 存入 repo node。User request 只携带 execution specs 和 artifact references。每个 reference 同时包含 `largeDataReference` 和用于 manifest-aware fetch 的 `repoManifest`；被选中的 providers 自己从 repo 获取被分配 artifacts，并在本地缓存。

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

对于不熟悉 NDN 的用户，推荐的 API 边界是：

```text
Application code:
  APPClient / APPProvider / APPController
  SplitterOutput 或 yolo_policy.yaml
  ONNX role handler 使用 execute_onnx_dependency_chunk(...)

Framework/internal code:
  NDNSF request/ACK/selection/response names
  segmented large-data fetch/publish
  repo segment names 和 placement details
  NAC-ABE attributes 和 permission Interests
```

也就是说，AI application developer 应该描述 model layout、roles、artifacts、
dependencies 和 input/output codecs。正常情况下，他们不需要手写 NDN names，
也不需要自己 fetch 单个 Data segments。如果某个 handler 仍然必须直接调用
`ctx.ndnsf.wait_one(...)` 或 `ctx.ndnsf.fetch_large_reference(...)`，这通常说明当前
APP/runtime helper 对这个 workload 还暴露得太底层。

## Dependency Graph Generation Roadmap

分布式推理部署中需要区分三种图：

```text
Model dependency graph
  原始模型内部的 operator/tensor DAG，例如 ONNX graph。

Chunk collaboration graph
  模型切分后的 provider-role graph。每条边记录哪些 activation tensors
  从一个 chunk 传给另一个 chunk。

Deployment plan
  roles 到 providers 的映射、runtime artifacts、artifact references、
  security policy 和 NDNSF service names。
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
      "name": "/NDNSF-DistributeInference/example/provider/A",
      "compute_score": 1.0,
      "uplink_mbps": 200,
      "downlink_mbps": 200,
      "rtt_ms": 20
    },
    {
      "name": "/NDNSF-DistributeInference/example/provider/B",
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
    controller="/NDNSF-DistributeInference/example/controller",
    group="/NDNSF-DistributeInference/example/group",
    user="/NDNSF-DistributeInference/example/user",
    provider_prefix="/NDNSF-DistributeInference/example/provider",
    trust_app_roots=["/NDNSF-DistributeInference/example"],
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

生成的 YAML 因此是由 split 派生的 deployment policy。这个 split 可以来自 ONNX tensor DAG、PyTorch/model-specific splitter，也可以来自手写 application planner。同一个 service name 应始终映射到同一个 model layout 和 dependency graph。如果模型用不同方式切割，就应发布为不同 service name。Splitter output 使用具体 user 和 provider identity，这和 NDNSF controller policy 语义一致：每个命名身份获得明确的 service permission。

Provider handler 会在 `ctx.dependencies` 中收到 role-local dependency view，因此 handler 代码可以询问当前 role 应该发布或等待什么，而不是手写重复 topic strings：

```python
def handle_assigned_role(ctx):
    if ctx.dependencies.outputs:
        activation = run_local_stage(ctx.execution.path("model"), ctx.request)
        ctx.publish_output_large_reference(
            activation,
            data_topic_suffix="activation",
            ref_topic_suffix="ref",
            object_type="application/x-ndnsf-di-activation+npz")

    if ctx.dependencies.inputs:
        future = ctx.prefetch_input_large(topic_suffix="ref")
        activation = ctx.wait_prefetched_input_large(future)
```

对于有多个 inputs 或 outputs 的 role，可以显式传入 `key_scope`，例如 `ctx.dependencies.input("stage0-to-stage1")` 或 `ctx.publish_output(payload, key_scope="stage1-internal")`。

## User-Facing Security Config

应用开发者不需要手写 NDN validator trust schema。他们使用 YAML 或 JSON 描述部署：

```yaml
# 部署相关部分：真实部署时先改这里的名字。
# application: 当前 APP deployment 的本地标签。
application: yolo-split-demo
# controller: NDNSF ServiceController 的 identity/prefix，用来签发和分发权限。
controller: /NDNSF-DistributeInference/example/controller
# group: controller、user、provider 共享的 NDN-SVS group prefix。
group: /NDNSF-DistributeInference/example/group

runtime:
  # user_identity: 当前配置启动 user/client 进程时使用的 identity。
  # 它本身不授予权限；真正授权由下面每个 service 的 users 决定。
  user_identity: /NDNSF-DistributeInference/example/user
  # provider_prefix: 示例用来生成具体 provider 名字的命名辅助，不是通配权限。
  provider_prefix: /NDNSF-DistributeInference/example/provider

trust:
  # app_roots: 这个部署 namespace 的 trust-schema roots。
  app_roots: [/NDNSF-DistributeInference/example]
  # 生产部署应使用显式 trust anchor。
  # anchor_file: /path/to/root.cert

artifact_security:
  # executable artifacts 只有在 trust.anchor_file、artifact_security.allowlist
  # 和 sandbox 全部配置时才允许；否则会被拒绝。
  allowlist: []
  sandbox:
    kind: ""

authorization_summary:
  users:
    - identity: /NDNSF-DistributeInference/example/user
      services:
        - /AI/YOLO/SplitInference
  providers:
    - identity: /NDNSF-DistributeInference/example/provider
      services:
        - service: /AI/YOLO/SplitInference
          roles: all

services:
  - name: /AI/YOLO/SplitInference
    model: /Model/Ultralytics/YOLO/Split
    # users: 被允许调用该服务的具体 user identities。
    users: [/NDNSF-DistributeInference/example/user]
    # providers: 被允许提供该服务的具体 provider identities。roles=all 表示
    # 这个 provider 可以承担下面列出的任意 role。
    providers:
      - identity: /NDNSF-DistributeInference/example/provider
        roles: all
      - identity: /NDNSF-DistributeInference/example/provider/A
        roles: all
    # 下面是 splitter/planner 生成的模型切割内容；模型切割变化时应重新生成，
    # 而不是手工修改。
    roles: [/Stage/0, /Stage/1]
    dependencies:
      - producers: [/Stage/0]
        consumers: [/Stage/1]
        key_scope: stage0-to-stage1
        topic_prefix: /activation
```

Distributed-inference 层会把 config 编译为 NDNSF controller policy 和 NDN trust schema。`runtime.user_identity`
表示当前 client 进程默认使用的本地 identity；它本身不授权该 identity。服务授权使用每个 service
下面精确的 `users` 和 `providers` 条目，这和 NDNSF policy 一致：某个被命名的 identity
获得某些具体 service 和 role 的权限。生成的 Data 和 certificate 规则使用层次化验证：Data name
必须在 signer identity 名下，child certificate 必须在 parent certificate namespace
名下。生产部署中，`trust.anchor_file` 必须指向 trust-root certificate；本地示例
fallback 只用于临时 self-signed demo identities。

由 splitter 生成的 policy 文件会分成两个清楚可见的部分：`# editable
deployment section` 包含 namespace、controller/group prefix、runtime
identity、trust 和 artifact-security 等部署字段；`# generated model-plan
section` 包含每个 service 的 users/providers 以及模型 roles、dependencies、
artifacts、input 和 output。部署者可以修改精确的 `users` 和 `providers`
来分配权限；如果模型切割变化，则应重新生成 roles/dependencies/artifacts。
`ndnsf-di-policy` 还会检查 `runtime.user_identity` 是否出现在至少一个
service 的 `users` 列表中；如果配置语法看起来正确但本地 client 实际没有任何
service 权限，它会在生成部署文件之前失败。同一个校验还会检查每个声明的或
dependency 引用的 service role 是否至少有一个被授权 provider 可以承担，避免
plan 里悄悄需要一个没有任何 provider 能运行的 role。可选的
`authorization_summary` 是从 `services[].users/providers` 生成的只读审查辅助；
它让部署者快速看到每个 user 能调用哪些 services、每个 provider 能运行哪些
services 和 roles。它不是第二套权限来源。

```bash
ndnsf-di-policy \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-yolo-policy
```

如果部署前不想打开 YAML，也可以直接打印从配置推导出的授权和覆盖摘要：

```bash
ndnsf-di-policy \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-yolo-policy \
  --print-summary
```

`--explain` 是 `--print-summary` 的别名。报告会列出 user 到 service 的权限、
provider 到 service/role 的权限、role coverage、artifact coverage 和
artifact-security 状态。这个命令使用和生成部署文件相同的 parser 与 validation
路径，因此如果 user 未被授权，或者某个 role 没有任何 authorized provider，
会在打印摘要前直接报错。

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

统一入口里还包含一个快速的本地 ONNX executor 检查；它不会启动 MiniNDN：

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case onnx-executor
```

## YOLO Layout Split API Example

`yolo_2x2` 示例展示同一套 APP API 如何表达更通用的 layout。历史默认是
2x2：两个 pipeline stages，每个 stage 内有两个顺序 shards。现在同一个
splitter 也接受 `1x3`、`2x3`、`3x2`、`3x3` 等自定义 layout。它包含真正的
分布式推理路径，而不只是 repository smoke test：

`split_model.py` 会为每个 role 导出一个 ONNX chunk，并用 ONNX chunk 的实际
input/output 名字生成 `yolo_policy.yaml` 中的 dependency edges。以默认 2x2
为例，当前小型 YOLO split 的跨 role tensor set 是：

```text
/Stage/0/Shard/0 -> /Stage/0/Shard/1: x
/Stage/0/Shard/1 -> /Stage/1/Shard/0: x, saved_4
/Stage/1/Shard/0 -> /Stage/1/Shard/1: x, saved_4, saved_10, saved_13
```

每条边发布一个包含这些 tensors 的 activation large object，consumer 在继续
执行自己的 ONNX chunk 前先 fetch 这个对象。这是从真实模型 tensor boundary
派生出的 chunk-level collaboration graph；YOLO 内部复杂 operator graph 仍然
在每个 chunk 内本地执行。
生成的 `*-<layout>-onnx-graph-summary.json` 也会记录 full-model candidate split
points，因此后续 planner 可以比较不同 cut positions，而不需要改变
NDNSF-DI policy interface。

由于已经编译好的 distributed-inference plan 会让 dependency scope、topic
prefix、producer role 和 consumer role 都变得可预测，provider 也可以对计划内
输入做预取。`ProviderRuntimeContext.prefetch_input_large(...)` 会在后台等待并
获取某个 role-local dependency reference 指向的 large object；
`wait_prefetched_input_large(...)` 则在 handler 真正需要时返回已经取到的
activation object。这个优化是泛化的：它只依赖声明好的 dependency edge 和 topic
suffix，不依赖 YOLO。只有当 plan 能给出确定的 dependency name 时才应该使用；
否则 handler 仍然可以继续显式调用 `wait_one(...)` 和
`fetch_large_reference(...)`。新代码应该用标准 NDNSF large-data reference
payload 发布 dependency reference，而不是在 collaboration message 中放裸 Data name。
同样的 reference metadata 也会附加到 execution spec 里的 repo-backed
model/runtime artifacts 上，尽管这些 artifact 的 bytes 仍通过 repo 的
manifest-aware object API 获取。
Provider materialize artifact 时会优先读取这个 reference，然后再 fallback 到旧的
`repoManifest`、chunk-list 或单个 Data-name 字段。

对于 ONNX chunks，推荐的 provider-side 路径是
`execute_onnx_dependency_chunk(...)`。它会使用当前 role 的 dependency view
自动收集所有 input-edge tensor bundles，按 tensor name 合并，运行被分配的 ONNX
chunk，然后为每条声明的 output edge 发布一个 tensor bundle。YOLO provider
现在已经使用这个 dependency-driven executor：它根据 role-local dependency view
判断当前 role 是首块、中间块还是终块。YOLO-specific 代码只负责准备第一块的
image input，以及编码最后的 prediction response。这样 runtime path 不再把 2x2
pipeline chain 写死在 provider 中，而是可以由 policy 驱动执行 `1x3`、`2x3`、
`3x2`、`3x3` 等自定义 layout。

executor 还有一个不依赖 MiniNDN 的小型 smoke test。它会构造一个 toy ONNX DAG，
包含一条 fan-out edge 和一个 fan-in join：

```bash
PYTHONPATH="NDNSF-DistributedInference:$PYTHONPATH" \
  python3 Experiments/NDNSF_DI_OnnxExecutor_Smoke.py
```

只有看到下面输出才表示通过：

```text
ONNX_EXECUTOR_FANIN_FANOUT_OK
```

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --model yolo26n.pt \
  --input-size 32 \
  --layout 3x2 \
  --auto-split \
  --out-dir /tmp/ndnsf-yolo-3x2 \
  --policy /tmp/ndnsf-yolo-3x2/yolo_policy.yaml
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-3x2/yolo_policy.yaml --provider-id A --roles all
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-3x2/yolo_policy.yaml --provider-id B --roles all
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-3x2/yolo_policy.yaml --provider-id C --roles all
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-3x2/yolo_policy.yaml --provider-id D --roles all
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/user.py \
  --config /tmp/ndnsf-yolo-3x2/yolo_policy.yaml
```

使用 `--auto-split` 时，splitter 会用 ONNX planner recommendation
选择 pipeline boundary hint；layout 参数决定最终导出的 chunk 数量。不加这个
参数时，示例保留原来的 YOLO-specific split hint 作为稳定 fallback。

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

如果要用一个入口运行 APP API smoke、本地 ONNX executor smoke，以及两个稳定的
MiniNDN split smokes：

```bash
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case all
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case yolo-layout --layout 3x2
```

`yolo-layout` 验证自定义 YOLO layout 导出、本地 ONNX chunk 正确性和
policy 生成。它不表示自定义 layout 的网络级执行已经稳定；当前 repo-backed
MiniNDN 网络回归仍以 `yolo-2x2` 为准。

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
provider.serve_service(
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
