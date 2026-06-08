# ndn_service_framework

NDNSF 是一个基于 Named Data Networking 的通用动态 service framework。本仓库包含
C++ core runtime、Python bindings、distributed repo prototype、distributed inference
package，以及用于应用驱动验证的 UAV application。新应用应使用统一 service name 和动态
user/provider/controller API。旧的 generated service/stub 路径已经不再是支持的新开发方向。

当前主要组件：

```text
ndn-service-framework/        C++ core runtime 和通用动态 API
pythonWrapper/                Python 业务逻辑 API 和进程编排
NDNSF-DistributedRepo/        Distributed repo prototype 和 Python binding
NDNSF-DistributedInference/   高层 distributed inference package
NDNSF-UAV-APP/                基于 NDNSF 的 UAV network application
examples/                     C++ 和 Python smoke/regression examples
Experiments/                  MiniNDN 和实验 harness
RELEASE/                      本地 release packaging artifacts 和 manuals
```

框架贡献本身是 service runtime：provider discovery、permission distribution、
NAC-ABE-backed message protection、一次性 token handshake、ACK/Selection/Response
execution、面向已知 provider 的 Targeted invocation、同进程 trusted local invocation、
基于 ServiceContainer 的进程内组合，以及通用 large-data reference abstraction。
UAV 和 DistributedInference 是应用层 workload，用来验证并压力测试这些框架机制。

## 1. 前置条件

为了保证整个软件栈的版本一致，建议使用下面这些仓库：

```text
ndn-cxx: https://github.com/matianxing1992/ndn-cxx
NDNSD:   https://github.com/matianxing1992/NDNSD
ndn-svs: https://github.com/matianxing1992/ndn-svs
NAC-ABE: https://github.com/matianxing1992/NAC-ABE
NDNSF:   https://github.com/matianxing1992/NDN_Service_Framework
```

推荐使用下面的安装脚本。脚本会先检查外部 NDN 依赖是否已经安装；如果缺少某个依赖，它会从上面的仓库列表 clone 对应源码，编译安装依赖，然后再编译安装 NDNSF。

## 2. 安装

推荐使用仓库顶层的一键安装脚本：

```bash
sudo ./install_ndnsf_stack.sh
```

该脚本按依赖顺序安装整个栈：

1. 安装 NDNSF 栈默认需要的编译/运行时系统包。
2. 使用 `pkg-config` 检查外部依赖。
3. clone、编译并安装缺失依赖：`ndn-cxx`、`NDNSD`、`ndn-svs`、`OpenABE` 和 `NAC-ABE`。
4. 使用 `waf` 编译 NDNSF C++ 核心和仓库内的 C++ 子项目。
5. 安装 NDNSF Python wrapper 包 `ndnsf`。
6. 安装 NDNSF-DistributedRepo 的 Python binding `py_repoclient`。
7. 安装 NDNSF-DistributedInference Python 包。
8. 运行一个很小的 Python import/API smoke test。

默认系统包集合用于编译和运行：

```text
ndn-cxx, NDNSD, ndn-svs, OpenABE, NAC-ABE, and NDNSF
```

按需启用额外依赖组：

```bash
# 测试和文档常用额外包。
sudo ./install_ndnsf_stack.sh --with-system-tests-deps

# MiniNDN/Mininet 实验常用额外包。
sudo ./install_ndnsf_stack.sh --with-minindn-deps

# 从源码编译 NFD/NLSR 时常用额外包。
sudo ./install_ndnsf_stack.sh --with-nfd-nlsr-deps
```

`apt-get install` 是幂等的，已经安装的包会由包管理器自动跳过。

默认情况下，依赖源码会复用或 clone 到 `install_ndnsf_stack.sh` 旁边的 `dependencies/` 目录下。如果目录不存在，脚本会自动创建。也可以用 `--deps-dir` 指定其它源码目录：

```bash
sudo ./install_ndnsf_stack.sh --deps-dir ./dependencies
```

如果依赖已经安装好，只想重新编译 NDNSF：

```bash
./install_ndnsf_stack.sh --no-dependencies --no-system-install
```

如果想强制重新编译所有外部依赖，可以从本地源码树或重新 clone 的仓库构建：

```bash
sudo ./install_ndnsf_stack.sh --force-dependencies
```

### OpenABE 和 OpenSSL 说明

NAC-ABE 依赖 OpenABE。上游 OpenABE 对 OpenSSL 版本比较敏感，目前最稳定的是与 OpenSSL 1.1.x 一起构建。Ubuntu 20.04 默认提供 OpenSSL 1.1，但 Ubuntu 22.04 和 24.04 默认提供 OpenSSL 3。为了避免替换系统 OpenSSL，当缺少 `libopenabe` 时，安装脚本会使用 OpenABE 私有的 OpenSSL 1.1 依赖来构建 OpenABE。私有 OpenABE 会安装到：

```text
dependencies/local/openabe
```

这样可以在较新的 Ubuntu 上保持 OpenABE/NAC-ABE 兼容，同时不改变 `apt`、`git`、`curl`、Python 等系统工具使用的 OpenSSL。

如果是源码树开发，或者不想把 C++ 库和头文件安装到系统目录，可以使用：

```bash
./install_ndnsf_stack.sh --no-system-install
```

常用变体：

```bash
./install_ndnsf_stack.sh --configure --with-examples
./install_ndnsf_stack.sh --configure --with-tests
./install_ndnsf_stack.sh --no-system-install --with-examples
./install_ndnsf_stack.sh --no-dependencies --no-system-install
```

必要时可以覆盖依赖仓库 URL：

```bash
NDNCXX_REPO_URL=https://github.com/matianxing1992/ndn-cxx \
NDNSD_REPO_URL=https://github.com/matianxing1992/NDNSD \
NDNSVS_REPO_URL=https://github.com/matianxing1992/ndn-svs \
OPENABE_REPO_URL=https://github.com/zeutro/openabe \
NACABE_REPO_URL=https://github.com/matianxing1992/NAC-ABE \
sudo ./install_ndnsf_stack.sh --force-dependencies
```

仍然可以手动只安装 C++：

```bash
./waf configure
./waf
sudo ./waf install
```

如果手动安装，并且需要 Python API，请在 C++ 编译后安装这些 Python 包：

```bash
python3 -m pip install -e ./pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedRepo/pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedInference
```

## 3. 使用方法

### 3.1 通用动态 API，新应用推荐使用

新应用应该直接使用 framework-core 的通用动态 API。

Provider 侧：

```cpp
ndn_service_framework::ServiceProvider provider(
  face,
  ndn::Name("/muas/group"),
  providerCert,
  aaCert,
  "examples/trust-any.conf");

provider.addHandler<ObjectDetectionRequest, ObjectDetectionResponse>(
  ndn::Name("/ObjectDetection/YOLOv8"),
  [](const ndn::Name& requesterIdentity,
     const ObjectDetectionRequest& request,
     ObjectDetectionResponse& response) {
    // 服务逻辑从这里开始。
    response.set_label("person");
  });
```

User 侧：

```cpp
ndn_service_framework::ServiceUser user(
  face,
  ndn::Name("/muas/group"),
  userCert,
  aaCert,
  "examples/trust-any.conf");

ObjectDetectionRequest request;
request.set_image("frame-bytes");

user.RequestService<ObjectDetectionRequest, ObjectDetectionResponse>(
  ndn::Name("/ObjectDetection/YOLOv8"),
  request,
  300, // ACK collection window，单位 ms。
  ndn_service_framework::strategy::FirstResponding,
  1000, // 总 response timeout，单位 ms。
  [](const ObjectDetectionResponse& response) {
    // 处理 typed response。
  },
  [](const ndn::Name& requestId) {
    // 处理 timeout。
  });
```

对于新的 C++ application code，建议把公开 API surface 控制在下面这组入口：

```text
Provider normal service:       addHandler<RequestT, ResponseT>(serviceName, handler)
Provider known-target service: addTargetedService(serviceName, handler)
User normal service:           RequestService<RequestT, ResponseT>(serviceName, request, ackMs, policy, timeoutMs, onResponse, onTimeout)
User known-target service:     RequestServiceTargeted<RequestT, ResponseT>(provider, serviceName, request, onResponse, onTimeout, timeoutMs)
Same-process helper:           ServiceContainer::addLocalService<RequestT, ResponseT>(serviceName, handler)
```

接受 raw `RequestMessage`、legacy integer strategy、或显式 provider list 的低层 overload
仍然保留给 framework internals、tests 和兼容用途，但不建议作为新应用的起点。

对于已经知道目标 provider 的低延迟命令，例如 UAV flight-control/MAVLink
执行，使用 targeted invocation。Targeted invocation 仍然使用 NDNSF 的
`RequestMessage`/`ResponseMessage`、签名、权限检查、一次性 token 检查和
replay 防护。它只有在这个 provider/service 已经 bootstrap 出一批 token pair
之后，才跳过普通 ACK/Selection 阶段：

```cpp
provider.addTargetedService(
  ndn::Name("/UAV/MAVLink/Execute"),
  handler);

user.RequestServiceTargeted<MavlinkCommand, MavlinkResult>(
  ndn::Name("/example/uav/drone/A"),
  ndn::Name("/UAV/MAVLink/Execute"),
  command,
  onResponse,
  onTimeout,
  timeoutMs);
```

已知 provider 的低延迟调用只使用 `Targeted` API 名称。旧的 Direct API 名称
不再作为兼容别名保留。

Targeted invocation 的安全模型：

```text
第一次调用或 token 用完后的 refill:
  TargetedBootstrapRequest -> ACK -> SELECTION -> RESPONSE
  provider 的 response 会带回一批后续使用的一次性 token pair。

有缓存 token pair 时的 fast path:
  REQUEST -> RESPONSE
  request 携带一个没用过的 ProviderToken。
  response 回显与之配对的 UserToken。
  provider 在执行 handler 前消费这个 ProviderToken。
```

这样已知 provider 的控制命令仍然可以低延迟执行，同时不丢失 provider
authorization 和 replay resistance。缓存 token pool 用完后，下一次
`RequestServiceTargeted(...)` 会自动重新走 bootstrap/refill 流程。

对于较大的 service payload，NDNSF 使用统一的 large-data reference abstraction。
小 request/response payload 仍然内联放在 `RequestMessage.payload` 和
`ResponseMessage.payload` 里。大的 request input 可以由应用或 runtime 先发布为
segmented NDN Data，再把 `LargeDataReference` 放进 request payload。大的 response
则由 NDNSF core 自动处理：如果成功的 `ResponseMessage.payload` 超过配置阈值，
provider 会把它发布为签名的 segmented NDN Data，并把内联 response payload 替换为
`LargeDataReference`。user runtime 识别该 reference 后，会自动 fetch segments、解密、
验证 size/hash，然后把原始 response payload 交给同一个应用 callback。应用层 response
API 不需要改变。

大的 response 使用 hybrid message encryption，而不是对整个大 payload 使用 NAC-ABE
加密。payload 本体用 AES-GCM 加密，并作为 segmented `HybridMessageEnvelope` 保存；
NAC-ABE 只在 message key 尚未缓存时用于包装这个很小的 message key。这样既保留普通
`/PERMISSION/<service>` response authorization，又避免在大型 catalog snapshot、model
artifact、activation、recording 等 response body 上调用性能很差的 NAC-ABE
`produce/consume`。

NDNSF provider 还会为从 in-memory storage 服务的 Data 保留一个短期 pending
Interest 队列。如果某个可预测 Data name 的 Interest 早于 Data 本身到达，
provider 会在该 Interest 的正常 InterestLifetime 过期前保留它；当后续插入的
Data 能匹配这个 Interest 时，provider 会立即回复。这是 large-data reference、
repo object 和 distributed-inference activation object 的传输优化。它不改变
Request/ACK/Selection/Response 协议、Data 名字、签名、加密或应用 callback。

Provider collaboration large-data fetch 默认使用 10 秒 Interest lifetime
（`NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS`）。这个默认值刻意比普通低延迟命令
timeout 更长，因为 distributed-inference role 可能会在上游 role 完成 segments
发布前，就预取一个确定性 activation name。实验仍然可以通过这个环境变量显式调低
或调高该值。
设置 `NDNSF_COLLAB_LARGE_FETCH_INIT_CWND` 可以调节 collaboration large-data fetch
的 SegmentFetcher 初始 pipeline window，默认值是 `8`。设置
`NDNSF_COLLAB_LARGE_FETCH_TIMING=1` 时，Core 会为这些 collaboration large-data
fetch 输出 SegmentFetcher 级 timing 日志。DI MiniNDN 回归会把这些日志解析到
`collab-large-fetch-stats.json`，这样可以把应用层 dependency wait 和 native segmented
fetch 时间分开比较。

自动 response reference 的默认阈值是 6000 bytes，可以用环境变量调整或关闭：

```bash
NDNSF_RESPONSE_LARGE_DATA_THRESHOLD=4096 ./your-app
NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1 ./your-app
```

User 侧解析自动 large response reference 时也走同样的 segmented-fetch 纪律。
`NDNSF_RESPONSE_LARGE_INTEREST_LIFETIME_MS` 控制 Interest lifetime，
`NDNSF_RESPONSE_LARGE_FETCH_INIT_CWND` 控制 SegmentFetcher 初始窗口，
`NDNSF_RESPONSE_LARGE_FETCH_TIMING=1` 会输出 timing 日志。当 distributed inference
这类应用故意让较大的最终输出走 response-reference path 时，这些配置可以用于调试和调优。

对于同一个进程内部的可信服务组合，NDNSF 也提供 `LocalServiceRegistry`。
这不是一种网络调用模式，远程 caller 看不到也不能选择它。只有 container 显式把某个
service 注册到 local registry 后，这个 service 才能被 local call；未注册时 local call
默认失败。Local invocation 会绕过 NDNSF 签名、NAC-ABE、permission fetch、SVS 发布以及
token/replay 检查，所以只能用于同一个可信进程或 service container 内部：

```cpp
ndn_service_framework::LocalServiceRegistry localRegistry;

localRegistry.registerLocalService<TelemetryRequest, TelemetryStatus>(
  ndn::Name("/UAV/Telemetry/GetStatus"),
  telemetryHandler);

auto result = localRegistry.localInvoke<TelemetryRequest, TelemetryStatus>(
  ndn::Name("/UAV/Telemetry/GetStatus"),
  request);

auto future = localRegistry.localInvokeAsync<TelemetryRequest, TelemetryStatus>(
  ndn::Name("/UAV/Telemetry/GetStatus"),
  request);
```

跨进程、跨节点或不可信 caller 仍然必须使用普通 `RequestService(...)` 或
`RequestServiceTargeted(...)`。Local invocation 不会增加 `/NDNSF/LOCAL/...` 名字，
也不能被远程节点通过 request 指定。

对于更复杂的 service-oriented application，NDNSF core 还提供
`ServiceContainer`，作为同一进程内的 runtime composition 和 lifecycle
边界。它不会替代 `ServiceController`、`ServiceUser`、`ServiceProvider` 或
`LocalServiceRegistry`，而是拥有或引用它们，让一个应用可以在同一个进程级
配置下管理多个角色。一个 container 可以包含 controller、user、provider 和
local helper 的任意组合，但不要求每个进程都运行所有角色。UAV-APP 和
DistributedInference 这类应用中，一个进程可能同时是 user、provider、本地
helper host，有时还会嵌入 controller runtime，这正是 ServiceContainer 的适用场景。

`ServiceContainer` 负责：

```text
管理同一进程内的多个 ServiceController、ServiceUser、ServiceProvider、helper 和
local-only module；
提供共享的进程级 runtime configuration；
协调应用自己拥有的 module 的 lifecycle start/stop hook；
暴露 LocalServiceRegistry，用于可信的同进程组合；
把 remote、Targeted 和 local service registration 放在同一个应用边界里管理；
为复杂 NDNSF 应用提供标准结构。
```

`ServiceContainer` 不负责：

```text
改变 Request/ACK/Selection/Response wire protocol；
让远程 caller 选择 container-local mode；
绕过 remote permission、signature、NAC-ABE、UserToken、ProviderToken 或 replay protection；
把应用特定状态模型强塞进 NDNSF core。
```

Container registration 采用保守语义。应用应该在调用 `start()` 之前完成 user、
provider、local service 和 lifecycle hook 注册。`start()` 之后，role 和 hook
注册会被拒绝，避免进程级 service 边界在 request 运行期间改变。`start()` 是幂等的；
如果某个 start hook 抛异常，已经启动的 hook 会按相反顺序停止，container 保持 stopped。
`stop()` 也是幂等的，并按相反顺序执行 stop hook。

对于由 container 管理的本地 helper，推荐使用 `container.addLocalService(...)`，
而不是直接调用 `container.localRegistry().registerLocalService(...)`。registry accessor
仍然保留为低层 escape hatch，并用于 local invocation；但 `addLocalService(...)` 会使用
与 role 和 hook registration 一致的 lifecycle 边界。`start()` 前重复注册同一个 local
service name 时，沿用 `LocalServiceRegistry` 语义，新的 handler 会替换旧 handler。
`stop()` 之后，应用可以在再次启动 container 前调整 role、local service 和 lifecycle hook。

真正调用服务的 API 保持不变：

```cpp
ndn_service_framework::ServiceContainer container({
  ndn::Name("/example/app/container"),
  ndn::Name("/example/group"),
  ndn::Name("/example/controller"),
  "examples/trust-any.conf"
});

container.addUser("operator", user);
container.addProvider("drone-services", provider);
container.addController("controller", controller);

container.provider("drone-services").addHandler<RequestT, ResponseT>(
  serviceName, handler);

container.user("operator").RequestService<RequestT, ResponseT>(
  providers, serviceName, request, onResponse, onTimeout, timeoutMs, strategy);

container.addLocalService<LocalRequest, LocalResponse>(
  localServiceName, localHandler);

container.addLifecycleHook("repo-helper", {
  [] { /* start application helper */ },
  [] { /* stop application helper */ }
});

container.start();
container.stop();
```

简单说，`ServiceController` 是面向网络的 authority/policy role，`ServiceProvider`
是面向网络的 provider role，`ServiceUser` 是面向网络的 caller role，而
`ServiceContainer` 是可信进程内部用于组合和管理这些角色的 runtime。

`RequestT` 和 `ResponseT` 只需要提供类似 protobuf 的方法：

```cpp
bool SerializeToString(std::string* out) const;
bool ParseFromArray(const void* data, size_t size);
```

应用 handler callback 可以从 Face/event-loop 线程移到 worker 线程：

```cpp
provider.setHandlerThreads(2);
user.setHandlerThreads(1);
```

默认值是 `0`，表示保持 inline callback 执行。设置 `handlerThreads > 0` 后，provider ACK/admission callback、provider selected request execution callback 和 user response callback 会在有界 worker queue 上运行。Face、IMS、SVS、NAC-ABE 消费/发布步骤、token 检查和 framework 状态表仍保留在 Face event loop 上。如果 `handlerThreads > 1`，应用 handler 必须是线程安全的，或者由应用自行同步。

HELLO 示例也暴露同样设置：

```bash
./build/examples/App_Provider --handler-threads 2
./build/examples/App_User --handler-threads 1
```

### 3.2 统一 serviceName 规则

完整 endpoint 路径应使用一个统一的 `serviceName`：

```text
/ObjectDetection/YOLOv8
/FlightControl/Takeoff
/LLM/Llama3/Prefill
```

新代码不要围绕分离的 `ServiceName + FunctionName` 路径设计。拆分形式只保留给 legacy compatibility。

### 3.3 V2 naming 说明

通用 runtime 路径使用 V2 naming helper，并使用一个统一的可变长度 `serviceName`。Service name 通过位置倒推解析：Request、ACK、Selection 和 Response 名字末尾的 `requestId` 是固定字段。当 user 或 provider identity 出现在另一个 identity 的命名空间内部时，它会被编码为单个 URI component，因此中间剩余 components 可以无歧义地解释为 service name。

Request：

```text
/<requester>/NDNSF/REQUEST/<serviceName...>/<requestId>
```

Response：

```text
/<provider>/NDNSF/RESPONSE/<requester-uri-component>/<serviceName...>/<requestId>
```

ACK：

```text
/<provider>/NDNSF/ACK/<requester-uri-component>/<serviceName...>/<requestId>
```

Selection：

```text
/<requester>/NDNSF/SELECTION/<serviceName...>/<requestId>
```

新的 V2 selection message 对单 provider 和 multi-provider selection 都使用这一种
统一 name 形状。被选中的 provider 集合放在 `ServiceSelectionMessage` payload 的
provider entries 里。每个 entry 绑定一个具体 provider identity，携带 provider-bound
token proof 和可选的 provider-specific assignment payload。Provider 解开
service-level selection payload 后，只在找到自己的 entry 时执行，否则拒绝该 message。旧的
`.../SELECTION/<provider-uri-component>/<service>/<requestId>` provider-specific
selection name 只作为已有 regression 和 deployment 的兼容输入继续解析，不再是新路径。

### 3.4 权限模型

权限直接从 `ServiceController` 获取。

Permission Interest 名字：

```text
/<controller>/NDNSF/PERMISSIONS/USER/<targetIdentity...>
/<controller>/NDNSF/PERMISSIONS/PROVIDER/<targetIdentity...>
```

Permission discovery Interest 通常不签名。`ServiceController` 从 Interest 名字中解析 target identity，为该 target 构造 `PermissionResponse`，使用 target identity certificate 加密 payload，用 controller identity 签名返回 Data，然后 put Data。其它 identity 可以 fetch 某个 target 的加密 PermissionResponse，但无法解密。User 和 provider runtime 会拒绝这条路径上的明文 `PermissionResponse` Data。`PermissionResponse` 描述允许的 user/provider/service 映射；其中 legacy token 字段已废弃并保持为空。

Controller 不再发放 service invocation token。服务调用使用 `ServiceUser` 为每个请求生成的一次性 `UserToken`，以及 `ServiceProvider` 为每个 ACK 生成的一次性 `ProviderToken`。NAC-ABE attribute 负责 group-level access，一次性 token 则把 ACK、selection 和 response 绑定到特定请求。

这个 PermissionResponse encryption 不是 NAC-ABE。

NAC-ABE 仍然是 NDNSF service request/response message、未来 selection payload、content key、IMS 和 SVS-backed runtime publication 的 runtime encryption 机制。

### 3.5 分布式部署中的证书发布

NDN certificate 本身也是具名 Data packet。在分布式部署中，user、provider、controller 和 AA certificate 必须能通过 certificate name 被路由/FIB 到达，就像普通 service Data 一样。远端 validator、NAC-ABE authority 和 controller 可能会在 DKEY、permission 和 bootstrap 流程中 fetch 这些 certificate。

NDNSF 部署应使用一个应用级 root identity 作为 trust anchor。推荐让 root identity 就是应用
namespace 本身，例如 `/example/hello`、`/example/uav` 或 `/example/repo`，而不是额外加一个
非父节点后缀，例如 `/root`。每个节点先创建自己的 identity key，然后获取由应用 root 签名的 NDN
certificate。节点只保留自己的私钥，安装 root certificate 作为本地信任锚，并按 certificate name
对外服务自己的 root-signed certificate。MiniNDN HELLO harness 现在也采用这个流程：先创建
`/example/hello`，再用它签发 `/example/hello/controller`、`/example/hello/user` 和 provider
certificates，然后把对应 keychain 材料分发到各节点。

框架提供 `ndn_service_framework::CertificatePublisher`：

```cpp
ndn_service_framework::CertificatePublisher certPublisher(
  face,
  keyChain,
  providerCert.getName());
```

它会在本地 KeyChain 中找到 certificate，在精确 certificate name 下服务已有 certificate Data，默认注册 certificate 的 `.../KEY/<key-id>` prefix。HELLO 示例默认启用这个功能；如果部署环境已经用其它机制服务 certificate，可以使用 `--no-serve-certificates`。

有物理 access 时的手动证书 bootstrap：

如果 operator 能接触每台机器，NDNCERT 是可选的。最安全的手动流程是每个节点在本机生成自己的私钥；只有 certificate request 离开节点。CA/root 机器只负责签这个 request，然后把公开 certificate 发回节点。

在 CA/root 机器上：

```bash
ndnsec key-gen -t r /ndn/ndnsf/demo > root.cert
ndnsec cert-install -f root.cert
```

在某个节点上，例如 drone A：

```bash
ndnsec key-gen -n -t r /ndn/ndnsf/demo/drone/A > drone-A.req
```

把 `drone-A.req` 复制到 CA/root 机器并签发：

```bash
ndnsec cert-gen -s /ndn/ndnsf/demo -i ROOT drone-A.req > drone-A.cert
```

把 `root.cert` 和 `drone-A.cert` 复制回 drone A，并在 drone A 上安装：

```bash
ndnsec cert-install -f root.cert
ndnsec cert-install -f drone-A.cert
ndnsec-ls-identity -c
```

这个流程不要 export/import safebag；节点私钥永远不离开本机。只有当部署方式明确要求在一台机器上集中生成 key、再把 private key 发给另一台机器时，才使用 safebag。

### 3.6 示例

`/examples/generic-dynamic-user-provider.cpp` 是最小通用动态示例。它直接使用 `ServiceProvider::addHandler<RequestT, ResponseT>` 和 `ServiceUser::RequestService<RequestT, ResponseT>`。它使用本地/mock request publication，因此可以不依赖真实 NFD/network 展示 request/response 流程。

`/examples/ServiceContainer_LocalHelper.cpp` 是最小 ServiceContainer 组合示例。
同一个进程拥有 user role、provider role 和一个可信 local helper。provider 的 remote
service handler 会通过 `LocalServiceRegistry` 调用这个 local helper，但 user 仍然只看到
普通 remote service invocation。这个例子说明 local helper 是进程内部组合工具，不会变成
外部 caller 可以选择的网络服务。

构建：

```bash
./waf configure --with-examples
./waf build --target=generic-dynamic-user-provider
./waf build --target=service-container-local-helper
```

`/examples/App_ServiceController.cpp`、`/examples/App_Provider.cpp` 和 `/examples/App_User.cpp` 是当前 HELLO regression examples。它们使用 controller-issued permission mapping、动态 `addService(...)`、`RequestMessage.payload = "HELLO"`、`ResponseMessage.payload = "HELLO"`、`AckDecision` metadata payload、`UserToken`/`ProviderToken` 握手，以及基于 timeout 的 `AckSelectionCandidate` custom selection。

编译示例的方式见 `/examples/wscript`。

### 3.7 如何运行示例

如果要在多台机器上运行示例，请先按上面的手动证书 bootstrap 流程安装 identity certificates。本地 regression scripts 会自动创建临时 keychain material。

当前 HELLO 示例由下面这些 regression script 测试：

```bash
./examples/run_hello_auth_regression.sh
./examples/run_hello_ack_payload_regression.sh
./examples/run_selective_ack_custom_selection_regression.sh
./examples/run_nac_abe_attribute_routing_regression.sh
./examples/run_token_handshake_negative_regression.sh
```

`run_hello_auth_regression.sh` 验证 controller-issued user/provider permission mapping、通用 HELLO request/response 流程，以及 `UserToken` 在 request、ACK 和 response 中的传播。

`run_hello_ack_payload_regression.sh` 验证 provider 发布 ACK metadata payload，并且 user 在收到 HELLO response 前收集 payload。

`run_selective_ack_custom_selection_regression.sh` 验证 multi-provider selective ACK、ACK payload metadata、timeout-driven custom selection、Provider C rejection、Provider B selection，以及只有 Provider B 发布最终 response。

`run_nac_abe_attribute_routing_regression.sh` 验证 NAC-ABE routing 的 runtime `GetAttributesByName` 日志：REQUEST 和 SELECTION 使用 `/SERVICE/HELLO`，ACK 和 RESPONSE 使用 `/PERMISSION/HELLO`。

`run_token_handshake_negative_regression.sh` 验证错误 `UserToken` 的 ACK/response 会被拒绝，错误 `ProviderToken` 的 selection 会被拒绝，以及 replayed ProviderToken 会被拒绝。

### 3.8 Python wrapper 和高层应用包

Python wrapper 是当前 C++ runtime 的 binding，不是另一个独立 framework implementation。
它支持普通 service handler、service-independent ACK decision、应用自定义 ACK selection、
异步请求、collaboration handler、collaboration request、encrypted large-data publication、
标准 large-data reference payload，以及 NDN segmented Data helpers。API 示例见
`pythonWrapper/README.md`。

`NDNSF-DistributedInference` 构建在这个 wrapper 之上，并暴露 model-plan 和 distributed
inference API。当前路径支持 ONNX chunk policy、dependency-driven execution、repo-backed
或 NDN-backed artifacts、通过 large-data reference 交换 activation，以及 MiniNDN smoke
tests。它仍然是 NDNSF core 之上的应用包；model-specific splitter 和 planner 保留在该包中，
不会放进 framework core。

`NDNSF-DistributedRepo` 是 repository-oriented application layer。它应该保存和服务应用
已经发布的 NDN Data segments 或 references，而不是重新定义 NDNSF service security。
Repo 细节有意保持在 core service invocation protocol 之外。

`NDNSF-UAV-APP` 是基于 NDNSF 的 UAV service application。跨节点 control、telemetry、
video、recording discovery 和 mission operations 走 NDNSF remote/Targeted services。
同进程 helper 可以通过 `container.addLocalService(...)` 使用
`ServiceContainer::localRegistry()`，但 local helper 不是外部可选择的 service。

### 3.9 这些 regression 对应的安全机制

```text
Permission distribution:
  User fetches /NDNSF/PERMISSIONS/USER/<user>.
  Provider fetches /NDNSF/PERMISSIONS/PROVIDER/<provider>.
  Permission discovery Interests are normally unsigned.
  ServiceController signs the returned Data with the controller identity.
  PermissionResponse payloads are encrypted to the target identity certificate.

NAC-ABE attributes:
  REQUEST and SELECTION use /SERVICE/<service>.
  ACK and RESPONSE use /PERMISSION/<service>.

Authorization:
  User requests carry a one-time UserToken generated by ServiceUser.
  ACKs carry the same UserToken and a one-time ProviderToken generated by ServiceProvider.
  V2 Selection messages carry provider-specific entries. 新的统一 V2 selection 不在共享的
  service-level payload 中暴露明文 ProviderToken；每个 entry 携带 provider-bound
  ProviderToken proof。
  Responses carry the original UserToken.
  Users reject ACK/response UserToken mismatches.
  Providers reject selection ProviderToken mismatches.
  Providers reject replayed ProviderTokens for consumed or new request IDs.
  Targeted fast-path requests carry one unused ProviderToken obtained from a
  prior targeted bootstrap/refill response, and targeted responses echo the
  paired UserToken.
  Targeted services refill token batches through the normal ACK/Selection path.
  Providers must install their own provider permission before serving a service.
  Service authorization is enforced by NAC-ABE attributes, provider permission checks, and token handshake validation.
```

### 3.10 如何把日志写到文件

例如程序是 `./app`，并且希望记录所有日志，先在命令行设置 log level：

```bash
export NDN_LOG="*=TRACE"
```

然后运行：

```bash
./app > filename.log 2>&1
```

输出会保存到当前目录下的 `filename.log`。如果使用 MiniNDN，输出会存储到 `/tmp/minindn/<nodeName>`。

### 3.11 MiniNDN latency reproduction profile

低延迟 HELLO benchmark 使用动态 API：Memphis 作为 user，UCLA 作为 provider，
CSU 作为 controller，默认禁用 adaptive admission control，SVS maximum
suppression 设为 1 ms，并启用 performance mode。hot path 上的逐请求日志必须
低于 `INFO`；否则 request/ACK/selection/response 日志本身就会主导 benchmark。

做性能测试时应尽量少打印。代码中逐请求、逐 ACK/selection/response、
lifecycle、publication 和详细诊断事件应使用 `TRACE` 级别。正常性能测试使用
`NDN_LOG=ndn_service_framework.*=INFO`；如果需要确认启动和状态但不想打开 hot
path trace，可以使用 `DEBUG`。只有专门做性能分析或排查瓶颈时才使用 `TRACE`。
性能分析 trace 默认也要采样：`--timeline-trace` 会打开 timeline/lifecycle
诊断，`--timeline-trace-sample-rate N` 会按 request ID 稳定保留每 `N` 个请求
中的一个样本，默认值为 `100`。只有非常短的定点调试才使用
`--timeline-trace-sample-rate 1` 全量记录。同理，NFD packet dump 和其它详细
诊断只应在分析问题时打开。10 分钟 rate run 会产生大量原始输出，可能扰动
latency，甚至填满文件系统；有用结论记录后，`results/` 中应只保留
summary/aggregate 结果。

关键运行设置：

```text
NDN_LOG=ndn_service_framework.*=INFO
NFD log level: WARN
NDNSF_SVS_MAX_SUPPRESSION_MS=1
NDNSF_SVS_ASYNC_PUBLISH=1
NDNSF_SVS_PARALLEL_SYNC=1
NDNSF_SVS_PARALLEL_WORKERS=4
NDNSF_SVS_PARALLEL_QUEUE=256
NDNSF_SVS_PARALLEL_PRODUCTION=4
NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING=0
NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK=1
adaptive admission: disabled
provider handler threads: 2
provider ACK worker threads: 2
strategy: first-responding
workload: open-loop, latency floor 验证使用 60 s warmup + 60 s measured duration
```

100 RPS latency-floor run 已验证的软件栈：

```text
OS: Ubuntu 20.04.3 LTS
Compiler: g++ 9.4.0
Python: 3.8.10
Boost: 1.71.0
OpenSSL: 1.1.1f
ndn-cxx: 0.9.0 (/usr/local/lib/libndn-cxx.so.0.9.0)
NFD: 24.07-14-g2b43d675
MiniNDN: 0.7.0 (/home/tianxing/NDN/mini-ndn)
Mininet: 2.3.1b4
ndn-svs: /home/tianxing/NDN/ndn-svs commit 70302b6
NDNSF: 记录该 profile 的当前仓库 commit
OpenABE: /usr/local/lib/libopenabe.so，基于 OpenSSL 1.1.x 构建
```

这里的 commit hash 是复现实验记录的一部分，不表示永久最低版本要求。如果更新了
`ndn-cxx`、`NFD`、`ndn-svs`、MiniNDN/Mininet、OpenABE 或本仓库后 latency
floor 退化，应先重新执行下面的复现命令，再和 166 ms reference 比较。同时要确认
程序实际链接的是期望的 `/usr/local/lib/libndn-svs.so` 和
`/usr/local/lib/libndn-cxx.so`；如果还在使用旧的系统库，源码修改可能看起来没有生效。

复现命令：

```bash
sudo -n python3 Experiments/NDNSF_NewAPI_Minindn_Perf.py \
  --topology-file 'Experiments/Topology/testbed(loss=0%).conf' \
  --user-node memphis \
  --provider-nodes ucla \
  --controller-node csu \
  --providers 1 \
  --rate-rps 100 \
  --duration 60 \
  --warmup 60 \
  --max-total-runtime-seconds 300 \
  --workload-mode open-loop \
  --strategy first-responding \
  --disable-adaptive-admission-control \
  --performance-mode \
  --handler-threads 2 \
  --ack-threads 2 \
  --nfd-log-level WARN \
  --skip-post-run-diagnostics
```

参考结果来自 `results/newapi_testbed_rate_series_20260528_194238`：

```text
RPS   Actual   Success   Avg ms   P50 ms   P95 ms   P99 ms   Timeout
20    20.00    100%      166.19   165.20   172.88   178.70   0
60    60.00    100%      168.85   166.61   184.34   199.18   0
100   99.99    100%      166.40   165.67   169.04   174.19   0
```

当前 60 秒诊断确认，在切换到 StateVectorSync v3 bootstrap-time wire format
之后，低延迟区间仍然可以达到。新的 SVS wire format 为
`StateVectorEntry(Name, SeqNoEntry(BootstrapTime, SeqNo))`。关键 harness 设置是
`NDNSF_SVS_PARALLEL_PRODUCTION=4`。之前 `--performance-mode` 默认值误把它设成
`0`，导致相同 20 RPS workload 上升到约 210 ms。恢复 parallel Sync Interest
production 后，20 RPS 和 100 RPS 都回到了 160-170 ms 区间：

```text
Result directory                                      RPS    Actual   Success   Avg ms   P50 ms   P95 ms   P99 ms   Timeout
results/newapi_testbed_rate_series_20260529_201154   20     20.00    100%      162.04   161.92   163.68   165.83   0
results/newapi_testbed_rate_series_20260529_201458   100    100.00   100%      164.38   164.00   167.46   173.67   0
```

除非实验明确要测试 periodic Sync Interest timer，否则不要修改这个 timer：
periodic sync 会影响 piggyback 机会。Sync Interest suppression 应保持在
1-5 ms 范围内；本复现 profile 使用 1 ms。

当前 open-loop 生成器避免零延迟 catch-up burst。event loop 落后时，它会记录
delayed publications，并用有下限的 catch-up 间隔，而不是把多个已到期请求连续
发布出去。在 60 秒 100 RPS 测试
(`results/newapi_minindn_perf_20260529_125158`) 中，生成器保持 99.995 actual RPS，
同时把小于 1 ms 的发送间隔降为 0 次：

```text
Actual RPS  Success  Avg ms  P50 ms  P95 ms  P99 ms  Timeout
99.995      99.95%   203.81  201.47  235.27  251.08  3
```

采样 timeline 测试 (`results/newapi_minindn_perf_20260529_125523`) 显示，AES-GCM
加密和本地 publish 调用不是瓶颈，二者 p50 都在亚毫秒级。剩余延迟主要在
SVS/NFD delivery：REQUEST-to-ACK p50 约 97 ms，SELECTION-to-RESPONSE p50 约
96 ms。user/provider 四个单向 delivery leg 各约 46-50 ms，而 Memphis 到 UCLA
的 route cost 为 37 ms。因此后续优化应聚焦 SVS/NFD delivery path 和
piggyback delivery effectiveness，而不是继续调整 periodic-sync timer、增加
hot-path 日志，或优化应用层 crypto。

2026-05-29 这次 regression 的主要经验是：即使协议和代码路径本身正确，如果
harness 悄悄关闭 parallel SVS production，benchmark 也会显得很慢。验证
latency floor 时应保持 `--performance-mode` 与上面的 runtime profile 一致；
只有在实验明确研究单线程 production 行为时，才设置
`--svs-disable-parallel-production`。
