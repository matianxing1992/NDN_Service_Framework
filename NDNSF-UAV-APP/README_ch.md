# NDNSF-UAV-APP

`NDNSF-UAV-APP` 是一个基于 NDNSF 的 C++ UAV Service Container Application 参考实现。它放在
`NDNSF-DistributedRepo` 和 `NDNSF-DistributedInference` 同级，因为它是建立在通用
NDNSF runtime 之上的应用层，而不是低层 core example。

核心思想是：一个进程可以承载多个 NDNSF service instances 和 client-side workflows。
`UavDroneApp` 是 drone-side container，承载 MAVLink execution、video control、telemetry、
camera frame、mission assignment 等服务实例。`UavGroundStationApp` 是 ground-station
container，承载控制客户端、图传播放、任务协同、telemetry 显示，以及供无人机反向调用的
object detection 等服务。container 进程负责 identity、trust schema、policy fetch、GUI 和
本地硬件适配；每个 named NDNSF service 仍然是独立可寻址、独立受权限控制的服务。

## 为什么需要这个应用

直接基于 IP 开发 UAV network application 时，应用经常要处理很多本不属于任务逻辑的网络问题：
移动无人机的地址绑定、NAT 和移动性、多无人机服务发现、operator 授权、数据真实性、
无线丢包恢复，以及无人机忙碌或断连时的任务重新分配。

这个应用展示 NDNSF 如何把这些需求表达成命名服务、签名/加密数据、权限控制的服务调用，以及
provider selection。

## 第一版范围

第一版展示 UAV workflow 的核心路径：

- ground station 生成 `arm`、`takeoff`、`land` 和 mission command 的 MAVLink bytes。
- drone 收到 NDNSF request，完成 NDNSF 安全路径验证，然后把 opaque MAVLink bytes 转发给
  mock backend 或 UDP MAVLink flight-controller backend。
- drone 提供 telemetry 和 camera-frame 服务。
- ground station 通过目标 drone 名下的专属控制服务开启/停止图传；drone 在自己的命名空间下发布
  frame data，ground station 根据 frame name 拉取和预取后续帧。
- ground station 把同一个巡逻/巡检任务拆成 role-specific waypoint sectors，并分配给多架无人机协作完成。
- drone 返回 mock 拍照结果，并且可以请求 ground station 对它已经解码到的最新直播画面执行
  低频真实 object detection。
- ground station 提供 `/UAV/GS/ObjectDetection`；开启直播时，drone 会周期性调用这个服务，
  判断当前摄像头画面里是否有 `Car` 或 `Truck`，并在 Drone 窗口里提示。这个 request payload
  只放元数据，不把图像 bytes 塞进服务请求。
- 多个 drone 可以提供相同服务；如果某个 drone suppress ACK 或不可用，NDNSF provider selection
  可以选择另一个 drone。

drone 不解释 MAVLink 命令语义。MAVLink message 由 ground station 构造。drone app 只把
MAVLink 当作 opaque bytes，并交给 flight-controller backend。

MAVLink execution 使用 NDNSF Targeted invocation。发给某架 drone 的第一条命令先走正常的
request/ACK/selection/response 认证流程，并拿到一批一次性 token pair。后续 `arm`、`takeoff`
和 `land` 命令，以及键盘手操产生的低频 `MANUAL_CONTROL` 更新，都会使用 request/response-only
Targeted 调用 `/UAV/MAVLink/Execute`，减少命令延迟，同时仍然验证 provider 并拒绝 token replay。

## Service Containers

```text
UavDroneApp
  Drone-side service container。它承载 MAVLink execution、telemetry、camera frame、
  video-control 和 mission-assignment 等 service instances，并管理本地 camera/video source
  与 flight controller adapter。

UavGroundStationApp
  Ground-station service container。它承载 MAVLink command、video、telemetry 和 patrol
  workflow 的 NDNSF users，并内置 drone 可调用的 /UAV/GS/ObjectDetection provider。
  `--serve-object-detection` 独立模式仍保留，用于只测试 object detection service。
```

这种 container model 是有意设计的：真实 UAV 部署需要多个相关服务共享同一个 identity、
本地 GUI 状态、硬件 adapter 和进程生命周期，但不应该把它们的服务名和权限合并成一个单体 RPC
endpoint。

## 一台 PC 和多架无人机的实机部署

这一节面向私有部署：一台 PC 作为 ground station 和 controller，每架物理无人机运行一个
DroneAPP 实例。这个流程不需要连接真实 NDN Testbed。

推荐部署位置：

```text
PC / ground station:
  NFD
  App_ServiceController
  UavGroundStationApp
  可选 /UAV/GS/ObjectDetection provider

每架无人机:
  NFD
  UavDroneApp
  到飞控的 MAVLink 连接
  摄像头或视频源
```

如果要发布 release，建议在 Ubuntu 20.04 build host 上生成 portable tarball：

```bash
./waf build
packaging/uav-release/create-portable-release.sh
```

这个 tarball 会包含 controller、ground station 和 drone app 的 wrapper commands，
以及 NDNSF runtime libraries。只要 build host 上 `ldd` 能看到，它也会把 `ndn-cxx`、
`ndn-svs`、NDNSD、NAC-ABE、OpenABE 和 RELIC 一起打进去。NFD 故意不打包：
它是每台机器自己的 host daemon，管理本机 socket、faces、routes 和 keychain state。
每台机器都应该运行自己的兼容 NFD。

如果目标是依赖很少的 Ubuntu 20.04 实验机器，可以生成更大的 same-OS bundle：

```bash
NDNSF_UAV_RELEASE_INCLUDE_SYSTEM_LIBS=1 \
  packaging/uav-release/create-portable-release.sh
```

把 tarball 拷到目标机器后：

```bash
tar -xzf ndnsf-uav-ubuntu20-x86_64-*.tar.gz
cd ndnsf-uav-ubuntu20-x86_64-*
./scripts/check-runtime-deps.sh
```

当前二进制默认读取 `configs/uav_runtime.conf`。这个文件保留了 demo 用的
`/example/uav/controller`、`/example/uav/gs` 和 `/example/uav/drone/<id>`，
但真实部署的 identity namespace 和 service name 应该写在复制出来的 runtime config
里，而不是改进 C++ 代码。实机部署时先复制配置文件，修改其中的 namespace，并确保
policy file 和 trust schema 与这些名字一致：

```text
cp NDNSF-UAV-APP/configs/uav_runtime.conf /etc/ndnsf/uav_runtime.conf
```

需要临时覆盖时，也可以继续使用命令行参数：

```text
--runtime-config /etc/ndnsf/uav_runtime.conf
--group-prefix /example/uav/group
--controller-prefix /example/uav/controller
--ground-station-identity /example/uav/gs      # 仅 ground station 需要
--drone-prefix /example/uav/drone              # drone identity = <prefix>/<id>
--trust-schema /absolute/path/to/uav-trust.conf
--service-mavlink-execute /UAV/MAVLink/Execute
--service-mission-assign /UAV/Mission/Assign
--service-telemetry-status /UAV/Telemetry/GetStatus
--service-camera-frame /UAV/Camera/GetFrame
--service-camera-video-control-suffix /UAV/Camera/Video
--service-camera-recording-manifest-suffix /UAV/Camera/Recording/Manifest
--service-gs-object-detection /UAV/GS/ObjectDetection
```

每个正在运行的 APP 实例还可以用 `--app-config` 加载自己的实例配置。
上面的 runtime config 是整个部署共享的；app config 是每个进程独有的。
两架无人机就是靠这个区分 drone id、摄像头设备、MAVLink 端口或串口设备，同时共享同一套
service namespace：

```text
UavDroneApp --app-config /etc/ndnsf/drone-A.conf
UavDroneApp --app-config /etc/ndnsf/drone-B.conf
UavGroundStationApp --app-config /etc/ndnsf/ground-station.conf
```

示例模板在：

```text
NDNSF-UAV-APP/configs/drone-A.conf
NDNSF-UAV-APP/configs/drone-B.conf
NDNSF-UAV-APP/configs/ground-station.conf
```

命令行参数会覆盖 app config 和 runtime config，所以 MiniNDN 和临时实验仍然可以不改文件，
直接覆盖端口、视频源或目标无人机。

如果想改成 `/ndn/ndnsf/uav-demo/...` 这样的命名，优先修改 runtime config 或使用命令行参数，
不要改代码并重新编译；同时同步更新 `configs/uav_demo.policies` 和部署用 trust schema。

### 证书 bootstrap

如果 operator 能物理接触 PC 和无人机，NDNCERT 是可选的。使用根目录 README 里的手动
root-signed certificate 流程即可：每个节点在本机生成私钥，只把 certificate request 发给
CA/root 机器，然后安装 CA/root 签回来的 certificate 和 root certificate。

用当前 UAV namespace 的最小例子：

```bash
# 在 PC / CA 机器上
ndnsec key-gen -t r /example/uav > root.cert
ndnsec cert-install -f root.cert

# 在 drone A 上
ndnsec key-gen -n -t r /example/uav/drone/A > drone-A.req

# 回到 PC / CA 机器上
ndnsec cert-gen -s /example/uav -i ROOT drone-A.req > drone-A.cert

# 回到 drone A 上
ndnsec cert-install -f root.cert
ndnsec cert-install -f drone-A.cert
ndnsec-ls-identity -c
```

对 `/example/uav/gs`、`/example/uav/controller` 和每个 drone identity 重复
request/sign/install 流程。这个流程不要 export/import safebag；私钥应该留在拥有该 identity
的机器上。

### 生产部署 Trust Schema

示例 trust schema 中的 `type any` 只用于本地 example 和自动化 regression。实机部署必须把
这个 anchor 换成真实 deployment root certificate。实机部署不要使用 `examples/trust-any.conf`。

把 `examples/trust-schema.conf` 复制成部署专用文件，并把：

```conf
trust-anchor
{
  type any
}
```

替换为指向 root certificate 的绝对路径：

```conf
trust-anchor
{
  type file
  file-name "/absolute/path/to/root.cert"
}
```

controller、ground station、drone、repo 和 provider 的 certificate 都应该由这个 root 签发，
或者由同一 namespace 下的层次化子 CA 签发。

### 网络和 NFD

每台机器都必须运行 NFD，并且机器之间要有 face/route 让这些 prefix 可达：

```text
/example/uav
/example/uav/controller
/example/uav/gs
/example/uav/drone/<id>
```

具体 face 和 route 取决于部署网络。第一次私有 LAN 测试可以在 PC 和每架无人机之间配置静态
UDP face，并把 UAV namespace 路由到 PC/controller。

### 部署前检查

在真实机器上启动 containers 前，先运行 preflight checker。它不会修改系统状态，只检查 NFD
是否可达、目标 identity certificate 是否已经安装、trust schema 是否存在，以及 ffmpeg、YOLO、
video source、MAVLink UDP port 等本地 adapter 条件是否合理。它还会检查同一个 identity
是否存在多套本地 key/certificate；这种情况可能导致 Controller 把 permission response 加密给旧证书。

在 Controller 上：

```bash
python3 NDNSF-UAV-APP/tools/uav_deployment_check.py \
  --role controller \
  --runtime-config NDNSF-UAV-APP/configs/uav_runtime.conf \
  --policy-file NDNSF-UAV-APP/configs/uav_demo.policies \
  --expected-cert /example/uav/drone/A=/path/to/drone-A.cert
```

`--expected-cert` 不是必须的，但多机器部署时建议使用。先在远端节点导出公开证书：
`ndnsec cert-dump -i /example/uav/drone/A > drone-A.cert`，复制到 Controller，然后让
preflight 对比 Controller keychain 中用于加密权限的证书是否就是这张证书。

release wrapper 也可以在启动 app 前自动运行同样的检查：

```bash
NDNSF_UAV_PREFLIGHT=1 ./bin/ndnsf-uav-controller
NDNSF_UAV_PREFLIGHT=1 \
NDNSF_UAV_PREFLIGHT_ARGS="--expected-cert /example/uav/drone/A=certs/drone-A.cert" \
  ./bin/ndnsf-uav-controller
```

在 PC / ground station 上：

```bash
python3 NDNSF-UAV-APP/tools/uav_deployment_check.py \
  --role ground-station \
  --runtime-config NDNSF-UAV-APP/configs/uav_runtime.conf \
  --app-config NDNSF-UAV-APP/configs/ground-station.conf \
  --trust-schema /absolute/path/to/uav-trust.conf
```

在 drone A 上：

```bash
python3 NDNSF-UAV-APP/tools/uav_deployment_check.py \
  --role drone \
  --runtime-config NDNSF-UAV-APP/configs/uav_runtime.conf \
  --app-config NDNSF-UAV-APP/configs/drone-A.conf \
  --trust-schema /absolute/path/to/uav-trust.conf \
  --flight-controller-backend udp \
  --mavlink-udp-host 127.0.0.1 \
  --mavlink-udp-port 18570 \
  --mavlink-udp-listen-port 14550
```

如果使用串口飞控连接，把 MAVLink backend 参数换成：

```bash
  --flight-controller-backend serial \
  --mavlink-serial-device /dev/ttyAMA0 \
  --mavlink-serial-baud 57600
```

如果看到 warning，例如 trust schema 里仍然使用 `type any`，实飞前应该修正。failure 必须先修好，
再启动 service containers。

### 启动顺序

在 PC 上：

```bash
nfd-start

./build/examples/App_ServiceController \
  --controller-prefix /example/uav/controller \
  --policy-file NDNSF-UAV-APP/configs/uav_demo.policies

./build/examples/UavGroundStationApp \
  --target-drone A \
  --patrol-drones A,B,C \
  --video-bitrate-kbps 8000 \
  --video-width 480 \
  --group-prefix /example/uav/group \
  --controller-prefix /example/uav/controller \
  --ground-station-identity /example/uav/gs \
  --drone-prefix /example/uav/drone \
  --trust-schema /absolute/path/to/uav-trust.conf
```

交互式 ground-station 模式下，如果没有显式传入 `--ground-station-identity`，APP 会在启动
NDNSF runtime 之前弹出一个小的 certificate picker。这个窗口会列出本机 ndn-cxx PIB
里已经安装的 identities；选择那个 certificate 能被当前 deployment trust schema 信任的
identity。自动化 MiniNDN/smoke 模式会跳过这个窗口；手动运行时也可以用 `--no-cert-dialog`
跳过。

在 drone A 上：

```bash
nfd-start

./build/examples/UavDroneApp \
  --drone-id A \
  --video-source NDNSF-UAV-APP/videos/drone.mp4 \
  --flight-controller-backend udp \
  --mavlink-udp-host 127.0.0.1 \
  --mavlink-udp-port 18570 \
  --mavlink-udp-listen-port 14550 \
  --group-prefix /example/uav/group \
  --controller-prefix /example/uav/controller \
  --drone-prefix /example/uav/drone \
  --trust-schema /absolute/path/to/uav-trust.conf
```

其它无人机使用相同命令，但要使用唯一的 `--drone-id` 和各自的飞控连接参数。如果 companion
computer 通过串口 MAVLink 连接 PX4 飞控，可以使用：

```bash
./build/examples/UavDroneApp \
  --drone-id A \
  --video-source /dev/video0 \
  --flight-controller-backend serial \
  --mavlink-serial-device /dev/ttyAMA0 \
  --mavlink-serial-baud 57600 \
  --group-prefix /example/uav/group \
  --controller-prefix /example/uav/controller \
  --drone-prefix /example/uav/drone \
  --trust-schema /absolute/path/to/uav-trust.conf
```

如果无人机上使用 `mavlink-router`，继续使用 `--flight-controller-backend udp`，并把
`--mavlink-udp-host` / `--mavlink-udp-port` 指向 router 的本地 endpoint。`mavlink-router`
这个 backend 名字也可以作为 UDP 路径的 alias。

### GPS 数据来源

DroneAPP 通过 MAVLink telemetry 从飞控获得 GPS 和 EKF readiness。真实无人机部署中，
GPS unit 连接到 FlightController 上，由 PX4/ArduPilot 负责 GPS 融合、解锁检查、
起飞 readiness 和飞行控制。companion computer 不再直接扫描 USB 或串口 GPS 设备。

### 飞控 readiness 和安全策略

`UavDroneApp` 现在会从 UDP 或 serial backend 解析常见 MAVLink 状态消息。Telemetry response
会在飞控提供时包含 `heartbeat_seen`、`armed`、`flight_controller_ready`、`gps_ready`、
`battery_ready`、`readiness`、`ready_for_takeoff`、`gps_fix_type`、
`gps_satellites_visible`、`gps_fix_name`、`ekf_ready`、`system_status_name`、
`landed_state_name`、`battery_voltage_v`、`battery_current_a`、`altitude_m`、
`groundspeed_mps` 和 `battery_percent`。Ground station 会在 telemetry / mission 视图里显示
这些字段，让 operator 能看到当前选中的 drone 是否真的 ready。

Ground station 会把这些值保存成 typed `TelemetryState` 和 `MissionState` snapshot。
左侧 vehicle list、地图 marker、inspector panel 和 mission 控件都从同一个状态模型刷新，
不再靠临时 status 字符串解析，所以多无人机 UI 状态会始终跟当前选中的 drone 对齐。
Mission upload response 和后续 telemetry 都会更新同一个 `MissionState`；`uploaded`、
`executing`、`stopping` 这些 phase 会直接决定 Start Mission 和 Stop Patrol 按钮状态。
Start Mission 还会把 mission phase 和 typed `FlightSafetyGateState` 组合起来判断，因此 mission
上传完成后，如果对应 patrol drone 还没有可用 readiness/link/safety state，GUI 会显示为 blocked。
Mission-control model 还会记录 typed upload/start/stop reason，例如 `waiting-heartbeat`、
`progress-active` 和 `ok`，所以 UI 和 smoke test 不需要再反推为什么某个 mission 按钮被禁用。
Stop Patrol 对 uploaded 或 active mission 仍然保持可用，避免异常状态下反而无法让无人机降落。

Ground station 还会为每架 drone 保存最近一条飞控命令的 typed `FlightCommandState`。
Targeted MAVLink response、command timeout、readiness gate 拦截，以及 command-in-flight
丢弃都会更新这个模型。因此 vehicle list 和 inspector 可以直接显示最近命令、ACK result、
flight-controller state，以及 timeout/block reason，而不需要再解析临时日志字符串。

命令 response 不再只代表“bytes 已转发”。对于标准 MAVLink `COMMAND_ACK`，backend 会返回
`ack_result`、`ack_command_id` 和 `ack_raw_result`。非 manual command 只有在飞控 ACK 为
`accepted` 或 `in-progress` 时才算成功；否则 NDNSF response 会是失败。

Manual-control safety 采用保守策略。Drone 只会在很短的新鲜窗口内重放最新 `MANUAL_CONTROL`
frame。窗口过期后，它会发送一次 neutral manual-control frame，然后停止重放，直到收到新的
GS command。这样可以避免链路卡住后旧的键盘/手柄输入一直持续生效。
GS 在发送 manual-control update 前也会检查最近 telemetry：必须看到 heartbeat、flight
controller ready，并且无人机已经 armed。如果这些条件不满足，GS 会抑制手操包，并显示
readiness reason，而不是继续向链路里灌入无效控制请求。

这些信息也会暴露成 typed `SafetyState`。Drone telemetry 会报告当前 link state、
manual-control freshness、replay 是否活跃、neutral fallback 是否已经发送，以及 replay
count。GS 的 vehicle list、地图 marker 和 inspector 都直接渲染这个模型，所以 stale
manual input 和 heartbeat loss 会表现成状态，而不是只藏在 backend log 里。
GS 还会根据最近一次收到的 `TelemetryState` 在本地推导 telemetry age。Ground-station
配置里的 `link-stale-ms`、`link-lost-ms` 和 `lost-link-action` 决定什么时候把选中的
drone 显示为 `stale` 或 `lost`；这只是 operator 诊断状态，不改变 NDNSF service
protocol。

Takeoff 会受 telemetry state 保护：GS 在发送 Targeted takeoff command 前，必须看到
heartbeat、flight-controller readiness、GPS/EKF readiness、电池 readiness、armed 状态，
并且 `landed_state_name` 必须明确是 `on-ground`。
UI 也增加了 Emergency Stop 按钮，走同一条 Targeted MAVLink 路径。Emergency Stop
被当作 safety-critical command：GS 发送前会先退出手操模式，并使用独立的 in-flight guard，
不会仅仅因为普通 MAVLink command 仍在等待 response 就被丢弃。

任何真机电机测试前：

1. 拆掉螺旋桨。
2. 确认证书、trust schema、NFD route 和 Drone/GS identity 名字都正确。
3. 先用 `mock` 或 SITL 验证 flight-controller backend。
4. 确认 telemetry 能看到 heartbeat、GPS/EKF readiness、电池状态、arming 状态和 command ACK。
5. 在无桨状态测试 `Arm`、`Disarm`/`Land`、neutral manual control 和 emergency-stop 行为。
6. 然后才进入系留或低风险真机短测。

不要把未经验证的 demo 直接装桨运行。

### 操作流程

1. 在 PC 上启动 NFD 和 `App_ServiceController`。
2. 在每架无人机上启动 `UavDroneApp`，等 Drone 窗口显示 ready。
3. 在 PC 上启动 `UavGroundStationApp`。
4. 在左侧 vehicle list 选择目标无人机。
5. 点击 `Arm`，然后点击 `Takeoff`。
6. 点击 `Start Control` 后使用键盘手操。
7. 点击 `Start Video` 开启当前选中无人机的图传。
8. 关闭前先点击 `Land`。

接真实飞控前应先使用 mock backend 或 SITL 验证。不要把只为 SITL demo 服务的 safety parameter
改动直接用于真实飞控，除非 operator 明确理解这些参数的影响。Mission assignment 仍然是共享
NDNSF service，由 ACK metadata 参与 selection；针对某架具体无人机的 MAVLink control 和
telemetry 使用 Targeted request。

## 服务名

```text
/UAV/MAVLink/Execute
/UAV/Mission/Assign
/UAV/Telemetry/GetStatus
/UAV/Camera/GetFrame
/example/uav/drone/A/UAV/Camera/Video
/UAV/GS/ObjectDetection
```

共享 drone service 可以由多架 drone 提供，并由 NDNSF 进行 provider selection。针对某个具体节点的
服务使用 `/<provider>/<serviceName>` 命名，因此天然全局唯一。图传控制属于 provider-specific
service，因为 operator 是在开启或关闭某一架 drone 的物理摄像头。Start 和 stop 都位于
`/example/uav/drone/A/UAV/Camera/Video` 这个 provider-specific service 下面，控制动作区分
start 和 stop。`/UAV/GS/ObjectDetection` 由 ground station 提供，用于对最新解码出的直播 frame
执行更重的计算。ObjectDetection request 只携带 frame id、drone id、目标类别等紧凑元数据。
大图、录像片段、报告以及其它大对象应该用 ndn-cxx segmenter/SegmentFetcher 模式发布为 NDN
segmented Data，或者通过 `NDNSF-DistributedRepo` 存到发布者自己的命名空间下，然后在服务请求中
只引用数据名。这样可以保持 service request 很小，避免把 NDNSF invocation payload 变成临时文件传输通道。

## 巡逻任务补偿机制

巡逻任务在 APP 层被建模成一个可能需要多次 NDNSF service invocation 才能完成的业务任务。
NDNSF 层仍然把每一次调用都看成独立 request，每次都有新的 `requestId`、`UserToken` 和
`ProviderToken`；ground station APP 用共享的 `patrol_task_id` 把这些调用关联成同一个巡逻任务。

ground station 维护一个很小的 patrol task ledger：

```text
patrol_task_id
attempt_id
part_id
assigned_provider
state: pending / done / missing / compensated
response_digest
deadline_ms
```

第一次 attempt 会为每个 part 选择候选 provider 并分配任务。如果 deadline 到了之后某些 part
没有有效 response，ground station 会发起一个或多个 compensation request，只携带缺失的
parts。compensation request 不是重新开始整个任务，而是属于同一个 APP 层巡逻任务的另一次
NDNSF 调用。

mission service 仍然是共享服务名 `/UAV/Mission/Assign`。任务分配不应该通过 payload 里的
provider 名字来硬编码。Drone 使用 selective ACK handler 报告自己当前是否有可用 mission
slot：忙碌的 drone 仍然发布 ACK，但 ACK 的 `status=false`，metadata 中带
`mission_busy=true`、`queue=1`；空闲 drone 发布 `status=true`。这样仍然走正常的 NDNSF
provider selection 流程，由 selection 选中空闲 drone。

例子：

```text
Attempt 1:
  part0 -> drone A
  part1 -> drone B

Result:
  part1 done
  part0 missing

Attempt 2:
  part0 -> candidates {drone A, drone B}
  drone A ACK: mission_busy=true
  drone B ACK: mission_busy=false
  selection -> drone B

Task:
  part0 和 part1 都有有效 response 后完成
```

这样 NDNSF core 仍然保持泛化。框架只负责安全 request、ACK、selection、response、provider
token、replay protection 和 timeout 行为；UAV APP 负责 patrol-specific 语义，例如 part
状态、deadline、补偿请求和最终任务完成判断。Ground station 会把这些语义记录到 typed
`MissionProgressState`，并输出 `PATROL_PROGRESS` marker，因此 GUI 和 smoke test 可以直接
跟踪 assigning、waiting-compensation、compensating、completed 和 failed phase，而不需要解析
旧的 ledger 字符串。

## 图传设计

图传不应该建模成长 service response。控制和数据路径应该分开：

```text
Ground station -> /example/uav/drone/A/UAV/Camera/Video/start/<nonce>
  开启当前 drone 的实时图传 downlink。

Drone -> drone-owned frame Data name
  发布签名后的视频 packet：
    /example/uav/drone/A/video/<stream-start-ms>/<packetSeq>

Ground station -> frame Data names
  连续预取递增 packetSeq。Data content 携带 frame metadata，所以即使每帧大小不同，
  名字仍然可以保持顺序。

Ground station -> /example/uav/drone/A/UAV/Camera/Video/stop/<nonce>
  Drone 停止实时图传，并停止服务新的 stream packets。
```

当前 GUI 图传控制路径使用 NDNSF generic service invocation。完整的 provider-specific name
`/example/uav/drone/A/UAV/Camera/Video` 就是 service name；provider prefix 是 service name 的一部分，
用于让这个摄像头控制服务全局唯一。高频 video packets 仍然作为 drone namespace 下的签名
NDN Data 拉取，因此 generic request/response 路径只承载控制消息，不承载视频字节流。

摄像头采集、本地录像和实时图传是三个独立状态：

```text
camera capture
  Drone 本地摄像头采集，可以由 DroneAPP 启动配置决定是否开机即启用。

local recording
  Drone 把原始 H264 chunks 写入本进程内嵌的 NDNSF-DistributedRepo。
  这由 drone 配置决定，不由 ground station 的 Start Video 按钮默认决定。

live streaming
  ground station 通过 provider-specific video-control service 请求的低延迟 NDN Data 图传。
```

因此，drone 可以在没有 ground station 观看时仍然持续采集并保存到本地 repo；ground station
也可以临时请求直播而不改变 drone 的本地录像策略。video-control response 会返回实际生效的
`capture`、`recording`、`recording_session_id`、`recording_object_prefix`、
`recording_chunks` 和 `recording_bytes`。

当前实现把 `NDNSF-UAV-APP/videos/drone.mp4` 交给 `ffmpeg` 循环读取，编码成低延迟 H264 byte stream，
然后切成符合 NDN packet 大小的 video packets。这不是普通 NDN segmentation，
也不是把一个大 Data object 切段。每个 Data 都是一个独立 video packet，名字只由 stream
启动时间和递增 `packetSeq` 组成；一个 Data 只包含一个 frame 的一部分，绝不会混入两个 frame
的 bytes。Data content 开头有紧凑 metadata header，包含 `frame_seq`、
`frame_segment_index`、`frame_segment_count`、`frame_first_packet_seq`、
`frame_last_packet_seq`、`bucket_packet_count`、`capture_ms` 和 `key_frame`，后面才是本
packet 承载的 frame bytes。

当前阶段采用轻量级前向纠错（FEC）：默认每帧使用 `N = K + 1`（一个校验分片）。发送端在同一
`packetSeq` 序列内发布全部数据分片和校验分片，并在 header 中附带 `fec_data_shards`、
`fec_parity_shards`、`fec_symbol_index`、`fec_symbol_count`、`fec_data_lengths`。
ground station 按 frame 级别缓存分片；当某一帧只缺一个 data 分片时，用 XOR 与其余分片恢复后再喂给
解码器。这样名字依然可预测，允许一定程度的丢包与乱序，而不会先天影响抓包与预获取。

这种命名方便预获取：ground station 可以连续请求 `/<stream>/0`、`/1`、`/2`……，不再需要处理
秒级边界。frame 重组依赖 Data content 里的 metadata，不依赖 NDN FinalBlockId。某个 frame 的
所有 packet 到齐后，GUI 立刻显示这一帧。迟到或缺失的旧非关键帧 packet 会被跳过，避免影响
实时性。后续切换到 H264/H265 时可以沿用同样命名，必要时等待 key frame，并在下一个可用 key
frame 到来前丢弃过期 delta frame。

真实部署时，drone 配置默认使用 `video-source auto`。在这个模式下，`UavDroneApp` 会自动选择
第一个可用的本机 USB/V4L2 摄像头设备，例如 `/dev/video0`；如果没有可用摄像头，就为了本地
troubleshooting 回退到 sample video `videos/drone.mp4`。如果部署时需要固定某个摄像头或文件，
可以在 drone 配置里写 `video-source /dev/videoX` 或具体文件路径。USB UVC 摄像头采集默认使用
自适应参数：`camera-v4l2-input-format auto`、`camera-v4l2-input-size auto` 和
`camera-v4l2-input-fps 0`。在这个模式下，Drone 会查询摄像头能力，优先选择保守的
YUYV 640x480；`0` 表示不强制 V4L2 input frame interval，只在后续 encoder filter 里做降采样。
这样可以避开一些嵌入式 USB 控制器或摄像头在显式 frame-rate probe 下卡住的问题。因此实机部署前应先用
`ffmpeg -f v4l2 -list_formats all -i /dev/videoX` 确认可用格式，设备和 USB 口稳定后再覆盖这些
配置项。选中的 source 会通过 `ffmpeg`/V4L2 编码成低延迟 H264 byte stream，并拆成
NDN-sized video packets 发布。

ground station 会在同一个 `/<drone>/UAV/Camera/Video` 控制服务请求里带上目标图传 bitrate
和 frame width。drone 会先把请求值 clamp 到自己支持的 bitrate/width 范围，再把 accepted bitrate
映射成 `ffmpeg` 的 H264 CRF quality 设置，并设置编码缩放宽度。响应里会同时返回 requested
bitrate、accepted bitrate、requested width、accepted width、FPS、encoder quality 和 packet
payload 大小。ground station 再用 accepted bitrate 以及每个 packet metadata 里的
high-watermark 估计 prefetch window，避免盲目请求无边界的 packet sequence number。同时 GS 会用
实测 video RTT 动态调整 live prefetch window、lookahead、decoder reorder window、Interest
lifetime、probe backoff 和 missing-packet skip timeout。timeout 和 Nack 压力也会进入同一套策略：
当链路开始丢包或 chunk 延迟变高时，GS 会降低 lookahead/prefetch 压力，并缩短 decoder
等待缺失 delta chunk 的时间。这样低 bitrate / 低 FPS 摄像头不会
过度预取，高 bitrate stream 也能保持足够的 in-flight Interests，减少卡顿。当前接收端会把
这些决策保存成 `VideoAdaptiveState`，因此 video panel、selected-drone inspector、左侧
drone row 和 MiniNDN smoke logs 都能直接显示 RTT、window、lookahead、pressure、
missing-packet timeout、pending chunks、decoded-frame progress、主要压力来源、policy reason
和当前码率建议，而不用解析 packet log。这个码率建议只走显式控制：GS 不会在后台静默改变 drone encoder；操作者可以点击
`Apply Bitrate`，这会先停止当前直播，等待 drone 的 Stop response，再用建议码率启动新的
stream，让 packet sequence、stream id 和 decoder state 保持清楚。默认码率策略是 `manual`。
实验时也可以用 `--video-bitrate-policy auto-after-pressure` 启动 GS；这种策略只会在压力持续
超过 `--video-bitrate-auto-pressure-ms` 后才应用非 hold 建议。把这个值设成 `0` 时会进入更激进的
回归测试模式，第一次基于压力的建议就会触发应用。
当前 demo
默认是 8000 kbps、480 px frame width、30 FPS 的 H264 stream。提高 bitrate 会增加 stream 质量和
packet 数据量；提高 frame width 才会让 GUI 里显示的视频更大。

点击 `Stop Video` 后，drone 会停止实时 stream，清空 pending Interests 和缓存的 stream packets，
并忽略已经停止的 stream 上迟到的 frame Interests。这样可以避免 GS 端视频停了，但 drone 还在继续
服务旧缓存包。GS 即使已经停止本地 decoder，也会继续发送 stop control request；如果 control
response 超时，NDNSF 会写出 selection-status timeout diagnostics，GS 会提示操作者在 drone
仍显示 streaming 时再次点击 `Stop Video`。重复 stop request 是安全的，因为 drone 把 stop
作为幂等操作处理。如果 drone 配置启用了 `camera-capture-on-start` 或
`camera-record-to-local-repo`，本地摄像头采集/录像可以在直播停止后继续运行。

后续可以增加每个 stream 独立的小 SVS group 来发布 frame-name announcement，但它应该和主 UAV
control group 分开，避免高频视频信令影响 command、telemetry 和 mission traffic。同一个控制服务
以后也可以携带 H265、GOP 等参数。

Frame data 应该发布在 drone 自己的 namespace 下，由 drone 签名，并可选通过
`NDNSF-DistributedRepo` 存储，用于回放或任务后分析。

## GUI 计划

当前第一版 GUI 使用 gtkmm，因为它已经在本仓库构建配置中可用。wxWidgets 仍然可以作为后续目标，
尤其是当我们希望统一跨平台 native GUI toolkit 时。

GUI 应该是同一套 service layer 上的 ground-station frontend：

- mission map 和 role assignment panel；
- live telemetry table；
- 由 frame-name/prefetch pipeline 驱动的视频预览；
- keyboard/gamepad control mode；
- object-detection result overlay；
- drone availability 和 selected-provider view。

当前 ground-station 窗口按 QGroundControl Fly View 的思路做了第一版结构：
左侧是多无人机列表，中间是 map/mission context 与视频区域，右侧是 telemetry、服务能力和
command status inspector。它还不是完整 QGC；map 仍然是轻量 OpenStreetMap tile + text workspace，
不是完整 GIS map，但启动时会以 ground station 为中心，也就是 University of Memphis 附近，
并在地图上画出带标签的 `GS`、drone 和 mission waypoint marker。地图工具栏提供放大、缩小、
`Center GS`、`Undo WP` 和 `Clear WPs` 按钮；拖动地图可以查看附近区域，按 `Center GS`
会回到以 ground station 为中心。这里参考 QGroundControl 的 Plan/Fly 分工做了简化版：
点击地图会连续追加 `WP1`、`WP2` 和后续 waypoint，规划时可以撤销/清空，之后再上传并启动 mission。
drone marker 来自 `/UAV/Telemetry/GetStatus` 返回的 lat/lon/alt；使用 UDP MAVLink backend
时，这些 telemetry 来自 flight controller，所以 marker 对应的就是 ManualControl 正在控制的无人机。
左侧 vehicle list 可以切换当前控制目标，同时 telemetry poller 会轮流请求预期 drone 列表里的 A/B，
因此两架无人机都能留在地图上显示。如果地图上已经有至少两个 waypoint，按
`Upload Patrol Mission` 后，ground station 会按巡逻无人机数量对这条 route 做 waypoint
聚簇。每架无人机拿到一团空间上相近的 waypoint，簇内再整理成一条较短的局部路线，并在最后
追加该 drone 上传 mission 时从 telemetry 采样到的出发位置作为返回点。如果 telemetry 不可用，
才退回到该簇的第一个 waypoint。如果没有显式 route，则继续使用中心点和边长为每架无人机自动
生成一个相邻的 waypoint sector。
布局已经为后续 video/map 前景切换和 mission tools 留出了位置。

如果要做离线 demo，可以先准备一小块 Memphis 地图 tile 缓存：

```bash
python3 NDNSF-UAV-APP/tools/prepare_memphis_offline_map.py
```

默认会下载 University of Memphis 附近 zoom 14、15、16 的 3x3 OpenStreetMap tile，保存到
`NDNSF-UAV-APP/maps/osm/`。ground-station GUI 会优先读取这个离线缓存，其次读取 `/tmp`
缓存，最后才尝试联网。MiniNDN launcher 在缺少 tracked cache 时也会先在宿主机侧预取这些
zoom levels，因此节点网络命名空间隔离后地图缩放按钮仍然能工作。

命令路径保持不变：GUI action 在 ground station 端构造 MAVLink bytes，然后通过 NDNSF Targeted
service invocation 发送 opaque MAVLink frame。

NDNSF 内置 NDNSD 默认仍然不启用。对 UAV demo 来说，NDNSD 可以作为低频服务公告通道：
DroneAPP 可以间歇性发布自己安装的服务，例如 video control、Targeted MAVLink execution、
telemetry、camera frame 和 mission assignment。core 现在提供了通用的
`ServiceProvider::publishServiceInfo(...)` 钩子，但 MiniNDN launcher 仍然保持
`NDNSF_DISABLE_NDNSD=1`，因为旧 NDNSD runtime 路径需要单独做兼容性修复后才适合放进 GUI demo。
性能和低延迟测试应保持默认禁用。

## 开发路线

当前应用已经可以作为 MiniNDN 和 SITL 演示系统使用。下一步目标是把它推进成可以部署到真实
UAV service-container workload 的应用。计划顺序如下：

1. **收束状态模型。** 现在 telemetry、readiness、mission、video、command 和 safety state
   已经驱动主要飞控按钮、selected-drone action model、selected-drone view-state gate reason、
   inspector/map 文本、地图 marker、左侧 drone list 和 MiniNDN smoke markers。Mission Start/Stop 现在也通过 typed mission start gate
   把 `MissionState`、flight readiness 和 safety 组合起来判断，并向 UI 与 smoke test 暴露
   upload/start/stop reason 以及 action-pending tooltip。周期性 telemetry 不会再让空的 idle
   mission fields 覆盖 GS 侧更新的 mission state。Patrol task progress 现在也有
   typed `MissionProgressState`，用于 assignment、compensation、completion 和 return-home
   planning；ground-station mission 按钮也会使用这个 progress model，在 patrol assignment 或
   compensation 仍然 active 时阻止重复 upload/start。左侧 drone row 和地图 marker 也会使用同一个
   progress model，让操作者不用打开 inspector 就能看到 compensation active 或 completed 状态。
   左侧 vehicle list 现在使用共享的 `DroneListRowState` 派生逻辑，保证 selected/standby、
   readiness、mission progress、video、command 和 safety 摘要与其它状态层显示一致。
   直播链路现在也有 typed `VideoAdaptiveState`，记录 RTT、prefetch window、lookahead、
   timeout pressure、probe pressure、decoder backlog 和 decoded-frame progress；video panel、
   selected-drone view、左侧 drone row 和 MiniNDN smoke logs 都从这个 model 读取状态，而不是
   解析内部 packet log。后续新增 mission/video/safety UI 路径时继续坚持这一点：只要有 typed
   state model，GUI 就不应该再从临时 status string 推断状态。
2. **Drone headless 部署模式。** 保持 Drone container 可以在 ODROID 这类板子或真实机载计算机
   上运行，而不依赖 GUI/X server。headless 模式只运行 NDNSF、MAVLink、camera、repo、
   telemetry 和 mission services。
3. **飞控 readiness 和安全 gate。** 在 arm、takeoff 和 mission execution 前，清楚显示并检查
   heartbeat、GPS fix、EKF readiness、battery、arming state、mode 和 landed state。
   Manual control 必须超时回到 neutral，emergency stop 和 lost-link 行为必须明确。
4. **自适应视频服务质量。** 继续把 video 当作 NDNSF service workload：requested bitrate、
   accepted bitrate、RTT、backlog、timeout pressure、key-frame recovery 和 FEC 应该驱动
   prefetch 与 skip 决策，而不是依赖固定常数。当前 GS 已把这些决策记录为
   `VideoAdaptiveState`，包括 advisory 的 decrease/hold/increase 码率建议，以及说明主要压力源的
   `primary_pressure` 和 `policy_reason`。核心窗口、
   lookahead、timeout 和码率建议现在由 typed `VideoAdaptivePolicyInput` 到
   `VideoAdaptivePolicyDecision` helper 计算，并有单元测试覆盖 pressure、high-RTT 和
   recovery 行为，避免只为某个 MiniNDN 拓扑调整隐藏常数。现在 `Apply Bitrate` 会把非 hold
   建议转换成显式 Stop-then-Start stream restart。MiniNDN smoke 也可以注入受控的
   congestion/backlog/probe pressure profile，并验证 `primary_pressure` 会按预期切换。默认策略仍然是 manual；
   `auto-after-pressure` 可以用于实验场景，在压力持续一段时间后自动应用建议。
5. **任务协作模型。** 把当前 patrol demo 提升成可复用 mission model，包括 `MissionPlan`、
   `MissionPart`、assignment、progress、failure/compensation 和 return-to-home 语义。Patrol
   route clustering、默认 sector 生成、drone assignment 和 return-to-departure waypoint
   插入现在都放进共享 typed mission helper，因此 GUI workflow、service container 和测试使用
   同一套协作模型。GS 也会通过 typed view state 暴露最新 mission plan 和 selected drone 的
   mission part，而不是只在内部 assignment log 里体现。GUI 现在也会用同一个 helper 根据画出的
   waypoints 生成 pre-upload mission preview，因此 mission 发送前就能看到 planned parts。
6. **Repo-backed UAV data products。** 通过 `NDNSF-DistributedRepo` 保存 recording、mission
   image、telemetry log、object-detection event 和 report，使用 publisher-owned name、
   encrypted payload 和 manifest-based discovery。Camera recording manifest 现在会解析成
   typed `RecordingDataProductState`，让 GS playback 和 smoke test 根据 product availability /
   playability 判断状态，而不是解析临时字符串。
7. **Distributed inference 集成。** 当 object detection 或 image workflow 需要在 ground station、
   drone 和 edge machine 之间拆分执行时，把相关流程接入 `NDNSF-DistributedInference`。

## 构建

从仓库根目录执行：

```bash
./waf configure
./waf build --targets=UavDroneApp,UavGroundStationApp
```

## 两窗口图传 demo

`UavDroneApp` 和 `UavGroundStationApp` 会先启动 NDNSF runtime，等 runtime ready 后才显示
GUI 窗口。这样 NDNSF/NAC-ABE 构造、SVS 初始化、权限获取仍然尽量接近普通命令行 example 的
写法；GUI main loop 只在 runtime ready 之后启动。
真实无人机上如果不需要本地 operator 窗口，可以使用 `UavDroneApp --headless`。headless
模式运行同一套 NDNSF 服务、MAVLink backend、摄像头和本地录像 pipeline，但不会创建 GTK
窗口，也就不需要在小型机载计算机上额外使用 Xvfb。

先启动 NFD 和 UAV controller，然后启动一个 drone 窗口和一个 ground-station 窗口：

```bash
nfd-start

./build/examples/App_ServiceController \
  --controller-prefix /example/uav/controller \
  --policy-file NDNSF-UAV-APP/configs/uav_demo.policies

./build/examples/UavDroneApp --drone-id A --video-source /dev/video0
# 真实机载计算机没有显示器时：
# ./build/examples/UavDroneApp --drone-id A --video-source auto --headless
./build/examples/UavGroundStationApp --target-drone A \
  --video-bitrate-kbps 8000 --video-width 480
```

如果只是本地调试而没有摄像头，可以临时把视频文件传给 `--video-source`；MiniNDN launcher
会优先尝试真实/虚拟摄像头，只有失败时才回退到这种直接文件输入。

Drone 摄像头策略由 drone 端配置决定。如果希望 DroneAPP 启动后就保持采集，并把原始 H264
chunks 保存到本地 SQLite-backed embedded repo：

```bash
./build/examples/UavDroneApp \
  --drone-id A \
  --video-source /dev/video0 \
  --camera-capture-on-start \
  --camera-record-to-local-repo \
  --camera-record-repo-path /var/lib/ndnsf-uav/drone-A-camera.sqlite3 \
  --camera-record-object-prefix /muas/drone/A/repo/camera/recording
```

默认示例 `drone-A.conf` / `drone-B.conf` 仍然关闭 recording；真实无人值守部署时可以在
每台 drone 自己的配置文件里设置 `camera-record-to-local-repo true`。
启用 recording 后，drone 还会暴露一个 provider-specific manifest service：

```text
/<drone>/UAV/Camera/Recording/Manifest
```

例如 `/example/uav/drone/A/UAV/Camera/Recording/Manifest` 会返回当前 recording session id、
object prefix、object naming pattern、chunk count、byte count，以及 first/last chunk object
name。这样 GS 或任务后报告可以发现本地 repo objects，而不需要猜文件路径或扫描 SQLite store。
manifest 故意只暴露 service object names，不暴露 drone 本地 SQLite 文件路径。

ground station 针对当前选中的 drone 提供 `Find Recordings` 和 `Play Recording` 两个按钮。
`Find Recordings` 调用上面的 manifest service；`Play Recording` 使用 recording helper
按照 manifest 里的对象名去取加密 repo Data。GS 会把 manifest 保存成 typed
`RecordingDataProductState`，其中包含 availability、encryption、playable-state 和可预测
chunk name helper；这是第一个具体 repo-backed UAV data product model，后续 mission image、
telemetry log、detection event 和 report 会沿用这个方向。chunk 路径故意不是 NDNSF service：

```text
/<drone>/repo/camera/recording/<session-id>/chunk/<index>
```

manifest service 是授权点。它的 NDNSF-protected response 会把 recording encryption metadata
和 content key 只返回给有权限的 viewer。repo 里保存的是 hybrid AES-GCM 加密后的 H264 chunks；
drone 只把这些 encrypted chunks 作为普通 signed NDN Data 提供，ground station 取回后在本地
解密，再交给和直播相同的低延迟解码器播放。只拿到 chunk Data 但没有 manifest key，不能观看录像。

如果只想验证本地 recording，不启动 GUI，也不需要 GS 交互，可以运行：

```bash
rm -f /tmp/ndnsf-uav-camera-record-smoke.sqlite3
./build/examples/UavDroneApp \
  --auto-camera-record-smoke \
  --video-source NDNSF-UAV-APP/videos/drone.mp4 \
  --camera-record-to-local-repo \
  --camera-record-repo-path /tmp/ndnsf-uav-camera-record-smoke.sqlite3 \
  --camera-record-chunk-limit 3
```

成功时会在原始 H264 chunks 写入本地 repo 后打印带 last chunk object name 的
`DRONE_CAMERA_RECORD_SMOKE_OK`。

在 ground-station 窗口点击 `Arm`、`Takeoff` 或 `Land`，可以通过 Targeted MAVLink command
控制目标 drone。如果要手操飞行，先点击 `Start Control`，然后在控制面板里选择 `Keyboard`
或 `Xbox Gamepad`。如果本机没有可读的 `/dev/input/js*` 手柄设备，手柄选项会灰色不可选。
布局采用接近 QGroundControl/Mode-2 的习惯：左摇杆负责 yaw/throttle，右摇杆负责
roll/pitch。

键盘布局：

```text
左摇杆模拟                         右摇杆模拟
        R throttle up                     W pitch forward
A yaw left   D yaw right          Q roll left   E roll right
        F throttle down                   S pitch back

命令键：I arm, T takeoff, L land, V video start, X video stop
```

Xbox 手柄布局：

```text
左摇杆：yaw / throttle
右摇杆：roll / pitch
A: arm       Y: takeoff
B: land      X: start/stop video
```

按住某个键、拨动摇杆或按下手柄按钮时，对应控件会变成黑色，让 operator 明确知道当前输入。control mode
开启期间，GS 会持续低频发送 Targeted `MANUAL_CONTROL` 更新；没有按键时也会发送 neutral update。
Drone 会在本机用较高频率短时间重放最新 manual frame，这样即使 NDNSF request/response 有抖动，
PX4 看到的仍然是连续控制流。Manual-control response 会带回当前可用的飞控状态字段，例如
`altitude_m`、`groundspeed_mps`、`battery_percent` 和 controller state。
当 MiniNDN launcher 自己启动 PX4 SITL 时，也会给 DroneAPP 传入
`--configure-px4-sitl-demo-params`。DroneAPP 会通过 MAVLink `PARAM_SET`
设置几个只服务于 demo 的参数，让 PX4 更能容忍 Targeted 手操控制流里的网络抖动：
`COM_RC_LOSS_T=30`、`COM_FAIL_ACT_T=25`、`NAV_RCL_ACT=1`。手动启动 DroneAPP
时默认不会启用这个选项，因此真实飞控仍然保留自己的安全策略，除非 operator 显式选择 SITL demo
行为。

基本操作顺序：

1. 等 Drone 窗口显示 `ready for takeoff`。
2. 点击 `Arm` 或按 `I`。Arm 的意思是解锁飞控，让飞控允许电机转动并接受飞行命令；只有准备起飞时才使用。
3. 点击 `Takeoff` 或按 `T`。在 PX4/jMAVSim demo 中，这会让无人机起飞到较低的固定悬停高度。
4. 点击 `Start Control` 后再按键手操。`R` 是上升，`F` 是下降；松开按键会发送 neutral control。
5. 结束前点击 `Land` 或按 `L`。

PX4/jMAVSim demo 里，`Takeoff` 当前会生成原始 MAVLink `MAV_CMD_NAV_TAKEOFF`，并使用适合默认
SITL world 的绝对高度值。默认高度特意调得比较低，方便在模拟器视角里观察。后续 telemetry
打通后，应把 operator 输入的相对起飞高度转换为当前 vehicle AMSL 高度，再生成 MAVLink command。

点击 `Start Video`
或在 control mode 下按 `v` 后，drone 窗口应该显示正在图传，ground-station 窗口应该能播放收到的
live video packet stream。点击 `Stop Video` 或在 control mode 下按 `X` 后，drone 窗口应该回到
`Video stopped`。

如果要不手动点击按钮做 GUI smoke test：

```bash
./build/examples/UavDroneApp --drone-id A --video-source NDNSF-UAV-APP/videos/drone.mp4
./build/examples/UavGroundStationApp --target-drone A \
  --video-bitrate-kbps 8000 --video-width 480 \
  --auto-video-test --auto-stop-seconds 10
```

如果要做 Targeted MAVLink command smoke test：

```bash
./build/examples/UavGroundStationApp --target-drone A --auto-mavlink-test
```

如果要测试 GUI 的 `a/t/l` 键盘快捷键路径：

```bash
./build/examples/UavGroundStationApp --target-drone A --auto-keyboard-test
```

如果要测试 ManualControl 手操路径：

```bash
./build/examples/UavGroundStationApp --target-drone A --auto-manual-control-test
```

预期 smoke-test 标记：

```text
DRONE_STATUS drone=A video streaming
GS_STATUS Video packet stream from /example/uav/drone/A/video/<stream-id>
GS_DECODED_FRAMES count=30
GS_STATUS Video stopped, packets=<stream-packets>, fec_groups=<fec-groups>
DRONE_STATUS drone=A object detection frame=<n> objects=Car
```

## MiniNDN GUI demo

MiniNDN launcher 会把 controller、drone GUI、ground-station GUI 分别放到不同 MiniNDN 节点里运行，
同时把窗口转发到 host 的 X11 session：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py
```

如果要模拟板端/真机部署，让 drone 侧不创建 GTK 窗口，可以加 `--drone-headless`。
controller 和 ground station 仍然照常启动；drone 进程会把 `DRONE_HEADLESS_READY`
和周期性的 readiness/video 状态行写入它的 MiniNDN log：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-video-test --auto-stop-seconds 10 --no-cli
```

可以用 `--video-bitrate-kbps <kbps>` 修改请求的图传 bitrate，用 `--video-width <pixels>` 修改
请求的编码 frame width。ground station 会把这些值通过 NDNSF 控制服务发给 drone；drone 调整
encoder quality 和缩放宽度；ground station 再用接受的 bitrate 计算预取预算。

默认节点分配：

```text
controller: csu
drone:      ucla
ground station: memphis
```

交互模式下，launcher 默认会在和 DroneAPP 相同的 MiniNDN 节点上启动 PX4 SITL 和 jMAVSim
GUI，所以 simulator 窗口会和 drone 窗口一起出现，方便直接观察手操反应。如果只想使用 mock
flight-controller backend，可以加 `--no-start-jmavsim`。
不带额外参数直接运行 launcher 时，会启动两架无人机的交互 demo：Drone A 在 `ucla`，Drone B
在 `wustl`，ground station 左侧会列出两架无人机，方便切换当前控制目标。launcher 还会在
MiniNDN 隔离节点网络命名空间之前，在缺少离线缓存时先在宿主机侧预取 University of Memphis
附近的 OpenStreetMap tile，因此地图区域启动后就能显示真实地图 tile。
DroneAPP 会在 simulator 进程启动后立刻显示出来，不会为了等待 PX4 完全 ready 而阻塞 GUI。
Drone 窗口会通过 launcher 写入的小状态文件显示 `Flight controller: starting`、
`simulator connected`、`ready for takeoff` 等状态。
PX4/jMAVSim 输出会先经过过滤再写入 `jmavsim-<drone>.log`：重复的 `pxh>` prompt 会被丢弃，
日志大小由 `NDNSF_UAV_JMAVSIM_LOG_MAX_BYTES` 限制（默认 8 MiB）。这样可以避免交互 demo
期间终端 prompt 刷屏导致 VM 卡死。
launcher 默认也会启用 DroneAPP 的 PX4 SITL demo 参数设置；如果想保持 PX4 原始 RC/手操
failsafe 参数，可以加 `--no-configure-px4-sitl-demo-params`。
`--enable-ndnsd` 目前只作为 NDNSD 实验入口保留；在完成 NDNSD runtime 兼容性修复前，
launcher 仍然导出 `NDNSF_DISABLE_NDNSD=1`。
如果要在交互式 GUI 里启动多架 drone 并填充 ground-station 的 vehicle list，可以加
`--multi-drone-gui`，例如 `--patrol-drone-ids A,B --patrol-drone-nodes ucla,wustl`。

如果要先做不启动重型模拟器的单无人机 mission-upload smoke test：

```bash
xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-single-mission-test \
  --no-start-jmavsim --no-cli --no-xhost
```

如果要用同一路径测试一台 PX4/jMAVSim，把 `--no-start-jmavsim` 去掉，并加
`--flight-controller-backend udp`。这样只启动一个 DroneAPP 和一个 simulator，然后给目标
drone 下发一个小矩形 mission。
launcher 默认会把 PX4/jMAVSim home 设成和 ground-station 地图一致的 University of Memphis
坐标：`--sim-home-lat 35.1186 --sim-home-lon -89.9375 --sim-home-alt 100`。这点很关键：
jMAVSim 上游默认 home 在 Zurich，如果不改，PX4 会认为 Memphis waypoint 距离 home 数百万米，
于是拒绝 mission；表现就是飞机能起飞，但不会进入上传的 waypoint mission。
已经验证通过的单模拟器命令是：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-single-mission-test \
  --auto-single-mission-start-test \
  --flight-controller-backend udp \
  --start-jmavsim --jmavsim-headless --no-cli
```

成功时脚本会打印 `NDNSF_UAV_SINGLE_MISSION_MININDN_SMOKE_OK`；ground-station log
里应该能看到 `mission_transport=mavlink-mission-upload` 和 `mission_ack=accepted`。
drone log 里应该能看到 `UDP_FC_MISSION_COUNT`，默认矩形航线对应的 4 组
`UDP_FC_MISSION_REQUEST` / `UDP_FC_MISSION_ITEM_SENT`，以及
`UDP_FC_MISSION_ACK ... result=accepted` 和
`UDP_FC_COMMAND_ACK ... command=start_mission result=accepted`。

如果要做不需要人工点击的双无人机协作巡逻 smoke test：

```bash
xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-patrol-test \
  --patrol-drone-ids A,B \
  --patrol-drone-nodes ucla,wustl \
  --no-start-jmavsim --no-cli --no-xhost
```

ground station 会把画出的 waypoint 按巡逻无人机数量聚簇；如果没有画 route，则为每架无人机
生成一个相邻 waypoint sector。然后它通过 `/UAV/Mission/Assign` 分配这些 parts，并在每条
路线末尾追加返回该 drone telemetry 出发位置的 waypoint；如果某个 part 缺失，会发 compensation
request 只补这个缺失 part。
同一个 smoke 还会检查 typed `PATROL_PROGRESS` marker，确认 missing part、compensation、
return-home planning 和最终 completed 状态都被状态模型记录。
Ground-station runtime 还会保存最近的 typed `MissionPlan`；selected drone 的 inspector
和 map summary 会显示它被分配到的 `MissionPart`、waypoint 数量和 return-home 标记。
上传前，GUI 也会用同一个 helper 根据已经画出的 waypoints 生成本地 mission preview。
Preview marker 和 selected-drone inspector 会显示哪架 drone 会收到哪个 part，因此 operator
可以在发送任何 NDNSF mission request 前先检查计划。
交互式 GUI 里也有 `Upload Patrol Mission` 按钮，会运行同一套协作巡逻上传流程。可以用
`+`/`-` 调整缩放，拖动地图查看附近区域，按 `Center GS` 回到 ground station；点击地图可以
连续追加 `WP1`、`WP2` 和后续 waypoint；`Undo WP` 会删除最后一个点，`Clear WPs` 会清空计划。
如果至少有两个地图 waypoint，ground station 会按巡逻无人机数量把这条 route 聚簇，并且
NDNSF request payload 里只发送每架无人机生成后的 waypoint 文本。每条分配路线都会在末尾回到
该 drone 上传任务时的 telemetry 出发位置。如果没有画 route，也可以直接编辑按钮旁边三个输入框：巡逻中心纬度、
中心经度和边长米数，ground station 会按这些轻量输入为每架无人机生成一个相邻的 waypoint
sector。
注意，上传 mission 只是把 waypoint 装进 PX4。现在按 `Start Mission` 后，ground station
会按类似 QGroundControl 的分阶段操作顺序执行：先对所有巡逻无人机发送 arm，再对所有巡逻无人机
发送 takeoff，最后对所有巡逻无人机发送 start mission。同一阶段内仍保留短 stagger，避免
Targeted MAVLink 连续请求被 in-flight 保护丢弃。这个 start sequence 只会使用本轮 mission upload
成功的 drone；如果某架无人机的 mission 上传超时或缺失，会跳过它，避免误启动 PX4 里残留的旧
mission。`Stop Patrol` 会向巡逻无人机发送 `land`。
使用 UDP MAVLink backend 时，DroneAPP 会尝试标准 MAVLink mission upload handshake：
`MISSION_COUNT`、`MISSION_REQUEST(_INT)`、`MISSION_ITEM_INT`、`MISSION_ACK`。
使用 mock backend 时，确定性 smoke 路径仍然报告 command-long waypoint forwarding。
drone response 会返回 `mission_transport`、`mission_ack`、`waypoints_forwarded`、
`waypoint_acks_accepted` 和 `last_waypoint_ack`，方便实测时判断 simulator/flight controller
是否真正接受了 mission。

脚本打印 `NDNSF_UAV_GUI_MININDN_READY` 后，就可以在 ground-station 窗口点击 `Start Video`
和 `Stop Video`。日志会写到 `results/uav_gui_minindn/`。这个命令应该从图形桌面 session
里启动，这样 `DISPLAY` 和 `XAUTHORITY` 可以传给 MiniNDN 节点进程。

如果要做不需要人工点击的自动 smoke test：

```bash
xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --video-bitrate-kbps 8000 \
  --video-width 480 \
  --auto-video-test --auto-stop-seconds 8 --no-cli --no-xhost
```

如果需要让 smoke test 覆盖显式 `Apply Bitrate` Stop-then-Start 路径，在同一命令后加
`--auto-apply-bitrate-test`。
如果需要覆盖 policy-driven 路径，可以加
`--video-bitrate-policy auto-after-pressure --video-bitrate-auto-pressure-ms <ms>`；当 smoke test
必须稳定覆盖自动 Stop-then-Start 路径时，可以把 `<ms>` 设成 `0`。
这个 smoke 也会检查 adaptive log 里有 `primary_pressure` 和 `policy_reason`，避免以后只看到
数字窗口变化却不知道策略为什么这样判定。
如果还需要确定性覆盖不同压力来源，可以加 `--auto-video-pressure-profile-test`。它会在视频
启动后注入 congestion、backlog 和 probe 三类受控样本，并要求 GS 记录
`auto-video-pressure-congestion`、`auto-video-pressure-backlog` 和
`auto-video-pressure-probe` view state，避免依赖随机丢包来验证策略解释。

smoke test 会在确认 ground station 解码到视频 frame，且 drone 进入并退出 streaming 状态后退出。
在集成 runtime 中，ground station 同时提供 `/UAV/GS/ObjectDetection`；直播期间 drone 会周期性
调用它，并在日志和窗口中提示 `Car`/`Truck` 检测结果。默认 detector command 使用
长驻 `tools/yolo_detect_worker.py` 和 `yolo26n.pt`，因此模型只加载一次，后续低频请求复用同一个
worker。部署其它本地模型或 worker 时可以用 `--yolo-model` 和 `--yolo-worker-script` 覆盖。
`--yolo-script` 仍保留为 one-shot fallback helper。CPU 推理刻意保持低频，目前约 1 Hz。

如果要做不需要人工点击的 Targeted MAVLink smoke test：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-mavlink-test --no-cli
```

这个测试会检查 ground station 收到 `arm`、`takeoff` 和 `land` 的 Targeted response，并检查
drone 是否把 opaque MAVLink bytes 转发给 mock flight-controller backend。

如果要测试同一套 GUI 键盘快捷键路径，把 `--auto-mavlink-test` 换成 `--auto-keyboard-test`。

如果要在 smoke test 里包含 ManualControl 按键保持：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-manual-control-test --no-cli
```

如果要显式把 PX4 SITL + jMAVSim 运行在和 DroneAPP 相同的 MiniNDN 节点，并把 MAVLink
command 转发到 PX4 的 GCS MAVLink UDP 端口：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --start-jmavsim \
  --flight-controller-backend udp \
  --mavlink-udp-port 18570
```

Drone app 也会绑定本地 MAVLink GCS 端口，默认是 `14550`，用于接收 PX4 侧的 command ACK 和
telemetry。如果这个端口已经被其它 GCS 进程占用，可以用 `--mavlink-udp-listen-port` 覆盖。
在交互式 MiniNDN 模式下，关闭 ground-station GUI 会让 launcher 自动退出并清理 PX4/jMAVSim，
避免模拟器进程在后台继续占用 CPU。

如果要做不需要人工操作的 PX4/jMAVSim smoke test：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --start-jmavsim --jmavsim-headless \
  --auto-manual-control-test --no-cli
```

如果要回归测试 PX4/jMAVSim 的 live telemetry 字段和状态变化：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --start-jmavsim --jmavsim-headless \
  --flight-controller-backend udp \
  --auto-telemetry-test --no-cli
```

这个测试会在 GS 通过 NDNSF Targeted request 执行 arm/takeoff/land 时，检查
`gps_fix_name`、`ekf_ready`、`landed_state_name`、`battery_voltage_v`、
`armed` 和 `lat/lon`。

如果只想做不启动 PX4/jMAVSim 的 MiniNDN 回归测试：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-telemetry-test --no-start-jmavsim \
  --no-cli --no-xhost
```

这种情况下脚本会自动启用 mock-field telemetry 模式。它仍然验证
NDNSF telemetry 请求路径、类型化状态更新、arm/takeoff/land 命令状态变化、
landed-state 变化、`ekf_ready`、`armed` 和 `lat/lon`，但不会因为没有真实
GPS fix 或 battery voltage 字段而失败。上面的 PX4/jMAVSim 命令仍然是真实
飞控传感器字段的严格检查。

如果要在没有真机硬件的情况下回归测试 GS 本地 stale/lost link 模型：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-link-state-test --link-stale-ms 600 \
  --link-lost-ms 1400 --no-cli
```

这个测试先获取一次 telemetry，然后停止刷新并等待，确认 GS safety model 会从
fresh/connected 变成 `stale`，再变成 `lost`。

如果要回归测试视频 Start/Stop 控件是否跟随当前选中的 drone，而不是错误地跟随全局 stream
flag：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-video-selection-test --no-cli
```

launcher 会启动 Drone A 和 Drone B，只开启 Drone A 的图传，然后让 GS 切换到 Drone B
再切回 Drone A，并检查 typed video state 是否正确驱动当前选中无人机的按钮模型。

如果要回归测试 mission Upload/Start/Stop 控件是否由 typed `MissionState` 驱动，而不是依赖
临时 status string：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-mission-controls-test --no-cli
```

launcher 会使用两架 mock drone 环境，并在 GS smoke 路径中注入 uploaded
`MissionState`。它会先确认 not-ready flight safety gate 会阻止 uploaded mission start，并明确显示
`start_reason=blocked-...waiting-heartbeat`；然后注入 ready/unarmed `ReadinessState`，再检查 GS 的
mission control model 是否变成 `can_start=true` / `start_reason=ok` / `can_stop=true`，不依赖飞控
waypoint upload 的实际行为。同一个 smoke 还会检查本地 `start-pending` 和 `stop-pending`
action gate，确保 Mission Start/Stop 按钮状态和 tooltip 在长命令序列执行期间仍然来自 typed
model。周期性 telemetry 里携带的空 `idle` mission snapshot 如果比 GS 已知的非 idle mission
state 更弱，不会再覆盖已上传或正在执行的任务状态。

如果要回归测试 Arm/Takeoff/Land/手操按钮是否由 typed `ReadinessState` 驱动：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-flight-controls-test --no-cli
```

launcher 会在 GS smoke 路径中注入 not-ready、ready-but-not-armed 和
armed-ready 三种 readiness snapshot。它检查 Arm 只在选中无人机 ready 且未 armed 时启用，
Takeoff/Land/手操只在选中无人机 armed 后启用。这些 UI gate 和实际 MAVLink command-send
路径共用同一个 typed `FlightSafetyGateState`，把 readiness 和 safety/link state 合在一起判断。
同一个 smoke 也会记录 selected-drone action model，包括手操模式、Emergency Stop 可用性，以及
mission Start/Stop readiness。它也会检查 selected drone view model，确认 inspector/map 文本、
地图 marker 状态以及 `can_arm`、`can_takeoff`、`can_manual` 这类 flight gate 字段都由 typed
state 派生；例如 mission upload 后用 typed marker suffix 表示，而不是解析临时 status string。
左侧 drone list 也通过同一套 typed state 路径检查，包括 readiness、mission、video 和 safety 摘要。

对于两架无人机的 jMAVSim 路径，launcher 不会把单实例
`make px4_sitl jmavsim` target 启动两次，而是显式启动
`px4 -i 0` 和 `px4 -i 1`。Drone A 使用 PX4 MAVLink UDP 端口
`18570` 和 simulator TCP 端口 `4560`；Drone B 使用 `18571` 和
`4561`。

如果要 smoke-test 在两架无人机之间切换手动控制：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-two-drone-switch-test --multi-drone-gui \
  --flight-controller-backend udp \
  --start-jmavsim --jmavsim-headless --no-cli
```

如果要 smoke-test 两架 PX4/jMAVSim 共同执行巡逻 mission upload：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-patrol-test --multi-drone-gui \
  --flight-controller-backend udp \
  --start-jmavsim --jmavsim-headless --no-cli
```

launcher 会把 MiniNDN 节点 HOME 放在 `/tmp/minindn/<node>` 下，因此它会保留当前 Python
package path，供 PX4 build helper（例如 `kconfiglib`）使用。它也默认给较新的 CMake 版本传入
`CMAKE_ARGS=-DCMAKE_POLICY_VERSION_MINIMUM=3.5`；如果你的 PX4 checkout 已经不需要这个参数，
可以用 `--px4-cmake-args` 覆盖。

launcher 默认会压低 framework 日志量：`ndn_service_framework.*=WARN`，UAV app 自身日志保持
`INFO`。只有调试 NDNSF 内部问题时才建议用 `NDNSF_APP_NDN_LOG` 覆盖。

## 完整 NDNSF service 草图

```bash
nfd-start
nfdc strategy set /example/uav/group /localhost/nfd/strategy/multicast

./build/examples/App_ServiceController \
  --controller-prefix /example/uav/controller \
  --policy-file NDNSF-UAV-APP/configs/uav_demo.policies

./build/examples/UavGroundStationApp --serve-object-detection
./build/examples/UavDroneApp --drone-id A
./build/examples/UavDroneApp --drone-id B
./build/examples/UavGroundStationApp --target-drone A
```

如果要模拟某架 drone 不可用：

```bash
./build/examples/UavDroneApp --drone-id A --unavailable
```

这架 drone 会 suppress successful ACK，因此 NDNSF provider selection 可以选择另一个提供相同服务的 drone。

## 后续版本

1. 把当前 OpenStreetMap tile + marker overlay 升级成完整 map widget：支持缓存/离线 tile、
   点击添加 waypoint，以及更完整的多无人机 marker 图层。
2. 把当前 waypoint-sector patrol demo 扩展成完整 mission editor：polygon 绘制、自动分区、
   对缺失 part 发 compensation request、按电量重新分配，以及 coverage report。
3. 继续加固 MAVLink UDP mission upload 路径：清理/替换旧 mission、mission-current 行为、
   retry window、mission progress telemetry，以及 operator 可读的失败原因。
4. 针对有损无线链路调优 H264/H265 GOP、bitrate、chunk size、FEC 和 keyframe recovery，
   同时保留即时播放语义。
5. 通过 `NDNSF-DistributedRepo` 存储任务图像、telemetry log 和 report，数据名使用
   publisher-owned namespace，并存储签名后的 segments。
7. 把部分图像/object-detection workflow 接到 `NDNSF-DistributedInference`，用于 ground station
   和 drones 之间的分割模型执行。
