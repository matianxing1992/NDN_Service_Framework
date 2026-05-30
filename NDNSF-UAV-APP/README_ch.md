# NDNSF-UAV-APP

`NDNSF-UAV-APP` 是一个基于 NDNSF 的 C++ UAV network application 参考实现。它放在
`NDNSF-DistributedRepo` 和 `NDNSF-DistributedInference` 同级，因为它是建立在通用
NDNSF runtime 之上的应用层，而不是低层 core example。

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
- ground station 下发带 role-specific waypoints 的巡逻/巡检任务。
- drone 返回 mock 拍照结果和 mock detection summary。
- ground station 可以提供 `/UAV/GS/ObjectDetection`，供未来 drone 上传图像并请求
  ground-station compute。
- 多个 drone 可以提供相同服务；如果某个 drone suppress ACK 或不可用，NDNSF provider selection
  可以选择另一个 drone。

drone 不解释 MAVLink 命令语义。MAVLink message 由 ground station 构造。drone app 只把
MAVLink 当作 opaque bytes，并交给 flight-controller backend。

MAVLink execution 使用 NDNSF Targeted invocation。发给某架 drone 的第一条命令先走正常的
request/ACK/selection/response 认证流程，并拿到一批一次性 token pair。后续 `arm`、`takeoff`
和 `land` 命令，以及键盘手操产生的低频 `MANUAL_CONTROL` 更新，都会使用 request/response-only
Targeted 调用 `/UAV/MAVLink/Execute`，减少命令延迟，同时仍然验证 provider 并拒绝 token replay。

## 当前二进制

```text
UavDroneApp
  Drone-side provider，提供 MAVLink execution、telemetry、camera frame 和 mission assignment。

UavGroundStationApp
  Ground-station user，执行 video、MAVLink command 和 patrol workflow。加上
  --serve-object-detection 后，它会变成 ground-station object detection provider。
```

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
start 和 stop。`/UAV/GS/ObjectDetection` 由 ground station 提供，用于对上传 frame 执行更重的计算。

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
状态、deadline、补偿请求和最终任务完成判断。

## 图传设计

图传不应该建模成长 service response。控制和数据路径应该分开：

```text
Ground station -> /example/uav/drone/A/UAV/Camera/Video/start/<nonce>
  开启摄像头图传。

Drone -> drone-owned frame Data name
  发布签名后的视频 packet：
    /example/uav/drone/A/video/<stream-start-ms>/<packetSeq>

Ground station -> frame Data names
  连续预取递增 packetSeq。Data content 携带 frame metadata，所以即使每帧大小不同，
  名字仍然可以保持顺序。

Ground station -> /example/uav/drone/A/UAV/Camera/Video/stop/<nonce>
  Drone 停止采集，并停止服务新 frame。
```

当前 GUI 图传控制路径使用 NDNSF generic service invocation。完整的 provider-specific name
`/example/uav/drone/A/UAV/Camera/Video` 就是 service name；provider prefix 是 service name 的一部分，
用于让这个摄像头控制服务全局唯一。高频 video packets 仍然作为 drone namespace 下的签名
NDN Data 拉取，因此 generic request/response 路径只承载控制消息，不承载视频字节流。

当前实现把 `NDNSF-UAV-APP/videos/drone.mp4` 交给 `ffmpeg` 循环读取，编码成低延迟 MJPEG frame，
然后把每个 frame 切成符合 NDN packet 大小的 video packets。这不是普通 NDN segmentation，
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

ground station 会在同一个 `/<drone>/UAV/Camera/Video` 控制服务请求里带上目标图传 bitrate
和 frame width。drone 会先把请求值 clamp 到自己支持的 bitrate/width 范围，再据此选择
MJPEG encoder quality，也就是 `ffmpeg -q:v`，并设置编码缩放宽度。响应里会同时返回 requested
bitrate、accepted bitrate、requested width、accepted width、FPS、encoder quality 和 packet
payload 大小。ground station 再用 accepted bitrate 以及每个 packet metadata 里的
high-watermark 估计 prefetch window，避免盲目请求无边界的 packet sequence number。当前 demo
默认是 8000 kbps、480 px frame width、30 FPS 的 MJPEG stream。提高 bitrate 会增加 JPEG 质量和
packet 数据量；提高 frame width 才会让 GUI 里显示的视频更大。

点击 `Stop Video` 后，drone 会停止 encoder loop，清空 pending Interests 和缓存的 stream packets，
并忽略已经停止的 stream 上迟到的 frame Interests。这样可以避免 GS 端视频停了，但 drone 还在继续
服务旧缓存包。

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

命令路径保持不变：GUI action 在 ground station 端构造 MAVLink bytes，然后通过 NDNSF Targeted
service invocation 发送 opaque MAVLink frame。

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

先启动 NFD 和 UAV controller，然后启动一个 drone 窗口和一个 ground-station 窗口：

```bash
nfd-start

./build/examples/App_ServiceController \
  --controller-prefix /example/uav/controller \
  --policy-file NDNSF-UAV-APP/configs/uav_demo.policies

./build/examples/UavDroneApp --drone-id A --video-source NDNSF-UAV-APP/videos/drone.mp4
./build/examples/UavGroundStationApp --target-drone A \
  --video-bitrate-kbps 8000 --video-width 480
```

在 ground-station 窗口点击 `Arm`、`Takeoff` 或 `Land`，可以通过 Targeted MAVLink command
控制目标 drone。如果要用键盘操作，先点击 `Start Control`。GUI 会显示手操 keycap：

```text
W forward        S back
A yaw left       D yaw right
Q roll left      E roll right
R throttle up    F throttle down
I arm            T takeoff
L land           V video start
X video stop
```

按住某个键时，对应 keycap 会变成黑色，让 operator 明确知道当前按下的是哪个控制键。按住手操键时，
GS 会以较低固定频率发送 MAVLink `MANUAL_CONTROL`；松开后会发送 neutral update。点击 `Start Video`
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
```

## MiniNDN GUI demo

MiniNDN launcher 会把 controller、drone GUI、ground-station GUI 分别放到不同 MiniNDN 节点里运行，
同时把窗口转发到 host 的 X11 session：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py
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
DroneAPP 会在 simulator 进程启动后立刻显示出来，不会为了等待 PX4 完全 ready 而阻塞 GUI。
Drone 窗口会通过 launcher 写入的小状态文件显示 `Flight controller: starting`、
`simulator connected`、`ready for takeoff` 等状态。
PX4/jMAVSim 输出会先经过过滤再写入 `jmavsim-<drone>.log`：重复的 `pxh>` prompt 会被丢弃，
日志大小由 `NDNSF_UAV_JMAVSIM_LOG_MAX_BYTES` 限制（默认 8 MiB）。这样可以避免交互 demo
期间终端 prompt 刷屏导致 VM 卡死。

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

smoke test 会在确认 ground station 解码到视频 frame，且 drone 进入并退出 streaming 状态后退出。

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

如果要做不需要人工操作的 PX4/jMAVSim smoke test：

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --start-jmavsim --jmavsim-headless \
  --auto-manual-control-test --no-cli
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

1. 增加真实 MAVLink UDP backend，用于 ArduPilot SITL / PX4 SITL，同时保留
   `MockFlightControllerBackend` 便于确定性测试。
2. 在 ground station 增加键盘和手柄控制模式。这些模式仍然在 ground station 端构造 MAVLink
   bytes，再通过 NDNSF 发送 opaque frame。
3. 增加可选的 per-stream 小 SVS group，用于 frame-name announcement 和更宽的 prefetch window。
4. 针对有损无线链路调优 H264/H265 GOP、bitrate、chunk size 和 keyframe recovery，同时保留即时播放语义。
5. 增加 wxWidgets 或更完善的 gtkmm ground-station GUI，用于 mission planning、telemetry、video preview、
   keyboard/gamepad control 和 detection overlay。
6. 让 drones 使用拍摄 frame 请求 `/UAV/GS/ObjectDetection`，并在 mission execution 中消费检测结果。
7. 增加多无人机 patrol planning，包括动态任务重新分配、battery-aware role selection 和 coverage report。
8. 通过 `NDNSF-DistributedRepo` 存储任务图像、telemetry log 和 report，数据名使用
   publisher-owned namespace，并存储签名后的 segments。
9. 把部分图像/object-detection workflow 接到 `NDNSF-DistributedInference`，用于 ground station
   和 drones 之间的分割模型执行。
