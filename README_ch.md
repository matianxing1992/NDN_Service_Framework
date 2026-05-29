# ndn_service_framework

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
  providers,
  ndn::Name("/ObjectDetection/YOLOv8"),
  request,
  [](const ObjectDetectionResponse& response) {
    // 处理 typed response。
  },
  [] {
    // 处理 timeout。
  },
  1000,
  ndn_service_framework::tlv::FirstResponding);
```

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
/<requester>/NDNSF/SELECTION/<provider-uri-component>/<serviceName...>/<requestId>
```

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

NDNSF 部署应使用一个应用级 root identity 作为 trust anchor。每个节点先创建自己的 identity key，然后获取由应用 root 签名的 NDN certificate。节点只保留自己的私钥，安装 root certificate 作为本地信任锚，并按 certificate name 对外服务自己的 root-signed certificate。MiniNDN HELLO harness 现在也采用这个流程：先创建 `/example/hello/root`，再用它签发 controller、user 和 provider certificates，然后把对应 keychain 材料分发到各节点。

框架提供 `ndn_service_framework::CertificatePublisher`：

```cpp
ndn_service_framework::CertificatePublisher certPublisher(
  face,
  keyChain,
  providerCert.getName());
```

它会在本地 KeyChain 中找到 certificate，在精确 certificate name 下服务已有 certificate Data，默认注册 certificate 的 `.../KEY/<key-id>` prefix。HELLO 示例默认启用这个功能；如果部署环境已经用其它机制服务 certificate，可以使用 `--no-serve-certificates`。

### 3.6 示例

`/examples/generic-dynamic-user-provider.cpp` 是最小通用动态示例。它直接使用 `ServiceProvider::addHandler<RequestT, ResponseT>` 和 `ServiceUser::RequestService<RequestT, ResponseT>`。它使用本地/mock request publication，因此可以不依赖真实 NFD/network 展示 request/response 流程。

构建：

```bash
./waf configure --with-examples
./waf build --target=generic-dynamic-user-provider
```

`/examples/App_ServiceController.cpp`、`/examples/App_Provider.cpp` 和 `/examples/App_User.cpp` 是当前 HELLO regression examples。它们使用 controller-issued permission mapping、动态 `addService(...)`、`RequestMessage.payload = "HELLO"`、`ResponseMessage.payload = "HELLO"`、`AckDecision` metadata payload、`UserToken`/`ProviderToken` 握手，以及基于 timeout 的 `AckSelectionCandidate` custom selection。

编译示例的方式见 `/examples/wscript`。

### 3.7 如何运行示例

NDN 需要先创建对应 root certificate，再用 root certificate 生成子 certificate。root certificate 和这些子 certificate 都需要安装到每个节点。`/Experiments/NDNSFExperiment_AutoConfig.py` 展示了如何创建 certificate；实际部署时需要按自己的需求修改。

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

这些 regression 对应的安全机制：

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
  Selection messages carry the selected provider's ProviderToken.
  Responses carry the original UserToken.
  Users reject ACK/response UserToken mismatches.
  Providers reject selection ProviderToken mismatches.
  Providers reject replayed ProviderTokens for consumed or new request IDs.
  Providers must install their own provider permission before serving a service.
  Service authorization is enforced by NAC-ABE attributes, provider permission checks, and token handshake validation.
```

### 3.8 如何把日志写到文件

例如程序是 `./app`，并且希望记录所有日志，先在命令行设置 log level：

```bash
export NDN_LOG="*=TRACE"
```

然后运行：

```bash
./app > filename.log 2>&1
```

输出会保存到当前目录下的 `filename.log`。如果使用 MiniNDN，输出会存储到 `/tmp/minindn/<nodeName>`。

### 3.9 MiniNDN latency reproduction profile

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
NDNSF_SVS_PARALLEL_PRODUCTION=0
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
ndn-svs: /home/tianxing/NDN/ndn-svs commit 8b26f10
NDNSF: /home/tianxing/NDN/ndn-service-framework commit 1259111
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

当前 60 秒、100 RPS 诊断解释了为什么后续持续运行可能高于 166 ms 的短窗口
baseline。除非实验明确要测试这个 timer，否则不要修改 ndn-svs periodic Sync
Interest 时间：periodic sync 会影响 piggyback 机会。Sync Interest suppression
应保持在 1-5 ms 范围内；本复现 profile 使用 1 ms。

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

后续诊断隔离出一个重要 delivery 抖动来源：parallel Sync Interest production。
使用 `--svs-disable-parallel-production` 并把 send-but-not-measure warmup 扩到
60 秒后，持续 60 秒、100 RPS measured run 回到了低延迟区间。随后又验证了
默认 `--performance-mode` profile，结果目录为
`results/newapi_minindn_perf_20260529_131700`：

```text
Actual RPS  Success  Avg ms  P50 ms  P95 ms  P99 ms  Timeout
100.000     100.00%  168.23  166.77  177.15  190.17  0
```

这说明 166 ms 行为并不只存在于短窗口；系统进入 steady state 后，60 秒持续
测量也能达到接近该水平。parallel production 会把本地 publication delay 隐藏
在 worker queue 后面：应用 timeline 里本地 publish 仍是亚毫秒级，但端到端
delivery 会增加额外 tail。验证 latency floor 时应使用这个 profile；只有当实验
明确研究 throughput/CPU tradeoff 时才启用 parallel production。
