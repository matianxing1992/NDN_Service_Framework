# NDNSF-UAV-APP

`NDNSF-UAV-APP` is a C++ reference UAV network application built on NDNSF. It is
kept beside `NDNSF-DistributedRepo` and `NDNSF-DistributedInference` because it
is an application layer above the generic NDNSF runtime, not a low-level core
example.

## Why This Application Exists

Developing UAV network applications directly over IP often forces application
code to solve networking problems that are not part of mission logic: address
binding for moving drones, NAT and mobility, multi-drone service discovery,
operator authorization, data authenticity, wireless-loss recovery, and task
reassignment when a drone is busy or disconnected.

This app shows how NDNSF can express those requirements as named services,
signed/encrypted data, permission-controlled service invocation, and provider
selection.

## First Version Scope

The first version demonstrates the main UAV workflow:

- The ground station builds MAVLink bytes for `arm`, `takeoff`, `land`, and
  mission commands.
- The drone receives NDNSF requests, verifies the NDNSF security path, and
  forwards opaque MAVLink bytes to either a mock backend or a UDP MAVLink
  flight-controller backend.
- The drone exposes telemetry and camera-frame services.
- The ground station starts/stops video streaming through a provider-specific
  control service named under the target drone; the drone publishes frames
  under its own namespace, and the ground station fetches frames by name.
- The ground station assigns a patrol/inspection mission with role-specific
  waypoints.
- The drone returns a mock image capture result and mock detection summary.
- The ground station can provide `/UAV/GS/ObjectDetection` for future
  drone-to-ground-station object detection requests.
- Multiple drones can advertise the same services; if one drone suppresses ACKs
  or is unavailable, NDNSF provider selection can choose another drone.

The drone does not interpret MAVLink command semantics. The ground station owns
MAVLink message construction. The drone app treats MAVLink as opaque bytes and
passes them to the flight-controller backend.

MAVLink execution uses NDNSF Targeted invocation. The first command to a drone
bootstraps through the normal authenticated request/ACK/selection/response
flow and obtains one-time token pairs. Later `arm`, `takeoff`, and `land`
commands, as well as low-rate `MANUAL_CONTROL` updates from the keyboard, use
request/response-only Targeted calls to `/UAV/MAVLink/Execute`, reducing command
latency while still validating the provider and rejecting token replay.

## Current Binaries

```text
UavDroneApp
  Drone-side provider for MAVLink execution, telemetry, camera frames, and
  mission assignment.

UavGroundStationApp
  Ground-station user for video, MAVLink command, and patrol workflows. With
  --serve-object-detection, it becomes a ground-station object detection
  provider.
```

## Services

```text
/UAV/MAVLink/Execute
/UAV/Mission/Assign
/UAV/Telemetry/GetStatus
/UAV/Camera/GetFrame
/example/uav/drone/A/UAV/Camera/Video
/UAV/GS/ObjectDetection
```

Shared drone services can be provided by multiple drones and selected by NDNSF.
Provider-specific services are named as `/<provider>/<serviceName>` so they are
globally unique. Video control is provider-specific because the operator is
starting or stopping one physical drone camera. Start and stop are both carried
under `/example/uav/drone/A/UAV/Camera/Video`; the control action distinguishes
start from stop. `/UAV/GS/ObjectDetection` is provided by the ground station for
heavier compute on uploaded frames.

## Patrol Task Compensation

Patrol missions are modeled as application-level tasks that may require more
than one NDNSF service invocation to complete. NDNSF still sees each invocation
as an independent request with a fresh `requestId`, `UserToken`, and
`ProviderToken`; the ground-station application links them with a shared
`patrol_task_id`.

The ground station keeps a small patrol task ledger:

```text
patrol_task_id
attempt_id
part_id
assigned_provider
state: pending / done / missing / compensated
response_digest
deadline_ms
```

The first attempt assigns all parts of a task by choosing candidate providers
for each part. If the deadline expires and one or more parts have no valid
response, the ground station sends one or more compensation requests containing
only the missing parts. A compensation request does not restart the whole
mission; it is another NDNSF invocation that belongs to the same
application-level patrol task.

The mission service remains a shared service name, `/UAV/Mission/Assign`.
Task assignment is not encoded as a provider name inside the payload. Instead,
providers use their selective ACK handler to report whether they currently have
a mission slot available. A busy drone still publishes an ACK, but the ACK has
`status=false` and metadata such as `mission_busy=true` and `queue=1`. Idle
drones publish `status=true`, allowing normal NDNSF provider selection to pick
an available drone.

Example:

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
  done when part0 and part1 both have valid responses
```

This keeps NDNSF generic. The framework handles secure request, ACK,
selection, response, provider tokens, replay protection, and timeout behavior.
The UAV application owns patrol-specific semantics such as part status,
deadlines, compensation, and final task completion.

## Video Streaming Design

Video streaming is not modeled as a long service response. Control and data use
different paths:

```text
Ground station -> /example/uav/drone/A/UAV/Camera/Video/start/<nonce>
  Start the camera stream.

Drone -> drone-owned frame Data names
  Publishes signed video packets:
    /example/uav/drone/A/video/<stream-start-ms>/<packetSeq>

Ground station -> frame Data names
  Prefetches monotonically increasing packetSeq values. Data content carries
  frame metadata, so names stay sequential even when frame sizes vary.

Ground station -> /example/uav/drone/A/UAV/Camera/Video/stop/<nonce>
  Drone stops capturing and serving new frames.
```

The current GUI video control path uses NDNSF generic service invocation. The
full provider-specific name `/example/uav/drone/A/UAV/Camera/Video` is the
service name; the provider prefix is part of the service name to make this
camera-control service globally unique. High-rate video packets are still
fetched as signed NDN Data under the drone namespace, so the generic
request/response path carries control only, not the video byte stream.

The current implementation loops `NDNSF-UAV-APP/videos/drone.mp4` through
`ffmpeg`, encodes low-latency MJPEG frames, and splits each frame into NDN-sized
video packets. This is not ordinary NDN segmentation of one object. Each Data
packet is an independent video packet named only by the stream start timestamp
and a monotonically increasing `packetSeq`; it never mixes bytes from two
different frames. The Data content begins with a compact metadata header
containing `frame_seq`, `frame_segment_index`, `frame_segment_count`,
`frame_first_packet_seq`, `frame_last_packet_seq`, `bucket_packet_count`,
`capture_ms`, and `key_frame`, followed by the frame bytes for that packet.

In the current phase, each frame is protected by a lightweight forward-error
correction code: `N = K + 1` (one parity shard by default). The sender publishes
all data shards plus one parity shard with the same packet naming pattern and
per-packet metadata (`fec_data_shards`, `fec_parity_shards`, `fec_symbol_index`,
`fec_symbol_count`, and `fec_data_lengths`). The ground station buffers all
received shards of a frame, and when at most one data shard is missing, it
recovers it by XOR before feeding the decoder. This keeps packet naming fully
predictable while still tolerating one loss or reordering hole.

This naming keeps prefetch simple: the ground station can fetch
`/<stream>/0`, `/1`, `/2`, ... without handling second boundaries. Reassembly is
driven by the per-packet metadata, not by NDN FinalBlockId. Once all packets for
a frame arrive, the GUI displays that frame. Missing or late old non-key-frame
packets are skipped so later frames can remain live. A future H264/H265
implementation can use the same name layout, waiting for key frames when needed
and dropping stale delta frames until the next usable key frame.

The ground station includes requested video bitrate and frame width in the same
`/<drone>/UAV/Camera/Video` control service request. The drone uses the bitrate
to choose its MJPEG encoder quality (`ffmpeg -q:v`) and clamps both bitrate and
width to its supported range. The drone then returns the requested bitrate,
accepted bitrate, requested width, accepted width, FPS, encoder quality, and
packet payload size. The ground station derives its prefetch window from those
returned values and from the packet high-watermark carried in each packet.
The default is currently 8000 kbps, 480 px frame width, and 30 FPS for the demo
MJPEG stream. Raising bitrate improves JPEG quality and packet volume; raising
frame width makes the displayed video larger.

When `Stop Video` is invoked, the drone stops the encoder loop, clears pending
Interests and cached stream packets, and ignores late frame Interests for the
stopped stream. This prevents the GUI from stopping while the drone continues
serving old cached packets.

A later version can add a small per-stream SVS group for frame-name
announcements, but it should remain separate from the main UAV control group so
high-rate video signaling does not disturb command/telemetry/mission traffic.
The same control service can also carry future H265 or tuned GOP parameters.

The frame data should be published under the drone's namespace, signed by the
drone, and optionally stored through `NDNSF-DistributedRepo` for replay or
post-mission analysis.

## GUI Plan

The current first GUI uses gtkmm because it is already available in this
repository's build configuration. wxWidgets remains a good future target if we
want a dedicated native GUI toolkit across platforms.

The GUI should be a ground-station frontend over the same service layer:

- mission map and role assignment panel;
- live telemetry table;
- video preview panel fed by the frame-name/prefetch pipeline;
- keyboard/gamepad control mode;
- object-detection result overlay;
- drone availability and selected-provider view.

The command path should remain identical: GUI actions build MAVLink bytes at the
ground station, then send opaque MAVLink frames through NDNSF Targeted service
invocation.

## Build

From the repository root:

```bash
./waf configure
./waf build --targets=UavDroneApp,UavGroundStationApp
```

## Two-Window Video Demo

`UavDroneApp` and `UavGroundStationApp` start their NDNSF runtime before the GUI
window is shown. This keeps the NDNSF/NAC-ABE construction, SVS setup, and
permission fetch path close to the normal command-line examples, while the GUI
main loop only starts after the runtime reports ready.

Start NFD and the UAV controller once, then start one drone window and one
ground-station window:

```bash
nfd-start

./build/examples/App_ServiceController \
  --controller-prefix /example/uav/controller \
  --policy-file NDNSF-UAV-APP/configs/uav_demo.policies

./build/examples/UavDroneApp --drone-id A --video-source NDNSF-UAV-APP/videos/drone.mp4
./build/examples/UavGroundStationApp --target-drone A \
  --video-bitrate-kbps 8000 --video-width 480
```

Click `Arm`, `Takeoff`, or `Land` in the ground-station window to send Targeted
MAVLink commands to the drone. For keyboard operation, click `Start Control`.
The GUI displays manual-control keycaps:

```text
W forward        S back
A yaw left       D yaw right
Q roll left      E roll right
R throttle up    F throttle down
I arm            T takeoff
L land           V video start
X video stop
```

While a key is held, its keycap turns black so the operator can see which
command is active. Holding a manual key sends MAVLink `MANUAL_CONTROL` frames at
a low fixed rate; releasing the key sends a neutral update.
The drone window should switch to `video streaming`, and the ground-station
window should display the live video packet stream. Click `Stop Video` or press
`X` in control mode to stop the stream; the drone window should switch back to
`Video stopped`.

For an automated GUI smoke test without manual button clicks:

```bash
./build/examples/UavDroneApp --drone-id A --video-source NDNSF-UAV-APP/videos/drone.mp4
./build/examples/UavGroundStationApp --target-drone A \
  --video-bitrate-kbps 8000 --video-width 480 \
  --auto-video-test --auto-stop-seconds 10
```

For a Targeted MAVLink command smoke test:

```bash
./build/examples/UavGroundStationApp --target-drone A --auto-mavlink-test
```

For a keyboard-shortcut smoke test that exercises the same `a/t/l` handlers used
by the GUI:

```bash
./build/examples/UavGroundStationApp --target-drone A --auto-keyboard-test
```

For a ManualControl smoke test that holds manual keys and emits MAVLink
`MANUAL_CONTROL`:

```bash
./build/examples/UavGroundStationApp --target-drone A --auto-manual-control-test
```

Expected smoke-test markers:

```text
DRONE_STATUS drone=A video streaming
GS_STATUS Video packet stream from /example/uav/drone/A/video/<stream-id>
GS_DECODED_FRAMES count=30
GS_STATUS Video stopped, frames=<frames>
```

## MiniNDN GUI Demo

The MiniNDN launcher runs the controller, drone GUI, and ground-station GUI on
separate MiniNDN nodes while forwarding the windows to the host X11 session:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py
```

Use `--video-bitrate-kbps <kbps>` to change the requested stream bitrate, and
`--video-width <pixels>` to change the requested encoded frame width. The ground
station forwards those values through NDNSF, the drone adjusts its encoder
quality and scaling, and the ground station sizes the prefetch budget from the
accepted bitrate.

Default placement:

```text
controller: csu
drone:      ucla
ground station: memphis
```

After the script prints `NDNSF_UAV_GUI_MININDN_READY`, use the ground-station
window to click `Start Video` and `Stop Video`. Logs are written under
`results/uav_gui_minindn/`. The command should be launched from a graphical
session so `DISPLAY` and `XAUTHORITY` are available to the MiniNDN node
processes.

For a non-interactive smoke test that starts and stops video automatically:

```bash
xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --video-bitrate-kbps 8000 \
  --video-width 480 \
  --auto-video-test --auto-stop-seconds 8 --no-cli --no-xhost
```

The smoke test exits after checking that the ground station decoded video
frames and that the drone entered and left streaming mode.

For a non-interactive Targeted MAVLink smoke test:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-mavlink-test --no-cli
```

The test checks that the ground station receives Targeted responses for
`arm`, `takeoff`, and `land`, and that the drone forwards opaque MAVLink bytes
to the mock flight-controller backend.

To run the same flow through the GUI keyboard shortcut path, replace
`--auto-mavlink-test` with `--auto-keyboard-test`.

To include ManualControl key holds in the smoke test:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-manual-control-test --no-cli
```

To run PX4 SITL with jMAVSim on the same MiniNDN node as the drone and forward
commands to PX4's GCS MAVLink UDP port:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --start-jmavsim \
  --flight-controller-backend udp \
  --mavlink-udp-port 18570
```

For a non-interactive PX4/jMAVSim smoke test:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --start-jmavsim --jmavsim-headless \
  --auto-manual-control-test --no-cli
```

The launcher keeps MiniNDN node homes under `/tmp/minindn/<node>`, so it
preserves the current Python package path for PX4 build helpers such as
`kconfiglib`. It also passes `CMAKE_ARGS=-DCMAKE_POLICY_VERSION_MINIMUM=3.5` by
default for newer CMake versions; override this with `--px4-cmake-args` if your
PX4 checkout no longer needs it.

## Full NDNSF Service Sketch

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

To simulate an unavailable drone:

```bash
./build/examples/UavDroneApp --drone-id A --unavailable
```

That drone suppresses successful ACKs, so NDNSF provider selection can choose
another drone that provides the same service.

## Future Versions

1. Add a real MAVLink UDP backend for ArduPilot SITL / PX4 SITL while keeping
   `MockFlightControllerBackend` for deterministic tests.
2. Add keyboard and gamepad control modes at the ground station. These modes
   still build MAVLink bytes at the ground station and send opaque frames
   through NDNSF.
3. Add an optional per-stream small SVS group for frame-name announcements and
   wider prefetch windows.
4. Tune H264/H265 GOP, bitrate, chunk sizing, and keyframe recovery for lossy
   wireless links while keeping immediate playback semantics.
5. Add a wxWidgets or refined gtkmm ground-station GUI for mission planning, telemetry, video
   preview, keyboard/gamepad control, and detection overlays.
6. Let drones call `/UAV/GS/ObjectDetection` with captured frames and consume
   detection results during mission execution.
7. Add multi-drone patrol planning with dynamic task reassignment,
   battery-aware role selection, and coverage reports.
8. Store mission images, telemetry logs, and reports through
   `NDNSF-DistributedRepo`, using publisher-owned data names and signed
   segments.
9. Connect selected image/object-detection workflows to
   `NDNSF-DistributedInference` when model execution is split across ground
   stations and drones.
