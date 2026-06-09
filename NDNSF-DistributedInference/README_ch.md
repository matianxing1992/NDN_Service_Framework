# NDNSF-DistributedInference

NDNSF-DistributedInference 是构建在 NDNSF 之上的应用层分布式推理运行时。
当前用户可见 API 和示例仍然以 Python 为主，但性能方向必须转向 C++：hot-path
调度、dependency dataflow、prefetch、worker dispatch，以及后续 ONNX Runtime
执行，都应在 native C++ 中运行并直接调用 NDNSF Core。Python 应保留为薄 API、
deployment、GUI 和实验层。

因此当前仓库中同时存在两层：

```text
稳定的 APP-facing layer
  Python APPClient / APPProvider / APPController / APPDeployment
  policy 生成、MiniNDN 实验、GUI 和示例脚本

Native hot-path 迁移层
  NDNSF-DistributedInference/cpp/ 下的 C++ async dataflow runtime
  multi-worker role scheduling、planned dependency edges、fan-in/fan-out
  execution，以及后续 C++ ONNX/NDNSF 集成所需的 timing hooks
```

长期目标不是永远用 Python executor 包一层 NDNSF。更合理的结构是：Python 描述
service 并提交 inference jobs，而 providers 通过 native C++ workers 执行
dependency-driven inference。

分层结构：

```text
APP
  calls model/inference APIs

NDNSF-DistributedInference
  understands model plans, roles, stages, shards, runtime artifacts,
  backend requirements, and inference dependencies; current public API is
  Python, while the hot-path runtime is migrating to C++

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

`NxM` layout 这个术语在 NDNSF-DI 中固定表示真正的分布式推理目标：
`N` 个纵向 model stages，每个 stage 内有 `M` 个并行 shards。同一 stage
内的 shards 应运行在不同 providers 上并发执行，然后交换或 merge 下一 stage
需要的 tensors。换句话说，真正的 NxM 需要 model-specific splitter 输出
水平/tensor-sharded ONNX chunks 和 fan-in/fan-out dependency graph；把模型切成
一条 sequential chunk chain 不能算真正的 NxM parallel sharding。Splitter 可以使用
`nxm_stage_roles(...)` 和 `nxm_stage_frontier_dependencies(...)` 生成通用
stage-frontier skeleton，然后填入模型特定的 tensor 名称、merge 语义和 artifacts。

### 2.1 Native Hot-Path Runtime 方向

Python executor 仍然适合实验、policy validation 和 model-specific splitter 原型，
但它不适合长期作为性能 hot path。Native 迁移分阶段推进：

```text
Step 1: C++ async dataflow runtime
  Role frontier scheduling、fan-in/fan-out readiness、multi-worker execution、
  planned dependency edge metadata 和 timing records。

Step 2: C++ NDNSF integration
  直接在 C++ 中使用 ServiceUser/ServiceProvider、large-data references、
  pending Interests 和 deterministic activation names，避免每个 role callback
  都穿过 Python wrapper。

Step 3: C++ backend runners
  ONNX Runtime CPU 执行现在已经有最小 native role runner。Python 仍可负责
  生成 policy 和提交 jobs，但 provider 执行应继续向 native runners 收敛。
```

`NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp` 是第一块
native graph-level 基座。它刻意保持 model-agnostic：role runner 可以是 ONNX
chunk、PyTorch 导出的 native runner、containerized function，或未来的
accelerator backend。这个 runtime 只负责 dependency readiness 和并行执行语义。

`NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp` 是 provider-side
hot-path 边界。当 provider 收到某个 role assignment 后，worker 会立刻对所有
planned input edges 发起 prefetch，等齐 required inputs 后运行 native role runner，
再发布每个 declared output edge。source role 也可以通过 native session/runtime API
直接收到 request-level initial input bundle，因此第一段不再需要为了拿用户输入而
伪造 dependency edge。它的 `DependencyIo` 接口就是接入 C++ NDNSF large-data
fetch/publish 和 pending-Interest support 的位置。这样可以把 Python 从 per-edge
execution loop 中移出去，同时保留现有 Python-facing API。

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp` 定义 backend
边界。从测试 runner 切换到 ONNX chunk runner 时，不应该再改 provider scheduling
和 dependency I/O。它也定义了 `NativeModelRunnerSpec` 和
`RegistryNativeModelRunnerFactory`，这样 deployment/artifact metadata 可以通过
一个很窄的 backend registry 转成 role runner。

`NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp` 定义 native
multi-tensor activation format。它为每个 tensor 保存 tensor name、dtype、shape 和
payload，因此 C++ providers 可以交换 YOLO 风格的多 backbone/head tensor bundle，
而不是假设每个 activation 都只是一个 raw byte vector。Provider worker 现在也能
使用 dependency edge 携带的 tensor names，从更大的 ONNX output bundle 中发布
某条 edge 需要的 tensor 子集。

`NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp` 和
`OnnxRuntimeModelRunner.cpp` 提供了第一个真实 native backend。当前 adapter 支持
CPU ONNX Runtime 执行 float32 raw tensor bundle，也支持 native multi-tensor bundle
codec。它从 `NativeModelRunnerSpec` 读取 ONNX input/output 名称和可选 shape/scope
metadata，在 C++ 中运行 ONNX chunk，并把输出作为 `TensorBundle` 返回给 dependency
executor。这已经能证明 native hot path 可以在不穿过 Python wrapper 的情况下执行
真实 ONNX 模型。不过它仍然是窄实现：native tensor-bundle input/output 目前主要
支持 float32 tensor，非 float tensor、更完整的 shape/type 协商，以及从
repo/largeDataReference materialize artifact 的 C++ 路径仍是后续工作。

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp` 是 provider
进程 facade，具体实现在 `NativeProviderRuntime.cpp`。它持有 worker pool 和
role-to-runner registry。Deployment/Python 代码后续应为 provider 能执行的 roles
注册 native runners，然后把分配到的 `RoleSpec` 提交给这个 runtime。这就是预期的
“C++ core, thin Python API” 结构。

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp` 是 native
provider skeleton 边界，具体实现在 `NativeProviderSession.cpp`。它把 generated
execution plan、provider assignment、`DependencyIo`、runner factory 和 provider
runtime 组合起来。后续真正服务网络请求的 provider executable 应加载 generated
plan，根据 artifact metadata 注册 role runners，再通过这个 session 执行被分配的
roles，而不是在应用里手写这些组合逻辑。它现在已经支持 source-role initial input、
dependency-driven intermediate roles 和 final role result publication。

`NDNSF-DistributedInference/cpp/ndnsf-di/` 下每个 header 现在都有对应 `.cpp`
translation unit。`ProviderRoleWorker`、`NativeProviderRuntime`、
`NativeProviderSession` 这类有状态 runtime 类已经把实现移出 header。较小的 codec
和 plan helper 仍保留必要 inline helper，但对应 `.cpp` 会被 native DI targets 编译，
使 C++ 项目结构不再只是 header-only scaffold。

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp` 把这个
session 形态适配到 `ServiceProvider::CollaborationContext`。它会为每次请求构造
`NdnsfCollaborationDependencyIo`，执行被分配的 native role，通过 planned
dependency edges 发布 inter-role activation outputs，并让最终 role 的用户可见结果
继续走普通 NDNSF response path，也就是 `publishFinalResponse(...)`。

`examples/DI_NativeProviderSessionSmoke.cpp` 是这个边界的小型 native executable
smoke。它使用 fake backend 和 in-memory dependency I/O，在不启动 MiniNDN 或 NFD
的情况下跑一轮 `/Backbone -> /Head/* -> /Merge` plan：

```bash
./waf build --targets=di-native-provider-session-smoke
build/examples/di-native-provider-session-smoke
```

`examples/DI_NativeOnnxRuntimeSmoke.cpp` 是对应的真实 backend smoke。它加载一个
小型 ONNX 模型，通过 `OnnxRuntimeModelRunner` 在 C++ 中执行，并验证 float32
输出：

```bash
./waf configure --with-examples --with-tests
./waf build --targets=di-native-onnxruntime-smoke
build/examples/di-native-onnxruntime-smoke /tmp/ndnsf-di-add-one.onnx
```

成功时应看到：

```text
NDNSF_DI_NATIVE_ONNXRUNTIME_SMOKE_OK 2,3,4
```

`examples/DI_NativePlanManifestSmoke.cpp` 检查 generated-plan handoff。它会加载
`native-execution-plan.json` 和 `service-manifest.json`，从 manifest 创建 fake
role runners，通过 in-memory dependency I/O 跑一轮 `/Backbone -> /Head/* -> /Merge`
frontier，并验证 tensor-level edge metadata 能驱动 C++ bundle selection：

```bash
./waf build --targets=di-native-plan-manifest-smoke
build/examples/di-native-plan-manifest-smoke \
  /tmp/ndnsf-di-yolo-policy/native-execution-plan.json \
  /tmp/ndnsf-di-yolo-policy/service-manifest.json \
  /AI/YOLO/2x2Inference
```

`examples/DI_NativeProviderExecutable.cpp` 是第一版 native provider executable。
`--check-only` 模式会加载 `native-execution-plan.json`，加载
`service-manifest.json`，通过构造 `OnnxRuntimeModelRunner` materialize 本地 ONNX
artifact path，并把每个 role 注册进 `NativeProviderSession`：

```bash
./waf build --targets=di-native-provider
build/examples/di-native-provider \
  --plan /tmp/ndnsf-di-yolo-policy/native-execution-plan.json \
  --manifest /tmp/ndnsf-di-yolo-policy/service-manifest.json \
  --service /AI/YOLO/2x2Inference \
  --provider /NDNSF-DistributeInference/example/provider/A \
  --workers 4 \
  --check-only
```

`--serve` 模式会把同一个 native session 注册为 NDNSF collaboration provider。
它仍然使用普通 NDNSF permissions、tokens、ACK/Selection/Response、planned
large-data activation names 和 `ServiceProvider::CollaborationContext`；只有
provider execution hot path 变成 native C++：

```bash
build/examples/di-native-provider \
  --serve \
  --plan /tmp/ndnsf-di-yolo-policy/native-execution-plan.json \
  --manifest /tmp/ndnsf-di-yolo-policy/service-manifest.json \
  --service /AI/YOLO/2x2Inference \
  --provider /NDNSF-DistributeInference/example/provider/A \
  --group /NDNSF-DistributeInference/example/group \
  --controller /NDNSF-DistributeInference/example/controller \
  --roles /Head/Shard/0 \
  --workers 4
```

顶层 `wscript` 会检查可选的 `onnxruntime` pkg-config package。存在 C++ dev 包时，
unit-test target 会链接 native adapter；不存在时，注册 `onnxruntime` backend 仍会
给出明确 runtime error，而不是悄悄退回 Python。

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp` 是 deployment
plan 的 C++ 镜像。它把 role/dependency metadata、session/provider assignment
转换成 role-local `RoleSpec`，其中包含 deterministic planned activation object
names；如果 segment count 是静态的，还会得到确定的 segment Data names，以及
expected segment counts / expected byte counts。这是 Python policy/deployment
code 进入 native provider runtime 的交接点。预期的 API 边界是：

```text
deployPlan(nativeExecutionPlan, serviceManifest, providerAssignment)
  -> planSessionId + role-local deterministic edge plan

invoke(planSessionId, inputReference, inferenceId)
  -> source role 只收到新的 input reference；providers 复用已经安装好的
     runner specs、role mapping、tensor edge list、object name templates
     和 static segment counts。
```

换句话说，model artifacts、runner metadata、role assignment、edge tensor list、
object-name template 和 static segment count 都属于 deployment/session state，
不应该每次 inference 重新发布。每次 inference 的消息只应该携带变化的 input
reference、inference/session id，以及本次运行需要的安全上下文。

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp` 会把生成的
`native-execution-plan.json` 加载成这些 C++ plan objects。这个 JSON loader
刻意保持很窄，只读取 native hot-path 字段，包括 dependency edge 上的 tensor
names 和 `segmentNaming`，因此 C++ providers 不需要解析完整 deployment YAML。

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeServiceManifest.hpp` 会把生成的
`service-manifest.json` artifact metadata 加载成 `NativeModelRunnerSpec`。它会保留
每个 role 的 backend、kind、本地 artifact path，以及 `input_tensors`、
`output_tensors` 这类 scalar metadata。这样 C++ runtime 可以直接消费
planner/deployment 输出，而不需要在测试里手写 runner specs。第一版故意只支持本地
artifact path materialization；从 repo/largeDataReference 拉取 artifact 是后续
native provider 阶段。

`NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp`
是第一块面向 Core 的 adapter。它把 `DependencyIo` 映射到
`ServiceProvider::CollaborationContext`：planned input names 用 `fetchLarge(...)`
获取，planned output names 用 `publishLargeNamed(...)` 发布。如果某条 edge 有静态
`expectedSegments`，Core fetch path 会立刻把 planned activation object name
展开成精确 segment Interests（`segment=0..N-1`）；如果 edge 是 dynamic shape，
runtime 会从 planned object name 开始，再跟随对象的 final block。只要
`expectedSegments > 0`，exact-segment mode 就是默认行为；只有对比实验需要关闭时
才设置 `NDNSF_COLLAB_LARGE_EXACT_SEGMENT_FETCH=0`。Exact segment fetch 使用
`NDNSF_COLLAB_LARGE_EXACT_SEGMENT_WINDOW`（默认 `64`）和
`NDNSF_COLLAB_LARGE_EXACT_SEGMENT_INTEREST_LIFETIME_MS`（默认 `5000`），这样大
activation bundle 可以快速重试缺失 segment，而不是等到整个 collaboration fetch
timeout。它还不是完整的 C++ ONNX provider，但它明确了边界：DI 执行逻辑可以是
native C++，而 NDNSF Core 继续负责
segmented large data、pending Interests、encryption、permissions 和 wire behavior。

这些 native components 不改变 NDNSF Request/ACK/Selection/Response 语义，也不会
把 AI-specific behavior 加进 NDNSF Core。

距离目标架构的当前状态：

```text
已完成：
  C++ async frontier runtime
  C++ provider role worker pool
  C++ native execution-plan loader
  C++ provider session skeleton
  C++ NDNSF CollaborationContext adapter
  C++ ONNX Runtime CPU runner for float32 raw tensors
  C++ multi-tensor bundle codec for native activation exchange
  native-execution-plan.json 中的 tensor-level dependency metadata
  C++ service-manifest loader to NativeModelRunnerSpec
  C++ generated plan + service manifest smoke
  native provider executable check-only path for local artifacts
  native provider executable 的 NDNSF serving path for local artifacts
  MiniNDN YOLO 2x2 parallel-detect smoke with native compute providers
  source role 的 request initial-input injection

仍缺：
  从 repo/largeDataReference materialize artifact 的 C++ 路径
  面向用户的薄 Python API wrapper 调用 native executable
```

所以项目已经越过 “headers only” 阶段：native C++ runtime 可以本地执行真实 ONNX
计算，也能验证 generated plan，并且可以在 MiniNDN smoke 中替换 YOLO 2x2
parallel-detect compute providers。它还没有完全 C++-first，因为 controller/user
编排和 artifact deployment 仍然是 Python-facing，native providers 当前也只直接消费
本地 artifact paths，还没有直接从 repo-backed artifact references materialize。

### 3. 创建或审查 Policy

Policy 可以手写，可以由 model-specific splitter 生成，也可以由 ONNX-assisted
planner 生成。当前 YOLO 示例已经带有可运行的 policy 文件和 splitter 脚本。
当前 YOLO layout splitter 会把真实 Ultralytics YOLO 模型导出成 sequential
ONNX chunks，并在部署后由同一个 dependency executor 驱动执行。它适合验证
NDNSF-DI 的 role assignment、artifact provisioning、large-data activation
exchange 和 deterministic dependency names，但它还不是真正的 NxM tensor-parallel
splitter。真正的 NxM splitter 必须生成同一 stage 内多个并行 shards，以及明确的
merge/fan-in dependency edges。网络级回归中，生成的 policy 会按 chunk role 数量
生成足够的计算 provider identities，并把 repo provider 单独保留。

`yolo_2x2` splitter 现在提供两种实验性 parallel 模式。较早的
`--parallel-output-shards` 是一个小型正确性脚手架：同一 stage 内的 roles 独立
运行，最后由 `/Merge` role concat 输出 slices，但 Stage-0 shards 会重复上游
YOLO backbone 计算。新的 `--parallel-detect-scale-shards` 更接近真实 YOLO
切割：一个共享 `/Backbone` chunk 只计算一次 backbone/neck，多个并行
`/Head/Shard/*` chunks 运行 YOLO Detect scale 分支，最后由 `/Merge` 解码最终
predictions。它仍然是 model-specific splitter，但已经能验证目标 fan-out/fan-in
dependency executor，而不是假设所有 ONNX 模型都有矩形 shard layout。Merge
fan-in edges 使用 producer-local key scope，例如
`detect-head-shard0-to-merge` 和 `detect-head-shard1-to-merge`，这样 native
C++ hot path 可以独立预取和保存每个 planned input，而不是把多个 producer
折叠到同一个 scope。

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

YOLO layout smoke 也可以在 parallel-detect layout 下用 native C++ provider
executable 替换 Python compute providers：

```bash
sudo -E python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-requests 1
```

成功输出应包含：

```text
YOLO_SPLIT_RESULT ... ok=true
YOLO_2X2_RESULT ... ok=true
PYTORCH_2X2_RESULT ... ok=true
```

统一 runner 也可以运行指定 cases。`yolo-layout` 是 MiniNDN 网络级
custom-layout 回归；`yolo-layout-local` 是快速的非 MiniNDN layout/policy
smoke：

```bash
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case all
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case yolo-layout --layout 2x3
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case yolo-layout-local --layout 3x2
```

当前已经验证的 YOLO layout 覆盖范围：

```text
2x2  历史默认网络级回归
2x3  MiniNDN 网络级回归，每个生成 role 一个 provider
3x2  MiniNDN 网络级回归，每个生成 role 一个 provider
1x3  本地 export/policy/ONNX correctness smoke
3x3  本地 export/policy/ONNX correctness smoke；作为 release baseline 前应先跑 yolo-layout
2x2 parallel-detect-scale  本地 ONNX correctness smoke，角色为 /Backbone、/Head/*、/Merge
2x3 parallel-detect-scale  本地 policy/ONNX smoke；MiniNDN smoke 是网络级 baseline
```

历史 YOLO 回归里的 layout 写作 `ROWSxCOLS`，但生成 metadata 会明确标注
`layout_semantics: pipeline-sequential-chunks` 和
`stage_shards_parallel: false`。不要把这些 YOLO sequential chunk 数字作为真正
NxM parallel sharding 的性能证据。当前 planner 仍生成 YOLO-specific sequential
chunk plan；它还不是“任意 ONNX graph 到任意 parallel distributed layout”的完全
通用 planner。

生成实验性 parallel-output prototype：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --layout 2x2 \
  --parallel-output-shards \
  --out-dir /tmp/ndnsf-yolo-parallel-2x2
```

生成 YOLO Detect-scale DAG splitter：

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --layout 2x3 \
  --parallel-detect-scale-shards \
  --out-dir /tmp/ndnsf-yolo-detect-scale-2x3
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

如果已经生成了 `DistributedInferencePlan`，并且要在同一部署 layout 上反复推理，
推荐显式使用 plan-session API：

```python
session = client.deploy_plan(plan)

for image in images:
    result = client.invoke_plan(
        session,
        encode_image_for_yolo(image),
        ack_timeout_ms=300,
        timeout_ms=10000,
    )
```

`deploy_plan(...)` 会安装或复用静态 model/runtime artifacts、key-scope
material、role metadata、dependency tensor lists、object-name templates 和
static segment-count hints。`invoke_plan(...)` 只发送变化的 input
payload/reference，以及正常 NDNSF collaboration request。

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

这表示 role-level edge 携带一个 large activation object，里面包含列出的 tensors。Request
本身只应携带小 reference。Images、activations、model shards 和 runtime bundles 都不能作为
大型 inline NDNSF payload 塞进 request/response。只要超过 inline/single-segment 阈值，
DI 就使用 NDNSF Core 的 large-data abstraction：`LargeDataReference` 指向 hybrid
AES-GCM 加密、签名并分段的 NDN Data，或者指向保存同类 opaque segmented Data 的
NDNSF-DistributedRepo manifest。压缩或降低精度可以作为应用层模型质量/带宽 tradeoff，
但不应成为大对象传输机制本身。

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
/tmp/ndnsf-di-yolo-policy/native-execution-plan.json
/tmp/ndnsf-di-yolo-policy/native-execution-plan.json.sha256
```

Service manifest 是 service-to-model 和 service-to-dependency 映射的 canonical JSON 形式。`.sha256` 文件只是部署工具的本地 fingerprint，不是安全签名。安全性来自把 manifest 作为 NDN Data 发布并验证 Data signature。

`native-execution-plan.json` 比 service manifest 更窄。它是交给 C++ hot path 的
handoff artifact，只包含构造 native `RoleSpec` 所需的字段：service name、roles、
dependency producers/consumers、key scopes、topic prefixes、deterministic
object-name templates、expected segment counts 和 expected byte counts。它由
policy 生成；部署者应该修改 policy 或 splitter 输入，而不是手工改这个文件。

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

`yolo_2x2` 示例现在明确是 YOLO sequential-chunk regression。历史默认是
2x2：两个 pipeline stages，每个 stage 用两个顺序 chunks 表示。现在同一个
splitter 也接受 `1x3`、`2x3`、`3x2`、`3x3` 等自定义 chunk layout。它包含真实的
NDNSF-DI distributed execution path，而不只是 repository smoke test；但它还没有实现
同一 stage 内多个 shards 并行运行的 tensor-parallel YOLO split。

如果要实验真实 parallel graph，优先使用
`split_model.py --parallel-detect-scale-shards`。它会生成共享 `/Backbone` role、
并行 YOLO Detect scale head roles，以及 `/Merge` decode role。这是
model-specific YOLO DAG，不是完全通用的矩形 `N x M` mapper，但它避免重复
backbone 计算，是当前 NDNSF-DI 中最接近真实并行模型执行的 YOLO 示例。它生成
的 policy 和 `native-execution-plan.json` 会保持每条 fan-in edge scope 唯一，
因此 `/Merge` 会收到每个 head shard 对应的一个 planned input，并能批量等待
所有 required inputs。

较早的 `split_model.py --parallel-output-shards` 仍保留为最小 fan-in correctness
脚手架。由于它的 Stage-0 shards 会重复上游 YOLO 计算，不应把它作为性能论证。

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
suffix，不依赖 YOLO。只有当 plan 能给出确定的 dependency topic 时才应该使用；
否则 handler 仍然可以继续显式调用 `wait_one(...)` 和
`fetch_large_reference(...)`。新代码应该用标准 NDNSF large-data reference
payload 发布 dependency reference，而不是在 collaboration message 中放裸 Data name。

当前 prefetch 路径在 provider handler 开始时启动，因此可以把本地准备工作和
等待上游 activation object 的过程重叠起来。对于计划内 ONNX dependencies，
policy 可以包含 `object_name_template`、`expected_segments` 和 `expected_bytes`。
运行时会用当前 run/session id、key scope、producer role、producer provider 和
bundle sequence 填充这个 template。Producer 会把 activation object 发布到这个
确定性名字。对于 static-shape edge，DI 会把 segment count 视为 execution plan
的一部分：consumer 在 role 启动时立刻把 object name 展开成 planned segment
Data names，并预先发出所有 segment Interests。对于 dynamic-shape edge，
consumer 先取 segment 0，再跟随对象的 final block id。这是预期的数据流优化：
dependency traffic 仍然是 NDN large Data，不是 provider 之间互相发 service
invocation；应用也不再需要等待额外的 activation-reference control message 才能
开始取 planned inputs。
NDNSF core 的 provider 数据服务路径也会为 IMS-served Data 保留 pending
Interests。因此下游 role 可以在上游 role 完成发布前，对确定性 activation name
先发 Interest；如果该 Interest 还没有超过自己的 InterestLifetime，上游 provider
会在 activation segments 插入后立即回复。这就是 DI 在 policy 中正式写入
`object_name_template` 和 segment-count hint 的原因，而不是让应用私下猜一个
碰巧可 fetch 的 Data name。生成的 `native-execution-plan` 也会记录
`segmentNaming` metadata，这样部署产物本身就能被审查，而不必阅读 runtime code。
native dependency I/O 路径不会再额外发布 activation-ready notification。Consumer
发现上游 activation 的方式，是已经挂起的 Interest 被满足；如果 edge 是 dynamic，
才通过普通 NDN segmented retrieval 的 segment 0 / final block fallback 继续取。

同样的 reference metadata 也会附加到 execution spec 里的 repo-backed
model/runtime artifacts 上，尽管这些 artifact 的 bytes 仍通过 repo 的
manifest-aware object API 获取。
Provider materialize artifact 时会优先读取这个 reference，然后再 fallback 到旧的
`repoManifest`、chunk-list 或单个 Data-name 字段。

在同一次 distributed-inference run 内，provider 之间不会再通过新的 NDNSF
service invocation 互相调用。外层 user request 负责启动 run 并完成 role
assignment；之后每个 provider 只根据 dependency graph 等待自己的 input edges，
在 activation reference 出现后 fetch 对应 large-data object，运行本地 ONNX
chunk，并为下游 role 发布 output-edge activation reference。因此 provider 之间的
协作是 dataflow-driven，不是 Request/ACK/Selection 服务调用链。

对于 ONNX chunks，推荐的 provider-side 路径是
`execute_onnx_dependency_chunk(...)`。它会使用当前 role 的 dependency view
自动收集所有 input-edge tensor bundles，按 tensor name 合并，运行被分配的 ONNX
chunk，然后为每条声明的 output edge 发布一个 tensor bundle。YOLO provider
现在已经使用这个 dependency-driven executor：它根据 role-local dependency view
判断当前 role 是首块、中间块还是终块。YOLO-specific 代码只负责准备第一块的
image input，以及编码最后的 prediction response。这样 runtime path 不再把 2x2
pipeline chain 写死在 provider 中，而是可以由 policy 驱动执行 `1x3`、`2x3`、
`3x2`、`3x3` 等自定义 layout。

executor 会在每个 provider 进程内按 model 文件大小和 SHA-256 digest 缓存 ONNX
Runtime session。即使 artifact 被 materialize 到新的临时路径，只要模型内容相同，
连续执行同一个 role 时仍然可以复用 session；如果模型文件重新生成，digest 会变化。
这个优化不改变 APP API，只减少重复 request 中的 session 初始化开销。

client 也会在同一进程内缓存 plan-level references。对于同一个 plan fingerprint，
重复 inference request 会复用 artifact spec 和 scope key 的 large-data reference，
不会每次都重新发布相同的 model/runtime metadata；每次 inference 只发布新的输入
reference。长期运行的服务后续应增加显式 plan-session rotation，但默认 APP API
不需要变化。

为了做性能分析，executor 会输出：

```text
NDNSF_DI_ONNX_TIMING
NDNSF_DI_DEPENDENCY_INPUT_TIMING
NDNSF_DI_DEPENDENCY_OUTPUT_TIMING
NDNSF_DI_PLAN_CACHE
```

这些日志把 latency 拆成 input collection、activation reference wait、large-object
fetch、tensor decode、ONNX session lookup、ONNX run 和 output publish。
dependency input/output timing 会带上 DI session identifier，因此长时间运行时可以
按单次 inference request 分组，而不是把 cold path 和 warm path 混在一起。当
splitter 能估计时，这些日志也会记录实际 payload bytes 以及 planned segment/byte
counts。它们的目的是指导泛化的数据流优化，而不是针对某一个 YOLO layout 手工调参。

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

同一个脚本还会在 `results/yolo_<layout>_minindn_quick/` 下写入回归统计：

```text
inference-latency-stats.json
traffic-stats.json
nfd-data-stats.json
plan-cache-stats.json
onnx-timing-stats.json
dependency-input-timing-stats.json
dependency-output-timing-stats.json
dependency-volume-stats.json
dependency-frontier-timing-stats.json
```

这些文件记录端到端 latency、节点流量计数、NFD Data 计数、plan cache 命中、
ONNX session/run 时间、每条 activation edge 的 reference wait/fetch timing，以及
每条 activation edge 的 publish timing、planned-vs-actual activation 数据量，以及
producer-output-ready 到 consumer-first-segment 的 frontier timing。
后续分析瓶颈时，应根据这些统计判断问题是在 ACK/selection、artifact 发布、
ONNX 执行、activation reference wait、segmented fetch、activation publish、
frontier scheduling，还是 tensor decode。

面向 AI 的 MiniNDN 回归统一使用
`Experiments/Topology/AI_Lab.conf`。这个拓扑使用 5 个节点：`memphis` 作为
user/controller 侧 hub，`ucla`、`arizona`、`wustl`、`neu` 四个 provider 节点
以 1 ms、1 Gbps 链路直接连接到 `memphis`。它比旧的 `AI_testbed.conf` 更小，
目的是降低 MiniNDN/NFD 进程数量带来的 CPU scheduling 噪声。只有当实验目标就是
更大规模多节点拓扑时，才使用 `AI_testbed.conf`。不要在 MiniNDN 命令外层设置全局
`NDN_LOG`：NFD 会继承这个环境变量，而应用风格的 logging filter 可能导致 NFD
启动失败。低噪声 benchmark 应使用下面的脚本参数和
`NDNSF_TIMELINE_TRACE_SAMPLE_RATE`。

当前 2x2 native-provider benchmark recipe 是：

这个 MiniNDN 初始化流程会在每个节点独立的 `/tmp/Minindn/<node>` keychain 中，为
每个 DI user/provider/controller identity 安装两套 root-signed key：RSA key 保持为
default encryption certificate，用于 NAC-ABE/PermissionResponse unwrap；ECDSA key
作为存在时优先使用的 signing certificate。runtime 会在进程启动时选择并缓存这些
certificate role，不会在每次 request 或 Data packet 签名时重新扫描 keychain。如果启动时没有
ECDSA key，NDNSF 会自动回退到 RSA certificate 签名，并一直保持到进程重启。只有在比较
dual-certificate signing path 与旧 RSA-only 行为时，才使用 `--single-rsa-certs` 做对照运行。

```bash
sudo mn -c >/tmp/ndnsf_mn_cleanup.log 2>&1 || true
sleep 3
sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=1 \
  python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --preflight-requests 2 \
  --ack-timeout-ms 300 \
  --timeout-ms 10000 \
  --quiet-perf-logs \
  --results-dir results/yolo_2x2_native_provider_selection_direct_prefetch_60s_minimal
```

`results/yolo_2x2_native_provider_selection_direct_prefetch_60s_minimal` 中的代表性运行输出
`YOLO_2X2_NATIVE_PROVIDERS_MININDN_OK`，60 个 measured warm requests 的
p50 为 66.91 ms，p95 为 95.61 ms，mean 为 67.02 ms。丢弃第一个 measured
warm request 后，steady warm p50 为 66.76 ms，p95 为 95.64 ms，mean 为
66.62 ms。NFD 在 warm-side 62 个被脚本计数的请求上观察到 1212 个 incoming/outgoing
Data packets，即每个 observed request 19.55 个 Data packets；按 measured window
折算，每次 inference 约 20.20 个 NFD out Data packets，约 451 KB NFD
`nOutBytes`。和前面一样，byte average 只是近似 transport-size ratio，因为 NFD
byte counters 包含测量 face 上的所有包类型。

当前 native provider 路径在 DI MiniNDN native-provider 实验中默认使用
deterministic activation names、active-put segment delivery 和 direct Selection
prefetch。`NDNSF_COLLAB_LARGE_ACTIVE_PUT=1` 让 Provider 生成 activation segments
后同时放入 IMS，并立即 `put` 到 NFD，使已经预挂的 exact segment Interests 能直接
被满足，而不再等待额外的应用层通知路径。`NDNSF_SELECTION_DIRECT_PREFETCH=1` 让每个
provider 在发布 ACK 后，预先 express 指向可预测 SelectionMessage name 的 Interest；
user 仍然通过 SVS 发布同一个 Selection，同时也在同一个 selection name 下 put 一个
签名 Data packet。Provider 收到这个 Data 后进入同一个 selection handler、token 检查和
hybrid decrypt 路径，之后到达的 SVS duplicate 由正常 duplicate guard 抑制。Core
把这个能力放在环境变量后面；DI native-provider runner 在这个测量实验中启用它。只有做
A/B 诊断时才建议用 `NDNSF_COLLAB_LARGE_ACTIVE_PUT=0` 或
`NDNSF_SELECTION_DIRECT_PREFETCH=0` 关闭这些行为。

这个 minimal recipe 有意关闭 dependency/control/crypto 细粒度 timing，避免 tracing
本身支配 latency。应把这个 60 秒形状作为性能基线。如果要解释瓶颈，仍然保持
同样的 60 秒 measured window，只额外打开窄采样 instrumentation，例如
`--control-timing`、`--dependency-timing` 或 `--crypto-timing`；这类 diagnostic
run 用于分析剩余的 outer ACK/Selection/Response 和 SVS delivery cost，不应直接
和 minimal latency baseline 比较。

保留了一次 sampled control-timing diagnostic run：
`results/yolo_2x2_native_provider_selection_direct_prefetch_60s_control`。它使用同样的
60 秒形状并打开 `--control-timing`，60 个 measured warm requests 的 p50 为
78.72 ms，p95 为 96.92 ms，steady-after-first p50 为 78.64 ms。该运行测得
p50 request latency 为 78.66 ms，provider dataflow 为 20.00 ms，role run
window 为 16.00 ms，ONNX run sum 为 2.52 ms，outer-control residual 为
51.93 ms。Activation reference wait 对所有 planned inputs 都是 0.00 ms，252 个
native dataflow inputs 和 outputs 全部匹配 deterministic plan，activation publish
每条 edge 的 p50 约 1.21 ms。Control propagation 仍然可见：request SVS 到
provider request observation 约 18.02 ms p50，ACK SVS 到 user pre-decrypt 约
7.62 ms p50，sampled rows 中 selection delivery 到 provider selection handling
约 15.73 ms p50，response SVS 到 user observation 约 15.69 ms p50。Provider 侧
request admission、selection decrypt/dispatch 和 response publication 仍然是亚毫秒级。
换句话说，ONNX 和 activation publication 已经不是瓶颈；最大的剩余 residual 是外层
ACK/Selection/Response control path，以及 request、ACK、selection、response 周围的
NFD/SVS delivery。

Control-timing run 现在还会打印 `YOLO_LAYOUT_SELECTION_DIRECT_PREFETCH`，并写出
`selection-direct-prefetch-stats.json`。这个诊断会把 user 侧
`SELECTION_DIRECT_PUT` 与每个 provider 侧的 `SELECTION_DIRECT_PREFETCH_DATA` 和
`SELECTION_OBSERVED` 配对，用来回答一个很窄的问题：direct Selection Data 是否卡在
`Face::put()` 到 provider handler 之间。一次短 packet-trace smoke
`results/yolo_2x2_selection_direct_put_packet_trace_smoke` 显示，在
`SELECTION_DIRECT_PUT` 后约 3 ms，user 节点就能看到第一批较大的 NDN/UDP 包；
provider 节点也在之后几毫秒内观察到对应流量。因此目前没有证据说明 direct
Selection path 上存在几十毫秒级的 `Face::put()` 卡顿。

如果问题是 ndn-svs 自身是否在 Sync processing 内部消耗了毫秒级时间，可以用
`--svs-internal-timing` 做短诊断。这个选项只打开 `ndn_svs.SyncTimeline` 和
`ndn_svs.SVSPubSub` 的 TRACE，并写出 `svs-internal-timing-stats.json`；它不是
baseline benchmark 模式。一次短 smoke
`results/yolo_2x2_svs_mapping_timing_smoke` 显示：
`sync_worker_p50_ms=0.000`、`sync_main_blocked_p50_ms=0.000`、
`sync_encode_p50_us=66.0`、`sync_sign_p50_us=702.5`、
`sync_face_put_p50_us=11.0`。Extra mapping piggyback 也是有界的：34 次
extra-block build 总共携带 28 个 mapping entries 和 24 个 piggyback Data packets，
extra-block size p50 约 1145 bytes，并且没有 network mapping fetch。更细的
mapping 专项拆分也很小：mapping block 构造
`extra_mapping_build_total_p50_us=16.5`，接收侧 mapping parse/process
`extra_mapping_parse_total_p50_us=244.5`、
`extra_mapping_parse_process_p50_us=157.5`。也就是说，目前证据不支持
“ndn-svs 每条消息有很大的 CPU bottleneck”或“每次重复携带全历史 mapping payload”。
剩余成本主要来自外层控制面 Request -> ACK -> Selection -> Response 在当前
MiniNDN/NFD/SVS 路径上的多轮 delivery。

对于 activation transport，打开 dependency-timing 后现在还会打印
`YOLO_LAYOUT_ACTIVATION_SEGMENT_TIMELINE`，并写出
`activation-segment-timeline-stats.json`。这个诊断会按 deterministic segment name
配对 consumer 侧 `segment_interest`、producer 侧 `segment_active_put`，以及
consumer 侧 `segment_received/segment_validated`。同一次运行还会写出
`dependency-edge-ndnping-rtt-stats.json`，从 consumer provider 节点到 producer
provider prefix 测量每条 planned dependency edge 的 ndnping RTT。解释 activation
delivery 时应优先看这个 edge RTT，而不能只看 user-to-provider RTT；在 AI_Lab 中，
activation edges 的观测 RTT 可能明显高于 memphis-to-provider baseline。短诊断 smoke
`results/yolo_2x2_activation_segment_timeline_smoke` 显示：
`interest_to_data_p50_ms=83.81`，但其中 `interest_to_active_put_p50_ms=80.27`，
`active_put_to_data_p50_ms=8.51`。也就是说，看起来很大的 activation fetch 时间
主要是预挂 Interest 后等待上游 role 完成计算并发布 segment；producer active-put
之后的 Data delivery 和 validation 小得多。因此解读 dependency fetch p50 时要区分：
prefetch overlap 不是阻塞式网络传输时间。

另一次同步本地 SVS publish 的对照运行保存在
`results/yolo_2x2_native_provider_active_put_60s_sync_publish_control`。它得到
warm p50 95.28 ms、p95 115.27 ms，并把 outer-control residual 从 78.56 ms
降到 73.59 ms。这是有用的 A/B 结果，但不是根治：selection SVS 到 provider
observation 仍约为 33.67 ms p50。因此，接下来不应继续纠结压缩
SelectionMessage payload，也不应默认修改 ndn-svs timing。这个结果推动了上面记录的
direct Selection prefetch：保持同一个 selection name、permission、token 和
hybrid encryption 语义，但让 provider 直接预取可预测的 Selection Data。

如果问题具体是“延迟是否来自 NFD/SVS 包传播”，不要先改 ndn-svs timing，也不要
先开全量 packet trace。应先运行不带抓包的窄 60 秒 control diagnostic：

```bash
sudo mn -c >/tmp/ndnsf_mn_cleanup.log 2>&1 || true
sleep 3
sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=10 \
  python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --preflight-requests 2 \
  --ack-timeout-ms 300 \
  --timeout-ms 10000 \
  --quiet-perf-logs \
  --control-timing \
  --results-dir results/yolo_2x2_native_provider_selection_direct_prefetch_60s_control
```

如果问题是 40-100 ms 的 latency 波动是否和 MiniNDN/NFD forwarding pressure 有关，
应保持同样的 60 秒 measured window，只额外打开 warm RTT/NFD monitor：

```bash
sudo mn -c >/tmp/ndnsf_mn_cleanup.log 2>&1 || true
sleep 3
sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=1 \
  python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --preflight-requests 3 \
  --ack-timeout-ms 300 \
  --timeout-ms 10000 \
  --quiet-perf-logs \
  --control-timing \
  --warm-rtt-monitor-interval-s 1 \
  --results-dir results/yolo_2x2_warm_rtt_nfd_monitor_60s
```

这个模式会写出 `warm-rtt-nfd-monitor.json`。每条 measured inference result
都会携带 `epoch_start_s` 和 `epoch_end_s`；monitor 会把每次 request 和最近的
user-to-provider ndnping-style RTT sample、NFD network-face Data counter delta
对齐。它每个采样间隔会额外产生四条 ndnping probe，所以只用于相关性诊断，不作为
canonical low-overhead benchmark。
配合 `--control-timing` 时，脚本还会写出
`outer-control-rtt-correlation-stats.json`，把每个 request 同 outer
ACK/Selection/Response timing 对齐，并报告哪些阶段和 inference latency 的相关性最强。
这个 correlation run 应使用 `NDNSF_TIMELINE_TRACE_SAMPLE_RATE=1`；`10`
这类抽样率适合低开销汇总，但不会为每次 inference 都保留 control row。
同一次 run 还会写出 `native-session-breakdown-stats.json`，按 request/session id
把 user outer-control row、provider role timing、dependency fetch timing 和
ONNX timing 合并。分析单个 outlier 时优先看这个文件：它会拆出 request 到 first ACK、
ACK 到 selection、selection 到最终 response、final merge execution，以及最慢的
activation dependency fetch。当前 2x2 native-provider 诊断中，RTT/NFD counter 漂移
和 latency 的相关性较弱；较大的波动通常来自 outer ACK/Selection 传播，或者 final
merge 等某条 activation fetch。

如果要比较 RSA-only signing 和 RSA+ECDSA split certificates，不要把多个独立
MiniNDN run 当成主要证据。MiniNDN/NFD 的 RTT 漂移可能比签名策略差异还大。
应在同一个拓扑里连续运行签名模式：

```bash
sudo mn -c >/tmp/ndnsf_mn_cleanup.log 2>&1 || true
sleep 3
sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=0 \
  python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --preflight-requests 3 \
  --ack-timeout-ms 300 \
  --timeout-ms 10000 \
  --quiet-perf-logs \
  --signing-ab-phases rsa,ecdsa,rsa \
  --results-dir results/yolo_2x2_cert_ab_same_topology_60s
```

这个模式会安装双 RSA/ECDSA 证书，保持同一个 controller、repo、topology 和
NFD instances，只在每个 signing phase 之间重启 compute providers。
`inference-latency-stats.json` 中的 phase 标签是 `warm-rsa-1`、
`warm-ecdsa-2` 和 `warm-rsa-3`。短 run 只能当 smoke test；要作为证据仍应使用
上面的 60 秒 measured window。单独的 `--single-rsa-certs` run 仍然适合作为兼容性
smoke，但如果它运行在不同的 MiniNDN RTT baseline 下，就不是公平的 signing
performance 对照。

`splitSigning` 是证书选择机制，不是 SelectionMessage 模式。RSA 仍然作为
NAC-ABE / permission unwrap 使用的 encryption certificate；如果存在 EC 证书且没有设置
`NDNSF_DISABLE_SPLIT_SIGNING`，EC 证书会作为 signing certificate。解释 signing
benchmark 之前，先检查 user/provider 日志里的 `NDNSF_CERT_SELECTION` 行：
`splitSigning=true` 表示 RSA-encryption + EC-signing，`splitSigning=false`
表示同一个 RSA 证书同时用于 encryption 和 signing。不要把它叫成 “split
selection”；selection-message 行为是普通 selection 或 compact selection 的另一套机制。

对于 signing A/B/A run，latency samples 才是主要信号。`nfd-data-stats.json`
和 `traffic-stats.json` 的 phase delta 覆盖整个 phase window，包括
`--preflight-requests` 以及首次 certificate、repo 或 control fetch。短 phase 因此可能看到
repo/certificate-publisher 节点多出 Data packets，即使 measured inference latency 没有变化。
`*PerObservedRequest` 更适合作为 phase-average transport 视图；只有当
`preflightRequestCount` 为 0，或者你明确想把 preflight/control traffic 摊到 measured requests
上时，才把 `*PerMeasuredInference` 当成单次 measured inference 的近似。

只有当需要定位无法理解或不必要的 NDN name、stale Sync traffic，或者
startup/repo/keychain 行为时，才把 full packet trace 当作最后手段。不要把 packet
trace run 当成 latency benchmark；tcpdump、pcap 解码和 summary generation 会扰动
MiniNDN 运行。如果确实需要抓包：

```bash
sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=10 \
  python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --preflight-requests 3 \
  --ack-timeout-ms 300 \
  --timeout-ms 10000 \
  --quiet-perf-logs \
  --control-timing \
  --ndn-packet-trace \
  --ndn-packet-trace-nodes memphis,ucla,arizona,wustl,neu \
  --results-dir results/yolo_2x2_control_packet_trace_60s
```

`--ndn-packet-trace` 会在选中的 MiniNDN 节点上启动 full-snaplen `tcpdump`，
运行结束后用 `ndndump` 解码 pcap。脚本会写出
`ndn-packet-trace-summary.json`，其中包含每个观察到的 NDNSF/SVS name、每个
节点第一次/最后一次观察到它的时间戳，以及观察到的 IP endpoint 方向。这是外部
packet-level diagnostic：它能说明某个节点的 NFD-facing capture 何时看到某个
Sync Interest 或 Data name，但不能解码 SVS payload 里的 NDNSF
Request/ACK/Selection/Response 语义消息。因此应把它和
`svs-control-propagation-stats.json`、provider lifecycle logs 一起使用。
脚本现在默认 `--ndn-packet-trace-window=warm`，让 pcap 聚焦 measured inference
window；只有在调试 keychain、artifact deployment、repo 或 startup synchronization
时，才建议显式改用 `all`。

较旧的 2x3 Python-provider parallel-detect-scale baseline 建议用 60 秒 warm window
和显式 Python worker 参数运行：

```bash
PYTHONPATH=NDNSF-DistributedInference:$PYTHONPATH \
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py \
  --case yolo-layout \
  --layout 2x3 \
  --parallel-detect-scale-shards \
  --cold-requests 1 \
  --warm-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --ack-timeout-ms 1500 \
  --timeout-ms 60000 \
  --provider-handler-workers 4 \
  --user-async-workers 4
```

当前代表性 native-provider MiniNDN 诊断保存在
`results/yolo_2x2_ailab_dependency_control_60s_latest`。它使用 generated native
execution plan、C++ native provider executable、本地 ONNX Runtime role runner、
deterministic activation name、direct Selection prefetch 和 dependency timing。
这个结果对应小型 `yolo26n.pt` workload，input size 为 32；它应作为 tiny-model
DI 的 canonical baseline。60 个 measured warm requests 的 p50 为 53.75 ms，
p95 为 79.49 ms，max 为 93.49 ms。同一次运行记录到每次 measured warm inference
约 23.98 个 NFD Data
packet，以及约 143.81 KB NFD `nOutBytes`。`nfd-data-stats.json` 会输出
`data_packets_per_inference` 和 `avg_data_packet_bytes`；后者只是近似
transport-size ratio，因为 NFD byte counters 包含测量 face 上的所有包类型。
`traffic-stats.json` 也会输出每次 inference 的 total node bytes。

同一次运行确认了 native provider 路径里的 planned-name prefetch：256/256 个
dependency fetch 都使用确定性 planned name，ref-wait p50 为 0.00 ms，
provider input-fetch-wait p50 约 13.69 ms。对于这个很小的模型，ONNX 本身不是
p50 主瓶颈：整条 role chain 的 ONNX run sum p50 约 2.37 ms，而 dependency fetch
max p50 约 28.41 ms，outer control residual p50 约 24.94 ms。相比旧
AI_testbed 诊断，AI_Lab 明显降低了 MiniNDN RTT 噪声和 outer-control residual，
但 activation delivery 仍然有几十毫秒成本。当前剩余 latency 主要来自 NDNSF 外层
Request/ACK/Selection/Response 传播和 activation dependency fetch wait，而不是本地
ONNX 执行。

比较后续 benchmark 时，必须先看脚本输出的 `YOLO_LAYOUT_RUN_CONFIG`。尤其是
`yolo26n.pt --input-size 32` 和 `yolov8n.pt --input-size 32` 是不同 workload。
当前一次低开销 60 秒 `yolo26n.pt` 运行保存在
`results/yolo26n_32_2x2_current_default_60s_latest`，warm p50 为 63.57 ms，
p95 为 80.54 ms；该次运行的 MiniNDN edge RTT 样本高于 canonical baseline。
一次 60 秒窄 timing 运行保存在
`results/yolo26n_32_2x2_current_timing_60s_latest`，p50 为 62.14 ms，并且结构上
和 canonical result 一致：provider dataflow p50 约 30.00 ms，
dependency-fetch-max p50 约 28.82 ms，outer-control residual p50 约 33.34 ms，
ONNX run sum p50 约 2.99 ms。相比之下，`yolov8n.pt --input-size 32` 在
`results/yolov8n_32_2x2_current_default_60s_latest` 中输出 shape 为
`(1, 84, 21)`，warm p50 为 85.98 ms。不要把它当成相对 `yolo26n.pt`
tiny-model baseline 的退化；它是更重的模型和输出形状，应作为单独 benchmark line
跟踪。

分析 outer control 时，应使用窄 `--control-timing` 运行，而不是 packet trace。
`results/yolo26n_32_2x2_current_control_60s_latest` 得到 warm p50 58.10 ms、
p95 84.53 ms。该运行显示，SVS propagation 本身不是 p50 主瓶颈：Request SVS 到
provider 收到 request 约 3.33 ms p50，ACK SVS 到 user pre-decrypt 约
3.23 ms p50，Selection SVS 到 provider 收到 selection 约 3.26 ms p50，
Response SVS 到 user observe 约 3.28 ms p50。user 在 request publish 后约
7.06 ms p50 看到 first ACK，ACK matching 约 0.71 ms p50；selected providers
在 user put selection Data 后约 3.02 ms p50 观察到 direct Selection prefetch
Data。更大的 `Selection -> Response` 区间主要包含 final provider execution/dataflow：
final-provider execution-start-to-done 约 30.77 ms p50，而 selection-received 到
execution-start、response decrypt 到 callback 在 p50 下都低于 1 ms。因此，下一步优化
应优先看 final Merge/dataflow 和 activation fetch latency，而不是继续压缩
SelectionMessage 体积。

另一次 edge RTT 诊断运行保存在
`results/yolo_2x2_ailab_edge_rtt_60s_latest`。它使用同一个 AI_Lab 拓扑，但额外
在 generated dependency edges 上启动短 ndnping probe，因此应被视为解释延迟来源
的诊断运行，而不是替代上面的 canonical baseline。该运行得到 warm p50 63.86 ms、
p95 85.64 ms。更关键的是，它显示 dependency fetch p50 为 20.90 ms，
dependency fetch max p50 为 29.53 ms，provider dataflow p50 为 31.00 ms。edge RTT
p50 样本分别为：Backbone -> Head/Shard/0 约 16.75 ms，Backbone -> Head/Shard/1
约 15.44 ms，Head/Shard/0 -> Merge 约 20.35 ms，Head/Shard/1 -> Merge 约
5.60 ms。这说明解释 activation latency 时必须使用 provider-to-provider edge RTT，
而不能只看 user-to-provider baseline。

更细的 outer-control breakdown 运行保存在
`results/yolo_2x2_ailab_outer_breakdown_60s_latest`。它使用同样的 60 秒 AI_Lab
recipe，并把 Selection -> final provider -> Response -> User 拆开。该运行得到
warm p50 63.52 ms、p95 87.11 ms。warm SVS propagation p50 分别约为：Request
3.99 ms、ACK 3.89 ms、Selection 4.41 ms、Response 3.95 ms。final-provider
breakdown 显示：Selection 发布到 final provider 收到 Selection p50 为 5.05 ms，
final provider 发布 Response 到 user 观察到 Response p50 为 4.06 ms，而 final
provider 收到 Selection 到发布 Response p50 约为 33.22 ms。因此在这个诊断中，
较长的 `selection_to_response` 不是主要来自 Selection 或 Response delivery，而是
final Merge provider 的 dataflow/handler 路径；该路径主要由从 head shards 获取
dependency activation 的等待主导。

同一个脚本也会启用 `NDNSF_COLLAB_LARGE_FETCH_TIMING=1` 并写出
`collab-large-fetch-stats.json`。这个文件记录每次 collaboration large-data fetch
在 Core SegmentFetcher 层的 elapsed time、encoded object size 和 InterestLifetime。
把它和 `dependency-input-timing-stats.json` 对照，就能区分 native segmented fetch、
provider scheduling 和 tensor decode 各自的成本。它还会启用
`NDNSF_PENDING_IMS_TIMING=1` 并写出 `pending-ims-timing-stats.json`，记录可预测
activation Interest 是否在 Data 插入 in-memory storage 之前到达 producer。上面的
native-provider 代表性运行中，Core-level collaboration fetch 256/256 次完成、无错误，
elapsed p50 为 16.70 ms，p95 为 33.75 ms，first-segment p50 为 16.62 ms，
encoded object p50 为 3839 bytes，received/validated segment p50 为 1，
InterestLifetime p50 为 10000 ms。`activation-segment-timeline-stats.json`
显示 segment interest-to-Data p50 为 18.46 ms；其中 interest-to-active-put p50
为 10.30 ms，active-put-to-Data p50 为 8.90 ms，Data-to-validated p50 为
0.07 ms。`pending-ims-timing-stats.json` 显示 245 个 pending activation
Interests 后续被满足，pending-age p50 为 8.03 ms。同一次运行还写出了
`dependency-frontier-timing-stats.json`：256 个 output/fetch pair 通过确定性
Data name 成功 join，producer-output-ready 到 consumer-first-segment p50 为
9.00 ms，publish-done 到 consumer-first-segment p50 为 9.00 ms，
producer-output-ready 到 fetch-complete p50 为 9.00 ms。这说明 planned prefetch
确实在 activation Data 产生前到达了 producer。剩余成本主要是 outer control
propagation、stage-frontier scheduling、active-put 后的 activation delivery 和
最终结果返回，而不是 ONNX 执行、tensor decode、segment validation 或 segment
window size。

比较 cold 和 warm inference 时要注意 user 进程模型。如果脚本把 cold 和 warm
作为两个独立 user 进程启动，内存中的 plan cache 和 recent-responder history 不会
跨进程复用。稳定 P95 应使用同一个 user 进程内的多次 sequential requests，或
MiniNDN runner 支持的 60 秒 warm window。

MiniNDN 脚本会在第一个 command 前清空 provider artifact cache。它在 `neu`
启动 repo node，在 `memphis` 启动 controller，然后运行 controller-side deployer
把 model shards 和 runner 写入 repo。Provider logs 随后会在 cold command 中
为每个 role 的 `model` 和 `runner` artifacts 打印
`NDNSF_EXECUTION_ARTIFACT_CACHE_MISS ... source=repo`，并在 warm command 中
打印 `NDNSF_EXECUTION_ARTIFACT_CACHE_HIT`。

如果要用一个入口运行 APP API smoke、本地 ONNX executor smoke，以及两个稳定的
MiniNDN split smokes：

```bash
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case all
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case yolo-layout --layout 2x3
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case yolo-layout-local --layout 3x2
```

`yolo-layout` 验证自定义 YOLO layout 导出、repo-backed artifact 部署、
dependency prefetch、activation exchange 和 MiniNDN 中的最终结果返回。
`yolo-layout-local` 仍可用于快速检查生成的 chunk graph 和本地 ONNX
正确性。
当前已经通过网络级回归验证的 custom layouts 是 `2x3` 和 `3x2`。`1x3`
和 `3x3` 适合作为快速本地 export smoke；`3x3` 在作为部署 baseline 前应先
通过 `yolo-layout` 跑一轮。

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
每个 role 都有自己的 ONNX chunk。因此这是建立在真实 YOLO layers 上的 execution
plan，而不是 synthetic NumPy model；但当前 YOLO chunks 形成的是
pipeline-sequential dependency graph：每个 provider fetch 前一个 chunk 的
activation reference，继续 ONNX computation，然后发布下一个 activation reference。
最后一个 chunk 发布 response。User 会把这个 response 与本地完整 YOLO forward pass
比较，只有数值一致时才打印 `ok=true`。

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
