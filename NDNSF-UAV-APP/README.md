# NDNSF-UAV-APP

`NDNSF-UAV-APP` is a C++ UAV Service Container Application built on NDNSF. It is
kept beside `NDNSF-DistributedRepo` and `NDNSF-DistributedInference` because it
is an application layer above the generic NDNSF runtime, not a low-level core
example.

The key idea is that one process can host multiple NDNSF service instances and
client-side workflows. `UavDroneApp` is a drone-side container for services such
as MAVLink execution, video control, telemetry, camera frames, and mission
assignment. `UavGroundStationApp` is a ground-station container for control
clients, video playback, mission coordination, telemetry display, and services
that drones may call back into, such as object detection. The container process
owns deployment concerns such as identity, trust schema, policy fetch, GUI, and
local hardware adapters, while each named NDNSF service remains independently
addressable and permission-controlled.

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
- The ground station assigns one patrol/inspection mission to multiple drones
  by splitting the area into role-specific waypoint sectors.
- The drone returns a mock image capture result and can ask the ground station
  to run real low-rate object detection on the latest live video frame already
  decoded at the ground station.
- The ground station provides `/UAV/GS/ObjectDetection`; while live video is
  enabled, the drone periodically asks this service whether its camera stream
  contains `Car` or `Truck` and displays an alert in the Drone window. The
  request payload is metadata only; image bytes are not stuffed into service
  requests.
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

## Service Containers

```text
UavDroneApp
  Drone-side service container. It hosts MAVLink execution, telemetry, camera
  frame, video-control, and mission-assignment service instances, plus local
  adapters for the camera/video source and flight controller.

UavGroundStationApp
  Ground-station service container. It hosts NDNSF users for MAVLink command,
  video, telemetry, and patrol workflows, plus the built-in
  /UAV/GS/ObjectDetection provider that drones can call. The standalone
  --serve-object-detection mode remains available for focused service tests.
```

This container model is intentional: real UAV deployments need several related
services to share the same identity, local GUI state, hardware adapters, and
process lifecycle without merging their service names or permissions into one
monolithic RPC endpoint.

## Physical Deployment On One PC And Multiple Drones

This section is for a private deployment where one PC acts as the ground station
and controller, and each physical drone runs one DroneAPP instance. It does not
require connecting to the public NDN Testbed.

Recommended placement:

```text
PC / ground station:
  NFD
  App_ServiceController
  UavGroundStationApp
  optional /UAV/GS/ObjectDetection provider

Each drone:
  NFD
  UavDroneApp
  MAVLink connection to the flight controller
  camera or video source
```

For release deployments, build a portable Ubuntu 20.04 tarball on an Ubuntu
20.04 build host:

```bash
./waf build
packaging/uav-release/create-portable-release.sh
```

The tarball contains wrapper commands for the controller, ground station, and
drone apps, plus bundled NDNSF runtime libraries. It also bundles `ndn-cxx`,
`ndn-svs`, NDNSD, NAC-ABE, OpenABE, and RELIC when those libraries are visible
through `ldd` on the build host. NFD is intentionally not bundled: it is a host
daemon with local sockets, faces, routes, and keychain state. Each machine
should run its own compatible NFD.

For sparse Ubuntu 20.04 lab machines, build the larger same-OS bundle:

```bash
NDNSF_UAV_RELEASE_INCLUDE_SYSTEM_LIBS=1 \
  packaging/uav-release/create-portable-release.sh
```

After copying the tarball to a target machine:

```bash
tar -xzf ndnsf-uav-ubuntu20-x86_64-*.tar.gz
cd ndnsf-uav-ubuntu20-x86_64-*
./scripts/check-runtime-deps.sh
```

The binaries load deployment names from `configs/uav_runtime.conf` by default.
That file keeps convenient demo values such as `/example/uav/controller`,
`/example/uav/gs`, and `/example/uav/drone/<id>`, but real deployment
identities and service names should live in a copied runtime config rather than
being edited into C++ code. For a real deployment, copy the runtime config,
change the namespace there, and keep the policy file and trust schema
consistent with those names:

```text
cp NDNSF-UAV-APP/configs/uav_runtime.conf /etc/ndnsf/uav_runtime.conf
```

The same values can still be overridden from the command line when needed:

```text
--runtime-config /etc/ndnsf/uav_runtime.conf
--group-prefix /example/uav/group
--controller-prefix /example/uav/controller
--ground-station-identity /example/uav/gs      # ground station only
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

Each running APP instance can also load its own config with `--app-config`.
The runtime config above is deployment-wide; the app config is per process.
This is how two drones keep different IDs, camera devices, MAVLink ports, or
serial devices while sharing the same service namespace:

```text
UavDroneApp --app-config /etc/ndnsf/drone-A.conf
UavDroneApp --app-config /etc/ndnsf/drone-B.conf
UavGroundStationApp --app-config /etc/ndnsf/ground-station.conf
```

Example templates are provided in:

```text
NDNSF-UAV-APP/configs/drone-A.conf
NDNSF-UAV-APP/configs/drone-B.conf
NDNSF-UAV-APP/configs/ground-station.conf
```

Command-line options override both app config and runtime config, so MiniNDN
and quick experiments can still adjust ports, video source, or target drones
without editing files.

If the deployment wants names such as `/ndn/ndnsf/uav-demo/...`, use those
runtime config values or command-line options instead of editing and rebuilding
the app, and update
`configs/uav_demo.policies` plus the deployment trust schema together.

### Certificate Bootstrap

If the operator has physical access to the PC and drones, NDNCERT is optional.
Use the manual root-signed certificate flow from the top-level README: each node
generates its private key locally, sends only the certificate request to the
CA/root machine, and installs the returned certificate plus the root
certificate.

Minimal example with the current UAV namespace:

```bash
# On the PC / CA machine
ndnsec key-gen -t r /example/uav > root.cert
ndnsec cert-install -f root.cert

# On drone A
ndnsec key-gen -n -t r /example/uav/drone/A > drone-A.req

# Back on the PC / CA machine
ndnsec cert-gen -s /example/uav -i ROOT drone-A.req > drone-A.cert

# Back on drone A
ndnsec cert-install -f root.cert
ndnsec cert-install -f drone-A.cert
ndnsec-ls-identity -c
```

Repeat the request/sign/install steps for `/example/uav/gs`,
`/example/uav/controller`, and every drone identity. Do not export/import a
safebag in this flow; the private key should remain on the machine that owns
the identity.

### Production Trust Schema

The example trust schemas use `type any` only for local examples and automated
regressions. A physical deployment must replace that anchor with the actual
deployment root certificate. Do not use `examples/trust-any.conf` for physical
deployment.

Copy `examples/trust-schema.conf` to a deployment-specific file and replace:

```conf
trust-anchor
{
  type any
}
```

with an absolute path to the root certificate:

```conf
trust-anchor
{
  type file
  file-name "/absolute/path/to/root.cert"
}
```

All controller, ground-station, drone, repo, and provider certificates should
be signed by this root or by a hierarchical child CA under the same namespace.

### Network And NFD

Every machine must run NFD, and the machines must have faces/routes that make
these prefixes reachable:

```text
/example/uav
/example/uav/controller
/example/uav/gs
/example/uav/drone/<id>
```

The exact face and routing setup depends on the deployment network. For a first
private LAN test, configure static UDP faces between the PC and each drone and
advertise the UAV namespace toward the PC/controller.

### Deployment Preflight

Before starting the containers on real machines, run the preflight checker. It
does not change system state; it checks that NFD is reachable, the expected
identity certificate is installed, the trust schema exists, and local adapters
such as ffmpeg, YOLO, video source, and MAVLink UDP ports are plausible. It also
fails if one identity has multiple local key/certificate choices, because that
can make the Controller encrypt permission responses to an old certificate.

On the Controller:

```bash
python3 NDNSF-UAV-APP/tools/uav_deployment_check.py \
  --role controller \
  --runtime-config NDNSF-UAV-APP/configs/uav_runtime.conf \
  --policy-file NDNSF-UAV-APP/configs/uav_demo.policies \
  --expected-cert /example/uav/drone/A=/path/to/drone-A.cert
```

The `--expected-cert` argument is optional but recommended in multi-machine
deployment. Export the public certificate on each remote node with
`ndnsec cert-dump -i /example/uav/drone/A > drone-A.cert`, copy it to the
Controller, and make preflight compare it with the Controller keychain.

Release wrappers can run the same check automatically before starting the app:

```bash
NDNSF_UAV_PREFLIGHT=1 ./bin/ndnsf-uav-controller
NDNSF_UAV_PREFLIGHT=1 \
NDNSF_UAV_PREFLIGHT_ARGS="--expected-cert /example/uav/drone/A=certs/drone-A.cert" \
  ./bin/ndnsf-uav-controller
```

On the PC / ground station:

```bash
python3 NDNSF-UAV-APP/tools/uav_deployment_check.py \
  --role ground-station \
  --runtime-config NDNSF-UAV-APP/configs/uav_runtime.conf \
  --app-config NDNSF-UAV-APP/configs/ground-station.conf \
  --trust-schema /absolute/path/to/uav-trust.conf
```

On drone A:

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

For a serial flight-controller link, replace the MAVLink backend options with:

```bash
  --flight-controller-backend serial \
  --mavlink-serial-device /dev/ttyAMA0 \
  --mavlink-serial-baud 57600
```

Warnings, such as `type any` in a trust schema, should be fixed before flight.
Failures must be fixed before starting the service containers.

### Start Order

On the PC:

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

In interactive ground-station mode, if `--ground-station-identity` is not
provided, the app opens a small certificate picker before the NDNSF runtime is
started. The picker lists identities already installed in the local ndn-cxx PIB.
Choose the identity whose certificate is trusted by the deployment trust
schema. Automated MiniNDN/smoke modes skip this dialog; pass
`--no-cert-dialog` to skip it in manual runs.

On drone A:

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

Use the same command on other drones with unique `--drone-id` values and
flight-controller connection settings. For a companion computer connected to a
PX4 flight controller over a serial MAVLink port, use:

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

If the drone uses `mavlink-router`, keep `--flight-controller-backend udp` and
point `--mavlink-udp-host` / `--mavlink-udp-port` at the router's local endpoint.
The `mavlink-router` backend name is accepted as an alias for this UDP path.

### GPS Source

DroneAPP obtains GPS and EKF readiness from the flight controller through
MAVLink telemetry. In the intended real-drone deployment, the GPS unit is
connected to the flight controller, and PX4/ArduPilot remains the authoritative
component for GPS fusion, arming checks, takeoff readiness, and flight control.
The companion computer does not scan USB or serial GPS devices directly.

### Flight-Controller Readiness And Safety

`UavDroneApp` now parses common MAVLink status messages from the UDP or serial
backend. Telemetry responses include `heartbeat_seen`, `armed`,
`flight_controller_ready`, `gps_ready`, `battery_ready`, `readiness`,
`ready_for_takeoff`, `gps_fix_type`, `gps_satellites_visible`,
`gps_fix_name`, `ekf_ready`, `system_status_name`, `landed_state_name`,
`battery_voltage_v`, `battery_current_a`, `altitude_m`, `groundspeed_mps`, and
`battery_percent` when the flight controller publishes them. The ground station
shows these fields in the telemetry/mission view so the operator can see
whether the selected drone is actually ready.

The ground station keeps these values as typed `TelemetryState` and
`MissionState` snapshots. The vehicle list, map markers, inspector panel, and
mission controls refresh from the same state model instead of parsing temporary
status strings, so multi-drone UI state remains tied to the selected drone.
Mission upload responses and later telemetry both update the same
`MissionState`; `uploaded`, `executing`, and `stopping` phases now drive the
Start Mission and Stop Patrol buttons.

Command responses no longer mean only "bytes were forwarded": for standard
MAVLink `COMMAND_ACK` messages the backend reports `ack_result`,
`ack_command_id`, and `ack_raw_result`. Non-manual commands are considered
accepted only when the flight controller acknowledges them as `accepted` or
`in-progress`; otherwise the NDNSF response is a failure.

Manual-control safety is intentionally conservative. The drone repeats the
latest `MANUAL_CONTROL` frame only inside a short freshness window. When that
window expires, it sends one neutral manual-control frame and stops replaying
until a new GS command arrives. This prevents stale keyboard/gamepad input from
continuing indefinitely after a link stall.
The ground station also checks recent telemetry before sending manual-control
updates: it requires a heartbeat, a ready flight controller, and an armed
vehicle. If those conditions are missing, manual-control packets are suppressed
and the operator sees the readiness reason instead of silently flooding the
link.

Takeoff is guarded by the telemetry state: the GS requires heartbeat,
flight-controller readiness, GPS/EKF readiness, battery readiness, and an armed
state before sending the Targeted takeoff command. The UI also exposes an
Emergency Stop button that uses the Targeted MAVLink path.

Before any real motor test:

1. Remove propellers.
2. Verify certificates, trust schema, NFD routes, and Drone/GS identity names.
3. Verify the flight-controller backend with `mock` or SITL first.
4. Verify that telemetry reports a heartbeat, GPS/EKF readiness, battery state,
   arming state, and command ACKs.
5. Test `Arm`, `Disarm`/`Land`, neutral manual control, and emergency-stop
   behavior with no propellers.
6. Only then proceed to a restrained or low-risk real-flight test.

Do not directly run the demo with propellers installed on an unvalidated
vehicle.

### Operator Workflow

1. Start NFD and `App_ServiceController` on the PC.
2. Start `UavDroneApp` on every drone and wait for the Drone window to report
   ready.
3. Start `UavGroundStationApp` on the PC.
4. Select a target drone in the left vehicle list.
5. Click `Arm`, then `Takeoff`.
6. Click `Start Control` for keyboard manual control.
7. Click `Start Video` to start the selected drone's video stream.
8. Click `Land` before shutdown.

Use the mock backend or SITL before a real flight controller. Do not enable
SITL-only safety-parameter changes on a real flight controller unless the
operator explicitly understands the effect. Mission assignment remains a shared
NDNSF service selected by ACK metadata, while MAVLink control and telemetry for
a specific drone use Targeted requests.

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
heavier compute on the latest decoded live frame. The ObjectDetection request
carries compact metadata such as frame id, drone id, and requested target
classes. Large images, recorded clips, reports, and other big objects should be
published as NDN segmented Data using the ndn-cxx segmenter/SegmentFetcher
pattern, or stored through `NDNSF-DistributedRepo` under a publisher-owned
name, then referenced by name in the service request. Keeping service requests
small avoids turning NDNSF invocation payloads into an ad-hoc file-transfer
channel.

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
  Start live downlink for this drone.

Drone -> drone-owned frame Data names
  Publishes signed video packets:
    /example/uav/drone/A/video/<stream-start-ms>/<packetSeq>

Ground station -> frame Data names
  Prefetches monotonically increasing packetSeq values. Data content carries
  frame metadata, so names stay sequential even when frame sizes vary.

Ground station -> /example/uav/drone/A/UAV/Camera/Video/stop/<nonce>
  Drone stops the live downlink and serving new stream packets.
```

The current GUI video control path uses NDNSF generic service invocation. The
full provider-specific name `/example/uav/drone/A/UAV/Camera/Video` is the
service name; the provider prefix is part of the service name to make this
camera-control service globally unique. High-rate video packets are still
fetched as signed NDN Data under the drone namespace, so the generic
request/response path carries control only, not the video byte stream.

Camera capture, local recording, and live downlink are intentionally separate:

```text
camera capture
  Drone-local camera acquisition. It may be enabled at DroneAPP startup.

local recording
  Drone-local storage of raw H264 chunks in an embedded NDNSF-DistributedRepo.
  This is controlled by the drone config, not by the ground station's video
  button.

live streaming
  Low-latency NDN Data publication requested by the ground station through the
  provider-specific video-control service.
```

This means a drone can keep its camera running and record to its local repo even
when no ground station is watching. Conversely, the ground station can request a
temporary live downlink without changing the drone's recording policy. The
drone video-control response reports the effective `capture`, `recording`,
`recording_session_id`, `recording_object_prefix`, `recording_chunks`, and
`recording_bytes` values.

For real deployment, `UavDroneApp` reads `video-source auto` from the drone
config by default. In this mode it selects the first local V4L2 capture device
such as `/dev/video0`; if no usable camera is present, it falls back to the
sample video `videos/drone.mp4` for local troubleshooting. A deployment can
force a specific USB camera or file by setting `video-source /dev/videoX` or a
file path in the drone config. USB UVC camera capture uses adaptive defaults:
`camera-v4l2-input-format auto`, `camera-v4l2-input-size auto`, and
`camera-v4l2-input-fps 0`. In this mode the drone queries the camera and chooses
a conservative format and size, preferring YUYV 640x480 when available. A frame
rate value of `0` means the drone does not force the V4L2 input frame interval
and only downsamples later in the encoder filter. This avoids cameras or
embedded USB controllers that fail when probed with explicit frame-rate
settings. Check the real camera first with
`ffmpeg -f v4l2 -list_formats all -i /dev/videoX`, then override these keys only
when the device and port are stable. The selected
source is encoded through `ffmpeg`/V4L2 as a low-latency H264 byte stream and
split into NDN-sized video
packets. This is not ordinary NDN segmentation of one object. Each Data
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
`/<drone>/UAV/Camera/Video` control service request. The drone clamps both
bitrate and width to its supported range and maps the accepted bitrate to an
H264 CRF quality setting for `ffmpeg`. The drone then returns the requested bitrate,
accepted bitrate, requested width, accepted width, FPS, encoder quality, and
packet payload size. The ground station derives its prefetch window from those
returned values and from the packet high-watermark carried in each packet.
It also uses the measured video RTT to adapt the live prefetch window,
lookahead, decoder reorder window, Interest lifetime, probe backoff, and
missing-packet skip timeout. Timeout and Nack pressure are folded into the same
policy: when the link starts dropping or delaying chunks, the GS reduces
lookahead/prefetch pressure and shortens the decoder's wait for missing delta
chunks. This keeps low-bitrate/low-FPS camera streams from overfetching while
giving higher-bitrate streams enough in-flight Interests to avoid stalls.
The default is currently 8000 kbps, 480 px frame width, and 30 FPS for the demo
H264 stream. Raising bitrate improves stream quality and packet volume; raising
frame width makes the displayed video larger.

When `Stop Video` is invoked, the drone stops the live stream, clears pending
Interests and cached stream packets, and ignores late frame Interests for the
stopped stream. The GS sends the stop control request even after it has stopped
the local decoder. If the control response times out, NDNSF emits selection-status
timeout diagnostics and the GS prompts the operator to click `Stop Video` again
if the drone still reports streaming; duplicate stop requests are safe because
the drone treats stop as idempotent. If the drone config enables `camera-capture-on-start` or
`camera-record-to-local-repo`, the camera capture loop may continue running
locally after the live stream stops.

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

The current ground-station window follows the same rough organization as a
QGroundControl Fly View: a left vehicle list for multi-drone awareness, a center
fly workspace for map/mission context and video, and a right inspector for
telemetry, services, and command status. It is intentionally simpler than QGC:
the map is still a lightweight OpenStreetMap tile plus text workspace rather
than a full GIS map, but it starts centered on the ground station near the
University of Memphis and draws labeled `GS`, drone, and mission waypoint markers.
The map toolbar has zoom-in, zoom-out, `Center GS`, `Undo WP`, and `Clear WPs`
buttons; dragging the map pans to nearby areas, and `Center GS` returns the
view to the ground station. This mirrors the QGroundControl split between
planning and flying at a smaller scale: click the map repeatedly to append
`WP1`, `WP2`, and later mission waypoints, use undo/clear while planning, then
upload and start the mission from the fly controls. Drone markers are updated
from `/UAV/Telemetry/GetStatus`; with the
UDP MAVLink backend, that telemetry is populated from the flight controller, so
the marker tracks the same vehicle that ManualControl is commanding. The left
vehicle list can switch the active target drone, while the telemetry poller
rotates across the expected drone list so Drone A and Drone B can both remain
visible on the map. When at least two explicit waypoints are present,
`Upload Patrol Mission` clusters the route by the number of patrol drones. Each
drone receives one compact waypoint cluster, the cluster is ordered into a short
local route, and the final waypoint returns to that drone's departure position
sampled from telemetry at mission-upload time. If telemetry is unavailable, the
route falls back to the first waypoint in that cluster as the return point.
Otherwise it falls back to the center latitude/longitude and side-length boxes
and generates one adjacent patrol sector per drone.

For offline demos, prepare a small Memphis tile cache before running MiniNDN:

```bash
python3 NDNSF-UAV-APP/tools/prepare_memphis_offline_map.py
```

The default cache is a 3x3 OpenStreetMap tile window around the University of
Memphis at zoom levels 14, 15, and 16, stored under `NDNSF-UAV-APP/maps/osm/`.
The ground-station GUI reads this offline cache first, then `/tmp`, and only
then tries the network. The MiniNDN launcher also prefetches these levels on
the host side when the tracked cache is missing, so the map zoom buttons still
work after node network namespaces are isolated.

The command path should remain identical: GUI actions build MAVLink bytes at the
ground station, then send opaque MAVLink frames through NDNSF Targeted service
invocation.

NDNSF's built-in NDNSD integration remains disabled by default. For UAV demos it
can be useful as a low-frequency service advertisement channel: each DroneAPP
can periodically publish installed services such as video control, Targeted
MAVLink execution, telemetry, camera frame, and mission assignment. The core now
has a generic `ServiceProvider::publishServiceInfo(...)` hook for that purpose,
but the MiniNDN launcher still keeps `NDNSF_DISABLE_NDNSD=1` because the older
NDNSD runtime path needs a separate compatibility pass before it is safe for the
GUI demo. Performance and latency tests should keep the default disabled
setting.

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
On a physical drone, use `UavDroneApp --headless` when no local operator window
is needed. Headless mode runs the same NDNSF services, MAVLink backend, camera,
and local recording pipeline, but avoids the GTK window and the need for Xvfb on
small onboard computers.

Start NFD and the UAV controller once, then start one drone window and one
ground-station window:

```bash
nfd-start

./build/examples/App_ServiceController \
  --controller-prefix /example/uav/controller \
  --policy-file NDNSF-UAV-APP/configs/uav_demo.policies

./build/examples/UavDroneApp --drone-id A --video-source /dev/video0
# On a real onboard computer without a display:
# ./build/examples/UavDroneApp --drone-id A --video-source auto --headless
./build/examples/UavGroundStationApp --target-drone A \
  --video-bitrate-kbps 8000 --video-width 480
```

For file-based local debugging without a camera, pass a video file to
`--video-source`; the MiniNDN launcher does this automatically only as a
fallback after trying a real or virtual camera.

Drone camera policy is configured on the drone side. To keep capture running
from startup and persist raw H264 chunks to a local SQLite-backed embedded repo:

```bash
./build/examples/UavDroneApp \
  --drone-id A \
  --video-source /dev/video0 \
  --camera-capture-on-start \
  --camera-record-to-local-repo \
  --camera-record-repo-path /var/lib/ndnsf-uav/drone-A-camera.sqlite3 \
  --camera-record-object-prefix /muas/drone/A/repo/camera/recording
```

The default example `drone-A.conf`/`drone-B.conf` leaves recording disabled;
set `camera-record-to-local-repo true` there for unattended deployments.
When recording is enabled, the drone also exposes a provider-specific manifest
service:

```text
/<drone>/UAV/Camera/Recording/Manifest
```

For example, `/example/uav/drone/A/UAV/Camera/Recording/Manifest` returns the
current recording session id, object prefix, object naming pattern, chunk count,
byte count, and first/last chunk object names. This lets GS or a post-mission
report discover the local repo objects without guessing file paths or scanning
the SQLite store. The manifest intentionally exposes service object names, not
the drone's local SQLite file path.

The ground station has `Find Recordings` and `Play Recording` buttons for the
selected drone. `Find Recordings` calls the manifest service above. `Play
Recording` then uses a recording helper to fetch the encrypted repo Data named
by that manifest. The chunk path is intentionally not an NDNSF service:

```text
/<drone>/repo/camera/recording/<session-id>/chunk/<index>
```

The manifest service is the authorization point. Its NDNSF-protected response
includes the recording encryption metadata and content key for authorized
viewers. The repo stores only hybrid AES-GCM encrypted H264 chunks; the drone
serves those encrypted chunks as ordinary signed NDN Data, and the ground
station decrypts them locally before feeding the live-video decoder. Fetching
the Data without the manifest key is not enough to view the recording.

For a local recording smoke test that does not start the GUI or require GS
interaction:

```bash
rm -f /tmp/ndnsf-uav-camera-record-smoke.sqlite3
./build/examples/UavDroneApp \
  --auto-camera-record-smoke \
  --video-source NDNSF-UAV-APP/videos/drone.mp4 \
  --camera-record-to-local-repo \
  --camera-record-repo-path /tmp/ndnsf-uav-camera-record-smoke.sqlite3 \
  --camera-record-chunk-limit 3
```

Success prints `DRONE_CAMERA_RECORD_SMOKE_OK` with the last chunk object name
after raw H264 chunks have been stored in the local repo.

Click `Arm`, `Takeoff`, or `Land` in the ground-station window to send Targeted
MAVLink commands to the drone. For manual flight, click `Start Control` and
choose `Keyboard` or `Xbox Gamepad` in the control panel. The gamepad option is
disabled when no readable `/dev/input/js*` device is present. The layout follows
the usual QGroundControl/Mode-2 mental model: the left stick controls
yaw/throttle, and the right stick controls roll/pitch.

Keyboard layout:

```text
Left stick emulation              Right stick emulation
        R throttle up                     W pitch forward
A yaw left   D yaw right          Q roll left   E roll right
        F throttle down                   S pitch back

Commands: I arm, T takeoff, L land, V video start, X video stop
```

Xbox gamepad layout:

```text
Left stick:  yaw / throttle
Right stick: roll / pitch
A: arm       Y: takeoff
B: land      X: start/stop video
```

While a key, stick, or gamepad button is active, the corresponding control turns
black so the operator can see which command is active. While control mode is enabled, the ground station keeps
sending low-rate Targeted `MANUAL_CONTROL` updates, including neutral updates
when no key is pressed. The drone repeats the latest manual frame locally at a
higher rate for a short freshness window, so PX4 sees a continuous control
stream even when NDNSF request/response timing jitters. Manual-control responses
include the currently available flight-controller status fields such as
`altitude_m`, `groundspeed_mps`, `battery_percent`, and controller state.
When the MiniNDN launcher starts PX4 SITL itself, DroneAPP is also passed
`--configure-px4-sitl-demo-params`. This sends a few MAVLink `PARAM_SET`
messages to make the demo tolerate Targeted-control jitter better
(`COM_RC_LOSS_T=30`, `COM_FAIL_ACT_T=25`, `NAV_RCL_ACT=1`). The flag is not
enabled by default for a manually started DroneAPP, so real flight controllers
keep their own safety policy unless the operator explicitly opts into the
SITL demo behavior.

Basic operation order:

1. Wait until the Drone window reports `ready for takeoff`.
2. Click `Arm` or press `I`. Arming unlocks the flight controller so it is
   allowed to spin motors and accept flight commands; use it only when the
   vehicle is ready to fly.
3. Click `Takeoff` or press `T`. In the PX4/jMAVSim demo, this sends the drone
   to a low fixed hover altitude.
4. Click `Start Control`, then hold keys to maneuver. Use `R` to climb and `F`
   to descend; release keys to send neutral control.
5. Click `Land` or press `L` before closing the demo.

The PX4/jMAVSim demo currently sends `Takeoff` as a raw MAVLink
`MAV_CMD_NAV_TAKEOFF` with an absolute altitude value suitable for the default
SITL world. The default is intentionally low for the simulator camera view. A
later telemetry pass should translate the operator's relative
takeoff altitude into the vehicle's current AMSL altitude before building the
MAVLink command.

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
GS_STATUS Video stopped, packets=<stream-packets>, fec_groups=<fec-groups>
DRONE_STATUS drone=A object detection frame=<n> objects=Car
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

In interactive mode, the launcher starts PX4 SITL with the jMAVSim GUI on the
same MiniNDN node as DroneAPP by default, so the simulator window appears with
the drone window and manual-control reactions are visible. Use
`--no-start-jmavsim` to keep the mock flight-controller backend.
Running the launcher without extra flags starts the two-drone interactive demo:
Drone A runs on `ucla`, Drone B runs on `wustl`, and the ground station lists
both vehicles for target switching. The launcher also prefetches the initial
University of Memphis OpenStreetMap tile on the host before MiniNDN isolates
node network namespaces if the offline cache is missing, so the map pane has a
real tile immediately.
DroneAPP starts as soon as the simulator process is launched; it does not block
the GUI while PX4 finishes booting. The Drone window shows
`Flight controller: starting`, `simulator connected`, and `ready for takeoff`
from a small status file written by the launcher.
PX4/jMAVSim output is filtered before it reaches `jmavsim-<drone>.log`: repeated
`pxh>` prompt updates are dropped and the log is capped by
`NDNSF_UAV_JMAVSIM_LOG_MAX_BYTES` (default 8 MiB). This avoids VM stalls caused
by terminal prompt spam during interactive demos.
The launcher also enables DroneAPP's PX4 SITL demo parameter setup by default;
use `--no-configure-px4-sitl-demo-params` to leave PX4's RC/manual-control
failsafe parameters untouched.
`--enable-ndnsd` is currently reserved for NDNSD experiments; the launcher still
exports `NDNSF_DISABLE_NDNSD=1` until the NDNSD runtime compatibility pass is
done.
Use `--multi-drone-gui` to start the patrol-drone set in an interactive GUI run
and populate the ground-station vehicle list, for example with
`--patrol-drone-ids A,B --patrol-drone-nodes ucla,wustl`.
The ground-station window also has an `Upload Patrol Mission` button that runs
the same cooperative patrol upload flow from the GUI. Use `+`/`-` to change
zoom, drag the map to inspect nearby areas, and press `Center GS` to return to
the ground station. Click the map repeatedly to append `WP1`, `WP2`, and later
waypoints; `Undo WP` removes the last point, and `Clear WPs` resets the plan.
If at least two map waypoints exist, the ground station clusters that route by
the patrol-drone count and sends only each drone's resulting waypoint text in
the NDNSF request payload. Each assigned route ends by returning to that
drone's departure position sampled from telemetry at upload time. If no route is drawn, it uses the three patrol input
boxes: center latitude, center longitude, and side length in meters, and
generates one adjacent patrol sector per drone. Uploading a
mission only installs waypoints in PX4. Pressing `Start Mission` now follows a
QGroundControl-style phased sequence: arm all patrol drones, then send takeoff
to all patrol drones, then start the uploaded mission on all patrol drones. A
short stagger is kept inside each phase so the Targeted MAVLink path does not
drop back-to-back requests. The start sequence uses only drones that
successfully accepted the current mission upload; drones with missing or timed
out uploads are skipped so an old PX4 mission cannot be started by accident.
`Stop Patrol` sends `land` to the patrol drones.

For a one-drone mission-upload smoke test, useful before starting a heavier
single PX4/jMAVSim instance:

```bash
xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-single-mission-test \
  --no-start-jmavsim --no-cli --no-xhost
```

To exercise the same path against one simulator, remove `--no-start-jmavsim`
and use `--flight-controller-backend udp`. This starts only one DroneAPP and one
simulator, then sends a small rectangular mission to the target drone.
The launcher sets PX4/jMAVSim home to the same University of Memphis position
used by the ground-station map by default:
`--sim-home-lat 35.1186 --sim-home-lon -89.9375 --sim-home-alt 100`. This
matters because jMAVSim's upstream default home is near Zurich; if the simulator
keeps that default, PX4 rejects Memphis waypoints as millions of meters away, so
the vehicle can take off but will not enter the uploaded mission.
The validated single-simulator command is:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-single-mission-test \
  --auto-single-mission-start-test \
  --flight-controller-backend udp \
  --start-jmavsim --jmavsim-headless --no-cli
```

A successful run prints `NDNSF_UAV_SINGLE_MISSION_MININDN_SMOKE_OK`; the ground
station log should show `mission_transport=mavlink-mission-upload` and
`mission_ack=accepted`, while the drone log should show `UDP_FC_MISSION_COUNT`,
four `UDP_FC_MISSION_REQUEST` / `UDP_FC_MISSION_ITEM_SENT` pairs for the
default rectangle, `UDP_FC_MISSION_ACK ... result=accepted`, and
`UDP_FC_COMMAND_ACK ... command=start_mission result=accepted`.

For the non-interactive two-drone cooperative patrol smoke test:

```bash
xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-patrol-test \
  --patrol-drone-ids A,B \
  --patrol-drone-nodes ucla,wustl \
  --no-start-jmavsim --no-cli --no-xhost
```

The ground station clusters drawn waypoints by the patrol-drone count, or
generates one adjacent patrol sector per drone when no route is drawn. It
assigns those parts through `/UAV/Mission/Assign`, appends a telemetry-based
return-to-departure waypoint to each drone's route, and sends a compensation request if a part is
missing. With the UDP MAVLink backend, DroneAPP now tries the standard
MAVLink mission upload handshake (`MISSION_COUNT`, `MISSION_REQUEST(_INT)`,
`MISSION_ITEM_INT`, `MISSION_ACK`). With the mock backend, the deterministic
smoke path still reports command-long waypoint forwarding. Drone responses
report `mission_transport`, `mission_ack`, `waypoints_forwarded`,
`waypoint_acks_accepted`, and `last_waypoint_ack`, so field tests can tell
whether the simulator/flight controller accepted the mission.

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
frames and that the drone entered and left streaming mode. In the integrated
runtime, the ground station also serves `/UAV/GS/ObjectDetection`; during live
video the drone periodically calls it and logs `Car`/`Truck` detections. The
default detector uses the long-lived `tools/yolo_detect_worker.py` with
`yolo26n.pt`, so the model is loaded once and reused across low-rate requests.
Override it with `--yolo-model` and `--yolo-worker-script` when deploying a
different local model or worker. `--yolo-script` remains as the one-shot
fallback helper. CPU inference is intentionally low-rate, currently about 1 Hz.

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

To explicitly run PX4 SITL with jMAVSim on the same MiniNDN node as the drone
and forward commands to PX4's GCS MAVLink UDP port:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --start-jmavsim \
  --flight-controller-backend udp \
  --mavlink-udp-port 18570
```

The Drone app also binds its local MAVLink GCS port, default `14550`, for
PX4-side command acknowledgments and telemetry. Override it with
`--mavlink-udp-listen-port` if another GCS process is already using that port.
In interactive MiniNDN mode, closing the ground-station GUI stops the launcher
and cleans up PX4/jMAVSim so simulator processes do not keep burning CPU in the
background.

For a non-interactive PX4/jMAVSim smoke test:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --start-jmavsim --jmavsim-headless \
  --auto-manual-control-test --no-cli
```

To regression-test live PX4/jMAVSim telemetry fields and state changes:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --start-jmavsim --jmavsim-headless \
  --flight-controller-backend udp \
  --auto-telemetry-test --no-cli
```

This checks `gps_fix_name`, `ekf_ready`, `landed_state_name`,
`battery_voltage_v`, `armed`, and `lat/lon` while the GS runs
arm/takeoff/land over NDNSF Targeted requests.

For the two-drone jMAVSim path, the launcher starts PX4 with explicit
instances (`px4 -i 0`, `px4 -i 1`) instead of invoking the single-instance
`make px4_sitl jmavsim` target twice. Drone A uses PX4 MAVLink UDP port
`18570` and simulator TCP port `4560`; Drone B uses `18571` and `4561`.

To smoke-test switching manual control between two drones:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-two-drone-switch-test --multi-drone-gui \
  --flight-controller-backend udp \
  --start-jmavsim --jmavsim-headless --no-cli
```

To smoke-test cooperative patrol mission upload with two PX4/jMAVSim
instances:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-patrol-test --multi-drone-gui \
  --flight-controller-backend udp \
  --start-jmavsim --jmavsim-headless --no-cli
```

The launcher keeps MiniNDN node homes under `/tmp/minindn/<node>`, so it
preserves the current Python package path for PX4 build helpers such as
`kconfiglib`. It also passes `CMAKE_ARGS=-DCMAKE_POLICY_VERSION_MINIMUM=3.5` by
default for newer CMake versions; override this with `--px4-cmake-args` if your
PX4 checkout no longer needs it.

The launcher keeps framework logs quiet by default:
`ndn_service_framework.*=WARN` with UAV app logs at `INFO`. Override
`NDNSF_APP_NDN_LOG` only when debugging NDNSF internals.

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

## Development Roadmap

The app is now useful as a MiniNDN and SITL demonstrator, but the next work is
to make it a deployable UAV service-container workload. The planned order is:

1. **State model consolidation.** Treat telemetry, readiness, mission, and
   video state as the single source of truth for the drone list, map markers,
   inspector text, command buttons, and smoke-test markers. GUI code should not
   infer state from ad hoc status strings when a typed state model is available.
2. **Drone headless deployment mode.** Keep the Drone container usable on
   ODROID-class or real airframe computers without a GUI/X server. In headless
   mode the app should run only NDNSF, MAVLink, camera, repo, telemetry, and
   mission services.
3. **Flight-controller readiness and safety gates.** Before arm/takeoff/mission
   execution, surface heartbeat, GPS fix, EKF readiness, battery, arming state,
   mode, and landed state. Manual control must time out to neutral, and
   emergency stop / lost-link behavior must be explicit.
4. **Adaptive video service quality.** Continue treating video as an NDNSF
   service workload: requested bitrate, accepted bitrate, RTT, backlog,
   timeout pressure, key-frame recovery, and FEC should drive prefetch and
   skip decisions rather than fixed constants.
5. **Mission collaboration model.** Promote the patrol demo into a reusable
   mission model with `MissionPlan`, `MissionPart`, assignment, progress,
   failure/compensation, and return-to-home semantics.
6. **Repo-backed UAV data products.** Store recordings, mission images,
   telemetry logs, object-detection events, and reports through
   `NDNSF-DistributedRepo` using publisher-owned names, encrypted payloads,
   and manifest-based discovery.
7. **Distributed inference integration.** Connect selected image and
   object-detection workflows to `NDNSF-DistributedInference` when model
   execution is split across ground stations, drones, and edge machines.
