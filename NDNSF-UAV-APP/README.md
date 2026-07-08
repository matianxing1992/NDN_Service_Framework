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
  as signed NDN video packets under its own namespace, and the ground station
  prefetches packets by predictable names.
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

## Current Runtime Status

The current implementation should be read as a service-oriented UAV prototype
with several real deployment-facing pieces already wired in:

- **Camera.** DroneAPP supports file, USB/V4L2, and `auto` video sources. In
  `auto` mode it probes local camera capability conservatively, falls back to
  the bundled sample video for local troubleshooting when no camera is usable,
  and reports camera availability/source/reason through telemetry and readiness
  state so GS can show whether the selected drone actually has a usable camera.
- **Flight controller.** DroneAPP supports mock, UDP MAVLink, serial MAVLink,
  and the `mavlink-router` alias for the UDP path. GPS is intentionally read
  from MAVLink telemetry produced by PX4/ArduPilot; the companion computer does
  not scan standalone USB or serial GPS units. GS shows heartbeat, armed,
  flight-controller readiness, GPS/EKF, battery, landed state, and command ACK
  fields when the backend provides them.
- **Mission.** Mission upload/start/stop is represented as typed
  `MissionState`, `MissionPlan`, and `MissionProgressState` data. Patrol
  waypoints are clustered deterministically across selected drones, each part
  can include a return-to-start target sampled from telemetry, and GS prevents
  obvious duplicate mission operations while progress is active.
- **Video.** Live video uses a provider-specific video-control NDNSF service and
  drone-owned signed NDN Data packets for the high-rate stream. The receiver
  tracks adaptive RTT/timeout/backlog pressure, packet sequence, stream id, and
  stream session metadata so stale packets from an older live session can be
  dropped instead of being displayed as current video.
- **Repo.** Drone-side recording to an embedded `NDNSF-DistributedRepo` is
  optional and configured on the drone, not by the GS Start Video button. The
  current concrete repo-backed UAV data product is encrypted camera recording:
  GS discovers it through a recording manifest service, then fetches/decrypts
  the named chunks for playback. Mission images, telemetry logs, detection
  events, and reports are planned extensions of the same data-product model.

The drone does not interpret MAVLink command semantics. The ground station owns
MAVLink message construction. The drone app treats MAVLink as opaque bytes and
passes them to the flight-controller backend.

MAVLink execution uses NDNSF Targeted invocation. The first command to a drone
bootstraps through the normal authenticated request/ACK/selection/response
flow and obtains one-time token pairs. Later `arm`, `takeoff`, and `land`
commands, as well as low-rate `MANUAL_CONTROL` updates from the keyboard, use
request/response-only Targeted calls to `/UAV/MAVLink/Execute`, reducing command
latency while still validating the provider and rejecting token replay.

## Current Boundary

This application is not yet a production-grade flight ground station. It is a
research and validation workload for NDNSF that happens to exercise a realistic
UAV service stack. The parts that are most mature are the NDNSF service
composition, permission-controlled cross-node invocation, typed telemetry and
mission state, adaptive video transport, recording discovery, and MiniNDN/SITL
test paths. The parts that still require careful real-device hardening are
flight-controller fail-safe policy, long-duration mission recovery, operator
UX under abnormal hardware/network states, and repeated outdoor flight testing.

The intended interpretation is therefore:

```text
NDNSF contribution:
  service discovery, authorization, provider collaboration, Targeted command
  paths, same-process helper composition, large-data references, and service
  containers.

UAV-APP contribution:
  a high-pressure UAV workload that validates those NDNSF mechanisms with
  command-sensitive control, telemetry, video, mission, recording, and
  multi-drone workflows.

Not claimed:
  a drop-in replacement for a certified autopilot ground station.
```

## Comparison With QGroundControl

QGroundControl is a mature operator-facing ground-control station for
PX4/ArduPilot. It is optimized around direct vehicle operation: setup,
calibration, parameter management, mission editing, map/video display, safety
checks, and robust operator feedback. `NDNSF-UAV-APP` has a different purpose:
it validates whether UAV functions can be expressed as named, permissioned,
multi-provider NDNSF services. The useful comparison is therefore not "which
ground station is more complete", but "which operator-facing capabilities are
still needed before this NDNSF UAV workload can be trusted in harder tests".

Current strengths relative to a conventional ground station:

- UAV functions are exposed as named services rather than only as one vehicle
  link.
- Cross-node command, telemetry, video control, recording discovery, and
  object-detection callbacks all use the same NDNSF authorization and naming
  model.
- Targeted invocation keeps known-drone commands low-latency while preserving
  token and permission checks.
- Multiple drones/providers can be selected or coordinated by service logic,
  which is the main NDNSF-specific value.
- Same-process helpers are separated from cross-node services through
  `ServiceContainer.localRegistry()`, so local composition does not become a
  new network protocol.

Major gaps compared with a mature ground-control workflow:

- **Vehicle setup and calibration.** QGroundControl provides rich sensor,
  radio, motor, airframe, parameter, and firmware setup flows. UAV-APP assumes
  the flight controller is already configured and only consumes MAVLink state.
- **Mission editing.** UAV-APP has deterministic waypoint clustering and
  mission state tracking, but it does not yet provide a full map-based mission
  editor with survey patterns, altitude profiles, geofence editing, or reusable
  mission files.
- **Safety UX.** UAV-APP has typed readiness, command lifecycle, stale-link
  detection, manual-control neutral fallback, and emergency stop. It still
  needs more explicit pre-arm check presentation, persistent alerts,
  operator-confirmed dangerous actions, and clearer lost-link behavior.
- **Parameter and log workflow.** UAV-APP does not replace QGC-style parameter
  inspection, tuning, MAVLink log download, or flight review. It should either
  integrate only the pieces needed for NDNSF experiments or document that those
  tasks remain external.
- **Long-duration reliability.** The app has MiniNDN/SITL and local smoke
  tests, but real deployments still need repeated long-run video, telemetry,
  mission, recording, and multi-drone recovery tests.
- **Hardware breadth.** The code supports mock, UDP, serial, USB/V4L2, and
  auto camera paths, but it has not been validated across the hardware range
  that a mature ground station normally sees.

Near-term improvement priorities:

1. Make the safety/readiness panel operator-grade: pre-arm checklist, reasoned
   blocks, persistent warning history, and clear confirmation for dangerous
   commands.
2. Strengthen mission planning and recovery: richer waypoint editing,
   persistent mission records, partial completion, compensation tasks, cancel,
   and return-to-start policies.
3. Keep improving video stability under real wireless pressure: adaptive
   bitrate/request-window control, frame-order guards, stream-session guards,
   and visible transport diagnostics.
4. Expand regression coverage around the behaviors an operator cares about:
   selected-drone stability, command lifecycle, stale telemetry, Start/Stop
   Video idempotence, recording playback, mission cancel/recovery, and manual
   neutral fallback.
5. Treat real-device testing as a source of new NDNSF requirements. If UAV
   deployment exposes missing framework mechanisms, those findings should feed
   back into NDNSF rather than being patched only inside the app.

The gaps above should be tracked as concrete engineering and evaluation items,
not as open-ended UI wishes:

| Area | Current implementation | Needed improvement | Validation path |
| --- | --- | --- | --- |
| Vehicle setup/calibration | Consumes MAVLink status from an already configured PX4/ArduPilot backend. | Add deployment diagnostics for required flight-controller state; keep full calibration/firmware setup external unless a NDNSF experiment needs it. | Preflight check plus SITL telemetry showing heartbeat, GPS/EKF, battery, armed, landed state, and command ACKs. |
| Mission editor | Supports deterministic waypoint clustering, mission state, progress, partial recovery, cancel, and return-to-start targets. | Add richer map editing, reusable mission files, geofence/survey templates, and clearer per-drone segment review. | Unit tests for deterministic planning plus MiniNDN mission upload/start/stop/cancel smoke. |
| Safety UX | Maintains typed readiness, safety, command lifecycle, stale telemetry, manual neutral fallback, and emergency stop. | Add persistent warning history, explicit pre-arm checklist, dangerous-action confirmation, and clearer lost-link policy display. | UI smoke at 1600x800 plus protocol tests for safety gates, stale/lost telemetry, command timeout, and manual neutral fallback. |
| Parameter/log workflow | Does not manage autopilot parameters or flight log review. | Either integrate only the small parameter/log subset needed by NDNSF experiments or document use of external tools. | README/release manual regression that states the boundary and avoids claiming QGC replacement. |
| Long-duration reliability | Has MiniNDN/SITL smoke paths and local video/mission/recording tests. | Add repeated long-run telemetry, video, recording, mission recovery, and multi-drone loss/reconnect scenarios. | Scheduled MiniNDN profiles with 0/5/15% loss plus real-device run logs when hardware is available. |
| Hardware breadth | Supports mock, UDP, serial, `mavlink-router` alias, file video, USB/V4L2, and auto camera probing. | Build a hardware compatibility matrix for ODROID/PC, USB cameras, serial/UDP MAVLink, and flight-controller variants. | Preflight/device diagnostics, camera capability logs, and ODROID/GCP release smoke when explicitly requested. |

The same work can be grouped by product quality axis:

| Axis | What should improve | Concrete next items | Test evidence |
| --- | --- | --- | --- |
| Functionality | Add operator-visible capabilities that are currently missing or partial. | Mission editor, per-drone mission segment review, persistent mission files, recording/log browsing, limited parameter/status inspection, richer object-detection result display, and clearer multi-drone service selection. The ground-station state layer exposes these as `UavFunctionalityState` with `available`, `prototype`, `limited`, `metadata-only`, or `missing` values so incomplete features are not accidentally presented as finished. | Unit tests for mission planning/state and `UavFunctionalityState`, MiniNDN mission smoke, recording playback smoke, and documentation checks that each feature states its boundary. |
| Practicality | Make the app easier to deploy, operate, and diagnose without knowing the internals. | Better preflight summaries, hardware compatibility notes, camera/flight-controller diagnostic panels, config validation, identity/certificate guidance, and readable operator workflows for GS and Drone windows. The GS inspector exposes these as `UavPracticalityState` so deployment usability is visible as structured state instead of being buried in docs. | Documentation regression, `UavPracticalityState` unit tests, preflight script runs, GUI smoke at 1600x800, and release/manual walkthroughs. |
| Stability | Make runtime behavior predictable under loss, timeout, repeated clicks, old packets, stale telemetry, and long-running sessions. | Command lifecycle/timeout handling, Stop Video idempotence, stream-session and frame-sequence guards, adaptive video pressure control, stale/lost telemetry handling, manual neutral fallback, and long-duration loss profiles. The GS inspector exposes these as `UavStabilityState` so transport/control stability is visible to the operator instead of being only a log detail. | `UavProtocolState` and `UavStabilityState` unit tests, MiniNDN 0/5/15% loss profiles, Start/Stop Video smoke, stale telemetry smoke, and real-device run logs when explicitly requested. |

Automated GS smoke paths also emit `UAV_APP_QUALITY_STATE`, which summarizes
`UavFunctionalityState`, `UavPracticalityState`, and `UavStabilityState` in one
log line. This gives MiniNDN regressions a single observable checkpoint for the
application's functionality, practicality, and stability posture.

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

The current container boundary follows a simple rule. Cross-node control and
data discovery always use NDNSF remote or Targeted invocation, including
MAVLink commands, video-control requests, telemetry polling, mission
assignment, recording manifest discovery, and GS object-detection calls from a
drone. Same-process helper composition uses the core
`ServiceContainer.localRegistry()` path instead. Today this is used for
helpers such as GS object-detection execution, Drone camera-status reads, and
Drone recording-manifest assembly. These local helpers are trusted
process-internal functions; they do not create a new wire protocol mode and
cannot be selected by a remote caller.

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

At runtime startup, NDNSF resolves certificate roles once: RSA remains the
encryption certificate for NAC-ABE and permission unwrap, while an installed
EC/ECDSA certificate is preferred for signing. The drone video and recording
Data publisher caches that signing choice, so frame-level publishing does not
rescan the keychain.

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

The ground station keeps these values as typed `TelemetryState`, `MissionState`,
and safety-gate snapshots. The vehicle list, map markers, inspector panel,
flight action bar, and mission controls refresh from the same state model instead
of parsing temporary status strings, so multi-drone UI state remains tied to the
selected drone.
Mission upload responses and later telemetry both update the same
`MissionState`; `uploaded`, `executing`, and `stopping` phases now drive the
Start Mission and Stop Patrol buttons. Start Mission additionally combines the
mission phase with a typed `FlightSafetyGateState`, so an uploaded mission is
shown as blocked until the selected patrol drones have usable readiness and
link/safety state. The mission-control model also records typed upload/start/stop
reasons such as `waiting-heartbeat`, `progress-active`, and `ok`, so the UI and
smoke tests do not have to infer why a mission button is disabled. Stop Patrol
remains available for uploaded or active missions so the operator can still land
drones during abnormal states.

The ground station also keeps a typed `FlightCommandState` for the latest
flight-control command per drone. Targeted MAVLink responses, command timeouts,
blocked readiness gates, and command-in-flight drops all update this model.
The vehicle list and inspector can therefore show the most recent command,
ACK result, flight-controller state, and timeout/block reason without parsing
ad hoc log strings.

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

The same information is exposed as a typed `SafetyState`. Drone telemetry now
reports the current link state, manual-control freshness, replay activity,
neutral fallback, and replay count. The GS vehicle list, map marker, and
inspector render that model directly, so stale manual input and heartbeat loss
are visible as state rather than only as backend logs.
The GS also derives local telemetry age from the latest received
`TelemetryState`. `link-stale-ms`, `link-lost-ms`, and `lost-link-action` in
the ground-station config determine when a selected drone is shown as
`stale` or `lost`; this is a local operator diagnostic and does not change the
NDNSF service protocol.

Takeoff is guarded by the telemetry state: the GS requires heartbeat,
flight-controller readiness, GPS/EKF readiness, battery readiness, and an armed
state, and it blocks Takeoff unless `landed_state_name` is explicitly
`on-ground`, before sending the Targeted takeoff command. The UI also exposes an
Emergency Stop button that uses the Targeted MAVLink path. Emergency Stop is
treated as safety-critical: the GS exits manual-control mode before sending it,
and the request uses a separate in-flight guard so it is not dropped merely
because a normal MAVLink command is still waiting for a response.

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
classes. Large images, recorded clips, reports, and other big objects should
never be carried as large inline invocation payloads. They should use the
NDNSF Core large-data abstraction, which publishes hybrid AES-GCM encrypted,
signed segmented NDN Data and carries only a reference in the request, or they
should be stored through `NDNSF-DistributedRepo` under a publisher-owned name
and referenced by manifest/name. Keeping service requests small avoids turning
NDNSF invocation payloads into an ad-hoc file-transfer channel.

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
deadlines, compensation, and final task completion. The ground station records
those semantics in a typed `MissionProgressState` and logs `PATROL_PROGRESS`
markers, so the GUI and smoke tests can follow assigning, waiting-compensation,
compensating, completed, and failed phases without parsing the older ledger
strings.

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

NDNSF core now provides a reusable streaming substrate in
`ndn-service-framework/Stream.hpp`: stream identity, session epoch, chunk
metadata, codec-neutral FEC metadata, producer buffering, consumer reordering,
and adaptive fetch state. The UAV app still owns camera capture, H264 encoding,
XOR recovery, decoder queues, and bitrate policy. Future cleanup should map the
existing `VideoPacket` fields onto the core `StreamInfo`/`StreamChunk` helpers
without changing the control/data split described here.
The first compatibility layer is `videoPacketToStreamChunk(...)` and
`streamChunkToVideoPacket(...)` in `shared/UavProtocol.*`; these helpers do not
change the existing `encodeVideoPacket(...)` wire format.

Transfer API boundary: live downlink uses the NDNSF stream substrate because it
is an ongoing sequence with packet sequence, freshness, reordering, stale
session filtering, and optional FEC metadata. Local camera recording is not a
live stream. It is a repo-backed media object discovered through a manifest and
then fetched by exact object/chunk names through the large-data/repo path.
Start/Stop Video controls the live stream only; it does not define the
recorded-object transfer path.

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
giving higher-bitrate streams enough in-flight Interests to avoid stalls. The
current receiver stores these decisions as `VideoAdaptiveState`, so the video
panel, selected-drone inspector, left-side drone row, and MiniNDN smoke logs can
show RTT, window, lookahead, pressure, missing-packet timeout, pending chunks,
decoded-frame progress, the dominant pressure source, the policy reason, and
the current bitrate recommendation without scraping packet logs. The dominant
source is kept specific enough to distinguish timeout, loss, duplicate,
backlog, future-probe, and decode-gap pressure instead of hiding them all under one generic
congestion label. The actual
window, lookahead, timeout, and bitrate recommendation are computed through a
typed `VideoAdaptivePolicyInput` to
`VideoAdaptivePolicyDecision` helper, with unit tests covering pressure,
high-RTT, and recovery behavior. This keeps the policy generic and testable
instead of tuning hidden constants for one MiniNDN topology. The recommendation
is explicit-control only: the GS does not
silently change the drone encoder, but the operator can click `Apply Bitrate`.
That path stops the current live stream, waits for the drone's Stop response,
and then starts a new stream with the suggested bitrate so packet sequence,
stream id, and decoder state stay coherent. The GS logs the completed
Stop-then-Start loop with the requested and accepted bitrate, so smoke tests can
verify that the restarted stream is actually using the drone-confirmed setting.
The default bitrate policy is `manual`. For experiments, the GS can be started with
`--video-bitrate-policy auto-after-pressure`, which applies a non-hold
recommendation only after pressure persists for
`--video-bitrate-auto-pressure-ms`. Set that value to `0` for an aggressive
or regression-test mode that applies the first pressure-based recommendation.
The default is currently 8000 kbps, 480 px frame width, and 30 FPS for the demo
H264 stream. Raising bitrate improves stream quality and packet volume; raising
frame width makes the displayed video larger.

When `Stop Video` is invoked, the drone stops the live stream, clears pending
Interests and cached stream packets, and ignores late frame Interests for the
stopped stream. The GS sends the stop control request even after it has stopped
the local decoder. If the control response times out, NDNSF emits selection-status
timeout diagnostics and the GS prompts the operator to click `Stop Video` again
if the drone still reports streaming; duplicate stop requests are safe because
the drone treats stop as idempotent. The GS also refreshes the selected-drone
video control state after a stop timeout, so the Stop button becomes available
again when the last telemetry still reports a streaming drone. If the drone config enables `camera-capture-on-start` or
`camera-record-to-local-repo`, the camera capture loop may continue running
locally after the live stream stops.

A later version can add a small per-stream SVS group for video-packet
announcements, but it should remain separate from the main UAV control group so
high-rate video signaling does not disturb command/telemetry/mission traffic.
The same control service can also carry future H265 or tuned GOP parameters.

Video packet data is published under the drone's namespace, signed by the drone,
and can optionally be stored through `NDNSF-DistributedRepo` for replay or
post-mission analysis.

## GUI Plan

The current first GUI uses gtkmm because it is already available in this
repository's build configuration. wxWidgets remains a good future target if we
want a dedicated native GUI toolkit across platforms.

The GUI should be a ground-station frontend over the same service layer:

- mission map and role assignment panel;
- live telemetry table;
- video preview panel fed by the video-packet/prefetch pipeline;
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
by that manifest. The GS stores the manifest as a typed
`RecordingDataProductState` with availability, encryption, playable-state, and
predictable chunk-name helpers; this is the first concrete repo-backed UAV data
product model and will be reused for mission images, telemetry logs, detection
events, and reports. The chunk path is intentionally not an NDNSF service:

The repo control-plane prototype also treats UAV recordings, telemetry logs,
and mission logs as named data products with object-class metadata. In MiniNDN,
the DistributedRepo regression stores these objects, lets catalog gossip
propagate them, performs lookup from a Persistent repo, and fetches the original
payload. This validates the future recording/log browsing path at the repo
layer; it is not yet a full GS catalog browser UI.

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

For board-style smoke tests where the drone side should not create a GTK
window, add `--drone-headless`. The controller and ground station still run as
usual, while the drone process prints `DRONE_HEADLESS_READY` and periodic
readiness/video status lines into its MiniNDN log:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-video-test --auto-stop-seconds 10 --no-cli
```

For a fast launcher/config health check that does not start MiniNDN, X11, or
the APP processes, run:

```bash
python3 Experiments/NDNSF_UAV_GUI_Minindn.py --quick-smoke
```

Success prints `NDNSF_UAV_GUI_MININDN_QUICK_SMOKE_OK`.

Use `--video-bitrate-kbps <kbps>` to change the requested stream bitrate, and
`--video-width <pixels>` to change the requested encoded frame width. The ground
station forwards those values through NDNSF, the drone adjusts its encoder
quality and scaling, and the ground station sizes the prefetch budget from the
accepted bitrate.

Default placement:

```text
controller:     memphis
ground station: memphis
Drone A:        ucla
Drone B:        wustl
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

To verify the reusable mission-file path, run:

```bash
xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --auto-loaded-mission-plan-test \
  --no-start-jmavsim --no-cli --no-xhost
```

This generates a `MissionPlanDocument`, saves it through the ground-station
file helper, loads it back, and uploads the preserved per-drone mission parts
through the normal `serviceMissionAssign` path. A successful run prints
`NDNSF_UAV_LOADED_MISSION_PLAN_MININDN_SMOKE_OK`.

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
The same smoke also checks typed `PATROL_PROGRESS` markers for missing parts,
compensation, return-home planning, and final completion.
The ground-station runtime also keeps the latest typed `MissionPlan`, and the
selected-drone inspector/map summary shows the selected drone's assigned
`MissionPart`, waypoint count, and return-home flag.
Before upload, the GUI uses the same helper to build a local mission preview
from the drawn waypoints. The preview markers and selected-drone inspector show
which drone would receive each part, so operators can inspect the plan before
sending any NDNSF mission request.

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

Add `--auto-apply-bitrate-test` to the same command when you want the smoke test
to exercise the explicit `Apply Bitrate` Stop-then-Start path and verify the
drone-confirmed accepted bitrate after restart.
Add `--auto-repeat-stop-test` when you want the smoke test to delay the first
stop response, check that the GS re-enables the Stop button after timeout, and
then send a second idempotent stop request.
For the policy-driven path, use `--video-bitrate-policy auto-after-pressure`
with a short `--video-bitrate-auto-pressure-ms` value; use `0` when the smoke
test must deterministically exercise the automatic Stop-then-Start path. The
smoke also checks that adaptive logs include `primary_pressure` and
`policy_reason`, so tuning changes remain explainable rather than only changing
numeric windows.
Add `--auto-video-pressure-profile-test` when the smoke should inject controlled
timeout, backlog, and probe-pressure samples after video starts. The GS then
logs `auto-video-pressure-timeout`, `auto-video-pressure-backlog`, and
`auto-video-pressure-probe` view states, proving that the same typed policy can
explain different pressure sources without relying on random packet loss.

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

This smoke now verifies both the accepted Targeted `MANUAL_CONTROL` response
and the telemetry-derived safety transition from `manual=fresh` to
`manual=neutral-sent` after GS stops sending manual updates.

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
arm/takeoff/land over NDNSF Targeted requests. Each telemetry sample also logs
the shared `FlightActionControlState`, `SelectedActionState`,
`SelectedDroneSummaryState`, and `DroneListRowState` derived from the live
telemetry snapshot, so the regression verifies the same state models used by the
GUI.

For a MiniNDN-only regression that does not start PX4/jMAVSim:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-telemetry-test --no-start-jmavsim \
  --no-cli --no-xhost
```

The script automatically enables the mock-field telemetry mode in this case.
It still verifies the NDNSF telemetry request path, typed state updates,
arm/takeoff/land command transitions, landed-state changes, `ekf_ready`,
`armed`, `lat/lon`, and the telemetry-derived shared state models, but it does
not treat missing real GPS fix or battery voltage fields as a failure. The full
PX4/jMAVSim command above remains the strict check for real flight-controller
sensor fields.

To regression-test the GS local stale/lost link model without real hardware:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-link-state-test --link-stale-ms 600 \
  --link-lost-ms 1400 --no-cli
```

The test fetches one telemetry sample, waits without refreshing it, and checks
that the GS safety model transitions from fresh/connected to `stale` and then
`lost`. It also logs the same `FlightActionControlState` used by the flight
buttons, verifying that stale/lost links block normal commands while emergency
stop remains available for the selected drone.

To regression-test that video Start/Stop controls follow the selected drone
instead of a global stream flag:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-video-selection-test --no-cli
```

The launcher starts Drone A and Drone B, starts video only on Drone A, switches
the GS selection to Drone B and back, and checks that the typed video state
drives the button model for the selected drone.

To regression-test that mission Upload/Start/Stop buttons follow typed
`MissionState` rather than temporary status strings:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-mission-controls-test --no-cli
```

The launcher uses a two-drone mock setup and injects uploaded `MissionState`
objects inside the GS smoke path. It first verifies that uploaded mission parts
are still blocked by a not-ready flight safety gate, then injects ready/unarmed
`ReadinessState` snapshots and verifies that the mission control model changes
from explicit `start_reason=blocked-...waiting-heartbeat` to
`can_start=true` / `start_reason=ok` / `can_stop=true`, without depending on
flight-controller waypoint upload behavior. The same smoke also exercises local
`start-pending` and `stop-pending` action gates, which keep Mission Start/Stop
button state and tooltips derived from the typed model while a long command
sequence is in progress. Empty `idle` mission snapshots carried by periodic
telemetry are treated as stale when the GS already has a newer non-idle mission
state, so background telemetry polling does not erase an uploaded/active plan.

To regression-test that Arm/Takeoff/Land/manual-control buttons follow typed
`ReadinessState`:

```bash
sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-flight-controls-test --no-cli
```

The launcher injects not-ready, ready-but-not-armed, and armed-ready readiness
snapshots in the GS smoke path. It verifies that Arm is enabled only when the
selected drone is ready and not armed, and that Takeoff/Land/manual control are
enabled only after the selected drone is armed. These UI gates and the actual
MAVLink command-send path share the same typed `FlightSafetyGateState`, which
combines readiness and safety/link state. The same smoke also logs the
selected-drone action model, including manual-control mode, emergency-stop
availability/reason, and mission Start/Stop readiness. It also verifies the selected
drone view model that drives the inspector/map text, marker state, and flight
gate fields (`can_arm`, `can_takeoff`, `can_manual`, and their reasons), so
mission upload state is reflected as a typed marker suffix instead of temporary
status string parsing. The left drone list is checked through the same typed
state path, including readiness, mission, video, and safety summaries.

### ServiceContainer/local helper regression bundle

After changing `ServiceContainer`, `LocalServiceRegistry`, or UAV same-process
helpers, rerun this MiniNDN-only bundle before migrating more helpers. The
bundle keeps the intended boundary visible: cross-node controls and data access
still go through NDNSF remote/Targeted invocation, while local helpers are
exercised indirectly through normal GS/Drone workflows.

```bash
xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-telemetry-test --auto-telemetry-allow-mock-fields \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-mission-controls-test \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-video-test --auto-stop-seconds 8 \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-recording-playback-test \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-repo-catalog-browse-test \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-parameter-cache-test \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-authority-lease-test \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-authority-config-test \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-authority-issuer-test \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-authority-arbitration-test \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-authority-persistence-test \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480

xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --auto-authority-revocation-test \
  --no-cli --no-xhost --video-bitrate-kbps 8000 --video-width 480
```

The expected success markers are
`NDNSF_UAV_TELEMETRY_MININDN_SMOKE_OK`,
`NDNSF_UAV_MISSION_CONTROLS_MININDN_SMOKE_OK`,
`NDNSF_UAV_GUI_MININDN_SMOKE_OK`, and
`NDNSF_UAV_RECORDING_PLAYBACK_MININDN_SMOKE_OK`, and
`NDNSF_UAV_REPO_CATALOG_MININDN_SMOKE_OK`, and
`NDNSF_UAV_PARAMETER_CACHE_MININDN_SMOKE_OK`, and
`NDNSF_UAV_AUTHORITY_LEASE_MININDN_SMOKE_OK`, and
`NDNSF_UAV_AUTHORITY_CONFIG_MININDN_SMOKE_OK`, and
`NDNSF_UAV_AUTHORITY_ISSUER_MININDN_SMOKE_OK`, and
`NDNSF_UAV_AUTHORITY_ARBITRATION_MININDN_SMOKE_OK`, and
`NDNSF_UAV_AUTHORITY_PERSISTENCE_MININDN_SMOKE_OK`, and
`NDNSF_UAV_AUTHORITY_REVOCATION_MININDN_SMOKE_OK`. The repo catalog smoke
records camera chunks to the drone's in-app repo, fetches the
`/UAV/Camera/Repo/Catalog` service from the ground station, and verifies that
chunk objects are summarized as one queryable UAV recording product. The
parameter-cache smoke fetches `/UAV/MAVLink/Parameters` from the selected drone
and verifies that the ground station caches a usable vehicle capability and
parameter view. The authority lease smoke verifies that monitor-only or expired
operator leases block control and mission assignment before network/MAVLink
dispatch while the normal runtime keeps a default local control lease for demo
compatibility. The authority config smoke starts the GS with a configured
monitor-only lease for the selected drone and verifies that startup lease
configuration, rather than test-time injection, blocks control and mission
assignment while allowing telemetry. The authority issuer smoke requests a
control lease from `/UAV/GS/OperatorAuthority/Lease` through the normal NDNSF
service path, applies the returned lease, and verifies that mission/control
validation becomes allowed again. The authority arbitration smoke verifies the
minimal multi-operator conflict policy: monitor leases are non-exclusive,
control/mission/admin leases are exclusive for overlapping drone targets, the
same operator can renew, and an admin lease overrides existing exclusive
leases. The authority persistence smoke verifies an issuer-side active lease
state file, admin allowlist rejection, authorized admin override, and
response-carried revoked lease evidence. The authority revocation smoke verifies
that revoked lease evidence can also be fetched later through
`/UAV/GS/OperatorAuthority/Revocation`. This bundle intentionally uses mock
flight-controller fields and
the virtual camera
path, so it does not require PX4, jMAVSim, a USB camera, or real UAV hardware.

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
another available provider.

## Development Roadmap

At this checkpoint, the app is useful as a MiniNDN/SITL demonstrator and has
several deployment-facing pieces in place. The remaining roadmap is best read as
a mix of completed stabilization work and future hardening for a deployable UAV
service-container workload:

1. **State model consolidation.** Telemetry, readiness, mission, video, command,
   and safety state now drive the main flight buttons, selected-drone action
   model, selected-drone view-state gate reasons, inspector/map text, map
   markers, left drone list, and MiniNDN smoke markers. Mission Start/Stop now
   also goes through a typed mission start gate
   that combines `MissionState` with flight readiness and safety, and exposes
   upload/start/stop reasons and action-pending tooltips for the UI and smoke
   tests. Periodic telemetry no longer lets empty idle mission fields erase a
   newer GS-side mission state. Patrol task
   progress now has a typed `MissionProgressState` for assignment,
   compensation, completion, and return-home planning, and the ground-station
   mission buttons use that progress model to block duplicate upload/start
   actions while a patrol assignment or compensation step is still active. The
   mission toolbar now derives Upload/Start/Stop sensitivity and reasons through
   shared `MissionControlState`, so mission-control smoke logs and GUI buttons
   use the same model. The
   same progress model also updates left-side drone rows and map marker labels
   so operators can see active compensation or completion without opening the
   inspector. The left vehicle list now uses shared `DroneListRowState`
   derivation, keeping selected/standby, readiness, mission progress, video,
   command, and safety summaries consistent with the rest of the state layer.
   The live downlink also exposes a typed `VideoAdaptiveState`
   covering RTT, prefetch window, lookahead, timeout pressure, probe pressure,
   decoder backlog, and decoded-frame progress; the video panel, selected-drone
   view, left drone rows, and MiniNDN smoke logs read that model instead of
   scraping internal logs. The selected-drone Start/Stop video buttons now derive
   from shared `VideoControlState`, so timeout recovery and target switching use
   the same control model as the smoke logs. The Arm/Takeoff/Land/Manual/E-stop
   action bar now derives from shared `FlightActionControlState` and
   `SelectedActionState`, giving unit tests the same availability/reason model
   used by the GUI. The selected-drone inspector/map summary now derives its
   non-rendering fields from shared `SelectedDroneSummaryState`, while GTK text
   and map marker rendering stay in the window layer. Drone telemetry now also
   carries camera availability/source/reason and flight-controller
   backend/availability/readiness/state, so the GS can show whether a drone-side
   camera and flight controller are actually usable instead of only showing that
   a telemetry response arrived. The telemetry live smoke now emits those shared
   action/summary/row/subsystem models for every arm/takeoff/land telemetry
   sample, so state-model regressions use real NDNSF telemetry requests instead
   of only synthetic GUI injections. Continue extending this rule to new
   mission/video/safety UI paths: GUI code should not infer state from ad hoc
   status strings when a typed state model is available.
2. **Drone headless deployment mode.** The Drone container is intended to stay
   usable on ODROID-class or real airframe computers without a GUI/X server. In
   headless mode the app runs NDNSF, MAVLink, camera, repo, telemetry, and
   mission services.
3. **Flight-controller readiness and safety gates.** Before arm/takeoff/mission
   execution, surface heartbeat, GPS fix, EKF readiness, battery, arming state,
   mode, and landed state. Manual control must time out to neutral, and
   emergency stop / lost-link behavior must be explicit. MiniNDN smoke now
   verifies that manual-control telemetry moves from fresh replay to
   neutral-sent after GS stops sending manual updates, and that stale/lost links
   block normal flight gates while leaving Emergency Stop available.
4. **Adaptive video service quality.** Continue treating video as an NDNSF
   service workload: requested bitrate, accepted bitrate, RTT, backlog,
   timeout pressure, key-frame recovery, and FEC should drive prefetch and
   skip decisions rather than fixed constants. The current GS records these
   decisions as `VideoAdaptiveState`, including advisory bitrate
   decrease/hold/increase decisions plus `primary_pressure` and `policy_reason`
   fields that explain the dominant pressure source. `primary_pressure`
   distinguishes timeout, loss, duplicate, backlog, probe, and decode-gap pressure so
   operators can tell why the receiver is shrinking or recovering. The core window,
   lookahead, timeout, and bitrate recommendation calculations now live in a typed
   `VideoAdaptivePolicyInput` to `VideoAdaptivePolicyDecision` helper with unit
   tests for pressure, high-RTT, and recovery behavior, so tuning stays generic
   rather than tied to one MiniNDN topology. The `Apply Bitrate` control now turns a
   non-hold recommendation into an explicit Stop-then-Start stream restart and logs
   the completed restart with the accepted bitrate returned by the drone.
   MiniNDN smoke can also inject controlled congestion/backlog/probe pressure
   profiles and verify that `primary_pressure` switches accordingly.
   The default policy remains manual, while `auto-after-pressure` can be enabled
   for experiments that should apply persistent-pressure recommendations
   automatically.
5. **Mission collaboration model.** The patrol demo has been promoted toward a
   reusable mission model with `MissionPlan`, `MissionPart`, assignment, progress,
   failure/compensation, and return-to-home semantics. Patrol route clustering,
   default sector generation, drone assignment, and return-to-departure waypoint
   insertion now live in a shared typed mission helper, so GUI workflows,
   service containers, and tests use the same collaboration model. The GS also
   exposes the latest mission plan and selected drone's mission part through the
   typed view state instead of only logging internal assignment strings. The GUI
   now builds a pre-upload mission preview from drawn waypoints with the same
   helper, so planned parts are visible before the mission is sent.
6. **Repo-backed UAV data products.** The implemented repo-backed product is
   encrypted camera recording stored through `NDNSF-DistributedRepo` with
   publisher-owned names and manifest-based discovery. Mission images,
   telemetry logs, object-detection events, and reports should reuse that same
   data-product pattern. Camera recording manifests are now parsed into
   typed `RecordingDataProductState` instances so GS playback and smoke tests
   reason about product availability/playability instead of ad hoc strings.
7. **Vehicle parameter view.** Drones now expose a per-drone
   `/UAV/MAVLink/Parameters` service that returns `VehicleParameterSnapshot`.
   The GS caches the response and shows firmware, vehicle type, modes, and a
   small parameter subset. This is an operational capability/status view, not a
   full QGroundControl-style parameter editor.
8. **Operator authority lease gate.** The GS runtime keeps an active
   `OperatorAuthorityLease` and validates it before direct MAVLink commands and
   mission assignment requests. The default local lease preserves existing
   single-operator demos; monitor-only or expired leases fast-fail before
   network/MAVLink dispatch. The local lease source is configurable with
   `--operator-id`, `--operator-lease-drone`, `--operator-lease-scope`, and
   `--operator-lease-ttl-ms`, and the GUI inspector shows the active operator
   authority for the selected drone. The GS also exposes
   `/UAV/GS/OperatorAuthority/Lease` so a requester can obtain a lease through
   NDNSF before applying it locally.
9. **Distributed inference integration.** Future image and object-detection
   workflows can connect to `NDNSF-DistributedInference` when model execution is
   split across ground stations, drones, and edge machines.
