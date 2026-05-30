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

- ground station 生成 `arm`、`takeoff` 和 mission command 的 MAVLink bytes。
- drone 收到 NDNSF request，完成 NDNSF 安全路径验证，然后把 opaque MAVLink bytes 转发给
  `MockFlightControllerBackend`。
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

## 当前二进制

```text
UavDroneApp
  Drone-side provider，提供 MAVLink execution、telemetry、camera frame 和 mission assignment。

UavGroundStationApp
  Ground-station user，执行 patrol workflow。加上 --serve-object-detection 后，它会变成
  ground-station object detection provider。
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

当前实现把 `/home/tianxing/NDN/drone.mp4` 交给 `ffmpeg` 循环读取，编码成低延迟 MJPEG frame，
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

命令路径保持不变：GUI action 在 ground station 端构造 MAVLink bytes，然后通过 NDNSF 发送
opaque MAVLink frame。

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

./build/examples/UavDroneApp --drone-id A --video-source /home/tianxing/NDN/drone.mp4
./build/examples/UavGroundStationApp --target-drone A \
  --video-bitrate-kbps 8000 --video-width 480
```

在 ground-station 窗口点击 `Start Video`。drone 窗口应该显示正在图传，ground-station 窗口应该能
播放收到的 live video packet stream。点击 `Stop Video` 后，drone 窗口应该回到 `Video stopped`。

如果要不手动点击按钮做 GUI smoke test：

```bash
./build/examples/UavDroneApp --drone-id A --video-source /home/tianxing/NDN/drone.mp4
./build/examples/UavGroundStationApp --target-drone A \
  --video-bitrate-kbps 8000 --video-width 480 \
  --auto-video-test --auto-stop-seconds 10
```

预期 smoke-test 标记：

```text
DRONE_STATUS drone=A video streaming
GS_STATUS Video packet stream from /example/uav/drone/A/video/<stream-id>
GS_DECODED_FRAMES count=30
GS_STATUS Video stopped, frames=<frames>
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
